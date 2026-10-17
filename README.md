<!-- Gerado por update_dashboard.py a partir de status.json — não edite à mão. -->
<div align="center">

# 🎮 ➜ 📱 o.b.d.c

### Recompilação estática AArch64 → AArch64: binários de Nintendo Switch rodando nativamente no Android

![Endgame](https://img.shields.io/badge/Endgame-75%25-orange?style=for-the-badge) ![Fase atual](https://img.shields.io/badge/Fase%20atual-3%20%E2%80%93%20Em%20andamento-yellow?style=for-the-badge) ![ISA](https://img.shields.io/badge/ISA-AArch64%20%E2%86%92%20AArch64-informational?style=for-the-badge&logo=arm&logoColor=white) ![Alvo](https://img.shields.io/badge/Alvo-Galaxy%20A56%20%C2%B7%20Exynos%201580-1428A0?style=for-the-badge&logo=samsung&logoColor=white)

![Validação](https://img.shields.io/badge/Valida%C3%A7%C3%A3o-76%2F78%20OK-yellow?style=for-the-badge) ![Benchmarks](https://img.shields.io/badge/Benchmarks-10%20no%20A56-teal?style=for-the-badge&logo=android&logoColor=white) ![Instruções](https://img.shields.io/badge/Instru%C3%A7%C3%B5es-25%20suportadas-blueviolet?style=for-the-badge) ![LLM](https://img.shields.io/badge/LLM-qwen2.5--coder%207b-black?style=for-the-badge&logo=ollama&logoColor=white) ![Atualizado](https://img.shields.io/badge/Atualizado-2026--10--16-lightgrey?style=for-the-badge) [![Licença](https://img.shields.io/badge/Licen%C3%A7a-MIT-green?style=for-the-badge)](LICENSE)

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

## 📈 Progresso até o Endgame: `███████████████░░░░░ 75%`

## 🧩 Módulos atuais

| Módulo | Status | Progresso | O que faz hoje | Próximo |
|---|---|---|---|---|
| **Ghidra Extractor**<br>`ExtractMain.java` | ✅ Concluído | `████████░░ 80%` | C descompilado + ASM AArch64 + .rodata + isolamento de loops + nomes dos alvos de bl e strings carregadas (formatos de printf) + ponteiros de função referenciados por endereço (callbacks como o hook do applet, função criada no Ghidra se faltar) | Tipos de ponteiro/structs, objetos locais da pilha passados por endereço |
| **NEON Symbolic Lifter**<br>`neon_lifter.py` | 🟡 Em andamento | `██████████ 96%` | Elevação simbólica NEON e escalar sem código por binário (Clang do NDK e GCC do devkitA64): memória contígua e array of structs, [base, índice], pós-incremento, loops por ponteiro, estado de saída dos loops anteriores; esqueleto do main e das funções auxiliares gerado do CFG, com laços escalares deixados ao estruturador, 9 instruções escalares (umull, madd, msub, bic, mvn, ubfiz, ubfx, ldrb, ldrh), objetos locais da pilha passados por endereço e if/else dentro de laços (hb_fb.elf reconstruído) | Rodar o check_baseline.sh (5 JSONs de referência com os valores do aparelho) antes de toda mudança no estruturador e no lifter; um homebrew real com mrs tpidrro_el0 no código reconstruído (hoje só testes unitários e prova de ponta a ponta); depois switch, ld3/ld4 com binário real |
| **Local LLM Qwen 2.5 7B**<br>`qwen2.5-coder:7b (Ollama, CUDA parcial)` | 🟡 Em andamento | `████████░░ 80%` | Gera C a partir do resumo do lifter + esqueleto do CFG; checagem de estrutura e gate no host antes do ADB, com 1 nova tentativa guiada. Se o Qwen esgota as tentativas (ou o Ollama falha), o pipeline usa o C de referência determinístico e registra o motivo em <saida>.pipeline.json | Fazer o Qwen respeitar for (;;) com break do GCC e renomear variáveis de registrador |
| **Android AArch64 Runner**<br>`Galaxy A56 via ADB` | 🟡 Em andamento | `████░░░░░░ 45%` | push + chmod + execução em /data/local/tmp, medições intercaladas (15×) | Script único de round-trip (compilar → push → rodar → comparar), afinidade de núcleo (taskset) |
| **Switch HLE Shim**<br>`switch_hle_shim.h` | 🟡 Em andamento | `██████████ 95%` | Primitivas libnx, 29 SVCs do kernel (24 implementados sobre Bionic/POSIX e 5 de IPC/memória compartilhada como dummies), IPC CMIF (9c818ca): svcConnectToNamedPort/svcSendSyncRequest leem a mensagem SFCI no buffer da TLS (shimGetTls, direta ou dentro de HIPC em +0x10) e respondem SFCO para sm: (Initialize, GetService com handle novo), time (cmd 0: timestamp), set (cmd 0: en-US) e stubs; TLS do Horizon (shimGetTls, ThreadVars), transfer memory completa (svcCreateTransferMemory/svcMapTransferMemory/svcUnmapTransferMemory e tmemCreate/Map/Unmap/Close: origem protegida com a permissão pedida, alias real por memfd), API Thread/Mutex/RMutex/CondVar, time:u (timeGetCurrentTime sobre CLOCK_REALTIME, microssegundos, steady clock, calendário local), ciclo de vida do applet (hooks, foco, modo de operação, pedido de saída com appletLockExit, fila de mensagens entregue no appletMainLoop), framebuffer linear simulado (framebufferCreate/MakeLinear/Begin/End, RGBA8888/RGBX/BGRA/565/4444, checksum FNV-1a e PPM da tela), fs de alto nível (fsOpenSdCardFileSystem + fsFs/fsFile/fsDir sobre um diretório do host, portátil Bionic/Windows, caminhos normalizados contra ".."), dispositivos do stdio (fsdevMountSdmc/MountDevice/TranslatePath e romfsInit/MountSelf somente leitura, com fopen/opendir/stat/... roteados para sdmc:/ e romfs:/) e entrada desacoplada (roteiro por quadro + buffer ao vivo com seqlock, botões e analógicos, PadState compacta inalterada), com os tipos e resultados da libnx 2.10.0 | CMIF fiel por serviço: time:u cmd 0 devolve a sessão ISystemClock (hoje o timestamp vem direto), comandos de hid e fsp-srv, e erro para comando desconhecido (hoje o stub devolve sucesso); atualizar 0x1F/0x21 no svc_table.json; padRepeater e caminhos relativos a partir de sdmc:/ |
| **Horizon Static Analysis**<br>`horizon_rt/ (nro_tool.py, probe/)` | 🟡 Em andamento | `████████░░ 85%` | Leitor de NRO (NRO0, MOD0, segmentos, .dynamic, relocations, ASET com ícone/NACP/RomFS) com inventário de svc/mrs conferido contra o ELF em 3 homebrews (hb_compute: 21 sítios, hb_threads: 36, hb_services: 110); quem chama cada SVC de IPC, porta sm: e nomes de serviço no binário; pipeline direto NRO → neon_lifter (desmonta o .text do NRO, usa os símbolos do .elf ao lado) validado no A56 com hb_compute.nro | NRO stripped: descobrir limites de função por análise de branch / fecho do CFG, sem constantes fixas por binário (hoje main 0x100180/544 e sub_100430 = out); fixtures portáteis para o test_nro_pipeline.py em clone limpo |

## 🗺️ Roadmap de subsistemas

| Fase | Subsistema | Status | Progresso |
|:---:|---|---|---|
| **1** | Lifter NEON Aritmético | ✅ Concluído | `██████████ 100%` |
| **2** | Memória, Arrays e Ponteiros | ✅ Concluído | `██████████ 100%` |
| **3** | Loader ELF e Tradução de ABI / Syscalls | 🟡 Em andamento | `██████████ 99%` |
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
<summary><b>Fase 3 — Loader ELF e Tradução de ABI / Syscalls</b> (20/21)</summary>

- [x] Shim HLE de primitivas libnx (console, applet, pad, timer) — switch_hle_shim.h
- [x] Homebrew estilo libnx compilado do fonte e validado no A56 (switch_particle_bench)
- [x] Leitor NRO: NRO0, segmentos, MOD0, .dynamic, relocations RELA/RELR (nro_tool.py)
- [x] Inventário de sítios svc / mrs tpidrro_el0 / cntpct_el0 conferido contra o ELF original
- [x] Sonda do ambiente no A56 (registradores de sistema em EL0, x18, preservação de registradores)
- [x] Homebrew mínimo de teste (devkitPro + libnx) com oráculo pelo switch_hle_shim.h
- [x] Mapa de svc do Horizon → chamadas Bionic/POSIX (horizon_rt/svc_table.json, 12 SVCs do hb_compute)
- [x] Handlers HLE dos SVCs no switch_hle_shim.h, testados no A56 (svc_shim_test: 33/33)
- [x] C reconstruído do hb_compute.elf (devkitA64/GCC) pelo pipeline: lifter + esqueleto do CFG, validado no host e no A56
- [x] Threads, sleep e sincronização da libnx no shim (9 SVCs + Thread/Mutex/RMutex/CondVar), svc_shim_test 46/46 no A56
- [x] Homebrew multi-thread hb_threads (devkitA64) inventariado e com oráculo no A56 em 1 e 4 núcleos
- [x] Fallback determinístico no pipeline: Qwen reprovado → C de referência validado no gate (hb_compute)
- [x] Inventário IPC com hb_services (sm, applet, hid, fs): 6 SVCs de IPC/memória compartilhada mapeados, dummies no shim
- [x] fs (cartão SD sobre diretório do host) e entrada desacoplada (roteiro + buffer ao vivo, analógicos) no shim; svc_shim_test 124/124 no A56 e 70/70 no host; hb_io validado
- [x] RomFS (romfs:/ somente leitura) e fsdev (sdmc:/) com stdio roteado no shim; svc_shim_test 153/153 no A56 e 99/99 no host; hb_assets validado
- [x] time:u, ciclo de vida do applet (foco, hooks, pedido de saída) e framebuffer linear 1280x720 no shim; svc_shim_test 192/192 no A56 e 138/138 no host; hb_fb validado (checksum e PPM = referência numpy)
- [x] Transfer memory completa (0x15 / 0x51 / 0x52 + tmem*, alias por memfd) no shim; svc_shim_test 210/210 no A56 (15×); regressão de 12 binários idêntica
- [x] Reconstrução C do hb_fb.elf (instruções escalares e if/else no estruturador): soma ponderada e FNV-1a iguais ao oráculo no A56 (15×, taskset 80)
- [x] Shim de libnx completo (fs, hid real, time, serviços IPC): fs/hid/time/applet na API de alto nível + dispatcher CMIF (SFCI/SFCO) no buffer da TLS para sm: (Initialize, GetService), time, set e stubs; cmif_test 35/35 no host e no A56
- [x] TLS (tpidrro_el0) / registrador x18 e convenções de ABI: shimGetTls no shim (219/219 no A56) e mrs tpidrro_el0 → SHIM_MRS_TPIDRRO no esqueleto (test_mrs_tls 4/4, ponta a ponta no A56), x18 intocado
- [ ] Primeiro homebrew binário (NRO) traduzido rodando no A56 — Pipeline NRO com símbolos ELF: validado no Galaxy A56 (hb_compute.nro → neon_lifter → NDK r28 sem warnings, 15/15 execuções, checksum 88196450180). Pendente para 100%: desacoplamento do ELF (descoberta de limites de função por análise de branch / fecho do CFG em NRO stripped, sem constantes fixas por binário) e fixtures portáteis para clone limpo

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
| **`hb_compute`**<br>Homebrew mínimo libnx (sem serviços): 100.000 partículas × 60 frames, saída por svcOutputDebugString | ELF do devkitA64 (GCC 16.1.0): str/ldr q [base, índice], dup, eor, and, ushr, loop escalar ldr w pós-incremento, mrs cntpct_el0, ccmp, umulh | Checksum: `88196450180`<br>Última partícula: `1734747` | 0.0043 s | — | — | 📱 `reconstructed_hb_compute.c` |
| **`hb_threads`**<br>Homebrew libnx multi-thread: 4 threads × 25.000 partículas × 60 frames, Mutex + CondVar + sleep | threadCreate/Start/WaitForExit/Close, mutexLock/Unlock, condvarWait/WakeAll, svcSleepThread (oráculo pelo shim) | Checksum: `88196450180`<br>Última partícula: `1734747`<br>Threads: `4` | 0.0061 s | — | — | 🎮 `hb_threads_oracle` |
| **`hb_services`**<br>Homebrew libnx com serviços: sm, applet, hid e fs abertos e fechados, 60 frames de padUpdate | smInitialize, appletInitialize, hidInitialize, fsInitialize, padUpdate, appletMainLoop (IPC dummy no shim) | Servicos abertos: `4`<br>Frames: `60`<br>Botoes pressionados: `2` | — | — | — | 🎮 `hb_services_oracle` |
| **`hb_io`**<br>Homebrew libnx com cartão SD e analógicos: 1 MiB escrito e lido pelo fs + 60 frames de pad | fsOpenSdCardFileSystem, fsFsCreateDirectory/CreateFile/OpenFile/RenameFile/OpenDirectory/DeleteDirectoryRecursively, fsFileWrite (Append)/Read/GetSize/Flush, fsDirGetEntryCount, padUpdate, padGetStickPos | FNV-1a (1 MiB): `0x3cc5e3ccd09fa325`<br>Frames: `60`<br>Botoes pressionados: `2`<br>Analógicos (Σ lx / ly / rx): `-46080 / 514560 / 655340` | — | — | — | 🎮 `hb_io_oracle` |
| **`hb_assets`**<br>Homebrew libnx com RomFS: 256 KiB de assets lidos por romfs:/ (stdio) com CRC-32, gravação em sdmc:/ | romfsInit, fsdevMountSdmc, fopen/fread/fseek/fgets/stat/opendir/remove com romfs:/ e sdmc:/, romfsExit, fsdevUnmountAll | CRC-32: `0x5d07c479`<br>FNV-1a: `0x3e52039d8d7f4325`<br>Bytes em 0x10000: `8d34db82`<br>Entradas em romfs:/: `2`<br>Acessos recusados no romfs: `2/2` | — | — | — | 🎮 `hb_assets_oracle` |
| **`hb_fb`**<br>Homebrew libnx com framebuffer linear: 60 frames 1280x720 RGBA8888 por software + relógio + ciclo de vida do applet | timeInitialize/timeGetCurrentTime/timeToCalendarTimeWithMyRule, appletLockExit/appletHook/appletGetFocusState/appletIsAutoPowerDownRequested, framebufferCreate/MakeLinear/Begin/End/Close, appletMainLoop até o pedido de saída | Frames: `60`<br>Soma ponderada (60 frames): `0x1b5c804513888ea0`<br>FNV-1a do último frame: `0x2a45a5f1c5fad331`<br>Saída: `pedido do sistema (OnExitRequest x1)` | 0.0028 s | — | — | 📱 `reconstructed_hb_fb.c` |

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
| C de referência regerado pelo pipeline atual no host (regressão de 0867da6 corrigida em a712dcc; check_baseline.sh) | `60 / 88196450180 / 1734747` | ✅ |

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
| C de referência regerado pelo pipeline atual no host (regressão de 0867da6 corrigida em a712dcc; check_baseline.sh) | `60 / 88196450180 / 1734747` | ✅ |

### `hb_compute` — Homebrew mínimo libnx (sem serviços): 100.000 partículas × 60 frames, saída por svcOutputDebugString

> 📱 C reconstruído do hb_compute.elf (devkitA64) pelo pipeline: Ghidra extrai main e out(); o lifter eleva os 2 loops NEON e o loop escalar; o esqueleto do CFG traz out() com chamada de cauda, mrs cntpct_el0 -> armGetSystemTick(), ccmp e umulh. O Qwen foi reprovado nas duas tentativas e o pipeline aplicou o fallback determinístico: reconstructed_hb_compute.c é o C de referência (relatório em reconstructed_hb_compute.pipeline.json).

**Round-trip binário → LLM → C**

| # | Entrada para o LLM | Binário | Fórmula | Iterações | Resultado da reconstrução | LLM (s) |
|:---:|---|---|:---:|:---:|---|---:|
| 1 | Resumo do lifter + esqueleto do CFG (main + out) | `hb_compute.elf` | ✅ | ❌ | —<br><sub>Qwen moveu armGetSystemTick e o loop de física para fora do for (;;) e removeu w2 = 1; barrado pela checagem (2 tentativas)</sub> | 59.1 |
| 2 | Mesmo resumo + regra de copiar o esqueleto linha por linha | `hb_compute.elf` | ✅ | ❌ | —<br><sub>reprovado de novo pela checagem de estrutura; fallback determinístico aplicado (C final = C de referência)</sub> | 78.3 |

📱 = recompilado e executado no Galaxy A56 · 🐍 = verificado em Python · ❌ = diverge do esperado

**Execução no aparelho**

| Binário | Execuções | Mediana | Mínimo | Resultado |
|---|:---:|---:|---:|---|
| hb_oracle (shim com os handlers de SVC, taskset cpu7) | 15 | 0.0043 s | 0.0043 s | `88196450180` ✅ |
| hb_ref (C reconstruído do ELF, taskset cpu7) | 15 | 0.0063 s | 0.0058 s | `88196450180` ✅ |
| hb_oracle (intercalado A/B, cpu7) | 20 | 0.0045 s | 0.0043 s | `88196450180` ✅ |
| hb_ref (intercalado A/B, cpu7) | 20 | 0.0052 s | 0.0044 s | `88196450180` ✅ |
| reconstructed_hb_compute_bin (fallback determinístico, taskset cpu7) | 15 | 0.0066 s | 0.0063 s | `88196450180` ✅ |
| C traduzido do hb_compute.nro (neon_lifter app.nro, símbolos do .elf ao lado; NDK r28, taskset cpu7) | 15 | 0.0044 s | 0.0043 s | `88196450180` ✅ |

**Validação**

| Verificação | Valor | |
|---|---|:---:|
| Oráculo no host (llvm-mingw) | `60 / 88196450180 / 1734747` | ✅ |
| Oráculo no A56 (15×, taskset 80) | `60 / 88196450180 / 1734747` | ✅ |
| hb_compute.nro: 21 sítios do nro_tool.py conferidos contra o hb_compute.elf | `21/21` | ✅ |
| Handlers dos SVCs no A56 (svc_shim_test, sucesso e erros da libnx) | `33/33` | ✅ |
| Lifter: 2 loops NEON + 1 escalar do ELF do devkitA64 (instruções × fórmulas) | `5/5 comparações` | ✅ |
| C reconstruído do ELF no host (llvm-mingw) | `60 / 88196450180 / 1734747` | ✅ |
| C reconstruído do ELF no A56 (15×, taskset 80) | `60 / 88196450180 / 1734747` | ✅ |
| Desempenho do C reconstruído vs fonte via shim (20 pares intercalados, razão mediana) | `1,051×` | ✅ |
| Fallback determinístico: C final idêntico ao de referência, gate no host e A56 (15×) | `60 / 88196450180 / 1734747` | ✅ |
| C de referência regerado pelo pipeline atual no host (regressão de 0867da6 corrigida em a712dcc; check_baseline.sh) | `60 / 88196450180 / 1734747` | ✅ |
| Pipeline NRO com símbolos ELF: hb_compute.nro → neon_lifter (7e2e2c0) → NDK r28 (0 warnings) → A56 (15×, taskset 80); test_nro_pipeline.py 6/6 | `60 / 88196450180 / 1734747, 15/15` | ✅ |
| NRO stripped (sem o .elf ao lado): limites de função sem símbolos | `pendente: o esqueleto para em desvio para fora da função (main cortado pela heurística; constantes fixas do hb_compute em nro_tool.py)` | ⏳ |
| test_nro_pipeline.py em clone limpo | `pendente: depende de hb_compute.nro/.elf ignorados pelo git e de OBDC_HOST_CC no gate (fixtures portáteis)` | ⏳ |

### `hb_threads` — Homebrew libnx multi-thread: 4 threads × 25.000 partículas × 60 frames, Mutex + CondVar + sleep

> 🎮 Mesmo hb_threads.c do NRO do devkitA64 (21 SVCs, 36 sítios conferidos pelo nro_tool.py), compilado para Android com -DOBDC_ORACLE: as threads da libnx viram pthreads e a sincronização usa futex. Mesmo checksum em 1 núcleo (taskset 80) e em 4 (taskset f0).

**Execução no aparelho**

| Binário | Execuções | Mediana | Mínimo | Resultado |
|---|:---:|---:|---:|---|
| hb_threads_oracle (taskset 80, 1 núcleo) | 15 | 0.0124 s | 0.0111 s | `88196450180` ✅ |
| hb_threads_oracle (taskset f0, 4 núcleos grandes) | 15 | 0.0061 s | 0.0051 s | `88196450180` ✅ |

**Validação**

| Verificação | Valor | |
|---|---|:---:|
| hb_threads.nro: sítios do nro_tool.py conferidos contra o hb_threads.elf | `36/36 (21 svc)` | ✅ |
| Oráculo no A56 em 1 núcleo (15×) | `60 / 4 / 88196450180 / 1734747` | ✅ |
| Oráculo no A56 em 4 núcleos (15×) | `60 / 4 / 88196450180 / 1734747` | ✅ |
| svc_shim_test no A56 (27 SVCs + Thread/Mutex/RMutex/CondVar + IPC dummy) | `54/54` | ✅ |

### `hb_services` — Homebrew libnx com serviços: sm, applet, hid e fs abertos e fechados, 60 frames de padUpdate

> 🎮 Inventário do NRO (devkitA64): 21 SVCs, 6 novos de IPC/memória compartilhada (0x13, 0x14, 0x17, 0x1F, 0x21, 0x52), porta sm: e 80 leituras de tpidrro_el0 (buffer de IPC da TLS). O oráculo roda pelos dummies do shim: o pad determinístico dá A no frame 30 e Plus no 60.

**Execução no aparelho**

| Binário | Execuções | Mediana | Mínimo | Resultado |
|---|:---:|---:|---:|---|
| hb_services_oracle (taskset cpu7) | 1 | — | — | `4 / 60 / 2` ❌ |

**Validação**

| Verificação | Valor | |
|---|---|:---:|
| hb_services.nro: 110 sítios do nro_tool.py conferidos contra o hb_services.elf | `110/110 (21 svc)` | ✅ |
| Oráculo no A56 e no host (llvm-mingw) | `4 serviços / 60 frames / 2 botões` | ✅ |
| svc_shim_test no A56 com os SVCs de IPC dummy | `54/54` | ✅ |

### `hb_io` — Homebrew libnx com cartão SD e analógicos: 1 MiB escrito e lido pelo fs + 60 frames de pad

> 🎮 Mesmo hb_io.c compilado com a libnx 2.10.0 real (devkitA64, -Wall -Wextra sem warnings: as assinaturas do shim batem com o switch.h) e como oráculo pelo shim (NDK r28 e llvm-mingw). No NRO, fs e hid passam por IPC (svcSendSyncRequest): 148 sítios, 23 SVCs, todos já tratados pelo shim. No oráculo o cartão SD é ./sdcard e o pad segue um roteiro (A no 30, Plus no 60, analógico esquerdo em rampa, direito no máximo do 20 ao 39).

**Execução no aparelho**

| Binário | Execuções | Mediana | Mínimo | Resultado |
|---|:---:|---:|---:|---|
| hb_io_oracle (taskset cpu7) | 15 | — | — | `0x3cc5e3ccd09fa325` ✅ |

**Validação**

| Verificação | Valor | |
|---|---|:---:|
| Referência Python (FNV-1a do padrão de 1 MiB e somas do roteiro de analógicos) | `0x3cc5e3ccd09fa325 / -46080 / 514560 / 655340` | ✅ |
| Oráculo no host (llvm-mingw) e no A56 (15×, taskset 80): saída idêntica | `0x3cc5e3ccd09fa325 / 60 / 2 / -46080 / 514560 / 655340` | ✅ |
| hb_io.nro: sítios do nro_tool.py conferidos contra o hb_io.elf | `148 (23 svc), OK` | ✅ |
| svc_shim_test: fs + entrada portáveis no host (llvm-mingw) | `70/70` | ✅ |
| svc_shim_test no A56 (taskset 80 e 5× taskset f0: seqlock sem estados misturados, symlink fora da raiz preservado) | `124/124` | ✅ |
| Regressão: 9 binários da suíte com o shim novo × antigo no A56 (15× taskset 80; hb_threads também f0) e no host | `saída idêntica, valores do status.json` | ✅ |
| Regressão: main() dos 9 binários (NDK r28) igual instrução a instrução, só endereços de .rodata mudam; frames de pilha idênticos | `9/9` | ✅ |
| -Wall -Wextra sem warnings: shim + suíte (NDK r28 e llvm-mingw) e hb_io (devkitA64) | `0 warnings` | ✅ |

### `hb_assets` — Homebrew libnx com RomFS: 256 KiB de assets lidos por romfs:/ (stdio) com CRC-32, gravação em sdmc:/

> 🎮 Assets gerados por gen_romfs.py (que também calcula o CRC-32 pelo zlib e o FNV-1a). O mesmo hb_assets.c vira NRO com o RomFS embutido (devkitA64 + libnx 2.10.0, elf2nro --romfsdir) e oráculo pelo shim, no qual romfs:/ é um diretório do host (OBDC_ROMFS=/data/local/tmp/romfs no A56) e fopen/stat/opendir passam pelos wrappers de dispositivo. Escrita e ".." acima da raiz do romfs são recusados (EROFS / EACCES). Leitura de 256 KiB + CRC + FNV no A56: mediana 1,11 ms (0,81–1,60, 15×, cpu7).

**Execução no aparelho**

| Binário | Execuções | Mediana | Mínimo | Resultado |
|---|:---:|---:|---:|---|
| hb_assets_oracle (taskset cpu7, romfs em /data/local/tmp/romfs) | 15 | 0.0011 s | 0.0008 s | `0x5d07c479` ✅ |

**Validação**

| Verificação | Valor | |
|---|---|:---:|
| Referência Python (gen_romfs.py: zlib.crc32 e FNV-1a do bloco) | `0x5d07c479 / 0x3e52039d8d7f4325 / 8d34db82` | ✅ |
| Oráculo no host (llvm-mingw) e no A56 (15×, taskset 80): saída idêntica | `0x5d07c479 / 2 entradas / 2/2 recusados / sdmc ok` | ✅ |
| hb_assets.nro (RomFS embutido): sítios do nro_tool.py conferidos contra o hb_assets.elf | `89 (18 svc), OK` | ✅ |
| svc_shim_test: romfs:/, sdmc:/, fsdev e stdio roteado no host (llvm-mingw) | `99/99` | ✅ |
| svc_shim_test no A56 (10× taskset 80 + 5× taskset f0) | `153/153` | ✅ |
| Regressão: 10 binários da suíte com o shim novo × anterior no A56 (15× taskset 80; hb_threads também f0) e no host; main() igual instrução a instrução | `10/10` | ✅ |
| -Wall -Wextra sem warnings: shim + suíte (NDK r28 e llvm-mingw) e hb_assets (devkitA64) | `0 warnings` | ✅ |

### `hb_fb` — Homebrew libnx com framebuffer linear: 60 frames 1280x720 RGBA8888 por software + relógio + ciclo de vida do applet

> 📱 Barras de cor rolando, degradê animado e quadrado em movimento; a animação depende só do número do quadro (o tick do sistema só mede o tempo de render), por isso o checksum é determinístico. O mesmo hb_fb.c compila com a libnx 2.10.0 real (devkitA64, 0 warnings) e como oráculo pelo shim, no qual o sistema pede a saída depois do quadro 60 (appletLockExit + hook OnExitRequest, appletMainLoop devolve false). A tela apresentada pelo shim é conferida com o FNV-1a calculado pelo app e gravada em PPM, idêntico byte a byte à referência numpy (ref_hb_fb.py) no host e no A56. Render por software no A56: mediana 2,82 ms/frame (2,79–2,91, 15×, cpu7). O hb_fb.nro real usa svcCreateTransferMemory (0x15), agora tratado pelo shim. Reconstrução do hb_fb.elf pelo pipeline (Ghidra: main + out + on_applet → cfg_skeleton/neon_lifter → reconstructed_hb_fb.c, sem LLM): com OBDC_INPUT=none, 60 frames, saída pelo pedido do sistema, soma ponderada e FNV-1a iguais ao oráculo no host e no A56 (15×, taskset 80), render 3,56 ms/frame. Com 0036de1 a checagem do relógio também confere (load de x20 do objeto da pilha sp+0x68 preservado, if/else aninhado completo, assinatura do on_applet corrigida): saída inteira igual ao oráculo e 0 warnings.

**Execução no aparelho**

| Binário | Execuções | Mediana | Mínimo | Resultado |
|---|:---:|---:|---:|---|
| hb_fb_oracle (taskset cpu7, tempo de render por frame) | 15 | 0.0028 s | 0.0028 s | `0x2a45a5f1c5fad331` ✅ |
| reconstructed_hb_fb (C reconstruído do ELF, NDK r28 -O2, taskset cpu7, OBDC_INPUT=none) | 15 | 0.0035 s | 0.0035 s | `0x2a45a5f1c5fad331` ✅ |

**Validação**

| Verificação | Valor | |
|---|---|:---:|
| Referência numpy (ref_hb_fb.py): soma ponderada dos 60 frames e FNV-1a do último | `0x1b5c804513888ea0 / 0x2a45a5f1c5fad331` | ✅ |
| Oráculo no host (llvm-mingw) e no A56 (15×, taskset 80): saída idêntica | `60 frames / 0x1b5c804513888ea0 / 0x2a45a5f1c5fad331 / OnExitRequest x1` | ✅ |
| PPM da tela (shimFramebufferDumpPPM) = último frame da referência, host e A56 | `2.764.816 bytes, idêntico` | ✅ |
| hb_fb.nro: sítios do nro_tool.py conferidos contra o hb_fb.elf | `210 (24 svc, 0x15 novo), OK` | ✅ |
| svc_shim_test no A56 com transfer memory (0x15/0x51/0x52), 10× taskset 80 + 5× taskset f0 | `210/210` | ✅ |
| Regressão: 12 binários (suíte + hb_fb) com transfer memory, fim dos 60 quadros como pedido de saída e OBDC_INPUT, × shim anterior no A56 e no host; main() igual | `12/12` | ✅ |
| Pipeline no hb_fb.elf: Ghidra (main + out + on_applet) → cfg_skeleton/neon_lifter → reconstructed_hb_fb.c → A56 (FNV 0x2a45a5f1c5fad331) | `60 frames / 0x1b5c804513888ea0 / 0x2a45a5f1c5fad331 / OnExitRequest x1, 15/15 (host também)` | ✅ |
| C reconstruído: linha do relógio (time:u) igual ao oráculo | `coerente com time() (0036de1: load de x20 da pilha preservado, if/else aninhado completo), 15/15 no A56` | ✅ |
| C reconstruído: -Wall -Wextra (llvm-mingw / NDK r28) | `0 / 0 warnings` | ✅ |
| C reconstruído = saída do pipeline (neon_lifter --emit-c sobre ghidra_extraction_hb_fb.json) | `idêntico` | ✅ |
| svc_shim_test no A56 com TLS do Horizon (shimGetTls, 8 pthreads paralelas, x18 preservado), 10× taskset 80 + 5× taskset f0 | `219/219 (host: 145/145)` | ✅ |
| svc_shim_test: time, applet e framebuffer portáveis no host (llvm-mingw) | `138/138` | ✅ |
| svc_shim_test no A56 (10× taskset 80 + 5× taskset f0) | `192/192` | ✅ |
| Regressão: 11 binários da suíte com o shim novo × anterior no A56 (15× taskset 80; hb_threads também f0) e no host; main() igual instrução a instrução | `11/11` | ✅ |
| -Wall -Wextra sem warnings: shim + suíte (NDK r28 e llvm-mingw) e hb_fb (devkitA64) | `0 warnings` | ✅ |

## 🧮 Instruções ARM64/NEON no lifter

**25 suportadas** · 9 grupos planejados

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
| ✅ | `mrs tpidrro_el0` | sistema | TLS do Horizon: vira SHIM_MRS_TPIDRRO(xN) = shimGetTls() da thread (x18 intocado) |
| ⬜ | `ld3 / ld4 / st3 / st4` | memória | planejado — Fase 3 |
| ⬜ | `orr, bic, mvn` | NEON/escalar | planejado — Fase 2 |
| ⬜ | `sshr, ushl, sshl` | NEON | planejado — Fase 2 |
| ⬜ | `umull, smull, umlal, smlal` | NEON | planejado — Fase 2 |
| ⬜ | `ldr / str escalares gerais` | memória | planejado — Fase 2 |
| ⬜ | `b.cond / cbz / cbnz / tbz` | controle | planejado — Fase 2 |
| ⬜ | `bl / blr / ret` | controle | planejado — Fase 3 |
| ⬜ | `svc #imm (Horizon OS)` | sistema | planejado — Fase 3 |
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

## 🧷 SVCs do Horizon → Bionic/POSIX

29 SVCs (libnx 2.10.0) · especificação em `horizon_rt/svc_table.json` · handlers em `switch_hle_shim.h` · teste `horizon_rt/svc_shim_test.c`

| SVC | Função | Tradução Bionic/POSIX | Testado no A56 |
|:---:|---|---|---|
| `0x01` | `svcSetHeapSize` | `memfd_create + ftruncate (reserva de 4 GiB)`<br>`mmap(PROT_NONE, MAP_SHARED\|MAP_NORESERVE)`<br>`mprotect(PROT_READ\|PROT_WRITE) para crescer`<br>`madvise(MADV_REMOVE) + mprotect(PROT_NONE) para encolher` | 4 MiB, escrita/leitura, encolher para 2 MiB, tamanho invalido |
| `0x02` | `svcSetMemoryPermission` | `mprotect` | Perm_R, Perm_Rw, Perm_Rx invalido, endereco desalinhado |
| `0x03` | `svcSetMemoryAttribute` | — <sub>Sem equivalente POSIX (cache de pagina nao e controlavel em user space): o atributo e validado e registrado, e aparece no svcQueryMemory; nao altera o hardware.</sub> | liga e limpa IsUncached, IsBorrowed recusado |
| `0x04` | `svcMapMemory` | `mmap(dst, MAP_SHARED\|MAP_FIXED, memfd da heap, offset da origem)`<br>`mprotect(origem, PROT_NONE)` | alias le e escreve a mesma memoria da origem; origem None + IsBorrowed |
| `0x05` | `svcUnmapMemory` | `mmap(dst, PROT_NONE, MAP_ANONYMOUS\|MAP_FIXED\|MAP_NORESERVE) (volta a reserva)`<br>`mprotect(origem, PROT_READ\|PROT_WRITE)` | dado escrito pelo alias permanece na origem; argumentos errados recusados |
| `0x06` | `svcQueryMemory` | `leitura de /proc/self/maps` | heap, heap encolhida, origem emprestada, alias mapeado e desmapeado |
| `0x07` | `svcExitProcess` | `fflush(NULL)`<br>`_exit(0) (exit_group)` | fim do svc_shim_test com codigo 0 |
| `0x08` | `svcCreateThread` | `tabela de handles do shim (sem chamada ao kernel ate o svcStartThread)` | 4 threads via threadCreate; prioridade 64 e cpuid 7 recusados |
| `0x09` | `svcStartThread` | `pthread_create` | 4 threads via threadStart; handle invalido recusado |
| `0x0A` | `svcExitThread` | `sinaliza o handle (pthread_cond_broadcast)`<br>`pthread_exit` | thread que chama svcExitThread no meio da funcao: codigo depois dele nao roda e quem espera acorda |
| `0x0B` | `svcSleepThread` | `nanosleep (repetido em EINTR)`<br>`sched_yield (nano <= 0)` | 15 ms dormiu 15,13 ms; 0 e -1 fazem yield |
| `0x13` | `svcMapSharedMemory` | 🧪 **dummy** · — <sub>Dummy: valida alinhamento e devolve sucesso sem mapear. Na libnx: shmemMap (usado por hidInitialize).</sub> | sucesso e endereco desalinhado |
| `0x14` | `svcUnmapSharedMemory` | 🧪 **dummy** · — <sub>Dummy. Na libnx: shmemUnmap (hidExit).</sub> | devolve sucesso |
| `0x15` | `svcCreateTransferMemory` | `mprotect(addr, size, perm) na origem`<br>`handle na tabela do HLE com o memfd da origem (heap do svcSetHeapSize ou memfd_create do tmemCreate)` | tmemCreate Perm_None (leitura da origem -> SIGSEGV), heap Perm_R, origem em malloc, erros de alinhamento/perm/sobreposicao |
| `0x16` | `svcCloseHandle` | `pthread_join (thread ja terminada)`<br>`pthread_detach (ainda rodando: o objeto e liberado quando ela termina)` | threadClose depois de threadWaitForExit; thread criada e nunca iniciada |
| `0x17` | `svcResetSignal` | 🧪 **dummy** · — <sub>Dummy. Na libnx: waitObjects (espera de eventos).</sub> | devolve sucesso |
| `0x18` | `svcWaitSynchronization` | `pthread_cond_wait / pthread_cond_timedwait (CLOCK_REALTIME)` | threadWaitForExit x5; 20 ms numa thread parada -> TimedOut em 20,1 ms |
| `0x1A` | `svcArbitrateLock` | `futex(FUTEX_WAIT \| FUTEX_PRIVATE_FLAG)`<br>`__atomic_compare_exchange para tomar a posse quando a tag vira 0` | mutex no algoritmo da libnx, 4 threads x 200.000 secoes criticas, contagem exata |
| `0x1B` | `svcArbitrateUnlock` | `store atomico de 0`<br>`futex(FUTEX_WAKE, 1)` | mesmo teste de 4 threads; mutex termina em 0 |
| `0x1C` | `svcWaitProcessWideKeyAtomic` | `futex(FUTEX_WAIT \| FUTEX_PRIVATE_FLAG, timeout relativo) na chave`<br>`mutexUnlock / mutexLock da libnx sobre svcArbitrateLock/Unlock` | produtor/consumidor com 1.000 itens; condvarWaitTimeout(10 ms) -> TimedOut e volta com o mutex |
| `0x1D` | `svcSignalProcessWideKey` | `incremento atomico da chave`<br>`futex(FUTEX_WAKE)` | condvarWakeAll no produtor/consumidor e no portao de largada do hb_threads |
| `0x1F` | `svcConnectToNamedPort` | 🧪 **dummy** · `tabela de handles do shim (sessao ficticia)` | "sm:" devolve sessao; nome longo recusado |
| `0x21` | `svcSendSyncRequest` | 🧪 **dummy** · — <sub>Dummy: valida a sessao e devolve sucesso sem processar a mensagem. Na libnx: serviceDispatch/cmif via smInitialize, appletInitialize, hidInitialize, fsInitialize e os cleanups (27 chamadas em _appletCleanup).</sub> | sessao valida, invalida e fechada |
| `0x26` | `svcBreak` | `fprintf(stderr)`<br>`abort() (SIGABRT) quando nao e so notificacao` | notificacao volta 0; Panic encerra o filho com SIGABRT |
| `0x27` | `svcOutputDebugString` | `fflush(stdout)`<br>`write(STDOUT_FILENO)` | saida do svc_shim_test e do oraculo hb_compute |
| `0x28` | `svcReturnFromException` | `abort() (SIGABRT)` | encerra o filho com SIGABRT |
| `0x29` | `svcGetInfo` | `sched_getaffinity (CoreMask)`<br>`sysinfo (TotalMemorySize)`<br>`/proc/self/statm (UsedMemorySize)`<br>`/proc/self/status TracerPid (DebuggerAttached)`<br>`getrandom (RandomEntropy)`<br>`regioes reservadas pelo shim (Heap/Alias)` | Alias/Heap, CoreMask, memoria total e usada, depurador, entropia, tipo 65000, handle invalido |
| `0x51` | `svcMapTransferMemory` | `mmap(addr, MAP_SHARED \| MAP_FIXED, memfd da origem, deslocamento)` | alias le e escreve a origem (memfd do tmemCreate e heap), svcQueryMemory TransferMem/TransferMemIsolated, perm/tamanho/regiao errados |
| `0x52` | `svcUnmapTransferMemory` | `mmap(addr, PROT_NONE, MAP_ANONYMOUS \| MAP_FIXED \| MAP_NORESERVE) (destino volta a ser reserva)` | tmemUnmap -> destino Unmapped; desmapear de novo -> InvalidMemoryRange |

🧪 5 SVCs de IPC/memória compartilhada são dummies do mapeamento (devolvem sucesso sem emular o serviço); porta `sm:` e serviços vistos no `hb_services`: `hid`, `fsp-srv`, `set:sys`, `time:u`, `time:a`, `time:s`, `apm`.

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

<sub>Painel gerado por <code>update_dashboard.py</code> a partir de <code>status.json</code> · atualizado em 2026-10-16</sub>
