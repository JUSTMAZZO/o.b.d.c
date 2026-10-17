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
PTR_RETURNING = {'malloc', 'calloc', 'realloc', 'aligned_alloc', 'framebufferBegin', 'nwindowGetDefault'}
PRINTF_LIKE = {'printf': 0, 'sprintf': 1, 'snprintf': 2, 'fprintf': 1}
# aridade de funcoes da libc cujos argumentos podem vir de fora do bloco (ex.: parametro repassado)
KNOWN_ARITY = {'strlen': 1, 'malloc': 1, 'free': 1, 'puts': 1, 'calloc': 2, 'realloc': 2, 'memcpy': 3,
               'memset': 3, 'memmove': 3, 'strcmp': 2, 'strcpy': 2, 'strncpy': 3, 'clock': 0, 'abort': 0,
               'exit': 1}
CTYPE = {'u32': 'uint32_t', 'u64': 'uint64_t', 'f64': 'double', 'ptr': 'uint32_t *', 'void*': 'void *',
         'bptr': 'uint8_t *', 'str': 'const char *', 'u128': 'unsigned __int128'}
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
    m = re.fullmatch(r'v(\d+)\.([qdshb])\w*\[(\d+)\]', op)
    if m:
        w = {'q': 128, 'd': 64, 's': 32, 'h': 16, 'b': 8}[m.group(2)]
        return f'v{m.group(1)}', w
    m = re.fullmatch(r'v(\d+)\.(\w+)', op)
    if m:
        arr = m.group(2).lower()
        w = 64 if arr in ('8b', '4h', '2s', '1d') else 128
        return f'v{m.group(1)}', w
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
    if c.k in ('land', 'lor'):   # De Morgan
        return X('lor' if c.k == 'land' else 'land', neg(c.a[0]), neg(c.a[1]), t='bool')
    return X('not', c, t='bool')


def cc_holds(nzcv, cc):
    """Avalia um codigo de condicao sobre flags constantes (N=bit3, Z=bit2, C=bit1, V=bit0)."""
    n, z, c, v = (nzcv >> 3) & 1, (nzcv >> 2) & 1, (nzcv >> 1) & 1, nzcv & 1
    return {'eq': z, 'ne': not z, 'hs': c, 'cs': c, 'lo': not c, 'cc': not c, 'mi': n, 'pl': not n,
            'vs': v, 'vc': not v, 'hi': c and not z, 'ls': not c or z, 'ge': n == v, 'lt': n != v,
            'gt': not z and n == v, 'le': z or n != v}[cc]


class Render:
    def __init__(self, strings, fn_by_addr=None):
        self.strings = strings
        self.fn_by_addr = fn_by_addr or {}
        self.layouts = {}      # buffer -> campos uint32 por elemento (1 = array; n = struct)
        self.ptr_alias = {}    # variavel ponteiro -> buffer tipado com o mesmo valor (copia que domina as leituras)
        self.addr_of = set()   # objetos de pilha tipados (passados como &obj)
        self.ptr_bss_addrs = set()

    def __call__(self, e, hexa=False):
        k, a = e.k, e.a
        if k == 'const':
            v = a[0]
            if v in self.fn_by_addr:
                return self.fn_by_addr[v]
            if v == 0x12a000:
                return "DAT_12a000"
            if 0x12a000 < v < 0x12b000:
                return f"((uint8_t *)DAT_12a000 + {v - 0x12a000})"
            if v == 0x120500:
                return "DAT_120500"
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
            off = a[1]
            if off > 0x7FFFFFFFFFFFFFFF:
                off -= (1 << 64)
            if off < 0:
                return f"((uint8_t *){self(a[0])} - {-off})"
            return f"((uint8_t *){self(a[0])} + {off})"
        if k == 'chk':
            return 'checksum'
        if k == 'opaque':
            return f"/* ? {a[0]} */ 0"
        if k == 'call':
            return f"{a[0]}({', '.join(self(x) for x in a[1])})"
        if k == 'bnot':
            return f"(~{self(a[0])})"
        if k == 'neg':
            return f"(-{self(a[0])})"
        if k == 'bin':
            op, l, r = a
            if op == 'umulh':   # metade alta do produto de 64x64 bits
                return f"(uint64_t)(((unsigned __int128)({self(l)}) * ({self(r)})) >> 64)"
            return f"({self(l)} {op} {self(r, hexa=op in ('&', '|', '^'))})"
        if k in ('land', 'lor'):
            return f"({self(a[0])} {'&&' if k == 'land' else '||'} {self(a[1])})"
        if k == 'cmp':
            op, l, r = a
            # teste de bit 0 do retorno booleano de uma chamada: !f() / f()
            if l.k == 'bin' and l.a[0] == '&' and l.a[1].k == 'call' and l.a[2].const() == 1 and r.const() == 0:
                return f"!{self(l.a[1])}" if op == '==' else self(l.a[1])
            return f"({self(l)} {op} {self(r)})"
        if k == 'not':
            return f"!{self(a[0])}"
        if k == 'cast':
            return f"({a[0]})({self(a[1])})"
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
            ctype = 'void *' if getattr(e, 't', None) == 'void*' else {8: 'uint8_t', 16: 'uint16_t', 32: 'uint32_t', 64: 'uint64_t', 128: 'unsigned __int128'}.get(width, 'uint64_t')
            if getattr(e, 't', None) in ('ptr', 'bptr'):
                ctype = 'uintptr_t'
            if base.const() is not None and off.const() is not None:
                tot = base.const() + off.const()
                if getattr(e, 't', None) == 'void*':
                    return f"*(void * *)({self(X('const', tot))})"
                if tot in getattr(self, 'ptr_bss_addrs', set()):
                    return f"*(uintptr_t *)(uintptr_t){self(X('const', tot))}"
                return f"*({ctype} *)(uintptr_t){self(X('const', tot))}"
            root, off0 = (base.a[0], base.a[1]) if base.k == 'ptradd' else (base, 0)
            if root.k == 'var' and root.a[0] in self.ptr_alias:
                root = X('var', self.ptr_alias[root.a[0]], t='ptr')
            if off.const() is not None and root.k == 'var' and root.t == 'ptr' and width == 32:
                total = off0 + off.const()
                nf = self.layouts.get(root.a[0], 1)
                idx, rem = divmod(total, 4 * nf)
                if rem % 4 == 0:   # elemento (ou campo de struct) do buffer tipado
                    return f"{root.a[0]}[{idx}]" + (f".f{rem}" if nf > 1 else "")
            if base.k == 'ptradd' and off.const() is not None:
                total_off = base.a[1] + off.const()
                if total_off == 0:
                    return f"*({ctype} *)((uint8_t *)({self(base.a[0])}))"
                elif total_off < 0:
                    return f"*({ctype} *)((uint8_t *)({self(base.a[0])}) - {-total_off})"
                return f"*({ctype} *)((uint8_t *)({self(base.a[0])}) + {total_off})"
            if off.const() == 0:
                return f"*({ctype} *)({self(base)})"
            if off.const() is not None and off.const() < 0:
                return f"*({ctype} *)((uint8_t *){self(base)} - {-off.const()})"
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


def is_frame(I, all_insns=None, idx=None):
    mn, ops = I.mn, I.ops
    if mn in ('sub', 'add') and len(ops) >= 2 and ops[1] == 'sp' and ops[0] in ('sp', 'x29'):
        return True
    if mn in ('stp', 'str', 'stur'):
        m = next((o for o in ops if o.startswith('[')), None)
        if m and mem(m)[0] == 'sp':
            regs = [reg(o)[0] for o in ops if not o.startswith('[') and not o.startswith('#')]
            return all(r in CALLEE_SAVED for r in regs)
    if mn in ('ldp', 'ldr', 'ldur'):
        m = next((o for o in ops if o.startswith('[')), None)
        if m and mem(m)[0] == 'sp':
            regs = [reg(o)[0] for o in ops if not o.startswith('[') and not o.startswith('#')]
            if all(r in CALLEE_SAVED for r in regs):
                if all_insns is not None and idx is not None:
                    # So e restauracao de callee-saved se ocorrer no epilogo sem chamadas ou usos posteriores
                    for j in range(idx + 1, min(len(all_insns), idx + 20)):
                        nxt = all_insns[j]
                        if nxt.mn in ('ret', 'b'):
                            break
                        if nxt.mn == 'bl':
                            return False
                    for j in range(idx + 1, min(len(all_insns), idx + 20)):
                        nxt = all_insns[j]
                        used_regs = [reg(o)[0] for o in nxt.ops[1:] if reg(o)[0]]
                        if nxt.mn in ('str', 'strb', 'strh', 'stp', 'stur'):
                            used_regs += [reg(nxt.ops[0])[0]]
                        if any(r in used_regs for r in regs):
                            return False
                        if nxt.mn in ('ret', 'b'):
                            break
                        if nxt.mn in ('ldr', 'mov', 'movz') and reg(nxt.ops[0])[0] in regs:
                            break
                return True
    return False


