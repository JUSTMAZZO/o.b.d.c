<!-- Gerado por update_dashboard.py a partir de status.json — não edite à mão. -->
<div align="center">

# 🎮 ➜ 📱 o.b.d.c

### Recompilação estática AArch64 → AArch64: binários de Nintendo Switch rodando nativamente no Android

![Endgame](https://img.shields.io/badge/Endgame-58%25-orange?style=for-the-badge) ![Fase atual](https://img.shields.io/badge/Fase%20atual-3%20%E2%80%93%20Em%20andamento-yellow?style=for-the-badge) ![ISA](https://img.shields.io/badge/ISA-AArch64%20%E2%86%92%20AArch64-informational?style=for-the-badge&logo=arm&logoColor=white) ![Alvo](https://img.shields.io/badge/Alvo-Galaxy%20A56%20%C2%B7%20Exynos%201580-1428A0?style=for-the-badge&logo=samsung&logoColor=white)

![Validação](https://img.shields.io/badge/Valida%C3%A7%C3%A3o-26%2F26%20OK-brightgreen?style=for-the-badge) ![Benchmarks](https://img.shields.io/badge/Benchmarks-4%20no%20A56-teal?style=for-the-badge&logo=android&logoColor=white) ![Instruções](https://img.shields.io/badge/Instru%C3%A7%C3%B5es-24%20suportadas-blueviolet?style=for-the-badge) ![LLM](https://img.shields.io/badge/LLM-qwen2.5--coder%207b-black?style=for-the-badge&logo=ollama&logoColor=white) ![Atualizado](https://img.shields.io/badge/Atualizado-2026--10--15-lightgrey?style=for-the-badge) [![Licença](https://img.shields.io/badge/Licen%C3%A7a-MIT-green?style=for-the-badge)](LICENSE)

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

## 📈 Progresso até o Endgame: `████████████░░░░░░░░ 58%`

## 🧩 Módulos atuais

| Módulo | Status | Progresso | O que faz hoje | Próximo |
|---|---|---|---|---|
| **Ghidra Extractor**<br>`ExtractMain.java` | ✅ Concluído | `████████░░ 80%` | C descompilado + ASM AArch64 + .rodata + isolamento de loops + nomes dos alvos de bl e strings carregadas (formatos de printf) | Tipos de ponteiro/structs, várias funções além do main |
| **NEON Symbolic Lifter**<br>`neon_lifter.py` | 🟡 Em andamento | `████████░░ 85%` | Elevação simbólica NEON sem código por binário: memória contígua (ldr/str q, ldp/stp, ld1/st1) e array of structs (ld2/st2, campos .fN), pós-incremento, constantes do main; esqueleto do main gerado do CFG com structs, ponteiros e PadState tipados; C de referência no host | Testar ld3/ld4 com binário real, if/else e switch no estruturador, várias funções além do main |
| **Local LLM Qwen 2.5 7B**<br>`qwen2.5-coder:7b (Ollama, CUDA parcial)` | 🟡 Em andamento | `████████░░ 75%` | Gera C a partir do resumo do lifter + esqueleto do CFG; antes do ADB passa por checagem de estrutura (profundidade e ordem das chamadas) e pelo gate no host, que compila, executa e compara a saída com o C de referência, com 1 nova tentativa guiada pelo diff | Fazer o Qwen renomear variáveis de registrador (w21, x23, sp_0x10) e tipar PadState |
| **Android AArch64 Runner**<br>`Galaxy A56 via ADB` | 🟡 Em andamento | `████░░░░░░ 45%` | push + chmod + execução em /data/local/tmp, medições intercaladas (15×) | Script único de round-trip (compilar → push → rodar → comparar), afinidade de núcleo (taskset) |
| **Switch HLE Shim**<br>`switch_hle_shim.h` | 🟡 Em andamento | `██░░░░░░░░ 15%` | Tipos e primitivas libnx (console, appletMainLoop, pad/hid determinístico, armGetSystemTick a 19,2 MHz) sobre Bionic/POSIX | Shim de fs/time/sync, entrada real (toque/gamepad), loader ELF/NRO e tradução de svc |
| **Horizon Static Analysis**<br>`horizon_rt/ (nro_tool.py, probe/)` | 🟡 Em andamento | `███░░░░░░░ 30%` | Leitor de NRO (segmentos, MOD0, .dynamic, RELA/RELR) com inventário de svc e leituras de registradores de sistema; sonda empírica do ambiente EL0 do A56 | Mapa dos svc do Horizon para chamadas Bionic/POSIX e inventário de NROs maiores |

## 🗺️ Roadmap de subsistemas

| Fase | Subsistema | Status | Progresso |
|:---:|---|---|---|
| **1** | Lifter NEON Aritmético | ✅ Concluído | `██████████ 100%` |
| **2** | Memória, Arrays e Ponteiros | ✅ Concluído | `██████████ 100%` |
| **3** | Loader ELF e Tradução de ABI / Syscalls | 🟡 Em andamento | `███░░░░░░░ 30%` |
| **4** | Subsistema de Áudio, Threads e Vulkan Mobile | ⚪ Planejado | `░░░░░░░░░░ 0%` |

<details>
<summary><b>Fase 1 — Lifter NEON Aritmético</b> (5/5)</summary>

- [x] Ghidra headless sem PyGhidra (script Java)
- [x] Extração híbrida C + ASM + .rodata
- [x] Isolamento de loop + pré-cabeçalho
- [x] Lifter simbólico NEON (add/sub/mul/eor/shl/ushr/uaddw)
- [x] Validação concreta + round-trip no aparelho (benchmark_arm64)

</details>

<details>
<summary><b>Fase 2 — Memória, Arrays e Ponteiros</b> (14/14)</summary>

- [x] Modelo de memória simbólico (base + índice × escala)
- [x] ldp/stp de registradores Q
- [x] Vários loops em sequência na mesma função
- [x] Benchmark com buffers (benchmark_array_neon) validado pelo lifter
- [x] Stores dependentes no mesmo loop (ordem sequencial em C)
- [x] Loop NEON dentro do loop de frames (60× com boost simbólico)
- [x] C reconstruído do benchmark_array_neon executado no A56
- [x] ldr/str de registradores Q com pós-incremento
- [x] ld1/st1 e ld2/st2 com e sem pós-incremento (array of structs, switch_aos_bench)
- [x] C de referência sem LLM (esqueleto do CFG + loops do lifter) validado no A56 nos 3 benchmarks
- [x] Lifter sem código específico por binário (sem is_switch; validação genérica por loop)
- [x] Gate no host antes do ADB: C gerado compilado e comparado com o C de referência
- [x] CFG do main e loops aninhados: esqueleto C gerado automaticamente (cfg_skeleton.py)
- [x] Recuperação de structs (layout de ld2/st2) e tipos de ponteiro (PadState na pilha pelo header do shim)

</details>

<details open>
<summary><b>Fase 3 — Loader ELF e Tradução de ABI / Syscalls</b> (6/10)</summary>

- [x] Shim HLE de primitivas libnx (console, applet, pad, timer) — switch_hle_shim.h
- [x] Homebrew estilo libnx compilado do fonte e validado no A56 (switch_particle_bench)
- [x] Leitor NRO: NRO0, segmentos, MOD0, .dynamic, relocations RELA/RELR (nro_tool.py)
- [x] Inventário de sítios svc / mrs tpidrro_el0 / cntpct_el0 conferido contra o ELF original
- [x] Sonda do ambiente no A56 (registradores de sistema em EL0, x18, preservação de registradores)
- [x] Homebrew mínimo de teste (devkitPro + libnx) com oráculo pelo switch_hle_shim.h
- [ ] Mapa de svc do Horizon → chamadas Bionic/POSIX
- [ ] Shim de libnx completo (fs, hid real, time, sync)
- [ ] TLS (tpidrro_el0) / registrador x18 e convenções de ABI
- [ ] Primeiro homebrew binário (NRO) traduzido rodando no A56

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

| Benchmark | Operações | Esperado | Mediana NEON | Mediana escalar | Aceleração | C reconstruído / portado |
|---|---|---|---:|---:|:---:|---|
| **`benchmark_arm64`**<br>Loop bitwise escalar/vetorial | mul, add, eor, ushr, uaddw | Resultado: `151550809095744` | 0.0072 s | 0.0099 s | **1.38×** | 📱 `qwen_main` |
| **`benchmark_array_neon`**<br>Loop duplo com arrays in_buf/out_buf | ldp/stp, shl, ushr, sub, eor, uaddw/uaddw2 | Checksum: `12249998500128`<br>Último elemento: `24500040` | 0.0021 s | — | — | 📱 `reconstructed_array_bin` |
| **`switch_particle_bench`**<br>Homebrew libnx: física de 100.000 partículas × 60 frames (shim HLE) | ldr q, str q (pós-incremento), dup, add, and, eor, ushr, sub | Checksum: `88196450180`<br>Última partícula: `1734747` | 0.0046 s | — | — | 📱 `reconstructed_switch_bin` |
| **`switch_aos_bench`**<br>Homebrew libnx: 100.000 partículas como array of structs {x, vx} × 60 frames | ld2 / st2 (com e sem pós-incremento), dup, add, and, eor, ushr, sub, uaddw/uaddw2 + epílogo escalar | Checksum: `88196450180`<br>Última partícula: `1734747` | 0.0074 s | — | — | 📱 `reconstructed_aos.c` |

📱 = C reconstruído validado no aparelho · 🎮 = fonte libnx portado via shim HLE e validado · ⏳ = aguardando aparelho · ❌ = diverge

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
| C de referência sem LLM (esqueleto do CFG + lifter, ref_hybrid) no A56, 3× | `151550809095744` | ✅ |
| Gate no host: reconstructed_arm64.c do Qwen = C de referência (e conferido no A56, 3×) | `151550809095744` | ✅ |

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
| C de referência sem LLM (esqueleto do CFG + lifter, ref_array) no A56, 3× | `12249998500128 / 24500040` | ✅ |
| Gate no host: reconstructed_array.c do Qwen = C de referência (e conferido no A56, 3×) | `12249998500128 / 24500040` | ✅ |

### `switch_particle_bench` — Homebrew libnx: física de 100.000 partículas × 60 frames (shim HLE)

> 📱 Round-trip completo: binário com shim libnx → Ghidra → lifter + esqueleto do main gerado do CFG → Qwen → NDK r28 -O2 → A56 (15×, taskset 80) com checksum e última partícula idênticos. O C de referência sem LLM também confere. Medição intercalada (20 pares, cpu7): 4,58 ms original × 4,71 ms reconstruído, razão mediana 1,011×; a cpu7 varia de frequência (DVFS), por isso os tempos isolados oscilam entre ~3,2 e ~7 ms.

**Round-trip binário → LLM → C**

| # | Entrada para o LLM | Binário | Fórmula | Iterações | Resultado da reconstrução | LLM (s) |
|:---:|---|---|:---:|:---:|---|---:|
| 1 | Lifter (fórmulas sobre valores carregados) + esqueleto | `switch_particle_bench` | ❌ | ✅ | —<br><sub>pos_x reaplicava a atualização de vel_x já escrita; resumo do lifter passou a ordem sequencial</sub> | — |
| 2 | Resumo sequencial + esqueleto | `switch_particle_bench` | ✅ | ❌ | —<br><sub>Qwen moveu o loop de inicialização para dentro do while (detectado pela checagem de profundidade, inclusive na nova tentativa)</sub> | 49.1 |
| 3 | Resumo sequencial + esqueleto + posição de cada loop | `switch_particle_bench` | ✅ | ✅ | `88196450180` 📱 | 43.6 |
| 4 | Esqueleto gerado do CFG + resumo sequencial + posição | `switch_particle_bench` | ✅ | ❌ | `88196450180` 🐍<br><sub>valores certos, mas armGetSystemTick() foi movido para antes do loop de inicialização (tempo errado); pego pela nova checagem de ordem das chamadas</sub> | 65.0 |
| 5 | Esqueleto do CFG + posição com linha anterior/seguinte | `switch_particle_bench` | ✅ | ✅ | `88196450180` 📱<br><sub>estrutura fiel ao binário; o Qwen não renomeou as variáveis de registrador</sub> | 59.0 |

📱 = recompilado e executado no Galaxy A56 · 🐍 = verificado em Python · ❌ = diverge do esperado

**Execução no aparelho**

| Binário | Execuções | Mediana | Mínimo | Resultado |
|---|:---:|---:|---:|---|
| switch_particle_bench (sem afinidade) | 15 | 0.0054 s | 0.0044 s | `88196450180` ✅ |
| switch_particle_bench (taskset cpu7, 2,91 GHz) | 15 | 0.0044 s | 0.0044 s | `88196450180` ✅ |
| reconstructed_switch_bin (C do Qwen, taskset cpu7) | 15 | 0.0047 s | 0.0042 s | `88196450180` ✅ |
| switch_particle_bench (original, intercalado A/B, cpu7) | 20 | 0.0046 s | 0.0032 s | `88196450180` ✅ |
| reconstructed_switch_bin (Qwen + esqueleto do CFG, intercalado A/B, cpu7) | 20 | 0.0047 s | 0.0032 s | `88196450180` ✅ |

**Validação**

| Verificação | Valor | |
|---|---|:---:|
| Referência NumPy (uint32, 60 frames, impulso no frame 30) | `88196450180 / 1734747` | ✅ |
| A56: 30 execuções, checksum e última partícula idênticos | `88196450180 / 1734747` | ✅ |
| Frames executados (appletMainLoop + Plus no frame 60) | `60` | ✅ |
| Loop de física vetorizado pelo Clang -O2 (-Rpass=loop-vectorize) | `VF 4 × IC 1` | ✅ |
| Lifter: execução concreta NEON (500 voltas) vs fórmula simbólica | `27777460 = 27777460` | ✅ |
| Lifter: 60 frames sobre 100.000 partículas vs aparelho | `88196450180` | ✅ |
| C reconstruído executado no A56 (15×, frames + checksum + última partícula) | `60 / 88196450180 / 1734747` | ✅ |
| C de referência sem LLM (esqueleto do CFG + lifter, ref_switch) no A56, 3× | `60 / 88196450180 / 1734747` | ✅ |
| Desempenho do C reconstruído vs original (20 pares intercalados, razão B/A mediana) | `1,011×` | ✅ |
| Gate no host: reconstructed_switch.c do Qwen = C de referência (e conferido no A56, 3×) | `60 / 88196450180 / 1734747` | ✅ |

### `switch_aos_bench` — Homebrew libnx: 100.000 partículas como array of structs {x, vx} × 60 frames

> 📱 Mesma física do switch_particle_bench com layout AoS. O lifter modela ld2/st2 como campos de struct (buf_x19[i].f0/.f4); o esqueleto recupera `typedef struct { uint32_t f0; uint32_t f4; }`, o PadState da pilha e o epílogo escalar que o Clang gera para as 8 últimas structs (buf_x19[99992..99999].f0).

**Round-trip binário → LLM → C**

| # | Entrada para o LLM | Binário | Fórmula | Iterações | Resultado da reconstrução | LLM (s) |
|:---:|---|---|:---:|:---:|---|---:|
| 1 | Resumo do lifter (ld2/st2) + esqueleto do CFG com structs | `switch_aos_bench` | ✅ | ✅ | `88196450180` 📱<br><sub>aprovado pelo gate no host na primeira tentativa</sub> | 78.4 |

📱 = recompilado e executado no Galaxy A56 · 🐍 = verificado em Python · ❌ = diverge do esperado

**Execução no aparelho**

| Binário | Execuções | Mediana | Mínimo | Resultado |
|---|:---:|---:|---:|---|
| switch_aos_bench (fonte original, taskset cpu7) | 15 | 0.0074 s | 0.0071 s | `88196450180` ✅ |

**Validação**

| Verificação | Valor | |
|---|---|:---:|
| Referência NumPy (AoS, 60 frames, impulso no frame 30) | `88196450180 / 1734747` | ✅ |
| Binário original no A56 (15×, taskset 80) | `60 / 88196450180 / 1734747` | ✅ |
| Lifter: ld2/st2 concreto × fórmulas simbólicas (memória dos 2 campos + acumulador) | `3/3 loops` | ✅ |
| C de referência sem LLM no host (llvm-mingw, sem flags permissivas) | `60 / 88196450180 / 1734747` | ✅ |
| Gate no host: reconstructed_aos.c do Qwen = C de referência (e conferido no A56, 3×) | `60 / 88196450180 / 1734747` | ✅ |

## 🧮 Instruções ARM64/NEON no lifter

**24 suportadas** · 10 grupos planejados

| | Instrução | Tipo | Observação |
|:---:|---|---|---|
| ✅ | `mov` | escalar | constantes e cópias de registrador |
| ✅ | `movk` | escalar | constantes de 64 bits montadas em partes |
| ✅ | `movi` | NEON | broadcast imediato (.2S / .4S / .2D) |
| ✅ | `dup` | NEON | broadcast de escalar para lanes (`boost` simbólico) |
| ✅ | `ldr` | memória | literal de .rodata (d/q) e ldr q com pós-incremento |
| ✅ | `str` | memória | str q com pós-incremento |
| ✅ | `ldp` | memória | par de registradores Q, base + índice |
| ✅ | `stp` | memória | par de registradores Q → array de saída |
| ✅ | `ld1` | memória | contíguo, 1–4 registradores, pós-incremento por imediato ou registrador |
| ✅ | `st1` | memória | contíguo, 1–4 registradores, pós-incremento |
| ✅ | `ld2` | memória | array of structs de 2 campos: registrador j = campo j de 4 structs |
| ✅ | `st2` | memória | array of structs de 2 campos, com e sem pós-incremento |
| ✅ | `add` | NEON/escalar | por lane; escalar como indução/endereço |
| ✅ | `sub` | NEON | por lane, 32 bits |
| ✅ | `mul` | NEON | por lane, 32 bits |
| ✅ | `and` | NEON | AND bit a bit por lane (máscaras 0xFF / 0xFFFF) |
| ✅ | `eor` | NEON | .8B/.16B tratado como XOR por lane |
| ✅ | `ushr` | NEON | shift lógico à direita por imediato |
| ✅ | `shl` | NEON | shift à esquerda por imediato |
| ✅ | `uaddw` | NEON | acumula lanes baixas estendidas para 64 bits |
| ✅ | `uaddw2` | NEON | acumula lanes altas estendidas para 64 bits |
| ✅ | `subs` | controle | contador de voltas / trip count |
| ✅ | `cmp` | controle | limite em bytes (offset × passo) |
| ✅ | `b.ne` | controle | aresta de retorno do loop |
| ⬜ | `ld3 / ld4 / st3 / st4` | memória | planejado — Fase 3 |
| ⬜ | `orr, bic, mvn` | NEON/escalar | planejado — Fase 2 |
| ⬜ | `sshr, ushl, sshl` | NEON | planejado — Fase 2 |
| ⬜ | `umull, smull, umlal, smlal` | NEON | planejado — Fase 2 |
| ⬜ | `ldr / str escalares gerais` | memória | planejado — Fase 2 |
| ⬜ | `b.cond / cbz / cbnz / tbz` | controle | planejado — Fase 2 |
| ⬜ | `bl / blr / ret` | controle | planejado — Fase 3 |
| ⬜ | `svc #imm (Horizon OS)` | sistema | planejado — Fase 3 |
| ⬜ | `mrs tpidrro_el0 (TLS)` | sistema | planejado — Fase 3 |
| ⬜ | `ldxr / stxr / ldaxr / stlxr` | atômico | planejado — Fase 4 |

## 🔬 Ambiente de execução no A56 (sonda empírica)

Galaxy A56 (SM-A566E), processo de shell via ADB · 2026-10-15 · `horizon_rt/probe/ (probe.c + probe_guest.S)`

| Item | Medido | O que significa |
|---|---|---|
| `tpidrro_el0` | 0 | TLS do Horizon não existe no Android: toda leitura precisa de tratamento |
| `tpidr_el0` | ponteiro do bionic | TLS do próprio Android; não pode ser reaproveitado |
| `cntpct_el0` | SIGILL | Leitura usada por armGetSystemTick() na libnx; no A56 o equivalente é cntvct_el0 |
| `cntvct_el0` | legível em EL0 | Fonte de tempo disponível |
| `cntfrq_el0` | 0x186a000 = 25,6 MHz | Switch: 19,2 MHz; ticks precisam de escala ×0,75 |
| `x18` | ≠ 0 | Shadow Call Stack do bionic: x18 do host precisa ser preservado ao entrar em C |
| `Preservação de registradores` | x1–x29, x18 e NZCV preservados | Medido com sentinelas antes/depois de uma chamada ao handler em C |

**Inventário estático de `hb_compute.nro`** (94.208 bytes (imagem 0x18000)) com `nro_tool.py`:

- Relocations: 337 RELATIVE (RELR, todas em .data)
- Sítios: 12 svc (0x1, 0x2, 0x3, 0x5, 0x6, 0x7, 0x1a, 0x1b, 0x26, 0x27, 0x28, 0x29), 7 mrs tpidrro_el0, 2 mrs cntpct_el0
- Conferência: 21/21 sítios conferidos contra o disassembly do hb_compute.elf

## 🛠️ Toolchain

| Ferramenta | Versão | Papel |
|---|---|---|
| Ghidra | 12.1.4 | Análise headless + descompilação |
| Android NDK | r28 (28.2.13676358) | Clang aarch64-linux-android35 |
| Ollama | 0.40.3 | Servidor local do LLM |
| qwen2.5-coder:7b | 7.6B · Q4_K_M (GGUF) | 5,38 GB; 4,09 GB (~76%) em VRAM na RTX 4050 Laptop (6 GB), resto em CPU |
| Galaxy A56 | SM-A566E | 4×1,95 GHz + 3×2,6 GHz + 1×2,91 GHz |
| llvm-mingw | 20260922 (clang 23.1.2, UCRT x86_64) | Clang nativo do Windows para o gate no host (../tools/, SHA-256 conferido) |
| devkitPro | devkitA64 16.1.0 (GCC) + libnx 2.10.0 | Gera homebrews de teste (NRO + ELF com símbolos) para a análise da Fase 3 |

## 📄 Licença

Código aberto sob a licença **MIT** — veja [LICENSE](LICENSE).

Projeto independente, sem afiliação com Nintendo ou Samsung. Nomes e marcas citados pertencem aos seus respectivos donos e são usados apenas para identificar plataformas.

---

<sub>Painel gerado por <code>update_dashboard.py</code> a partir de <code>status.json</code> · atualizado em 2026-10-15</sub>
