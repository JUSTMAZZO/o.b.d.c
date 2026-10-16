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
 * Esse e o roteiro padrao de entrada; shimInputSetScript troca o roteiro e shimInputSetLive alimenta o
 * padUpdate com estados ao vivo (botoes + analogicos), sem mudar o layout da PadState.
 * fsOpenSdCardFileSystem e as operacoes fsFs/fsFile/fsDir usam um diretorio do host como cartao SD
 * (./sdcard, OBDC_SDCARD ou shimFsSetSdCardRoot), em Bionic e no gate do Windows.
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
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ tipos base */
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
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
#define BITL(n) (1ULL << (n))

#define MAKERESULT(module, description) ((((module) & 0x1FF)) | ((description) & 0x1FFF) << 9)
enum { Module_Libnx = 345 };
enum { LibnxError_IoError = 10, LibnxError_BadInput = 11 };
#define LIBNXRESULT(description) MAKERESULT(Module_Libnx, LibnxError_##description)

/* A, B e Plus ficam com BIT (u32) como antes, para nao mudar o codigo gerado dos benchmarks ja validados;
 * os demais seguem a libnx (BITL, u64). */
#define HidNpadButton_A             BIT(0)
#define HidNpadButton_B             BIT(1)
#define HidNpadButton_X             BITL(2)
#define HidNpadButton_Y             BITL(3)
#define HidNpadButton_StickL        BITL(4)
#define HidNpadButton_StickR        BITL(5)
#define HidNpadButton_L             BITL(6)
#define HidNpadButton_R             BITL(7)
#define HidNpadButton_ZL            BITL(8)
#define HidNpadButton_ZR            BITL(9)
#define HidNpadButton_Plus          BIT(10)
#define HidNpadButton_Minus         BITL(11)
#define HidNpadButton_Left          BITL(12)
#define HidNpadButton_Up            BITL(13)
#define HidNpadButton_Right         BITL(14)
#define HidNpadButton_Down          BITL(15)
#define HidNpadButton_StickLLeft    BITL(16)
#define HidNpadButton_StickLUp      BITL(17)
#define HidNpadButton_StickLRight   BITL(18)
#define HidNpadButton_StickLDown    BITL(19)
#define HidNpadButton_StickRLeft    BITL(20)
#define HidNpadButton_StickRUp      BITL(21)
#define HidNpadButton_StickRRight   BITL(22)
#define HidNpadButton_StickRDown    BITL(23)
#define HidNpadButton_AnyLeft  (HidNpadButton_Left  | HidNpadButton_StickLLeft  | HidNpadButton_StickRLeft)
#define HidNpadButton_AnyUp    (HidNpadButton_Up    | HidNpadButton_StickLUp    | HidNpadButton_StickRUp)
#define HidNpadButton_AnyRight (HidNpadButton_Right | HidNpadButton_StickLRight | HidNpadButton_StickRRight)
#define HidNpadButton_AnyDown  (HidNpadButton_Down  | HidNpadButton_StickLDown  | HidNpadButton_StickRDown)

#define HidNpadStyleSet_NpadStandard 0x1FU /* FullKey | Handheld | JoyDual | JoyLeft | JoyRight */
#define HidNpadAttribute_IsConnected BIT(0)
#define HidNpadIdType_Handheld       0x20
#define PAD_ANY_ID_MASK              0x1000100FFULL

#define JOYSTICK_MAX (0x7FFF)
#define JOYSTICK_MIN (-0x7FFF)

#define SHIM_FRAMES        60
#define SHIM_FRAME_BOOST_A 30
#define SHIM_FRAME_EXIT    60

typedef struct HidAnalogStickState {
    s32 x;
    s32 y;
} HidAnalogStickState;

/* Layout compacto: e a PadState da libnx 2.10.0 sem sticks[2] e gc_triggers[2]. Fica assim de proposito
 * (frames de pilha dos benchmarks reconstruidos da Fase 2); os analogicos vivem no estado do shim. */
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

/*
 * Fonte de entrada desacoplada do padUpdate. O padUpdate so consulta um estado (botoes + 2 analogicos):
 *   1. buffer ao vivo (shimInputSetLive), se ativo: um produtor (ex.: thread de input do Android) publica
 *      estados por seqlock e cada padUpdate le um retrato consistente, sem misturar dois estados;
 *   2. senao, o roteiro por quadro (shimInputSetScript): cada evento vale do seu quadro ate o proximo.
 * Sem roteiro customizado vale o padrao deterministico do benchmark (A no quadro 30, Plus no 60).
 */
typedef struct {
    u64 buttons;                     /* HidNpadButton_* */
    HidAnalogStickState sticks[2];   /* 0 = esquerdo, 1 = direito (JOYSTICK_MIN..JOYSTICK_MAX) */
} ShimInputState;

typedef struct {
    u32 frame;                       /* quadro do appletMainLoop (1..) a partir do qual o estado vale */
    ShimInputState state;
} ShimInputEvent;

#define SHIM_INPUT_MAX_EVENTS 256

static const ShimInputEvent g_shim_input_default[] = {
    { .frame = SHIM_FRAME_BOOST_A,     .state = { .buttons = HidNpadButton_A } },
    { .frame = SHIM_FRAME_BOOST_A + 1, .state = { .buttons = 0 } },
    { .frame = SHIM_FRAME_EXIT,        .state = { .buttons = HidNpadButton_Plus } },
    { .frame = SHIM_FRAME_EXIT + 1,    .state = { .buttons = 0 } },
};
static ShimInputEvent g_shim_input_script[SHIM_INPUT_MAX_EVENTS];
static u32 g_shim_input_count;
static bool g_shim_input_custom;

static ShimInputState g_shim_live;
static u32 g_shim_live_seq;          /* par = estavel, impar = escrita em andamento */
static u32 g_shim_live_on;
static ShimInputState g_shim_pad_input;   /* estado lido no ultimo padUpdate (padGetStickPos) */

/* Quadro atual do loop do applet (1..SHIM_FRAMES); 0 antes do primeiro quadro. */
static u32 g_shim_frame;
static u32 g_shim_max_players;
static u32 g_shim_style_set;

/* Copia o roteiro (quadros estritamente crescentes). events == NULL volta ao padrao; count == 0 = sem input. */
SHIM_API Result shimInputSetScript(const ShimInputEvent *events, u32 count)
{
    if (!events) {
        g_shim_input_custom = false;
        g_shim_input_count = 0;
        return 0;
    }
    if (count > SHIM_INPUT_MAX_EVENTS)
        return LIBNXRESULT(BadInput);
    for (u32 k = 1; k < count; k++)
        if (events[k].frame <= events[k - 1].frame)
            return LIBNXRESULT(BadInput);
    memcpy(g_shim_input_script, events, count * sizeof *events);
    g_shim_input_count = count;
    g_shim_input_custom = true;
    return 0;
}

