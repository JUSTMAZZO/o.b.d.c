/*
 * switch_hle_shim.h - camada HLE minima: primitivas estilo libnx sobre Android Bionic/POSIX.
 *
 * Fase 3 do o.b.d.c. Permite compilar codigo-fonte no padrao de homebrew do Switch
 * (consoleInit / appletMainLoop / padUpdate / armGetSystemTick) com o Clang do NDK.
 *
 * Execucao deterministica para benchmark:
 *   - appletMainLoop() devolve true por SHIM_FRAMES quadros (60) e depois false;
 *   - quadro 30: HidNpadButton_A pressionado (impulso);
 *   - quadro 60: HidNpadButton_Plus pressionado (sair).
 *
 * Todas as primitivas sao weak + noinline (SHIM_API) para preservar as chamadas no binario.
 * Os SVCs do Horizon usados pelo hb_compute tem handlers HLE em Bionic/POSIX (horizon_rt/svc_table.json).
 * Os valores de botoes/estilos e a frequencia do tick (19,2 MHz) seguem a libnx.
 */
#ifndef SWITCH_HLE_SHIM_H
#define SWITCH_HLE_SHIM_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

/* ------------------------------------------------------------------ tipos base */
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t  s32;
typedef int64_t  s64;
typedef u32      Result;

/* Cada primitiva vira uma funcao real (bl), como a fronteira de ABI de um homebrew
 * que chama a libnx. noinline sozinho nao basta: com static o Clang ainda propaga
 * constantes para dentro da funcao e apaga chamadas sem efeito. weak torna a
 * definicao substituivel no link, o que bloqueia essa otimizacao interprocedural. */
#define SHIM_API __attribute__((weak, noinline))

#define BIT(n) (1U << (n))
#define R_SUCCEEDED(res) ((res) == 0)
#define R_FAILED(res)    ((res) != 0)

/* ------------------------------------------------------------------ input (hid) */
#define HidNpadButton_A    BIT(0)
#define HidNpadButton_B    BIT(1)
#define HidNpadButton_Plus BIT(10)

#define HidNpadStyleSet_NpadStandard 0x1FU /* FullKey | Handheld | JoyDual | JoyLeft | JoyRight */

#define SHIM_FRAMES        60
#define SHIM_FRAME_BOOST_A 30
#define SHIM_FRAME_EXIT    60

typedef struct {
    u8  id_mask;
    u8  active_id_mask;
    bool read_handheld;
    bool active_handheld;
    u32 style_set;
    u32 attributes;
    u64 buttons_cur;
    u64 buttons_old;
} PadState;

/* Quadro atual do loop do applet (1..SHIM_FRAMES); 0 antes do primeiro quadro. */
static u32 g_shim_frame;
static u32 g_shim_max_players;
static u32 g_shim_style_set;

SHIM_API void padConfigureInput(u32 max_players, u32 style_set)
{
    g_shim_max_players = max_players;
    g_shim_style_set = style_set;
}

SHIM_API void padInitializeDefault(PadState *pad)
{
    *pad = (PadState){0};
    pad->id_mask = pad->active_id_mask = 1;
    pad->read_handheld = pad->active_handheld = true;
    pad->style_set = g_shim_style_set;
}

SHIM_API void padUpdate(PadState *pad)
{
    u64 buttons = 0;
    if (g_shim_frame == SHIM_FRAME_BOOST_A)
        buttons |= HidNpadButton_A;
    if (g_shim_frame == SHIM_FRAME_EXIT)
        buttons |= HidNpadButton_Plus;
    pad->buttons_old = pad->buttons_cur;
    pad->buttons_cur = buttons;
}

SHIM_API u64 padGetButtons(const PadState *pad)     { return pad->buttons_cur; }
SHIM_API u64 padGetButtonsDown(const PadState *pad) { return pad->buttons_cur & ~pad->buttons_old; }
SHIM_API u64 padGetButtonsUp(const PadState *pad)   { return ~pad->buttons_cur & pad->buttons_old; }

/* ------------------------------------------------------------------ applet / console */
SHIM_API bool appletMainLoop(void)
{
    if (g_shim_frame >= SHIM_FRAMES)
        return false;
    g_shim_frame++;
    return true;
}

/* No Switch o console desenha no framebuffer; aqui vai para o stdout do Bionic. */
SHIM_API void *consoleInit(void *console) { (void)console; return NULL; }
SHIM_API void consoleUpdate(void *console) { (void)console; fflush(stdout); }
SHIM_API void consoleExit(void *console)   { (void)console; fflush(stdout); }

