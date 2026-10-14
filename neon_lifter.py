"""neon_lifter.py - Lifter simbolico para loops NEON (AArch64) com suporte a Memoria/Arrays (ldp/stp/ldr q).

Uso:
  python neon_lifter.py ghidra_extraction_array.json --expected 12249998500128 --qwen reconstructed_array.c
"""
import argparse, json, re, sys, urllib.request

M32, M64 = 0xFFFFFFFF, 0xFFFFFFFFFFFFFFFF
BT3 = chr(96) * 3


class LiftError(Exception):
    pass


# ---------------------------------------------------------------- expressoes simbolicas
class E:
    """No de expressao. op: 'var' (i + c), 'mem' (arr, offset), 'const', ou operador ('+', '-', '*', '^', '>>', '<<')."""
    def __init__(self, op, *a):
        self.op, self.a = op, a

    def offsets(self):
        if self.op in ('var', 'mem'):
            return {self.a[-1]}
        if self.op == 'const':
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
        if self.op == 'const':
            return hex(self.a[0]) if self.a[0] >= 16 else str(self.a[0])
        l, r = self.a
        return f"({l.show(x, relative)} {self.op} {r.show(x, relative)})"

    def eval(self, x, mem_lookup=None):
        if self.op == 'var':
            return x & M32
        if self.op == 'mem':
            arr, _ = self.a
            return mem_lookup(arr, x) & M32
        if self.op == 'const':
            return self.a[0] & M32
        l = self.a[0].eval(x, mem_lookup)
        r = self.a[1].eval(x, mem_lookup)
        return {
            '+': lambda: (l + r) & M32,
            '-': lambda: (l - r) & M32,
            '*': lambda: (l * r) & M32,
            '^': lambda: l ^ r,
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
    # Simplificacao algebrica para (i * K + C1) -> agrupa avanco de lane
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
    'ushr': lambda a, n: E('>>', a, E('const', n)),
    'shl': lambda a, n: E('<<', a, E('const', n)),
    'uaddw': lambda acc, v: v if (acc.op == 'const' and acc.a[0] == 0) else E('+', acc, v),
}
CON = {
    'add': lambda a, b: (a + b) & M32,
    'sub': lambda a, b: (a - b) & M32,
    'mul': lambda a, b: (a * b) & M32,
    'eor': lambda a, b: a ^ b,
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
    scal, vec, ptr_names, notes = {}, {}, {}, []
    sp_q = None
    malloc_count = 0

    for line in preheader:
        addr, mn, ops = parse(line)
        if mn == 'mov' and ops[0][0] in 'wx':
            dst, src = ops[0][1:], ops[1]
            if src in ('xzr', 'wzr'):
                scal[dst] = 0
            elif src.startswith('#') or src.startswith('0x') or src.isdigit():
                scal[dst] = imm(src)
                notes.append(f"{line}  =>  {ops[0]} = {scal[dst]:#x}")
            elif src == 'x0':
                name = 'in_buf' if malloc_count == 0 else 'out_buf'
                malloc_count += 1
                ptr_names[dst] = name
                scal[dst] = 0
                notes.append(f"{line}  =>  x{dst} = ponteiro `{name}`")
        elif mn == 'movk':
            r, v = ops[0][1:], imm(ops[1])
            sh = int(re.search(r'LSL #(\w+)', ops[2]).group(1), 0) if len(ops) > 2 else 0
            scal[r] = (scal.get(r, 0) & ~(0xFFFF << sh)) | (v << sh)
            notes.append(f"{line}  =>  {ops[0]} = {scal[r]:#x} = {scal[r]}")
        elif mn == 'movi':
            name, n, w = vreg(ops[0])
            vec[name] = {'lanes': [imm(ops[1])] * n, 'width': w}
            notes.append(f"{line}  =>  {name} = {vec[name]['lanes']} ({n} x {w} bits)")
        elif mn == 'ldr' and ops[0][0] in 'dq':
            vname = qreg(ops[0])
            if '[sp' in ops[1] and sp_q is not None:
                vec[vname] = dict(sp_q)
                notes.append(f"{line}  =>  {vname} restaurado da stack = {vec[vname]['lanes']}")
                continue
            off_m = re.search(r'#(\w+)\]', ops[1])
            off = imm(off_m.group(1)) if off_m else 0
            key = next((k for k in resolved_data if int(k[4:], 16) & 0xFFF == off & 0xFFF), None)
            if key is None:
                raise LiftError(f"ldr sem dado resolvido: {line}")
            nbytes = 16 if ops[0][0].lower() == 'q' else 8
            raw = bytes.fromhex(resolved_data[key].replace(' ', ''))[:nbytes]
            use = next((o for l in body for o in parse(l)[2] if o.lower().startswith(vname + '.')), None)
            if use:
                _, n, w = vreg(use)
            else:
                w, n = 32, nbytes // 4
            lanes = [int.from_bytes(raw[k * w // 8:(k + 1) * w // 8], 'little') for k in range(n)]
            vec[vname] = {'lanes': lanes, 'width': w}
            sp_q = dict(vec[vname])
            notes.append(f"{line}  =>  {key} => {vname} = {lanes}")
        elif mn in ('adrp', 'str', 'add', 'mul', 'cmp', 'stp', 'ldp', 'b.ne'):
            pass
        else:
            raise LiftError(f"instrucao de pre-cabecalho nao suportada: {line}")
    return scal, vec, ptr_names, notes


# ---------------------------------------------------------------- execucao de uma volta
def step(body, vec, scal, sem, ptr_names=None, sym_mode=False, contrib=None, stores=None, mem_in=None, mem_out=None):
    branch = None
    addr_map = {}

    for line in body:
        addr, mn, ops = parse(line)
        if mn in ('add', 'sub', 'mul', 'eor') and ops[0].startswith('v'):
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
        elif mn == 'ldp' and ops[0][0].lower() == 'q':
            v1, v2 = qreg(ops[0]), qreg(ops[1])
            base_reg = ops[2].strip('[]')[1:]
            arr_name, _, byte_off = addr_map[base_reg]
            elem_off = byte_off // 4
            if sym_mode:
                vec[v1] = [E('mem', arr_name, k) for k in range(4)]
                vec[v2] = [E('mem', arr_name, k + 4) for k in range(4)]
            else:
                vec[v1] = [mem_in[elem_off + k] for k in range(4)]
                vec[v2] = [mem_in[elem_off + k + 4] for k in range(4)]
        elif mn == 'stp' and ops[0][0].lower() == 'q':
            v1, v2 = qreg(ops[0]), qreg(ops[1])
            base_reg = ops[2].strip('[]')[1:]
            arr_name, _, byte_off = addr_map.get(base_reg, ('out_buf', '8', 0))
            elem_off = byte_off // 4
            if sym_mode and stores is not None:
                for k, val in enumerate(vec[v1] + vec[v2]):
                    stores.append((arr_name, k, val))
            elif not sym_mode and mem_out is not None:
                for k, val in enumerate(vec[v1] + vec[v2]):
                    mem_out[elem_off + k] = val & M32
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
    scal, vec, ptr_names, notes = resolve_preheader(loop['preheader'], resolved_data, body)

    svec = {n: [E('const', x) for x in v['lanes']] for n, v in vec.items()}
    ind = next((n for n, v in vec.items() if len(set(v['lanes'])) > 1), None)
    i0 = 0
    if ind:
        base = vec[ind]['lanes']
        i0 = base[0]
        svec[ind] = [E('var', x - i0) for x in base]

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

    store_tmpl, store_str, target_arr = None, None, None
    if stores:
        target_arr = stores[0][0]
        st_templates = {}
        for arr, lane_k, e in stores:
            offs = e.offsets()
            if len(offs) != 1:
                raise LiftError(f"store mistura indices: {e.show()}")
            st_templates.setdefault(e.show('i', relative=True), []).append((lane_k, offs.pop(), e))
        if len(st_templates) != 1:
            raise LiftError(f"stores com formulas diferentes: {list(st_templates)}")
        store_str, st_uses = next(iter(st_templates.items()))
        store_tmpl = st_uses[0][2]

    acc_tmpl, acc_str = None, None
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
    else:
        uses = []

    return dict(idx=idx, notes=notes, ind=ind, i0=i0, stride=stride, trips=trips, total=total,
                store_arr=target_arr, store_tmpl=store_tmpl, store_str=store_str,
                tmpl=acc_tmpl, tmpl_str=acc_str, uses=uses,
                accs=sorted({u[0] for u in uses}), scal=scal, vec=vec, ptr_names=ptr_names, body=body)


def validate_all(loops, trips_check=1000):
    n_check = trips_check * loops[0]['stride']
    mem_concrete = {'in_buf': [0] * n_check, 'out_buf': [0] * n_check}
    mem_symbolic = {'in_buf': [0] * n_check, 'out_buf': [0] * n_check}

    last_concrete_acc, last_symbolic_acc = 0, 0

    for L in loops:
        vec = {n: list(v['lanes']) for n, v in L['vec'].items()}
        scal = dict(L['scal'])
        target_buf = mem_concrete.get(L['store_arr'])
        for _ in range(trips_check):
            step(L['body'], vec, scal, CON, ptr_names=L['ptr_names'], sym_mode=False,
                 mem_in=mem_concrete['in_buf'], mem_out=target_buf)

        for i in range(n_check):
            if L['store_tmpl']:
                val = L['store_tmpl'].eval(i, lambda arr, idx: mem_symbolic[arr][idx])
                mem_symbolic[L['store_arr']][i] = val
            if L['tmpl']:
                val = L['tmpl'].eval(i, lambda arr, idx: mem_symbolic[arr][idx])
                last_symbolic_acc = (last_symbolic_acc + val) & M64

        if L['accs']:
            last_concrete_acc = sum(sum(vec[a]) for a in L['accs']) & M64

    total_n = loops[-1]['total']
    full_acc = 0
    for i in range(total_n):
        in_val = loops[0]['store_tmpl'].eval(i) if loops[0]['store_tmpl'] else i
        elem = loops[-1]['tmpl'].eval(i, lambda arr, idx: in_val)
        full_acc = (full_acc + elem) & M64

    return last_concrete_acc, last_symbolic_acc, full_acc


def skeleton(fn):
    c = fn['decompiled_c']
    loop_idx = 0
    def repl_loop(_):
        nonlocal loop_idx
        loop_idx += 1
        return f"\n  /* <LOOP_VETORIAL_{loop_idx}> */"

    c = re.sub(r'\n\s*do \{.*?\} while \([^)]*\);', repl_loop, c, flags=re.S)
    c = c.replace('__ptr_00', 'out_buf').replace('__ptr', 'in_buf')
    c = c.replace('cVar7', 'start_time').replace('cVar8', 'end_time')
    c = c.replace('cVar12', 'start_time').replace('cVar13', 'end_time')

    out = []
    for line in c.splitlines():
        s = line.strip()
        if s.startswith('/* <LOOP_VETORIAL_'):
            out.append('  ' + s)
            continue
        if not re.search(r'\b(puts|printf|clock|malloc|free|return)\b', s):
            continue
        m = re.match(r'(.*?)printf\((".*?"),(.*)\);$', s)
        if m:
            fmt = m.group(2)
            n = len(re.findall(r'%(?!%)[-+ #0]*\d*(?:\.\d+)?[a-zA-Z]+', fmt))
            args = [a.strip() for a in re.split(r',(?![^()]*\))', m.group(3))]
            args = ['checksum' if re.fullmatch(r'lVar\d+( \+ lVar\d+)+', a) else a for a in args[-n:]]
            args = [re.sub(r'\(ulong\)\*\(uint \*\)\(\(long\)out_buf \+ 0x3d08fc\)', 'out_buf[1000000 - 1]', a) for a in args]
            s = f"{m.group(1)}printf({fmt}, {', '.join(args)});"
        out.append('  ' + s)
    return 'int main(void)\n{\n' + '\n'.join(out) + '\n}'


def summary_all(loops):
    blocks = []
    for L in loops:
        lines = [f"## Loop {L['idx']} (passo SIMD = {L['stride']} elementos/volta, total = {L['total']} elementos)"]
        if L['store_tmpl']:
            lines.append(f"- Escrita em memoria: `{L['store_arr']}[i] = {L['store_str']}` (para i = 0 .. {L['total'] - 1})")
        if L['tmpl']:
            lines.append(f"- Acumulador 64 bits: `checksum += {L['tmpl_str']}` (para i = 0 .. {L['total'] - 1})")
        blocks.append('\n'.join(lines))
    return '\n\n'.join(blocks)


def ask_qwen(summ, skel, out):
    prompt = (
        "Um lifter simbolico deterministico analisou os loops vetorizados (NEON) da funcao `main` de um binario AArch64. "
        "Use as formulas abaixo como verdade absoluta:\n\n"
        f"{summ}\n\n"
        "## Esqueleto da funcao main\n"
        f"{BT3}c\n{skel}\n{BT3}\n\n"
        "Tarefa: gere o codigo C escalar final, limpo e completo (com `#include <stdio.h>`, `<stdlib.h>`, `<stdint.h>`, `<time.h>`).\n"
        "Regras obrigatorias:\n"
        "1. Substitua cada `/* <LOOP_VETORIAL_N> */` pelo loop `for` escalar correspondente.\n"
        "2. PROIBIDO manter nomes genericos do Ghidra (`cVar`, `uVar`, `lVar`, `__ptr`, `ACUMULADOR_FINAL`). "
        "Use apenas nomes semanticos limpos: `in_buf`, `out_buf`, `start_time`, `end_time`, `checksum`, `N`.\n"
        "3. Use `CLOCKS_PER_SEC` para o calculo de tempo.\n"
        f"4. Responda apenas com o bloco {BT3}c contendo o codigo final compilavel."
    )
    req = urllib.request.Request(
        "http://localhost:11434/api/chat",
        data=json.dumps({"model": "qwen2.5-coder:7b", "messages": [{"role": "user", "content": prompt}],
                         "stream": False, "options": {"temperature": 0, "num_ctx": 8192}}).encode(),
        headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=900) as r:
        resp = json.load(r)
    content = resp['message']['content']
    pattern = BT3 + r'c\s*(.*?)' + BT3
    m = re.search(pattern, content, re.S)
    code_only = m.group(1).strip() + '\n' if m else content
    with open(out, 'w', encoding='utf-8') as f:
        f.write(code_only)
    print(f"\n[qwen] prompt_tokens={resp.get('prompt_eval_count')} eval_count={resp.get('eval_count')} "
          f"total_s={round(resp.get('total_duration', 0) / 1e9, 1)} -> {out}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('json')
    ap.add_argument('--expected', type=int, help='resultado observado no aparelho')
    ap.add_argument('--qwen', metavar='OUT', help='envia ao Qwen e grava o codigo C em OUT')
    args = ap.parse_args()

    with open(args.json, encoding='utf-8') as f:
        fn = next(x for x in json.load(f)['functions'] if x['name'] == 'main')

    loops = [lift_single_loop(lp, fn['resolved_data'], i + 1) for i, lp in enumerate(fn['loops'])]
    summ = summary_all(loops)
    print(summ)

    concrete, symbolic, full = validate_all(loops)
    print("\n## Validacao Deterministica")
    print(f"- Execucao concreta NEON (1000 voltas): {concrete}")
    print(f"- Formula simbolica nos mesmos i:       {symbolic}  -> {'OK' if concrete == symbolic else 'DIVERGE'}")
    print(f"- Checksum total ({loops[-1]['total']} elementos):   {full}"
          + (f"  -> aparelho {args.expected}: {'OK' if full == args.expected else 'DIVERGE'}" if args.expected else ''))
    if concrete != symbolic or (args.expected and full != args.expected):
        sys.exit("[lifter] validacao falhou; abortando.")

    skel = skeleton(fn)
    print("\n## Esqueleto Limpo\n" + skel)
    if args.qwen:
        ask_qwen(summ, skel, args.qwen)


if __name__ == '__main__':
    main()