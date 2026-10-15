"""cfg_skeleton.py - gera o esqueleto C do `main` a partir do CFG AArch64 extraido pelo Ghidra.

Pipeline:
  1. Blocos basicos, dominadores e loops naturais. Os loops NEON que o lifter ja elevou viram
     uma pseudo-instrucao `/* <LOOP_NEON_k> */` (usa os buffers, define os acumuladores = checksum).
  2. Fatia (slice) a partir do que tem efeito: chamadas, desvios, retorno, `dup` de escalares
     simbolicos (boost) e os loops NEON. Preparacao que so alimenta os loops NEON cai fora.
  3. Definicoes que alcancam o mesmo uso formam uma variavel ("web"); expressoes de uso unico,
     sem efeito colateral no meio, sao embutidas (argumentos de chamada, condicoes).
  4. Estruturacao: teste de saida no cabecalho -> `while`, teste no latch -> `if (...) break;`,
     desvio para o bloco de retorno -> `if (...) { return ...; }`.

Uso: build(fn, data, loops) -> str (esqueleto C com os placeholders dos loops NEON).
"""
import os
import re
import struct

M32, M64 = 0xFFFFFFFF, 0xFFFFFFFFFFFFFFFF
CALLEE_SAVED = {f'x{n}' for n in range(19, 31)} | {f'v{n}' for n in range(8, 16)}
CALLER_SAVED = ({f'x{n}' for n in range(19)} | {f'v{n}' for n in list(range(8)) + list(range(16, 32))}
                | {'nzcv'})
PTR_RETURNING = {'malloc', 'calloc', 'realloc', 'aligned_alloc'}
PRINTF_LIKE = {'printf': 0, 'sprintf': 1, 'snprintf': 2, 'fprintf': 1}
CTYPE = {'u32': 'uint32_t', 'u64': 'uint64_t', 'f64': 'double', 'ptr': 'uint32_t *', 'void*': 'void *',
         'bptr': 'uint8_t *'}
CMP_NEG = {'==': '!=', '!=': '==', '<': '>=', '>=': '<', '>': '<=', '<=': '>'}
CC_OP = {'eq': '==', 'ne': '!=', 'hi': '>', 'hs': '>=', 'cs': '>=', 'lo': '<', 'cc': '<', 'ls': '<=',
         'gt': '>', 'ge': '>=', 'lt': '<', 'le': '<='}
COND_BRANCH = ('bcc', 'cbz', 'tbz')


class SkeletonError(Exception):
    pass


# ---------------------------------------------------------------- operandos
def parse(line):
    addr, _, text = line.partition(': ')
    mnem, _, rest = text.strip().partition(' ')
    ops, depth, cur = [], 0, ''
    for ch in rest:
        depth += (ch in '[{') - (ch in ']}')
        if ch == ',' and depth == 0:
            ops.append(cur.strip())
            cur = ''
        else:
            cur += ch
    if cur.strip():
        ops.append(cur.strip())
    return int(addr, 16), mnem.lower(), ops


def reg(op):
    """(nome canonico, largura) - x/w -> 'xN', q/d/s/v -> 'vN'; None se nao for registrador."""
    op = op.strip().lower()
    if op in ('xzr', 'wzr'):
        return 'zr', 64 if op[0] == 'x' else 32
    if op in ('sp', 'wsp'):
        return 'sp', 64
    m = re.fullmatch(r'([xw])(\d+)', op)
    if m:
        return f'x{m.group(2)}', 64 if m.group(1) == 'x' else 32
    m = re.fullmatch(r'([qdshb])(\d+)', op)
    if m:
        return f'v{m.group(2)}', {'q': 128, 'd': 64, 's': 32, 'h': 16, 'b': 8}[m.group(1)]
    m = re.fullmatch(r'v(\d+)\.\w+', op)
    if m:
        return f'v{m.group(1)}', 128
    return None, None


def is_imm(op):
    return bool(re.fullmatch(r'#?-?(0x[0-9a-f]+|\d+)', op.strip().lower()))


def imm(op):
    return int(op.strip().lstrip('#'), 0)


def mem(op):
    """'[x19, x8#0x0]' -> ('x19', 'x8'); '[sp, #0x30]!' -> ('sp', '#0x30'); '[x10]' -> ('x10', '#0')."""
    parts = [p.strip() for p in op.strip().rstrip('!').strip('[]').split(',')]
    off = parts[1] if len(parts) > 1 else '#0'
    if not off.startswith('#') and '#' in off:
        off = off.split('#')[0]
    return parts[0], off


def mask(w):
    return M32 if w == 32 else M64


# ---------------------------------------------------------------- expressoes C
class X:
    """No de expressao C. t = tipo ('u32', 'u64', 'f64', 'ptr', 'void*', 'bool')."""
    def __init__(self, k, *a, t='u64'):
        self.k, self.a, self.t = k, a, t

    def const(self):
        return self.a[0] if self.k == 'const' else None


