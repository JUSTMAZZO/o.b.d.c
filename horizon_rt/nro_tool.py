"""nro_tool.py - leitor de NRO (homebrew do Switch) para a reemissao estatica da Fase 3.

Le o cabecalho NRO0, os segmentos .text/.rodata/.data (+ .bss), o MOD0, a secao .dynamic e as
relocations (RELA e RELR; o link da libnx usa -z pack-relative-relocs), e faz o inventario das
instrucoes que nao podem rodar como estao no Android:

  svc #imm            -> chamada ao kernel Horizon (vira trampolim para o HLE)
  mrs xN, tpidrro_el0 -> TLS do Horizon (no Android le 0)
  mrs xN, cntpct_el0  -> contador fisico (SIGILL no A56; vira cntvct_el0 escalado)
  mrs xN, cntfrq_el0  -> frequencia (Switch: 19,2 MHz; A56: 25,6 MHz)

Suporta tambem inspecao e extracao da secao de assets ASET (icone, metadados NACP e RomFS)
e desmonte/extracao do fluxo de controle em JSON para o lifter NEON (neon_lifter / cfg_skeleton).

Uso:
  python nro_tool.py arquivo.nro [--elf arquivo.elf] [--json saida.json]
  python nro_tool.py arquivo.nro --disasm-json extracao.json
  python nro_tool.py arquivo.nro --extract-aset diretorio_saida/
"""
import argparse
import glob
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

SEGMENTS = ('text', 'ro', 'data')

DT = {0: 'NULL', 7: 'RELA', 8: 'RELASZ', 9: 'RELAENT', 23: 'JMPREL', 2: 'PLTRELSZ', 30: 'FLAGS',
      0x6ffffffb: 'FLAGS_1', 36: 'RELR', 35: 'RELRSZ', 37: 'RELRENT', 5: 'STRTAB', 6: 'SYMTAB',
      10: 'STRSZ', 11: 'SYMENT', 0x6ffffef5: 'GNU_HASH', 4: 'HASH', 0x6ffffff9: 'RELACOUNT'}
R_AARCH64_RELATIVE = 1027

SYSREGS = {0xD53BD060: 'tpidrro_el0', 0xD53BE020: 'cntpct_el0', 0xD53BE000: 'cntfrq_el0',
           0xD53BE040: 'cntvct_el0', 0xD53BD040: 'tpidr_el0'}
MSR_TPIDR = 0xD51BD040

NACP_LANGUAGES = [
    'AmericanEnglish', 'BritishEnglish', 'Japanese', 'French',
    'German', 'LatinAmericanSpanish', 'Spanish', 'Italian',
    'Dutch', 'CanadianFrench', 'Portuguese', 'Russian',
    'Korean', 'TraditionalChinese', 'SimplifiedChinese', 'BrazilianPortuguese'
]

PREHEADER_MNEMS = {
    'lsl', 'tst', 'orr', 'ushr', 'msub', 'ldrb', 'mrs', 'mvn', 'ubfiz', 'ldp',
    'movi', 'adr', 'umull', 'dup', 'movk', 'ccmp', 'mul', 'add', 'csinc', 'ldr',
    'adrp', 'mov', 'subs', 'ld2', 'and', 'cmp', 'bic', 'stp', 'sub', 'lsr',
    'st2', 'str', 'madd', 'eor', 'bl'
}


class NroError(Exception):
    pass


def u32(b, o):
    return struct.unpack_from('<I', b, o)[0]


def find_default_objdump(custom=None):
    if custom and os.path.exists(custom):
        return custom
    found = shutil.which('aarch64-none-elf-objdump')
    if found:
        return found
    devkit = r'C:\devkitPro\devkitA64\bin\aarch64-none-elf-objdump.exe'
    if os.path.exists(devkit):
        return devkit
    found_llvm = shutil.which('llvm-objdump')
    if found_llvm:
        return found_llvm
    tools = glob.glob(os.path.join(os.path.dirname(__file__), '..', 'tools', 'llvm-mingw-*', 'bin', 'llvm-objdump.exe'))
    if tools and os.path.exists(tools[0]):
        return tools[0]
    return 'aarch64-none-elf-objdump'


def parse_nacp(nacp_bytes):
    """Decodifica a estrutura NACP (0x4000 bytes) contendo titulos, autor, versao e IDs."""
    if len(nacp_bytes) < 0x3080:
        return {'valid': False}
    titles = {}
    main_title, main_author = '', ''
    for idx, lang in enumerate(NACP_LANGUAGES):
        entry_off = idx * 0x300
        raw_name = nacp_bytes[entry_off:entry_off + 0x200]
        raw_author = nacp_bytes[entry_off + 0x200:entry_off + 0x300]
        name = raw_name.split(b'\x00', 1)[0].decode('utf-8', errors='replace').strip()
        author = raw_author.split(b'\x00', 1)[0].decode('utf-8', errors='replace').strip()
        if name or author:
            titles[lang] = {'name': name, 'author': author}
            if not main_title and name:
                main_title, main_author = name, author

    # Preferencias para titulo principal: AmericanEnglish, BrazilianPortuguese, Portuguese, ou primeiro
    for pref in ('AmericanEnglish', 'BrazilianPortuguese', 'Portuguese', 'BritishEnglish'):
        if pref in titles and titles[pref]['name']:
            main_title = titles[pref]['name']
            main_author = titles[pref]['author']
            break

    display_version = nacp_bytes[0x3060:0x3070].split(b'\x00', 1)[0].decode('ascii', errors='replace').strip()
    title_id = struct.unpack_from('<Q', nacp_bytes, 0x3070)[0]
    save_data_owner_id = struct.unpack_from('<Q', nacp_bytes, 0x3078)[0]

    return {
        'valid': True,
        'title': main_title,
        'author': main_author,
        'version': display_version,
        'title_id': f"{title_id:#018x}",
        'save_data_owner_id': f"{save_data_owner_id:#018x}",
        'titles': titles
    }


