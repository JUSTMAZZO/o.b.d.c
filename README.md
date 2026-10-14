<!-- Gerado por update_dashboard.py a partir de status.json — não edite à mão. -->
<div align="center">

# 🎮 ➜ 📱 o.b.d.c

### Recompilação estática AArch64 → AArch64: binários de Nintendo Switch rodando nativamente no Android

![Endgame](https://img.shields.io/badge/Endgame-38%25-orange?style=for-the-badge) ![Fase atual](https://img.shields.io/badge/Fase%20atual-2%20%E2%80%93%20Em%20andamento-yellow?style=for-the-badge) ![ISA](https://img.shields.io/badge/ISA-AArch64%20%E2%86%92%20AArch64-informational?style=for-the-badge&logo=arm&logoColor=white) ![Alvo](https://img.shields.io/badge/Alvo-Galaxy%20A56%20%C2%B7%20Exynos%201580-1428A0?style=for-the-badge&logo=samsung&logoColor=white)

![Validação](https://img.shields.io/badge/Valida%C3%A7%C3%A3o-7%2F7%20OK-brightgreen?style=for-the-badge) ![Benchmarks](https://img.shields.io/badge/Benchmarks-2%20no%20A56-teal?style=for-the-badge&logo=android&logoColor=white) ![Instruções](https://img.shields.io/badge/Instru%C3%A7%C3%B5es-17%20suportadas-blueviolet?style=for-the-badge) ![LLM](https://img.shields.io/badge/LLM-qwen2.5--coder%207b-black?style=for-the-badge&logo=ollama&logoColor=white) ![Atualizado](https://img.shields.io/badge/Atualizado-2026--10--14-lightgrey?style=for-the-badge) [![Licença](https://img.shields.io/badge/Licen%C3%A7a-MIT-green?style=for-the-badge)](LICENSE)

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

## 📈 Progresso até o Endgame: `████████░░░░░░░░░░░░ 38%`

## 🧩 Módulos atuais

| Módulo | Status | Progresso | O que faz hoje | Próximo |
|---|---|---|---|---|
| **Ghidra Extractor**<br>`ExtractMain.java` | ✅ Concluído | `████████░░ 75%` | C descompilado + ASM AArch64 + .rodata resolvido + isolamento de múltiplos loops (corpo e pré-cabeçalho) | CFG completo por bloco básico, múltiplos loops aninhados, tipos de ponteiro |
| **NEON Symbolic Lifter**<br>`neon_lifter.py` | 🟡 Em andamento | `████░░░░░░ 45%` | Propagação simbólica por lane, memória base+índice (ldp/stp q), stores e acumuladores, vários loops em sequência, validação concreta | ld1/st1 e pós-incremento, and/orr/sshr, umull/smull, corrigir printf quebrado no esqueleto do main |
| **Local LLM Qwen 2.5 7B**<br>`qwen2.5-coder:7b (Ollama, CUDA parcial)` | 🟡 Em andamento | `██████░░░░ 55%` | Gera C escalar limpo a partir do resumo determinístico do lifter + esqueleto do main | Diff automático C gerado × fórmula do lifter (o Qwen acumulou o valor antes do XOR no benchmark_array_neon) |
| **Android AArch64 Runner**<br>`Galaxy A56 via ADB` | 🟡 Em andamento | `████░░░░░░ 45%` | push + chmod + execução em /data/local/tmp, medições intercaladas (15×) | Script único de round-trip (compilar → push → rodar → comparar), afinidade de núcleo (taskset) |

## 🗺️ Roadmap de subsistemas

| Fase | Subsistema | Status | Progresso |
|:---:|---|---|---|
| **1** | Lifter NEON Aritmético | ✅ Concluído | `██████████ 100%` |
| **2** | Memória, Arrays e Ponteiros | 🟡 Em andamento | `█████░░░░░ 50%` |
| **3** | Loader ELF e Tradução de ABI / Syscalls | ⚪ Planejado | `░░░░░░░░░░ 0%` |
| **4** | Subsistema de Áudio, Threads e Vulkan Mobile | ⚪ Planejado | `░░░░░░░░░░ 0%` |

<details>
<summary><b>Fase 1 — Lifter NEON Aritmético</b> (5/5)</summary>

- [x] Ghidra headless sem PyGhidra (script Java)
- [x] Extração híbrida C + ASM + .rodata
- [x] Isolamento de loop + pré-cabeçalho
- [x] Lifter simbólico NEON (add/sub/mul/eor/shl/ushr/uaddw)
- [x] Validação concreta + round-trip no aparelho (benchmark_arm64)

</details>

<details open>
<summary><b>Fase 2 — Memória, Arrays e Ponteiros</b> (5/8)</summary>

- [x] Modelo de memória simbólico (base + índice × escala)
- [x] ldp/stp de registradores Q
- [x] Vários loops em sequência na mesma função
- [x] Benchmark com buffers (benchmark_array_neon) validado pelo lifter
- [x] C reconstruído do benchmark_array_neon executado no A56
- [ ] ld1/st1 e ldr/str com pós-incremento
- [ ] CFG completo e loops aninhados
- [ ] Recuperação de structs e tipos de ponteiro

</details>

<details>
<summary><b>Fase 3 — Loader ELF e Tradução de ABI / Syscalls</b> (0/5)</summary>

- [ ] Loader ELF/NSO/NRO (segmentos, MOD0, relocações)
- [ ] Mapa de svc do Horizon → chamadas Bionic/POSIX
- [ ] Shim de libnx (fs, hid, time, sync)
- [ ] TLS (tpidrro_el0) / registrador x18 e convenções de ABI
- [ ] Primeiro homebrew de console (printf) rodando no A56

</details>

<details>
<summary><b>Fase 4 — Subsistema de Áudio, Threads e Vulkan Mobile</b> (0/5)</summary>

- [ ] Threads e sincronização Horizon → pthread
- [ ] Áudio (audren/audout) → AAudio
- [ ] Entrada (hid) → toque/gamepad Android
- [ ] Comandos gráficos (deko3d/NVN) → Vulkan (Xclipse 540)
- [ ] Homebrew gráfico rodando no A56

</details>

## 📱 Resultados no Galaxy A56

| Benchmark | Operações | Esperado | Mediana NEON | Mediana escalar | Aceleração | C reconstruído |
|---|---|---|---:|---:|:---:|---|
| **`benchmark_arm64`**<br>Loop bitwise escalar/vetorial | mul, add, eor, ushr, uaddw | Resultado: `151550809095744` | 0.0072 s | 0.0099 s | **1.38×** | 📱 `qwen_main` |
| **`benchmark_array_neon`**<br>Loop duplo com arrays in_buf/out_buf | ldp/stp, shl, ushr, sub, eor, uaddw/uaddw2 | Checksum: `12249998500128`<br>Último elemento: `24500040` | 0.0021 s | — | — | 📱 `reconstructed_array_bin` |

📱 = validado no aparelho · ⏳ = aguardando aparelho · ❌ = diverge

## 📊 Telemetria por benchmark

### `benchmark_arm64` — Loop bitwise escalar/vetorial

**Round-trip binário → LLM → C**

| # | Entrada para o LLM | Binário | Fórmula | Iterações | Resultado da reconstrução | LLM (s) |
|:---:|---|---|:---:|:---:|---|---:|
| 1 | C do Ghidra (O2 vetorizado) | `benchmark_arm64` | ❌ | ✅ | `14319067213159522304` ❌ | 78.6 |
| 2 | C do Ghidra (O2 sem vetorização) | `benchmark_arm64_novec` | ✅ | ✅ | `151550809095744` 🐍 | 45.6 |
| 3 | C + ASM + DAT (vetorizado) | `benchmark_arm64` | ❌ | ⚠️ acidental | — | 129.6 |
| 4 | Só loop ASM, prompt guiado | `benchmark_arm64` | ❌ | ❌ | — | 314.6 |
| 5 | Lifter simbólico + esqueleto | `benchmark_arm64` | ✅ | ✅ | `151550809095744` 📱 | 25.5 |

📱 = recompilado e executado no Galaxy A56 · 🐍 = verificado em Python · ❌ = diverge do esperado

**Execução no aparelho**

| Binário | Execuções | Mediana | Mínimo | Resultado |
|---|:---:|---:|---:|---|
| benchmark_arm64 (NEON, -O2) | 15 | 0.0072 s | 0.0063 s | `151550809095744` ✅ |
| benchmark_arm64_novec (escalar) | 15 | 0.0099 s | 0.0094 s | `151550809095744` ✅ |
| qwen_main (C gerado pelo Qwen, tentativa 5) | 1 | 0.0099 s | 0.0099 s | `151550809095744` ✅ |

**Validação**

| Verificação | Valor | |
|---|---|:---:|
| Lifter: execução concreta (2000 voltas) vs fórmula simbólica | `96467232 = 96467232` | ✅ |
| Lifter: fórmula sobre 10.000.000 iterações vs aparelho | `151550809095744` | ✅ |
| C do Qwen recompilado (NDK) e executado no A56 | `151550809095744` | ✅ |

### `benchmark_array_neon` — Loop duplo com arrays in_buf/out_buf

> 📱 C reconstruído (NDK r28, -O2) executado 15× no A56: checksum e último elemento idênticos ao binário original

**Round-trip binário → LLM → C**

| # | Entrada para o LLM | Binário | Fórmula | Iterações | Resultado da reconstrução | LLM (s) |
|:---:|---|---|:---:|:---:|---|---:|
| 1 | Lifter simbólico (2 loops + memória) + esqueleto | `benchmark_array_neon` | ❌ | ✅ | `12249998500000` ❌<br><sub>Qwen acumulou o valor antes do XOR 0x5a e moveu start_time para antes do loop 1</sub> | — |
| 2 | Saída do Qwen corrigida manualmente (reconstructed_array.c) | `benchmark_array_neon` | ✅ | ✅ | `12249998500128` 📱 | — |

📱 = recompilado e executado no Galaxy A56 · 🐍 = verificado em Python · ❌ = diverge do esperado

**Execução no aparelho**

| Binário | Execuções | Mediana | Mínimo | Resultado |
|---|:---:|---:|---:|---|
| benchmark_array_neon (NEON, -O2) | 15 | 0.0021 s | — | `12249998500128` ✅ |
| reconstructed_array_bin (C reconstruído, -O2) | 15 | 0.0023 s | 0.0019 s | `12249998500128` ✅ |

**Validação**

| Verificação | Valor | |
|---|---|:---:|
| Lifter: execução concreta NEON (1000 voltas) vs fórmula simbólica | `783988128 = 783988128` | ✅ |
| Lifter: checksum sobre 1.000.000 elementos vs aparelho | `12249998500128` | ✅ |
| Último elemento out_buf[N-1] (Python) | `24500040` | ✅ |
| C reconstruído executado no A56 (15×, checksum + último elemento) | `12249998500128 / 24500040` | ✅ |

## 🧮 Instruções ARM64/NEON no lifter

**17 suportadas** · 10 grupos planejados

| | Instrução | Tipo | Observação |
|:---:|---|---|---|
| ✅ | `mov` | escalar | constantes e cópias de registrador |
| ✅ | `movk` | escalar | constantes de 64 bits montadas em partes |
| ✅ | `movi` | NEON | broadcast imediato (.2S / .4S / .2D) |
| ✅ | `ldr` | memória | carga literal de .rodata (d/q) |
| ✅ | `ldp` | memória | par de registradores Q, base + índice |
| ✅ | `stp` | memória | par de registradores Q → array de saída |
| ✅ | `add` | NEON/escalar | por lane; escalar como indução/endereço |
| ✅ | `sub` | NEON | por lane, 32 bits |
| ✅ | `mul` | NEON | por lane, 32 bits |
| ✅ | `eor` | NEON | .8B/.16B tratado como XOR por lane |
| ✅ | `ushr` | NEON | shift lógico à direita por imediato |
| ✅ | `shl` | NEON | shift à esquerda por imediato |
| ✅ | `uaddw` | NEON | acumula lanes baixas estendidas para 64 bits |
| ✅ | `uaddw2` | NEON | acumula lanes altas estendidas para 64 bits |
| ✅ | `subs` | controle | contador de voltas / trip count |
| ✅ | `cmp` | controle | limite em bytes (offset × passo) |
| ✅ | `b.ne` | controle | aresta de retorno do loop |
| ⬜ | `and, orr, bic, mvn` | NEON/escalar | planejado — Fase 2 |
| ⬜ | `sshr, ushl, sshl` | NEON | planejado — Fase 2 |
| ⬜ | `umull, smull, umlal, smlal` | NEON | planejado — Fase 2 |
| ⬜ | `ld1 / st1 (pós-incremento)` | memória | planejado — Fase 2 |
| ⬜ | `ldr / str escalares gerais` | memória | planejado — Fase 2 |
| ⬜ | `b.cond / cbz / cbnz / tbz` | controle | planejado — Fase 2 |
| ⬜ | `bl / blr / ret` | controle | planejado — Fase 3 |
| ⬜ | `svc #imm (Horizon OS)` | sistema | planejado — Fase 3 |
| ⬜ | `mrs tpidrro_el0 (TLS)` | sistema | planejado — Fase 3 |
| ⬜ | `ldxr / stxr / ldaxr / stlxr` | atômico | planejado — Fase 4 |

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

<sub>Painel gerado por <code>update_dashboard.py</code> a partir de <code>status.json</code> · atualizado em 2026-10-14</sub>
