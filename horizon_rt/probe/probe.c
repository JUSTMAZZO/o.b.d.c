/*
 * probe.c - sonda do ambiente Android AArch64 para a Fase 3 (reemissao estatica de homebrew).
 *
 * 1. Registradores de sistema que o codigo do Horizon/libnx le em EL0
 *    (tpidrro_el0, tpidr_el0, cntfrq_el0, cntvct_el0, cntpct_el0) e se algum gera SIGILL.
 * 2. x18 do processo (Shadow Call Stack do bionic?).
 * 3. Mecanismo de patch: svc -> b trampolim (wrapper folha com ret), mrs tpidrro_el0 -> b trampolim,
 *    preservacao de x1-x29 (inclusive x18 do guest) e NZCV atraves do trampolim.
 *
 * Build: aarch64-linux-android35-clang -O2 probe.c probe_guest.S -o probe
 */
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

uint64_t g_host_x18;   /* x18 do host, restaurado pelo trampolim antes de entrar em C */
uint64_t g_fake_tls;   /* regiao de TLS do Horizon (0x200 bytes) que o guest enxerga em tpidrro_el0 */
static uint8_t s_tls_region[0x200] __attribute__((aligned(16)));

uint32_t guest_debug_string(const char *str, uint64_t size);
uint64_t guest_tls_x0(void);
uint64_t guest_tls_x7(void);
uint64_t guest_reg_check(void);

static unsigned s_svc_calls;

/* HLE do kernel: regs[0..30] = x0..x30 do guest; altera x0.. como o svc real faria */
void hle_svc(uint64_t *regs, uint32_t id)
{
    s_svc_calls++;
    switch (id) {
    case 0x27: /* svcOutputDebugString(str, size) */
        fwrite((const char *)regs[0], 1, regs[1], stdout);
        regs[0] = 0;
        break;
    case 0x00: /* svc de teste: nao altera registradores */
        break;
    default:
        printf("[hle] svc 0x%x nao implementado\n", id);
        regs[0] = 0xF601; /* KernelResult NotImplemented (valor ilustrativo) */
    }
}

static sigjmp_buf s_jmp;
static void on_sigill(int sig) { (void)sig; siglongjmp(s_jmp, 1); }

#define READ_SYSREG(name, out) __asm__ volatile("mrs %0, " #name : "=r"(out))

static void probe_sysreg(const char *name, int which)
{
    uint64_t v = 0;
    if (sigsetjmp(s_jmp, 1) == 0) {
        switch (which) {
        case 0: READ_SYSREG(tpidrro_el0, v); break;
        case 1: READ_SYSREG(tpidr_el0, v); break;
        case 2: READ_SYSREG(cntfrq_el0, v); break;
        case 3: READ_SYSREG(cntvct_el0, v); break;
        case 4: READ_SYSREG(cntpct_el0, v); break;
        }
        printf("[sysreg] %-12s = 0x%016llx\n", name, (unsigned long long)v);
    } else {
        printf("[sysreg] %-12s -> SIGILL (acesso em EL0 bloqueado)\n", name);
    }
}

int main(void)
{
    __asm__ volatile("mov %0, x18" : "=r"(g_host_x18));
    g_fake_tls = (uint64_t)(uintptr_t)s_tls_region;

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigill;
    sigaction(SIGILL, &sa, NULL);

    puts("== 1. registradores de sistema em EL0");
    probe_sysreg("tpidrro_el0", 0);
    probe_sysreg("tpidr_el0", 1);
    probe_sysreg("cntfrq_el0", 2);
    probe_sysreg("cntvct_el0", 3);
    probe_sysreg("cntpct_el0", 4);

    puts("== 2. x18 do processo");
    printf("[x18] host x18 = 0x%016llx (%s)\n", (unsigned long long)g_host_x18,
           g_host_x18 ? "provavel Shadow Call Stack ativo" : "zero");

    puts("== 3. patch svc/tpidrro -> trampolim");
    const char msg[] = "[guest] ola do svcOutputDebugString patchado\n";
    uint32_t rc = guest_debug_string(msg, sizeof msg - 1);
    printf("[svc 0x27] resultado=0x%x (esperado 0), chamadas ao HLE=%u\n", rc, s_svc_calls);

    uint64_t t0 = guest_tls_x0(), t7 = guest_tls_x7();
    printf("[tls] x0=0x%llx x7=0x%llx esperado=0x%llx -> %s\n", (unsigned long long)t0,
           (unsigned long long)t7, (unsigned long long)g_fake_tls,
           (t0 == g_fake_tls && t7 == g_fake_tls) ? "OK" : "DIVERGE");

    uint64_t bad = guest_reg_check();
    printf("[regs] mascara de registradores alterados = 0x%llx -> %s\n", (unsigned long long)bad,
           bad == 0 ? "OK (x1-x29, x18 do guest e NZCV preservados)" : "DIVERGE");

    return (rc == 0 && bad == 0 && t0 == g_fake_tls && t7 == g_fake_tls) ? 0 : 1;
}