/* ------------------------------------------------------------------ kernel (svc) */
/*
 * Handlers HLE dos SVCs do Horizon usados pelo hb_compute (horizon_rt/hb_compute/hb_compute.sites.json),
 * com os mesmos prototipos, tipos e codigos de resultado da libnx 2.10.0. A traducao de cada um para
 * Bionic/POSIX esta documentada em horizon_rt/svc_table.json.
 *
 * Linux/Bionic: implementacao completa (mmap/memfd, mprotect, futex, write, _exit, /proc/self/maps).
 * Outros hosts (gate llvm-mingw no Windows): so os SVCs portaveis (debug string, exit, break).
 */
typedef u32 Handle;

#define INVALID_HANDLE     ((Handle)0)
#define CUR_PROCESS_HANDLE 0xFFFF8001
#define CUR_THREAD_HANDLE  0xFFFF8000
#define HANDLE_WAIT_MASK   0x40000000U

#define MAKERESULT(module, description) ((((module) & 0x1FF)) | ((description) & 0x1FFF) << 9)
#define KERNELRESULT(description) MAKERESULT(1, KernelError_##description)

enum {
    KernelError_NotImplemented = 33,
    KernelError_InvalidSize = 101,
    KernelError_InvalidAddress = 102,
    KernelError_OutOfMemory = 104,
    KernelError_InvalidMemoryState = 106,
    KernelError_InvalidMemoryPermissions = 108,
    KernelError_InvalidMemoryRange = 110,
    KernelError_InvalidHandle = 114,
    KernelError_InvalidCombination = 116,
    KernelError_OutOfRange = 119,
    KernelError_InvalidEnumValue = 120,
    KernelError_InvalidState = 125,
};

typedef enum {
    Perm_None = 0, Perm_R = BIT(0), Perm_W = BIT(1), Perm_X = BIT(2),
    Perm_Rw = Perm_R | Perm_W, Perm_Rx = Perm_R | Perm_X, Perm_DontCare = BIT(28),
} Permission;

enum { MemType_Unmapped = 0x00, MemType_Normal = 0x02, MemType_Heap = 0x05, MemType_MappedMemory = 0x0B };
enum { MemAttr_IsBorrowed = BIT(0), MemAttr_IsUncached = BIT(3) };
enum { BreakReason_Panic = 0, BreakReason_Assert = 1, BreakReason_User = 2,
       BreakReason_NotificationOnlyFlag = 0x80000000 };

typedef enum {
    InfoType_CoreMask = 0, InfoType_PriorityMask = 1,
    InfoType_AliasRegionAddress = 2, InfoType_AliasRegionSize = 3,
    InfoType_HeapRegionAddress = 4, InfoType_HeapRegionSize = 5,
    InfoType_TotalMemorySize = 6, InfoType_UsedMemorySize = 7,
    InfoType_DebuggerAttached = 8, InfoType_RandomEntropy = 11,
    InfoType_AslrRegionAddress = 12, InfoType_AslrRegionSize = 13,
    InfoType_StackRegionAddress = 14, InfoType_StackRegionSize = 15,
    InfoType_ProgramId = 18,
    InfoType_TotalNonSystemMemorySize = 21, InfoType_UsedNonSystemMemorySize = 22,
    InfoType_IsApplication = 23,
} InfoType;

typedef struct {
    u64 addr;
    u64 size;
    u32 type;
    u32 attr;
    u32 perm;
    u32 ipc_refcount;
    u32 device_refcount;
    u32 padding;
} MemoryInfo;

#include <stdlib.h>
#include <string.h>

/* svc 0x27: no Switch vai para o log de debug do kernel; aqui para o stdout (write, depois do stdio). */
#if defined(__linux__)
#include <unistd.h>
SHIM_API Result svcOutputDebugString(const char *str, u64 size)
{
    fflush(stdout);
    while (size > 0) {
        ssize_t n = write(STDOUT_FILENO, str, size);
        if (n <= 0)
            break;
        str += n;
        size -= (u64)n;
    }
    return 0;
}
#else
SHIM_API Result svcOutputDebugString(const char *str, u64 size)
{
    fwrite(str, 1, size, stdout);
    return 0;
}
#endif

/* svc 0x07: encerra o processo (stdio descarregado antes, como o __libnx_exit faz com o newlib). */
__attribute__((noreturn)) SHIM_API void svcExitProcess(void)
{
    fflush(NULL);
#if defined(__linux__)
    _exit(0);
#else
    exit(0);
#endif
}

