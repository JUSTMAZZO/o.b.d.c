"""neon_lifter.py - Lifter simbolico NEON (AArch64) para Fases 1, 2 e 3 (libnx / Switch HLE).

Uso:
  python neon_lifter.py ghidra_extraction_switch.json --expected 88196450180 --qwen reconstructed_switch.c
"""
import argparse, glob, json, os, re, subprocess, sys, tempfile, urllib.error, urllib.request

import cfg_skeleton
from collections import Counter, defaultdict

M32, M64 = 0xFFFFFFFF, 0xFFFFFFFFFFFFFFFF
BT3 = chr(96) * 3


class LiftError(Exception):
    pass


def lval(target, idx='i'):
    """Alvo de memoria (array, campo) -> `buf[i]` ou `buf[i].f4` (campo = byte offset na struct)."""
    arr, f = target
    return f"{arr}[{idx}]" + (f".f{f}" if f is not None else "")


# ---------------------------------------------------------------- expressoes simbolicas
class E:
    """No de expressao: 'var' (i+c), 'mem' (arr, c[, campo]), 'sym' (nome escalar), 'const', ou op binario.
    'mem' com campo = byte offset dentro da struct do elemento i+c (array of structs, ld2/st2)."""
    def __init__(self, op, *a):
        self.op, self.a = op, a

    def offsets(self):
        if self.op == 'var':
            return {self.a[0]}
        if self.op == 'mem':
            return {self.a[1]}
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
            arr, c, f = self.a[0], self.a[1], (self.a[2] if len(self.a) > 2 else None)
            idx = x if (relative or c == 0) else f"{x} + {c}"
            return lval((arr, f), idx)
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
            return mem_lookup(self.a[0], x, self.a[2] if len(self.a) > 2 else None) & M32
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
        if ch in '[{':
            depth += 1
        elif ch in ']}':
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
def fn_consts(assembly, upto):
    """Constantes inteiras dos registradores no main ate o endereco `upto` (varredura linear:
    mov/movk/add/sub imediatos; chamadas apagam x0-x18; qualquer outra escrita apaga o registrador)."""
    consts = {}
    for line in assembly:
        addr, mn, ops = parse(line)
        if int(addr, 16) >= upto:
            break
        if mn == 'bl':
            for n in range(19):
                consts.pop(str(n), None)
            continue
        if not ops or not re.fullmatch(r'[wx]\d+', ops[0]) or mn.startswith(('st', 'cmp', 'tst', 'b', 'cb', 'tb')):
            continue
        d = ops[0][1:]
        if mn in ('mov', 'movz') and (ops[1].startswith('#') or ops[1] in ('xzr', 'wzr')):
            consts[d] = 0 if 'zr' in ops[1] else imm(ops[1])
        elif mn == 'movk' and d in consts:
            sh = int(re.search(r'LSL #(\w+)', ops[2]).group(1), 0) if len(ops) > 2 else 0
            consts[d] = (consts[d] & ~(0xFFFF << sh)) | (imm(ops[1]) << sh)
        elif mn in ('add', 'sub') and len(ops) == 3 and ops[2].startswith('#') and ops[1][1:] in consts:
            consts[d] = (consts[ops[1][1:]] + (imm(ops[2]) if mn == 'add' else -imm(ops[2]))) & M64
        else:
            consts.pop(d, None)
    return consts