/* Produtor unico: publica um estado completo; os padUpdate seguintes passam a ler o buffer ao vivo. */
SHIM_API void shimInputSetLive(const ShimInputState *st)
{
    u32 seq = __atomic_load_n(&g_shim_live_seq, __ATOMIC_RELAXED);
    __atomic_store_n(&g_shim_live_seq, seq + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&g_shim_live.buttons, st->buttons, __ATOMIC_RELAXED);
    for (int k = 0; k < 2; k++) {
        __atomic_store_n(&g_shim_live.sticks[k].x, st->sticks[k].x, __ATOMIC_RELAXED);
        __atomic_store_n(&g_shim_live.sticks[k].y, st->sticks[k].y, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&g_shim_live_seq, seq + 2, __ATOMIC_RELEASE);
    __atomic_store_n(&g_shim_live_on, 1, __ATOMIC_RELEASE);
}

/* Desliga o buffer ao vivo: padUpdate volta ao roteiro. */
SHIM_API void shimInputClearLive(void) { __atomic_store_n(&g_shim_live_on, 0, __ATOMIC_RELEASE); }

static ShimInputState shim_input_live(void)
{
    ShimInputState st;
    u32 s0, s1;
    do {
        s0 = __atomic_load_n(&g_shim_live_seq, __ATOMIC_ACQUIRE);
        st.buttons = __atomic_load_n(&g_shim_live.buttons, __ATOMIC_RELAXED);
        for (int k = 0; k < 2; k++) {
            st.sticks[k].x = __atomic_load_n(&g_shim_live.sticks[k].x, __ATOMIC_RELAXED);
            st.sticks[k].y = __atomic_load_n(&g_shim_live.sticks[k].y, __ATOMIC_RELAXED);
        }
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        s1 = __atomic_load_n(&g_shim_live_seq, __ATOMIC_RELAXED);
    } while ((s0 & 1) || s0 != s1);
    return st;
}

static ShimInputState shim_input_at(u32 frame)
{
    const ShimInputEvent *ev = g_shim_input_custom ? g_shim_input_script : g_shim_input_default;
    u32 n = g_shim_input_custom ? g_shim_input_count
                                : (u32)(sizeof g_shim_input_default / sizeof g_shim_input_default[0]);
    ShimInputState st = {0};
    for (u32 k = 0; k < n && ev[k].frame <= frame; k++)
        st = ev[k].state;
    return st;
}

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

/* Uma unica fonte de entrada no shim: todo id pedido e reportado como ativo. */
SHIM_API void padInitializeWithMask(PadState *pad, u64 mask)
{
    *pad = (PadState){0};
    pad->id_mask = pad->active_id_mask = (u8)mask;
    pad->read_handheld = pad->active_handheld = (mask & BITL(HidNpadIdType_Handheld)) != 0;
    pad->style_set = g_shim_style_set;
}

SHIM_API void padInitializeAny(PadState *pad) { padInitializeWithMask(pad, PAD_ANY_ID_MASK); }

SHIM_API void padUpdate(PadState *pad)
{
    ShimInputState st = __atomic_load_n(&g_shim_live_on, __ATOMIC_ACQUIRE) ? shim_input_live()
                                                                           : shim_input_at(g_shim_frame);
    g_shim_pad_input = st;
    pad->attributes = HidNpadAttribute_IsConnected;
    pad->buttons_old = pad->buttons_cur;
    pad->buttons_cur = st.buttons;
}

SHIM_API u64 padGetButtons(const PadState *pad)     { return pad->buttons_cur; }
SHIM_API u64 padGetButtonsDown(const PadState *pad) { return pad->buttons_cur & ~pad->buttons_old; }
SHIM_API u64 padGetButtonsUp(const PadState *pad)   { return ~pad->buttons_cur & pad->buttons_old; }
SHIM_API bool padIsConnected(const PadState *pad)   { return pad->attributes & HidNpadAttribute_IsConnected; }
SHIM_API bool padIsHandheld(const PadState *pad)    { return pad->active_handheld; }
SHIM_API u32 padGetStyleSet(const PadState *pad)    { return pad->style_set; }
SHIM_API u32 padGetAttributes(const PadState *pad)  { return pad->attributes; }

/* 0 = esquerdo, 1 = direito. A PadState compacta nao guarda os analogicos: vem do ultimo padUpdate. */
SHIM_API HidAnalogStickState padGetStickPos(const PadState *pad, unsigned i)
{
    (void)pad;
    return i < 2 ? g_shim_pad_input.sticks[i] : (HidAnalogStickState){0, 0};
}

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

/* ---- servicos da libnx (IPC): dummies do mapeamento da Fase 3. No Switch cada um abre uma sessao com o
 * servico (sm: por svcConnectToNamedPort; hid, fsp-srv, appletOE... pelo sm) e conversa por
 * svcSendSyncRequest; aqui so devolvem sucesso, para o homebrew seguir em frente. */
SHIM_API Result smInitialize(void) { return 0; }
SHIM_API void smExit(void) {}
SHIM_API Result appletInitialize(void) { return 0; }
SHIM_API void appletExit(void) {}
SHIM_API Result hidInitialize(void) { return 0; }
SHIM_API void hidExit(void) {}
SHIM_API Result fsInitialize(void) { return 0; }
SHIM_API void fsExit(void) {}

/* ------------------------------------------------------------------ fs: cartao SD sobre um diretorio do host */
/*
 * API de alto nivel da libnx (fsOpenSdCardFileSystem, fsFs*, fsFile*, fsDir*) interceptada em C, sem CMIF.
 * O cartao SD e um diretorio do host. Raiz, em ordem de prioridade:
 *   shimFsSetSdCardRoot(dir)  >  variavel de ambiente OBDC_SDCARD  >  macro OBDC_SDCARD_ROOT ("./sdcard").
 * A raiz e copiada quando o FsFileSystem e aberto; trocar depois nao afeta sistemas ja abertos.
 *
 * Caminhos do Horizon ("/dir/arq") sao normalizados lexicamente antes de tocar o host: "//" e "." somem,
 * ".." sobe um nivel e nunca passa da raiz (DirectoryUnobtainable, como o PathNormalizer do fs);
 * \ : * ? < > | e caracteres de controle sao InvalidCharacter (no Windows virariam separador/drive).
 * Apagar recursivamente usa lstat e nao segue symlinks. Leitura e escrita seguem symlinks que ja estejam
 * dentro da raiz (o shim nao cria nenhum).
 *
 * Regras do fsp-srv reproduzidas: arquivo precisa existir para abrir; escrever alem do fim exige
 * FsOpenMode_Append; apagar/renomear algo aberto e TargetLocked; abrir para escrita algo ja aberto (ou
 * abrir algo aberto para escrita) e TargetLocked. Codigos do fsp-srv (modulo 2), numeracao do Atmosphere.
 *
 * Mesmo codigo no Bionic e no gate do Windows (llvm-mingw). Leitura posicional: pread no Linux,
 * _lseeki64 + _read no Windows (no host, um mesmo FsFile nao deve ser lido por duas threads ao mesmo tempo).
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#else
#include <sys/statvfs.h>
#include <unistd.h>
#endif

#ifndef OBDC_SDCARD_ROOT
#define OBDC_SDCARD_ROOT "./sdcard"
#endif

typedef struct Service {
    Handle session;
    u32 own_handle;
    u32 object_id;
    u16 pointer_buffer_size;
} Service;

#define FS_MAX_PATH 0x301

typedef struct { Service s; } FsFileSystem;
typedef struct { Service s; } FsFile;
typedef struct { Service s; } FsDir;

typedef struct {
    char name[FS_MAX_PATH];
    u8 pad[3];
    s8 type;                 /* FsDirEntryType */
    u8 pad2[3];
    s64 file_size;
} FsDirectoryEntry;

typedef enum { FsDirEntryType_Dir = 0, FsDirEntryType_File = 1 } FsDirEntryType;
typedef enum { FsOpenMode_Read = BIT(0), FsOpenMode_Write = BIT(1), FsOpenMode_Append = BIT(2) } FsOpenMode;
typedef enum { FsCreateOption_BigFile = BIT(0) } FsCreateOption;
typedef enum { FsDirOpenMode_ReadDirs = BIT(0), FsDirOpenMode_ReadFiles = BIT(1) } FsDirOpenMode;
#define FsDirOpenMode_NoFileSize BIT(31)   /* fora do enum: 0x80000000 nao cabe em int (-Wpedantic) */
typedef enum { FsReadOption_None = 0 } FsReadOption;
typedef enum { FsWriteOption_None = 0, FsWriteOption_Flush = BIT(0) } FsWriteOption;