/* svc 0x26: so-notificacao volta com sucesso; o resto e erro fatal do processo (abort). */
SHIM_API Result svcBreak(u32 breakReason, uintptr_t address, uintptr_t size)
{
    fflush(stdout);
    fprintf(stderr, "[svcBreak] reason=0x%x address=0x%llx size=0x%llx\n", breakReason,
            (unsigned long long)address, (unsigned long long)size);
    if (breakReason & BreakReason_NotificationOnlyFlag)
        return 0;
    abort();
}

/* svc 0x28: so e valido dentro de um handler de excecao do usuario; o shim nao entrega excecoes
 * do Horizon (o equivalente POSIX seria retornar de um handler de sinal), entao e fatal. */
__attribute__((noreturn)) SHIM_API void svcReturnFromException(Result res)
{
    fflush(stdout);
    fprintf(stderr, "[svcReturnFromException] res=0x%x fora de um handler de excecao\n", res);
    abort();
}

#if defined(__linux__)
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/futex.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>

#define SHIM_PAGE            0x1000ULL
#define SHIM_HEAP_ALIGN      0x200000ULL          /* svcSetHeapSize exige multiplo de 2 MiB */
#define SHIM_HEAP_REGION     (4ULL << 30)         /* HeapRegion reservada: 4 GiB de VA */
#define SHIM_ALIAS_REGION    (4ULL << 30)         /* AliasRegion (destino de svcMapMemory): 4 GiB de VA */
#define SHIM_MAX_RANGES      32
#define SHIM_PROGRAM_ID      0x0100000000001000ULL /* id ficticio de homebrew */

/* A heap e um memfd mapeado MAP_SHARED: assim svcMapMemory cria um alias real da mesma memoria. */
static u8 *g_shim_heap_base, *g_shim_alias_base;
static u64 g_shim_heap_size;
static int g_shim_heap_fd = -1;

typedef struct { u64 addr, size, src; u32 attr; } ShimRange;
static ShimRange g_shim_maps[SHIM_MAX_RANGES];   /* aliases criados por svcMapMemory (dst -> src) */
static ShimRange g_shim_attrs[SHIM_MAX_RANGES];  /* atributos de svcSetMemoryAttribute (sem efeito no HW) */

static int shim_in(const u8 *base, u64 region, u64 addr, u64 size)
{
    return base && addr >= (u64)(uintptr_t)base && size <= region &&
           addr + size <= (u64)(uintptr_t)base + region && addr + size >= addr;
}

