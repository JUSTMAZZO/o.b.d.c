"""update_dashboard.py - gera README.md a partir de status.json.

Uso: python update_dashboard.py [status.json] [README.md]
"""
import json, os, sys
from urllib.parse import quote

STATUS = {
    'done':        ('✅', 'Concluído',     'brightgreen'),
    'in_progress': ('🟡', 'Em andamento',  'yellow'),
    'next':        ('🔵', 'Próximo passo', 'blue'),
    'planned':     ('⚪', 'Planejado',     'lightgrey'),
}


REC = {'verified': '📱', 'hle_port': '🎮', 'pending_device': '⏳', 'failed': '❌'}
REC_LEGEND = '📱 = C reconstruído validado no aparelho · 🎮 = fonte libnx portado via shim HLE e validado · ⏳ = aguardando aparelho · ❌ = diverge'
OK = {True: '✅', False: '❌', None: '⏳'}


def secs(x):
    return f"{x:.4f} s" if x is not None else '—'


def bar(pct, width=10):
    full = round(pct / 100 * width)
    return '█' * full + '░' * (width - full) + f' {pct}%'


def badge(label, message, color, logo=None):
    esc = lambda s: quote(str(s).replace('-', '--').replace('_', '__'), safe='')
    url = f"https://img.shields.io/badge/{esc(label)}-{esc(message)}-{color}?style=for-the-badge"
    if logo:
        url += f"&logo={logo}&logoColor=white"
    return f"![{label}]({url})"


def status(s):
    icon, text, _ = STATUS[s]
    return f"{icon} {text}"


def attempts_table(b, expected):
    it_icon = {'ok': '✅', 'acidental': '⚠️ acidental', 'erro': '❌'}
    out = ['**Round-trip binário → LLM → C**', '',
           '| # | Entrada para o LLM | Binário | Fórmula | Iterações | Resultado da reconstrução | LLM (s) |',
           '|:---:|---|---|:---:|:---:|---|---:|']
    for a in b['attempts']:
        res = f"`{a['result']}`" if a['result'] != '—' else '—'
        if a.get('verified_on_device'):
            res += ' 📱'
        elif a['result'] in expected:
            res += ' 🐍'
        elif a['result'] != '—':
            res += ' ❌'
        if a.get('note'):
            res += f"<br><sub>{a['note']}</sub>"
        llm = a['llm_seconds'] if a['llm_seconds'] is not None else '—'
        out.append(f"| {a['n']} | {a['input']} | `{a['binary']}` | {'✅' if a['formula'] else '❌'} | "
                   f"{it_icon[a['iterations']]} | {res} | {llm} |")
    out += ['', '📱 = recompilado e executado no Galaxy A56 · 🐍 = verificado em Python · ❌ = diverge do esperado', '']
    return out


