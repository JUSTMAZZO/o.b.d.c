/*
 * svc_shim_test.c - testa no Android (A56) os handlers HLE dos SVCs do Horizon em switch_hle_shim.h.
 *
 * Cada SVC e exercitado no caminho de sucesso e nos erros que a libnx espera (KERNELRESULT).
 * Threads, Mutex, RMutex e CondVar sao exercitados pela API da libnx implementada no shim
 * (threadCreate/..., mutexLock/..., condvarWait/...). svcBreak fatal e svcReturnFromException rodam
 * num filho (fork).
 *
 * Build: aarch64-linux-android35-clang -O2 -Wall -Wextra -I .. svc_shim_test.c -o svc_shim_test  (pthread vem na libc do Bionic)
 */
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

/* ---------------------------------------------- threads, mutex, condvar e rmutex pela API da libnx no shim */
#define THREADS 4
#define ITERS   200000
static Mutex s_mutex;
static volatile u64 s_counter;
static Handle s_handles[THREADS];

static void worker(void *arg)
{
    s_handles[(uintptr_t)arg] = threadGetCurHandle();
    for (int k = 0; k < ITERS; k++) {
        mutexLock(&s_mutex);
        s_counter = s_counter + 1;          /* leitura+escrita nao atomica: so fica certo com exclusao */
        mutexUnlock(&s_mutex);
    }
}

static void exits_early(void *arg)
{
    *(volatile int *)arg = 1;
    svcExitThread();                        /* 0x0A: termina antes de retornar da funcao */
    *(volatile int *)arg = 2;
}

/* produtor/consumidor com CondVar: fila de 1 posicao */
#define ITEMS 1000
static Mutex s_qm;
static CondVar s_qcv;
static int s_slot = -1, s_consumed_sum;

static void consumer(void *arg)
{
    (void)arg;
    for (int n = 0; n < ITEMS; n++) {
        mutexLock(&s_qm);
        while (s_slot < 0)
            condvarWait(&s_qcv, &s_qm);
        s_consumed_sum += s_slot;
        s_slot = -1;
        condvarWakeAll(&s_qcv);
        mutexUnlock(&s_qm);
    }
}

static RMutex s_rm;
static int recurse(int depth)
{
    rmutexLock(&s_rm);
    int r = depth == 0 ? (int)s_rm.counter : recurse(depth - 1);
    rmutexUnlock(&s_rm);
    return r;
}