enum { Module_Fs = 2 };
enum {
    FsError_PathNotFound = 1,
    FsError_PathAlreadyExists = 2,
    FsError_TargetLocked = 7,
    FsError_DirectoryNotEmpty = 8,
    FsError_UsableSpaceNotEnough = 30,
    FsError_SdCardNotPresent = 2001,
    FsError_NotImplemented = 3001,
    FsError_OutOfRange = 3005,
    FsError_TooLongPath = 6003,
    FsError_InvalidCharacter = 6004,
    FsError_InvalidPathFormat = 6005,
    FsError_DirectoryUnobtainable = 6006,
    FsError_DirectoryNotDeletable = 6031,
    FsError_DirectoryNotRenamable = 6032,
    FsError_InvalidOffset = 6061,
    FsError_InvalidSize = 6062,
    FsError_NullptrArgument = 6063,
    FsError_InvalidOpenMode = 6072,
    FsError_FileExtensionWithoutOpenModeAllowAppend = 6201,
    FsError_ReadNotPermitted = 6202,
    FsError_WriteNotPermitted = 6203,
    FsError_PermissionDenied = 6400,
    FsError_OpenCountLimit = 6706,
};
#define FSRESULT(description) MAKERESULT(Module_Fs, FsError_##description)

#define SHIM_FS_ROOT_MAX    1024
#define SHIM_FS_HOST_MAX    (SHIM_FS_ROOT_MAX + FS_MAX_PATH)
#define SHIM_FS_MAX_MOUNTS  8
#define SHIM_FS_MAX_FILES   32
#define SHIM_FS_MAX_DIRS    16
#define SHIM_FS_HANDLE_FS   0x1000U   /* faixas proprias, longe dos handles do kernel (0x200..) */
#define SHIM_FS_HANDLE_FILE 0x2000U
#define SHIM_FS_HANDLE_DIR  0x3000U
#define SHIM_FS_KIND_NONE   (-1)      /* shim_fs_kind: alem de FsDirEntryType_Dir / _File */
#define SHIM_FS_KIND_OTHER  2         /* symlink (sem seguir), dispositivo, fifo... */

#if defined(_WIN32)
typedef struct _stat64 ShimStat;
#define shim_stat(p, st)  _stat64((p), (st))
#define shim_lstat(p, st) _stat64((p), (st))   /* sem lstat no CRT; symlinks sao raros no host */
#define shim_fstat(fd, st) _fstat64((fd), (st))
#define shim_sys_mkdir(p) _mkdir(p)
#define shim_sys_rmdir    _rmdir
#define shim_sys_unlink   _unlink
#define shim_sys_close    _close
#define shim_sys_fsync    _commit
#else
typedef struct stat ShimStat;
#define shim_stat(p, st)  stat((p), (st))
#define shim_lstat(p, st) lstat((p), (st))
#define shim_fstat(fd, st) fstat((fd), (st))
#define shim_sys_mkdir(p) mkdir((p), 0777)
#define shim_sys_rmdir    rmdir
#define shim_sys_unlink   unlink
#define shim_sys_close    close
#define shim_sys_fsync    fsync
#endif

typedef struct { bool used; char root[SHIM_FS_ROOT_MAX]; } ShimFsMount;
typedef struct { bool used; int fd; u32 mode; char host[SHIM_FS_HOST_MAX]; } ShimFsFile;
typedef struct { bool used; DIR *dir; u32 mode; char host[SHIM_FS_HOST_MAX]; } ShimFsDir;

static ShimFsMount g_shim_fs_mounts[SHIM_FS_MAX_MOUNTS];
static ShimFsFile g_shim_fs_files[SHIM_FS_MAX_FILES];
static ShimFsDir g_shim_fs_dirs[SHIM_FS_MAX_DIRS];
static char g_shim_sdcard_root[SHIM_FS_ROOT_MAX];    /* "" = OBDC_SDCARD ou OBDC_SDCARD_ROOT */
static u32 g_shim_fs_lock;                            /* tabelas acima; spinlock portavel */

static void shim_fs_lock(void)   { while (__atomic_exchange_n(&g_shim_fs_lock, 1, __ATOMIC_ACQUIRE)) {} }
static void shim_fs_unlock(void) { __atomic_store_n(&g_shim_fs_lock, 0, __ATOMIC_RELEASE); }

static ShimFsMount *shim_fs_mount(const FsFileSystem *fs)
{
    u32 k = fs ? fs->s.session - SHIM_FS_HANDLE_FS : ~0U;
    return k < SHIM_FS_MAX_MOUNTS && g_shim_fs_mounts[k].used ? &g_shim_fs_mounts[k] : NULL;
}

static ShimFsFile *shim_fs_file(const FsFile *f)
{
    u32 k = f ? f->s.session - SHIM_FS_HANDLE_FILE : ~0U;
    return k < SHIM_FS_MAX_FILES && g_shim_fs_files[k].used ? &g_shim_fs_files[k] : NULL;
}

static ShimFsDir *shim_fs_dir(const FsDir *d)
{
    u32 k = d ? d->s.session - SHIM_FS_HANDLE_DIR : ~0U;
    return k < SHIM_FS_MAX_DIRS && g_shim_fs_dirs[k].used ? &g_shim_fs_dirs[k] : NULL;
}

static Result shim_fs_errno(int e)
{
    switch (e) {
    case ENOENT: case ENOTDIR:  return FSRESULT(PathNotFound);
    case EEXIST:                return FSRESULT(PathAlreadyExists);
    case ENOTEMPTY:             return FSRESULT(DirectoryNotEmpty);
    case ENOSPC:                return FSRESULT(UsableSpaceNotEnough);
    case ENAMETOOLONG:          return FSRESULT(TooLongPath);
    case EMFILE: case ENFILE:   return FSRESULT(OpenCountLimit);
    case EACCES: case EPERM:    return FSRESULT(PermissionDenied);
    case EBUSY:                 return FSRESULT(TargetLocked);
    default:                    return LIBNXRESULT(IoError);
    }
}

/* "/a/./b//../c" -> "/a/c"; a raiz vira "". out tem FS_MAX_PATH bytes e nunca cresce alem da entrada. */
static Result shim_fs_normalize(const char *path, char *out)
{
    if (!path)
        return FSRESULT(NullptrArgument);
    if (path[0] != '/')
        return FSRESULT(InvalidPathFormat);
    if (strnlen(path, FS_MAX_PATH) >= FS_MAX_PATH)
        return FSRESULT(TooLongPath);
    size_t o = 0;
    const char *p = path;
    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        const char *q = p;
        for (; *q && *q != '/'; q++)
            if ((unsigned char)*q < 0x20 || strchr("\\:*?<>|", *q))
                return FSRESULT(InvalidCharacter);
        size_t n = (size_t)(q - p);
        if (n == 2 && p[0] == '.' && p[1] == '.') {
            if (o == 0)
                return FSRESULT(DirectoryUnobtainable);   /* tentaria sair da raiz */
            while (out[o - 1] != '/')
                o--;
            o--;
        } else if (!(n == 1 && p[0] == '.')) {
            out[o++] = '/';
            memcpy(out + o, p, n);
            o += n;
        }
        p = q;
    }
    out[o] = '\0';
    return 0;
}

/* host = raiz do FsFileSystem + caminho normalizado. */
static Result shim_fs_host(const FsFileSystem *fs, const char *path, char *host, bool *is_root)
{
    ShimFsMount *m = shim_fs_mount(fs);
    if (!m)
        return KERNELRESULT(InvalidHandle);
    char norm[FS_MAX_PATH];
    Result rc = shim_fs_normalize(path, norm);
    if (R_FAILED(rc))
        return rc;
    if (is_root)
        *is_root = norm[0] == '\0';
    snprintf(host, SHIM_FS_HOST_MAX, "%s%s", m->root, norm);
    return 0;
}

static int shim_fs_kind(const char *host, bool follow)
{
    ShimStat st;
    if ((follow ? shim_stat(host, &st) : shim_lstat(host, &st)) != 0)
        return SHIM_FS_KIND_NONE;
    return S_ISDIR(st.st_mode) ? FsDirEntryType_Dir : S_ISREG(st.st_mode) ? FsDirEntryType_File : SHIM_FS_KIND_OTHER;
}

/* Algum FsFile aberto em host (ou abaixo dele, se subtree)? Chamar com o lock. */
static bool shim_fs_in_use(const char *host, bool subtree)
{
    size_t n = strlen(host);
    for (int k = 0; k < SHIM_FS_MAX_FILES; k++) {
        const ShimFsFile *f = &g_shim_fs_files[k];
        if (f->used && (strcmp(f->host, host) == 0 ||
                        (subtree && strncmp(f->host, host, n) == 0 && f->host[n] == '/')))
            return true;
    }
    return false;
}