static Result shim_reserve_regions(void)
{
    if (g_shim_heap_base)
        return 0;
    g_shim_heap_fd = (int)syscall(SYS_memfd_create, "obdc-horizon-heap", 0);
    if (g_shim_heap_fd < 0 || ftruncate(g_shim_heap_fd, (off_t)SHIM_HEAP_REGION) != 0)
        return KERNELRESULT(OutOfMemory);
    void *h = mmap(NULL, SHIM_HEAP_REGION, PROT_NONE, MAP_SHARED | MAP_NORESERVE, g_shim_heap_fd, 0);
    void *a = mmap(NULL, SHIM_ALIAS_REGION, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (h == MAP_FAILED || a == MAP_FAILED)
        return KERNELRESULT(OutOfMemory);
    g_shim_heap_base = (u8 *)h;
    g_shim_alias_base = (u8 *)a;
    return 0;
}

static int shim_prot(u32 perm)
{
    return ((perm & Perm_R) ? PROT_READ : 0) | ((perm & Perm_W) ? PROT_WRITE : 0) |
           ((perm & Perm_X) ? PROT_EXEC : 0);
}

/* svc 0x01: cresce/encolhe a heap no lugar; *out_addr = base da HeapRegion. */
SHIM_API Result svcSetHeapSize(void **out_addr, u64 size)
{
    if (size % SHIM_HEAP_ALIGN)
        return KERNELRESULT(InvalidSize);
    if (size > SHIM_HEAP_REGION)
        return KERNELRESULT(OutOfMemory);
    Result rc = shim_reserve_regions();
    if (R_FAILED(rc))
        return rc;
    if (size > g_shim_heap_size) {
        if (mprotect(g_shim_heap_base + g_shim_heap_size, size - g_shim_heap_size, PROT_READ | PROT_WRITE))
            return KERNELRESULT(OutOfMemory);
    } else if (size < g_shim_heap_size) {
        madvise(g_shim_heap_base + size, g_shim_heap_size - size, MADV_REMOVE);   /* devolve as paginas */
        mprotect(g_shim_heap_base + size, g_shim_heap_size - size, PROT_NONE);
    }
    g_shim_heap_size = size;
    *out_addr = g_shim_heap_base;
    return 0;
}

/* svc 0x02: mprotect dentro da heap; o Horizon so aceita None, R e Rw (X e W sozinho sao invalidos). */
SHIM_API Result svcSetMemoryPermission(void *addr, u64 size, u32 perm)
{
    u64 a = (u64)(uintptr_t)addr;
    if (a % SHIM_PAGE)
        return KERNELRESULT(InvalidAddress);
    if (size == 0 || size % SHIM_PAGE)
        return KERNELRESULT(InvalidSize);
    if (perm != Perm_None && perm != Perm_R && perm != Perm_Rw)
        return KERNELRESULT(InvalidMemoryPermissions);
    if (!shim_in(g_shim_heap_base, g_shim_heap_size, a, size))
        return KERNELRESULT(InvalidMemoryState);
    return mprotect(addr, size, shim_prot(perm)) ? KERNELRESULT(InvalidMemoryState) : 0;
}

/* svc 0x03: so o bit IsUncached pode mudar e attr precisa estar contido na mascara. Nao ha controle
 * de cache de pagina no POSIX: o atributo e so registrado (aparece no svcQueryMemory). */
SHIM_API Result svcSetMemoryAttribute(void *addr, u64 size, u32 val0, u32 val1)
{
    u64 a = (u64)(uintptr_t)addr;
    if (a % SHIM_PAGE)
        return KERNELRESULT(InvalidAddress);
    if (size == 0 || size % SHIM_PAGE)
        return KERNELRESULT(InvalidSize);
    if ((val0 | val1) & ~(u32)MemAttr_IsUncached || (val1 & ~val0))
        return KERNELRESULT(InvalidCombination);
    if (!shim_in(g_shim_heap_base, g_shim_heap_size, a, size))
        return KERNELRESULT(InvalidMemoryState);
    for (int k = 0; k < SHIM_MAX_RANGES; k++)
        if (g_shim_attrs[k].size == 0 || (g_shim_attrs[k].addr == a && g_shim_attrs[k].size == size)) {
            g_shim_attrs[k] = (ShimRange){a, size, 0, (g_shim_attrs[k].attr & ~val0) | val1};
            return 0;
        }
    return KERNELRESULT(OutOfRange);
}

/* svc 0x04 (contraparte de 0x05): alias de heap -> AliasRegion. Origem fica Perm_None + IsBorrowed. */
SHIM_API Result svcMapMemory(void *dst_addr, void *src_addr, u64 size)
{
    u64 d = (u64)(uintptr_t)dst_addr, s = (u64)(uintptr_t)src_addr;
    if (d % SHIM_PAGE || s % SHIM_PAGE)
        return KERNELRESULT(InvalidAddress);
    if (size == 0 || size % SHIM_PAGE)
        return KERNELRESULT(InvalidSize);
    if (!shim_in(g_shim_heap_base, g_shim_heap_size, s, size) ||
        !shim_in(g_shim_alias_base, SHIM_ALIAS_REGION, d, size))
        return KERNELRESULT(InvalidMemoryRange);
    int slot = -1;
    for (int k = 0; k < SHIM_MAX_RANGES; k++)
        if (g_shim_maps[k].size == 0) { slot = k; break; }
    if (slot < 0)
        return KERNELRESULT(OutOfRange);
    off_t off = (off_t)(s - (u64)(uintptr_t)g_shim_heap_base);
    if (mmap(dst_addr, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, g_shim_heap_fd, off) == MAP_FAILED)
        return KERNELRESULT(InvalidMemoryState);
    mprotect(src_addr, size, PROT_NONE);
    g_shim_maps[slot] = (ShimRange){d, size, s, 0};
    return 0;
}

/* svc 0x05: desfaz um svcMapMemory: destino volta a ser reserva PROT_NONE, origem volta a Rw. */
SHIM_API Result svcUnmapMemory(void *dst_addr, void *src_addr, u64 size)
{
    u64 d = (u64)(uintptr_t)dst_addr, s = (u64)(uintptr_t)src_addr;
    for (int k = 0; k < SHIM_MAX_RANGES; k++) {
        ShimRange *m = &g_shim_maps[k];
        if (m->size && m->addr == d && m->src == s && m->size == size) {
            mmap(dst_addr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
            mprotect(src_addr, size, PROT_READ | PROT_WRITE);
            m->size = 0;
            return 0;
        }
    }
    return KERNELRESULT(InvalidMemoryRange);
}

/* svc 0x06: bloco que contem addr, a partir de /proc/self/maps (lacunas e reservas = Unmapped). */
SHIM_API Result svcQueryMemory(MemoryInfo *meminfo_ptr, u32 *pageinfo, u64 addr)
{
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f)
        return KERNELRESULT(InvalidMemoryState);
    unsigned long long lo, hi, prev_end = 0, next_start = ~0ULL;
    char perms[8], line[512];
    int found = 0;
    MemoryInfo mi = {0};
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%llx-%llx %7s", &lo, &hi, perms) != 3)
            continue;
        if (perms[0] == '-' && perms[1] == '-' && perms[2] == '-')
            continue;                                      /* reserva PROT_NONE conta como livre */
        if (addr >= lo && addr < hi) {
            mi.addr = lo;
            mi.size = hi - lo;
            mi.perm = (perms[0] == 'r' ? Perm_R : 0) | (perms[1] == 'w' ? Perm_W : 0) | (perms[2] == 'x' ? Perm_X : 0);
            found = 1;
            break;
        }
        if (hi <= addr && hi > prev_end)
            prev_end = hi;
        if (lo > addr && lo < next_start)
            next_start = lo;
    }
    fclose(f);
    if (!found) {                                         /* bloco livre ao redor de addr */
        mi.addr = prev_end;
        mi.size = (next_start == ~0ULL ? (1ULL << 39) : next_start) - prev_end;
        mi.type = MemType_Unmapped;
    } else if (shim_in(g_shim_heap_base, SHIM_HEAP_REGION, addr, 1)) {
        mi.type = MemType_Heap;
    } else if (shim_in(g_shim_alias_base, SHIM_ALIAS_REGION, addr, 1)) {
        mi.type = MemType_MappedMemory;
    } else {
        mi.type = MemType_Normal;
    }
    for (int k = 0; k < SHIM_MAX_RANGES; k++) {
        if (g_shim_maps[k].size && addr >= g_shim_maps[k].src && addr < g_shim_maps[k].src + g_shim_maps[k].size)
            mi.attr |= MemAttr_IsBorrowed;
        if (g_shim_attrs[k].size && addr >= g_shim_attrs[k].addr && addr < g_shim_attrs[k].addr + g_shim_attrs[k].size)
            mi.attr |= g_shim_attrs[k].attr;
    }
    *meminfo_ptr = mi;
    if (pageinfo)
        *pageinfo = 0;
    return 0;
}