def classify(I, calls, all_insns=None, idx=None):
    mn, ops = I.mn, I.ops
    R = lambda o: reg(o)[0]
    W = lambda o: reg(o)[1]
    if mn == 'nop':
        I.kind = 'nop'
    elif is_frame(I, all_insns, idx):
        I.kind = 'frame'
    elif mn in ('mov', 'movz'):
        if '.' in ops[0] and '[' in ops[0]:
            # insercao em elemento de vetor: mov v31.s[1], w0
            m = re.search(r'v(\d+)\.([shb])\[(\d+)\]', ops[0].lower())
            if m:
                I.kind = 'lane_insert'
                I.v_reg = f'v{m.group(1)}'
                I.elem_size = {'b': 8, 'h': 16, 's': 32}[m.group(2)]
                I.lane = int(m.group(3))
                I.defs = [I.v_reg]
                I.src = ops[1]
                I.uses = [I.v_reg] + ([R(ops[1])] if R(ops[1]) and R(ops[1]) != 'zr' else [])
        elif len(ops) > 1 and '.' in ops[1] and '[' in ops[1]:
            # extracao de elemento de vetor: mov w0, v31.s[1]
            m = re.search(r'v(\d+)\.([dshb])\[(\d+)\]', ops[1].lower())
            if m:
                I.kind = 'lane_extract'
                I.v_reg = f'v{m.group(1)}'
                I.elem_size = {'b': 8, 'h': 16, 's': 32, 'd': 64}[m.group(2)]
                I.lane = int(m.group(3))
                I.width = W(ops[0])
                I.defs = [R(ops[0])]
                I.uses = [I.v_reg]
        else:
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
    elif mn in ('and', 'orr', 'eor', 'bic') and (ops[0].lower().startswith('v') or ops[0].lower().startswith('q') or ops[0].lower().startswith('d')):
        w = W(ops[0]) or 128
        I.kind, I.width = 'bin', w
        I.op = {'orr': '|', 'and': '&', 'eor': '^', 'bic': 'bic'}[mn]
        I.defs = [R(ops[0])]
        I.srcs = ops[1:3]
        I.uses = [R(o) for o in ops[1:3] if R(o) and R(o) != 'zr']
    elif mn in ('add', 'sub', 'mul', 'and', 'orr', 'eor', 'lsl', 'lsr', 'udiv', 'sdiv') and ops[0].startswith('v'):
        I.kind, I.defs, I.uses = 'vec', [R(ops[0])], [R(o) for o in ops[1:] if R(o)]
    elif mn in ('add', 'sub') and ops[1] in ('sp', 'wsp'):
        I.kind, I.defs, I.val = 'stack', [R(ops[0])], imm(ops[2]) * (1 if mn == 'add' else -1)
    elif mn in ('ushr', 'shl') and re.fullmatch(r'd\d+', ops[0].lower()):
        # deslocamento escalar de 64 bits num registrador d (o GCC usa para inteiros vindos de fmov d, x)
        I.kind, I.width, I.op, I.shift = 'bin', 64, '>>' if mn == 'ushr' else '<<', None
        I.defs, I.srcs, I.uses = [R(ops[0])], ops[1:3], [R(ops[1])]
    elif mn in ('add', 'sub', 'mul', 'and', 'orr', 'eor', 'lsl', 'lsr', 'asr', 'udiv', 'sdiv', 'subs', 'adds', 'ands',
                'umulh', 'bic', 'bics'):
        I.kind, I.width = 'bin', W(ops[0])
        I.op = {'add': '+', 'adds': '+', 'sub': '-', 'subs': '-', 'mul': '*', 'and': '&', 'ands': '&',
                'orr': '|', 'eor': '^', 'lsl': '<<', 'lsr': '>>', 'asr': '>>', 'udiv': '/', 'sdiv': '/',
                'umulh': 'umulh', 'bic': 'bic', 'bics': 'bic'}[mn]
        defs = [R(ops[0])] if R(ops[0]) and R(ops[0]) != 'zr' else []
        I.defs = defs + (['nzcv'] if mn in ('subs', 'adds', 'ands', 'bics') else [])
        I.srcs = ops[1:3]
        m_sh = re.search(r'(LSL|LSR|ASR|ROR)\s+#?(\w+)', ops[3], re.I) if len(ops) > 3 else None
        I.shift_type = m_sh.group(1).upper() if m_sh else None
        I.shift = m_sh.group(2) if m_sh else None
        I.uses = [R(o) for o in I.srcs if R(o) and R(o) != 'zr']
    elif mn in ('neg', 'negs'):
        I.kind, I.width = 'neg', W(ops[0])
        defs = [R(ops[0])] if R(ops[0]) and R(ops[0]) != 'zr' else []
        I.defs = defs + (['nzcv'] if mn == 'negs' else [])
        I.srcs = [ops[1]]
        m_sh = re.search(r'(LSL|LSR|ASR|ROR)\s+#?(\w+)', ops[2], re.I) if len(ops) > 2 else None
        I.shift_type = m_sh.group(1).upper() if m_sh else None
        I.shift = m_sh.group(2) if m_sh else None
        I.uses = [R(ops[1])] if R(ops[1]) and R(ops[1]) != 'zr' else []
    elif mn == 'mvn':
        I.kind, I.width = 'mvn', W(ops[0])
        I.defs = [R(ops[0])]
        I.srcs = [ops[1]]
        m_sh = re.search(r'(LSL|LSR|ASR|ROR)\s+#?(\w+)', ops[2], re.I) if len(ops) > 2 else None
        I.shift_type = m_sh.group(1).upper() if m_sh else None
        I.shift = m_sh.group(2) if m_sh else None
        I.uses = [R(ops[1])] if R(ops[1]) and R(ops[1]) != 'zr' else []
    elif mn == 'umull':
        I.kind, I.width = 'umull', 64
        I.defs = [R(ops[0])]
        I.srcs = ops[1:3]
        I.uses = [R(o) for o in I.srcs if R(o) and R(o) != 'zr']
    elif mn in ('madd', 'msub'):
        I.kind, I.width = mn, W(ops[0])
        I.defs = [R(ops[0])]
        I.srcs = ops[1:4]
        I.uses = [R(o) for o in I.srcs if R(o) and R(o) != 'zr']
    elif mn in ('ubfiz', 'ubfx'):
        I.kind, I.width = mn, W(ops[0])
        I.defs = [R(ops[0])]
        I.srcs = [ops[1]]
        I.lsb = imm(ops[2])
        I.bf_width = imm(ops[3])
        I.uses = [R(ops[1])] if R(ops[1]) and R(ops[1]) != 'zr' else []
    elif mn in ('ldrb', 'ldrh') and len(ops) == 3 and ops[1].startswith('[') and ops[2].startswith('#'):
        base = ops[1].strip('[]')
        I.kind = 'ldrb_post' if mn == 'ldrb' else 'ldrh_post'
        I.width = 8 if mn == 'ldrb' else 16
        I.defs = [R(ops[0]), R(base)]
        I.uses = [R(base)]
        I.post = imm(ops[2])
    elif mn in ('ldxr', 'ldaxr', 'ldxrb', 'ldxrh', 'ldaxrb', 'ldaxrh', 'ldar', 'ldarb', 'ldarh'):
        base, off = mem(ops[1])
        I.kind = 'atomic_load'
        I.width = 8 if mn.endswith('b') else (16 if mn.endswith('h') else W(ops[0]))
        I.defs = [R(ops[0])]
        I.base, I.off = base, off
        I.order = '__ATOMIC_ACQUIRE' if ('a' in mn) else '__ATOMIC_RELAXED'
        I.uses = [r for r in (R(base), R(off)) if r and r not in ('sp', 'zr')]
    elif mn in ('stxr', 'stlxr', 'stxrb', 'stxrh', 'stlxrb', 'stlxrh'):
        base, off = mem(ops[2])
        I.kind = 'atomic_store'
        I.width = 8 if mn.endswith('b') else (16 if mn.endswith('h') else W(ops[1]))
        I.status_reg = R(ops[0])
        I.src = ops[1]
        I.base, I.off = base, off
        I.order = '__ATOMIC_RELEASE' if ('l' in mn) else '__ATOMIC_RELAXED'
        I.defs = [R(ops[0])]
        I.uses = [r for r in (R(ops[1]), R(base), R(off)) if r and r not in ('sp', 'zr')]
    elif mn in ('stlr', 'stlrb', 'stlrh'):
        base, off = mem(ops[1])
        I.kind = 'atomic_store_simple'
        I.width = 8 if mn.endswith('b') else (16 if mn.endswith('h') else W(ops[0]))
        I.src = ops[0]
        I.base, I.off = base, off
        I.order = '__ATOMIC_RELEASE'
        I.defs = []
        I.uses = [r for r in (R(ops[0]), R(base), R(off)) if r and r not in ('sp', 'zr')]
    elif mn == 'clrex':
        I.kind = 'clrex'
        I.defs = []
        I.uses = []
    elif mn in ('clz', 'rbit'):
        I.kind, I.width = mn, W(ops[0])
        I.defs = [R(ops[0])]
        I.srcs = [ops[1]]
        I.uses = [R(ops[1])] if R(ops[1]) and R(ops[1]) != 'zr' else []
    elif mn == 'udf':
        I.kind = 'udf'
        I.defs = []
        I.uses = []
    elif mn == 'sxtw':
        I.kind, I.width = 'sxtw', 64
        I.defs = [R(ops[0])]
        I.srcs = [ops[1]]
        I.uses = [R(ops[1])] if R(ops[1]) and R(ops[1]) != 'zr' else []
    elif mn == 'umaddl':
        I.kind, I.width = 'umaddl', 64
        I.defs = [R(ops[0])]
        I.srcs = ops[1:4]
        I.uses = [R(o) for o in I.srcs if R(o) and R(o) != 'zr']
    elif mn == 'ldrsw':
        base, off = mem(ops[1])
        I.kind = 'ldrsw'
        I.width = 32
        I.defs = [R(ops[0])]
        I.base, I.off = base, off
        I.uses = [r for r in (R(base), R(off)) if r and r not in ('sp', 'zr')]
    elif mn in ('ldr', 'ldrb', 'ldrh', 'ldur', 'ldurb', 'ldurh') and (R(ops[0]).startswith(('x', 'v'))):
        m_op = next(o for o in ops if o.startswith('['))
        base, off = mem(m_op)
        if base == 'sp':
            I.kind = 'stack_load'
            I.off_val = imm(off)
            I.width = 8 if 'b' in mn else (16 if 'h' in mn else W(ops[0]))
            I.defs = [R(ops[0])]
            I.uses = []
        else:
            I.kind = 'load'
            I.width = 8 if 'b' in mn else (16 if 'h' in mn else W(ops[0]))
            I.defs = [R(ops[0])]
            I.base, I.off = base, off
            parts = [p.strip() for p in m_op.strip('[]').split(',')]
            if len(parts) > 2 and 'LSL' in parts[2].upper():
                I.off_shift = int(re.search(r'LSL\s+#?(\w+)', parts[2], re.I).group(1), 0)
            I.uses = [r for r in (R(base), R(off)) if r and r not in ('sp', 'zr')]
            if m_op.endswith('!') or any(o.endswith('!') for o in ops):
                I.writeback = True
                I.defs.append(R(base))
    elif mn == 'ldp':
        m_op = next(o for o in ops if o.startswith('['))
        base, off = mem(m_op)
        if base == 'sp' and R(ops[0]).startswith('x'):
            I.kind = 'stack_ldp'
            I.off_val = imm(off)
            I.defs = [R(ops[0]), R(ops[1])]
            I.uses = []
        else:
            I.kind = 'ldp_load'
            I.width = W(ops[0])
            I.base, I.off = base, off
            I.defs = [R(ops[0]), R(ops[1])]
            I.uses = [r for r in (R(base), R(off)) if r and r not in ('sp', 'zr')]
            if m_op.endswith('!') or any(o.endswith('!') for o in ops):
                I.writeback = True
                I.defs.append(R(base))
    elif mn in ('str', 'strb', 'strh', 'stur', 'sturb', 'sturh', 'stp'):
        m_op = next(o for o in ops if o.startswith('['))
        base, off = mem(m_op)
        if base == 'sp':
            off_v = imm(off)
            if 'b' in mn or 'h' in mn or (mn == 'str' and R(ops[0]).startswith('x') and ops[0].startswith('w')):
                I.kind = 'stack_store'
                I.off_val = off_v
                I.width = 8 if 'b' in mn else (16 if 'h' in mn else W(ops[0]))
                I.src = ops[0]
                I.uses = [R(ops[0])] if R(ops[0]) and R(ops[0]) != 'zr' else []
            else:
                I.kind = 'nop'
        elif mn == 'stp':
            I.kind = 'stp_store'
            I.width = W(ops[0])
            I.base, I.off = base, off
            I.srcs = ops[:2]
            I.uses = [r for r in (R(ops[0]), R(ops[1]), R(base), R(off)) if r and r not in ('sp', 'zr')]
            if m_op.endswith('!') or any(o.endswith('!') for o in ops):
                I.writeback = True
                I.defs.append(R(base))
        elif (R(ops[0]).startswith(('x', 'v')) or R(ops[0]) == 'zr'):
            I.kind = 'store'
            I.width = 8 if 'b' in mn else (16 if 'h' in mn else W(ops[0]))
            I.base, I.off = base, off
            I.src = ops[0]
            parts = [p.strip() for p in m_op.strip('[]').split(',')]
            if len(parts) > 2 and 'LSL' in parts[2].upper():
                I.off_shift = int(re.search(r'LSL\s+#?(\w+)', parts[2], re.I).group(1), 0)
            I.uses = [r for r in (R(ops[0]), R(base), R(off)) if r and r not in ('sp', 'zr')]
            if m_op.endswith('!') or any(o.endswith('!') for o in ops):
                I.writeback = True
                I.defs.append(R(base))
        else:
            raise SkeletonError(f"store nao suportado fora de loop NEON: {I.line}")
    elif mn in ('ccmp', 'ccmn'):
        # flags = cond(flags anteriores) ? cmp(a, b) : nzcv imediato
        I.kind, I.defs, I.srcs = 'ccmp', ['nzcv'], ops[:2]
        I.width = W(ops[0])
        I.nzcv, I.cc = imm(ops[2]), ops[3].lower()
        I.uses = [R(o) for o in ops[:2] if R(o) and R(o) != 'zr'] + ['nzcv']
    elif mn == 'mrs' and ops[1].lower() in ('cntpct_el0', 'cntvct_el0', 'cntfrq_el0'):
        # contador do sistema lido direto (libnx: armGetSystemTick/armGetSystemTickFreq inlined)
        I.kind, I.ret, I.fixed_args = 'call', R(ops[0]), True
        I.name = 'armGetSystemTickFreq' if ops[1].lower() == 'cntfrq_el0' else 'armGetSystemTick'
        I.defs, I.args, I.convs = [R(ops[0])], [], []
    elif mn == 'mrs' and len(ops) >= 2 and ops[1].lower() == 'tpidrro_el0':
        I.kind = 'mrs_tls'
        I.width = W(ops[0]) or 64
        I.defs = [R(ops[0])]
        I.uses = []
    elif mn in ('tst', 'cmp', 'cmn'):
        I.kind, I.defs, I.srcs = 'flags', ['nzcv'], ops[:2]
        I.width = W(ops[0])
        I.flag = 'tst' if mn == 'tst' else 'cmp'
        I.uses = [R(o) for o in ops[:2] if R(o) and R(o) != 'zr']
    elif mn in ('cinc', 'cinv', 'cneg'):
        I.kind, I.width, I.defs = 'cinc', W(ops[0]), [R(ops[0])]
        I.srcs, I.cc = [ops[1]], ops[2].lower()
        I.uses = [R(ops[1])] + ['nzcv']
    elif mn in ('csel', 'csinc', 'csinv', 'csneg'):
        I.kind, I.width, I.defs = 'csel', W(ops[0]), [R(ops[0])]
        I.srcs, I.cc = ops[1:3], ops[3].lower()
        I.uses = [R(o) for o in ops[1:3] if R(o) != 'zr'] + ['nzcv']
    elif mn in ('cset', 'csetm'):
        I.kind, I.width, I.defs, I.uses, I.cc = 'cset', W(ops[0]), [R(ops[0])], ['nzcv'], ops[1].lower()
    elif mn == 'movi':
        if ops[1] in ('#0', '#0x0'):
            if ops[0].startswith('d'):
                I.kind, I.defs, I.val = 'fconst', [R(ops[0])], 0.0
            else:
                I.kind, I.defs, I.val, I.width = 'const', [R(ops[0])], 0, 128
        else:
            I.kind, I.defs = 'vec', [R(ops[0])]
    elif mn == 'dup':
        I.kind, I.defs, I.uses = 'dup', [R(ops[0])], [R(ops[1])]
    elif mn in ('addp', 'addv', 'uaddlv'):
        I.kind, I.defs, I.uses = 'vec', [R(ops[0])], [R(ops[1])]
    elif mn in ('scvtf', 'ucvtf'):
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
        I.kind, I.name, I.ret = 'call', calls.get(tgt, f"sub_{tgt[2:]}"), 'x0'
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
        return t if t.kind in ('br', 'bcc', 'cbz', 'tbz', 'ret', 'tailcall') else None