static u64 now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ULL + (u64)ts.tv_nsec;
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

    /* ---- 0x08/0x09/0x18/0x16 + mutex libnx (0x1A/0x1B) com 4 threads */
    Thread th[THREADS];
    int created = 1;
    for (uintptr_t k = 0; k < THREADS; k++)
        created &= threadCreate(&th[k], worker, (void *)k, NULL, 0x10000, 0x2C, -2) == 0 && threadStart(&th[k]) == 0;
    CHECK(created, "threadCreate + threadStart (svcCreateThread/svcStartThread) x%d", THREADS);
    int waited = 1;
    for (int k = 0; k < THREADS; k++)
        waited &= threadWaitForExit(&th[k]) == 0 && threadClose(&th[k]) == 0;
    CHECK(waited, "threadWaitForExit + threadClose (svcWaitSynchronization/svcCloseHandle)");
    CHECK(s_counter == (u64)THREADS * ITERS && s_mutex == 0, "mutexLock/Unlock da libnx: %d threads x %d -> %llu (mutex final=0x%x)",
          THREADS, ITERS, (unsigned long long)s_counter, s_mutex);
    int distinct = threadGetCurHandle() != INVALID_HANDLE;
    for (int k = 0; k < THREADS; k++)
        for (int j = 0; j < k; j++)
            distinct &= s_handles[k] != s_handles[j] && s_handles[k] != threadGetCurHandle();
    CHECK(distinct, "threadGetCurHandle distinto por thread (tag do mutex)");

    Handle hx;
    CHECK(svcCreateThread(&hx, (void *)worker, NULL, NULL, 64, -2) == KERNELRESULT(InvalidPriority), "svcCreateThread(prio 64) -> InvalidPriority");
    CHECK(svcCreateThread(&hx, (void *)worker, NULL, NULL, 0x2C, 7) == KERNELRESULT(InvalidCoreId), "svcCreateThread(cpuid 7) -> InvalidCoreId");
    CHECK(svcStartThread(0x1234) == KERNELRESULT(InvalidHandle), "svcStartThread(handle invalido) -> InvalidHandle");

    /* ---- 0x0A */
    volatile int flag = 0;
    Thread te;
    CHECK(threadCreate(&te, exits_early, (void *)&flag, NULL, 0x10000, 0x2C, -2) == 0 && threadStart(&te) == 0 &&
          threadWaitForExit(&te) == 0 && flag == 1 && threadClose(&te) == 0, "svcExitThread termina a thread e sinaliza quem espera");

    /* ---- 0x18 timeout + 0x0B */
    Thread ts_;
    threadCreate(&ts_, exits_early, (void *)&flag, NULL, 0x10000, 0x2C, -2);   /* criada e nao iniciada */
    s32 idx = -1;
    u64 t0 = now_ns();
    CHECK(svcWaitSynchronization(&idx, &ts_.handle, 1, 20000000ULL) == KERNELRESULT(TimedOut) && now_ns() - t0 >= 20000000ULL,
          "svcWaitSynchronization(20 ms, thread parada) -> TimedOut depois de %.1f ms", (now_ns() - t0) / 1e6);
    threadClose(&ts_);
    t0 = now_ns();
    svcSleepThread(15000000LL);
    u64 slept = now_ns() - t0;
    CHECK(slept >= 15000000ULL, "svcSleepThread(15 ms) dormiu %.2f ms", slept / 1e6);
    svcSleepThread(0);
    svcSleepThread(-1);
    CHECK(1, "svcSleepThread(0/-1) -> yield");

    /* ---- 0x1C/0x1D CondVar */
    Thread tc;
    mutexInit(&s_qm);
    condvarInit(&s_qcv);
    threadCreate(&tc, consumer, NULL, NULL, 0x10000, 0x2C, -2);
    threadStart(&tc);
    int produced_sum = 0;
    for (int n = 1; n <= ITEMS; n++) {
        mutexLock(&s_qm);
        while (s_slot >= 0)
            condvarWait(&s_qcv, &s_qm);
        s_slot = n;
        produced_sum += n;
        condvarWakeAll(&s_qcv);
        mutexUnlock(&s_qm);
    }
    threadWaitForExit(&tc);
    threadClose(&tc);
    CHECK(s_consumed_sum == produced_sum, "CondVar (svcWaitProcessWideKeyAtomic/svcSignalProcessWideKey): %d itens, soma %d = %d",
          ITEMS, s_consumed_sum, produced_sum);
    mutexLock(&s_qm);
    t0 = now_ns();
    Result rcw = condvarWaitTimeout(&s_qcv, &s_qm, 10000000ULL);
    CHECK(rcw == KERNELRESULT(TimedOut) && (s_qm & ~HANDLE_WAIT_MASK) == threadGetCurHandle(),
          "condvarWaitTimeout(10 ms) -> TimedOut e volta com o mutex (%.1f ms)", (now_ns() - t0) / 1e6);
    mutexUnlock(&s_qm);

    /* ---- RMutex */
    rmutexInit(&s_rm);
    CHECK(recurse(5) == 6 && s_rm.counter == 0 && s_rm.lock == 0, "rmutexLock recursivo: profundidade 6, solta tudo no fim");

    /* ---- 0x26 / 0x28 */
    CHECK(svcBreak(BreakReason_User | BreakReason_NotificationOnlyFlag, 0, 0) == 0, "svcBreak(so notificacao) -> 0");
    CHECK(child_signal(do_break_panic) == SIGABRT, "svcBreak(Panic) encerra o processo com SIGABRT");
    CHECK(child_signal(do_return_from_exception) == SIGABRT, "svcReturnFromException fora de handler -> SIGABRT");

    printf("== %d ok, %d falhas\n", s_ok, s_fail);
    if (s_fail)
        return 1;
    svcExitProcess();   /* 0x07: termina com codigo 0 */
}