static long shim_futex(u32 *addr, int op, u32 val)
{
    return syscall(SYS_futex, addr, op | FUTEX_PRIVATE_FLAG, val, NULL, NULL, 0);
}

/* svc 0x1A: espera enquanto a tag ainda e (dono | WAIT_MASK). Quando o dono solta (tag = 0), toma a
 * posse escrevendo self_tag | WAIT_MASK (mantem a mascara: o proximo unlock tambem acorda alguem).
 * Se a tag mudou para outro valor, volta com sucesso e a libnx tenta de novo, como no kernel. */
SHIM_API Result svcArbitrateLock(u32 wait_tag, u32 *tag_location, u32 self_tag)
{
    if ((uintptr_t)tag_location % 4)
        return KERNELRESULT(InvalidAddress);
    for (;;) {
        u32 v = __atomic_load_n(tag_location, __ATOMIC_ACQUIRE);
        if (v == 0) {
            if (__atomic_compare_exchange_n(tag_location, &v, self_tag | HANDLE_WAIT_MASK, false,
                                            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                return 0;
            continue;
        }
        if (v != (wait_tag | HANDLE_WAIT_MASK))
            return 0;
        shim_futex(tag_location, FUTEX_WAIT, v);
    }
}

/* svc 0x1B: solta (tag = 0) e acorda um esperador, que disputa a posse no svcArbitrateLock. */
SHIM_API Result svcArbitrateUnlock(u32 *tag_location)
{
    if ((uintptr_t)tag_location % 4)
        return KERNELRESULT(InvalidAddress);
    __atomic_store_n(tag_location, 0, __ATOMIC_RELEASE);
    shim_futex(tag_location, FUTEX_WAKE, 1);
    return 0;
}

/* svc 0x29: informacoes do processo, tiradas do Linux quando ha equivalente. */
SHIM_API Result svcGetInfo(u64 *out, u32 id0, Handle handle, u64 id1)
{
    Result rc = shim_reserve_regions();
    if (R_FAILED(rc))
        return rc;
    if (id0 == InfoType_DebuggerAttached || id0 == InfoType_RandomEntropy) {
        if (handle != INVALID_HANDLE)
            return KERNELRESULT(InvalidHandle);
    } else if (handle != CUR_PROCESS_HANDLE) {
        return KERNELRESULT(InvalidHandle);
    }
    if (id0 != InfoType_RandomEntropy && id1 != 0)
        return KERNELRESULT(InvalidCombination);
    struct sysinfo si;
    switch (id0) {
    case InfoType_CoreMask: {
        u64 mask[16] = {0};                                   /* nucleos permitidos (sched_getaffinity) */
        if (syscall(SYS_sched_getaffinity, 0, sizeof mask, mask) < 0)
            return KERNELRESULT(InvalidState);
        *out = mask[0];
        return 0;
    }
    case InfoType_PriorityMask:         *out = ~0ULL;       return 0;  /* prioridades 0..63 */
    case InfoType_AliasRegionAddress:   *out = (u64)(uintptr_t)g_shim_alias_base; return 0;
    case InfoType_AliasRegionSize:      *out = SHIM_ALIAS_REGION; return 0;
    case InfoType_HeapRegionAddress:    *out = (u64)(uintptr_t)g_shim_heap_base; return 0;
    case InfoType_HeapRegionSize:       *out = SHIM_HEAP_REGION; return 0;
    case InfoType_TotalMemorySize:
    case InfoType_TotalNonSystemMemorySize:
        if (sysinfo(&si))
            return KERNELRESULT(InvalidState);
        *out = (u64)si.totalram * si.mem_unit;
        return 0;
    case InfoType_UsedMemorySize:
    case InfoType_UsedNonSystemMemorySize: {
        FILE *f = fopen("/proc/self/statm", "r");
        unsigned long long vm = 0, rss = 0;
        if (!f || fscanf(f, "%llu %llu", &vm, &rss) != 2) {
            if (f) fclose(f);
            return KERNELRESULT(InvalidState);
        }
        fclose(f);
        *out = rss * (u64)sysconf(_SC_PAGESIZE);
        return 0;
    }
    case InfoType_DebuggerAttached: {
        FILE *f = fopen("/proc/self/status", "r");
        char line[128];
        int tracer = 0;
        while (f && fgets(line, sizeof line, f))
            if (sscanf(line, "TracerPid: %d", &tracer) == 1)
                break;
        if (f) fclose(f);
        *out = tracer != 0;
        return 0;
    }
    case InfoType_RandomEntropy:
        if (id1 > 3)
            return KERNELRESULT(InvalidCombination);
        if (syscall(SYS_getrandom, out, sizeof *out, 0) != (long)sizeof *out)
            return KERNELRESULT(InvalidState);
        return 0;
    case InfoType_AslrRegionAddress:    *out = 0x8000000ULL; return 0;          /* espaco de enderecos do processo */
    case InfoType_AslrRegionSize:       *out = (1ULL << 39) - 0x8000000ULL; return 0;
    case InfoType_StackRegionAddress:
    case InfoType_StackRegionSize:      return KERNELRESULT(InvalidEnumValue); /* pilha vem do Bionic */
    case InfoType_ProgramId:            *out = SHIM_PROGRAM_ID; return 0;
    case InfoType_IsApplication:        *out = 1; return 0;
    default:                            return KERNELRESULT(InvalidEnumValue);
    }
}
#endif /* __linux__ */

/* ------------------------------------------------------------------ timer (arm) */
/* Contador de sistema do Switch: 19,2 MHz. Emulado a partir de CLOCK_MONOTONIC. */
#define SHIM_TICK_FREQ 19200000ULL

SHIM_API u64 armGetSystemTickFreq(void) { return SHIM_TICK_FREQ; }

SHIM_API u64 armNsToTicks(u64 ns) { return ns * 12 / 625; }    /* ns * 19,2e6 / 1e9 */
SHIM_API u64 armTicksToNs(u64 tick) { return tick * 625 / 12; } /* tick * 1e9 / 19,2e6 */

SHIM_API u64 armGetSystemTick(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return armNsToTicks((u64)ts.tv_sec * 1000000000ULL + (u64)ts.tv_nsec);
}

#endif /* SWITCH_HLE_SHIM_H */
