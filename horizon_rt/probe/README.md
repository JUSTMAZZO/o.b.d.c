# Sonda do ambiente (Fase 3)

`probe.c` + `probe_guest.S` testam no Galaxy A56, antes de existir um NRO real, o mecanismo que a
reemissão estática vai usar: cada `svc #imm` e cada `mrs xN, tpidrro_el0` do código do Switch vira
`b trampolim` (mesmo tamanho, 4 bytes, sem tocar em `x30`), e o trampolim volta com `b`.

```
aarch64-linux-android35-clang -O2 probe.c probe_guest.S -o hz_probe
adb push hz_probe /data/local/tmp/ && adb shell "chmod +x /data/local/tmp/hz_probe && /data/local/tmp/hz_probe"
```

## Resultado no A56 (SM-A566E, 2026-10-15)

| Item | Valor | Consequência para o reemissor |
|---|---|---|
| `tpidrro_el0` | `0` | Todo `mrs xN, tpidrro_el0` precisa de patch (TLS do Horizon). |
| `tpidr_el0` | ponteiro do bionic | Não reutilizar: é o TLS do Android. |
| `cntfrq_el0` | `0x186a000` = 25,6 MHz | O Switch usa 19,2 MHz: ticks precisam de escala ×0,75. |
| `cntvct_el0` | legível | Fonte de tempo para o patch. |
| `cntpct_el0` | **SIGILL** | `armGetSystemTick()` da libnx lê esse registrador: precisa de patch para `cntvct_el0` escalado. |
| `x18` | ≠ 0 (Shadow Call Stack do bionic) | O trampolim troca para o `x18` do host antes de chamar C e devolve o `x18` do guest. |
| svc `0x27` patchado em wrapper folha (`svc; ret`) | saída correta, `ret` volta para o chamador | Trampolim por sítio com `b`, nunca `bl`. |
| `mrs x0/x7, tpidrro_el0` patchados | ponteiro de TLS falso correto | Trampolim de TLS por registrador de destino. |
| `x1`–`x29` (inclusive `x18` do guest) e NZCV | preservados | Quadro de 256 bytes na pilha do guest. |