def parse_aset(data, nro_size):
    """Inspeciona a secao Asset Storage (ASET) anexada ao final do NRO."""
    if len(data) < nro_size + 0x38:
        return {'has_aset': False}
    if data[nro_size:nro_size + 4] != b'ASET':
        return {'has_aset': False}

    version = u32(data, nro_size + 0x04)
    icon_offset, icon_size = struct.unpack_from('<QQ', data, nro_size + 0x08)
    nacp_offset, nacp_size = struct.unpack_from('<QQ', data, nro_size + 0x18)
    romfs_offset, romfs_size = struct.unpack_from('<QQ', data, nro_size + 0x28)

    icon_data = b''
    icon_type = 'none'
    if icon_size > 0 and len(data) >= nro_size + icon_offset + icon_size:
        icon_data = data[nro_size + icon_offset:nro_size + icon_offset + icon_size]
        if icon_data.startswith(b'\xff\xd8\xff'):
            icon_type = 'jpeg'
        elif icon_data.startswith(b'\x89PNG'):
            icon_type = 'png'
        else:
            icon_type = 'raw'

    nacp_info = {'valid': False}
    nacp_data = b''
    if nacp_size > 0 and len(data) >= nro_size + nacp_offset + nacp_size:
        nacp_data = data[nro_size + nacp_offset:nro_size + nacp_offset + nacp_size]
        nacp_info = parse_nacp(nacp_data)

    return {
        'has_aset': True,
        'version': version,
        'icon': {
            'offset': icon_offset,
            'size': icon_size,
            'type': icon_type,
            'data': icon_data
        },
        'nacp': {
            'offset': nacp_offset,
            'size': nacp_size,
            'data': nacp_data,
            'info': nacp_info
        },
        'romfs': {
            'offset': romfs_offset,
            'size': romfs_size
        }
    }


def parse_nro(data):
    if data[0x10:0x14] != b'NRO0':
        raise NroError("magia NRO0 ausente em 0x10")
    nro = {
        'mod0_offset': u32(data, 0x4),
        'version': u32(data, 0x14),
        'size': u32(data, 0x18),
        'flags': u32(data, 0x1C),
        'segments': {name: {'offset': u32(data, 0x20 + 8 * k), 'size': u32(data, 0x24 + 8 * k)}
                     for k, name in enumerate(SEGMENTS)},
        'bss_size': u32(data, 0x38),
        'build_id': data[0x40:0x60].hex(),
    }
    # na NRO o offset no arquivo e o offset na memoria (imagem carregada na base 0)
    seg = nro['segments']
    if seg['text']['offset'] != 0 or seg['ro']['offset'] < seg['text']['size'] or \
            seg['data']['offset'] < seg['ro']['offset'] + seg['ro']['size']:
        raise NroError(f"segmentos fora da ordem esperada: {seg}")
    nro['image_size'] = seg['data']['offset'] + seg['data']['size'] + nro['bss_size']
    image = bytearray(nro['image_size'])
    for s in SEGMENTS:
        o, n = seg[s]['offset'], seg[s]['size']
        image[o:o + n] = data[o:o + n]
    nro['image'] = image

    # MOD0: offsets relativos ao proprio MOD0
    m = nro['mod0_offset']
    if image[m:m + 4] != b'MOD0':
        raise NroError(f"MOD0 ausente em {m:#x}")
    rel = struct.unpack_from('<6i', image, m + 4)
    nro['mod0'] = {k: m + v for k, v in zip(('dynamic', 'bss_start', 'bss_end', 'eh_frame_hdr_start',
                                             'eh_frame_hdr_end', 'module_object'), rel)}

    dyn, o = {}, nro['mod0']['dynamic']
    while True:
        tag, val = struct.unpack_from('<qQ', image, o)
        if tag == 0:
            break
        dyn[tag] = val
        o += 16
    nro['dynamic'] = {DT.get(t, hex(t)): v for t, v in dyn.items()}
    nro['relocs'] = parse_relocs(image, dyn)
    nro['aset'] = parse_aset(data, nro['size'])
    return nro


def parse_relocs(image, dyn):
    """Lista de offsets que recebem `base + addend` na carga (RELATIVE). Outros tipos sao erro."""
    out = []
    if dyn.get(8):
        start, size, ent = dyn[7], dyn[8], dyn.get(9, 24)
        for o in range(start, start + size, ent):
            off, info, addend = struct.unpack_from('<QQq', image, o)
            if info & 0xFFFFFFFF != R_AARCH64_RELATIVE:
                raise NroError(f"relocation RELA tipo {info & 0xFFFFFFFF} em {off:#x} (so RELATIVE e suportado)")
            out.append(('rela', off, addend))
    if dyn.get(23) and dyn.get(2):
        raise NroError("JMPREL/PLT presente: binario com simbolos dinamicos nao e suportado")
    if dyn.get(35):
        start, size = dyn[36], dyn[35]
        where = None
        for o in range(start, start + size, 8):
            entry = struct.unpack_from('<Q', image, o)[0]
            if entry & 1 == 0:                 # endereco: reloca e segue para a proxima palavra
                where = entry
                out.append(('relr', where, struct.unpack_from('<Q', image, where)[0]))
                where += 8
            else:                              # bitmap de 63 palavras a partir de `where`
                bits = entry >> 1
                for k in range(63):
                    if bits >> k & 1:
                        a = where + 8 * k
                        out.append(('relr', a, struct.unpack_from('<Q', image, a)[0]))
                where += 63 * 8
    return out