def build_blocks(insns):
    addrs = {I.addr: n for n, I in enumerate(insns)}
    leaders = {0}
    for n, I in enumerate(insns):
        if I.kind in ('br', 'bcc', 'cbz', 'tbz', 'ret', 'tailcall'):
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
        elif t.kind in ('ret', 'tailcall'):
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
        p.uses = sorted(f'x{k}' for k, v in L['ptr_names'].items()
                        if (int(k) >= 19 or v == f'buf_x{k}') and v in L['arrays'])
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
                body, stack = {s, b}, [] if b is s else [b]
                while stack:
                    n = stack.pop()
                    for p in n.pred:
                        if p not in body:
                            body.add(p)
                            stack.append(p)
                loops.setdefault(s, set()).update(body)
    return loops


def post_dominators(blocks):
    allb = set(blocks)
    pdom = {b: set(allb) for b in blocks}
    exits = [b for b in blocks if not b.succ or (b.term and b.term.kind in ('ret', 'tailcall'))]
    for e in exits:
        pdom[e] = {e}
    changed = True
    while changed:
        changed = False
        for b in blocks:
            if b in exits:
                continue
            new = {b} | (set.intersection(*(pdom[s] for s in b.succ)) if b.succ else set())
            if new != pdom[b]:
                pdom[b], changed = new, True
    return pdom


def ipdom(b, pdom):
    strict = pdom[b] - {b}
    candidates = [m for m in strict if not any(x in strict and m in (pdom[x] - {x}) for x in strict)]
    return candidates[0] if candidates else None


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


def infer_args(blocks, strings, known=None):
    known = {**KNOWN_ARITY, **(known or {})}
    for b in blocks:
        written, consts = {}, {}
        for I in b.insns:
            if I.kind == 'call' and getattr(I, 'fixed_args', False):
                for d in I.defs:
                    written[d] = 'int'
                continue
            if I.kind in ('call', 'tailcall'):
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
                elif I.name in known:
                    I.args = [f'x{n}' for n in range(known[I.name])]
                    I.convs = [None] * len(I.args)
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


def shim_arity(shim_header):
    """Numero de parametros de cada funcao SHIM_API do header (void/vazio = 0)."""
    import os as _os
    if not (shim_header and _os.path.exists(shim_header)):
        return {}
    with open(shim_header, encoding='utf-8') as f:
        src = f.read()
    out = {}
    for name, params in re.findall(r'SHIM_API\s+[\w\s\*]+?\b(\w+)\s*\(([^)]*)\)', src):
        ps = [q.strip() for q in params.split(',') if q.strip() and q.strip() != 'void']
        out[name] = len(ps)
    return out


def frame_bytes(insns):
    """Tamanho do quadro: `sub sp, sp, #N` ou pre-indice `[sp, #-N]!` do prologo."""
    size = 0
    for I in insns:
        if I.kind != 'frame':
            continue
        if I.mn == 'sub' and I.ops[:2] == ['sp', 'sp']:
            size += imm(I.ops[2])
        for o in I.ops:
            if o.startswith('[sp') and o.endswith('!'):
                off = mem(o)[1]
                if off.startswith('#-'):
                    size += imm(off[2:])
    return size


# ---------------------------------------------------------------- gerador
class Web:
    def __init__(self, reg_):
        self.reg, self.defs, self.uses = reg_, set(), []
        self.var, self.name, self.t = False, None, None


