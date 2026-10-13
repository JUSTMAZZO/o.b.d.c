<!-- Gerado por update_dashboard.py a partir de status.json — não edite à mão. -->
<div align="center">

# 🎮 ➜ 📱 o.b.d.c

### Recompilação estática AArch64 → AArch64: binários de Nintendo Switch rodando nativamente no Android

![Endgame](https://img.shields.io/badge/Endgame-10%25-orange?style=for-the-badge) ![Fase atual](https://img.shields.io/badge/Fase%20atual-1%20%E2%80%93%20Em%20andamento-yellow?style=for-the-badge) ![ISA](https://img.shields.io/badge/ISA-AArch64%20%E2%86%92%20AArch64-informational?style=for-the-badge&logo=arm&logoColor=white) ![Alvo](https://img.shields.io/badge/Alvo-Galaxy%20A56%20%C2%B7%20Exynos%201580-1428A0?style=for-the-badge&logo=samsung&logoColor=white)

![Validação](https://img.shields.io/badge/Valida%C3%A7%C3%A3o-3%2F3%20OK-brightgreen?style=for-the-badge) ![Instruções](https://img.shields.io/badge/Instru%C3%A7%C3%B5es-9%20suportadas-blueviolet?style=for-the-badge) ![LLM](https://img.shields.io/badge/LLM-qwen2.5--coder%207b-black?style=for-the-badge&logo=ollama&logoColor=white) ![Atualizado](https://img.shields.io/badge/Atualizado-2026--10--13-lightgrey?style=for-the-badge) [![Licença](https://img.shields.io/badge/Licen%C3%A7a-MIT-green?style=for-the-badge)](LICENSE)

</div>

## 🎯 Objetivo final (Endgame)

> **Tradução e recompilação estática AArch64-to-AArch64 (Switch ELF/NSO → Android Bionic ELF)**

| | |
|---|---|
| **Origem** | Nintendo Switch — AArch64, Horizon OS, homebrew via libnx (ELF/NRO/NSO) |
| **Destino** | Samsung Galaxy A56 — Exynos 1580 (s5e8855), GPU Xclipse 540 (Vulkan), Android / Bionic, AArch64 |
| **Por que é viável** | Origem e destino compartilham o mesmo ISA (ARMv8-A AArch64). Não há emulação de CPU: o código de máquina é elevado, entendido e reemitido como ELF Android nativo. O trabalho real está na tradução de ABI, syscalls (svc), memória e subsistemas (áudio, threads, GPU). |

> [!NOTE]
> **Escopo:** Projeto de engenharia de compiladores e tradução de ABI. O foco são homebrews e ELFs próprios/de código aberto; não distribui nem ajuda a obter firmware, chaves ou jogos comerciais.

```
Switch ELF/NSO ──► Ghidra (ASM + CFG + .rodata) ──► Lifter simbólico ──► LLM (síntese C)
                                                                              │
Galaxy A56 (Bionic ELF) ◄── ADB round-trip ◄── NDK Clang ◄── shim ABI/svc ◄──┘
```

## 📈 Progresso até o Endgame: `██░░░░░░░░░░░░░░░░░░ 10%`

## 🧩 Módulos atuais

| Módulo | Status | Progresso | O que faz hoje | Próximo |
|---|---|---|---|---|
| **Ghidra Headless Extractor**<br>`ExtractMain.java` | ✅ Concluído | `███████░░░ 70%` | C descompilado + ASM AArch64 + dados .rodata resolvidos + isolamento de loops (corpo e pré-cabeçalho) | CFG completo por bloco básico, múltiplos loops aninhados, tipos de ponteiro |
| **NEON Symbolic Lifter**<br>`neon_lifter.py` | 🟡 Em andamento | `███░░░░░░░ 30%` | Propagação simbólica por lane, detecção de indução/passo/trip count, validação concreta e contra o aparelho | Memória (ld1/st1/ldr/str), and/orr/shl/sshr, umull/smull, loops com várias fórmulas |
| **Local LLM Synthesizer**<br>`qwen2.5-coder:7b (Ollama, CUDA parcial)` | ✅ Concluído | `██████░░░░ 60%` | Gera C escalar limpo a partir do resumo determinístico do lifter + esqueleto do main | Renomear variáveis com significado, recompilar e diferenciar automaticamente |
| **Target Hardware Runner**<br>`Galaxy A56 via ADB` | 🟡 Em andamento | `████░░░░░░ 45%` | push + chmod + execução em /data/local/tmp, medições intercaladas (15×) | Script único de round-trip (compilar → push → rodar → comparar), afinidade de núcleo (taskset) |

## 🗺️ Roadmap de subsistemas

| Fase | Subsistema | Status | Progresso |
|:---:|---|---|---|
| **1** | Extração CFG/ASM e Elevação Simbólica NEON | 🟡 Em andamento | `████░░░░░░ 40%` |
| **2** | Ponteiros, Arrays e Memória (ld1/st1/ldr/str) | 🔵 Próximo passo | `░░░░░░░░░░ 0%` |
| **3** | Loader e Tradução de ABI / Syscalls (svc Horizon / libnx → Bionic) | ⚪ Planejado | `░░░░░░░░░░ 0%` |
| **4** | Áudio, Threads e Redirecionamento Gráfico para Vulkan Mobile | ⚪ Planejado | `░░░░░░░░░░ 0%` |

<details open>
<summary><b>Fase 1 — Extração CFG/ASM e Elevação Simbólica NEON</b> (5/7)</summary>

- [x] Ghidra headless sem PyGhidra (script Java)
- [x] Extração híbrida C + ASM + .rodata
- [x] Isolamento de loop + pré-cabeçalho
- [x] Lifter simbólico NEON (aritmética/bitwise)
- [x] Validação concreta + round-trip no aparelho
- [ ] CFG completo e loops aninhados
- [ ] Conjunto amplo de instruções escalares/NEON

</details>

<details>
<summary><b>Fase 2 — Ponteiros, Arrays e Memória (ld1/st1/ldr/str)</b> (0/4)</summary>

- [ ] Modelo de memória simbólico (base + índice × escala)
- [ ] ld1/st1 e ldr/str com pós-incremento
- [ ] Recuperação de arrays e structs
- [ ] Benchmarks com buffers (memcpy, saxpy, dot product)

</details>

<details>
<summary><b>Fase 3 — Loader e Tradução de ABI / Syscalls (svc Horizon / libnx → Bionic)</b> (0/5)</summary>

- [ ] Parser NSO/NRO (segmentos, MOD0, relocações)
- [ ] Mapa de svc do Horizon → chamadas Bionic/POSIX
- [ ] Shim de libnx (fs, hid, time, sync)
- [ ] TLS (tpidrro_el0) / registrador x18 e convenções de ABI
- [ ] Primeiro homebrew de console (printf) rodando no A56

</details>

<details>
<summary><b>Fase 4 — Áudio, Threads e Redirecionamento Gráfico para Vulkan Mobile</b> (0/5)</summary>

- [ ] Threads e sincronização Horizon → pthread
- [ ] Áudio (audren/audout) → AAudio
- [ ] Entrada (hid) → toque/gamepad Android
- [ ] Comandos gráficos (deko3d/NVN) → Vulkan (Xclipse 540)
- [ ] Homebrew gráfico rodando no A56

</details>

## 📊 Telemetria real

### Round-trip binário → LLM → C (resultado esperado: `151550809095744`)

| # | Entrada para o LLM | Binário | Fórmula | Iterações | Resultado da reconstrução | LLM (s) |
|:---:|---|---|:---:|:---:|---|---:|
| 1 | C do Ghidra (O2 vetorizado) | `benchmark_arm64` | ❌ | ✅ | `14319067213159522304` | 78.6 |
| 2 | C do Ghidra (O2 sem vetorização) | `benchmark_arm64_novec` | ✅ | ✅ | `151550809095744` 🐍 | 45.6 |
| 3 | C + ASM + DAT (vetorizado) | `benchmark_arm64` | ❌ | ⚠️ acidental | — | 129.6 |
| 4 | Só loop ASM, prompt guiado | `benchmark_arm64` | ❌ | ❌ | — | 314.6 |
| 5 | Lifter simbólico + esqueleto | `benchmark_arm64` | ✅ | ✅ | `151550809095744` 📱 | 25.5 |

📱 = recompilado e executado no Galaxy A56 · 🐍 = verificado em Python

### Execução no Galaxy A56

| Binário | Execuções | Mediana | Mínimo | Resultado |
|---|:---:|---:|---:|---|
| benchmark_arm64 (NEON, -O2) | 15 | 0.0072 s | 0.0063 s | `151550809095744` ✅ |
| benchmark_arm64_novec (escalar) | 15 | 0.0099 s | 0.0094 s | `151550809095744` ✅ |
| qwen_main (C gerado pelo Qwen, tentativa 5) | 1 | 0.0099 s | 0.0099 s | `151550809095744` ✅ |

Aceleração NEON (mediana): **1.38×**

### Validação matemática

| Verificação | Valor | |
|---|---|:---:|
| Lifter: execução concreta (2000 voltas) vs fórmula simbólica | `96467232 = 96467232` | ✅ |
| Lifter: fórmula sobre 10.000.000 iterações vs aparelho | `151550809095744` | ✅ |
| C do Qwen recompilado (NDK) e executado no A56 | `151550809095744` | ✅ |

## 🧮 Instruções ARM64/NEON no lifter

| | Instrução | Tipo | Observação |
|:---:|---|---|---|
| ✅ | `mov / movk` | escalar | constantes de 32/64 bits resolvidas |
| ✅ | `movi` | NEON | broadcast imediato (.2S / .2D) |
| ✅ | `ldr d (literal .rodata)` | memória | só carga de constante resolvida |
| ✅ | `add` | NEON | inclui indução afim i + c |
| ✅ | `mul` | NEON | por lane, 32 bits |
| ✅ | `eor` | NEON | .8B/.16B tratado como XOR por lane |
| ✅ | `ushr` | NEON | shift lógico à direita por imediato |
| ✅ | `uaddw` | NEON | acumulação com extensão para 64 bits |
| ✅ | `subs + b.ne` | controle | contador de voltas / trip count |
| ⬜ | `sub, and, orr, bic` | NEON/escalar | planejado |
| ⬜ | `shl, sshr, ushl` | NEON | planejado |
| ⬜ | `umull, smull, umlal, smlal` | NEON | planejado |
| ⬜ | `ld1 / st1` | memória | planejado |
| ⬜ | `ldr / str / ldp / stp gerais` | memória | planejado |
| ⬜ | `cmp + b.cond / cbz / tbz` | controle | planejado |
| ⬜ | `svc #imm (Horizon OS)` | sistema | planejado |
| ⬜ | `mrs tpidrro_el0 (TLS)` | sistema | planejado |

## 🛠️ Toolchain

| Ferramenta | Versão | Papel |
|---|---|---|
| Ghidra | 12.1.4 | Análise headless + descompilação |
| Android NDK | r28 (28.2.13676358) | Clang aarch64-linux-android35 |
| Ollama | 0.40.3 | Servidor local do LLM |
| qwen2.5-coder:7b | 7.6B · Q4_K_M (GGUF) | 5,38 GB; 4,09 GB (~76%) em VRAM na RTX 4050 Laptop (6 GB), resto em CPU |
| Galaxy A56 | SM-A566E | 4×1,95 GHz + 3×2,6 GHz + 1×2,91 GHz |

## 📄 Licença

Código aberto sob a licença **MIT** — veja [LICENSE](LICENSE).

Projeto independente, sem afiliação com Nintendo ou Samsung. Nomes e marcas citados pertencem aos seus respectivos donos e são usados apenas para identificar plataformas.

---

<sub>Painel gerado por <code>update_dashboard.py</code> a partir de <code>status.json</code> · atualizado em 2026-10-13</sub>
