/*
 * svc_shim_test.c - testa no Android (A56) os handlers HLE dos SVCs do Horizon em switch_hle_shim.h.
 *
 * Cada SVC e exercitado no caminho de sucesso e nos erros que a libnx espera (KERNELRESULT).
 * O mutex do teste segue o algoritmo do mutexLock/mutexUnlock da libnx sobre
 * svcArbitrateLock/svcArbitrateUnlock. svcBreak fatal e svcReturnFromException rodam num filho (fork).
 *
 * Build: aarch64-linux-android35-clang -O2 -Wall -Wextra -I .. svc_shim_test.c -o svc_shim_test  (pthread vem na libc do Bionic)
 */
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include "switch_hle_shim.h"

static int s_ok, s_fail;

#define CHECK(cond, ...) do { \
        if (cond) { s_ok++; printf("[ok]      "); } else { s_fail++; printf("[FALHOU]  "); } \
        printf(__VA_ARGS__); printf("\n"); \
    } while (0)

static MemoryInfo query(void *p)
{
    MemoryInfo mi;
    u32 pi;
    Result rc = svcQueryMemory(&mi, &pi, (u64)(uintptr_t)p);
    if (R_FAILED(rc))
        memset(&mi, 0xFF, sizeof mi);
    return mi;
}

/* ---------------------------------------------- mutex no algoritmo da libnx */
typedef u32 Mutex;
static __thread u32 t_tag;   /* na libnx: getThreadVars()->handle (handle da thread) */

