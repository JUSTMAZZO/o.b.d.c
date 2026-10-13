"""neon_lifter.py - Lifter simbólico leve para loops NEON (AArch64) extraídos pelo ExtractMain.java.

Uso:
  python neon_lifter.py ghidra_extraction_hybrid.json [--expected N] [--qwen saida.md]

1. Resolve constantes do pré-cabeçalho (mov/movk/movi/ldr de .rodata) deterministicamente.
2. Propaga lanes simbolicamente pelo corpo do loop (i, i+1, ...) e gera a expressão de cada acumulador.
3. Valida a fórmula contra uma execução concreta das mesmas instruções.
4. Opcional: envia resumo + esqueleto do main ao Qwen 2.5 Coder local.
"""
import argparse, json, re, sys, urllib.request

M32, M64 = 0xFFFFFFFF, 0xFFFFFFFFFFFFFFFF


class LiftError(Exception):
    pass


# ---------------------------------------------------------------- expressões simbólicas
class E:
    """Nó de expressão. op: 'var' (i + c), 'const', ou operador binário ('+', '*', '^', '>>')."""
    def __init__(self, op, *a):
        self.op, self.a = op, a

    def offsets(self):
        if self.op == 'var':
            return {self.a[0]}
        if self.op == 'const':
            return set()
        return self.a[0].offsets() | self.a[1].offsets()

    def show(self, x='i', relative=False):
        if self.op == 'var':
            c = self.a[0]
            return x if (relative or c == 0) else f"({x} + {c})"
        if self.op == 'const':
            return str(self.a[0])
        l, r = self.a
        return f"({l.show(x, relative)} {self.op} {r.show(x, relative)})"

    def eval(self, x):
        """Avalia o template (folha var = x) com semântica de lane de 32 bits."""
        if self.op == 'var':
            return x & M32
        if self.op == 'const':
            return self.a[0] & M32
        l, r = self.a[0].eval(x), self.a[1].eval(x)
        return {'+': lambda: (l + r) & M32, '*': lambda: (l * r) & M32,
                '^': lambda: l ^ r, '>>': lambda: l >> r}[self.op]()


def s_add(a, b):
    if a.op == 'var' and b.op == 'const':
        return E('var', a.a[0] + b.a[0])
    if a.op == 'const' and b.op == 'var':
        return E('var', b.a[0] + a.a[0])
    if a.op == 'const' and b.op == 'const':
        return E('const', (a.a[0] + b.a[0]) & M32)
    return E('+', a, b)