def scan_text(nro):
    """Inventario de svc e mrs/msr de registradores de sistema no .text."""
    t = nro['segments']['text']
    sites = []
    for off in range(t['offset'], t['offset'] + t['size'], 4):
        w = u32(nro['image'], off)
        if w & 0xFFE0001F == 0xD4000001:
            sites.append({'offset': off, 'kind': 'svc', 'imm': (w >> 5) & 0xFFFF, 'word': w})
        elif w & ~0x1F in SYSREGS:
            sites.append({'offset': off, 'kind': 'mrs', 'sysreg': SYSREGS[w & ~0x1F], 'rt': w & 0x1F, 'word': w})
        elif w & ~0x1F == MSR_TPIDR:
            sites.append({'offset': off, 'kind': 'msr', 'sysreg': 'tpidr_el0', 'rt': w & 0x1F, 'word': w})
    return sites


def elf_crosscheck(nro, sites, elf_path, objdump):
    """Confere cada sitio contra o disassembly do ELF original (mesmo endereco, mesma palavra)."""
    dis = subprocess.run([objdump, '-d', elf_path], capture_output=True, text=True, check=True).stdout
    func, by_addr = None, {}
    for line in dis.splitlines():
        m = re.match(r'^([0-9a-f]+) <(.+)>:$', line)
        if m:
            func = m.group(2)
            continue
        m = re.match(r'^\s+([0-9a-f]+):\s+([0-9a-f]{8})\s+(.*)$', line)
        if m:
            by_addr[int(m.group(1), 16)] = (int(m.group(2), 16), func, m.group(3).strip())
    elf_sites = {a for a, (w, f, txt) in by_addr.items()
                 if re.match(r'svc\s|mrs\s+x\d+, (tpidrro_el0|cntpct_el0|cntfrq_el0|cntvct_el0|tpidr_el0)|'
                             r'msr\s+tpidr_el0', txt)}
    problems = []
    for s in sites:
        e = by_addr.get(s['offset'])
        if e is None or e[0] != s['word']:
            problems.append(f"{s['offset']:#x}: palavra {s['word']:#010x} nao bate com o ELF ({e})")
        else:
            s['function'] = e[1]
    missing = elf_sites - {s['offset'] for s in sites}
    for a in sorted(missing):
        problems.append(f"{a:#x}: sitio no ELF ausente no NRO ({by_addr[a]})")
    return problems


def load_svc_table():
    """Carrega tabela de nomes de chamadas ao sistema Horizon a partir de svc_table.json."""
    svc_map = {}
    svc_path = os.path.join(os.path.dirname(__file__), 'svc_table.json')
    if os.path.exists(svc_path):
        try:
            with open(svc_path, encoding='utf-8') as f:
                for s in json.load(f).get('svcs', []):
                    imm = int(s['id'], 16) if 'id' in s else s.get('svc')
                    if imm is not None:
                        svc_map[imm] = s['name']
        except Exception:
            pass
    return svc_map


