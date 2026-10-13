"""update_dashboard.py - gera README.md a partir de status.json.

Uso: python update_dashboard.py [status.json] [README.md]
"""
import json, sys
from urllib.parse import quote

STATUS = {
    'done':        ('✅', 'Concluído',     'brightgreen'),
    'in_progress': ('🟡', 'Em andamento',  'yellow'),
    'next':        ('🔵', 'Próximo passo', 'blue'),
    'planned':     ('⚪', 'Planejado',     'lightgrey'),
}


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


def render(d):
    p, t = d['project'], d['telemetry']
    phases = d['roadmap']
    endgame = round(sum(ph['progress'] for ph in phases) / len(phases))
    current = next((ph for ph in phases if ph['status'] == 'in_progress'), phases[0])
    passed = sum(v['ok'] for v in t['validation'])
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
               badge('Validação', f'{passed}/{len(t["validation"])} OK', 'brightgreen' if passed == len(t['validation']) else 'red'),
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

    out += ['## 📊 Telemetria real', '',
            f"### Round-trip binário → LLM → C (resultado esperado: `{t['expected_result']}`)", '',
            '| # | Entrada para o LLM | Binário | Fórmula | Iterações | Resultado da reconstrução | LLM (s) |',
            '|:---:|---|---|:---:|:---:|---|---:|']
    it_icon = {'ok': '✅', 'acidental': '⚠️ acidental', 'erro': '❌'}
    for a in t['attempts']:
        res = f"`{a['result']}`" if a['result'] != '—' else '—'
        if a.get('verified_on_device'):
            res += ' 📱'
        elif a['result'] == str(t['expected_result']):
            res += ' 🐍'
        out.append(f"| {a['n']} | {a['input']} | `{a['binary']}` | {'✅' if a['formula'] else '❌'} | "
                   f"{it_icon[a['iterations']]} | {res} | {a['llm_seconds']} |")
    out += ['', '📱 = recompilado e executado no Galaxy A56 · 🐍 = verificado em Python', '']

    out += ['### Execução no Galaxy A56', '',
            '| Binário | Execuções | Mediana | Mínimo | Resultado |', '|---|:---:|---:|---:|---|']
    for r in t['device_runs']:
        ok = '✅' if r['result'] == str(t['expected_result']) else '❌'
        out.append(f"| {r['binary']} | {r['runs']} | {r['median_s']:.4f} s | {r['min_s']:.4f} s | `{r['result']}` {ok} |")
    vec, novec = t['device_runs'][0], t['device_runs'][1]
    out += ['', f"Aceleração NEON (mediana): **{novec['median_s'] / vec['median_s']:.2f}×**", '']

    out += ['### Validação matemática', '', '| Verificação | Valor | |', '|---|---|:---:|']
    out += [f"| {x['check']} | `{x['value']}` | {'✅' if x['ok'] else '❌'} |" for x in t['validation']]
    out.append('')

    out += ['## 🧮 Instruções ARM64/NEON no lifter', '',
            '| | Instrução | Tipo | Observação |', '|:---:|---|---|---|']
    out += [f"| ✅ | `{i['insn']}` | {i['kind']} | {i['note']} |" for i in d['isa']['supported']]
    out += [f"| ⬜ | `{i['insn']}` | {i['kind']} | planejado |" for i in d['isa']['planned']]
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
        f.write(render(data))
    print(f"[+] {dst} gerado a partir de {src}")


if __name__ == '__main__':
    main()