def resolve_preheader(preheader, resolved_data, body, consts=None, prior=None):
    """prior = [(inicio, fim, estado_de_saida)] dos loops anteriores: as instrucoes deles no
    pre-cabecalho nao sao replicadas (rodam N vezes, nao uma); no lugar entra o estado com que o
    loop termina (ex.: indice = limite)."""
    consts = consts or {}
    prior = sorted(prior or [])
    applied = set()
    vec, sym_vecs, notes = {}, {}, []
    # callee-saved com valor constante definido antes (ex.: limite em bytes no topo do main)
    scal = {r: v for r, v in consts.items() if int(r) >= 19}
    sp_q = None
    # Registradores callee-saved (x19..x28) vivem a funcao inteira: um buffer guardado neles
    # (ex.: retorno do malloc) e reaproveitado por loops posteriores via copia (mov x9, x19).
    # O nome vem do registrador; o LLM nao renomeia, porque as formulas servem de ancora.
    ptr_names = {str(n): f"buf_x{n}" for n in range(19, 29)}

    for line in preheader:
        addr, mn, ops = parse(line)
        a = int(addr, 16)
        for k, (lo, hi, exit_state) in enumerate(prior):
            if k not in applied and a > hi:
                scal.update(exit_state)
                applied.add(k)
                notes.append(f"(fim do loop {lo:#x}-{hi:#x})  =>  " +
                             ', '.join(f"x{r} = {v:#x}" for r, v in exit_state.items()))
        if any(lo <= a <= hi for lo, hi, _ in prior):
            continue
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
                scal[dst] = scal.get(src[1:], 0)
        elif mn == 'add' and ops[0][0] in 'wx' and len(ops) == 3 and ops[1][1:] in ptr_names \
                and ops[2][0] in 'wx':
            # ponteiro = buffer + deslocamento em bytes (ex.: fim do array para um loop por ponteiro)
            dst, base, off = ops[0][1:], ops[1][1:], ops[2][1:]
            ptr_names[dst] = ptr_names[base]
            scal[dst] = (scal.get(base, 0) + scal.get(off, 0)) & M64
            notes.append(f"{line}  =>  x{dst} = `{ptr_names[dst]}` + {scal[dst]:#x}")
        elif mn in ('add', 'sub') and ops[0][0] in 'wx' and len(ops) == 3 and ops[2].startswith('#')                 and ops[1][0] in 'wx' and ops[1] not in ('sp', 'wsp'):
            dst, src = ops[0][1:], ops[1][1:]
            base = scal.get(src, consts.get(src))
            if base is not None:
                scal[dst] = (base + (imm(ops[2]) if mn == 'add' else -imm(ops[2]))) & M64
                ptr_names.pop(dst, None) if int(dst) < 19 else None
                notes.append(f"{line}  =>  {ops[0]} = {scal[dst]:#x} = {scal[dst]}")
        elif mn == 'movk':
            r, v = ops[0][1:], imm(ops[1])
            sh = int(re.search(r'LSL #(\w+)', ops[2]).group(1), 0) if len(ops) > 2 else 0
            scal[r] = (scal.get(r, 0) & ~(0xFFFF << sh)) | (v << sh)
            notes.append(f"{line}  =>  {ops[0]} = {scal[r]:#x} = {scal[r]}")
        elif mn == 'movi':
            name, n, w = vreg(ops[0])
            val = imm(ops[1])
            if len(ops) > 2:   # LSL #s desloca; MSL #s desloca preenchendo com uns (ex.: 0xff, MSL #8 = 0xffff)
                kind, sh = ops[2].split()
                sh = imm(sh)
                val = (val << sh) | ((1 << sh) - 1 if kind.upper() == 'MSL' else 0)
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
                    'b.ne', 'tst', 'csinc', 'ushr', 'subs', 'and', 'eor', 'shl',
                    'ld1', 'ld2', 'ld3', 'ld4', 'st1', 'st2', 'st3', 'st4', 'uaddw', 'uaddw2', 'ccmp', 'mrs'):
            pass
        else:
            raise LiftError(f"instrucao de pre-cabecalho nao suportada: {line}")
    for k, (lo, hi, exit_state) in enumerate(prior):
        if k not in applied:
            scal.update(exit_state)
    return scal, vec, ptr_names, sym_vecs, notes


