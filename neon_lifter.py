"""neon_lifter.py - Lifter simbolico NEON (AArch64) para Fases 1, 2 e 3 (libnx / Switch HLE).

Uso:
  python neon_lifter.py ghidra_extraction_switch.json --expected 88196450180 --qwen reconstructed_switch.c
"""
import argparse, json, re, sys, urllib.request

M32, M64 = 0xFFFFFFFF, 0xFFFFFFFFFFFFFFFF
BT3 = chr(96) * 3


class LiftError(Exception):
    pass


# ---------------------------------------------------------------- expressoes simbolicas
class E:
    """No de expressao: 'var' (i+c), 'mem' (arr, c), 'sym' (nome escalar), 'const', ou op binario."""
    def __init__(self, op, *a):
        self.op, self.a = op, a

    def offsets(self):
        if self.op in ('var', 'mem'):
            return {self.a[-1]}
        if self.op in ('const', 'sym'):
            return set()
        return self.a[0].offsets() | self.a[1].offsets()

    def show(self, x='i', relative=False):
        if self.op == 'var':
            c = self.a[0]
            return x if (relative or c == 0) else f"({x} + {c})"
        if self.op == 'mem':
            arr, c = self.a
            idx = x if (relative or c == 0) else f"{x} + {c}"
            return f"{arr}[{idx}]"
        if self.op == 'sym':
            return self.a[0]
        if self.op == 'const':
            return hex(self.a[0]) if self.a[0] >= 16 else str(self.a[0])
        l, r = self.a
        if self.op == '+' and l.show(x, relative) == r.show(x, relative):
            return f"({l.show(x, relative)} << 1)"
        return f"({l.show(x, relative)} {self.op} {r.show(x, relative)})"

    def eval(self, x, mem_lookup=None, sym_env=None):
        if self.op == 'var':
            return x & M32
        if self.op == 'mem':
            arr, _ = self.a
            return mem_lookup(arr, x) & M32
        if self.op == 'sym':
            return (sym_env or {}).get(self.a[0], 1) & M32
        if self.op == 'const':
            return self.a[0] & M32
        l = self.a[0].eval(x, mem_lookup, sym_env)
        r = self.a[1].eval(x, mem_lookup, sym_env)
        return {
            '+': lambda: (l + r) & M32,
            '-': lambda: (l - r) & M32,
            '*': lambda: (l * r) & M32,
            '^': lambda: l ^ r,
            '&': lambda: l & r,
            '>>': lambda: l >> r,
            '<<': lambda: (l << r) & M32,
        }[self.op]()


def s_add(a, b):
    if a.op == 'var' and b.op == 'const':
        return E('var', a.a[0] + b.a[0])
    if a.op == 'const' and b.op == 'var':
        return E('var', b.a[0] + a.a[0])
    if a.op == 'const' and b.op == 'const':
        return E('const', (a.a[0] + b.a[0]) & M32)
    if a.op == '*' and a.a[0].op == 'var' and a.a[1].op == 'const' and b.op == 'const':
        k = a.a[1].a[0]
        c = b.a[0]
        if k != 0 and c >= k:
            extra_i, rem = divmod(c, k)
            base_mul = E('*', E('var', a.a[0].a[0] + extra_i), E('const', k))
            return base_mul if rem == 0 else E('+', base_mul, E('const', rem))
    return E('+', a, b)


SYM = {
    'add': s_add,
    'sub': lambda a, b: E('-', a, b),
    'mul': lambda a, b: E('*', a, b),
    'eor': lambda a, b: E('^', a, b),
    'and': lambda a, b: E('&', a, b),
    'ushr': lambda a, n: E('>>', a, E('const', n)),
    'shl': lambda a, n: E('<<', a, E('const', n)),
    'uaddw': lambda acc, v: v if (acc.op == 'const' and acc.a[0] == 0) else E('+', acc, v),
}
CON = {
    'add': lambda a, b: (a + b) & M32,
    'sub': lambda a, b: (a - b) & M32,
    'mul': lambda a, b: (a * b) & M32,
    'eor': lambda a, b: a ^ b,
    'and': lambda a, b: a & b,
    'ushr': lambda a, n: a >> n,
    'shl': lambda a, n: (a << n) & M32,
    'uaddw': lambda acc, v: (acc + v) & M64,
}