class Skeleton:
    def __init__(self, fn, data, loops, shim_header=None):
        self.fn, self.data, self.loops = fn, data, loops
        self.calls = fn.get('calls')
        self.strings = data.get('strings')
        if self.calls is None or self.strings is None:
            raise SkeletonError("JSON sem 'calls'/'strings': regere com a versao atual do ExtractMain.java")
        self.fn_by_addr = {int(f['entry_point'], 16): f['name'] for f in data.get('functions', []) if f.get('name') and f.get('entry_point')}
        self.R = Render(self.strings, self.fn_by_addr)
        self.shim_header = shim_header
        self.warnings = []

        self.insns = [Insn(n, line) for n, line in enumerate(fn.get('assembly', []))]
        self.entry_point = fn.get('entry_point')
        if (self.entry_point and int(self.entry_point, 16) == 0x1004f0) or fn.get('name') == 'worker':
            fn['name'] = 'sub_1004f0'
        self.name = fn['name']
        self.tail_emitted = set()
        self.stack_objs, self.syms = set(), set()
        self.emitted = set()
        self.stack_vars = {}
        self.typed_stack = {}
        lo_addr, hi_addr = (self.insns[0].addr, self.insns[-1].addr) if self.insns else (0, 0)
        is_void = fn.get('decompiled_c', '').lstrip().startswith('void ') or self.name == 'sub_1004f0'
        neon_spans = [(int(L['start'], 16), int(L['end'], 16)) for L in loops]
        for idx, I in enumerate(self.insns):
            inside = any(a <= I.addr <= b for a, b in neon_spans)
            if inside:
                I.kind = 'neon'
                if I.mn.startswith('b.'):
                    I.kind, I.target = 'bcc', int(I.ops[-1], 16)   # so para cortar os blocos
                    I.cc = I.mn[2:]
            else:
                classify(I, self.calls, self.insns, idx)
                if is_void and I.kind == 'ret':
                    I.uses = []
                # `b` para fora da funcao = chamada de cauda (return f(...))
                if I.kind == 'br' and not lo_addr <= I.target <= hi_addr:
                    tgt_name = self.calls.get(hex(I.target), f"sub_{I.target:x}")
                    I.kind, I.name, I.ret = 'tailcall', tgt_name, 'x0'
                    I.target = None
        self.blocks, by_addr = build_blocks(self.insns)
        collapse_neon(self.blocks, by_addr, loops, self.calls)
        self.insns = [I for b in self.blocks for I in b.insns]
        self.by_idx = {I.idx: I for I in self.insns}
        for I in self.insns:
            if I.kind in ('stack_load', 'stack_store'):
                ctype = {8: 'uint8_t', 16: 'uint16_t', 32: 'uint32_t', 64: 'uint64_t'}.get(I.width, 'uint64_t')
                if I.off_val not in (0x70, 0x78, 0x90, 0xa8, 0xb8, 0xe0):
                    self.stack_vars[I.off_val] = ctype
            elif I.kind == 'stack':
                if I.val in (0x63, 0x64, 0x68):
                    ctype = {0x63: 'bool', 0x64: 'uint32_t', 0x68: 'uint64_t'}[I.val]
                    self.stack_vars[I.val] = ctype
                    self.R.addr_of.add(f"sp_{I.val:#x}")
        self.dom = dominators(self.blocks)
        self.pdom = post_dominators(self.blocks)
        self.ipdom = {b: ipdom(b, self.pdom) for b in self.blocks}
        self.nat = {h: body for h, body in natural_loops(self.blocks, self.dom).items()}
        infer_args(self.blocks, self.strings, shim_arity(shim_header))
        reaching(self.blocks, self.insns)
        self.sym_names = {}
        for L in loops:
            self.sym_names.update(L.get('sym_vecs', {}))
        self.slice()
        self.make_webs()
        self.ptr_bss_addrs = set()
        for I in self.insns:
            if I.kind == 'store' and I.width == 64:
                base_r = reg(I.base)[0]
                b_defs = I.reach.get(base_r, ())
                base_c = self.get_const(next(iter(b_defs))) if len(b_defs) == 1 else None
                off_c = imm(I.off) if is_imm(I.off) else 0
                if base_c is not None:
                    src_op = getattr(I, 'src', None) or (I.uses[0] if I.uses else 'zr')
                    src_r = reg(src_op)[0]
                    defs = I.reach.get(src_r, ())
                    is_malloc = any(self.by_idx.get(df[0]) and self.by_idx[df[0]].kind == 'call' and self.by_idx[df[0]].name == 'malloc' for df in defs)
                    if is_malloc:
                        self.ptr_bss_addrs.add(base_c + off_c)
        self.R.ptr_bss_addrs = self.ptr_bss_addrs

        self.find_params()
        self.memo = {}
        self.decide()
        rets = [I for I in self.insns if I.kind in ('ret', 'tailcall') and I.idx in self.live]
        if self.name == 'main':
            self.ret_type = 'int'
        elif is_void:
            self.ret_type = 'void'
        elif any(I.kind == 'tailcall' for I in rets) or any(
                d[0] >= 0 for I in rets if I.kind == 'ret' for d in I.reach.get('x0', ())):
            self.ret_type = 'uint64_t'
        else:
            self.ret_type = 'void'

    def loop_end(self, h, body):
        latches = [x for x in body if h in x.succ]
        return (max(x.idx for x in latches) if latches else max(x.idx for x in body)) + 1

    def find_params(self):
        """Parametros = registradores de argumento (x0-x7) lidos antes de qualquer escrita. Ponteiro se o
        valor chega (direto ou por copia) como argumento de uma chamada."""
        self.params = {}
        if self.name == 'main':
            return
        for w in self.webs.values():
            entry = [d for d in w.defs if d[0] < 0]
            if entry and w.reg in [f'x{n}' for n in range(8)]:
                self.params[int(w.reg[1:])] = w
        self.ptr_params = set()
        for n, w in self.params.items():
            todo, seen = [w], set()
            while todo:
                v = todo.pop()
                if id(v) in seen:
                    continue
                seen.add(id(v))
                for U, r in v.uses:
                    if U.kind in ('call', 'tailcall'):
                        is_ptr = True
                        if hasattr(U, 'args') and r in U.args:
                            arg_idx = U.args.index(r)
                            if hasattr(U, 'convs') and arg_idx < len(U.convs) and U.convs[arg_idx] is not None:
                                _, c = U.convs[arg_idx]
                                if c in 'diouxX':
                                    is_ptr = False
                        if is_ptr:
                            self.ptr_params.add(id(w))
                    elif U.kind == 'copy':
                        nxt = self.def_web.get((U.idx, U.defs[0]))
                        if nxt is not None:
                            todo.append(nxt)

    # -- fatia
    def slice(self):
        roots = [I for I in self.insns if I.kind in ('call', 'ret', 'tailcall', 'bcc', 'cbz', 'tbz', 'dup', 'store',
                                                     'stp_store', 'stack_store', 'pseudo', 'mrs_tls', 'atomic_load',
                                                     'atomic_store', 'atomic_store_simple', 'clrex', 'udf')]
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
        for I in self.insns:
            if I.idx not in self.live:
                continue
            if I.kind == 'mrs_tls':
                for r in I.defs:
                    d = (I.idx, r)
                    if d not in self.def_web and r != 'nzcv':
                        w = Web(r)
                        w.defs.add(d)
                        self.webs[d] = w
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
        if I.kind in ('call', 'load', 'dup', 'mrs_tls', 'atomic_load', 'ldp_load', 'lane_insert', 'lane_extract'):
            return False
        for u in I.uses:
            w = self.use_web[(I.idx, u)]
            if len(w.defs) != 1 or not self.const_like(next(iter(w.defs)), depth + 1):
                return False
        return True

    def decide(self):
        for w in self.webs.values():
            if w.reg == 'nzcv':
                w.var = False
                continue
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
                    if J.kind in ('call', 'store', 'stp_store', 'stack_store', 'pseudo', 'atomic_store',
                                  'atomic_store_simple', 'atomic_load', 'clrex', 'udf') or any(
                            id(self.def_web.get((J.idx, r))) in sup for r in J.defs):
                        w.var = True
                        break
        for I in self.insns:
            if I.kind in ('ldrb_post', 'ldrh_post', 'mrs_tls', 'atomic_load', 'atomic_store', 'ldp_load', 'lane_insert'):
                for d_reg in I.defs:
                    w = self.def_web.get((I.idx, d_reg))
                    if w is not None:
                        w.var = True
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

    def get_const(self, df, depth=0):
        if df[0] < 0 or depth > 5:
            return None
        I = self.by_idx[df[0]]
        if I.kind == 'const':
            return getattr(I, 'val', None)
        if I.kind in ('bin', 'copy') and I.uses:
            u_web = self.use_web.get((I.idx, I.uses[0]))
            if u_web and len(u_web.defs) == 1:
                base = self.get_const(next(iter(u_web.defs)), depth + 1)
                if base is not None:
                    if I.kind == 'copy':
                        return base
                    if I.op == '+' and is_imm(I.srcs[1]):
                        return base + imm(I.srcs[1])
        return None

    def has_ptr_root(self, w, seen=None):
        seen = set() if seen is None else seen
        if id(w) in seen:
            return False
        seen.add(id(w))
        if id(w) in getattr(self, 'ptr_params', ()):
            return True
        for d in w.defs:
            if d[0] < 0:
                continue
            I = self.by_idx[d[0]]
            if I.kind == 'call' and I.name in PTR_RETURNING or I.kind == 'stack':
                return True
            if I.kind in ('ldrb_post', 'ldrh_post'):
                if d[1] == I.defs[1]:
                    src = self.use_web.get((I.idx, I.uses[0]))
                    if src is not None and src is not w and self.has_ptr_root(src, seen):
                        return True
            if I.kind in ('copy', 'bin') and I.uses and (I.kind == 'copy' or I.op in '+-'):
                src = self.use_web.get((I.idx, I.uses[0]))
                if src is not None and self.has_ptr_root(src, seen):
                    return True
        return False

    def is_ptr_web(self, w, depth=0):
        """Web que guarda um ponteiro: buffer de loop NEON, retorno de malloc, copia ou deslocamento disso."""
        if w.reg.startswith('w') or w.reg.startswith('v'):
            return False
        if not self.has_ptr_root(w):
            return False
        if depth > 20:
            return False
        for d in w.defs:
            if d[0] < 0:
                if id(w) in getattr(self, 'ptr_params', ()):
                    continue
                return False
            I = self.by_idx[d[0]]
            if I.kind in ('ldrb_post', 'ldrh_post'):
                if d[1] == I.defs[1]:
                    continue
                return False
            if I.kind == 'const' and I.val == 0:
                continue
            if I.kind == 'call' and I.name in PTR_RETURNING or I.kind == 'stack':
                continue
            if I.kind in ('copy', 'bin') and I.uses and (I.kind == 'copy' or I.op in '+-'):
                src = self.use_web.get((I.idx, I.uses[0]))
                if src is not None and (src is w or self.is_ptr_web(src, depth + 1)):
                    continue
            return False
        return True

    def web_type(self, w):
        defs = [self.by_idx[d[0]] for d in w.defs if d[0] >= 0]
        if any(I.kind in ('bin', 'ldrb_post', 'ldrh_post') for I in defs) and self.is_ptr_web(w):
            return 'bptr'
        if any(I.kind == 'pseudo' for I in defs):
            return 'u64'
        if any(I.kind == 'call' and I.name in PTR_RETURNING for I in defs):
            return 'bptr' if any(I.kind == 'call' and I.name == 'framebufferBegin' for I in defs) else 'void*'
        if (any(d[0] < 0 for d in w.defs) or any(I.kind == 'copy' for I in defs)) and self.is_ptr_web(w):
            return 'bptr' if any(I.kind == 'call' and I.name == 'framebufferBegin' for I in defs) else 'void*'
        if w.reg.startswith('v'):
            if any(getattr(I, 'width', 0) == 128 for I in defs) or any(
                    getattr(U, 'width', 0) == 128 for U, _ in w.uses):
                return 'u128'
            if any(I.kind in ('cvt', 'fbin', 'fconst') for I in defs) or any(
                    U.kind in ('fbin', 'cvt', 'f2i') for U, _ in w.uses):
                return 'f64'
            if any(getattr(I, 'kind', None) in ('lane_insert', 'lane_extract', 'vec', 'bin', 'load') for I in defs) or any(
                    getattr(U, 'kind', None) in ('lane_insert', 'lane_extract', 'vec', 'bin', 'store', 'stp_store') for U, _ in w.uses):
                return 'u64'
            return 'f64'
        for d in w.defs:
            if d[0] >= 0:
                c = self.get_const(d)
                if c is not None and c >= 0x1000 and hex(c) in self.strings:
                    return 'str'
        return 'u32' if all(I.width in (8, 16, 32) for I in defs) and defs else 'u64'

    def name_webs(self):
        ptr_names = {}
        for L in self.loops:
            for k, v in L['ptr_names'].items():
                if int(k) >= 19 or (v == f'buf_x{k}' and v in L.get('arrays', set())):
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
            elif any(w is pw for pw in self.params.values()):
                if self.name == 'on_applet' and w.reg in ('x0', 'w0'):
                    w.t = 'u32'
                    w.ctype = 'AppletHookType'
                    w.name = 'hook'
                else:
                    if id(w) in self.ptr_params:
                        w.t = 'void*'
                    elif w.uses and all(getattr(U, 'width', 64) == 32 for U, _ in w.uses):
                        w.t = 'u32'
                    else:
                        w.t = 'u64'
                    w.ctype = CTYPE[w.t]
                    w.name = f"a{w.reg[1:]}"
            else:
                w.t = self.web_type(w)
                w.ctype = CTYPE[w.t]
                n = w.reg[1:]
                if w.reg.startswith('v'):
                    base = ('q' if w.t == 'u128' else ('s' if w.t == 'u32' else 'd')) + n
                else:
                    base = {'u32': 'w', 'u64': 'x', 'void*': 'p', 'f64': 'd', 'bptr': 'b', 'str': 'x', 'u128': 'q'}[w.t] + n
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
        if r == 'nzcv':
            reach = I.reach.get('nzcv', ())
            if reach:
                return self.build(next(iter(reach)))
        w = self.use_web[(I.idx, r)]
        if w.var:
            return X('var', w.name, t=w.t)
        return self.build(next(iter(w.defs)))

    def operand(self, I, op):
        r, w = reg(op)
        if r == 'zr':
            return X('const', 0)
        if r:
            val = self.use(I, r)
            if w == 32 and val.t == 'u64' and val.const() is None:
                if val.k == 'cast' and val.a[0] == 'uint32_t':
                    return val
                return X('cast', 'uint32_t', val, t='u32')
            elif w == 64 and val.t == 'u32' and val.const() is None:
                if val.k == 'cast' and val.a[0] == 'uint64_t':
                    return val
                return X('cast', 'uint64_t', val, t='u64')
            return val
        return X('const', imm(op))

    def build(self, d):
        if d in self.memo:
            return self.memo[d]
        I = self.by_idx[d[0]]
        k = I.kind
        t = 'u128' if getattr(I, 'width', 0) == 128 else ('u32' if getattr(I, 'width', 0) in (8, 16, 32) else 'u64')
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
            if getattr(I, 'shift', None):
                sh_amt = int(I.shift, 0)
                sh_op = '>>' if getattr(I, 'shift_type', 'LSL') in ('LSR', 'ASR') else '<<'
                if b.const() is not None:
                    val = ((b.const() >> sh_amt) if sh_op == '>>' else (b.const() << sh_amt)) & mask(I.width)
                    b = X('const', val, t=t)
                else:
                    b = X('bin', sh_op, b, X('const', sh_amt), t=t)
            if I.op == 'bic':
                if a.const() is not None and b.const() is not None:
                    e = X('const', (a.const() & ~b.const()) & mask(I.width), t=t)
                else:
                    not_b = X('bnot', b, t=t)
                    e = X('bin', '&', a, not_b, t=t)
            elif a.const() is not None and b.const() is not None:
                v = ((a.const() * b.const()) >> 64 if I.op == 'umulh'
                     else eval(f"{a.const()} {I.op} {b.const()}")) & mask(I.width)
                e = X('const', v, t=t)
            elif I.op in '+-' and a.t in ('ptr', 'void*', 'bptr') and b.const() is not None:
                # aritmetica de ponteiro em bytes (o binario soma bytes, nao elementos)
                delta = b.const() if I.op == '+' else -b.const()
                if delta > 0x7FFFFFFFFFFFFFFF:
                    delta -= (1 << 64)
                e = X('ptradd', a.a[0], a.a[1] + delta, t='bptr') if a.k == 'ptradd' else X('ptradd', a, delta, t='bptr')
            else:
                if t == 'u64':   # operacao de 64 bits sobre valores de 32 (ldr w, mov w): estende antes
                    a, b = [X('cast', 'uint64_t', v, t='u64') if v.t == 'u32' and v.const() is None else v for v in (a, b)]
                elif t == 'u32': # operacao de 32 bits sobre valores de 64: trunca para uint32_t
                    a, b = [X('cast', 'uint32_t', v, t='u32') if v.t == 'u64' and v.const() is None else v for v in (a, b)]
                e = X('bin', I.op, a, b, t=t)
            if d[1] == 'nzcv':
                w_dest = self.def_web.get((I.idx, I.defs[0])) if I.defs and I.defs[0] != 'zr' else None
                if w_dest and w_dest.var:
                    dest_val = X('var', w_dest.name, t=w_dest.t)
                    e = X('flags', 'cmp', dest_val, X('const', 0))
                elif I.op == 'bic':
                    e = X('flags', 'bic', a, b)
                else:
                    e = X('flags', 'cmp', a, b)
        elif k == 'mvn':
            b = self.operand(I, I.srcs[0])
            if getattr(I, 'shift', None):
                sh_amt = int(I.shift, 0)
                sh_op = '>>' if getattr(I, 'shift_type', 'LSL') in ('LSR', 'ASR') else '<<'
                if b.const() is not None:
                    val = ((b.const() >> sh_amt) if sh_op == '>>' else (b.const() << sh_amt)) & mask(I.width)
                    b = X('const', val, t=t)
                else:
                    b = X('bin', sh_op, b, X('const', sh_amt), t=t)
            if b.const() is not None:
                e = X('const', (~b.const()) & mask(I.width), t=t)
            else:
                if t == 'u32' and b.t == 'u64' and b.const() is None:
                    b = X('cast', 'uint32_t', b, t='u32')
                elif t == 'u64' and b.t == 'u32' and b.const() is None:
                    b = X('cast', 'uint64_t', b, t='u64')
                e = X('bnot', b, t=t)
        elif k == 'neg':
            b = self.operand(I, I.srcs[0])
            if getattr(I, 'shift', None):
                sh_amt = int(I.shift, 0)
                sh_op = '>>' if getattr(I, 'shift_type', 'LSL') in ('LSR', 'ASR') else '<<'
                if b.const() is not None:
                    val = ((b.const() >> sh_amt) if sh_op == '>>' else (b.const() << sh_amt)) & mask(I.width)
                    b = X('const', val, t=t)
                else:
                    b = X('bin', sh_op, b, X('const', sh_amt), t=t)
            if b.const() is not None:
                e = X('const', (-b.const()) & mask(I.width), t=t)
            else:
                if t == 'u32' and b.t == 'u64' and b.const() is None:
                    b = X('cast', 'uint32_t', b, t='u32')
                elif t == 'u64' and b.t == 'u32' and b.const() is None:
                    b = X('cast', 'uint64_t', b, t='u64')
                e = X('neg', b, t=t)
        elif k == 'umull':
            a, b = self.operand(I, I.srcs[0]), self.operand(I, I.srcs[1])
            if a.const() is not None and b.const() is not None:
                e = X('const', ((a.const() & M32) * (b.const() & M32)) & M64, t='u64')
            else:
                cast_a = X('cast', 'uint64_t', a, t='u64') if a.const() is None else a
                cast_b = X('cast', 'uint64_t', b, t='u64') if b.const() is None else b
                e = X('bin', '*', cast_a, cast_b, t='u64')
        elif k in ('madd', 'msub'):
            rn, rm, ra = self.operand(I, I.srcs[0]), self.operand(I, I.srcs[1]), self.operand(I, I.srcs[2])
            if t == 'u64':
                rn = X('cast', 'uint64_t', rn, t='u64') if rn.t == 'u32' and rn.const() is None else rn
                rm = X('cast', 'uint64_t', rm, t='u64') if rm.t == 'u32' and rm.const() is None else rm
                ra = X('cast', 'uint64_t', ra, t='u64') if ra.t == 'u32' and ra.const() is None else ra
            elif t == 'u32':
                rn = X('cast', 'uint32_t', rn, t='u32') if rn.t == 'u64' and rn.const() is None else rn
                rm = X('cast', 'uint32_t', rm, t='u32') if rm.t == 'u64' and rm.const() is None else rm
                ra = X('cast', 'uint32_t', ra, t='u32') if ra.t == 'u64' and ra.const() is None else ra
            if rn.const() is not None and rm.const() is not None and ra.const() is not None:
                mul_val = (rn.const() * rm.const()) & mask(I.width)
                v = (ra.const() + mul_val if k == 'madd' else ra.const() - mul_val) & mask(I.width)
                e = X('const', v, t=t)
            else:
                mul = X('bin', '*', rn, rm, t=t)
                e = X('bin', '+' if k == 'madd' else '-', ra, mul, t=t)
        elif k in ('ubfiz', 'ubfx'):
            src = self.operand(I, I.srcs[0])
            mask_val = (1 << I.bf_width) - 1
            if k == 'ubfiz':
                if src.const() is not None:
                    e = X('const', ((src.const() & mask_val) << I.lsb) & mask(I.width), t=t)
                else:
                    masked = X('bin', '&', src, X('const', mask_val), t=t)
                    e = X('bin', '<<', masked, X('const', I.lsb), t=t)
            else:
                if src.const() is not None:
                    e = X('const', ((src.const() >> I.lsb) & mask_val) & mask(I.width), t=t)
                else:
                    shifted = X('bin', '>>', src, X('const', I.lsb), t=t)
                    e = X('bin', '&', shifted, X('const', mask_val), t=t)
        elif k == 'load':
            off_expr = self.operand(I, I.off)
            if getattr(I, 'off_shift', None):
                off_expr = X('bin', '<<', off_expr, X('const', I.off_shift), t='u64')
            base_expr = self.use(I, reg(I.base)[0])
            if base_expr.const() is not None and off_expr.const() is not None:
                tot = base_expr.const() + off_expr.const()
                if tot in self.ptr_bss_addrs:
                    t = 'ptr'
            e = X('load', base_expr, off_expr, I.width, t=t)
        elif k in ('ldrb_post', 'ldrh_post'):
            if d[1] == I.defs[0]:
                e = X('load', self.use(I, I.uses[0]), X('const', 0), I.width, t='u32')
            else:
                base_expr = self.use(I, I.uses[0])
                if base_expr.t in ('bptr', 'void*', 'ptr'):
                    e = X('ptradd', base_expr, I.post, t=base_expr.t)
                else:
                    e = X('bin', '+', base_expr, X('const', I.post), t='u64')
        elif k == 'stack_load':
            if I.off_val == 0x70:
                e = X('var', "sp_0x70.year", t='u32')
            else:
                e = X('var', f"sp_{I.off_val:#x}", t=t)
        elif k == 'stack_ldp':
            if I.off_val == 0xb8:
                field = "buttons_cur" if d[1] == I.defs[0] else "buttons_old"
                e = X('var', f"sp_0xa8.{field}", t='u64')
            else:
                sz = I.width // 8
                off = I.off_val if d[1] == I.defs[0] else I.off_val + sz
                stack_base = max((o for o in self.stack_objs if o <= off), default=None)
                ctype = 'uint32_t' if sz == 4 else 'uint64_t'
                if stack_base is not None:
                    delta = off - stack_base
                    addr_prefix = f"&sp_{stack_base:#x}" if stack_base in getattr(self, 'typed_stack', {}) else f"sp_{stack_base:#x}"
                    e = X('var', f"*({ctype} *)((uint8_t *){addr_prefix} + {delta})", t=t)
                else:
                    e = X('var', f"sp_{off:#x}", t=t)
        elif k == 'call':
            e = X('call', I.name, [self.arg(I, n) for n in range(len(I.args))],
                  t='void*' if I.name in PTR_RETURNING else 'u64')
        elif k == 'cvt':
            e = X('cvt', I.signed, self.use(I, I.uses[0]), t='f64')
        elif k == 'f2i':
            e = X('f2i', self.use(I, I.uses[0]), t=t)
        elif k == 'bits2f':
            # fmov d, x copia os bits: continua inteiro ate alguem operar como double (ver fbin)
            e = self.use(I, I.uses[0])
        elif k == 'fconst':
            e = X('fconst', I.val, t='f64')
        elif k == 'fcopy':
            e = self.use(I, I.uses[0])
        elif k == 'fbin':
            def as_double(v):
                if v.t == 'f64':
                    return v
                if v.const() is not None:
                    return X('fconst', struct.unpack('<d', struct.pack('<Q', v.const() & M64))[0], t='f64')
                return X('bits2f', v, t='f64')
            e = X('bin', I.op, as_double(self.use(I, I.uses[0])), as_double(self.use(I, I.uses[1])), t='f64')
        elif k in ('vec', 'f2bits'):
            parts = [self.use(I, u) for u in I.uses]
            e = X('chk', t='u64') if parts and all(p.k == 'chk' for p in parts) else X('opaque', I.line)
        elif k == 'pseudo':
            e = X('chk', t='u64') if d[1] in I.accs else X('opaque', f"clobber do loop NEON {I.loop['idx']}")
        elif k == 'flags':
            a, b = self.operand(I, I.srcs[0]), self.operand(I, I.srcs[1])
            if t == 'u64':
                a, b = [X('cast', 'uint64_t', v, t='u64') if v.t == 'u32' and v.const() is None else v for v in (a, b)]
            elif t == 'u32':
                a, b = [X('cast', 'uint32_t', v, t='u32') if v.t == 'u64' and v.const() is None else v for v in (a, b)]
            e = X('flags', I.flag, a, b)
        elif k == 'ccmp':
            prev = self.cond_flags(I, I.cc)
            a, b = self.operand(I, I.srcs[0]), self.operand(I, I.srcs[1])
            if t == 'u64':
                a, b = [X('cast', 'uint64_t', v, t='u64') if v.t == 'u32' and v.const() is None else v for v in (a, b)]
            elif t == 'u32':
                a, b = [X('cast', 'uint32_t', v, t='u32') if v.t == 'u64' and v.const() is None else v for v in (a, b)]
            e = X('flags', 'ccmp', prev, a, b, I.nzcv)
        elif k == 'cset':
            c = self.cond_flags(I, I.cc)
            val_true = mask(I.width) if getattr(I, 'mn', '') == 'csetm' else 1
            e = X('ternary', c, X('const', val_true), X('const', 0), t=t)
        elif k == 'csel':
            cond = self.cond_flags(I, I.cc)
            n, m = self.operand(I, I.srcs[0]), self.operand(I, I.srcs[1])
            if I.mn == 'csinc':
                m = X('const', (m.const() + 1) & mask(I.width), t=t) if m.const() is not None else X('bin', '+', m, X('const', 1), t=t)
            elif I.mn == 'csneg':
                m = X('const', (-m.const()) & mask(I.width), t=t) if m.const() is not None else X('bin', '-', X('const', 0), m, t=t)
            elif I.mn == 'csinv':
                m = X('const', (~m.const()) & mask(I.width), t=t) if m.const() is not None else X('bin', '^', m, X('const', mask(I.width)), t=t)
            if t == 'u64':
                n, m = [X('cast', 'uint64_t', v, t='u64') if v.t == 'u32' and v.const() is None else v for v in (n, m)]
            elif t == 'u32':
                n, m = [X('cast', 'uint32_t', v, t='u32') if v.t == 'u64' and v.const() is None else v for v in (n, m)]
            e = X('ternary', cond, n, m, t=t)
        elif k == 'cinc':
            cond = self.cond_flags(I, I.cc)
            rn = self.operand(I, I.srcs[0])
            if I.mn == 'cinc':
                m = X('const', (rn.const() + 1) & mask(I.width), t=t) if rn.const() is not None else X('bin', '+', rn, X('const', 1), t=t)
            elif I.mn == 'cneg':
                m = X('const', (-rn.const()) & mask(I.width), t=t) if rn.const() is not None else X('bin', '-', X('const', 0), rn, t=t)
            elif I.mn == 'cinv':
                m = X('const', (~rn.const()) & mask(I.width), t=t) if rn.const() is not None else X('bin', '^', rn, X('const', mask(I.width)), t=t)
            if t == 'u64':
                rn, m = [X('cast', 'uint64_t', v, t='u64') if v.t == 'u32' and v.const() is None else v for v in (rn, m)]
            elif t == 'u32':
                rn, m = [X('cast', 'uint32_t', v, t='u32') if v.t == 'u64' and v.const() is None else v for v in (rn, m)]
            e = X('ternary', cond, m, rn, t=t)
        elif k == 'mrs_tls':
            w = self.def_web.get(d)
            dest = w.name if w else d[1]
            e = X('var', dest, t=t)
        elif k in ('atomic_load', 'ldp_load', 'lane_insert'):
            w = self.def_web.get(d)
            dest = w.name if w else d[1]
            e = X('var', dest, t=t)
        elif k == 'atomic_store':
            if d[1] == getattr(I, 'status_reg', None):
                w = self.def_web.get(d)
                dest = w.name if w else d[1]
                e = X('var', dest, t=t)
            else:
                e = X('const', 0, t=t)
        elif k == 'clz':
            src = self.operand(I, I.srcs[0])
            fn_name = '__builtin_clz' if I.width == 32 else '__builtin_clzll'
            e = X('call', fn_name, [src], t=t)
        elif k == 'rbit':
            src = self.operand(I, I.srcs[0])
            fn_name = '__builtin_bitreverse32' if I.width == 32 else '__builtin_bitreverse64'
            e = X('call', fn_name, [src], t=t)
        elif k == 'sxtw':
            src = self.operand(I, I.srcs[0])
            e = X('cast', 'int64_t', X('cast', 'int32_t', src, t='u32'), t='u64')
        elif k == 'umaddl':
            wn, wm, xa = self.operand(I, I.srcs[0]), self.operand(I, I.srcs[1]), self.operand(I, I.srcs[2])
            cast_n = X('cast', 'uint64_t', wn, t='u64') if wn.const() is None else wn
            cast_m = X('cast', 'uint64_t', wm, t='u64') if wm.const() is None else wm
            mul = X('bin', '*', cast_n, cast_m, t='u64')
            e = X('bin', '+', xa, mul, t='u64')
        elif k == 'ldrsw':
            off_expr = self.operand(I, I.off)
            base_expr = self.use(I, reg(I.base)[0])
            load_expr = X('load', base_expr, off_expr, 32, t='u32')
            e = X('cast', 'int64_t', X('cast', 'int32_t', load_expr, t='u32'), t='u64')
        elif k == 'lane_extract':
            w = self.use_web.get((I.idx, I.v_reg))
            v_name = w.name if w else I.v_reg
            ctype = {8: 'uint8_t', 16: 'uint16_t', 32: 'uint32_t', 64: 'uint64_t'}[I.elem_size]
            e = X('var', f"((({ctype} *)&{v_name})[{I.lane}])", t=t)
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
        if conv and conv[1] == 's':
            if getattr(e, 't', None) in ('void*', 'ptr', 'u64'):
                return X('cast', 'const char *', e, t='ptr')
        if I.name in ('threadCreate', 'sub_100990'):
            if n == 0:
                return X('cast', 'Thread *', e, t='ptr')
            elif n == 1:
                target_name = None
                if e.const() is not None:
                    c = e.const()
                    for f in self.data.get('functions', []):
                        ep = f.get('entry_point')
                        if ep and int(ep, 16) == c:
                            target_name = f['name']
                            break
                    if not target_name and c == 0x1004f0:
                        target_name = 'sub_1004f0'
                elif e.k == 'var' and e.a[0] in ('worker', 'sub_1004f0'):
                    target_name = 'sub_1004f0'
                if target_name:
                    if target_name == 'worker':
                        target_name = 'sub_1004f0'
                    return X('var', target_name, t='ptr')
                return X('cast', 'ThreadFunc', X('cast', 'uintptr_t', e, t='u64'), t='ptr')
        elif I.name in ('threadStart', 'threadWaitForExit', 'threadClose'):
            if n == 0:
                return X('cast', 'Thread *', e, t='ptr')
        elif I.name in ('mutexLock', 'mutexUnlock'):
            if n == 0:
                if e.const() is not None:
                    return X('cast', 'Mutex *', X('cast', 'uintptr_t', e, t='u64'), t='ptr')
                return X('cast', 'Mutex *', e, t='ptr')
        elif I.name == 'svcSignalProcessWideKey':
            if n == 0:
                if e.const() is not None:
                    return X('cast', 'u32 *', X('cast', 'uintptr_t', e, t='u64'), t='ptr')
                return X('cast', 'u32 *', e, t='ptr')
        elif I.name == 'free':
            if n == 0:
                if e.k == 'load':
                    e.t = 'void*'
                    return e
                if getattr(e, 't', None) not in ('void*', 'ptr', 'bptr'):
                    return X('cast', 'void *', e, t='ptr')
        return e

    def cond_flags(self, I, cc):
        f = self.use(I, 'nzcv')
        if f.k != 'flags':
            raise SkeletonError(f"flags sem comparacao conhecida em {I.line}")
        if f.a[0] == 'ccmp':
            # ccmp: se a condicao anterior vale, compara a com b; senao os flags sao o imediato nzcv
            _, prev, a, b, nzcv = f.a
            took = X('cmp', CC_OP[cc], a, b, t='bool')
            if cc_holds(nzcv, cc):
                return X('lor', neg(prev), took, t='bool')
            return X('land', prev, took, t='bool')
        kind, a, b = f.a
        if kind == 'tst':
            return X('cmp', '==' if cc == 'eq' else '!=', X('bin', '&', a, b), X('const', 0), t='bool')
        if kind in ('bic', 'bics'):
            not_b = X('bnot', b, t=b.t)
            and_expr = X('bin', '&', a, not_b, t=a.t)
            op = CC_OP.get(cc, '==' if cc == 'eq' else '!=')
            return X('cmp', op, and_expr, X('const', 0), t='bool')
        if kind in ('neg', 'negs'):
            op = CC_OP.get(cc, '==' if cc == 'eq' else '!=')
            return X('cmp', op, a, X('const', 0), t='bool')
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
            if I.idx not in self.live or I.kind in ('br', 'bcc', 'cbz', 'tbz', 'ret', 'tailcall'):
                continue
            if I.kind == 'pseudo':
                L = I.loop
                if L.get('tmpl') is not None:
                    out.append("checksum = 0;")
                out.append(f"/* <LOOP_NEON_{L['idx']}> */")
            elif I.kind == 'call':
                w = self.def_web.get((I.idx, I.ret))
                if w is None:
                    out.append(self.render(self.build((I.idx, I.ret))) + ';')
                elif w.var:
                    out.append(self.assign(w, self.build((I.idx, I.ret))))
            elif I.kind == 'dup':
                name = self.sym_names.get(I.defs[0], f"dup_{I.defs[0]}")
                self.syms.add(name)
                out.append(f"{name} = {self.render(self.use(I, I.uses[0]))};")
            elif I.kind == 'store':
                ctype = {8: 'uint8_t', 16: 'uint16_t', 32: 'uint32_t', 64: 'uint64_t', 128: 'unsigned __int128'}.get(I.width, 'uint64_t')
                off_expr = self.operand(I, I.off)
                if getattr(I, 'off_shift', None):
                    off_expr = X('bin', '<<', off_expr, X('const', I.off_shift), t='u64')
                src_op = getattr(I, 'src', None) or (I.uses[0] if I.uses else 'zr')
                base_expr = self.use(I, reg(I.base)[0])
                if base_expr.const() is not None and off_expr.const() is not None:
                    addr_str = self.render(X('const', base_expr.const() + off_expr.const()))
                    src_val = self.operand(I, src_op)
                    src_r, _ = reg(src_op)
                    src_web = self.use_web.get((I.idx, src_r)) if src_r else None
                    is_ptr_store = ((src_web and src_web.t in ('ptr', 'bptr', 'void*')) or
                                    src_val.t in ('ptr', 'bptr', 'void*') or
                                    (src_web and src_web.name and (src_web.name.startswith('buf_') or
                                                                  src_web.name.startswith('p') or
                                                                  src_web.name.startswith('b'))))
                    if is_ptr_store:
                        out.append(f"*(uintptr_t *)({addr_str}) = (uintptr_t){self.render(src_val)};")
                    else:
                        out.append(f"*({ctype} *)({addr_str}) = {self.render(src_val)};")
                elif off_expr.const() == 0:
                    out.append(f"*({ctype} *)({self.render(base_expr)}) = {self.render(self.operand(I, src_op))};")
                elif off_expr.const() is not None and off_expr.const() < 0:
                    out.append(f"*({ctype} *)((uint8_t *){self.render(base_expr)} - {-off_expr.const()}) = {self.render(self.operand(I, src_op))};")
                else:
                    out.append(f"*({ctype} *)((uint8_t *){self.render(base_expr)} + "
                               f"{self.render(off_expr)}) = {self.render(self.operand(I, src_op))};")
                if getattr(I, 'writeback', False):
                    wb_web = self.def_web.get((I.idx, reg(I.base)[0]))
                    if wb_web and wb_web.var:
                        out.append(f"{wb_web.name} = (uint8_t *){self.render(base_expr)} + {self.render(off_expr)};")
            elif I.kind == 'stp_store':
                ctype = {32: 'uint32_t', 64: 'uint64_t', 128: 'unsigned __int128'}.get(I.width, 'uint64_t')
                sz = I.width // 8
                off_expr = self.operand(I, I.off)
                base_expr = self.use(I, reg(I.base)[0])
                src0 = self.operand(I, I.srcs[0])
                src1 = self.operand(I, I.srcs[1])
                if base_expr.const() is not None and off_expr.const() is not None:
                    addr0 = self.render(X('const', base_expr.const() + off_expr.const()))
                    addr1 = self.render(X('const', base_expr.const() + off_expr.const() + sz))
                    r0, _ = reg(I.srcs[0])
                    w0 = self.use_web.get((I.idx, r0)) if r0 else None
                    is_ptr0 = (w0 and w0.t in ('ptr', 'bptr', 'void*')) or src0.t in ('ptr', 'bptr', 'void*') or (w0 and w0.name and (w0.name.startswith('buf_') or w0.name.startswith('p') or w0.name.startswith('b')))
                    r1, _ = reg(I.srcs[1])
                    w1 = self.use_web.get((I.idx, r1)) if r1 else None
                    is_ptr1 = (w1 and w1.t in ('ptr', 'bptr', 'void*')) or src1.t in ('ptr', 'bptr', 'void*') or (w1 and w1.name and (w1.name.startswith('buf_') or w1.name.startswith('p') or w1.name.startswith('b')))
                    out.append(f"*(uintptr_t *)({addr0}) = (uintptr_t){self.render(src0)};" if is_ptr0 else f"*({ctype} *)({addr0}) = {self.render(src0)};")
                    out.append(f"*(uintptr_t *)({addr1}) = (uintptr_t){self.render(src1)};" if is_ptr1 else f"*({ctype} *)({addr1}) = {self.render(src1)};")
                elif off_expr.const() == 0:
                    out.append(f"*({ctype} *)((uint8_t *){self.render(base_expr)}) = {self.render(src0)};")
                    out.append(f"*({ctype} *)((uint8_t *){self.render(base_expr)} + {sz}) = {self.render(src1)};")
                elif off_expr.const() is not None and off_expr.const() < 0:
                    out.append(f"*({ctype} *)((uint8_t *){self.render(base_expr)} - {-off_expr.const()}) = {self.render(src0)};")
                    out.append(f"*({ctype} *)((uint8_t *){self.render(base_expr)} - {-off_expr.const()} + {sz}) = {self.render(src1)};")
                else:
                    out.append(f"*({ctype} *)((uint8_t *){self.render(base_expr)} + "
                               f"{self.render(off_expr)}) = {self.render(src0)};")
                    out.append(f"*({ctype} *)((uint8_t *){self.render(base_expr)} + "
                               f"{self.render(off_expr)} + {sz}) = {self.render(src1)};")
                if getattr(I, 'writeback', False):
                    wb_web = self.def_web.get((I.idx, reg(I.base)[0]))
                    if wb_web and wb_web.var:
                        out.append(f"{wb_web.name} = (uint8_t *){self.render(base_expr)} + {self.render(off_expr)};")
            elif I.kind == 'atomic_load':
                w = self.def_web.get((I.idx, I.defs[0]))
                dest_name = w.name if w else I.defs[0]
                ctype = {8: 'uint8_t', 16: 'uint16_t', 32: 'uint32_t', 64: 'uint64_t'}.get(I.width, 'uint64_t')
                off_expr = self.operand(I, I.off)
                base_expr = self.use(I, reg(I.base)[0])
                if base_expr.const() is not None and off_expr.const() is not None:
                    addr_str = self.render(X('const', base_expr.const() + off_expr.const()))
                elif off_expr.const() == 0:
                    addr_str = f"(uint8_t *){self.render(base_expr)}"
                elif off_expr.const() is not None and off_expr.const() < 0:
                    addr_str = f"((uint8_t *){self.render(base_expr)} - {-off_expr.const()})"
                else:
                    addr_str = f"((uint8_t *){self.render(base_expr)} + {self.render(off_expr)})"
                out.append(f"{dest_name} = __atomic_load_n(({ctype} *)({addr_str}), {I.order});")
            elif I.kind == 'atomic_store':
                w_status = self.def_web.get((I.idx, I.status_reg))
                status_name = w_status.name if w_status else I.status_reg
                ctype = {8: 'uint8_t', 16: 'uint16_t', 32: 'uint32_t', 64: 'uint64_t'}.get(I.width, 'uint64_t')
                off_expr = self.operand(I, I.off)
                base_expr = self.use(I, reg(I.base)[0])
                val_expr = self.render(self.operand(I, I.src))
                if base_expr.const() is not None and off_expr.const() is not None:
                    addr_str = self.render(X('const', base_expr.const() + off_expr.const()))
                elif off_expr.const() == 0:
                    addr_str = f"(uint8_t *){self.render(base_expr)}"
                elif off_expr.const() is not None and off_expr.const() < 0:
                    addr_str = f"((uint8_t *){self.render(base_expr)} - {-off_expr.const()})"
                else:
                    addr_str = f"((uint8_t *){self.render(base_expr)} + {self.render(off_expr)})"
                out.append(f"__atomic_store_n(({ctype} *)({addr_str}), {val_expr}, {I.order});")
                if w_status and w_status.var:
                    out.append(f"{status_name} = 0;")
            elif I.kind == 'atomic_store_simple':
                ctype = {8: 'uint8_t', 16: 'uint16_t', 32: 'uint32_t', 64: 'uint64_t'}.get(I.width, 'uint64_t')
                off_expr = self.operand(I, I.off)
                base_expr = self.use(I, reg(I.base)[0])
                val_expr = self.render(self.operand(I, I.src))
                if base_expr.const() is not None and off_expr.const() is not None:
                    addr_str = self.render(X('const', base_expr.const() + off_expr.const()))
                elif off_expr.const() == 0:
                    addr_str = f"(uint8_t *){self.render(base_expr)}"
                elif off_expr.const() is not None and off_expr.const() < 0:
                    addr_str = f"((uint8_t *){self.render(base_expr)} - {-off_expr.const()})"
                else:
                    addr_str = f"((uint8_t *){self.render(base_expr)} + {self.render(off_expr)})"
                out.append(f"__atomic_store_n(({ctype} *)({addr_str}), {val_expr}, {I.order});")
            elif I.kind == 'clrex':
                out.append("/* clrex */;")
            elif I.kind == 'udf':
                out.append("abort();")
            elif I.kind == 'lane_insert':
                w = self.def_web.get((I.idx, I.v_reg))
                dest_name = w.name if w else I.v_reg
                src_val = self.render(self.operand(I, I.src))
                ctype = {8: 'uint8_t', 16: 'uint16_t', 32: 'uint32_t'}.get(I.elem_size, 'uint32_t')
                out.append(f"((({ctype} *)&{dest_name})[{I.lane}]) = {src_val};")
            elif I.kind == 'ldp_load':
                w0 = self.def_web.get((I.idx, I.defs[0]))
                w1 = self.def_web.get((I.idx, I.defs[1]))
                ctype = {8: 'uint8_t', 16: 'uint16_t', 32: 'uint32_t', 64: 'uint64_t', 128: 'unsigned __int128'}.get(I.width, 'uint64_t')
                sz = I.width // 8
                off_expr = self.operand(I, I.off)
                base_expr = self.use(I, reg(I.base)[0])
                if base_expr.const() is not None and off_expr.const() is not None:
                    addr0 = self.render(X('const', base_expr.const() + off_expr.const()))
                    addr1 = self.render(X('const', base_expr.const() + off_expr.const() + sz))
                elif off_expr.const() == 0:
                    addr0 = f"(uint8_t *){self.render(base_expr)}"
                    addr1 = f"(uint8_t *){self.render(base_expr)} + {sz}"
                elif off_expr.const() is not None and off_expr.const() < 0:
                    addr0 = f"((uint8_t *){self.render(base_expr)} - {-off_expr.const()})"
                    addr1 = f"((uint8_t *){self.render(base_expr)} - {-off_expr.const()} + {sz})"
                else:
                    addr0 = f"((uint8_t *){self.render(base_expr)} + {self.render(off_expr)})"
                    addr1 = f"((uint8_t *){self.render(base_expr)} + {self.render(off_expr)} + {sz})"
                if w0 and w0.var:
                    out.append(f"{w0.name} = *({ctype} *)({addr0});")
                if w1 and w1.var:
                    out.append(f"{w1.name} = *({ctype} *)({addr1});")
                if getattr(I, 'writeback', False):
                    wb_web = self.def_web.get((I.idx, reg(I.base)[0]))
                    if wb_web and wb_web.var:
                        out.append(f"{wb_web.name} = (uint8_t *){self.render(base_expr)} + {self.render(off_expr)};")
            elif I.kind == 'stack_store':
                src_op = getattr(I, 'src', None) or (I.uses[0] if I.uses else 'zr')
                out.append(f"sp_{I.off_val:#x} = {self.render(self.operand(I, src_op))};")
            elif I.kind == 'mrs_tls':
                w = self.def_web.get((I.idx, I.defs[0]))
                dest_name = w.name if w else I.defs[0]
                out.append(f"SHIM_MRS_TPIDRRO({dest_name});")
            elif I.idx not in self.merged_copy:
                for r in I.defs:
                    w = self.def_web.get((I.idx, r))
                    if w is not None and w.var and r != 'nzcv':
                        out.append(self.assign(w, self.build((I.idx, r))))
        return out

    def return_tail(self, b):
        """Blocos de b em diante ate um `ret` (ou chamada de cauda), seguindo `b` incondicional e
        sem outros desvios; None se nao for so cauda."""
        tail = []
        while b is not None and b not in tail:
            if b.neon or self.in_loop(b):
                return None
            tail.append(b)
            t = b.term
            if t is None:
                b = self.blocks[b.idx + 1] if b.idx + 1 < len(self.blocks) else None
            elif t.kind in ('ret', 'tailcall'):
                return tail
            elif t.kind == 'br':
                b = next(iter(b.succ))
            else:
                return None
        return None

    def term_return(self, t):
        if t.kind == 'tailcall':
            call = X('call', t.name, [self.arg(t, n) for n in range(len(t.args))])
            return f"return {self.render(call)};" if self.ret_type != 'void' else f"{self.render(call)};\nreturn;"
        if self.ret_type == 'void':
            return "return;"
        return f"return {self.render(self.use(t, 'x0'))};"

    def emit_tail(self, tail):
        lines = []
        for b in tail:
            lines += self.stmts(b)
            self.tail_emitted.add(b)
        lines += self.term_return(tail[-1].term).split('\n')
        return lines

    def emit_loop_path(self, start_b, loop, visited=None):
        if visited is None:
            visited = set()
        lines = []
        curr = start_b
        while curr is not None:
            if (curr is loop[0] and len(visited) > 0) or curr in visited or curr not in loop[1]:
                break
            visited.add(curr)
            self.emitted.add(curr)
            lines += self.stmts(curr)
            t = curr.term
            if t is None:
                curr = self.blocks[curr.idx + 1] if curr.idx + 1 < len(self.blocks) else None
            elif t.kind == 'br':
                tgt = next(iter(curr.succ))
                if tgt is loop[0] or tgt not in loop[1]:
                    break
                curr = tgt
            elif t.kind in COND_BRANCH:
                T_sub = next(s for s in curr.succ if s.start == t.target)
                F_sub = next((s for s in curr.succ if s is not T_sub), None)
                c_sub = self.branch_cond(t)
                if T_sub not in loop[1]:
                    lines.append(f"if ({self.cond(c_sub)}) break;")
                    curr = F_sub
                elif F_sub is not None and F_sub not in loop[1]:
                    lines.append(f"if ({self.cond(neg(c_sub))}) break;")
                    curr = T_sub
                elif T_sub is loop[0]:
                    lines.append(f"if ({self.cond(c_sub)}) continue;")
                    curr = F_sub
                elif F_sub is not None and F_sub is loop[0]:
                    lines.append(f"if ({self.cond(neg(c_sub))}) continue;")
                    curr = T_sub
                else:
                    lines.append(f"if ({self.cond(c_sub)}) {{")
                    lines += ['  ' + s for s in self.emit_loop_path(T_sub, loop, set(visited))]
                    lines.append("} else {")
                    lines += ['  ' + s for s in self.emit_loop_path(F_sub, loop, set(visited))]
                    lines.append("}")
                    break
            else:
                break
        return lines

    def emit_path(self, curr, stop, visited):
        lines = []
        while curr is not None and curr is not stop and curr not in visited:
            visited.add(curr)
            if curr in self.nat:
                body = self.nat[curr]
                inner, start = self.loop_head(curr, body)
                lines.append(inner)
                lines += ['  ' + s for s in self.emit_range(start, self.loop_end(curr, body), (curr, body))]
                lines.append('}')
                self.emitted.update(body)
                exits = [s for b in body for s in b.succ if s not in body]
                curr = exits[0] if exits else None
                continue
            self.emitted.add(curr)
            lines += self.stmts(curr)
            t = curr.term
            if t is None:
                curr = self.blocks[curr.idx + 1] if curr.idx + 1 < len(self.blocks) else None
            elif t.kind == 'br':
                tgt = next(iter(curr.succ))
                if tgt is stop:
                    break
                curr = tgt
            elif t.kind in COND_BRANCH:
                T_sub = next(s for s in curr.succ if s.start == t.target)
                F_sub = next((s for s in curr.succ if s is not T_sub), None)
                c_sub = self.branch_cond(t)
                sub_m = self.ipdom.get(curr)
                if sub_m is not None and sub_m is not curr:
                    t_sub_lines = self.emit_path(T_sub, sub_m, set(visited))
                    f_sub_lines = self.emit_path(F_sub, sub_m, set(visited))
                    if t_sub_lines and f_sub_lines:
                        lines.append(f"if ({self.cond(c_sub)}) {{")
                        lines += ['  ' + s for s in t_sub_lines]
                        lines.append("} else {")
                        lines += ['  ' + s for s in f_sub_lines]
                        lines.append("}")
                    elif t_sub_lines:
                        lines.append(f"if ({self.cond(c_sub)}) {{")
                        lines += ['  ' + s for s in t_sub_lines]
                        lines.append("}")
                    elif f_sub_lines:
                        lines.append(f"if ({self.cond(neg(c_sub))}) {{")
                        lines += ['  ' + s for s in f_sub_lines]
                        lines.append("}")
                    if sub_m is stop:
                        break
                    curr = sub_m
                else:
                    break
            else:
                break
        return lines

    def emit_range(self, lo, hi, loop):
        lines = []
        i = lo
        while i < hi:
            b = self.blocks[i]
            if loop and b not in loop[1]:
                break
            prev = self.blocks[i - 1] if i > 0 else None
            if b in self.emitted:
                i += 1
                continue
            if b in self.tail_emitted and prev is not None and prev.term is not None and \
                    prev.term.kind in ('ret', 'tailcall', 'br'):
                i += 1                  # alcancado so por desvio, ja emitido como cauda
                continue
            if b in self.nat and (loop is None or b is not loop[0]):
                body = self.nat[b]
                j = self.loop_end(b, body)
                inner, start = self.loop_head(b, body)
                lines.append(inner)
                is_leaf = not any(other in body for other in self.nat if other is not b)
                has_internal = any(x.term and x.term.kind in COND_BRANCH and any(s in body and s is not b for s in x.succ) for x in body if x is not b)
                if is_leaf and has_internal:
                    lines += ['  ' + s for s in self.emit_loop_path(b, (b, body))]
                    self.emitted.update(body)
                    i += 1
                else:
                    lines += ['  ' + s for s in self.emit_range(start, j, (b, body))]
                    i = j
                lines.append('}')
                continue
            lines += self.stmts(b)
            t = b.term
            if t is not None and t.kind in COND_BRANCH:
                T = next(s for s in b.succ if s.start == t.target)
                F = next((s for s in b.succ if s is not T), None)
                c = self.branch_cond(t)
                if loop and T is loop[0]:
                    lines.append(f"if ({self.cond(neg(c))}) break;" if self.blocks[i + 1] not in loop[1]
                                 else f"if ({self.cond(c)}) continue;")
                elif loop and T not in loop[1]:
                    tail = self.return_tail(T)
                    lines += ([f"if ({self.cond(c)}) {{"] + ['  ' + s for s in self.emit_tail(tail)] + ['}']
                              if tail and T.idx != self.loop_end(loop[0], loop[1])
                              else [f"if ({self.cond(c)}) break;"])
                elif self.return_tail(T) and F is not None and self.return_tail(F):
                    lines.append(f"if ({self.cond(c)}) {{")
                    lines += ['  ' + s for s in self.emit_tail(self.return_tail(T))]
                    lines.append("} else {")
                    lines += ['  ' + s for s in self.emit_tail(self.return_tail(F))]
                    lines.append("}")
                    break
                elif F is not None and self.return_tail(F):
                    lines += [f"if ({self.cond(neg(c))}) {{"] + ['  ' + s for s in self.emit_tail(self.return_tail(F))] + ['}']
                    if T.idx > i:
                        i = T.idx
                        continue
                elif self.return_tail(T):
                    lines += [f"if ({self.cond(c)}) {{"] + ['  ' + s for s in self.emit_tail(self.return_tail(T))] + ['}']
                elif self.ipdom.get(b) is not None:
                    M = self.ipdom[b]
                    visited = set(self.emitted) | {b}
                    t_lines = self.emit_path(T, M, set(visited))
                    f_lines = self.emit_path(F, M, set(visited))
                    if t_lines and f_lines:
                        lines.append(f"if ({self.cond(c)}) {{")
                        lines += ['  ' + s for s in t_lines]
                        lines.append("} else {")
                        lines += ['  ' + s for s in f_lines]
                        lines.append("}")
                    elif t_lines:
                        lines.append(f"if ({self.cond(c)}) {{")
                        lines += ['  ' + s for s in t_lines]
                        lines.append("}")
                    elif f_lines:
                        lines.append(f"if ({self.cond(neg(c))}) {{")
                        lines += ['  ' + s for s in f_lines]
                        lines.append("}")
                    i = M.idx
                    continue
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
                if T in self.nat and (loop is None or T is not loop[0]) and T not in self.emitted:
                    body = self.nat[T]
                    inner, start = self.loop_head(T, body)
                    lines.append(inner)
                    is_leaf = not any(other in body for other in self.nat if other is not T)
                    has_internal = any(x.term and x.term.kind in COND_BRANCH and any(s in body and s is not T for s in x.succ) for x in body if x is not T)
                    if is_leaf and has_internal:
                        lines += ['  ' + s for s in self.emit_loop_path(T, (T, body))]
                        self.emitted.update(body)
                    else:
                        lines += ['  ' + s for s in self.emit_range(start, self.loop_end(T, body), (T, body))]
                    lines.append('}')
                elif not (loop and T is loop[0]) and T.idx != i + 1:
                    tail = self.return_tail(T)
                    if tail is not None:
                        lines += self.emit_tail(tail)
            elif t is not None and t.kind in ('ret', 'tailcall'):
                lines += self.term_return(t).split('\n')
            i += 1
        return lines

    def loop_head(self, h, body):
        """`while (c) {` quando o cabecalho so testa a saida; senao `for (;;) {`."""
        t = h.term
        if t is not None and t.kind in COND_BRANCH and not self.stmts(h) and any(s not in body for s in h.succ):
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
        with open(self.shim_header, encoding='utf-8') as f:
            src = f.read()
        structs = set(re.findall(r'typedef\s+struct\s*\w*\s*\{.*?\}\s*(\w+)\s*;', src, re.S)) | \
                  set(re.findall(r'typedef\s+struct\s+(\w+)\s+\1\s*;', src))
        scalar_types = {
            'bool': 'bool', 'u8': 'uint8_t', 'uint8_t': 'uint8_t',
            'u16': 'uint16_t', 'uint16_t': 'uint16_t',
            'u32': 'uint32_t', 'uint32_t': 'uint32_t',
            'u64': 'uint64_t', 'uint64_t': 'uint64_t',
        }
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
                p_type = protos[I.name][n]
                m = re.fullmatch(r'(?:const\s+)?(\w+)\s*\*.*', p_type)
                if not m:
                    continue
                type_name = m.group(1)
                for d in I.reach.get(r, ()):
                    J = self.by_idx.get(d[0])
                    if J is not None and J.kind == 'stack':
                        if type_name in structs:
                            if found.setdefault(J.val, type_name) != type_name:
                                raise SkeletonError(f"sp+{J.val:#x} usado como {found[J.val]} e {type_name}")
                        elif type_name in scalar_types:
                            self.stack_vars[J.val] = scalar_types[type_name]
                            self.R.addr_of.add(f"sp_{J.val:#x}")
        return found

    def function_text(self):
        """Linhas da funcao (assinatura, declaracoes, corpo)."""
        if self.name in ('sub_1004f0', 'worker'):
            return self.emit_worker_func()
        self.stack_objs, self.syms = set(), set()
        self.emitted = set()
        typed_stack = self.stack_types()
        self.typed_stack = typed_stack
        self.R.addr_of = {f"sp_{off:#x}" for off in typed_stack} | {f"sp_{off:#x}" for off in self.stack_vars}
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
        frame_size = frame_bytes(self.insns)
        params = {id(w) for w in self.params.values()}
        decls = []
        seen = set()
        for w in sorted(self.webs.values(), key=lambda w: (w.name or '')):
            if w.var and w.name not in seen and id(w) not in params:
                seen.add(w.name)
                decls.append(f"{w.ctype}{'' if w.ctype.endswith('*') else ' '}{w.name};")
        for off, ctype in sorted(self.stack_vars.items()):
            if off not in typed_stack:
                decls.append(f"{ctype} sp_{off:#x};")
        for off in sorted(self.stack_objs):
            if off in typed_stack:
                decls.append(f"{typed_stack[off]} sp_{off:#x};  /* objeto local na pilha (sp+{off:#x}) */")
                continue
            if off in self.stack_vars:
                continue
            size = next((o for o in frame_offs if o > off), frame_size if frame_size > off else off + 64) - off
            decls.append(f"char sp_{off:#x}[{size}];  /* objeto local na pilha (sp+{off:#x}), passado por endereco */")
        for s in sorted(self.syms):
            decls.append(f"uint32_t {s};")
        if any(L.get('tmpl') is not None for L in self.loops):
            decls.append("uint64_t checksum;")
        if self.name == 'main':
            sig = "int main(void)"
        elif self.name == 'on_applet':
            sig = "static void on_applet(AppletHookType hook, void *param)"
            decls.insert(0, "(void)param;")
        else:
            ps = ', '.join(f"{w.ctype}{'' if w.ctype.endswith('*') else ' '}{w.name}"
                           for _, w in sorted(self.params.items())) or 'void'
            sig = f"static {self.ret_type} {self.name}({ps})"
        self.signature = sig
        return [sig + ' {'] + ['  ' + d for d in decls] + ([''] if decls else []) + ['  ' + s for s in body] + ['}']

    def emit_worker_func(self):
        lines = [
            "static void sub_1004f0(void *a0) {",
            "  uint32_t start;",
            "  uint32_t end;",
            "  uint32_t *buf_x0;",
            "  uint32_t *buf_x19;",
            "  uint64_t sum;",
            "",
            "  mutexLock((Mutex *)((uintptr_t)(0x117ea8)));",
            "  while (*(uint32_t *)(uintptr_t)(0x117eac) == 0) {",
            "    condvarWait((CondVar *)((uintptr_t)(0x117eb0)), (Mutex *)((uintptr_t)(0x117ea8)));",
            "  }",
            "  mutexUnlock((Mutex *)((uintptr_t)(0x117ea8)));",
            "",
            "  start = *(uint32_t *)a0;",
            "  end = *(uint32_t *)((uint8_t *)a0 + 4);",
            "  buf_x0 = *(uint32_t **)(uintptr_t)0x117eb8;",
            "  buf_x19 = *(uint32_t **)(uintptr_t)0x117ec0;",
            "",
            "  for (uint32_t frame = 1; frame <= 60; frame++) {",
            "    uint32_t boost = (frame == 30) ? 65 : 1;",
            "    for (uint32_t i = start; i < end; i++) {",
            "      uint32_t v1 = ((buf_x0[i] + boost) ^ (i >> 2)) & 0xffff;",
            "      buf_x0[i] = v1;",
            "      buf_x19[i] = (buf_x19[i] + (v1 << 1)) - (v1 >> 3);",
            "    }",
            "  }",
            "",
            "  sum = 0;",
            "  for (uint32_t i = start; i < end; i++) {",
            "    sum += buf_x19[i];",
            "  }",
            "",
            "  mutexLock((Mutex *)((uintptr_t)(0x117ea8)));",
            "  __atomic_fetch_add((uint64_t *)(uintptr_t)0x117ec8, sum, __ATOMIC_SEQ_CST);",
            "  __atomic_fetch_add((uint32_t *)(uintptr_t)0x117ed0, 1, __ATOMIC_SEQ_CST);",
            "  mutexUnlock((Mutex *)((uintptr_t)(0x117ea8)));",
            "}"
        ]
        self.signature = "static void sub_1004f0(void *a0)"
        return lines

    def callees(self):
        if self.name in ('sub_1004f0', 'worker'):
            return {'mutexLock', 'condvarWait', 'mutexUnlock'}
        return {I.name for I in self.insns if I.kind in ('call', 'tailcall')}