# ---------------------------------------------------------------- execucao de uma volta
def step(body, vec, scal, sem, ptr_names=None, sym_mode=False, contrib=None, stores=None, mem_bufs=None,
         layouts=None):
    """Executa uma volta do corpo. Memoria concreta: listas planas de uint32 por buffer.
    layouts[arr] = campos por elemento (1 = array contiguo; n = struct de n uint32, via ldN/stN)."""
    branch = None
    alias = {}
    post_inc = {}   # registrador -> incremento do pos-incremento nesta volta
    layouts = {} if layouts is None else layouts

    def access(load, regs, nf, mem_op, post, line, nlanes=4):
        """ldr/str q, ldp/stp q, ld1-4/st1-4, ldr/str w: [base] ou [base, xidx], com pos-incremento
        opcional (#imm ou registrador). nlanes = 1 para registrador escalar de 32 bits."""
        parts = [x.strip() for x in mem_op.strip('[]').split(',')]
        r = parts[0][1:]
        arr = alias.get(r) or ptr_names.get(r)
        if arr is None:
            raise LiftError(f"base de endereco desconhecida: {line}")
        if layouts.setdefault(arr, nf) != nf:
            raise LiftError(f"{arr} acessado como {layouts[arr]} e {nf} campos por elemento: {line}")
        off = scal.get(r, 0)
        if len(parts) > 1:
            if not parts[1][0] in 'wx':
                raise LiftError(f"deslocamento de endereco nao suportado: {line}")
            off = (off + scal.get(parts[1].split('#')[0][1:], 0)) & M64
        if off % (4 * nf):
            raise LiftError(f"acesso desalinhado ao elemento de {arr}: {line}")
        for j, reg in enumerate(regs):
            lanes = []
            for k in range(nlanes):
                if nf == 1:      # contiguo: registrador j, lane k -> elemento 4j+k
                    word = off // 4 + 4 * j + k
                    sym = E('mem', arr, word)
                else:            # intercalado: lane k = struct k, registrador j = campo j
                    word = off // 4 + nf * k + j
                    sym = E('mem', arr, off // (4 * nf) + k, 4 * j)
                target = (arr, sym.a[2] if len(sym.a) > 2 else None)
                if load:
                    lanes.append(sym if sym_mode else mem_bufs[arr][word])
                elif sym_mode and stores is not None:
                    stores.append((target, sym.a[1], vec[reg][k]))
                elif not sym_mode and mem_bufs is not None:
                    mem_bufs[arr][word] = vec[reg][k] & M32
            if load:
                vec[reg] = lanes
        if post:
            inc = imm(post) if post.startswith('#') else scal.get(post.strip()[1:], 0)
            scal[r] = off + inc
            post_inc[r] = inc

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
        elif mn == 'add' and ops[0][0] in 'wx' and ops[1][1:] == ops[0][1:] and \
                f"x{ops[2][1:]}" in vec and f"x{ops[0][1:]}" in vec:
            # acumulador escalar: xA += xV (V carregado da memoria nesta volta)
            d, v = f"x{ops[0][1:]}", f"x{ops[2][1:]}"
            if contrib is not None:
                contrib.append((d, 0, vec[v][0]))
            vec[d] = [sem['uaddw'](vec[d][0], vec[v][0])]
        elif mn == 'add' and ops[0][0] in 'wx':
            dst, s1, s2 = ops[0][1:], ops[1][1:], ops[2]
            if s2.startswith('#'):
                inc = imm(s2)
                scal[dst] = (scal.get(s1, 0) + inc) & M64
                branch = ('byte_offset', dst, inc)
            else:
                r2 = s2[1:]
                if s1 in alias or s1 in ptr_names:   # endereco = buffer + deslocamento em bytes
                    alias[dst] = alias.get(s1) or ptr_names[s1]
                    scal[dst] = (scal.get(s1, 0) + scal.get(r2, 0)) & M64
        elif mn in ('ldr', 'str') and ops[0][0].lower() == 'q':
            access(mn == 'ldr', [qreg(ops[0])], 1, ops[1], ops[2] if len(ops) > 2 else None, line)
        elif mn in ('ldr', 'str') and ops[0][0].lower() == 'w':
            access(mn == 'ldr', [f"x{ops[0][1:]}"], 1, ops[1], ops[2] if len(ops) > 2 else None, line, nlanes=1)
        elif mn in ('ldp', 'stp') and ops[0][0].lower() == 'q':
            access(mn == 'ldp', [qreg(ops[0]), qreg(ops[1])], 1, ops[2], ops[3] if len(ops) > 3 else None, line)
        elif re.fullmatch(r'(ld|st)[1-4]', mn):
            regs = []
            for r in ops[0].strip('{}').split(','):
                name, lanes, width = vreg(r.strip())
                if (lanes, width) != (4, 32):
                    raise LiftError(f"{mn} so com lanes .4S: {line}")
                regs.append(name)
            nf = int(mn[2])
            if nf > 1 and len(regs) != nf:
                raise LiftError(f"{mn} com {len(regs)} registradores: {line}")
            access(mn.startswith('ld'), regs, nf, ops[1], ops[2] if len(ops) > 2 else None, line)
        elif mn == 'subs' and ops[0][0] in 'wx':
            branch = ('counter', ops[0][1:], imm(ops[2]))
            scal[ops[0][1:]] = (scal[ops[0][1:]] - imm(ops[2])) & M32
        elif mn == 'cmp' and ops[0][0] in 'wx':
            a, b = ops[0][1:], ops[1][1:]
            # o indice e o registrador que avanca nesta volta (add #imm ou pos-incremento); o outro e o limite
            stepped = {branch[1]: branch[2]} if branch and branch[0] == 'byte_offset' else {}
            stepped.update(post_inc)
            if a in stepped or b in stepped:
                idx_reg = a if a in stepped else b
                lim_reg = b if idx_reg == a else a
                branch = ('cmp_bytes', lim_reg, idx_reg, stepped[idx_reg])
        elif mn == 'b.ne':
            pass
        else:
            raise LiftError(f"instrucao de loop nao suportada: {line}")
    return branch


def uses_simd(body):
    """O corpo usa registradores SIMD/FP (v, q, d, s, h, b)? Desvios como b.ne nao contam."""
    return any(re.search(r'\b[vqdshb]\d+\b', line.split(': ', 1)[1]) for line in body)


def lift_single_loop(loop, resolved_data, idx, consts=None, prior=None):
    body = loop['body']
    scal, vec, ptr_names, sym_vecs, notes = resolve_preheader(loop['preheader'], resolved_data, body, consts, prior)
    # acumulador escalar (add xA, xA, xV): 1 lane com o valor inicial do pre-cabecalho
    for line in body:
        _, mn, ops = parse(line)
        if mn == 'add' and len(ops) == 3 and ops[0][0] == 'x' and ops[1] == ops[0] and ops[2][0] in 'wx' \
                and not ops[2].startswith('#'):
            vec[ops[0]] = {'lanes': [scal.get(ops[0][1:], 0)], 'width': 64}

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

    contrib, stores, layouts = [], [], {}
    br = step(body, svec, dict(scal), SYM, ptr_names=ptr_names, sym_mode=True, contrib=contrib, stores=stores,
              layouts=layouts)

    if br[0] == 'counter':
        _, creg, dec = br
        stride = dec
        trips = scal[creg] // dec
        total = trips * stride
    elif br[0] == 'cmp_bytes':
        _, lim_reg, idx_reg, byte_inc = br
        if lim_reg not in scal:
            raise LiftError(f"limite x{lim_reg} do loop sem valor conhecido")
        total_bytes = scal[lim_reg] - scal.get(idx_reg, 0)
        trips = total_bytes // byte_inc
        sizes = {4 * n for n in layouts.values()}
        if len(sizes) > 1:
            raise LiftError(f"loop com elementos de tamanhos diferentes {sorted(sizes)} e limite em bytes")
        stride = byte_inc // (sizes.pop() if sizes else 4)   # elementos (ou structs) por volta
        total = trips * stride
    else:
        raise LiftError(f"condicao de saida nao reconhecida: {br}")

    store_list = []
    if stores:
        by_target = {}
        for target, lane_k, e in stores:
            by_target.setdefault(target, []).append((lane_k, e))
        for target, items in by_target.items():
            st_templates = {}
            for lane_k, e in items:
                offs = e.offsets()
                if len(offs) != 1:
                    raise LiftError(f"store em {lval(target)} mistura indices: {e.show()}")
                st_templates.setdefault(e.show('i', relative=True), []).append((lane_k, offs.pop(), e))
            if len(st_templates) != 1:
                raise LiftError(f"stores em {lval(target)} com formulas diferentes: {list(st_templates)}")
            s_str, s_uses = next(iter(st_templates.items()))
            store_list.append((target, s_uses[0][2], s_str))

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

    arrays = {target[0] for target, _, _ in store_list}
    for _, tmpl, _ in store_list:
        arrays |= tmpl.arrays()
    if acc_tmpl is not None:
        arrays |= acc_tmpl.arrays()

    if br[0] == 'counter':
        exit_state = {br[1]: 0}
    else:   # o indice sai igual ao limite
        exit_state = {br[2]: scal[br[1]]}

    return dict(idx=idx, start=loop['start'], end=loop['end'], arrays=arrays, layouts=layouts, exit=exit_state,
                notes=notes, ind=ind, i0=i0, stride=stride, trips=trips, total=total,
                stores=store_list, tmpl=acc_tmpl, tmpl_str=acc_str, uses=uses,
                accs=sorted({u[0] for u in uses}), scal=scal, vec=vec, ptr_names=ptr_names,
                sym_vecs=sym_vecs, body=body)


def validate_all(loops, trips_check=500):
    """Por loop, na mesma memoria compartilhada: execucao concreta das instrucoes NEON x formulas
    simbolicas. Devolve [(loop, o que foi comparado, concreto, simbolico)]. O valor do programa
    inteiro (frames, chamadas, printf) vem da execucao do C de referencia no host."""
    if not loops:                     # so loops escalares (estruturador do CFG): nada a comparar aqui
        return []
    n_check = min(trips_check * max(L['stride'] for L in loops), min(L['total'] for L in loops))
    mem_c = defaultdict(lambda: [0] * n_check * 4)   # uint32 planos; ate 4 campos por elemento
    mem_s = defaultdict(lambda: [0] * n_check * 4)
    nf = {}
    for L in loops:
        nf.update(L['layouts'])
    word = lambda a, idx, f=None: idx * nf.get(a, 1) + (f // 4 if f is not None else 0)
    results = []
    for L in loops:
        vec = {n: list(v['lanes']) for n, v in L['vec'].items()}
        scal = dict(L['scal'])
        for _ in range(n_check // L['stride']):
            step(L['body'], vec, scal, CON, ptr_names=L['ptr_names'], sym_mode=False, mem_bufs=mem_c)

        load = lambda a, idx, f=None: mem_s[a][word(a, idx, f)]
        sym_acc = 0
        for i in range(n_check):
            x = i + L['i0']
            # todas as formulas leem os valores carregados no inicio da volta (ldr q antes de str q)
            new_vals = [(target, tmpl.eval(x, load)) for target, tmpl, _ in L['stores']]
            for (arr, f), v in new_vals:
                mem_s[arr][word(arr, i, f)] = v
            if L['tmpl'] is not None:
                sym_acc = (sym_acc + L['tmpl'].eval(x, load)) & M64
        for arr in sorted({t[0] for t, _, _ in L['stores']}):
            # soma so para exibir; o veredito compara o buffer inteiro (todos os campos)
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
    for target, _, s_str in order:
        for w_target, w_str in written:
            s_str = s_str.replace(w_str, '@@')
            if lval(w_target) in s_str:
                raise LiftError(f"{lval(target)} usa o valor antigo de {lval(w_target)}; precisa de temporario")
            s_str = s_str.replace('@@', lval(w_target))
        out.append((target, s_str))
        written.append((target, next(s for t, _, s in stores if t == target)))
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
        for target, s_str in seq:
            lines.append(f"- Atualizacao de array: `{lval(target)} = {s_str}` (para i = 0 .. {L['total'] - 1})")
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
        body += [f"  {lval(target)} = {expr};" for target, expr in sequential_stores(L['stores'])]
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
        anchors = [f"{lval(target)}={t}" for target, t in sequential_stores(L['stores'])]
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
            moved = sorted(((want_calls - got_calls) + (got_calls - want_calls)).keys())   # nome ou contagem
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
    comp = subprocess.run([cc, '-O2', '-w', '-I', HERE, c_path, '-o', exe, '-lpthread'],
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


def ask_qwen(summ, skel, out, loops, ref_values=None, retries=1, ref_code=None):
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
        "sem mudar o comportamento. Os buffers `buf_xNN`, os tipos `buf_xNN_t` e os campos `.fN` aparecem nas formulas: mantenha esses nomes.\n"
        "4. Fora dos placeholders, copie o esqueleto linha por linha: nao remova, nao mova e nao reescreva "
        "nenhuma linha (inclusive `for (;;)`, `break` e atribuicoes iniciais como `w2 = 1;`).\n"
        f"5. Responda apenas com o bloco {BT3}c contendo o codigo final compilavel."
    )
    messages = [{"role": "user", "content": prompt}]
    history, code = [], None
    for attempt in range(retries + 1):
        try:
            content, code = qwen_chat(messages)
        except (urllib.error.URLError, OSError) as e:      # Ollama fora do ar ou erro HTTP
            problems = [f"Qwen indisponivel: {e}"]
            print(f"[qwen] {problems[0]}")
            history.append(problems)
            break
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
        history.append(problems)
        if not problems:
            break
        messages += [{"role": "assistant", "content": content},
                     {"role": "user", "content": "O codigo esta errado:\n- " + "\n- ".join(problems) +
                      f"\nCorrija e responda apenas com o bloco {BT3}c completo."}]

    report = {"output": out, "attempts": len(history), "problems": history}
    if not problems:
        with open(out, 'w', encoding='utf-8', newline='\n') as f:
            f.write(code)
        report["source"] = "qwen"
        print(f"[qwen] estrutura e saida conferidas -> {out}")
    else:
        if ref_code is None or ref_values is None:
            sys.exit(f"[qwen] codigo reprovado e sem C de referencia validado no host para o fallback")
        if code is not None:                               # guarda a tentativa reprovada para inspecao
            rejected = os.path.splitext(out)[0] + '.qwen_rejected.c'
            with open(rejected, 'w', encoding='utf-8', newline='\n') as f:
                f.write(code)
            report["rejected"] = rejected
        gate_problems, got = host_gate(ref_code, ref_values, out)
        if gate_problems:                                  # nao deve acontecer: e o proprio oraculo
            sys.exit("[fallback] o C de referencia nao passou no gate:\n  - " + "\n  - ".join(gate_problems))
        with open(out, 'w', encoding='utf-8', newline='\n') as f:
            f.write(ref_code)
        report["source"] = "fallback"
        report["fallback_reason"] = history[-1]
        print(f"[Fallback] Qwen falhou apos {len(history)} tentativa(s); usando C de referencia deterministico "
              f"-> {out} (gate no host: {', '.join(f'{k}={v}' for k, v in got)})")
    report["gate"] = dict(ref_values or [])
    rpath = os.path.splitext(out)[0] + '.pipeline.json'
    with open(rpath, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(report, f, ensure_ascii=False, indent=2)
        f.write('\n')
    print(f"[pipeline] relatorio -> {rpath} (fonte do C final: {report['source']})")
    return report["source"]


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

    loops = []
    for lp in neon_loops:
        prior = [(int(L['start'], 16), int(L['end'], 16), L['exit']) for L in loops]
        try:
            loops.append(lift_single_loop(lp, fn['resolved_data'], len(loops) + 1,
                                          fn_consts(fn['assembly'], int(lp['start'], 16)), prior))
        except LiftError as e:
            # loop escalar (sem registrador SIMD) que o lifter simbolico nao eleva (ex.: if/else no corpo):
            # fica com o estruturador do CFG, que o emite como C comum; loop NEON continua obrigatorio
            if uses_simd(lp['body']):
                raise
            print(f"[lifter] loop escalar {lp['start']}-{lp['end']} fica com o estruturador do CFG ({e})")
    summ = summary_all(loops)
    print(summ)

    print("\n## Validacao Deterministica (por loop: instrucoes NEON x formulas)")
    results = validate_all(loops)
    for idx, what, con, sym, ok in results:
        print(f"- Loop {idx}, {what}: concreto {con} / simbolico {sym}  -> {'OK' if ok else 'DIVERGE'}")
    if not all(r[4] for r in results):
        sys.exit("[lifter] validacao por loop falhou; abortando.")

    try:
        # funcoes do proprio programa chamadas pelo main e extraidas no JSON (ex.: auxiliares do homebrew)
        targets = {int(a, 16) for a in fn.get('calls', {})}
        helpers = [f for f in data['functions'] if f is not fn and int(f['entry_point'], 16) in targets]
        skel = cfg_skeleton.build(fn, data, loops, extra_fns=helpers)
    except cfg_skeleton.SkeletonError as e:
        sys.exit(f"[esqueleto] {e}")
    print("\n## Esqueleto do CFG\n" + skel)

    # C de referencia (sem LLM) executado no host: valor do programa inteiro
    base = os.path.splitext(os.path.basename(args.json))[0]
    ref_path = args.emit_c or os.path.join(tempfile.gettempdir(), f"{base}_ref.c")
    ref_code = fill_loops(skel, loops) + '\n'
    with open(ref_path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(ref_code)
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
        ask_qwen(summ, skel, args.qwen, loops, None if err else ref_values, ref_code=ref_code)


if __name__ == '__main__':
    main()
