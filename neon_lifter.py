"""neon_lifter.py - Lifter simbolico NEON (AArch64) para Fases 1, 2 e 3 (libnx / Switch HLE).

Uso:
  python neon_lifter.py ghidra_extraction_switch.json --expected 88196450180 --qwen reconstructed_switch.c
"""
import argparse, glob, json, os, re, subprocess, sys, tempfile, urllib.request

import cfg_skeleton
from collections import Counter, defaultdict

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

    def arrays(self):
        if self.op == 'mem':
            return {self.a[0]}
        if self.op in ('var', 'const', 'sym'):
            return set()
        return self.a[0].arrays() | self.a[1].arrays()

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
def resolve_preheader(preheader, resolved_data, body):
    scal, vec, sym_vecs, notes = {}, {}, {}, []
    sp_q = None
    # Registradores callee-saved (x19..x28) vivem a funcao inteira: um buffer guardado neles
    # (ex.: retorno do malloc) e reaproveitado por loops posteriores via copia (mov x9, x19).
    # O nome vem do registrador; o LLM nao renomeia, porque as formulas servem de ancora.
    ptr_names = {str(n): f"buf_x{n}" for n in range(19, 29)}

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
            elif src == 'x0' and dst in ptr_names:
                scal[dst] = 0
                notes.append(f"{line}  =>  x{dst} = ponteiro `{ptr_names[dst]}` (retorno de chamada)")
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
            if base_reg not in addr_map:
                raise LiftError(f"stp com base de endereco desconhecida: {line}")
            arr_name, _, byte_off = addr_map[base_reg]
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


def lift_single_loop(loop, resolved_data, idx):
    body = loop['body']
    scal, vec, ptr_names, sym_vecs, notes = resolve_preheader(loop['preheader'], resolved_data, body)

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

    arrays = {arr for arr, _, _ in store_list}
    for _, tmpl, _ in store_list:
        arrays |= tmpl.arrays()
    if acc_tmpl is not None:
        arrays |= acc_tmpl.arrays()

    return dict(idx=idx, start=loop['start'], end=loop['end'], arrays=arrays,
                notes=notes, ind=ind, i0=i0, stride=stride, trips=trips, total=total,
                stores=store_list, tmpl=acc_tmpl, tmpl_str=acc_str, uses=uses,
                accs=sorted({u[0] for u in uses}), scal=scal, vec=vec, ptr_names=ptr_names,
                sym_vecs=sym_vecs, body=body)