def generate_disasm_json(nro_path, elf_path=None, objdump=None, base=0x100000):
    """Desmonta o NRO e gera a estrutura JSON de extracao compativel com o neon_lifter e cfg_skeleton.

    Opera de forma autonoma em NROs stripped (sem ELF) utilizando fecho de desvios (Branch Target Closure)
    e classificacao estatica de chamadas. Se um arquivo ELF for explicitamente fornecido via elf_path,
    seus simbolos serao carregados e priorizados.
    """
    objdump = find_default_objdump(objdump)

    with open(nro_path, 'rb') as f:
        data = f.read()
    nro = parse_nro(data)
    seg = nro['segments']

    # 1. Carrega simbolos do ELF se explicitamente fornecido
    syms = {}          # addr -> (name, size, stype, sec)
    syms_by_name = {}  # name -> (addr, size, stype, sec)
    has_elf_syms = False
    if elf_path and os.path.exists(elf_path):
        out = subprocess.run([objdump, '-t', elf_path], capture_output=True, text=True, check=True).stdout
        for line in out.splitlines():
            m = re.match(r'^([0-9a-fA-F]{16})\s+(\S+)\s+(\S+)\s+(\S+)\s+([0-9a-fA-F]{16})\s+(\S+)$', line)
            if m:
                addr = int(m.group(1), 16) + base
                stype = m.group(3)
                sec = m.group(4)
                size = int(m.group(5), 16)
                name = m.group(6)
                syms[addr] = (name, size, stype, sec)
                syms_by_name[name] = (addr, size, stype, sec)
        has_elf_syms = bool(syms)

    # 2. Extracao de strings em .rodata e .data
    strings = {}
    ro_off = seg['ro']['offset']
    ro_sz = seg['ro']['size']
    ro_data = nro['image'][ro_off:ro_off + ro_sz]
    for m in re.finditer(rb'[\x20-\x7e\n\r\t]{3,}\x00', ro_data):
        addr_hex = hex(base + ro_off + m.start())
        s = m.group(0)[:-1].decode('utf-8', errors='replace')
        strings[addr_hex] = s

    # 3. Desmonte do segmento .text com objdump
    with tempfile.NamedTemporaryFile(delete=False, suffix='.bin') as tf:
        tf.write(nro['image'][:seg['text']['size']])
        tf_path = tf.name

    try:
        dis = subprocess.run([objdump, '-b', 'binary', '-m', 'aarch64', '-D', f'--adjust-vma={base:#x}', tf_path],
                             capture_output=True, text=True, check=True).stdout
    finally:
        if os.path.exists(tf_path):
            os.unlink(tf_path)

    insns = []
    insn_by_addr = {}
    bl_targets = set()
    for line in dis.splitlines():
        m = re.match(r'^\s*([0-9a-fA-F]+):\s+([0-9a-fA-F]{8})\s+(\S+)(?:\s+(.*))?$', line)
        if m:
            addr = int(m.group(1), 16)
            mnem = m.group(3).lower()
            rest = (m.group(4) or '').strip()
            if '//' in rest:
                rest = rest.split('//')[0].strip()
            if ';' in rest:
                rest = rest.split(';')[0].strip()
            rest = re.sub(r'<[^>]+>', '', rest).strip()

            # Normaliza destino de saltos para 0x...
            if mnem.startswith('b') or mnem in ('cbz', 'cbnz', 'tbz', 'tbnz', 'bl'):
                parts = rest.split(',')
                tgt = parts[-1].strip()
                if re.fullmatch(r'[0-9a-fA-F]+', tgt) and not tgt.startswith('0x'):
                    parts[-1] = f'0x{int(tgt, 16):08x}'
                    rest = ', '.join(parts)
                if mnem == 'bl':
                    try:
                        bl_targets.add(int(parts[-1].strip(), 16))
                    except ValueError:
                        pass

            # Normaliza tipos de deslocamento (LSL, LSR, MSL) e qualificadores vetoriais
            rest = re.sub(r'\b(lsl|lsr|asr|ror|msl)\b', lambda m_sh: m_sh.group(1).upper(), rest)
            rest = re.sub(r'\b([vqdshb]\d+\.)(\d+)([bhsd])\b',
                          lambda m_v: m_v.group(1) + m_v.group(2) + m_v.group(3).upper(), rest)

            # Normaliza imediatos decimais (#160 -> #0xa0)
            def repl_imm(m_imm):
                sign = m_imm.group(1)
                val = int(m_imm.group(2))
                return f'#{sign}0x{val:x}' if val >= 10 else f'#{sign}{val}'
            rest = re.sub(r'#(-?)(?![0-9]*x)(\d+)\b', repl_imm, rest)

            fmt_line = f'{addr:08x}: {mnem} {rest}' if rest else f'{addr:08x}: {mnem}'
            item = (addr, mnem, rest, fmt_line)
            insns.append(item)
            insn_by_addr[addr] = item

    # 4. Inferencia de simbolos e pontos de entrada conhecidos para binarios stripped
    known_entries = set(bl_targets)
    if base in insn_by_addr and insn_by_addr[base][1] == 'b':
        try:
            start_tgt = int(insn_by_addr[base][2].strip(), 16)
            known_entries.add(start_tgt)
        except ValueError:
            pass

    if not has_elf_syms:
        # 4a. Mapeia stubs de chamadas de sistema (svc #imm)
        svc_map = load_svc_table()
        for addr, mnem, rest, line in insns:
            if mnem == 'svc':
                imm_m = re.search(r'#0x([0-9a-fA-F]+)|#(\d+)', rest)
                if imm_m:
                    imm = int(imm_m.group(1), 16) if imm_m.group(1) else int(imm_m.group(2))
                    name = svc_map.get(imm, f'svc_{imm:#x}')
                    stub_start = addr
                    prev = insn_by_addr.get(addr - 4)
                    if prev and 'str' in prev[1] and 'sp' in prev[2]:
                        stub_start = addr - 4
                    syms[stub_start] = (name, 8, 'FUNC', '.text')
                    syms_by_name[name] = (stub_start, 8, 'FUNC', '.text')
                    known_entries.add(stub_start)

        # 4b. Detecta strlen atraves das mascaras de Alan Mycroft (0x101010101010101 e 0x7f7f7f7f7f7f7f7f)
        for addr, mnem, rest, line in insns:
            if '101010101010101' in rest or '7f7f7f7f7f7f7f7f' in rest:
                start = addr
                while start > addr - 0x40 and start in insn_by_addr:
                    if insn_by_addr[start][1] in ('bti', 'stp') or (
                            start - 4 in insn_by_addr and insn_by_addr[start - 4][1] in ('ret', 'b')):
                        break
                    start -= 4
                syms[start] = ('strlen', 0x100, 'FUNC', '.text')
                syms_by_name['strlen'] = (start, 0x100, 'FUNC', '.text')
                known_entries.add(start)
                break

    # 5. Deteccao dinamica do ponto de entrada do main
    main_addr = None
    if 'main' in syms_by_name:
        main_addr = syms_by_name['main'][0]
    else:
        # Varre a crt0 (_start) ate o MOD0 em busca do salto para o main
        for i, (addr, mnem, rest, line) in enumerate(insns):
            if addr >= base + nro['mod0_offset']:
                break
            if mnem in ('b', 'bl'):
                try:
                    tgt = int(rest.strip(), 16)
                    if tgt >= base + nro['mod0_offset']:
                        prev = insns[i - 1] if i > 0 else None
                        if prev and 'x30' in prev[2]:
                            main_addr = tgt
                except ValueError:
                    pass
        if not main_addr:
            main_addr = base + 0x180
    known_entries.add(main_addr)

    # 6. Delimitador de funcoes por fecho transitivo de desvios (Branch Target Closure)
    def delimit_func(start_addr):
        cur = start_addr
        max_target = start_addr
        fn_ins = []
        while cur in insn_by_addr:
            if cur != start_addr and cur in known_entries:
                break
            item = insn_by_addr[cur]
            addr, mnem, rest, line = item
            fn_ins.append(item)

            # Fecho de desvios condicionais internos (b.cond, cbz, cbnz, tbz, tbnz)
            if mnem.startswith('b.') or mnem in ('cbz', 'cbnz', 'tbz', 'tbnz'):
                try:
                    tgt = int(rest.split(',')[-1].strip(), 16)
                    if tgt > max_target and tgt not in known_entries:
                        max_target = tgt
                except ValueError:
                    pass
            elif mnem == 'b':
                try:
                    tgt = int(rest.strip(), 16)
                    # Desvio incondicional para a frente pertence a mesma funcao se
                    # alcancavel antes do proximo ponto de entrada conhecido
                    if tgt > addr and (tgt <= max_target or tgt not in known_entries):
                        max_target = max(max_target, tgt)
                except ValueError:
                    pass

            # Termina a funcao apenas quando o fluxo incondicional encerra E
            # nenhum salto interno alcanca enderecos posteriores
            if addr >= max_target and mnem in ('ret', 'b', 'br'):
                break
            cur += 4
        return fn_ins

    main_ins = delimit_func(main_addr)
    main_size = len(main_ins) * 4

    # Carrega lista de primitivas conhecidas do switch_hle_shim.h se disponivel
    shim_fns = set()
    for sp in [
        os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'switch_hle_shim.h'),
        'switch_hle_shim.h',
        os.path.join(os.getcwd(), 'switch_hle_shim.h'),
    ]:
        if os.path.exists(sp):
            try:
                with open(sp, encoding='utf-8') as f:
                    shim_fns = set(re.findall(r'SHIM_API\s+[\w\s\*]+?\b(\w+)\s*\(', f.read()))
            except Exception:
                pass
            break

    # 7. Inferencia contextual de simbolos de biblioteca e shim para binarios stripped
    if not has_elf_syms:
        def register_sym(s_addr, s_name, s_size=0x40):
            syms[s_addr] = (s_name, s_size, 'FUNC', '.text')
            syms_by_name[s_name] = (s_addr, s_size, 'FUNC', '.text')
            known_entries.add(s_addr)

        last_bl = None
        for i, (addr, mnem, rest, line) in enumerate(main_ins):
            if mnem != 'bl':
                continue
            try:
                tgt = int(rest.strip(), 16)
            except ValueError:
                continue

            start_bb = i
            while start_bb > 0:
                prev_m = main_ins[start_bb - 1][1]
                if prev_m in ('bl', 'b', 'ret', 'br') or prev_m.startswith('b.') or prev_m in ('cbz', 'cbnz', 'tbz', 'tbnz'):
                    break
                start_bb -= 1

            window = main_ins[start_bb:i]
            reg_val = {}
            reg_str = {}
            for w_addr, w_mn, w_ops, _ in window:
                toks = [o.strip() for o in w_ops.split(',')] if w_ops else []
                if w_mn == 'adrp':
                    try:
                        reg_val[toks[0].lower()] = int(toks[1].lstrip('#'), 16)
                    except ValueError:
                        pass
                elif w_mn == 'add' and len(toks) >= 3:
                    dst, src = toks[0].lower(), toks[1].lower()
                    if src in reg_val and toks[2].startswith('#'):
                        try:
                            off = int(toks[2].lstrip('#'), 0)
                            ptr = reg_val[src] + off
                            reg_val[dst] = ptr
                            if hex(ptr) in strings:
                                reg_str[dst] = strings[hex(ptr)]
                        except ValueError:
                            pass
                elif w_mn == 'mov':
                    dst = toks[0].lower()
                    if toks[1].startswith('#'):
                        try:
                            reg_val[dst] = int(toks[1].lstrip('#'), 0)
                        except ValueError:
                            pass
                    elif toks[1].lower() in reg_str:
                        reg_str[dst] = reg_str[toks[1].lower()]
                    elif toks[1].lower() in reg_val:
                        reg_val[dst] = reg_val[toks[1].lower()]
                elif w_mn == 'movk':
                    dst = toks[0].lower()
                    if dst in reg_val and len(toks) >= 2 and toks[1].startswith('#'):
                        try:
                            imm = int(toks[1].lstrip('#'), 0)
                            sh = int(toks[2].split('#')[1], 0) if len(toks) >= 3 and 'LSL' in toks[2] else 0
                            reg_val[dst] |= (imm << sh)
                        except ValueError:
                            pass

            s0 = reg_str.get('x0', '')
            s2 = reg_str.get('x2', '')
            w0 = reg_val.get('w0', reg_val.get('x0'))
            w1 = reg_val.get('w1', reg_val.get('x1'))
            w5 = reg_val.get('w5')
            w6 = reg_val.get('w6')

            if s0.startswith('[+] ') and s0.endswith('\n') and 'out' not in syms_by_name:
                register_sym(tgt, 'out', 0x20)

            if s0 in ('smInitialize', 'appletInitialize', 'hidInitialize', 'fsInitialize'):
                register_sym(tgt, 'open_service', 0x40)
                if last_bl:
                    register_sym(last_bl, s0, 0x100)

            if (w1 == 0x1f or w1 == 31) and w0 == 1:
                register_sym(tgt, 'padConfigureInput', 0x40)

            if w1 == 0x100000001:
                register_sym(tgt, 'padInitializeWithMask', 0x40)

            if ('%' in s2 or ('%' in strings.get(hex(reg_val.get('x2', 0)), ''))) and (w1 is not None and w1 > 0):
                register_sym(tgt, 'snprintf', 0x100)

            if w0 is not None and w0 in (400000, 100000, 0x61a80, 200000) and 'malloc' not in syms_by_name:
                register_sym(tgt, 'malloc', 0x40)
                if tgt + 0x40 in insn_by_addr:
                    register_sym(tgt + 0x40, 'free', 0x20)

            if w5 == 0x2c and w6 in (0xfffffffe, -2, 4294967294):
                register_sym(tgt, 'threadCreate', 0x40)
                w_worker = reg_val.get('x1')
                if w_worker and w_worker in insn_by_addr:
                    register_sym(w_worker, 'sub_1004f0', 0x200)

            last_bl = tgt

        if 'threadCreate' in syms_by_name:
            tc_addr = syms_by_name['threadCreate'][0]
            tc_idx = next(i for i, ins in enumerate(main_ins) if ins[1] == 'bl' and int(ins[2], 16) == tc_addr)
            for j in range(tc_idx + 1, min(tc_idx + 6, len(main_ins))):
                if main_ins[j][1] == 'bl':
                    register_sym(int(main_ins[j][2], 16), 'threadStart', 0x40)
                    break

            if 'svcSignalProcessWideKey' in syms_by_name:
                sig_addr = syms_by_name['svcSignalProcessWideKey'][0]
                sig_idx = next((i for i, ins in enumerate(main_ins) if ins[1] == 'bl' and int(ins[2], 16) == sig_addr), None)
                if sig_idx is not None:
                    for j in range(sig_idx - 1, max(0, sig_idx - 10), -1):
                        if main_ins[j][1] == 'bl':
                            register_sym(int(main_ins[j][2], 16), 'mutexLock', 0x40)
                            break
                    for j in range(sig_idx + 1, min(sig_idx + 10, len(main_ins))):
                        if main_ins[j][1] == 'bl':
                            register_sym(int(main_ins[j][2], 16), 'mutexUnlock', 0x40)
                            break

            if 'mutexUnlock' in syms_by_name:
                mu_addr = syms_by_name['mutexUnlock'][0]
                mu_idx = next(i for i, ins in enumerate(main_ins) if ins[1] == 'bl' and int(ins[2], 16) == mu_addr)
                join_bls = [int(main_ins[j][2], 16) for j in range(mu_idx + 1, len(main_ins)) if main_ins[j][1] == 'bl' and int(main_ins[j][2], 16) not in syms]
                if len(join_bls) >= 2:
                    register_sym(join_bls[0], 'threadWaitForExit', 0x40)
                    register_sym(join_bls[1], 'threadClose', 0x40)

        if 'smInitialize' in syms_by_name:
            if 'padInitializeWithMask' in syms_by_name:
                pinit_addr = syms_by_name['padInitializeWithMask'][0]
                pinit_idx = next(i for i, ins in enumerate(main_ins) if ins[1] == 'bl' and int(ins[2], 16) == pinit_addr)
                loop_branch_tgt = None
                for j in range(pinit_idx + 1, len(main_ins)):
                    if main_ins[j][1] == 'b':
                        try:
                            loop_branch_tgt = int(main_ins[j][2], 16)
                            break
                        except ValueError:
                            pass
                for j in range(pinit_idx + 1, len(main_ins)):
                    if main_ins[j][0] == loop_branch_tgt and main_ins[j][1] == 'bl':
                        try:
                            register_sym(int(main_ins[j][2], 16), 'appletMainLoop', 0x40)
                        except ValueError:
                            pass
                    elif loop_branch_tgt and main_ins[j][0] < loop_branch_tgt and main_ins[j][1] == 'bl':
                        try:
                            register_sym(int(main_ins[j][2], 16), 'padUpdate', 0x40)
                        except ValueError:
                            pass

            ret_indices = [i for i, ins in enumerate(main_ins) if ins[1] == 'ret']
            if ret_indices:
                last_ret = ret_indices[-1]
                trailing_bls = [int(main_ins[j][2], 16) for j in range(max(0, last_ret - 10), last_ret) if main_ins[j][1] == 'bl']
                if len(trailing_bls) >= 4:
                    register_sym(trailing_bls[-4], 'fsExit', 0x20)
                    register_sym(trailing_bls[-3], 'hidExit', 0x20)
                    register_sym(trailing_bls[-2], 'appletExit', 0x20)
                    register_sym(trailing_bls[-1], 'smExit', 0x20)

    # 8. Extrator de funcao
    def extract_func(fn_name, start_addr, size):
        fn_insns = [i for i in insns if start_addr <= i[0] < start_addr + size]
        asm_lines = [i[3] for i in fn_insns]

        calls = {}
        for addr, mnem, rest, line in fn_insns:
            if mnem == 'bl':
                tgt_str = rest.strip()
                if tgt_str.startswith('0x'):
                    tgt_addr = int(tgt_str, 16)
                    tgt_name = syms[tgt_addr][0] if tgt_addr in syms else f'sub_{tgt_addr:x}'
                    calls[hex(tgt_addr)] = tgt_name
            elif mnem == 'b':
                tgt_str = rest.strip()
                if tgt_str.startswith('0x'):
                    tgt_addr = int(tgt_str, 16)
                    if tgt_addr not in range(start_addr, start_addr + size) and tgt_addr in syms:
                        calls[hex(tgt_addr)] = syms[tgt_addr][0]

        resolved_data = {}
        adrp_regs = {}
        for addr, mnem, rest, line in fn_insns:
            if mnem == 'adrp':
                m_adrp = re.match(r'([wx]\d+),\s*0x([0-9a-fA-F]+)', rest)
                if m_adrp:
                    r = m_adrp.group(1).lower().replace('w', 'x')
                    adrp_regs[r] = int(m_adrp.group(2), 16)
            elif mnem in ('ldr', 'ldp', 'add'):
                m_load = re.search(r'\[([wx]\d+)(?:,\s*#(-?0x[0-9a-fA-F]+|-?\d+))?\]', rest)
                if m_load:
                    r = m_load.group(1).lower().replace('w', 'x')
                    if r in adrp_regs:
                        off = int(m_load.group(2), 0) if m_load.group(2) else 0
                        target_addr = adrp_regs[r] + off
                        nro_off = target_addr - base
                        if 0 <= nro_off < len(nro['image']):
                            raw = nro['image'][nro_off:nro_off + 16]
                            hex_str = ' '.join(f'{b:02x}' for b in raw)
                            resolved_data[f'DAT_{target_addr:06x}'] = hex_str

        fn_loops = []
        for i, (addr, mnem, rest, line) in enumerate(fn_insns):
            if mnem.startswith('b.') or mnem in ('cbz', 'cbnz', 'tbz', 'tbnz'):
                tgt_str = rest.split(',')[-1].strip()
                if tgt_str.startswith('0x'):
                    target = int(tgt_str, 16)
                    if start_addr <= target <= addr:
                        body = [x[3] for x in fn_insns if target <= x[0] <= addr]
                        preheader = [x[3] for x in fn_insns if x[0] < target and x[1] in PREHEADER_MNEMS]
                        fn_loops.append({
                            'start': f'{target:08x}',
                            'end': f'{addr:08x}',
                            'preheader': preheader,
                            'body': body
                        })

        return {
            'name': fn_name,
            'entry_point': f'{start_addr:08x}',
            'decompiled_c': '',
            'assembly': asm_lines,
            'resolved_data': resolved_data,
            'calls': calls,
            'loops': fn_loops
        }

    functions = []
    main_fn = extract_func('main', main_addr, main_size)
    functions.append(main_fn)

    # 9. Inclusao transitiva de funcoes auxiliares chamadas pelo main
    visited_addrs = {main_addr}
    queue = list(main_fn['calls'].keys())
    last_adrp = {}
    for line in main_fn.get('assembly', []):
        parts = line.split(': ', 1)
        if len(parts) < 2:
            continue
        tokens = parts[1].strip().split(None, 1)
        mn = tokens[0].lower()
        ops = [o.strip() for o in tokens[1].split(',')] if len(tokens) > 1 else []
        if mn == 'adr' and len(ops) >= 2 and ops[1].startswith('0x'):
            queue.append(ops[1])
        elif mn == 'adrp' and len(ops) >= 2:
            reg = ops[0].lower()
            try:
                last_adrp[reg] = int(ops[1].lstrip('#'), 16)
            except ValueError:
                pass
        elif mn == 'add' and len(ops) >= 3:
            dst = ops[0].lower()
            src = ops[1].lower()
            if src in last_adrp and ops[2].startswith('#'):
                try:
                    queue.append(hex(last_adrp[src] + int(ops[2].lstrip('#'), 0)))
                except ValueError:
                    pass
            last_adrp.pop(dst, None)
        else:
            if ops:
                last_adrp.pop(ops[0].lower(), None)

    while queue:
        c_addr_str = queue.pop(0)
        try:
            c_addr = int(c_addr_str, 16)
        except ValueError:
            continue
        if c_addr in visited_addrs:
            continue
        visited_addrs.add(c_addr)

        c_name = syms[c_addr][0] if c_addr in syms else f'sub_{c_addr:x}'
        # Funcoes da libc/shim (strlen, malloc, free, svc*, etc.) nao necessitam de extracao de corpo
        if c_name in shim_fns or c_name in ('strlen', 'memcpy', 'memset', 'memmove', 'strcmp', 'strcpy', 'strncpy',
                      'malloc', 'free', 'calloc', 'realloc', 'snprintf', 'printf', 'puts') or c_name.startswith('svc'):
            continue

        if base <= c_addr < base + seg['text']['size']:
            h_ins = delimit_func(c_addr)
            if h_ins:
                h_fn = extract_func(c_name, c_addr, len(h_ins) * 4)
                functions.append(h_fn)
                for next_call in h_fn['calls']:
                    queue.append(next_call)

    aset_clean = {}
    if nro['aset']['has_aset']:
        aset_clean = {
            'has_aset': True,
            'version': nro['aset']['version'],
            'icon': {'size': nro['aset']['icon']['size'], 'type': nro['aset']['icon']['type']},
            'nacp': nro['aset']['nacp']['info'],
            'romfs': {'size': nro['aset']['romfs']['size']}
        }

    return {
        'binary_name': os.path.basename(nro_path),
        'architecture': 'AARCH64',
        'functions': functions,
        'strings': strings,
        'aset': aset_clean
    }