static int shim_fs_open_fd(const char *host, int flags)
{
#if defined(_WIN32)
    return _open(host, flags | O_BINARY, _S_IREAD | _S_IWRITE);
#else
    return open(host, flags | O_CLOEXEC, 0666);
#endif
}

/* Devolve os bytes transferidos ou -1 (errno). Em pedacos de 1 GiB (o _read do Windows usa unsigned). */
static long long shim_fs_pio(int fd, void *buf, u64 size, s64 off, bool write_op)
{
    u8 *p = (u8 *)buf;
    u64 done = 0;
#if defined(_WIN32)
    if (_lseeki64(fd, off, SEEK_SET) < 0)
        return -1;
#endif
    while (done < size) {
        u64 chunk = size - done > 0x40000000ULL ? 0x40000000ULL : size - done;
#if defined(_WIN32)
        int r = write_op ? _write(fd, p + done, (unsigned)chunk) : _read(fd, p + done, (unsigned)chunk);
#else
        ssize_t r = write_op ? pwrite(fd, p + done, (size_t)chunk, (off_t)(off + (s64)done))
                             : pread(fd, p + done, (size_t)chunk, (off_t)(off + (s64)done));
#endif
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (r == 0)
            break;
        done += (u64)r;
    }
    return (long long)done;
}

/* 0 ou errno. */
static int shim_fs_truncate(int fd, s64 size)
{
#if defined(_WIN32)
    return _chsize_s(fd, size);
#else
    return ftruncate(fd, (off_t)size) ? errno : 0;
#endif
}

static s64 shim_fs_fd_size(int fd)
{
    ShimStat st;
    return shim_fstat(fd, &st) ? -1 : (s64)st.st_size;
}

