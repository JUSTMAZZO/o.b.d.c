"""nro_tool.py - leitor de NRO (homebrew do Switch) para a reemissao estatica da Fase 3.

Le o cabecalho NRO0, os segmentos .text/.rodata/.data (+ .bss), o MOD0, a secao .dynamic e as
relocations (RELA e RELR; o link da libnx usa -z pack-relative-relocs), e faz o inventario das
instrucoes que nao podem rodar como estao no Android:

  svc #imm            -> chamada ao kernel Horizon (vira trampolim para o HLE)
  mrs xN, tpidrro_el0 -> TLS do Horizon (no Android le 0)
  mrs xN, cntpct_el0  -> contador fisico (SIGILL no A56; vira cntvct_el0 escalado)
  mrs xN, cntfrq_el0  -> frequencia (Switch: 19,2 MHz; A56: 25,6 MHz)

Uso: python nro_tool.py arquivo.nro [--elf arquivo.elf] [--json saida.json]
Com --elf, cada sitio e conferido contra o ELF original (bytes e funcao que o contem).
"""
import argparse
import json
import re
import struct
import subprocess
import sys

SEGMENTS = ('text', 'ro', 'data')

DT = {0: 'NULL', 7: 'RELA', 8: 'RELASZ', 9: 'RELAENT', 23: 'JMPREL', 2: 'PLTRELSZ', 30: 'FLAGS',
      0x6ffffffb: 'FLAGS_1', 36: 'RELR', 35: 'RELRSZ', 37: 'RELRENT', 5: 'STRTAB', 6: 'SYMTAB',
      10: 'STRSZ', 11: 'SYMENT', 0x6ffffef5: 'GNU_HASH', 4: 'HASH', 0x6ffffff9: 'RELACOUNT'}
R_AARCH64_RELATIVE = 1027

SYSREGS = {0xD53BD060: 'tpidrro_el0', 0xD53BE020: 'cntpct_el0', 0xD53BE000: 'cntfrq_el0',
           0xD53BE040: 'cntvct_el0', 0xD53BD040: 'tpidr_el0'}
MSR_TPIDR = 0xD51BD040


class NroError(Exception):
    pass


def u32(b, o):
    return struct.unpack_from('<I', b, o)[0]


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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('nro')
    ap.add_argument('--elf', help='ELF original (com simbolos) para conferir os sitios')
    ap.add_argument('--objdump', default=r'C:\devkitPro\devkitA64\bin\aarch64-none-elf-objdump.exe')
    ap.add_argument('--json', help='grava o inventario em JSON')
    args = ap.parse_args()

    nro = parse_nro(open(args.nro, 'rb').read())
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

    if args.elf:
        problems = elf_crosscheck(nro, sites, args.elf, args.objdump)
        print(f"  conferencia com {args.elf}: {'OK' if not problems else 'DIVERGE'}")
        for p in problems:
            print(f"    - {p}")

    print(f"\n## Sitios para patch ({len(sites)})")
    for s in sites:
        what = f"svc #{s['imm']:#x}" if s['kind'] == 'svc' else f"{s['kind']} x{s['rt']}, {s['sysreg']}"
        print(f"  {s['offset']:#08x}  {what:<26} {s.get('function', '')}")

    if args.json:
        out = {k: v for k, v in nro.items() if k != 'image'}
        out['relocs'] = [{'kind': k, 'offset': o, 'addend': a} for k, o, a in nro['relocs']]
        out['sites'] = sites
        json.dump(out, open(args.json, 'w', encoding='utf-8'), indent=2)
        print(f"\n[nro_tool] inventario -> {args.json}")
    if args.elf and problems:
        sys.exit(1)


if __name__ == '__main__':
    main()