def c_string(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n').replace('\t', '\\t') + '"'


def neg(c):
    if c.k == 'cmp':
        return X('cmp', CMP_NEG[c.a[0]], c.a[1], c.a[2], t='bool')
    return X('not', c, t='bool')


class Render:
    def __init__(self, strings):
        self.strings = strings
        self.layouts = {}      # buffer -> campos uint32 por elemento (1 = array; n = struct)
        self.ptr_alias = {}    # variavel ponteiro -> buffer tipado com o mesmo valor (copia que domina as leituras)
        self.addr_of = set()   # objetos de pilha tipados (passados como &obj)

    def __call__(self, e, hexa=False):
        k, a = e.k, e.a
        if k == 'const':
            v = a[0]
            if v >= 0x1000 and hex(v) in self.strings:
                return c_string(self.strings[hex(v)])
            return hex(v) if (hexa or v > 0xFFFFF) and v > 9 else str(v)
        if k == 'fconst':
            return repr(float(a[0]))
        if k == 'var':
            return a[0]
        if k == 'stack':
            return f"&{a[0]}" if a[0] in self.addr_of else a[0]
        if k == 'ptradd':
            return f"((uint8_t *){self(a[0])} + {a[1]})"
        if k == 'chk':
            return 'checksum'
        if k == 'opaque':
            return f"/* ? {a[0]} */ 0"
        if k == 'call':
            return f"{a[0]}({', '.join(self(x) for x in a[1])})"
        if k == 'bin':
            op, l, r = a
            return f"({self(l)} {op} {self(r, hexa=op in ('&', '|', '^'))})"
        if k == 'cmp':
            op, l, r = a
            # teste de bit 0 do retorno booleano de uma chamada: !f() / f()
            if l.k == 'bin' and l.a[0] == '&' and l.a[1].k == 'call' and l.a[2].const() == 1 and r.const() == 0:
                return f"!{self(l.a[1])}" if op == '==' else self(l.a[1])
            return f"({self(l)} {op} {self(r)})"
        if k == 'not':
            return f"!{self(a[0])}"
        if k == 'cast':
            return f"({a[0]}){self(a[1])}"
        if k == 'ternary':
            return f"({self(a[0])} ? {self(a[1])} : {self(a[2])})"
        if k == 'cvt':
            signed, x = a
            return f"(double)({'int64_t' if signed else 'uint64_t'})({self(x)})"
        if k == 'f2i':
            return f"({CTYPE[e.t]})({self(a[0])})"
        if k == 'bits2f':
            return f"/* bits->double */ {self(a[0])}"
        if k == 'load':
            base, off, width = a
            ctype = 'uint32_t' if width == 32 else 'uint64_t'
            root, off0 = (base.a[0], base.a[1]) if base.k == 'ptradd' else (base, 0)
            if root.k == 'var' and root.a[0] in self.ptr_alias:
                root = X('var', self.ptr_alias[root.a[0]], t='ptr')
            if off.const() is not None and root.k == 'var' and root.t == 'ptr' and width == 32:
                total = off0 + off.const()
                nf = self.layouts.get(root.a[0], 1)
                idx, rem = divmod(total, 4 * nf)
                if rem % 4 == 0:   # elemento (ou campo de struct) do buffer tipado
                    return f"{root.a[0]}[{idx}]" + (f".f{rem}" if nf > 1 else "")
            return f"*({ctype} *)((uint8_t *){self(base)} + {self(off)})"
        raise SkeletonError(f"expressao sem renderizacao: {k}")


# ---------------------------------------------------------------- instrucoes
class Insn:
    def __init__(self, idx, line):
        self.idx, self.line = idx, line
        self.addr, self.mn, self.ops = parse(line)
        self.defs, self.uses, self.kind = [], [], None
        self.width, self.block, self.target = 64, None, None

    def __repr__(self):
        return self.line


def is_frame(I):
    mn, ops = I.mn, I.ops
    if mn in ('sub', 'add') and len(ops) >= 2 and ops[1] == 'sp' and ops[0] in ('sp', 'x29'):
        return True
    if mn in ('stp', 'ldp', 'str', 'ldr'):
        m = next((o for o in ops if o.startswith('[')), None)
        if m and mem(m)[0] == 'sp':
            regs = [reg(o)[0] for o in ops if not o.startswith('[') and not o.startswith('#')]
            return all(r in CALLEE_SAVED for r in regs)
    return False


def classify(I, calls):
    mn, ops = I.mn, I.ops
    R = lambda o: reg(o)[0]
    W = lambda o: reg(o)[1]
    if mn == 'nop':
        I.kind = 'nop'
    elif is_frame(I):
        I.kind = 'frame'
    elif mn in ('mov', 'movz'):
        d = R(ops[0])
        if d.startswith('v'):
            I.kind, I.defs, I.uses = 'vec', [d], [R(ops[1])]
        else:
            I.width, I.defs = W(ops[0]), [d]
            s = R(ops[1])
            if s == 'zr':
                I.kind, I.val = 'const', 0
            elif s:
                I.kind, I.uses = 'copy', [s]
            else:
                I.kind, I.val = 'const', imm(ops[1]) & mask(I.width)
    elif mn == 'movk':
        I.kind, I.width, I.defs, I.uses = 'movk', W(ops[0]), [R(ops[0])], [R(ops[0])]
        I.val = imm(ops[1])
        I.sh = int(re.search(r'LSL #(\w+)', ops[2]).group(1), 0) if len(ops) > 2 else 0
    elif mn in ('adr', 'adrp'):
        I.kind, I.defs, I.val = 'const', [R(ops[0])], int(ops[1].lstrip('#'), 16)
    elif mn in ('add', 'sub', 'mul', 'and', 'orr', 'eor', 'lsl', 'lsr', 'udiv', 'sdiv') and ops[0].startswith('v'):
        I.kind, I.defs, I.uses = 'vec', [R(ops[0])], [R(o) for o in ops[1:] if R(o)]
    elif mn in ('add', 'sub') and ops[1] in ('sp', 'wsp'):
        I.kind, I.defs, I.val = 'stack', [R(ops[0])], imm(ops[2]) * (1 if mn == 'add' else -1)
    elif mn in ('add', 'sub', 'mul', 'and', 'orr', 'eor', 'lsl', 'lsr', 'asr', 'udiv', 'sdiv', 'subs', 'adds', 'ands'):
        I.kind, I.width = 'bin', W(ops[0])
        I.op = {'add': '+', 'adds': '+', 'sub': '-', 'subs': '-', 'mul': '*', 'and': '&', 'ands': '&',
                'orr': '|', 'eor': '^', 'lsl': '<<', 'lsr': '>>', 'asr': '>>', 'udiv': '/', 'sdiv': '/'}[mn]
        I.defs = [R(ops[0])] + (['nzcv'] if mn in ('subs', 'adds', 'ands') else [])
        I.srcs = ops[1:3]
        I.shift = re.search(r'LSL #(\w+)', ops[3]).group(1) if len(ops) > 3 else None
        I.uses = [R(o) for o in I.srcs if R(o) and R(o) != 'zr']
    elif mn in ('tst', 'cmp', 'cmn'):
        I.kind, I.defs, I.srcs = 'flags', ['nzcv'], ops[:2]
        I.flag = 'tst' if mn == 'tst' else 'cmp'
        I.uses = [R(o) for o in ops[:2] if R(o) and R(o) != 'zr']
    elif mn in ('csel', 'csinc', 'csinv', 'csneg'):
        I.kind, I.width, I.defs = 'csel', W(ops[0]), [R(ops[0])]
        I.srcs, I.cc = ops[1:3], ops[3].lower()
        I.uses = [R(o) for o in ops[1:3] if R(o) != 'zr'] + ['nzcv']
    elif mn in ('cset', 'csetm'):
        I.kind, I.width, I.defs, I.uses, I.cc = 'cset', W(ops[0]), [R(ops[0])], ['nzcv'], ops[1].lower()
    elif mn == 'ldr' and R(ops[0]).startswith('x'):
        base, off = mem(ops[1])
        if len(ops) > 2 or ops[1].endswith('!'):
            raise SkeletonError(f"ldr com writeback fora de loop NEON: {I.line}")
        I.kind, I.width, I.defs, I.base, I.off = 'load', W(ops[0]), [R(ops[0])], base, off
        I.uses = [r for r in (R(base), R(off)) if r and r not in ('sp', 'zr')]
    elif mn in ('ldr', 'ldur', 'ldp') and R(ops[0]).startswith('v'):
        base = mem(next(o for o in ops if o.startswith('[')))[0]
        I.kind, I.defs = 'vec', [R(o) for o in ops if not o.startswith(('[', '#')) and R(o)]
        I.uses = [R(base)] if R(base) not in ('sp', None) else []
    elif mn in ('str', 'strb', 'strh', 'stur', 'stp'):
        base, off = mem(next(o for o in ops if o.startswith('[')))
        if base == 'sp':
            I.kind = 'nop'          # spill de vetor/escalar para a pilha (so alimenta loops NEON)
        elif R(ops[0]).startswith('x') and mn != 'stp':
            I.kind, I.width, I.base, I.off = 'store', W(ops[0]), base, off
            I.uses = [r for r in (R(ops[0]), R(base), R(off)) if r and r not in ('sp', 'zr')]
        else:
            raise SkeletonError(f"store nao suportado fora de loop NEON: {I.line}")
    elif mn == 'movi':
        I.kind, I.defs = 'vec', [R(ops[0])]
    elif mn == 'dup':
        I.kind, I.defs, I.uses = 'dup', [R(ops[0])], [R(ops[1])]
    elif mn in ('addp', 'addv', 'uaddlv'):
        I.kind, I.defs, I.uses = 'vec', [R(ops[0])], [R(ops[1])]
    elif mn in ('scvtf', 'ucvtf') and R(ops[1]).startswith('x'):
        I.kind, I.defs, I.uses, I.signed = 'cvt', [R(ops[0])], [R(ops[1])], mn == 'scvtf'
    elif mn in ('fcvtzs', 'fcvtzu'):
        I.kind, I.width, I.defs, I.uses = 'f2i', W(ops[0]), [R(ops[0])], [R(ops[1])]
    elif mn == 'fmov':
        d, s = R(ops[0]), R(ops[1])
        I.defs = [d]
        if s is None:
            I.kind, I.val = 'fconst', float(ops[1].lstrip('#'))
        elif d.startswith('v') and s.startswith('x'):
            I.kind, I.uses = 'bits2f', [s]
        elif d.startswith('x') and s.startswith('v'):
            I.kind, I.width, I.uses = 'f2bits', W(ops[0]), [s]
        else:
            I.kind, I.uses = 'fcopy', [s]
    elif mn in ('fdiv', 'fmul', 'fadd', 'fsub'):
        I.kind, I.defs, I.uses = 'fbin', [R(ops[0])], [R(ops[1]), R(ops[2])]
        I.op = {'fdiv': '/', 'fmul': '*', 'fadd': '+', 'fsub': '-'}[mn]
    elif mn == 'bl':
        tgt = hex(int(ops[0], 16))
        I.kind, I.name = 'call', calls.get(tgt, f"sub_{tgt[2:]}")
        I.defs = ['x0'] + sorted(CALLER_SAVED - {'x0'})
    elif mn == 'ret':
        I.kind, I.uses = 'ret', ['x0']
    elif mn == 'b':
        I.kind, I.target = 'br', int(ops[0], 16)
    elif mn.startswith('b.'):
        I.kind, I.cc, I.target, I.uses = 'bcc', mn[2:], int(ops[0], 16), ['nzcv']
    elif mn in ('cbz', 'cbnz'):
        I.kind, I.width, I.uses, I.target = 'cbz', W(ops[0]), [R(ops[0])], int(ops[1], 16)
    elif mn in ('tbz', 'tbnz'):
        I.kind, I.width, I.uses = 'tbz', W(ops[0]), [R(ops[0])]
        I.bit, I.target = imm(ops[1]), int(ops[2], 16)
    else:
        raise SkeletonError(f"instrucao nao suportada pelo gerador de esqueleto: {I.line}")
    I.uses = [u for u in I.uses if u and u not in ('zr', 'sp')]


def loop_clobbers(body):
    """Registradores escritos dentro de um loop NEON (destinos, listas {..} de ldN e bases com pos-incremento)."""
    out = set()
    for line in body:
        _, mn, ops = parse(line)
        if mn.startswith('b.') or mn in ('b', 'cbz', 'cbnz', 'tbz', 'tbnz'):
            continue
        if mn in ('cmp', 'cmn', 'tst', 'subs', 'adds'):
            out.add('nzcv')
            if mn in ('cmp', 'cmn', 'tst'):
                continue
        memop = next((o for o in ops if o.startswith('[')), None)
        if memop is not None and (memop.endswith('!') or ops[-1] is not memop):
            out.add(reg(mem(memop)[0])[0])          # pos/pre-incremento escreve a base
        if mn in ('str', 'stp', 'strb', 'strh') or mn[:2] == 'st' and mn[2:].isdigit():
            continue
        if ops[0].startswith('{'):
            out |= {reg(r.strip())[0] for r in ops[0].strip('{}').split(',')}
        else:
            out.add(reg(ops[0])[0])
            if mn == 'ldp':
                out.add(reg(ops[1])[0])
    return out - {None, 'zr', 'sp'}


# ---------------------------------------------------------------- CFG
class Block:
    def __init__(self, idx, insns):
        self.idx, self.insns = idx, insns
        self.start = insns[0].addr
        self.succ, self.pred = [], []
        self.neon = None

    @property
    def term(self):
        t = self.insns[-1]
        return t if t.kind in ('br', 'bcc', 'cbz', 'tbz', 'ret') else None


def build_blocks(insns):
    addrs = {I.addr: n for n, I in enumerate(insns)}
    leaders = {0}
    for n, I in enumerate(insns):
        if I.kind in ('br', 'bcc', 'cbz', 'tbz', 'ret'):
            if n + 1 < len(insns):
                leaders.add(n + 1)
            if I.target is not None:
                if I.target not in addrs:
                    raise SkeletonError(f"desvio para fora da funcao: {I.line}")
                leaders.add(addrs[I.target])
    cuts = sorted(leaders) + [len(insns)]
    blocks = [Block(k, insns[a:b]) for k, (a, b) in enumerate(zip(cuts, cuts[1:]))]
    by_addr = {b.start: b for b in blocks}
    for b in blocks:
        for I in b.insns:
            I.block = b
        t = b.term
        nxt = blocks[b.idx + 1] if b.idx + 1 < len(blocks) else None
        if t is None:
            b.succ = [nxt] if nxt else []
        elif t.kind == 'ret':
            b.succ = []
        elif t.kind == 'br':
            b.succ = [by_addr[t.target]]
        else:
            b.succ = [by_addr[t.target]] + ([nxt] if nxt else [])
    return blocks, by_addr


def collapse_neon(blocks, by_addr, loops, calls):
    """Troca o bloco de cada loop NEON por uma pseudo-instrucao (sem aresta de retorno)."""
    for L in loops:
        b = by_addr.get(int(L['start'], 16))
        if b is None or b.insns[-1].addr != int(L['end'], 16):
            raise SkeletonError(f"loop NEON {L['idx']} nao ocupa exatamente um bloco basico")
        p = Insn(b.insns[0].idx, b.insns[0].line)
        p.kind, p.loop, p.block = 'pseudo', L, b
        # buffers que o loop realmente le/escreve (o lifter registra em L['arrays'])
        p.uses = sorted(f'x{k}' for k, v in L['ptr_names'].items() if int(k) >= 19 and v in L['arrays'])
        accs = {a for a in L['accs']}
        p.defs = sorted(accs | (loop_clobbers(L['body']) - accs))
        p.accs = accs
        b.insns, b.neon = [p], L
        nxt = blocks[b.idx + 1]
        b.succ = [nxt]
    for b in blocks:
        b.pred = []
    for b in blocks:
        for s in b.succ:
            s.pred.append(b)


def dominators(blocks):
    allb = set(blocks)
    dom = {b: set(allb) for b in blocks}
    dom[blocks[0]] = {blocks[0]}
    changed = True
    while changed:
        changed = False
        for b in blocks[1:]:
            new = {b} | (set.intersection(*(dom[p] for p in b.pred)) if b.pred else set())
            if new != dom[b]:
                dom[b], changed = new, True
    return dom


def natural_loops(blocks, dom):
    loops = {}
    for b in blocks:
        for s in b.succ:
            if s in dom[b]:
                body, stack = {s, b}, [b]
                while stack:
                    n = stack.pop()
                    for p in n.pred:
                        if p not in body:
                            body.add(p)
                            stack.append(p)
                loops.setdefault(s, set()).update(body)
    return loops


# ---------------------------------------------------------------- definicoes alcancantes
def reaching(blocks, insns):
    allregs = {r for I in insns for r in I.defs + I.uses}
    gen = {}
    for b in blocks:
        g = {}
        for I in b.insns:
            for d in I.defs:
                g[d] = (I.idx, d)
        gen[b] = g
    inn = {b: {} for b in blocks}
    out = {b: {} for b in blocks}
    inn[blocks[0]] = {r: {(-1, r)} for r in allregs}
    changed = True
    while changed:
        changed = False
        for b in blocks:
            if b is not blocks[0]:
                env = {}
                for p in b.pred:
                    for r, ds in out[p].items():
                        env.setdefault(r, set()).update(ds)
                inn[b] = env
            new = {r: set(ds) for r, ds in inn[b].items()}
            for r, d in gen[b].items():
                new[r] = {d}
            if new != out[b]:
                out[b], changed = new, True
    for b in blocks:
        env = {r: set(ds) for r, ds in inn[b].items()}
        for I in b.insns:
            I.reach = {u: set(env.get(u, {(-1, u)})) for u in I.uses}
            for d in I.defs:
                env[d] = {(I.idx, d)}


# ---------------------------------------------------------------- aridade das chamadas
def printf_args(fmt):
    conv = re.findall(r'%[-+ #0]*\d*(?:\.\d+)?(hh|h|ll|l|j|z|t|L)?([diouxXcspfFeEgGaA%])', fmt)
    return [(ln, c) for ln, c in conv if c != '%']


def infer_args(blocks, strings):
    for b in blocks:
        written, consts = {}, {}
        for I in b.insns:
            if I.kind == 'call':
                fixed = PRINTF_LIKE.get(I.name)
                if fixed is not None and f'x{fixed}' in consts and hex(consts[f'x{fixed}']) in strings:
                    conv = printf_args(strings[hex(consts[f'x{fixed}'])])
                    ints = [f'x{n}' for n in range(fixed + 1)]
                    I.args, xi, vi = list(ints), fixed + 1, 0
                    I.convs = [None] * len(ints)
                    for ln, c in conv:
                        if c in 'fFeEgGaA':
                            I.args.append(f'v{vi}')
                            vi += 1
                        else:
                            I.args.append(f'x{xi}')
                            xi += 1
                        I.convs.append((ln, c))
                else:
                    I.args, I.convs = [], []
                    for n in range(8):
                        if written.get(f'x{n}') != 'int':
                            break
                        I.args.append(f'x{n}')
                    for n in range(8):
                        if written.get(f'v{n}') != 'fp':
                            break
                        I.args.append(f'v{n}')
                    I.convs = [None] * len(I.args)
                I.uses = list(I.args)
                written, consts = {}, {}
                continue
            for d in I.defs:
                written[d] = 'fp' if I.kind in ('fbin', 'cvt', 'bits2f', 'fcopy', 'fconst') else \
                             ('int' if d.startswith('x') else 'vec')
            if I.kind == 'const':
                consts[I.defs[0]] = I.val
            elif I.kind == 'bin' and I.op == '+' and I.uses and I.uses[0] in consts and is_imm(I.srcs[1]):
                consts[I.defs[0]] = consts[I.uses[0]] + imm(I.srcs[1])
            else:
                for d in I.defs:
                    consts.pop(d, None)


# ---------------------------------------------------------------- gerador
class Web:
    def __init__(self, reg_):
        self.reg, self.defs, self.uses = reg_, set(), []
        self.var, self.name, self.t = False, None, None


class Skeleton:
    def __init__(self, fn, data, loops, shim_header=None):
        self.fn, self.loops = fn, loops
        self.calls = fn.get('calls')
        self.strings = data.get('strings')
        if self.calls is None or self.strings is None:
            raise SkeletonError("JSON sem 'calls'/'strings': regere com a versao atual do ExtractMain.java")
        self.R = Render(self.strings)
        self.shim_header = shim_header
        self.warnings = []

        self.insns = [Insn(n, line) for n, line in enumerate(fn['assembly'])]
        neon_spans = [(int(L['start'], 16), int(L['end'], 16)) for L in loops]
        for I in self.insns:
            inside = any(a <= I.addr <= b for a, b in neon_spans)
            if inside:
                I.kind = 'neon'
                if I.mn.startswith('b.'):
                    I.kind, I.target = 'bcc', int(I.ops[-1], 16)   # so para cortar os blocos
                    I.cc = I.mn[2:]
            else:
                classify(I, self.calls)
        self.blocks, by_addr = build_blocks(self.insns)
        collapse_neon(self.blocks, by_addr, loops, self.calls)
        self.insns = [I for b in self.blocks for I in b.insns]
        self.by_idx = {I.idx: I for I in self.insns}
        self.dom = dominators(self.blocks)
        self.nat = {h: body for h, body in natural_loops(self.blocks, self.dom).items()}
        infer_args(self.blocks, self.strings)
        reaching(self.blocks, self.insns)
        self.sym_names = {}
        for L in loops:
            self.sym_names.update(L.get('sym_vecs', {}))
        self.slice()
        self.make_webs()
        self.decide()
        self.memo = {}

    # -- fatia
    def slice(self):
        roots = [I for I in self.insns if I.kind in ('call', 'ret', 'bcc', 'cbz', 'tbz', 'dup', 'store', 'pseudo')]
        self.live = set()
        work = list(roots)
        while work:
            I = work.pop()
            if I.idx in self.live:
                continue
            self.live.add(I.idx)
            for u in I.uses:
                for d in I.reach[u]:
                    if d[0] >= 0 and d[0] not in self.live:
                        work.append(self.by_idx[d[0]])

    # -- variaveis
    def make_webs(self):
        parent = {}

        def find(x):
            while parent.setdefault(x, x) != x:
                parent[x] = parent[parent[x]]
                x = parent[x]
            return x

        uses = []
        for I in self.insns:
            if I.idx not in self.live:
                continue
            for u in I.uses:
                ds = sorted(I.reach[u])
                for d in ds[1:]:
                    parent[find(d)] = find(ds[0])
                uses.append((I, u, ds))
        self.webs, self.use_web, self.def_web = {}, {}, {}
        for I, u, ds in uses:
            root = find(ds[0])
            w = self.webs.setdefault(root, Web(u))
            w.defs.update(ds)
            w.uses.append((I, u))
            self.use_web[(I.idx, u)] = w
        for w in self.webs.values():
            for d in w.defs:
                self.def_web[d] = w

    def support(self, d, seen=None):
        """Webs lidas (transitivamente, por definicoes unicas) pela expressao da definicao d."""
        seen = set() if seen is None else seen
        if d[0] < 0:
            return seen
        I = self.by_idx[d[0]]
        for u in I.uses:
            w = self.use_web[(I.idx, u)]
            if id(w) in seen:
                continue
            seen.add(id(w))
            if len(w.defs) == 1:
                self.support(next(iter(w.defs)), seen)
        return seen

    def const_like(self, d, depth=0):
        if d[0] < 0 or depth > 20:
            return False
        I = self.by_idx[d[0]]
        if I.kind == 'pseudo':
            return d[1] in I.accs
        if I.kind in ('call', 'load', 'dup'):
            return False
        for u in I.uses:
            w = self.use_web[(I.idx, u)]
            if len(w.defs) != 1 or not self.const_like(next(iter(w.defs)), depth + 1):
                return False
        return True

    def decide(self):
        for w in self.webs.values():
            if len(w.defs) > 1 or any(d[0] < 0 for d in w.defs):
                w.var = True
                continue
            if any(U.kind == 'pseudo' for U, _ in w.uses):   # buffer de loop NEON: sempre variavel nomeada
                w.var = True
                continue
            d = next(iter(w.defs))
            I = self.by_idx[d[0]]
            if I.kind == 'pseudo' and d[1] not in I.accs:
                self.warnings.append(f"valor clobberado pelo loop NEON {I.loop['idx']} usado depois: {d[1]}")
                w.var = True
                continue
            if self.const_like(d) or self.stable(d):
                continue
            if len(w.uses) != 1:
                w.var = True
                continue
            U, _ = w.uses[0]
            if U.block is not I.block or U.idx <= I.idx:
                w.var = True
                continue
            sup = self.support(d)
            for J in self.insns:
                if I.idx < J.idx < U.idx and J.idx in self.live and J.block is I.block:
                    if J.kind in ('call', 'store', 'pseudo') or any(
                            id(self.def_web.get((J.idx, r))) in sup for r in J.defs):
                        w.var = True
                        break
        self.coalesce()
        self.name_webs()

    def coalesce(self):
        """`R = f(); ... mov R2, R` no mesmo bloco -> a chamada ja escreve em R2."""
        self.merged_copy = set()
        for w in list(self.webs.values()):
            if not w.var or len(w.defs) != 1:
                continue
            d = next(iter(w.defs))
            if d[0] < 0:
                continue
            I = self.by_idx[d[0]]
            for U, _ in w.uses:
                if U.kind != 'copy' or U.block is not I.block or U.idx <= I.idx:
                    continue
                w2 = self.def_web.get((U.idx, U.defs[0]))
                if not w2 or not w2.var or len(w2.defs) != 1:
                    continue
                early = [V for V, _ in w2.uses if V.block is I.block and I.idx < V.idx <= U.idx]
                if early:
                    continue
                w.alias = w2
                self.merged_copy.add(U.idx)
                break

    def stable(self, d, depth=0):
        """Expressao pura (sem chamada nem leitura de memoria) calculada uma vez fora de loops, sobre
        valores que tambem nao mudam depois: pode ser repetida em qualquer uso (ex.: p + 0xc3000)."""
        if d[0] < 0 or depth > 20:
            return False
        I = self.by_idx[d[0]]
        if I.kind not in ('const', 'movk', 'copy', 'bin', 'stack') or self.in_loop(I.block):
            return False
        for u in I.uses:
            w = self.use_web[(I.idx, u)]
            if len(w.defs) != 1:
                return False
            dd = next(iter(w.defs))
            if dd[0] < 0 or self.in_loop(self.by_idx[dd[0]].block):
                return False
            J = self.by_idx[dd[0]]
            # chamada/leitura so pode ser referenciada se ja for variavel (mais de um uso)
            if J.kind in ('call', 'load', 'pseudo') and len(w.uses) < 2:
                return False
            if J.kind not in ('call', 'load', 'pseudo') and not (self.const_like(dd) or self.stable(dd, depth + 1)):
                return False
        return True

    def in_loop(self, b):
        return any(b in body for body in self.nat.values())

    def is_ptr_web(self, w, depth=0):
        """Web que guarda um ponteiro: buffer de loop NEON, retorno de malloc, copia ou deslocamento disso."""
        if depth > 20:
            return False
        for d in w.defs:
            if d[0] < 0:
                return False
            I = self.by_idx[d[0]]
            if I.kind == 'call' and I.name in PTR_RETURNING or I.kind == 'stack':
                continue
            if I.kind in ('copy', 'bin') and I.uses and (I.kind == 'copy' or I.op in '+-' and is_imm(I.srcs[1])):
                src = self.use_web.get((I.idx, I.uses[0]))
                if src is not None and (src is w or self.is_ptr_web(src, depth + 1)):
                    continue
            return False
        return True

    def web_type(self, w):
        defs = [self.by_idx[d[0]] for d in w.defs if d[0] >= 0]
        if any(I.kind == 'bin' for I in defs) and self.is_ptr_web(w):
            return 'bptr'
        if any(I.kind == 'pseudo' for I in defs):
            return 'u64'
        if any(I.kind == 'call' and I.name in PTR_RETURNING for I in defs):
            return 'void*'
        if w.reg.startswith('v'):
            return 'f64'
        return 'u32' if all(I.width == 32 for I in defs) and defs else 'u64'

    def name_webs(self):
        ptr_names = {}
        for L in self.loops:
            for k, v in L['ptr_names'].items():
                if int(k) >= 19:
                    ptr_names[f'x{k}'] = v
        used = set()
        webs = sorted((w for w in self.webs.values() if w.var and not hasattr(w, 'alias')),
                      key=lambda w: min(d[0] for d in w.defs))
        layouts = {}
        for L in self.loops:
            layouts.update(L.get('layouts', {}))
        self.structs = {}
        for w in webs:
            buf_use = any(I.kind == 'pseudo' for I, _ in w.uses)
            if buf_use and w.reg in ptr_names:
                w.name, w.t = ptr_names[w.reg], 'ptr'
                nf = layouts.get(w.name, 1)
                self.R.layouts[w.name] = nf
                if nf > 1:   # acessado com ld2/st2...: array of structs de nf campos uint32
                    self.structs[f"{w.name}_t"] = nf
                    w.ctype = f"{w.name}_t *"
                else:
                    w.ctype = CTYPE['ptr']
            else:
                w.t = self.web_type(w)
                w.ctype = CTYPE[w.t]
                n = w.reg[1:]
                base = {'u32': 'w', 'u64': 'x', 'void*': 'p', 'f64': 'd', 'bptr': 'b'}[w.t] + n
                w.name, k = base, 2
                while w.name in used:
                    w.name, k = f"{base}_{k}", k + 1
            used.add(w.name)
        for w in self.webs.values():
            if hasattr(w, 'alias'):
                w.name, w.t, w.ctype = w.alias.name, w.alias.t, w.alias.ctype

    # -- expressoes
    def use(self, I, r):
        if r == 'zr':
            return X('const', 0)
        w = self.use_web[(I.idx, r)]
        if w.var:
            return X('var', w.name, t=w.t)
        return self.build(next(iter(w.defs)))

    def operand(self, I, op):
        r, _ = reg(op)
        if r == 'zr':
            return X('const', 0)
        if r:
            return self.use(I, r)
        return X('const', imm(op))

    def build(self, d):
        if d in self.memo:
            return self.memo[d]
        I = self.by_idx[d[0]]
        k = I.kind
        t = 'u32' if I.width == 32 else 'u64'
        if k == 'const':
            e = X('const', I.val, t=t)
        elif k == 'movk':
            base = self.use(I, I.defs[0]).const()
            if base is None:
                raise SkeletonError(f"movk sobre valor nao constante: {I.line}")
            e = X('const', (base & ~(0xFFFF << I.sh) | (I.val << I.sh)) & mask(I.width), t=t)
        elif k == 'copy':
            e = self.use(I, I.uses[0])
        elif k == 'stack':
            e = X('stack', f"sp_{I.val:#x}", t='ptr')
            self.stack_objs.add(I.val)
        elif k == 'bin':
            a, b = self.operand(I, I.srcs[0]), self.operand(I, I.srcs[1])
            if I.shift:
                b = X('const', (b.const() << int(I.shift, 0)) & mask(I.width), t=t) if b.const() is not None                     else X('bin', '<<', b, X('const', int(I.shift, 0)), t=t)
            if a.const() is not None and b.const() is not None:
                v = eval(f"{a.const()} {I.op} {b.const()}") & mask(I.width)
                e = X('const', v, t=t)
            elif I.op in '+-' and a.t in ('ptr', 'void*', 'bptr') and b.const() is not None:
                # aritmetica de ponteiro em bytes (o binario soma bytes, nao elementos)
                delta = b.const() if I.op == '+' else -b.const()
                e = X('ptradd', a.a[0], a.a[1] + delta, t='bptr') if a.k == 'ptradd' else X('ptradd', a, delta, t='bptr')
            else:
                if t == 'u64':   # operacao de 64 bits sobre valores de 32 (ldr w, mov w): estende antes
                    a, b = [X('cast', 'uint64_t', v, t='u64') if v.t == 'u32' and v.const() is None else v for v in (a, b)]
                e = X('bin', I.op, a, b, t=t)
            if d[1] == 'nzcv':
                e = X('flags', 'cmp', a, b)
        elif k == 'flags':
            e = X('flags', I.flag, self.operand(I, I.srcs[0]), self.operand(I, I.srcs[1]))
        elif k in ('csel', 'cset'):
            c = self.cond_flags(I, I.cc)
            if k == 'cset':
                e = X('ternary', c, X('const', 1), X('const', 0), t=t)
            else:
                a, b = self.operand(I, I.srcs[0]), self.operand(I, I.srcs[1])
                if I.mn == 'csinc':
                    b = X('const', b.const() + 1) if b.const() is not None else X('bin', '+', b, X('const', 1), t=t)
                elif I.mn == 'csneg':
                    b = X('bin', '-', X('const', 0), b, t=t)
                elif I.mn == 'csinv':
                    b = X('bin', '^', b, X('const', mask(I.width)), t=t)
                e = X('ternary', c, a, b, t=t)
        elif k == 'load':
            e = X('load', self.use(I, reg(I.base)[0]), self.operand(I, I.off), I.width, t=t)
        elif k == 'call':
            e = X('call', I.name, [self.arg(I, n) for n in range(len(I.args))],
                  t='void*' if I.name in PTR_RETURNING else 'u64')
        elif k == 'cvt':
            e = X('cvt', I.signed, self.use(I, I.uses[0]), t='f64')
        elif k == 'f2i':
            e = X('f2i', self.use(I, I.uses[0]), t=t)
        elif k == 'bits2f':
            v = self.use(I, I.uses[0])
            e = X('fconst', struct.unpack('<d', struct.pack('<Q', v.const()))[0], t='f64') \
                if v.const() is not None else X('bits2f', v, t='f64')
        elif k == 'fconst':
            e = X('fconst', I.val, t='f64')
        elif k == 'fcopy':
            e = self.use(I, I.uses[0])
        elif k == 'fbin':
            e = X('bin', I.op, self.use(I, I.uses[0]), self.use(I, I.uses[1]), t='f64')
        elif k in ('vec', 'f2bits'):
            parts = [self.use(I, u) for u in I.uses]
            e = X('chk', t='u64') if parts and all(p.k == 'chk' for p in parts) else X('opaque', I.line)
        elif k == 'pseudo':
            e = X('chk', t='u64') if d[1] in I.accs else X('opaque', f"clobber do loop NEON {I.loop['idx']}")
        else:
            e = X('opaque', I.line)
        if e.k == 'opaque':
            self.warnings.append(f"valor sem traducao usado no esqueleto: {I.line}")
        self.memo[d] = e
        return e

    def arg(self, I, n):
        e = self.use(I, I.args[n])
        conv = I.convs[n] if n < len(I.convs) else None
        if conv and conv[1] in 'diouxX':
            ctype = {'ll': 'long long', 'l': 'long', 'j': 'intmax_t', 'z': 'size_t'}.get(conv[0], 'int')
            unsigned = conv[1] in 'ouxX' and ctype in ('long long', 'long', 'int')
            cast = f"{'unsigned ' if unsigned else ''}{ctype}"
            return X('cast', cast, e, t=e.t)
        return e

    def cond_flags(self, I, cc):
        f = self.use(I, 'nzcv')
        if f.k != 'flags':
            raise SkeletonError(f"flags sem comparacao conhecida em {I.line}")
        kind, a, b = f.a
        if kind == 'tst':
            return X('cmp', '==' if cc == 'eq' else '!=', X('bin', '&', a, b), X('const', 0), t='bool')
        return X('cmp', CC_OP[cc], a, b, t='bool')

    def branch_cond(self, I):
        """Condicao em que o desvio e tomado."""
        if I.kind == 'bcc':
            return self.cond_flags(I, I.cc)
        v = self.use(I, I.uses[0])
        if I.kind == 'cbz':
            return X('cmp', '==' if I.mn == 'cbz' else '!=', v, X('const', 0), t='bool')
        return X('cmp', '==' if I.mn == 'tbz' else '!=', X('bin', '&', v, X('const', 1 << I.bit)),
                 X('const', 0), t='bool')

    # -- emissao
    def render(self, e):
        return self.R(e)

    def cond(self, c):
        """Condicao para if/while, sem o par de parenteses externo redundante."""
        txt = self.R(c)
        if txt.startswith('(') and txt.endswith(')'):
            depth = 0
            for n, ch in enumerate(txt):
                depth += (ch == '(') - (ch == ')')
                if depth == 0 and n < len(txt) - 1:
                    return txt
            return txt[1:-1]
        return txt

    def assign(self, w, e):
        rhs = self.render(e)
        if w.t in ('ptr', 'bptr') and e.t != w.t:
            rhs = f"({w.ctype}){rhs}"
        return f"{w.name} = {rhs};"

    def stmts(self, b):
        out = []
        for I in b.insns:
            if I.idx not in self.live or I.kind in ('br', 'bcc', 'cbz', 'tbz', 'ret'):
                continue
            if I.kind == 'pseudo':
                L = I.loop
                if L.get('tmpl') is not None:
                    out.append("checksum = 0;")
                out.append(f"/* <LOOP_NEON_{L['idx']}> */")
            elif I.kind == 'call':
                w = self.def_web.get((I.idx, 'x0'))
                if w is None:
                    out.append(self.render(self.build((I.idx, 'x0'))) + ';')
                elif w.var:
                    out.append(self.assign(w, self.build((I.idx, 'x0'))))
            elif I.kind == 'dup':
                name = self.sym_names.get(I.defs[0], f"dup_{I.defs[0]}")
                self.syms.add(name)
                out.append(f"{name} = {self.render(self.use(I, I.uses[0]))};")
            elif I.kind == 'store':
                ctype = 'uint32_t' if I.width == 32 else 'uint64_t'
                out.append(f"*({ctype} *)((uint8_t *){self.render(self.use(I, reg(I.base)[0]))} + "
                           f"{self.render(self.operand(I, I.off))}) = {self.render(self.use(I, I.uses[0]))};")
            elif I.idx not in self.merged_copy:
                for r in I.defs:
                    w = self.def_web.get((I.idx, r))
                    if w is not None and w.var and r != 'nzcv':
                        out.append(self.assign(w, self.build((I.idx, r))))
        return out

    def return_tail(self, b):
        """Blocos de b em diante ate um `ret`, sem desvios no meio; None se nao for so cauda."""
        tail = []
        while b is not None:
            if b.neon or any(b in body for h, body in self.nat.items()):
                return None
            tail.append(b)
            t = b.term
            if t is not None:
                return tail if t.kind == 'ret' else None
            b = self.blocks[b.idx + 1] if b.idx + 1 < len(self.blocks) else None
        return None

    def emit_tail(self, tail):
        lines = []
        for b in tail:
            lines += self.stmts(b)
        lines.append(f"return {self.render(self.use(tail[-1].term, 'x0'))};")
        return lines

    def emit_range(self, lo, hi, loop):
        lines = []
        i = lo
        while i < hi:
            b = self.blocks[i]
            if b in self.nat and (loop is None or b is not loop[0]):
                body = self.nat[b]
                j = max(x.idx for x in body) + 1
                inner, start = self.loop_head(b, body)
                lines.append(inner)
                lines += ['  ' + s for s in self.emit_range(start, j, (b, body))]
                lines.append('}')
                i = j
                continue
            lines += self.stmts(b)
            t = b.term
            if t is not None and t.kind in COND_BRANCH:
                T = next(s for s in b.succ if s.start == t.target)
                c = self.branch_cond(t)
                if loop and T is loop[0]:
                    lines.append(f"if ({self.cond(neg(c))}) break;" if self.blocks[i + 1] not in loop[1]
                                 else f"if ({self.cond(c)}) continue;")
                elif loop and T not in loop[1]:
                    tail = self.return_tail(T)
                    lines += ([f"if ({self.cond(c)}) {{"] + ['  ' + s for s in self.emit_tail(tail)] + ['}']
                              if tail and T.idx != max(x.idx for x in loop[1]) + 1
                              else [f"if ({self.cond(c)}) break;"])
                elif self.return_tail(T) and T.idx > i + 1:
                    lines += [f"if ({self.cond(c)}) {{"] + ['  ' + s for s in self.emit_tail(self.return_tail(T))] + ['}']
                elif i + 1 < T.idx <= hi:
                    lines.append(f"if ({self.cond(neg(c))}) {{")
                    lines += ['  ' + s for s in self.emit_range(i + 1, T.idx, loop)]
                    lines.append('}')
                    i = T.idx
                    continue
                elif T.idx != i + 1:
                    raise SkeletonError(f"desvio condicional nao estruturado: {t.line}")
            elif t is not None and t.kind == 'br':
                T = next(iter(b.succ))
                if not (loop and T is loop[0]) and T.idx != i + 1:
                    raise SkeletonError(f"desvio incondicional nao estruturado: {t.line}")
            elif t is not None and t.kind == 'ret':
                lines.append(f"return {self.render(self.use(t, 'x0'))};")
            i += 1
        return lines

    def loop_head(self, h, body):
        """`while (c) {` quando o cabecalho so testa a saida; senao `for (;;) {`."""
        t = h.term
        if t is not None and t.kind in COND_BRANCH and not self.stmts(h):
            T = next(s for s in h.succ if s.start == t.target)
            c = self.branch_cond(t)
            if T not in body:
                return f"while ({self.cond(neg(c))}) {{", h.idx + 1
            return f"while ({self.cond(c)}) {{", h.idx + 1
        return "for (;;) {", h.idx

    def stack_types(self):
        """Objeto de pilha passado a uma funcao do shim cujo parametro e `T *` (T struct do header) -> T."""
        if not (self.shim_header and os.path.exists(self.shim_header)):
            return {}
        src = open(self.shim_header, encoding='utf-8').read()
        structs = set(re.findall(r'typedef\s+struct\s*\{.*?\}\s*(\w+)\s*;', src, re.S))
        protos = {}
        for name, params in re.findall(r'SHIM_API\s+[\w\s\*]+?\b(\w+)\s*\(([^)]*)\)', src):
            protos[name] = [re.sub(r'\b\w+\s*$', '', q).strip() for q in params.split(',')]
        found = {}
        for I in self.insns:
            if I.kind != 'call' or I.idx not in self.live or I.name not in protos:
                continue
            for n, r in enumerate(I.args):
                if n >= len(protos[I.name]):
                    break
                m = re.fullmatch(r'(?:const\s+)?(\w+)\s*\*', protos[I.name][n])
                if not m or m.group(1) not in structs:
                    continue
                for d in I.reach.get(r, ()):
                    J = self.by_idx.get(d[0])
                    if J is not None and J.kind == 'stack':
                        if found.setdefault(J.val, m.group(1)) != m.group(1):
                            raise SkeletonError(f"sp+{J.val:#x} usado como {found[J.val]} e {m.group(1)}")
        return found

    def code(self):
        self.stack_objs, self.syms = set(), set()
        typed_stack = self.stack_types()
        self.R.addr_of = {f"sp_{off:#x}" for off in typed_stack}
        # p0 = malloc(...); buf = p0; ... leituras via p0 + k  ->  leem buf[i].fN
        load_blocks = {I.block for I in self.insns if I.kind == 'load' and I.idx in self.live}
        for w in self.webs.values():
            if not (w.var and w.t == 'ptr' and len(w.defs) == 1 and not hasattr(w, 'alias')):
                continue
            C = self.by_idx.get(next(iter(w.defs))[0])
            if C is None or C.kind != 'copy':
                continue
            P = self.use_web.get((C.idx, C.uses[0]))
            if P is not None and P.var and len(P.defs) == 1 and all(C.block in self.dom[b] for b in load_blocks):
                self.R.ptr_alias[P.name] = w.name
        body = self.emit_range(0, len(self.blocks), None)
        frame_offs = sorted({imm(mem(next(o for o in I.ops if o.startswith('[')))[1])
                             for I in self.insns if I.kind == 'frame' and any(o.startswith('[sp') for o in I.ops)
                             and mem(next(o for o in I.ops if o.startswith('[')))[1].startswith('#')})
        decls = []
        seen = set()
        for w in sorted(self.webs.values(), key=lambda w: (w.name or '')):
            if w.var and w.name not in seen:
                seen.add(w.name)
                decls.append(f"{w.ctype}{'' if w.ctype.endswith('*') else ' '}{w.name};")
        for off in sorted(self.stack_objs):
            if off in typed_stack:
                decls.append(f"{typed_stack[off]} sp_{off:#x};  /* objeto local na pilha (sp+{off:#x}) */")
                continue
            size = next((o for o in frame_offs if o > off), off + 64) - off
            decls.append(f"uint8_t sp_{off:#x}[{size}];  /* objeto local na pilha (sp+{off:#x}), passado por endereco */")
        for s in sorted(self.syms):
            decls.append(f"uint32_t {s};")
        if any(L.get('tmpl') is not None for L in self.loops):
            decls.append("uint64_t checksum;")
        callees = {I.name for I in self.insns if I.kind == 'call'}
        inc = ['#include <stdio.h>', '#include <stdlib.h>', '#include <stdint.h>']
        if 'clock' in callees:
            inc.append('#include <time.h>')
        if self.shim_header and os.path.exists(self.shim_header):
            shim_src = open(self.shim_header, encoding='utf-8').read()
            shim_fns = set(re.findall(r'SHIM_API\s+[\w\s\*]+?\b(\w+)\s*\(', shim_src))
            if callees & shim_fns:
                inc.append(f'#include "{os.path.basename(self.shim_header)}"')
        typedefs = []
        for name, nf in sorted(self.structs.items()):
            fields = ' '.join(f"uint32_t f{4 * j};" for j in range(nf))
            typedefs.append(f"typedef struct {{ {fields} }} {name};  /* layout recuperado de ld{nf}/st{nf} */")
        lines = inc + [''] + (typedefs + [''] if typedefs else []) + ['int main(void) {'] + \
            ['  ' + d for d in decls] + [''] + ['  ' + s for s in body] + ['}']
        return '\n'.join(lines)


def build(fn, data, loops, shim_header='switch_hle_shim.h'):
    sk = Skeleton(fn, data, loops, shim_header)
    code = sk.code()
    for w in dict.fromkeys(sk.warnings):
        print(f"[esqueleto] aviso: {w}")
    return code