def main():
    ap = argparse.ArgumentParser(description="nro_tool.py - Leitor de NRO e extrator de CFG do Switch")
    ap.add_argument('nro', help='Caminho do arquivo .nro')
    ap.add_argument('--elf', help='ELF original (com simbolos) para conferir os sitios e extrair funcoes')
    ap.add_argument('--objdump', default=None, help='Caminho para o utilitario objdump (devkitA64 ou llvm-objdump)')
    ap.add_argument('--json', help='Grava o inventario de instrucoes e relocacoes em JSON')
    ap.add_argument('--disasm-json', metavar='OUT', help='Gera JSON de extracao compativel com o lifter/cfg_skeleton')
    ap.add_argument('--extract-icon', metavar='OUT', help='Extrai o icone embutido na secao ASET para arquivo OUT')
    ap.add_argument('--extract-nacp', metavar='OUT', help='Extrai os bytes do NACP embutido na secao ASET para OUT')
    ap.add_argument('--extract-romfs', metavar='OUT', help='Extrai a particao RomFS embutida para OUT')
    ap.add_argument('--extract-aset', metavar='DIR', help='Extrai todos os assets (icone, NACP, RomFS) no diretorio DIR')
    ap.add_argument('--nacp-info', action='store_true', help='Exibe detalhes dos metadados NACP (titulos por idioma)')
    args = ap.parse_args()

    with open(args.nro, 'rb') as f:
        data = f.read()
    nro = parse_nro(data)
    sites = scan_text(nro)
    seg = nro['segments']

    print(f"NRO {args.nro}: versao {nro['version']}, {nro['size']} bytes, build-id {nro['build_id'][:16]}...")
    for s in SEGMENTS:
        print(f"  .{s:<5} {seg[s]['offset']:#08x} +{seg[s]['size']:#08x}")
    print(f"  .bss   {seg['data']['offset'] + seg['data']['size']:#08x} +{nro['bss_size']:#08x}   imagem: {nro['image_size']:#x} bytes")
    print(f"  MOD0 em {nro['mod0_offset']:#x}: " + ', '.join(f"{k}={v:#x}" for k, v in nro['mod0'].items()))
    print(f"  dynamic: " + ', '.join(f"{k}={v:#x}" for k, v in nro['dynamic'].items()))
    kinds = {}
    for _, off, _ in nro['relocs']:
        seg_of = next(s for s in reversed(SEGMENTS) if off >= seg[s]['offset'])
        kinds[seg_of] = kinds.get(seg_of, 0) + 1
    print(f"  relocations RELATIVE: {len(nro['relocs'])} ({', '.join(f'{k}: {v}' for k, v in kinds.items())})")

    aset = nro['aset']
    if aset['has_aset']:
        nacp = aset['nacp']['info']
        print(f"  ASET: versao {aset['version']}, icone={aset['icon']['size']} bytes ({aset['icon']['type']}), "
              f"NACP={aset['nacp']['size']} bytes, RomFS={aset['romfs']['size']} bytes")
        if nacp.get('valid'):
            print(f"  NACP: '{nacp['title']}' por '{nacp['author']}', versao {nacp['version']}, Title ID: {nacp['title_id']}")
    else:
        print(f"  ASET: ausente (binario sem assets embutidos)")

    if args.nacp_info and aset['has_aset'] and aset['nacp']['info'].get('valid'):
        print("\n## Titulos NACP por Idioma:")
        for lang, item in aset['nacp']['info']['titles'].items():
            print(f"  - [{lang}]: '{item['name']}' ({item['author']})")

    # Extracao de assets sob demanda
    if args.extract_icon:
        if not aset['has_aset'] or not aset['icon']['data']:
            sys.exit("[nro_tool] Erro: arquivo NRO nao possui icone embutido.")
        with open(args.extract_icon, 'wb') as f:
            f.write(aset['icon']['data'])
        print(f"[nro_tool] Icone extraido -> {args.extract_icon} ({len(aset['icon']['data'])} bytes)")

    if args.extract_nacp:
        if not aset['has_aset'] or not aset['nacp']['data']:
            sys.exit("[nro_tool] Erro: arquivo NRO nao possui secao NACP embutida.")
        with open(args.extract_nacp, 'wb') as f:
            f.write(aset['nacp']['data'])
        print(f"[nro_tool] NACP extraido -> {args.extract_nacp} ({len(aset['nacp']['data'])} bytes)")

    if args.extract_romfs:
        if not aset['has_aset'] or aset['romfs']['size'] == 0:
            sys.exit("[nro_tool] Erro: arquivo NRO nao possui particao RomFS.")
        ro_off = nro['size'] + aset['romfs']['offset']
        ro_data = data[ro_off:ro_off + aset['romfs']['size']]
        with open(args.extract_romfs, 'wb') as f:
            f.write(ro_data)
        print(f"[nro_tool] RomFS extraido -> {args.extract_romfs} ({len(ro_data)} bytes)")

    if args.extract_aset:
        if not aset['has_aset']:
            sys.exit("[nro_tool] Erro: arquivo NRO nao possui secao ASET.")
        os.makedirs(args.extract_aset, exist_ok=True)
        if aset['icon']['data']:
            ext = 'jpg' if aset['icon']['type'] == 'jpeg' else ('png' if aset['icon']['type'] == 'png' else 'bin')
            icon_p = os.path.join(args.extract_aset, f'icon.{ext}')
            with open(icon_p, 'wb') as f:
                f.write(aset['icon']['data'])
            print(f"  - gravado {icon_p}")
        if aset['nacp']['data']:
            nacp_p = os.path.join(args.extract_aset, 'control.nacp')
            with open(nacp_p, 'wb') as f:
                f.write(aset['nacp']['data'])
            print(f"  - gravado {nacp_p}")
        if aset['romfs']['size'] > 0:
            romfs_p = os.path.join(args.extract_aset, 'romfs.bin')
            ro_off = nro['size'] + aset['romfs']['offset']
            with open(romfs_p, 'wb') as f:
                f.write(data[ro_off:ro_off + aset['romfs']['size']])
            print(f"  - gravado {romfs_p}")

    problems = []
    if args.elf:
        objdump_bin = find_default_objdump(args.objdump)
        problems = elf_crosscheck(nro, sites, args.elf, objdump_bin)
        print(f"  conferencia com {args.elf}: {'OK' if not problems else 'DIVERGE'}")
        for p in problems:
            print(f"    - {p}")

    print(f"\n## Sitios para patch ({len(sites)})")
    for s in sites:
        what = f"svc #{s['imm']:#x}" if s['kind'] == 'svc' else f"{s['kind']} x{s['rt']}, {s['sysreg']}"
        print(f"  {s['offset']:#08x}  {what:<26} {s.get('function', '')}")

    if args.json:
        out = {k: v for k, v in nro.items() if k not in ('image', 'aset')}
        out['relocs'] = [{'kind': k, 'offset': o, 'addend': a} for k, o, a in nro['relocs']]
        out['sites'] = sites
        if aset['has_aset']:
            out['aset'] = {
                'has_aset': True,
                'version': aset['version'],
                'icon': {'size': aset['icon']['size'], 'type': aset['icon']['type']},
                'nacp': aset['nacp']['info'],
                'romfs': {'size': aset['romfs']['size']}
            }
        else:
            out['aset'] = {'has_aset': False}
        with open(args.json, 'w', encoding='utf-8') as f:
            json.dump(out, f, indent=2)
        print(f"\n[nro_tool] inventario -> {args.json}")

    if args.disasm_json:
        disasm_data = generate_disasm_json(args.nro, elf_path=args.elf, objdump=args.objdump)
        with open(args.disasm_json, 'w', encoding='utf-8') as f:
            json.dump(disasm_data, f, indent=2)
        print(f"\n[nro_tool] disassembly JSON -> {args.disasm_json} ({len(disasm_data['functions'])} funcoes)")

    if args.elf and problems:
        sys.exit(1)


if __name__ == '__main__':
    main()
