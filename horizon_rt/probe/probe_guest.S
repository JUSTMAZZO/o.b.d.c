/*
 * probe_guest.S - prova do mecanismo de patch da Fase 3 no Android AArch64.
 *
 * "Guest" = codigo no formato da libnx ja patchado como a reemissao estatica fara:
 *   svc #imm            ->  b tramp_svc_<imm>_<k>   (trampolim por sitio; volta com b, sem tocar x30)
 *   mrs xN, tpidrro_el0 ->  b tramp_tls_<k>         (carrega o ponteiro de TLS do Horizon em xN)
 *
 * O trampolim de svc salva x0-x30 + NZCV num quadro na pilha do guest, troca x18 pelo x18 do
 * host (o bionic pode usar x18 para Shadow Call Stack), chama hle_svc(quadro, id) em C,
 * restaura tudo (inclusive o x18 do guest) e volta para a instrucao seguinte ao svc.
 */
    .text
    .p2align 2

/* ------------------------------------------------------------------ trampolins */
.macro SVC_TRAMP name, id, back
    .global \name
\name:
    sub  sp, sp, #256
    stp  x0,  x1,  [sp, #0]
    stp  x2,  x3,  [sp, #16]
    stp  x4,  x5,  [sp, #32]
    stp  x6,  x7,  [sp, #48]
    stp  x8,  x9,  [sp, #64]
    stp  x10, x11, [sp, #80]
    stp  x12, x13, [sp, #96]
    stp  x14, x15, [sp, #112]
    stp  x16, x17, [sp, #128]
    stp  x18, x19, [sp, #144]
    stp  x20, x21, [sp, #160]
    stp  x22, x23, [sp, #176]
    stp  x24, x25, [sp, #192]
    stp  x26, x27, [sp, #208]
    stp  x28, x29, [sp, #224]
    mrs  x9, nzcv
    stp  x30, x9,  [sp, #240]
    adrp x9, g_host_x18
    ldr  x18, [x9, :lo12:g_host_x18]
    mov  x0, sp
    mov  w1, #\id
    bl   hle_svc
    ldp  x30, x9,  [sp, #240]
    msr  nzcv, x9
    ldp  x0,  x1,  [sp, #0]
    ldp  x2,  x3,  [sp, #16]
    ldp  x4,  x5,  [sp, #32]
    ldp  x6,  x7,  [sp, #48]
    ldp  x8,  x9,  [sp, #64]
    ldp  x10, x11, [sp, #80]
    ldp  x12, x13, [sp, #96]
    ldp  x14, x15, [sp, #112]
    ldp  x16, x17, [sp, #128]
    ldp  x18, x19, [sp, #144]
    ldp  x20, x21, [sp, #160]
    ldp  x22, x23, [sp, #176]
    ldp  x24, x25, [sp, #192]
    ldp  x26, x27, [sp, #208]
    ldp  x28, x29, [sp, #224]
    add  sp, sp, #256
    b    \back
.endm

.macro TLS_TRAMP name, reg, back
    .global \name
\name:
    adrp \reg, g_fake_tls
    ldr  \reg, [\reg, :lo12:g_fake_tls]
    b    \back
.endm

/* ------------------------------------------------------------------ guest */
/* Result svcOutputDebugString(const char *str, u64 size) -- wrapper folha da libnx */
    .global guest_debug_string
guest_debug_string:
    b    tramp_svc_27_0          /* era: svc #0x27 */
1:  ret
SVC_TRAMP tramp_svc_27_0, 0x27, 1b

/* void *getThreadLocalRegion(void) -- libnx: mrs x0, tpidrro_el0; ret */
    .global guest_tls_x0
guest_tls_x0:
    b    tramp_tls_0             /* era: mrs x0, tpidrro_el0 */
2:  ret
TLS_TRAMP tramp_tls_0, x0, 2b

/* mesma coisa com outro registrador de destino */
    .global guest_tls_x7
guest_tls_x7:
    b    tramp_tls_1             /* era: mrs x7, tpidrro_el0 */
3:  mov  x0, x7
    ret
TLS_TRAMP tramp_tls_1, x7, 3b

/*
 * u64 guest_reg_check(void): poe padroes conhecidos em x1-x29 (inclusive x18) e NZCV, executa um
 * "svc" patchado e devolve uma mascara com 1 em cada registrador que mudou (bit 31 = NZCV).
 */
    .global guest_reg_check
guest_reg_check:
    stp  x29, x30, [sp, #-112]!
    str  x18, [sp, #96]          /* x18 do host (Shadow Call Stack do bionic) */
    stp  x19, x20, [sp, #16]
    stp  x21, x22, [sp, #32]
    stp  x23, x24, [sp, #48]
    stp  x25, x26, [sp, #64]
    stp  x27, x28, [sp, #80]
    .irp n, 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29
    mov  x\n, #(0x700 + \n)
    .endr
    mov  x0, #0
    cmp  x0, #1                  /* NZCV = N=1 (0 - 1 < 0), C=0 */
    b    tramp_svc_00_1          /* era: svc #0x00 (svc de teste: nao mexe em nada) */
4:  mrs  x0, nzcv                /* NZCV depois do svc, guardado antes de qualquer comparacao */
    str  x0, [sp, #104]
    mov  x30, #0
    .irp n, 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29
    cmp  x\n, #(0x700 + \n)
    cset x0, ne                  /* temporario x0: nao toca nos registradores ainda nao conferidos */
    orr  x30, x30, x0, lsl #\n
    .endr
    ldr  x0, [sp, #104]
    mov  x1, #0x80000000         /* N=1 Z=0 C=0 V=0 esperado */
    cmp  x0, x1
    cset x0, ne
    orr  x0, x30, x0, lsl #31
    ldp  x19, x20, [sp, #16]
    ldp  x21, x22, [sp, #32]
    ldp  x23, x24, [sp, #48]
    ldp  x25, x26, [sp, #64]
    ldp  x27, x28, [sp, #80]
    ldr  x18, [sp, #96]
    ldp  x29, x30, [sp], #112
    ret
SVC_TRAMP tramp_svc_00_1, 0x00, 4b