static void mutexLock(Mutex *m)
{
    u32 value = __atomic_load_n(m, __ATOMIC_SEQ_CST);
    for (;;) {
        if (value == INVALID_HANDLE) {
            if (__atomic_compare_exchange_n(m, &value, t_tag, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
                return;
            continue;
        }
        if ((value & HANDLE_WAIT_MASK) == 0) {
            if (!__atomic_compare_exchange_n(m, &value, value | HANDLE_WAIT_MASK, false,
                                             __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
                continue;
        }
        svcArbitrateLock(value & ~HANDLE_WAIT_MASK, m, t_tag);
        value = __atomic_load_n(m, __ATOMIC_SEQ_CST);
        if ((value & ~HANDLE_WAIT_MASK) == t_tag)
            return;
    }
}

static void mutexUnlock(Mutex *m)
{
    u32 value = __atomic_load_n(m, __ATOMIC_SEQ_CST);
    for (;;) {
        if (value != t_tag) {              /* tem WAIT_MASK: o kernel acorda o proximo */
            svcArbitrateUnlock(m);
            return;
        }
        if (__atomic_compare_exchange_n(m, &value, INVALID_HANDLE, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            return;
    }
}

#define THREADS 4
#define ITERS   200000
static Mutex s_mutex;
static volatile u64 s_counter;

static void *worker(void *arg)
{
    t_tag = 0x100 + (u32)(uintptr_t)arg;   /* tags distintas e != 0, como handles de thread */
    for (int k = 0; k < ITERS; k++) {
        mutexLock(&s_mutex);
        s_counter = s_counter + 1;          /* leitura+escrita nao atomica: so fica certo com exclusao */
        mutexUnlock(&s_mutex);
    }
    return NULL;
}

static int child_signal(void (*fn)(void))
{
    fflush(stdout);   /* senao o filho herda e reimprime o buffer do pai */
    pid_t pid = fork();
    if (pid == 0) {
        fn();
        _exit(0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFSIGNALED(st) ? WTERMSIG(st) : 0;
}
static void do_break_panic(void) { svcBreak(BreakReason_Panic, 0, 0); }
static void do_return_from_exception(void) { svcReturnFromException(0); }

int main(void)
{
    const char hello[] = "[svc 0x27] svcOutputDebugString via write()\n";
    CHECK(svcOutputDebugString(hello, sizeof hello - 1) == 0, "svcOutputDebugString -> 0");

    /* ---- 0x01 heap */
    void *heap = NULL;
    CHECK(svcSetHeapSize(&heap, 0x1000) == KERNELRESULT(InvalidSize), "svcSetHeapSize(0x1000) -> InvalidSize (0x%x)", KERNELRESULT(InvalidSize));
    CHECK(svcSetHeapSize(&heap, 0x400000) == 0 && heap != NULL, "svcSetHeapSize(4 MiB) -> base %p", heap);
    u8 *h = heap;
    memset(h, 0xA5, 0x400000);
    CHECK(h[0] == 0xA5 && h[0x3FFFFF] == 0xA5, "heap de 4 MiB legivel e gravavel");
    MemoryInfo mi = query(h);
    CHECK(mi.type == MemType_Heap && mi.perm == Perm_Rw && mi.size >= 0x400000,
          "svcQueryMemory(heap) -> type=Heap perm=Rw size=0x%llx", (unsigned long long)mi.size);
    CHECK(svcSetHeapSize(&heap, 0x200000) == 0 && heap == h, "svcSetHeapSize(2 MiB) encolhe no lugar");
    mi = query(h + 0x200000);
    CHECK(mi.type == MemType_Unmapped, "svcQueryMemory(heap + 2 MiB) -> Unmapped depois de encolher");

    /* ---- 0x02 permissao */
    CHECK(svcSetMemoryPermission(h, 0x1000, Perm_R) == 0 && query(h).perm == Perm_R, "svcSetMemoryPermission(Perm_R) -> QueryMemory perm=R");
    CHECK(svcSetMemoryPermission(h, 0x1000, Perm_Rx) == KERNELRESULT(InvalidMemoryPermissions), "svcSetMemoryPermission(Perm_Rx) -> InvalidMemoryPermissions");
    CHECK(svcSetMemoryPermission(h + 1, 0x1000, Perm_Rw) == KERNELRESULT(InvalidAddress), "svcSetMemoryPermission(desalinhado) -> InvalidAddress");
    CHECK(svcSetMemoryPermission(h, 0x1000, Perm_Rw) == 0 && query(h).perm == Perm_Rw, "svcSetMemoryPermission(Perm_Rw) restaura");

    /* ---- 0x03 atributo */
    CHECK(svcSetMemoryAttribute(h, 0x1000, MemAttr_IsUncached, MemAttr_IsUncached) == 0 &&
          (query(h).attr & MemAttr_IsUncached), "svcSetMemoryAttribute(IsUncached) -> QueryMemory attr");
    CHECK(svcSetMemoryAttribute(h, 0x1000, MemAttr_IsBorrowed, MemAttr_IsBorrowed) == KERNELRESULT(InvalidCombination),
          "svcSetMemoryAttribute(IsBorrowed) -> InvalidCombination");
    CHECK(svcSetMemoryAttribute(h, 0x1000, MemAttr_IsUncached, 0) == 0 && !(query(h).attr & MemAttr_IsUncached),
          "svcSetMemoryAttribute(limpa IsUncached)");

    /* ---- 0x29 info + 0x04/0x05 alias */
    u64 alias = 0, v = 0;
    CHECK(svcGetInfo(&alias, InfoType_AliasRegionAddress, CUR_PROCESS_HANDLE, 0) == 0 && alias != 0, "svcGetInfo(AliasRegionAddress) -> 0x%llx", (unsigned long long)alias);
    u8 *src = h + 0x10000, *dst = (u8 *)(uintptr_t)alias + 0x100000;
    src[0] = 0x11;
    CHECK(svcMapMemory(dst, src, 0x1000) == 0 && dst[0] == 0x11, "svcMapMemory: alias le o conteudo da origem");
    dst[1] = 0x22;
    mi = query(src);
    CHECK(mi.perm == Perm_None && (mi.attr & MemAttr_IsBorrowed), "origem fica Perm_None + IsBorrowed (QueryMemory)");
    CHECK(query(dst).type == MemType_MappedMemory, "svcQueryMemory(alias) -> MappedMemory");
    CHECK(svcUnmapMemory(dst, src + 0x1000, 0x1000) == KERNELRESULT(InvalidMemoryRange), "svcUnmapMemory(origem errada) -> InvalidMemoryRange");
    CHECK(svcUnmapMemory(dst, src, 0x1000) == 0 && src[1] == 0x22 && query(src).perm == Perm_Rw,
          "svcUnmapMemory: origem volta a Rw com o dado escrito pelo alias");
    CHECK(query(dst).type == MemType_Unmapped, "svcQueryMemory(alias) -> Unmapped depois do unmap");

    CHECK(svcGetInfo(&v, InfoType_HeapRegionAddress, CUR_PROCESS_HANDLE, 0) == 0 && v == (u64)(uintptr_t)h, "svcGetInfo(HeapRegionAddress) = base da heap");
    CHECK(svcGetInfo(&v, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0) == 0 && v != 0, "svcGetInfo(CoreMask) -> 0x%llx", (unsigned long long)v);
    CHECK(svcGetInfo(&v, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0) == 0 && v > 0, "svcGetInfo(TotalMemorySize) -> %llu MiB", (unsigned long long)(v >> 20));
    CHECK(svcGetInfo(&v, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0) == 0 && v > 0, "svcGetInfo(UsedMemorySize) -> %llu KiB", (unsigned long long)(v >> 10));
    CHECK(svcGetInfo(&v, InfoType_DebuggerAttached, INVALID_HANDLE, 0) == 0 && v == 0, "svcGetInfo(DebuggerAttached) -> 0");
    CHECK(svcGetInfo(&v, InfoType_RandomEntropy, INVALID_HANDLE, 2) == 0, "svcGetInfo(RandomEntropy, 2)");
    CHECK(svcGetInfo(&v, 65000, CUR_PROCESS_HANDLE, 0) == KERNELRESULT(InvalidEnumValue), "svcGetInfo(65000, como o detect.h da libnx) -> InvalidEnumValue (0x%x)", KERNELRESULT(InvalidEnumValue));
    CHECK(svcGetInfo(&v, InfoType_HeapRegionSize, 0x1234, 0) == KERNELRESULT(InvalidHandle), "svcGetInfo(handle invalido) -> InvalidHandle");

    /* ---- 0x1A/0x1B mutex */
    pthread_t th[THREADS];
    for (uintptr_t k = 0; k < THREADS; k++)
        pthread_create(&th[k], NULL, worker, (void *)k);
    for (int k = 0; k < THREADS; k++)
        pthread_join(th[k], NULL);
    CHECK(s_counter == (u64)THREADS * ITERS && s_mutex == 0, "mutex libnx sobre svcArbitrateLock/Unlock: %d threads x %d -> %llu (mutex final=0x%x)",
          THREADS, ITERS, (unsigned long long)s_counter, s_mutex);

    /* ---- 0x26 / 0x28 */
    CHECK(svcBreak(BreakReason_User | BreakReason_NotificationOnlyFlag, 0, 0) == 0, "svcBreak(so notificacao) -> 0");
    CHECK(child_signal(do_break_panic) == SIGABRT, "svcBreak(Panic) encerra o processo com SIGABRT");
    CHECK(child_signal(do_return_from_exception) == SIGABRT, "svcReturnFromException fora de handler -> SIGABRT");

    printf("== %d ok, %d falhas\n", s_ok, s_fail);
    if (s_fail)
        return 1;
    svcExitProcess();   /* 0x07: termina com codigo 0 */
}