/* Apaga o conteudo de host (e o proprio host se !keep_self), sem seguir symlinks. 0 ou errno. */
static int shim_fs_rmtree(const char *host, bool keep_self)
{
    DIR *dir = opendir(host);
    if (!dir)
        return errno;
    char *child = (char *)malloc(SHIM_FS_HOST_MAX);
    int err = child ? 0 : ENOMEM;
    struct dirent *e;
    while (!err && (e = readdir(dir)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        if (snprintf(child, SHIM_FS_HOST_MAX, "%s/%s", host, e->d_name) >= SHIM_FS_HOST_MAX) {
            err = ENAMETOOLONG;
            break;
        }
        if (shim_fs_kind(child, false) == FsDirEntryType_Dir)
            err = shim_fs_rmtree(child, false);
        else if (shim_sys_unlink(child))
            err = errno;
    }
    closedir(dir);
    free(child);
    if (!err && !keep_self && shim_sys_rmdir(host))
        err = errno;
    return err;
}

/* Raiz do proximo fsOpenSdCardFileSystem; NULL volta a OBDC_SDCARD / OBDC_SDCARD_ROOT. */
SHIM_API Result shimFsSetSdCardRoot(const char *dir)
{
    if (!dir) {
        g_shim_sdcard_root[0] = '\0';
        return 0;
    }
    size_t n = strlen(dir);
    if (n == 0)
        return FSRESULT(InvalidPathFormat);
    if (n >= SHIM_FS_ROOT_MAX)
        return FSRESULT(TooLongPath);
    memcpy(g_shim_sdcard_root, dir, n + 1);
    return 0;
}

SHIM_API Result fsOpenSdCardFileSystem(FsFileSystem *out)
{
    const char *root = g_shim_sdcard_root[0] ? g_shim_sdcard_root : getenv("OBDC_SDCARD");
    if (!root || !*root)
        root = OBDC_SDCARD_ROOT;
    size_t n = strlen(root);
    while (n > 1 && (root[n - 1] == '/' || root[n - 1] == '\\'))
        n--;
    if (n >= SHIM_FS_ROOT_MAX)
        return FSRESULT(TooLongPath);
    char buf[SHIM_FS_ROOT_MAX];
    memcpy(buf, root, n);
    buf[n] = '\0';
    if (shim_fs_kind(buf, true) != FsDirEntryType_Dir)
        return FSRESULT(SdCardNotPresent);
    shim_fs_lock();
    for (u32 k = 0; k < SHIM_FS_MAX_MOUNTS; k++)
        if (!g_shim_fs_mounts[k].used) {
            g_shim_fs_mounts[k].used = true;
            memcpy(g_shim_fs_mounts[k].root, buf, n + 1);
            shim_fs_unlock();
            *out = (FsFileSystem){ { SHIM_FS_HANDLE_FS + k, 1, 0, 0 } };
            return 0;
        }
    shim_fs_unlock();
    return FSRESULT(OpenCountLimit);
}

SHIM_API void fsFsClose(FsFileSystem *fs)
{
    shim_fs_lock();
    ShimFsMount *m = shim_fs_mount(fs);
    if (m)
        m->used = false;     /* FsFile/FsDir abertos continuam validos, como sessoes separadas no Horizon */
    shim_fs_unlock();
    if (fs)
        memset(fs, 0, sizeof *fs);
}

SHIM_API Result fsFsCommit(FsFileSystem *fs) { return shim_fs_mount(fs) ? 0 : KERNELRESULT(InvalidHandle); }

SHIM_API Result fsFsGetEntryType(FsFileSystem *fs, const char *path, FsDirEntryType *out)
{
    char host[SHIM_FS_HOST_MAX];
    Result rc = shim_fs_host(fs, path, host, NULL);
    if (R_FAILED(rc))
        return rc;
    if (!out)
        return FSRESULT(NullptrArgument);
    int kind = shim_fs_kind(host, true);
    if (kind != FsDirEntryType_Dir && kind != FsDirEntryType_File)
        return FSRESULT(PathNotFound);
    *out = (FsDirEntryType)kind;
    return 0;
}

/* option (BigFile = ConcatenationFile) nao muda nada no host: vira um arquivo comum. */
SHIM_API Result fsFsCreateFile(FsFileSystem *fs, const char *path, s64 size, u32 option)
{
    (void)option;
    char host[SHIM_FS_HOST_MAX];
    bool is_root;
    Result rc = shim_fs_host(fs, path, host, &is_root);
    if (R_FAILED(rc))
        return rc;
    if (size < 0)
        return FSRESULT(InvalidSize);
    if (is_root)
        return FSRESULT(PathAlreadyExists);
    int fd = shim_fs_open_fd(host, O_WRONLY | O_CREAT | O_EXCL);
    if (fd < 0)
        return shim_fs_errno(errno);
    int err = shim_fs_truncate(fd, size);
    shim_sys_close(fd);
    if (err) {
        shim_sys_unlink(host);
        return shim_fs_errno(err);
    }
    return 0;
}

SHIM_API Result fsFsDeleteFile(FsFileSystem *fs, const char *path)
{
    char host[SHIM_FS_HOST_MAX];
    Result rc = shim_fs_host(fs, path, host, NULL);
    if (R_FAILED(rc))
        return rc;
    if (shim_fs_kind(host, false) != FsDirEntryType_File)
        return FSRESULT(PathNotFound);
    shim_fs_lock();
    if (shim_fs_in_use(host, false))
        rc = FSRESULT(TargetLocked);
    else if (shim_sys_unlink(host))
        rc = shim_fs_errno(errno);
    shim_fs_unlock();
    return rc;
}

SHIM_API Result fsFsCreateDirectory(FsFileSystem *fs, const char *path)
{
    char host[SHIM_FS_HOST_MAX];
    bool is_root;
    Result rc = shim_fs_host(fs, path, host, &is_root);
    if (R_FAILED(rc))
        return rc;
    if (is_root)
        return FSRESULT(PathAlreadyExists);
    return shim_sys_mkdir(host) ? shim_fs_errno(errno) : 0;
}

SHIM_API Result fsFsDeleteDirectory(FsFileSystem *fs, const char *path)
{
    char host[SHIM_FS_HOST_MAX];
    bool is_root;
    Result rc = shim_fs_host(fs, path, host, &is_root);
    if (R_FAILED(rc))
        return rc;
    if (is_root)
        return FSRESULT(DirectoryNotDeletable);
    if (shim_fs_kind(host, false) != FsDirEntryType_Dir)
        return FSRESULT(PathNotFound);
    if (shim_sys_rmdir(host))
        return errno == EEXIST ? FSRESULT(DirectoryNotEmpty) : shim_fs_errno(errno);
    return 0;
}

static Result shim_fs_delete_tree(FsFileSystem *fs, const char *path, bool keep_self)
{
    char host[SHIM_FS_HOST_MAX];
    bool is_root;
    Result rc = shim_fs_host(fs, path, host, &is_root);
    if (R_FAILED(rc))
        return rc;
    if (is_root && !keep_self)
        return FSRESULT(DirectoryNotDeletable);
    if (shim_fs_kind(host, false) != FsDirEntryType_Dir)
        return FSRESULT(PathNotFound);
    shim_fs_lock();
    if (shim_fs_in_use(host, true)) {
        rc = FSRESULT(TargetLocked);
    } else {
        int err = shim_fs_rmtree(host, keep_self);
        rc = err ? shim_fs_errno(err) : 0;
    }
    shim_fs_unlock();
    return rc;
}

SHIM_API Result fsFsDeleteDirectoryRecursively(FsFileSystem *fs, const char *path) { return shim_fs_delete_tree(fs, path, false); }
SHIM_API Result fsFsCleanDirectoryRecursively(FsFileSystem *fs, const char *path)  { return shim_fs_delete_tree(fs, path, true); }

static Result shim_fs_rename(FsFileSystem *fs, const char *cur_path, const char *new_path, int kind)
{
    char a[SHIM_FS_HOST_MAX], b[SHIM_FS_HOST_MAX];
    bool a_root, b_root;
    Result rc = shim_fs_host(fs, cur_path, a, &a_root);
    if (R_SUCCEEDED(rc))
        rc = shim_fs_host(fs, new_path, b, &b_root);
    if (R_FAILED(rc))
        return rc;
    if (a_root)
        return kind == FsDirEntryType_Dir ? FSRESULT(DirectoryNotRenamable) : FSRESULT(PathNotFound);
    if (shim_fs_kind(a, false) != kind)
        return FSRESULT(PathNotFound);
    if (b_root || shim_fs_kind(b, false) != SHIM_FS_KIND_NONE)
        return FSRESULT(PathAlreadyExists);
    size_t n = strlen(a);
    if (kind == FsDirEntryType_Dir && strncmp(b, a, n) == 0 && b[n] == '/')
        return FSRESULT(DirectoryNotRenamable);          /* para dentro de si mesmo */
    shim_fs_lock();
    if (shim_fs_in_use(a, kind == FsDirEntryType_Dir))
        rc = FSRESULT(TargetLocked);
    else if (rename(a, b))
        rc = shim_fs_errno(errno);
    shim_fs_unlock();
    return rc;
}

SHIM_API Result fsFsRenameFile(FsFileSystem *fs, const char *cur_path, const char *new_path)
{
    return shim_fs_rename(fs, cur_path, new_path, FsDirEntryType_File);
}

SHIM_API Result fsFsRenameDirectory(FsFileSystem *fs, const char *cur_path, const char *new_path)
{
    return shim_fs_rename(fs, cur_path, new_path, FsDirEntryType_Dir);
}

/* Espaco do volume que contem a raiz (statvfs). No gate do Windows: NotImplemented. */
static Result shim_fs_space(FsFileSystem *fs, const char *path, s64 *out, bool total)
{
    char host[SHIM_FS_HOST_MAX];
    Result rc = shim_fs_host(fs, path, host, NULL);
    if (R_FAILED(rc))
        return rc;
    if (!out)
        return FSRESULT(NullptrArgument);
#if defined(_WIN32)
    (void)total;
    return FSRESULT(NotImplemented);
#else
    struct statvfs sv;
    if (statvfs(host, &sv))
        return shim_fs_errno(errno);
    *out = (s64)((total ? (u64)sv.f_blocks : (u64)sv.f_bavail) * (u64)sv.f_frsize);
    return 0;
#endif
}

SHIM_API Result fsFsGetFreeSpace(FsFileSystem *fs, const char *path, s64 *out)  { return shim_fs_space(fs, path, out, false); }
SHIM_API Result fsFsGetTotalSpace(FsFileSystem *fs, const char *path, s64 *out) { return shim_fs_space(fs, path, out, true); }

SHIM_API Result fsFsOpenFile(FsFileSystem *fs, const char *path, u32 mode, FsFile *out)
{
    char host[SHIM_FS_HOST_MAX];
    Result rc = shim_fs_host(fs, path, host, NULL);
    if (R_FAILED(rc))
        return rc;
    if (!out)
        return FSRESULT(NullptrArgument);
    if ((mode & ~(u32)(FsOpenMode_Read | FsOpenMode_Write | FsOpenMode_Append)) ||
        !(mode & (FsOpenMode_Read | FsOpenMode_Write)))
        return FSRESULT(InvalidOpenMode);
    if (shim_fs_kind(host, true) != FsDirEntryType_File)
        return FSRESULT(PathNotFound);
    shim_fs_lock();
    int slot = -1;
    for (int k = 0; k < SHIM_FS_MAX_FILES; k++) {
        const ShimFsFile *f = &g_shim_fs_files[k];
        if (!f->used) {
            if (slot < 0)
                slot = k;
        } else if (strcmp(f->host, host) == 0 && ((f->mode | mode) & FsOpenMode_Write)) {
            shim_fs_unlock();
            return FSRESULT(TargetLocked);
        }
    }
    if (slot < 0) {
        shim_fs_unlock();
        return FSRESULT(OpenCountLimit);
    }
    int fd = shim_fs_open_fd(host, (mode & FsOpenMode_Write) ? O_RDWR : O_RDONLY);
    if (fd < 0) {
        rc = shim_fs_errno(errno);
        shim_fs_unlock();
        return rc;
    }
    ShimFsFile *f = &g_shim_fs_files[slot];
    f->used = true;
    f->fd = fd;
    f->mode = mode;
    memcpy(f->host, host, strlen(host) + 1);
    shim_fs_unlock();
    *out = (FsFile){ { SHIM_FS_HANDLE_FILE + (u32)slot, 1, 0, 0 } };
    return 0;
}

/* Le ate read_size bytes a partir de off; no fim do arquivo devolve menos (0 em off == tamanho). */
SHIM_API Result fsFileRead(FsFile *f, s64 off, void *buf, u64 read_size, u32 option, u64 *bytes_read)
{
    (void)option;
    ShimFsFile *o = shim_fs_file(f);
    if (!o)
        return KERNELRESULT(InvalidHandle);
    if (!(o->mode & FsOpenMode_Read))
        return FSRESULT(ReadNotPermitted);
    if (off < 0)
        return FSRESULT(InvalidOffset);
    if (!bytes_read || (!buf && read_size))
        return FSRESULT(NullptrArgument);
    s64 size = shim_fs_fd_size(o->fd);
    if (size < 0)
        return shim_fs_errno(errno);
    if (off > size)
        return FSRESULT(OutOfRange);
    u64 n = read_size < (u64)(size - off) ? read_size : (u64)(size - off);
    long long r = n ? shim_fs_pio(o->fd, buf, n, off, false) : 0;
    if (r < 0)
        return shim_fs_errno(errno);
    *bytes_read = (u64)r;
    return 0;
}

SHIM_API Result fsFileWrite(FsFile *f, s64 off, const void *buf, u64 write_size, u32 option)
{
    ShimFsFile *o = shim_fs_file(f);
    if (!o)
        return KERNELRESULT(InvalidHandle);
    if (!(o->mode & FsOpenMode_Write))
        return FSRESULT(WriteNotPermitted);
    if (off < 0)
        return FSRESULT(InvalidOffset);
    if (!buf && write_size)
        return FSRESULT(NullptrArgument);
    s64 size = shim_fs_fd_size(o->fd);
    if (size < 0)
        return shim_fs_errno(errno);
    if ((u64)off + write_size > (u64)size && !(o->mode & FsOpenMode_Append))
        return FSRESULT(FileExtensionWithoutOpenModeAllowAppend);
    long long r = write_size ? shim_fs_pio(o->fd, (void *)(uintptr_t)buf, write_size, off, true) : 0;
    if (r < 0 || (u64)r != write_size)
        return r < 0 ? shim_fs_errno(errno) : FSRESULT(UsableSpaceNotEnough);
    if ((option & FsWriteOption_Flush) && shim_sys_fsync(o->fd))
        return shim_fs_errno(errno);
    return 0;
}

SHIM_API Result fsFileFlush(FsFile *f)
{
    ShimFsFile *o = shim_fs_file(f);
    if (!o)
        return KERNELRESULT(InvalidHandle);
    if ((o->mode & FsOpenMode_Write) && shim_sys_fsync(o->fd))
        return shim_fs_errno(errno);
    return 0;
}

SHIM_API Result fsFileSetSize(FsFile *f, s64 sz)
{
    ShimFsFile *o = shim_fs_file(f);
    if (!o)
        return KERNELRESULT(InvalidHandle);
    if (!(o->mode & FsOpenMode_Write))
        return FSRESULT(WriteNotPermitted);
    if (sz < 0)
        return FSRESULT(InvalidSize);
    int err = shim_fs_truncate(o->fd, sz);
    return err ? shim_fs_errno(err) : 0;
}

SHIM_API Result fsFileGetSize(FsFile *f, s64 *out)
{
    ShimFsFile *o = shim_fs_file(f);
    if (!o)
        return KERNELRESULT(InvalidHandle);
    if (!out)
        return FSRESULT(NullptrArgument);
    s64 size = shim_fs_fd_size(o->fd);
    if (size < 0)
        return shim_fs_errno(errno);
    *out = size;
    return 0;
}

SHIM_API void fsFileClose(FsFile *f)
{
    shim_fs_lock();
    ShimFsFile *o = shim_fs_file(f);
    if (o) {
        shim_sys_close(o->fd);
        o->used = false;
    }
    shim_fs_unlock();
    if (f)
        memset(f, 0, sizeof *f);
}

#define SHIM_FS_DIR_MODES ((u32)(FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles))

SHIM_API Result fsFsOpenDirectory(FsFileSystem *fs, const char *path, u32 mode, FsDir *out)
{
    char host[SHIM_FS_HOST_MAX];
    Result rc = shim_fs_host(fs, path, host, NULL);
    if (R_FAILED(rc))
        return rc;
    if (!out)
        return FSRESULT(NullptrArgument);
    if ((mode & ~(SHIM_FS_DIR_MODES | FsDirOpenMode_NoFileSize)) || !(mode & SHIM_FS_DIR_MODES))
        return FSRESULT(InvalidOpenMode);
    if (shim_fs_kind(host, true) != FsDirEntryType_Dir)
        return FSRESULT(PathNotFound);
    shim_fs_lock();
    for (u32 k = 0; k < SHIM_FS_MAX_DIRS; k++) {
        ShimFsDir *d = &g_shim_fs_dirs[k];
        if (d->used)
            continue;
        DIR *dir = opendir(host);
        if (!dir) {
            rc = shim_fs_errno(errno);
            shim_fs_unlock();
            return rc;
        }
        d->used = true;
        d->dir = dir;
        d->mode = mode;
        memcpy(d->host, host, strlen(host) + 1);
        shim_fs_unlock();
        *out = (FsDir){ { SHIM_FS_HANDLE_DIR + k, 1, 0, 0 } };
        return 0;
    }
    shim_fs_unlock();
    return FSRESULT(OpenCountLimit);
}

/* Tipo e tamanho de uma entrada de diretorio; false = pular (sumiu, tipo especial ou fora do modo). */
static bool shim_fs_dir_entry(const ShimFsDir *d, const char *name, FsDirectoryEntry *out)
{
    size_t len = strlen(name);
    if (!strcmp(name, ".") || !strcmp(name, "..") || len >= FS_MAX_PATH)
        return false;
    char p[SHIM_FS_HOST_MAX + FS_MAX_PATH];
    snprintf(p, sizeof p, "%s/%s", d->host, name);
    ShimStat st;
    if (shim_stat(p, &st))
        return false;
    int kind = S_ISDIR(st.st_mode) ? FsDirEntryType_Dir : S_ISREG(st.st_mode) ? FsDirEntryType_File : SHIM_FS_KIND_OTHER;
    if (!(kind == FsDirEntryType_Dir ? (d->mode & FsDirOpenMode_ReadDirs)
                                     : kind == FsDirEntryType_File && (d->mode & FsDirOpenMode_ReadFiles)))
        return false;
    if (out) {
        memset(out, 0, sizeof *out);
        memcpy(out->name, name, len + 1);
        out->type = (s8)kind;
        out->file_size = kind == FsDirEntryType_File && !(d->mode & FsDirOpenMode_NoFileSize) ? (s64)st.st_size : 0;
    }
    return true;
}

/* Continua de onde a leitura anterior parou; *total_entries = entradas escritas em buf. */
SHIM_API Result fsDirRead(FsDir *d, s64 *total_entries, size_t max_entries, FsDirectoryEntry *buf)
{
    ShimFsDir *o = shim_fs_dir(d);
    if (!o)
        return KERNELRESULT(InvalidHandle);
    if (!total_entries || (!buf && max_entries))
        return FSRESULT(NullptrArgument);
    size_t n = 0;
    struct dirent *e;
    while (n < max_entries && (e = readdir(o->dir)) != NULL)
        if (shim_fs_dir_entry(o, e->d_name, &buf[n]))
            n++;
    *total_entries = (s64)n;
    return 0;
}

/* Total de entradas do diretorio (no modo de abertura), independente da posicao de leitura. */
SHIM_API Result fsDirGetEntryCount(FsDir *d, s64 *count)
{
    ShimFsDir *o = shim_fs_dir(d);
    if (!o)
        return KERNELRESULT(InvalidHandle);
    if (!count)
        return FSRESULT(NullptrArgument);
    DIR *dir = opendir(o->host);
    if (!dir)
        return shim_fs_errno(errno);
    s64 n = 0;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL)
        n += shim_fs_dir_entry(o, e->d_name, NULL);
    closedir(dir);
    *count = n;
    return 0;
}