def render(d, src_path="status.json"):
    p, t = d['project'], d['telemetry']
    phases = d['roadmap']
    endgame = round(sum(ph['progress'] for ph in phases) / len(phases))
    current = next((ph for ph in phases if ph['status'] == 'in_progress'), phases[0])
    benches = t['benchmarks']
    checks = [v for b in benches for v in b['validation']]
    passed = sum(v['ok'] is True for v in checks)
    decided = sum(v['ok'] is not None for v in checks)
    sup = len(d['isa']['supported'])

    out = [f"<!-- Gerado por update_dashboard.py a partir de status.json — não edite à mão. -->",
           '<div align="center">', '',
           f"# 🎮 ➜ 📱 {p['name']}", '',
           f"### {p['tagline']}", '',
           ' '.join([
               badge('Endgame', f'{endgame}%', 'orange'),
               badge('Fase atual', f"{current['phase']} – {STATUS[current['status']][1]}", STATUS[current['status']][2]),
               badge('ISA', 'AArch64 → AArch64', 'informational', 'arm'),
               badge('Alvo', 'Galaxy A56 · Exynos 1580', '1428A0', 'samsung'),
           ]), '',
           ' '.join([
               badge('Validação', f'{passed}/{len(checks)} OK',
                     'red' if passed < decided else 'brightgreen' if decided == len(checks) else 'yellow'),
               badge('Benchmarks', f'{len(benches)} no A56', 'teal', 'android'),
               badge('Instruções', f'{sup} suportadas', 'blueviolet'),
               badge('LLM', 'qwen2.5-coder 7b', 'black', 'ollama'),
               badge('Atualizado', p['updated'], 'lightgrey'),
           ] + ([f"[{badge('Licença', p['license'], 'green')}](LICENSE)"] if p.get('license') else [])), '',
           '</div>', '']

    v = p['vision']
    out += ['## 🎯 Objetivo final (Endgame)', '',
            f"> **{v['endgame']}**", '',
            '| | |', '|---|---|',
            f"| **Origem** | {v['source']} |",
            f"| **Destino** | {v['target']} |",
            f"| **Por que é viável** | {v['rationale']} |", '',
            f"> [!NOTE]\n> **Escopo:** {p['scope_note']}", '',
            '```',
            'Switch ELF/NSO ──► Ghidra (ASM + CFG + .rodata) ──► Lifter simbólico ──► LLM (síntese C)',
            '                                                                              │',
            'Galaxy A56 (Bionic ELF) ◄── ADB round-trip ◄── NDK Clang ◄── shim ABI/svc ◄──┘',
            '```', '']

    out += [f"## 📈 Progresso até o Endgame: `{bar(endgame, 20)}`", '']

    out += ['## 🧩 Módulos atuais', '',
            '| Módulo | Status | Progresso | O que faz hoje | Próximo |',
            '|---|---|---|---|---|']
    for m in d['modules']:
        out.append(f"| **{m['name']}**<br>`{m['file']}` | {status(m['status'])} | `{bar(m['progress'])}` | {m['summary']} | {m['next']} |")
    out.append('')

    out += ['## 🗺️ Roadmap de subsistemas', '',
            '| Fase | Subsistema | Status | Progresso |', '|:---:|---|---|---|']
    for ph in phases:
        out.append(f"| **{ph['phase']}** | {ph['title']} | {status(ph['status'])} | `{bar(ph['progress'])}` |")
    out.append('')
    for ph in phases:
        done = sum(i['done'] for i in ph['items'])
        out += [f"<details{' open' if ph['status'] == 'in_progress' else ''}>",
                f"<summary><b>Fase {ph['phase']} — {ph['title']}</b> ({done}/{len(ph['items'])})</summary>", '']
        out += [f"- [{'x' if i['done'] else ' '}] {i['name']}" for i in ph['items']]
        out += ['', '</details>', '']

    out += ['## 📱 Resultados no Galaxy A56', '',
            '| Benchmark | Operações | Esperado | Mediana NEON | Mediana escalar | Aceleração | C reconstruído / portado |',
            '|---|---|---|---:|---:|:---:|---|']
    for b in benches:
        exp = '<br>'.join(f"{k}: `{v}`" for k, v in b['expected'].items())
        speed = f"**{b['scalar_median_s'] / b['neon_median_s']:.2f}×**" if b['scalar_median_s'] else '—'
        rec = b['reconstructed']
        out.append(f"| **`{b['id']}`**<br>{b['title']} | {b['ops']} | {exp} | {secs(b['neon_median_s'])} | "
                   f"{secs(b['scalar_median_s'])} | {speed} | {REC[rec['status']]} `{rec['binary']}` |")
    out += ['', REC_LEGEND, '']

    out += ['## 📊 Telemetria por benchmark', '']
    for b in benches:
        expected = set(b['expected'].values())
        out += [f"### `{b['id']}` — {b['title']}", '']
        if b['reconstructed'].get('note'):
            out += [f"> {REC[b['reconstructed']['status']]} {b['reconstructed']['note']}", '']

        if b['attempts']:
            out += attempts_table(b, expected)

        out += ['**Execução no aparelho**', '',
                '| Binário | Execuções | Mediana | Mínimo | Resultado |', '|---|:---:|---:|---:|---|']
        for r in b['device_runs']:
            ok = '✅' if r['result'] in expected else '❌'
            out.append(f"| {r['binary']} | {r['runs']} | {secs(r['median_s'])} | {secs(r['min_s'])} | `{r['result']}` {ok} |")
        out.append('')

        out += ['**Validação**', '', '| Verificação | Valor | |', '|---|---|:---:|']
        out += [f"| {x['check']} | `{x['value']}` | {OK[x['ok']]} |" for x in b['validation']]
        out.append('')

    out += ['## 🧮 Instruções ARM64/NEON no lifter', '',
            f"**{sup} suportadas** · {len(d['isa']['planned'])} grupos planejados", '',
            '| | Instrução | Tipo | Observação |', '|:---:|---|---|---|']
    out += [f"| ✅ | `{i['insn']}` | {i['kind']} | {i['note']} |" for i in d['isa']['supported']]
    out += [f"| ⬜ | `{i['insn']}` | {i['kind']} | planejado — Fase {i['phase']} |" for i in d['isa']['planned']]
    out.append('')

    probe = d.get('environment_probe')
    if probe:
        out += ['## 🔬 Ambiente de execução no A56 (sonda empírica)', '',
                f"{probe['device']} · {probe['date']} · `{probe['source']}`", '',
                '| Item | Medido | O que significa |', '|---|---|---|']
        out += [f"| `{r['item']}` | {r['value']} | {r['note']} |" for r in probe['results']]
        inv = probe.get('nro_inventory')
        if inv:
            out += ['', f"**Inventário estático de `{inv['name']}`** ({inv['size']}) com `nro_tool.py`:", '',
                    f"- Relocations: {inv['relocs']}",
                    f"- Sítios: {inv['sites']}",
                    f"- Conferência: {inv['crosscheck']}"]
        out.append('')

    if d.get('svc_map'):
        table_path = os.path.join(os.path.dirname(os.path.abspath(src_path)), d['svc_map'])
        with open(table_path, encoding='utf-8') as f:
            st = json.load(f)
        out += ['## 🧷 SVCs do Horizon → Bionic/POSIX', '',
                f"{len(st['svcs'])} SVCs (libnx {st['libnx']}) · especificação em `{d['svc_map']}` · "
                f"handlers em `{st['implementation']}` · teste `{st['test']}`", '',
                '| SVC | Função | Tradução Bionic/POSIX | Testado no A56 |', '|:---:|---|---|---|']
        cell = lambda t: t.replace('|', '\\|')   # | dentro da celula quebraria a tabela
        for s in st['svcs']:
            posix = '<br>'.join(f"`{cell(p)}`" for p in s['posix']) or f"— <sub>{cell(s.get('note', ''))}</sub>"
            out.append(f"| `{s['id']}` | `{s['name']}` | {posix} | {cell(s['tested'])} |")
        out.append('')

    out += ['## 🛠️ Toolchain', '', '| Ferramenta | Versão | Papel |', '|---|---|---|']
    out += [f"| {x['name']} | {x['version']} | {x['role']} |" for x in d['toolchain']]
    if p.get('license'):
        out += ['', '## 📄 Licença', '',
                f"Código aberto sob a licença **{p['license']}** — veja [LICENSE](LICENSE).", '',
                'Projeto independente, sem afiliação com Nintendo ou Samsung. Nomes e marcas citados '
                'pertencem aos seus respectivos donos e são usados apenas para identificar plataformas.']
    out += ['', '---', '',
            f"<sub>Painel gerado por <code>update_dashboard.py</code> a partir de <code>status.json</code> · atualizado em {p['updated']}</sub>", '']
    return '\n'.join(out)


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else 'status.json'
    dst = sys.argv[2] if len(sys.argv) > 2 else 'README.md'
    with open(src, encoding='utf-8') as f:
        data = json.load(f)
    with open(dst, 'w', encoding='utf-8', newline='\n') as f:
        f.write(render(data, src))
    print(f"[+] {dst} gerado a partir de {src}")


if __name__ == '__main__':
    main()