SYM = {
    'add': s_add,
    'mul': lambda a, b: E('*', a, b),
    'eor': lambda a, b: E('^', a, b),
    'ushr': lambda a, n: E('>>', a, E('const', n)),
    'uaddw': lambda acc, v: v if (acc.op == 'const' and acc.a[0] == 0) else E('+', acc, v),
}
CON = {
    'add': lambda a, b: (a + b) & M32,
    'mul': lambda a, b: (a * b) & M32,
    'eor': lambda a, b: a ^ b,
    'ushr': lambda a, n: a >> n,
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
            ops.append(cur.strip()); cur = ''
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


def imm(op):
    return int(op.lstrip('#'), 0)


# ---------------------------------------------------------------- pré-cabeçalho
def resolve_preheader(preheader, resolved_data, body):
    scal, vec, notes = {}, {}, []
    for line in preheader:
        addr, mn, ops = parse(line)
        if mn == 'mov' and ops[0][0] in 'wx':
            scal[ops[0][1:]] = imm(ops[1])
            notes.append(f"{line}  =>  {ops[0]} = {scal[ops[0][1:]]:#x}")
        elif mn == 'movk':
            r, v = ops[0][1:], imm(ops[1])
            sh = int(re.search(r'LSL #(\w+)', ops[2]).group(1), 0) if len(ops) > 2 else 0
            scal[r] = (scal.get(r, 0) & ~(0xFFFF << sh)) | (v << sh)
            notes.append(f"{line}  =>  {ops[0]} = {scal[r]:#x} = {scal[r]}")
        elif mn == 'movi':
            name, n, w = vreg(ops[0])
            vec[name] = {'lanes': [imm(ops[1])] * n, 'width': w}
            notes.append(f"{line}  =>  {name} = {vec[name]['lanes']} ({n} x {w} bits)")
        elif mn == 'ldr' and ops[0][0] == 'd':
            off = imm(re.search(r'#(\w+)\]', ops[1]).group(1))
            key = next((k for k in resolved_data if int(k[4:], 16) & 0xFFF == off & 0xFFF), None)
            if key is None:
                raise LiftError(f"ldr sem dado resolvido: {line}")
            raw = bytes.fromhex(resolved_data[key].replace(' ', ''))[:8]
            name = 'v' + ops[0][1:]
            # arranjo inferido do primeiro uso no corpo do loop (ex.: v8.2S)
            use = next((o for l in body for o in parse(l)[2] if o.lower().startswith(name + '.')), None)
            _, n, w = vreg(use)
            lanes = [int.from_bytes(raw[k * w // 8:(k + 1) * w // 8], 'little') for k in range(n)]
            vec[name] = {'lanes': lanes, 'width': w}
            notes.append(f"{line}  =>  {key} = {resolved_data[key]}  =>  {name}.{n}{'BHSD'[[8,16,32,64].index(w)]} = {lanes}")
        else:
            raise LiftError(f"instrução de pré-cabeçalho não suportada: {line}")
    return scal, vec, notes


# ---------------------------------------------------------------- execução de uma volta
def step(body, vec, scal, sem, induction=None, contrib=None):
    """Executa uma volta do corpo. vec: nome -> lista de lanes (E ou int)."""
    branch = None
    for line in body:
        addr, mn, ops = parse(line)
        if mn in ('add', 'mul', 'eor'):
            d, a, b = vreg(ops[0])[0], vreg(ops[1])[0], vreg(ops[2])[0]
            vec[d] = [sem[mn](x, y) for x, y in zip(vec[a], vec[b])]
        elif mn == 'ushr':
            d, a = vreg(ops[0])[0], vreg(ops[1])[0]
            vec[d] = [sem['ushr'](x, imm(ops[2])) for x in vec[a]]
        elif mn == 'uaddw':
            d, a, b = vreg(ops[0])[0], vreg(ops[1])[0], vreg(ops[2])[0]
            if contrib is not None:
                contrib.extend((d, k, x) for k, x in enumerate(vec[b]))
            vec[d] = [sem['uaddw'](x, y) for x, y in zip(vec[a], vec[b])]
        elif mn == 'subs' and ops[0][0] in 'wx':
            branch = ('counter', ops[0][1:], imm(ops[2]))
            scal[ops[0][1:]] = (scal[ops[0][1:]] - imm(ops[2])) & M32
        elif mn == 'b.ne':
            pass
        else:
            raise LiftError(f"instrução de loop não suportada: {line}")
    return branch


def lift(fn):
    loop = fn['loops'][0]
    body = loop['body']
    scal, vec, notes = resolve_preheader(loop['preheader'], fn['resolved_data'], body)

    # variável de indução: o vetor carregado de .rodata
    ind = next(n for n, v in vec.items() if len(set(v['lanes'])) > 1)
    base = vec[ind]['lanes']
    i0 = base[0]

    # --- simbólico
    svec = {n: [E('const', x) for x in v['lanes']] for n, v in vec.items()}
    svec[ind] = [E('var', x - i0) for x in base]
    contrib = []
    br = step(body, svec, dict(scal), SYM, contrib=contrib)
    if not all(e.op == 'var' for e in svec[ind]):
        raise LiftError("variável de indução não é afim")
    stride = svec[ind][0].a[0]

    templates = {}
    for acc, lane, e in contrib:
        offs = e.offsets()
        if len(offs) != 1:
            raise LiftError(f"expressão mistura índices: {e.show()}")
        templates.setdefault(e.show('x', relative=True), []).append((acc, lane, offs.pop(), e))
    if len(templates) != 1:
        raise LiftError(f"lanes com fórmulas diferentes: {list(templates)}")
    tmpl_str, uses = next(iter(templates.items()))
    tmpl = uses[0][3]
    offsets = sorted(u[2] for u in uses)
    if offsets != list(range(stride)):
        raise LiftError(f"offsets {offsets} não cobrem o passo {stride}")

    _, creg, dec = br
    if scal[creg] % dec:
        raise LiftError("contador não divisível pelo decremento")
    trips = scal[creg] // dec
    if dec != stride:
        raise LiftError(f"decremento do contador ({dec}) != passo da indução ({stride})")
    total = trips * stride

    return dict(notes=notes, ind=ind, i0=i0, stride=stride, trips=trips, total=total, counter=creg,
                counter_init=scal[creg], tmpl=tmpl, tmpl_str=tmpl_str, uses=uses,
                accs=sorted({u[0] for u in uses}), scal=scal, vec=vec, body=body)


def validate(L, trips_check=2000):
    """Execução concreta das instruções reais vs. fórmula simbólica."""
    vec = {n: list(v['lanes']) for n, v in L['vec'].items()}
    scal = dict(L['scal'])
    for _ in range(trips_check):
        step(L['body'], vec, scal, CON)
    concrete = sum(sum(vec[a]) for a in L['accs']) & M64
    symbolic = sum(L['tmpl'].eval(i) for i in range(L['i0'], L['i0'] + trips_check * L['stride'])) & M64
    full = sum(L['tmpl'].eval(i) for i in range(L['i0'], L['i0'] + L['total'])) & M64
    return concrete, symbolic, full


# ---------------------------------------------------------------- esqueleto do main
def skeleton(fn):
    c = fn['decompiled_c']
    c = re.sub(r'\n\s*do \{.*?\} while \([^)]*\);', '\n  /* LOOP */', c, flags=re.S)
    out = []
    for line in c.splitlines():
        s = line.strip()
        if s == '/* LOOP */':
            out.append('  /* <loop vetorial: ver resumo simbólico> */')
            continue
        if not re.search(r'\b(puts|printf|clock|return)\b', s):
            continue
        # printf: Ghidra não sabe que é variádico e inclui registradores extras (ex.: d0);
        # mantém apenas tantos argumentos quanto especificadores no formato.
        m = re.match(r'(.*?)printf\((".*?"),(.*)\);$', s)
        if m:
            fmt = m.group(2)
            n = len(re.findall(r'%(?!%)[-+ #0]*\d*(?:\.\d+)?[a-zA-Z]+', fmt))
            args = [a.strip() for a in re.split(r',(?![^()]*\))', m.group(3))]
            args = ['ACUMULADOR_FINAL' if re.fullmatch(r'lVar\d+( \+ lVar\d+)+', a) else a for a in args[-n:]]
            s = f"{m.group(1)}printf({fmt},{','.join(args)});"
        out.append('  ' + s)
    return 'int main(void)\n{\n' + '\n'.join(out) + '\n}'


def summary(L):
    lines = ["## Constantes resolvidas (pré-cabeçalho)"] + [f"- {n}" for n in L['notes']]
    lines += ["", "## Propagação simbólica das lanes (1 volta)",
              f"- Variável de indução: {L['ind']} = {[('i' if c == 0 else f'i + {c}') for c in [x - L['i0'] for x in L['vec'][L['ind']]['lanes']]]}, valor inicial i = {L['i0']}"]
    for acc, lane, off, e in sorted(L['uses'], key=lambda u: u[2]):
        lines.append(f"- {acc}[lane {lane}] += zext64({e.show()})")
    lines += ["", "## Resultado do lifting",
              f"- Fórmula escalar por elemento: f(x) = {L['tmpl_str']}   (aritmética em uint32, somada em 64 bits)",
              f"- Elementos por volta: {L['stride']} (i, i+1, ..., i+{L['stride'] - 1}); {L['ind']} avança {L['stride']} por volta",
              f"- Contador w{L['counter']}: inicial {L['counter_init']}, decrementa {L['stride']} por volta, sai em 0 => {L['trips']} voltas",
              f"- Iterações escalares equivalentes: i = {L['i0']} .. {L['i0'] + L['total'] - 1} ({L['total']} iterações)",
              f"- Redução pós-loop: soma de todas as lanes de {', '.join(L['accs'])} => ACUMULADOR_FINAL = soma de f(i) para todo i"]
    return '\n'.join(lines)


def ask_qwen(summ, skel, out):
    prompt = f"""Um lifter simbólico determinístico já analisou o loop vetorizado (NEON) da função `main` de um binário AArch64. O resultado abaixo é exato: não reinterprete o assembly, use-o como verdade.

{summ}

## Esqueleto da função main (chamadas e saídas, loop removido)
```c
{skel}
```

Tarefa: gere o código C escalar final, limpo e de alto nível, da função `main` completa (com includes). Substitua o comentário do loop por um único loop `for` escalar equivalente, use nomes de variáveis significativos, `long long` para o acumulador, e `CLOCKS_PER_SEC` no lugar de constantes numéricas de clock. Responda com o código em um único bloco ```c e, depois, uma linha explicando a fórmula."""
    req = urllib.request.Request(
        "http://localhost:11434/api/chat",
        data=json.dumps({"model": "qwen2.5-coder:7b", "messages": [{"role": "user", "content": prompt}],
                         "stream": False, "options": {"temperature": 0, "num_ctx": 8192}}).encode(),
        headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=900) as r:
        resp = json.load(r)
    with open(out, 'w', encoding='utf-8') as f:
        f.write(resp['message']['content'])
    print(f"\n[qwen] prompt_tokens={resp.get('prompt_eval_count')} eval_count={resp.get('eval_count')} "
          f"total_s={round(resp.get('total_duration', 0) / 1e9, 1)} -> {out}")
    with open(out + '.prompt.txt', 'w', encoding='utf-8') as f:
        f.write(prompt)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('json')
    ap.add_argument('--expected', type=int, help='resultado observado no aparelho')
    ap.add_argument('--qwen', metavar='OUT', help='envia ao Qwen e grava a resposta em OUT')
    args = ap.parse_args()

    with open(args.json, encoding='utf-8') as f:
        fn = next(x for x in json.load(f)['functions'] if x['name'] == 'main')
    try:
        L = lift(fn)
    except LiftError as e:
        sys.exit(f"[lifter] falhou: {e}")

    summ = summary(L)
    print(summ)
    concrete, symbolic, full = validate(L)
    print("\n## Validação")
    print(f"- Execução concreta das instruções (2000 voltas): {concrete}")
    print(f"- Fórmula simbólica nos mesmos i:               {symbolic}  -> {'OK' if concrete == symbolic else 'DIVERGE'}")
    print(f"- Fórmula sobre as {L['total']} iterações:      {full}"
          + (f"  -> aparelho {args.expected}: {'OK' if full == args.expected else 'DIVERGE'}" if args.expected else ''))
    if concrete != symbolic or (args.expected and full != args.expected):
        sys.exit("[lifter] validação falhou; não enviando ao Qwen")

    skel = skeleton(fn)
    print("\n## Esqueleto\n" + skel)
    if args.qwen:
        ask_qwen(summ, skel, args.qwen)


if __name__ == '__main__':
    main()