SHIM_API void fsDirClose(FsDir *d)
{
    shim_fs_lock();
    ShimFsDir *o = shim_fs_dir(d);
    if (o) {
        closedir(o->dir);
        o->used = false;
    }
    shim_fs_unlock();
    if (d)
        memset(d, 0, sizeof *d);
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
/* ------------------------------------------------------------------ threads e sincronizacao (Linux/Bionic) */
/*
 * Objetos do kernel (threads) ficam numa tabela de handles. Cada thread do Horizon e uma pthread; a
 * pilha passada pelo guest (stack_top / stack_mem) nao e usada: a pthread tem a pilha do Bionic.
 * A tag de mutex da libnx (handle da thread) vem de threadGetCurHandle().
 */
#include <pthread.h>
#include <sched.h>

typedef void (*ThreadFunc)(void *);
typedef u32 Mutex;                                   /* libnx: _LOCK_T do newlib */
typedef struct { Mutex lock; u32 counter; } RMutex;  /* libnx: struct __lock_t do newlib */
typedef u32 CondVar;

typedef struct Thread {
    Handle handle;
    bool   owns_stack_mem;
    void  *stack_mem;
    void  *stack_mirror;
    size_t stack_sz;
    void **tls_array;
    struct Thread *next;
    struct Thread **prev_next;
} Thread;

enum {
    KernelError_OutOfHandles = 105,
    KernelError_InvalidPriority = 112,
    KernelError_InvalidCoreId = 113,
    KernelError_TimedOut = 117,
};

#define SHIM_MAX_HANDLES 64
#define SHIM_HANDLE_BASE 0x200U   /* handles nao nulos e abaixo de HANDLE_WAIT_MASK */

typedef struct {
    int used, is_thread, started, signaled, closed, joined;
    pthread_t th;
    ThreadFunc entry;
    void *arg;
} ShimObj;

static ShimObj g_shim_objs[SHIM_MAX_HANDLES];
static pthread_mutex_t g_shim_obj_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_shim_obj_cond = PTHREAD_COND_INITIALIZER;
static __thread Handle t_shim_handle;

static ShimObj *shim_obj(Handle h)
{
    u32 k = h - SHIM_HANDLE_BASE;
    return (h >= SHIM_HANDLE_BASE && k < SHIM_MAX_HANDLES && g_shim_objs[k].used) ? &g_shim_objs[k] : NULL;
}

static Handle shim_alloc_obj(int is_thread)   /* chamar com g_shim_obj_lock */
{
    for (u32 k = 0; k < SHIM_MAX_HANDLES; k++)
        if (!g_shim_objs[k].used) {
            memset(&g_shim_objs[k], 0, sizeof g_shim_objs[k]);
            g_shim_objs[k].used = 1;
            g_shim_objs[k].is_thread = is_thread;
            return SHIM_HANDLE_BASE + k;
        }
    return INVALID_HANDLE;
}

SHIM_API Handle threadGetCurHandle(void)
{
    if (t_shim_handle == INVALID_HANDLE) {         /* thread principal: handle criado na primeira consulta */
        pthread_mutex_lock(&g_shim_obj_lock);
        t_shim_handle = shim_alloc_obj(1);
        ShimObj *o = shim_obj(t_shim_handle);
        if (o) {
            o->started = 1;
            o->th = pthread_self();
        }
        pthread_mutex_unlock(&g_shim_obj_lock);
    }
    return t_shim_handle;
}

static void shim_thread_finished(Handle h)
{
    pthread_mutex_lock(&g_shim_obj_lock);
    ShimObj *o = shim_obj(h);
    if (o) {
        o->signaled = 1;
        if (o->closed)
            o->used = 0;           /* handle ja fechado: o objeto morre com a thread */
    }
    pthread_cond_broadcast(&g_shim_obj_cond);
    pthread_mutex_unlock(&g_shim_obj_lock);
}

static void *shim_thread_trampoline(void *p)
{
    Handle h = (Handle)(uintptr_t)p;
    pthread_mutex_lock(&g_shim_obj_lock);
    ShimObj *o = shim_obj(h);
    ThreadFunc entry = o->entry;
    void *arg = o->arg;
    pthread_mutex_unlock(&g_shim_obj_lock);
    t_shim_handle = h;
    entry(arg);
    shim_thread_finished(h);       /* retornar da entrada equivale a svcExitThread na libnx */
    return NULL;
}

/* svc 0x08: cria a thread parada (prio 0..63; cpuid -2 = padrao, 0..3 = nucleo do Switch). */
SHIM_API Result svcCreateThread(Handle *out, void *entry, void *arg, void *stack_top, int prio, int cpuid)
{
    (void)stack_top;
    if (prio < 0 || prio > 63)
        return KERNELRESULT(InvalidPriority);
    if (cpuid != -2 && (cpuid < 0 || cpuid > 3))
        return KERNELRESULT(InvalidCoreId);
    pthread_mutex_lock(&g_shim_obj_lock);
    Handle h = shim_alloc_obj(1);
    ShimObj *o = shim_obj(h);
    if (o) {
        o->entry = (ThreadFunc)entry;
        o->arg = arg;
    }
    pthread_mutex_unlock(&g_shim_obj_lock);
    if (h == INVALID_HANDLE)
        return KERNELRESULT(OutOfHandles);
    *out = h;
    return 0;
}

/* svc 0x09: pthread_create. */
SHIM_API Result svcStartThread(Handle handle)
{
    pthread_mutex_lock(&g_shim_obj_lock);
    ShimObj *o = shim_obj(handle);
    Result rc = 0;
    if (!o || !o->is_thread)
        rc = KERNELRESULT(InvalidHandle);
    else if (o->started)
        rc = KERNELRESULT(InvalidState);
    else if (pthread_create(&o->th, NULL, shim_thread_trampoline, (void *)(uintptr_t)handle) != 0)
        rc = KERNELRESULT(OutOfMemory);
    else
        o->started = 1;
    pthread_mutex_unlock(&g_shim_obj_lock);
    return rc;
}

/* svc 0x0A: termina a thread atual (sinaliza o handle para quem espera). */
__attribute__((noreturn)) SHIM_API void svcExitThread(void)
{
    shim_thread_finished(threadGetCurHandle());
    pthread_exit(NULL);
}

/* svc 0x0B: ns > 0 dorme; 0, -1 e -2 sao os modos de yield do Horizon -> sched_yield. */
SHIM_API void svcSleepThread(s64 nano)
{
    if (nano <= 0) {
        sched_yield();
        return;
    }
    struct timespec ts = { (time_t)(nano / 1000000000LL), (long)(nano % 1000000000LL) };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
        ;
}

/* svc 0x16: fecha o handle; thread ainda rodando continua e o objeto e liberado quando ela termina. */
SHIM_API Result svcCloseHandle(Handle handle)
{
    pthread_mutex_lock(&g_shim_obj_lock);
    ShimObj *o = shim_obj(handle);
    Result rc = 0;
    if (!o) {
        rc = KERNELRESULT(InvalidHandle);
    } else if (o->is_thread && o->started && !pthread_equal(o->th, pthread_self())) {
        if (o->signaled) {
            pthread_t th = o->th;
            o->used = 0;
            pthread_mutex_unlock(&g_shim_obj_lock);
            pthread_join(th, NULL);
            return 0;
        }
        pthread_detach(o->th);
        o->closed = 1;
    } else {
        o->used = 0;
    }
    pthread_mutex_unlock(&g_shim_obj_lock);
    return rc;
}

/* svc 0x18: espera ate algum handle ficar sinalizado (thread terminada). timeout em ns, UINT64_MAX = sem fim. */
SHIM_API Result svcWaitSynchronization(s32 *index, const Handle *handles, s32 handleCount, u64 timeout)
{
    if (handleCount < 0 || handleCount > 64)
        return KERNELRESULT(OutOfRange);
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    if (timeout != UINT64_MAX) {
        u64 ns = (u64)deadline.tv_nsec + timeout % 1000000000ULL;
        deadline.tv_sec += (time_t)(timeout / 1000000000ULL + ns / 1000000000ULL);
        deadline.tv_nsec = (long)(ns % 1000000000ULL);
    }
    pthread_mutex_lock(&g_shim_obj_lock);
    for (s32 k = 0; k < handleCount; k++)
        if (!shim_obj(handles[k])) {
            pthread_mutex_unlock(&g_shim_obj_lock);
            return KERNELRESULT(InvalidHandle);
        }
    for (;;) {
        for (s32 k = 0; k < handleCount; k++)
            if (shim_obj(handles[k])->signaled) {
                *index = k;
                pthread_mutex_unlock(&g_shim_obj_lock);
                return 0;
            }
        int err = timeout == UINT64_MAX ? pthread_cond_wait(&g_shim_obj_cond, &g_shim_obj_lock)
                                        : pthread_cond_timedwait(&g_shim_obj_cond, &g_shim_obj_lock, &deadline);
        if (err == ETIMEDOUT) {
            pthread_mutex_unlock(&g_shim_obj_lock);
            return KERNELRESULT(TimedOut);
        }
    }
}

/* ---- mutex da libnx (mesmo algoritmo: tag = handle da thread, bit HANDLE_WAIT_MASK = ha esperadores) */
SHIM_API void mutexInit(Mutex *m) { *m = INVALID_HANDLE; }

SHIM_API void mutexLock(Mutex *m)
{
    u32 self = threadGetCurHandle();
    u32 value = __atomic_load_n(m, __ATOMIC_SEQ_CST);
    for (;;) {
        if (value == INVALID_HANDLE) {
            if (__atomic_compare_exchange_n(m, &value, self, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
                return;
            continue;
        }
        if ((value & HANDLE_WAIT_MASK) == 0 &&
            !__atomic_compare_exchange_n(m, &value, value | HANDLE_WAIT_MASK, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            continue;
        svcArbitrateLock(value & ~HANDLE_WAIT_MASK, m, self);
        value = __atomic_load_n(m, __ATOMIC_SEQ_CST);
        if ((value & ~HANDLE_WAIT_MASK) == self)
            return;
    }
}

SHIM_API bool mutexTryLock(Mutex *m)
{
    u32 expected = INVALID_HANDLE;
    return __atomic_compare_exchange_n(m, &expected, threadGetCurHandle(), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

SHIM_API void mutexUnlock(Mutex *m)
{
    u32 self = threadGetCurHandle();
    u32 value = __atomic_load_n(m, __ATOMIC_SEQ_CST);
    for (;;) {
        if (value != self) {               /* com WAIT_MASK: o "kernel" acorda o proximo */
            svcArbitrateUnlock(m);
            return;
        }
        if (__atomic_compare_exchange_n(m, &value, INVALID_HANDLE, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            return;
    }
}

/* ---- mutex recursivo (newlib __lock_t: lock + contador) */
SHIM_API void rmutexInit(RMutex *m) { m->lock = INVALID_HANDLE; m->counter = 0; }

SHIM_API void rmutexLock(RMutex *m)
{
    if ((__atomic_load_n(&m->lock, __ATOMIC_SEQ_CST) & ~HANDLE_WAIT_MASK) != threadGetCurHandle()) {
        mutexLock(&m->lock);
        m->counter = 0;
    }
    m->counter++;
}

SHIM_API bool rmutexTryLock(RMutex *m)
{
    if ((__atomic_load_n(&m->lock, __ATOMIC_SEQ_CST) & ~HANDLE_WAIT_MASK) != threadGetCurHandle()) {
        if (!mutexTryLock(&m->lock))
            return false;
        m->counter = 0;
    }
    m->counter++;
    return true;
}

SHIM_API void rmutexUnlock(RMutex *m)
{
    if (--m->counter == 0)
        mutexUnlock(&m->lock);
}

/* svc 0x1C: solta o mutex em tag_location, espera um sinal em key e, se acordar por sinal, retoma o mutex.
 * Com timeout devolve TimedOut sem retomar (a libnx faz o mutexLock depois, como no kernel). */
SHIM_API Result svcWaitProcessWideKeyAtomic(u32 *key, u32 *tag_location, u32 self_tag, u64 timeout)
{
    if ((uintptr_t)key % 4 || (uintptr_t)tag_location % 4)
        return KERNELRESULT(InvalidAddress);
    u32 seq = __atomic_load_n(key, __ATOMIC_SEQ_CST);
    u32 value = __atomic_load_n(tag_location, __ATOMIC_SEQ_CST);
    if ((value & ~HANDLE_WAIT_MASK) != self_tag)
        return KERNELRESULT(InvalidState);
    mutexUnlock(tag_location);
    struct timespec rel, *prel = NULL;
    if (timeout != UINT64_MAX) {
        rel.tv_sec = (time_t)(timeout / 1000000000ULL);
        rel.tv_nsec = (long)(timeout % 1000000000ULL);
        prel = &rel;
    }
    long r = syscall(SYS_futex, key, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, seq, prel, NULL, 0);
    if (r != 0 && errno == ETIMEDOUT)
        return KERNELRESULT(TimedOut);
    mutexLock(tag_location);
    return 0;
}

/* svc 0x1D: acorda ate num esperadores de key (num <= 0: todos). */
SHIM_API void svcSignalProcessWideKey(u32 *key, s32 num)
{
    __atomic_add_fetch(key, 1, __ATOMIC_SEQ_CST);
    syscall(SYS_futex, key, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, num <= 0 ? INT_MAX : num, NULL, NULL, 0);
}

/* ---- variavel de condicao da libnx */
SHIM_API void condvarInit(CondVar *c) { *c = 0; }

SHIM_API Result condvarWaitTimeout(CondVar *c, Mutex *m, u64 timeout)
{
    Result rc = svcWaitProcessWideKeyAtomic(c, m, threadGetCurHandle(), timeout);
    if (rc == KERNELRESULT(TimedOut))
        mutexLock(m);
    return rc;
}

SHIM_API Result condvarWait(CondVar *c, Mutex *m) { return condvarWaitTimeout(c, m, UINT64_MAX); }
SHIM_API Result condvarWake(CondVar *c, int num) { svcSignalProcessWideKey(c, num); return 0; }
SHIM_API Result condvarWakeOne(CondVar *c) { return condvarWake(c, 1); }
SHIM_API Result condvarWakeAll(CondVar *c) { return condvarWake(c, -1); }

/* ---- API de threads da libnx */
SHIM_API Result threadCreate(Thread *t, ThreadFunc entry, void *arg, void *stack_mem, size_t stack_sz,
                             int prio, int cpuid)
{
    Handle h;
    Result rc = svcCreateThread(&h, (void *)entry, arg, NULL, prio, cpuid);
    if (R_FAILED(rc))
        return rc;
    memset(t, 0, sizeof *t);
    t->handle = h;
    t->owns_stack_mem = stack_mem == NULL;
    t->stack_mem = stack_mem;
    t->stack_sz = stack_sz;
    return 0;
}

SHIM_API Result threadStart(Thread *t) { return svcStartThread(t->handle); }

SHIM_API Result threadWaitForExit(Thread *t)
{
    s32 idx;
    return svcWaitSynchronization(&idx, &t->handle, 1, UINT64_MAX);
}

SHIM_API Result threadClose(Thread *t)
{
    Result rc = svcCloseHandle(t->handle);
    t->handle = INVALID_HANDLE;
    return rc;
}

/* ------------------------------------------------------------------ IPC (dummies do mapeamento) */
/* svc 0x1F: sessao ficticia na tabela de handles (o nome da porta tem ate 11 caracteres no Horizon). */
SHIM_API Result svcConnectToNamedPort(Handle *session, const char *name)
{
    if (!name || strnlen(name, 12) > 11)
        return KERNELRESULT(OutOfRange);
    pthread_mutex_lock(&g_shim_obj_lock);
    Handle h = shim_alloc_obj(0);
    pthread_mutex_unlock(&g_shim_obj_lock);
    if (h == INVALID_HANDLE)
        return KERNELRESULT(OutOfHandles);
    *session = h;
    return 0;
}

/* svc 0x21: valida a sessao e devolve sucesso; a mensagem no buffer de IPC da TLS nao e processada. */
SHIM_API Result svcSendSyncRequest(Handle session)
{
    pthread_mutex_lock(&g_shim_obj_lock);
    ShimObj *o = shim_obj(session);
    int ok = o && !o->is_thread;
    pthread_mutex_unlock(&g_shim_obj_lock);
    return ok ? 0 : KERNELRESULT(InvalidHandle);
}

/* svc 0x13 / 0x14: memoria compartilhada de um servico (ex.: hid). Dummy: valida alinhamento e nao mapeia. */
SHIM_API Result svcMapSharedMemory(Handle handle, void *addr, size_t size, u32 perm)
{
    (void)handle; (void)perm;
    if ((uintptr_t)addr % SHIM_PAGE)
        return KERNELRESULT(InvalidAddress);
    if (size == 0 || size % SHIM_PAGE)
        return KERNELRESULT(InvalidSize);
    return 0;
}

SHIM_API Result svcUnmapSharedMemory(Handle handle, void *addr, size_t size)
{
    (void)handle; (void)addr; (void)size;
    return 0;
}

/* svc 0x17: limpa o sinal de um evento legivel (eventos de applet). Dummy. */
SHIM_API Result svcResetSignal(Handle handle)
{
    (void)handle;
    return 0;
}

/* svc 0x52: desfaz o mapeamento de memoria de transferencia (applet). Dummy. */
SHIM_API Result svcUnmapTransferMemory(Handle tmem_handle, void *addr, size_t size)
{
    (void)tmem_handle; (void)addr; (void)size;
    return 0;
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