# ---------------------------------------------------------------- parsing
def parse(line):
    addr, _, text = line.partition(': ')
    mnem, _, rest = text.strip().partition(' ')
    ops, depth, cur = [], 0, ''
    for ch in rest:
        if ch == '[':
            depth += 1
        elif ch == ']':
            depth -= 1
        if ch == ',' and depth == 0:
            ops.append(cur.strip())
            cur = ''
        else:
            cur += ch
    if cur.strip():
        ops.append(cur.strip())
    return addr, mnem.lower(), ops


def vreg(op):
    m = re.fullmatch(r'v(\d+)\.(\d+)([BHSD])', op, re.I)
    if not m:
        raise LiftError(f"operando vetorial inesperado: {op}")
    return f"v{m.group(1)}", int(m.group(2)), {'B': 8, 'H': 16, 'S': 32, 'D': 64}[m.group(3).upper()]


def qreg(op):
    m = re.fullmatch(r'[qd](\d+)', op, re.I)
    if not m:
        raise LiftError(f"registrador Q/D inesperado: {op}")
    return f"v{m.group(1)}"


def imm(op):
    return int(op.lstrip('#'), 0)


# ---------------------------------------------------------------- pre-cabecalho
def resolve_preheader(preheader, resolved_data, body, is_switch=False):
    scal, vec, sym_vecs, notes = {}, {}, {}, []
    sp_q = None
    malloc_names = ['pos_x', 'vel_x'] if is_switch else ['in_buf', 'out_buf']
    # x19/x20 guardam os dois buffers do malloc na funcao inteira; loops posteriores
    # (ex.: mov x9, x19) dependem deles mesmo sem o malloc no proprio pre-cabecalho.
    ptr_names = {'19': malloc_names[0], '20': malloc_names[1]}
    malloc_count = 0

    for line in preheader:
        addr, mn, ops = parse(line)
        if mn == 'mov' and ops[0].startswith('v'):
            d, s = vreg(ops[0])[0], vreg(ops[1])[0]
            vec[d] = {'lanes': list(vec[s]['lanes']), 'width': vec[s]['width']}
        elif mn == 'mov' and ops[0][0] in 'wx':
            dst, src = ops[0][1:], ops[1]
            if src in ('xzr', 'wzr'):
                scal[dst] = 0
            elif src.startswith('#') or src.startswith('0x') or src.isdigit():
                scal[dst] = imm(src)
                notes.append(f"{line}  =>  {ops[0]} = {scal[dst]:#x}")
            elif src == 'x0':
                if dst in ('19', '20') and malloc_count < 2:
                    name = malloc_names[min(malloc_count, 1)]
                    malloc_count += 1
                    ptr_names[dst] = name
                    scal[dst] = 0
                    notes.append(f"{line}  =>  x{dst} = ponteiro `{name}`")
            elif src[1:] in ptr_names:
                ptr_names[dst] = ptr_names[src[1:]]
                scal[dst] = 0
        elif mn == 'movk':
            r, v = ops[0][1:], imm(ops[1])
            sh = int(re.search(r'LSL #(\w+)', ops[2]).group(1), 0) if len(ops) > 2 else 0
            scal[r] = (scal.get(r, 0) & ~(0xFFFF << sh)) | (v << sh)
            notes.append(f"{line}  =>  {ops[0]} = {scal[r]:#x} = {scal[r]}")
        elif mn == 'movi':
            name, n, w = vreg(ops[0])
            val = imm(ops[1])
            if w == 64 and val > M32 and (val & M32) == ((val >> 32) & M32):
                vec[name] = {'lanes': [val & M32] * 4, 'width': 32}
            else:
                vec[name] = {'lanes': [val] * n, 'width': w}
            notes.append(f"{line}  =>  {name} = {vec[name]['lanes']}")
        elif mn == 'dup':
            name, n, w = vreg(ops[0])
            vec[name] = {'lanes': [1] * n, 'width': w}
            sym_vecs[name] = 'boost'
            notes.append(f"{line}  =>  {name} = broadcast(`boost`)")
        elif mn == 'ldr' and ops[0][0] in 'dq':
            vname = qreg(ops[0])
            if ops[1].strip('[]') in ('x9', 'x10'):
                continue
            if '[sp' in ops[1] and sp_q is not None:
                vec[vname] = dict(sp_q)
                continue
            off_m = re.search(r'#(\w+)\]', ops[1])
            off = imm(off_m.group(1)) if off_m else 0
            key = next((k for k in resolved_data if int(k[4:], 16) & 0xFFF == off & 0xFFF), None)
            if key is None:
                raise LiftError(f"ldr sem dado resolvido: {line}")
            nbytes = 16 if ops[0][0].lower() == 'q' else 8
            raw = bytes.fromhex(resolved_data[key].replace(' ', ''))[:nbytes]
            use = next((o for l in body for o in parse(l)[2] if o.lower().startswith(vname + '.')), None)
            if use and vreg(use)[2] in (32, 64):
                _, n, w = vreg(use)
            else:
                w, n = 32, nbytes // 4
            lanes = [int.from_bytes(raw[k * w // 8:(k + 1) * w // 8], 'little') for k in range(n)]
            vec[vname] = {'lanes': lanes, 'width': w}
            sp_q = dict(vec[vname])
            notes.append(f"{line}  =>  {key} => {vname} = {lanes}")
        elif mn in ('adrp', 'adr', 'sub', 'str', 'add', 'mul', 'cmp', 'stp', 'ldp',
                    'b.ne', 'tst', 'csinc', 'ushr', 'subs', 'and', 'eor', 'shl'):
            pass
        else:
            raise LiftError(f"instrucao de pre-cabecalho nao suportada: {line}")
    return scal, vec, ptr_names, sym_vecs, notes


# ---------------------------------------------------------------- execucao de uma volta
def step(body, vec, scal, sem, ptr_names=None, sym_mode=False, contrib=None, stores=None, mem_bufs=None):
    branch = None
    addr_map = {}

    for line in body:
        addr, mn, ops = parse(line)
        if mn in ('add', 'sub', 'mul', 'eor', 'and') and ops[0].startswith('v'):
            d = vreg(ops[0])[0]
            a, b = vreg(ops[1])[0], vreg(ops[2])[0]
            vec[d] = [sem[mn](x, y) for x, y in zip(vec[a], vec[b])]
        elif mn in ('ushr', 'shl') and ops[0].startswith('v'):
            d, a = vreg(ops[0])[0], vreg(ops[1])[0]
            vec[d] = [sem[mn](x, imm(ops[2])) for x in vec[a]]
        elif mn in ('uaddw', 'uaddw2'):
            d, a, b = vreg(ops[0])[0], vreg(ops[1])[0], vreg(ops[2])[0]
            src_lanes = vec[b][2:4] if mn == 'uaddw2' else vec[b][0:2]
            if contrib is not None:
                contrib.extend((d, k, x) for k, x in enumerate(src_lanes))
            vec[d] = [sem['uaddw'](x, y) for x, y in zip(vec[a], src_lanes)]
        elif mn == 'add' and ops[0][0] in 'wx':
            dst, s1, s2 = ops[0][1:], ops[1][1:], ops[2]
            if s2.startswith('#'):
                inc = imm(s2)
                scal[dst] = (scal.get(s1, 0) + inc) & M64
                branch = ('byte_offset', dst, inc)
            else:
                r2 = s2[1:]
                addr_map[dst] = (ptr_names.get(s1, f"ptr_{s1}"), r2, scal.get(r2, 0))
        elif mn == 'ldr' and ops[0][0].lower() == 'q':
            v1 = qreg(ops[0])
            base_reg = ops[1].strip('[]')[1:]
            arr_name = ptr_names[base_reg]
            elem_off = scal.get(base_reg, 0) // 4
            if sym_mode:
                vec[v1] = [E('mem', arr_name, k) for k in range(4)]
            else:
                vec[v1] = [mem_bufs[arr_name][elem_off + k] for k in range(4)]
            if len(ops) > 2:
                scal[base_reg] = scal.get(base_reg, 0) + imm(ops[2])
        elif mn == 'str' and ops[0][0].lower() == 'q':
            v1 = qreg(ops[0])
            base_reg = ops[1].strip('[]')[1:]
            arr_name = ptr_names[base_reg]
            elem_off = scal.get(base_reg, 0) // 4
            if sym_mode and stores is not None:
                for k, val in enumerate(vec[v1]):
                    stores.append((arr_name, k, val))
            elif not sym_mode and mem_bufs is not None:
                for k, val in enumerate(vec[v1]):
                    mem_bufs[arr_name][elem_off + k] = val & M32
            if len(ops) > 2:
                scal[base_reg] = scal.get(base_reg, 0) + imm(ops[2])
        elif mn == 'ldp' and ops[0][0].lower() == 'q':
            v1, v2 = qreg(ops[0]), qreg(ops[1])
            base_reg = ops[2].strip('[]')[1:]
            arr_name, _, byte_off = addr_map[base_reg]
            elem_off = byte_off // 4
            if sym_mode:
                vec[v1] = [E('mem', arr_name, k) for k in range(4)]
                vec[v2] = [E('mem', arr_name, k + 4) for k in range(4)]
            else:
                vec[v1] = [mem_bufs[arr_name][elem_off + k] for k in range(4)]
                vec[v2] = [mem_bufs[arr_name][elem_off + k + 4] for k in range(4)]
        elif mn == 'stp' and ops[0][0].lower() == 'q':
            v1, v2 = qreg(ops[0]), qreg(ops[1])
            base_reg = ops[2].strip('[]')[1:]
            arr_name, _, byte_off = addr_map.get(base_reg, ('out_buf', '8', 0))
            elem_off = byte_off // 4
            if sym_mode and stores is not None:
                for k, val in enumerate(vec[v1] + vec[v2]):
                    stores.append((arr_name, k, val))
            elif not sym_mode and mem_bufs is not None:
                for k, val in enumerate(vec[v1] + vec[v2]):
                    mem_bufs[arr_name][elem_off + k] = val & M32
        elif mn == 'subs' and ops[0][0] in 'wx':
            branch = ('counter', ops[0][1:], imm(ops[2]))
            scal[ops[0][1:]] = (scal[ops[0][1:]] - imm(ops[2])) & M32
        elif mn == 'cmp' and ops[0][0] in 'wx':
            lim_reg, idx_reg = ops[0][1:], ops[1][1:]
            if branch and branch[0] == 'byte_offset':
                branch = ('cmp_bytes', lim_reg, idx_reg, branch[2])
        elif mn == 'b.ne':
            pass
        else:
            raise LiftError(f"instrucao de loop nao suportada: {line}")
    return branch


def lift_single_loop(loop, resolved_data, idx, is_switch=False):
    body = loop['body']
    scal, vec, ptr_names, sym_vecs, notes = resolve_preheader(loop['preheader'], resolved_data, body, is_switch)

    svec = {}
    for n, v in vec.items():
        if n in sym_vecs:
            svec[n] = [E('sym', sym_vecs[n]) for _ in v['lanes']]
        elif len(set(v['lanes'])) > 1:
            i0 = v['lanes'][0]
            svec[n] = [E('var', x - i0) for x in v['lanes']]
        else:
            svec[n] = [E('const', x) for x in v['lanes']]

    ind = next((n for n, v in vec.items() if len(set(v['lanes'])) > 1), None)
    i0 = vec[ind]['lanes'][0] if ind else 0

    contrib, stores = [], []
    br = step(body, svec, dict(scal), SYM, ptr_names=ptr_names, sym_mode=True, contrib=contrib, stores=stores)

    if br[0] == 'counter':
        _, creg, dec = br
        stride = dec
        trips = scal[creg] // dec
        total = trips * stride
    elif br[0] == 'cmp_bytes':
        _, lim_reg, idx_reg, byte_inc = br
        total_bytes = scal[lim_reg]
        trips = total_bytes // byte_inc
        stride = byte_inc // 4
        total = trips * stride
    else:
        raise LiftError(f"condicao de saida nao reconhecida: {br}")

    store_list = []
    if stores:
        by_arr = {}
        for arr, lane_k, e in stores:
            by_arr.setdefault(arr, []).append((lane_k, e))
        for arr, items in by_arr.items():
            st_templates = {}
            for lane_k, e in items:
                offs = e.offsets()
                if len(offs) != 1:
                    raise LiftError(f"store em {arr} mistura indices: {e.show()}")
                st_templates.setdefault(e.show('i', relative=True), []).append((lane_k, offs.pop(), e))
            if len(st_templates) != 1:
                raise LiftError(f"stores em {arr} com formulas diferentes: {list(st_templates)}")
            s_str, s_uses = next(iter(st_templates.items()))
            store_list.append((arr, s_uses[0][2], s_str))

    acc_tmpl, acc_str, uses = None, None, []
    if contrib:
        templates = {}
        for acc, lane, e in contrib:
            offs = e.offsets()
            if len(offs) != 1:
                raise LiftError(f"acumulador mistura indices: {e.show()}")
            templates.setdefault(e.show('i', relative=True), []).append((acc, lane, offs.pop(), e))
        if len(templates) != 1:
            raise LiftError(f"lanes com formulas diferentes: {list(templates)}")
        acc_str, uses = next(iter(templates.items()))
        acc_tmpl = uses[0][3]

    return dict(idx=idx, notes=notes, ind=ind, i0=i0, stride=stride, trips=trips, total=total,
                stores=store_list, tmpl=acc_tmpl, tmpl_str=acc_str, uses=uses,
                accs=sorted({u[0] for u in uses}), scal=scal, vec=vec, ptr_names=ptr_names,
                sym_vecs=sym_vecs, body=body)


def validate_all(loops, is_switch=False, trips_check=500):
    n_check = trips_check * loops[0]['stride']
    buf_names = ['pos_x', 'vel_x'] if is_switch else ['in_buf', 'out_buf']
    mem_c = {k: [0] * n_check for k in buf_names}
    mem_s = {k: [0] * n_check for k in buf_names}

    for L in loops:
        t_loops = (trips_check * 2) if (is_switch and L['stride'] == 4) else trips_check
        vec = {n: list(v['lanes']) for n, v in L['vec'].items()}
        scal = dict(L['scal'])
        for _ in range(t_loops):
            step(L['body'], vec, scal, CON, ptr_names=L['ptr_names'], sym_mode=False, mem_bufs=mem_c)

        sym_acc = 0
        for i in range(n_check):
            # todas as formulas leem os valores carregados no inicio da volta (ldr q antes de str q)
            new_vals = [(arr, tmpl.eval(i, lambda a, idx: mem_s[a][idx], {'boost': 1})) for arr, tmpl, _ in L['stores']]
            for arr, v in new_vals:
                mem_s[arr][i] = v
            if L['tmpl']:
                sym_acc = (sym_acc + L['tmpl'].eval(i, lambda a, idx: mem_s[a][idx], {'boost': 1})) & M64
        if L['accs']:
            con_acc = sum(sum(vec[a]) for a in L['accs']) & M64

    # Calculo completo sobre N elementos
    total_n = loops[-1]['total']
    if is_switch:
        # Validacao rapida de equivalencia + execucao de todos os 60 frames sobre N=100.000
        pos_x = [loops[0]['stores'][0][1].eval(i) for i in range(total_n)]
        vel_x = [loops[0]['stores'][1][1].eval(i) for i in range(total_n)]
        vel_tmpl = next(t for a, t, _ in loops[1]['stores'] if a == 'vel_x')
        pos_tmpl = next(t for a, t, _ in loops[1]['stores'] if a == 'pos_x')
        for frame in range(1, 61):
            b_val = 65 if frame == 30 else 1
            env = {'boost': b_val}
            load = lambda a, idx: vel_x[idx] if a == 'vel_x' else pos_x[idx]
            for i in range(total_n):
                nv = vel_tmpl.eval(i, load, env)
                np = pos_tmpl.eval(i, load, env)
                vel_x[i], pos_x[i] = nv, np
        full_acc = sum(pos_x) & M64
    elif loops[0]['stores']:
        full_acc = 0
        for i in range(total_n):
            in_val = loops[0]['stores'][0][1].eval(i)
            elem = loops[-1]['tmpl'].eval(i, lambda a, idx: in_val)
            full_acc = (full_acc + elem) & M64
    else:
        full_acc = sum(loops[0]['tmpl'].eval(i) for i in range(loops[0]['i0'], loops[0]['i0'] + total_n)) & M64

    return con_acc, sym_acc, full_acc


def sequential_stores(stores):
    """As formulas dos stores leem os valores carregados no inicio da volta (ldr q antes de str q).
    Em C escalar as atribuicoes rodam em sequencia: se a formula de um store contem a de outro,
    esse outro vai primeiro e a subexpressao vira `arr[i]` (o valor recem-escrito)."""
    order = sorted(stores, key=lambda s: sum(o[2] in s[2] for o in stores if o is not s))
    out, written = [], []
    for arr, _, s_str in order:
        for w_arr, w_str in written:
            s_str = s_str.replace(w_str, '@@')
            if f"{w_arr}[i]" in s_str:
                raise LiftError(f"{arr}[i] usa o valor antigo de {w_arr}[i]; precisa de temporario")
            s_str = s_str.replace('@@', f"{w_arr}[i]")
        out.append((arr, s_str))
        written.append((arr, next(s for a, _, s in stores if a == arr)))
    return out


def summary_all(loops):
    blocks = []
    for L in loops:
        lines = [f"## Loop NEON {L['idx']} (passo SIMD = {L['stride']} elementos/volta, total = {L['total']} elementos)"]
        seq = sequential_stores(L['stores'])
        if any(s != orig for (_, s), (_, _, orig) in zip(seq, L['stores'])) or \
                [a for a, _ in seq] != [a for a, _, _ in L['stores']]:
            lines.append("- Atribuicoes em sequencia, nesta ordem, dentro do mesmo `for` "
                         "(cada linha ja usa o valor recem-escrito pela anterior):")
        for arr, s_str in seq:
            lines.append(f"- Atualizacao de array: `{arr}[i] = {s_str}` (para i = 0 .. {L['total'] - 1})")
        if L['tmpl']:
            lines.append(f"- Acumulador 64 bits: `checksum += {L['tmpl_str']}` (para i = 0 .. {L['total'] - 1})")
        blocks.append('\n'.join(lines))
    return '\n\n'.join(blocks)


def skeleton(fn, is_switch=False):
    c = fn['decompiled_c']
    if is_switch:
        return (
            '#include <stdio.h>\n#include <stdlib.h>\n#include <stdint.h>\n#include "switch_hle_shim.h"\n\n'
            'int main(void) {\n'
            '  PadState pad;\n'
            '  consoleInit(NULL);\n'
            '  padConfigureInput(1, HidNpadStyleSet_NpadStandard);\n'
            '  padInitializeDefault(&pad);\n'
            '  puts("[+] Switch Particle Bench (libnx HLE shim) iniciado no Android!");\n'
            '  uint32_t *pos_x = (uint32_t *)malloc(100000 * sizeof(uint32_t));\n'
            '  uint32_t *vel_x = (uint32_t *)malloc(100000 * sizeof(uint32_t));\n'
            '  if (!pos_x || !vel_x) return 1;\n'
            '  /* <LOOP_NEON_1> */\n'
            '  u64 start_tick = armGetSystemTick();\n'
            '  uint32_t frames = 0;\n'
            '  while (appletMainLoop()) {\n'
            '    padUpdate(&pad);\n'
            '    u64 kDown = padGetButtonsDown(&pad);\n'
            '    uint32_t boost = (kDown & HidNpadButton_A) ? 65 : 1;\n'
            '    /* <LOOP_NEON_2> */\n'
            '    frames++;\n'
            '    consoleUpdate(NULL);\n'
            '    if (kDown & HidNpadButton_Plus) break;\n'
            '  }\n'
            '  u64 end_tick = armGetSystemTick();\n'
            '  uint64_t checksum = 0;\n'
            '  /* <LOOP_NEON_3> */\n'
            '  printf("[+] Frames: %u\\n", frames);\n'
            '  printf("[+] Checksum: %llu\\n", (unsigned long long)checksum);\n'
            '  printf("[+] Ultima particula: %u\\n", pos_x[100000 - 1]);\n'
            '  printf("[+] Tempo (60 frames): %.4f ms\\n", (double)armTicksToNs(end_tick - start_tick) / 1000000.0);\n'
            '  free(pos_x);\n  free(vel_x);\n  consoleExit(NULL);\n  return 0;\n}'
        )
    loop_idx = 0
    def repl_loop(_):
        nonlocal loop_idx
        loop_idx += 1
        return f"\n  /* <LOOP_NEON_{loop_idx}> */"
    c = re.sub(r'\n\s*do \{.*?\} while \([^)]*\);', repl_loop, c, flags=re.S)
    c = c.replace('__ptr_00', 'out_buf').replace('__ptr', 'in_buf')
    c = c.replace('cVar7', 'start_time').replace('cVar8', 'end_time')
    return c


def brace_depth(code, pos):
    return code.count('{', 0, pos) - code.count('}', 0, pos)


def check_placement(skel, code, loops):
    """Cada loop gerado precisa ficar na mesma profundidade de chaves do seu placeholder."""
    squash = lambda t: re.sub(r'\s+', '', t)
    flat = squash(code)
    problems = []
    for L in loops:
        ph = skel.find(f"/* <LOOP_NEON_{L['idx']}> */")
        if ph < 0:
            continue
        want = brace_depth(squash(skel), squash(skel).find(squash(f"/* <LOOP_NEON_{L['idx']}> */")))
        anchors = [f"{a}[i]={t}" for a, t in sequential_stores(L['stores'])]
        if L['tmpl']:
            anchors.append(f"checksum+={L['tmpl_str']}")
        hits = [flat.find(squash(a)) for a in anchors]
        if any(h < 0 for h in hits):
            problems.append(f"LOOP_NEON_{L['idx']}: formula do lifter ausente ou alterada")
            continue
        got = brace_depth(flat, min(hits)) - 1  # descontar a chave do proprio for
        if got != want:
            problems.append(f"LOOP_NEON_{L['idx']} esta em profundidade {got}, mas o placeholder esta em {want} "
                            f"(o loop foi movido para {'dentro' if got > want else 'fora'} de um bloco)")
    return problems


def placement_hints(skel):
    """Descreve, para cada placeholder, se ele roda uma vez ou dentro de qual bloco do esqueleto."""
    hints = []
    for m in re.finditer(r'/\* <LOOP_NEON_(\d+)> \*/', skel):
        depth = brace_depth(skel, m.start())
        if depth <= 1:
            hints.append(f"- LOOP_NEON_{m.group(1)}: no corpo de `main`, FORA de qualquer `while`; executa uma unica vez.")
        else:
            opener = skel[:m.start()].rfind('{')
            head = skel[skel.rfind('\n', 0, opener) + 1:opener].strip()
            hints.append(f"- LOOP_NEON_{m.group(1)}: DENTRO de `{head}`; executa a cada volta desse bloco.")
    return '\n'.join(hints)


def qwen_chat(messages):
    req = urllib.request.Request(
        "http://localhost:11434/api/chat",
        data=json.dumps({"model": "qwen2.5-coder:7b", "messages": messages,
                         "stream": False, "options": {"temperature": 0, "num_ctx": 8192}}).encode(),
        headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=900) as r:
        resp = json.load(r)
    content = resp['message']['content']
    m = re.search(BT3 + r'c\s*(.*?)' + BT3, content, re.S)
    print(f"\n[qwen] prompt_tokens={resp.get('prompt_eval_count')} eval_count={resp.get('eval_count')} "
          f"total_s={round(resp.get('total_duration', 0) / 1e9, 1)}")
    return content, (m.group(1).strip() + '\n' if m else content)


def ask_qwen(summ, skel, out, loops, retries=1):
    prompt = (
        "Um lifter simbolico deterministico analisou os loops vetorizados (NEON) da funcao `main` de um binario AArch64. "
        "Use as formulas abaixo como verdade absoluta:\n\n"
        f"{summ}\n\n"
        "## Esqueleto da funcao main\n"
        f"{BT3}c\n{skel}\n{BT3}\n\n"
        "## Posicao de cada loop (obrigatoria)\n"
        f"{placement_hints(skel)}\n\n"
        "Tarefa: gere o codigo C escalar final, limpo e completo.\n"
        "Regras obrigatorias:\n"
        "1. Substitua cada `/* <LOOP_NEON_N> */` pelo loop `for (uint32_t i = 0; i < N; i++)` escalar correspondente, "
        "exatamente na posicao do placeholder: nao mova loops para dentro ou para fora de outros blocos.\n"
        "2. Mantenha todas as chamadas do shim `libnx` (`consoleInit`, `appletMainLoop`, `padUpdate`, `padGetButtonsDown`, `armGetSystemTick`, `armTicksToNs`, `consoleUpdate`, `consoleExit`) intactas.\n"
        f"3. Responda apenas com o bloco {BT3}c contendo o codigo final compilavel."
    )
    messages = [{"role": "user", "content": prompt}]
    for attempt in range(retries + 1):
        content, code = qwen_chat(messages)
        problems = check_placement(skel, code, loops)
        if not problems:
            break
        print("[qwen] estrutura divergente do esqueleto:\n  - " + "\n  - ".join(problems))
        messages += [{"role": "assistant", "content": content},
                     {"role": "user", "content": "O codigo nao respeita o esqueleto:\n- " + "\n- ".join(problems) +
                      f"\nCorrija e responda apenas com o bloco {BT3}c completo."}]
    with open(out, 'w', encoding='utf-8') as f:
        f.write(code)
    if problems:
        sys.exit(f"[qwen] estrutura ainda divergente apos {retries + 1} tentativas; codigo salvo em {out} para inspecao")
    print(f"[qwen] estrutura conferida com o esqueleto -> {out}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('json')
    ap.add_argument('--expected', type=int, help='resultado observado no aparelho')
    ap.add_argument('--qwen', metavar='OUT', help='envia ao Qwen e grava o codigo C em OUT')
    args = ap.parse_args()

    with open(args.json, encoding='utf-8') as f:
        data = json.load(f)
    is_switch = 'switch' in data.get('binary_name', '').lower()
    fn = next(x for x in data['functions'] if x['name'] == 'main')

    # Filtra apenas loops folha NEON (ignora loops de controle que contem chamadas 'bl ')
    neon_loops = [lp for lp in fn['loops'] if not any(': bl ' in insn for insn in lp['body'])]

    loops = [lift_single_loop(lp, fn['resolved_data'], i + 1, is_switch) for i, lp in enumerate(neon_loops)]
    summ = summary_all(loops)
    print(summ)

    concrete, symbolic, full = validate_all(loops, is_switch)
    print("\n## Validacao Deterministica")
    print(f"- Execucao concreta NEON (500 voltas): {concrete}")
    print(f"- Formula simbolica nos mesmos i:      {symbolic}  -> {'OK' if concrete == symbolic else 'DIVERGE'}")
    print(f"- Checksum total ({loops[-1]['total']} elementos):   {full}"
          + (f"  -> aparelho {args.expected}: {'OK' if full == args.expected else 'DIVERGE'}" if args.expected else ''))
    if concrete != symbolic or (args.expected and full != args.expected):
        sys.exit("[lifter] validacao falhou; abortando.")

    skel = skeleton(fn, is_switch)
    print("\n## Esqueleto Limpo\n" + skel)
    if args.qwen:
        ask_qwen(summ, skel, args.qwen, loops)


if __name__ == '__main__':
    main()