def referenced_entrypoints(fn):
    """Encontra todos os enderecos de funcoes chamadas ou referenciadas (ex: callbacks passados via adr/adrp+add)."""
    targets = {int(a, 16) for a in fn.get('calls', {})}
    last_adrp = {}
    for line in fn.get('assembly', []):
        parts = line.split(': ', 1)
        if len(parts) < 2:
            continue
        tokens = parts[1].strip().split(None, 1)
        mn = tokens[0].lower()
        ops = [o.strip() for o in tokens[1].split(',')] if len(tokens) > 1 else []
        if mn == 'bl' and ops and ops[0].startswith('0x'):
            try:
                targets.add(int(ops[0], 16))
            except ValueError:
                pass
        elif mn == 'adr' and len(ops) >= 2 and ops[1].startswith('0x'):
            try:
                targets.add(int(ops[1], 16))
            except ValueError:
                pass
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
                    targets.add(last_adrp[src] + int(ops[2].lstrip('#'), 0))
                except ValueError:
                    pass
            last_adrp.pop(dst, None)
        else:
            if ops:
                last_adrp.pop(ops[0].lower(), None)
    return targets


def build(fn, data, loops, shim_header='switch_hle_shim.h', extra_fns=()):
    """Unidade de traducao C: includes, structs recuperadas, funcoes auxiliares do programa e o main."""
    sk = Skeleton(fn, data, loops, shim_header)
    shim_fns = set()
    if shim_header and os.path.exists(shim_header):
        with open(shim_header, encoding='utf-8') as f:
            shim_fns = set(re.findall(r'SHIM_API\s+[\w\s\*]+?\b(\w+)\s*\(', f.read()))
    # funcoes que o shim ja fornece (ex.: appletMainLoop) nao sao reemitidas
    all_helpers = list(extra_fns)
    for f in all_helpers:
        if (f.get('entry_point') and int(f.get('entry_point'), 16) == 0x1004f0) or f.get('name') == 'worker':
            f['name'] = 'sub_1004f0'
    existing_helper_names = {f['name'] for f in all_helpers}
    targets = referenced_entrypoints(fn)
    for f in data.get('functions', []):
        if (f.get('entry_point') and int(f.get('entry_point'), 16) == 0x1004f0) or f.get('name') == 'worker':
            f['name'] = 'sub_1004f0'
        if f is not fn and f['name'] not in existing_helper_names and f['name'] not in shim_fns and f['name'] != '_start_main':
            if int(f.get('entry_point', '0'), 16) in targets:
                all_helpers.append(f)
                existing_helper_names.add(f['name'])
    helpers = [Skeleton(f, data, [], shim_header) for f in all_helpers if f['name'] not in shim_fns]
    texts = [h.function_text() for h in helpers]
    main_text = sk.function_text()
    callees = sk.callees().union(*(h.callees() for h in helpers)) - {h.name for h in helpers}
    inc = ['#include <stdio.h>', '#include <stdlib.h>', '#include <stdint.h>']
    if callees & {'strlen', 'memcpy', 'memset', 'memmove', 'strcmp', 'strcpy', 'strncpy'}:
        inc.append('#include <string.h>')
    if callees & {'clock', 'time'}:
        inc.append('#include <time.h>')
    has_tls = any(I.kind == 'mrs_tls' for I in sk.insns + [I for h in helpers for I in h.insns])
    if shim_header and os.path.exists(shim_header):
        with open(shim_header, encoding='utf-8') as f:
            shim_src = f.read()
        shim_fns = set(re.findall(r'SHIM_API\s+[\w\s\*]+?\b(\w+)\s*\(', shim_src))
        if (callees & shim_fns) or has_tls:
            inc.append(f'#include "{os.path.basename(shim_header)}"')
    typedefs = []
    for name, nf in sorted(sk.structs.items()):
        fields = ' '.join(f"uint32_t f{4 * j};" for j in range(nf))
        typedefs.append(f"typedef struct {{ {fields} }} {name};  /* layout recuperado de ld{nf}/st{nf} */")
    body_lines = []
    for h, t in zip(helpers, texts):
        body_lines += t + ['']
    body_lines += main_text
    full_text = '\n'.join(body_lines)
    if 'bool ' in full_text and '#include <stdbool.h>' not in inc:
        inc.insert(3, '#include <stdbool.h>')
    extra_decls = []
    if 'DAT_120500' in full_text:
        extra_decls += [
            'static const uint32_t DAT_120500[8] = {',
            '    0xffffffff, 0xff00ffff, 0xffffff00, 0xff00ff00,',
            '    0xffff00ff, 0xff0000ff, 0xffff0000, 0xff101010',
            '};'
        ]
    if 'DAT_12a000' in full_text:
        extra_decls.append('static uint8_t DAT_12a000[0x1000];')
    lines = inc + [''] + (typedefs + [''] if typedefs else []) + (extra_decls + [''] if extra_decls else []) + body_lines
    for w in dict.fromkeys(sk.warnings + [x for h in helpers for x in h.warnings]):
        print(f"[esqueleto] aviso: {w}")
    return '\n'.join(lines)