def validate_all(loops, trips_check=500):
    """Por loop, na mesma memoria compartilhada: execucao concreta das instrucoes NEON x formulas
    simbolicas. Devolve [(loop, o que foi comparado, concreto, simbolico)]. O valor do programa
    inteiro (frames, chamadas, printf) vem da execucao do C de referencia no host."""
    n_check = min(trips_check * max(L['stride'] for L in loops), min(L['total'] for L in loops))
    mem_c = defaultdict(lambda: [0] * n_check)
    mem_s = defaultdict(lambda: [0] * n_check)
    results = []
    for L in loops:
        vec = {n: list(v['lanes']) for n, v in L['vec'].items()}
        scal = dict(L['scal'])
        for _ in range(n_check // L['stride']):
            step(L['body'], vec, scal, CON, ptr_names=L['ptr_names'], sym_mode=False, mem_bufs=mem_c)

        load = lambda a, idx: mem_s[a][idx]
        sym_acc = 0
        for i in range(n_check):
            x = i + L['i0']
            # todas as formulas leem os valores carregados no inicio da volta (ldr q antes de str q)
            new_vals = [(arr, tmpl.eval(x, load)) for arr, tmpl, _ in L['stores']]
            for arr, v in new_vals:
                mem_s[arr][i] = v
            if L['tmpl'] is not None:
                sym_acc = (sym_acc + L['tmpl'].eval(x, load)) & M64
        for arr in sorted({a for a, _, _ in L['stores']}):
            # soma so para exibir; o veredito compara o array inteiro
            results.append((L['idx'], f"memoria {arr}[0..{n_check - 1}] (soma)", sum(mem_c[arr]), sum(mem_s[arr]),
                            mem_c[arr] == mem_s[arr]))
        if L['accs']:
            con_acc = sum(sum(vec[a]) for a in L['accs']) & M64
            results.append((L['idx'], f"acumulador ({n_check} elementos)", con_acc, sym_acc, con_acc == sym_acc))
    return results


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


def fill_loops(skel, loops):
    """Preenche os placeholders com os loops escalares do proprio lifter (C de referencia, sem LLM)."""
    for L in loops:
        ph = f"/* <LOOP_NEON_{L['idx']}> */"
        m = re.search(r'^( *)' + re.escape(ph), skel, re.M)
        if not m:
            raise LiftError(f"placeholder ausente no esqueleto: {ph}")
        if L['stores'] and L['i0'] != 0:
            raise LiftError(f"loop {L['idx']}: stores com inducao deslocada (i0={L['i0']}) nao suportados")
        body = [f"for (uint32_t i = {L['i0']}; i < {L['i0'] + L['total']}u; i++) {{"]
        body += [f"  {arr}[i] = {expr};" for arr, expr in sequential_stores(L['stores'])]
        if L['tmpl'] is not None:
            body.append(f"  checksum += {L['tmpl_str']};")
        body.append("}")
        skel = skel.replace(m.group(0), '\n'.join(m.group(1) + line for line in body), 1)
    return skel


def brace_depth(code, pos):
    return code.count('{', 0, pos) - code.count('}', 0, pos)


def check_placement(skel, code, loops):
    """Cada loop gerado precisa ficar na mesma profundidade de chaves do seu placeholder."""
    # sem comentarios (exceto os placeholders) e sem espacos
    strip = lambda t: re.sub(r'//[^\n]*|/\*.*?\*/', lambda m: m.group(0) if '<LOOP_NEON_' in m.group(0) else '',
                             t, flags=re.S)
    squash = lambda t: re.sub(r'\s+', '', strip(t))
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
        if all(h >= 0 for h in hits):
            pos = min(hits)
            got = brace_depth(flat, pos) - 1  # descontar a chave do proprio for
        else:
            # formula reescrita (ex.: temporario compartilhado): localiza pelo k-esimo `for`; a
            # equivalencia da formula fica com o gate do host, que compara a saida impressa
            order = [int(n) for n in re.findall(r'<LOOP_NEON_(\d+)>', skel)].index(L['idx'])
            fors = [m.start() for m in re.finditer(r'(?<![A-Za-z_0-9])for\(', flat)]
            if order >= len(fors):
                problems.append(f"LOOP_NEON_{L['idx']}: loop ausente no codigo gerado")
                continue
            pos = fors[order]
            got = brace_depth(flat, pos)
        if got != want:
            problems.append(f"LOOP_NEON_{L['idx']} esta em profundidade {got}, mas o placeholder esta em {want} "
                            f"(o loop foi movido para {'dentro' if got > want else 'fora'} de um bloco)")
        # chamadas antes do loop: mesmas, e na mesma quantidade, que antes do placeholder
        ph_pos = squash(skel).find(squash(f"/* <LOOP_NEON_{L['idx']}> */"))
        want_calls, got_calls = calls_before(squash(skel), ph_pos), calls_before(flat, pos)
        if want_calls != got_calls:
            moved = sorted(set(want_calls.elements()) ^ set(got_calls.elements()))
            problems.append(f"LOOP_NEON_{L['idx']} mudou de posicao em relacao as chamadas {moved}: "
                            f"no esqueleto elas ficam {'antes' if (want_calls - got_calls) else 'depois'} do loop")
    return problems


def calls_before(flat, pos):
    """Contagem das chamadas de funcao (sem palavras-chave nem casts) em flat[:pos]."""
    names = re.findall(r'([A-Za-z_]\w*)\(', flat[:pos])
    return Counter(n for n in names if n not in ('if', 'while', 'for', 'return', 'sizeof', 'main', 'switch'))


def placement_hints(skel):
    """Descreve, para cada placeholder, se ele roda uma vez ou dentro de qual bloco do esqueleto."""
    hints = []
    lines = skel.split('\n')
    for m in re.finditer(r'/\* <LOOP_NEON_(\d+)> \*/', skel):
        depth = brace_depth(skel, m.start())
        if depth <= 1:
            where = "no corpo de `main`, FORA de qualquer `while`; executa uma unica vez"
        else:
            opener = skel[:m.start()].rfind('{')
            head = skel[skel.rfind('\n', 0, opener) + 1:opener].strip()
            where = f"DENTRO de `{head}`; executa a cada volta desse bloco"
        n = skel.count('\n', 0, m.start())
        prev = next((l.strip() for l in reversed(lines[:n]) if l.strip() and l.strip() != 'checksum = 0;'), '')
        nxt = next((l.strip() for l in lines[n + 1:] if l.strip()), '')
        hints.append(f"- LOOP_NEON_{m.group(1)}: {where}; logo depois de `{prev}` e antes de `{nxt}`.")
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


# ---------------------------------------------------------------- gate no host (antes do ADB)
HERE = os.path.dirname(os.path.abspath(__file__))
TIMING = re.compile(r'^-?[\d.,]+\s*(ms|s|us|ns)$')


def host_cc():
    """Clang nativo do host: $OBDC_HOST_CC ou o llvm-mingw em ../tools/."""
    env = os.environ.get('OBDC_HOST_CC')
    if env:
        return env
    found = sorted(glob.glob(os.path.join(HERE, '..', 'tools', 'llvm-mingw-*', 'bin', 'clang.exe')))
    return found[-1] if found else None


def host_run(c_path):
    """Compila e executa o C no host. Devolve (saida [(rotulo, valor)] sem tempos, erro ou None)."""
    cc = host_cc()
    if cc is None:
        return None, "compilador do host nao encontrado (defina OBDC_HOST_CC ou instale llvm-mingw em ../tools/)"
    exe = os.path.abspath(os.path.splitext(c_path)[0] + '_host.exe')
    # objetos de pilha ainda sem tipo (uint8_t[] passado como PadState *): erro no clang 23, aviso no NDK
    comp = subprocess.run([cc, '-O2', '-w', '-Wno-error=incompatible-pointer-types', '-I', HERE, c_path, '-o', exe, '-lpthread'],
                          capture_output=True, text=True)
    if comp.returncode != 0:
        errs = [l for l in comp.stderr.splitlines() if 'error' in l][:5]
        return None, "nao compila no host:\n    " + "\n    ".join(errs or comp.stderr.splitlines()[:5])
    try:
        run = subprocess.run([exe], capture_output=True, text=True, timeout=120)
    except subprocess.TimeoutExpired:
        return None, "execucao no host passou de 120 s"
    if run.returncode != 0:
        return None, f"execucao no host terminou com codigo {run.returncode}"
    values = []
    for line in run.stdout.splitlines():
        m = re.match(r'^\[\+\]\s*(.+?):\s*(.+?)\s*$', line)
        if m and not TIMING.match(m.group(2)):
            values.append((m.group(1), m.group(2)))
    return values, None


def output_diff(ref, got):
    """Linhas `- ref` / `+ gerado` para os valores impressos que divergem."""
    diff = []
    for k in range(max(len(ref), len(got))):
        r = ref[k] if k < len(ref) else None
        g = got[k] if k < len(got) else None
        if r != g:
            if r:
                diff.append(f"- {r[0]}: {r[1]}   (referencia)")
            if g:
                diff.append(f"+ {g[0]}: {g[1]}   (gerado)")
    return diff


def host_gate(code, ref_values, out):
    """Compila e roda o C gerado no host e compara a saida com a do C de referencia."""
    path = os.path.join(tempfile.gettempdir(), os.path.splitext(os.path.basename(out))[0] + '_gate.c')
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(code)
    got, err = host_run(path)
    if err:
        return [err], []
    diff = output_diff(ref_values, got)
    return (["a saida difere da referencia do lifter:\n    " + "\n    ".join(diff)] if diff else []), got


def ask_qwen(summ, skel, out, loops, ref_values=None, retries=1):
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
        "2. Mantenha todas as chamadas de funcao do esqueleto, com os mesmos argumentos e na mesma ordem.\n"
        "3. O esqueleto foi gerado do CFG: variaveis com nome de registrador (`w21`, `x23`, `p0`...) e objetos de "
        "pilha (`sp_0x10`) devem ganhar nomes e tipos semanticos coerentes com as funcoes chamadas, "
        "sem mudar o comportamento. Os buffers `buf_xNN` aparecem nas formulas: mantenha esses nomes.\n"
        f"4. Responda apenas com o bloco {BT3}c contendo o codigo final compilavel."
    )
    messages = [{"role": "user", "content": prompt}]
    for attempt in range(retries + 1):
        content, code = qwen_chat(messages)
        problems = check_placement(skel, code, loops)
        if problems:
            print("[qwen] estrutura divergente do esqueleto:\n  - " + "\n  - ".join(problems))
        elif ref_values is not None:
            problems, got = host_gate(code, ref_values, out)
            if problems:
                print("[gate host] REPROVADO - nao envie ao aparelho:\n  - " + "\n  - ".join(problems))
            else:
                print(f"[gate host] OK: {len(got)} valores impressos identicos a referencia "
                      f"({', '.join(f'{k}={v}' for k, v in got)})")
        if not problems:
            break
        messages += [{"role": "assistant", "content": content},
                     {"role": "user", "content": "O codigo esta errado:\n- " + "\n- ".join(problems) +
                      f"\nCorrija e responda apenas com o bloco {BT3}c completo."}]
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write(code)
    if problems:
        sys.exit(f"[qwen] codigo ainda reprovado apos {retries + 1} tentativas; salvo em {out} para inspecao")
    print(f"[qwen] estrutura e saida conferidas -> {out}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('json')
    ap.add_argument('--expected', type=int, help='checksum observado no aparelho (conferido contra o C de referencia)')
    ap.add_argument('--qwen', metavar='OUT', help='envia ao Qwen e grava o codigo C em OUT (com gate no host)')
    ap.add_argument('--emit-c', metavar='OUT', help='grava o C de referencia: esqueleto do CFG + loops do lifter, sem LLM')
    args = ap.parse_args()

    with open(args.json, encoding='utf-8') as f:
        data = json.load(f)
    fn = next(x for x in data['functions'] if x['name'] == 'main')

    # Filtra apenas loops folha NEON (ignora loops de controle que contem chamadas 'bl ')
    neon_loops = [lp for lp in fn['loops'] if not any(': bl ' in insn for insn in lp['body'])]

    loops = [lift_single_loop(lp, fn['resolved_data'], i + 1) for i, lp in enumerate(neon_loops)]
    summ = summary_all(loops)
    print(summ)

    print("\n## Validacao Deterministica (por loop: instrucoes NEON x formulas)")
    results = validate_all(loops)
    for idx, what, con, sym, ok in results:
        print(f"- Loop {idx}, {what}: concreto {con} / simbolico {sym}  -> {'OK' if ok else 'DIVERGE'}")
    if not all(r[4] for r in results):
        sys.exit("[lifter] validacao por loop falhou; abortando.")

    try:
        skel = cfg_skeleton.build(fn, data, loops)
    except cfg_skeleton.SkeletonError as e:
        sys.exit(f"[esqueleto] {e}")
    print("\n## Esqueleto do CFG\n" + skel)

    # C de referencia (sem LLM) executado no host: valor do programa inteiro
    base = os.path.splitext(os.path.basename(args.json))[0]
    ref_path = args.emit_c or os.path.join(tempfile.gettempdir(), f"{base}_ref.c")
    with open(ref_path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(fill_loops(skel, loops) + '\n')
    print(f"\n## C de referencia (esqueleto do CFG + loops do lifter, sem LLM) -> {ref_path}")
    ref_values, err = host_run(ref_path)
    if err:
        print(f"- [host] {err}; gate desativado")
        if args.expected:
            sys.exit("[lifter] sem execucao no host nao da para conferir --expected; abortando.")
    else:
        for k, v in ref_values:
            print(f"- [host] {k}: {v}")
        if args.expected is not None:
            hit = any(v == str(args.expected) for _, v in ref_values)
            print(f"- aparelho {args.expected}: {'OK' if hit else 'DIVERGE'}")
            if not hit:
                sys.exit("[lifter] o C de referencia nao reproduz o valor do aparelho; abortando.")

    if args.qwen:
        ask_qwen(summ, skel, args.qwen, loops, ref_values)


if __name__ == '__main__':
    main()
