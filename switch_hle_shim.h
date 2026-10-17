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
#include <pthread.h>

/* ------------------------------------------------------------------ tipos base */
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int32_t  s32;
typedef int64_t  s64;
typedef u32      Result;

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
__attribute__((constructor)) static void shim_init_guest_bss(void) {
    VirtualAlloc((void *)0x100000, 0x100000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}
#elif defined(__linux__) || defined(__ANDROID__)
#include <sys/mman.h>
__attribute__((constructor)) static void shim_init_guest_bss(void) {
    mmap((void *)0x100000, 0x100000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
}
#endif

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
enum { LibnxError_OutOfMemory = 2, LibnxError_IoError = 10, LibnxError_BadInput = 11 };
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
 * Sem roteiro customizado vale o padrao deterministico do benchmark (A no quadro 30, Plus no 60), a menos
 * que a variavel de ambiente OBDC_INPUT=none desligue o roteiro padrao.
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

/* OBDC_INPUT=none: sem roteiro padrao (para rodar binarios sem mexer no codigo, ex.: o C reconstruido). */
static bool shim_input_env_none(void)
{
    static int none = -1;
    if (none < 0) {
        const char *v = getenv("OBDC_INPUT");
        none = v && strcmp(v, "none") == 0;
    }
    return none != 0;
}

static ShimInputState shim_input_at(u32 frame)
{
    const ShimInputEvent *ev = g_shim_input_custom ? g_shim_input_script : g_shim_input_default;
    u32 n = g_shim_input_custom ? g_shim_input_count
          : shim_input_env_none() ? 0 : (u32)(sizeof g_shim_input_default / sizeof g_shim_input_default[0]);
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
/*
 * Ciclo de vida do applet. Na libnx o appletMainLoop processa as mensagens do sistema (foco, modo de
 * operacao, pedido de saida...), atualiza o estado e chama os hooks (appletHook) antes de devolver true ou
 * false. Aqui as mensagens vem de uma fila alimentada por shimAppletPost* (simulam o sistema) e sao
 * entregues no proximo appletMainLoop, na mesma ordem dos hooks da libnx (o ultimo registrado primeiro).
 * Pedido de saida: o appletMainLoop passa a devolver false para sempre (termino limpo do laco). Na libnx
 * isso so acontece com appletLockExit; sem a trava o sistema mata o processo sem aviso, o shim sempre
 * deixa o laco terminar. Fora de foco o shim nao suspende o appletMainLoop (so atualiza o estado e chama
 * os hooks), para nao travar oraculos sem ninguem para devolver o foco.
 * Sem mensagens, o limite deterministico de SHIM_FRAMES quadros continua valendo e chega ao programa como
 * um pedido de saida (hooks OnExitRequest + false).
 */
enum { LibnxError_AlreadyInitialized = 7, LibnxError_NotInitialized = 8 };

typedef enum {
    AppletFocusState_InFocus    = 1,
    AppletFocusState_OutOfFocus = 2,
    AppletFocusState_Background = 3,
} AppletFocusState;

typedef enum { AppletOperationMode_Handheld = 0, AppletOperationMode_Console = 1 } AppletOperationMode;

typedef enum {
    AppletFocusHandlingMode_SuspendHomeSleep = 0,
    AppletFocusHandlingMode_NoSuspend,
    AppletFocusHandlingMode_SuspendHomeSleepNotify,
    AppletFocusHandlingMode_AlwaysSuspend,
    AppletFocusHandlingMode_Max,
} AppletFocusHandlingMode;

typedef enum {
    AppletHookType_OnFocusState = 0,
    AppletHookType_OnOperationMode,
    AppletHookType_OnPerformanceMode,
    AppletHookType_OnExitRequest,
    AppletHookType_OnResume,
    AppletHookType_OnCaptureButtonShortPressed,
    AppletHookType_OnAlbumScreenShotTaken,
    AppletHookType_RequestToDisplay,
    AppletHookType_Max,
} AppletHookType;

typedef void (*AppletHookFn)(AppletHookType hook, void *param);
typedef struct AppletHookCookie AppletHookCookie;
struct AppletHookCookie {
    AppletHookCookie *next;
    AppletHookFn callback;
    void *param;
};

#define SHIM_APPLET_QUEUE 16

typedef struct { AppletHookType type; u32 value; } ShimAppletMsg;

static AppletHookCookie g_shim_hook_root;          /* lista: g_shim_hook_root.next = mais recente */
static ShimAppletMsg g_shim_applet_queue[SHIM_APPLET_QUEUE];
static u32 g_shim_applet_head, g_shim_applet_count;
static AppletFocusState g_shim_focus = AppletFocusState_InFocus;
static AppletOperationMode g_shim_opmode = AppletOperationMode_Handheld;
static AppletFocusHandlingMode g_shim_focus_mode = AppletFocusHandlingMode_SuspendHomeSleep;
static bool g_shim_exit_requested, g_shim_exit_locked, g_shim_auto_power_down;

SHIM_API void appletHook(AppletHookCookie *cookie, AppletHookFn callback, void *param)
{
    if (!callback)
        return;
    cookie->callback = callback;
    cookie->param = param;
    cookie->next = g_shim_hook_root.next;
    g_shim_hook_root.next = cookie;
}

SHIM_API void appletUnhook(AppletHookCookie *cookie)
{
    for (AppletHookCookie *c = &g_shim_hook_root; c->next; c = c->next)
        if (c->next == cookie) {
            c->next = cookie->next;
            break;
        }
    cookie->next = NULL;
}

static void shim_applet_call_hooks(AppletHookType type)
{
    for (AppletHookCookie *c = g_shim_hook_root.next; c; ) {
        AppletHookCookie *next = c->next;      /* o hook pode se desregistrar */
        if (c->callback)
            c->callback(type, c->param);
        c = next;
    }
}

/* Enfileira uma mensagem do "sistema"; false se a fila estiver cheia. */
static bool shim_applet_post(AppletHookType type, u32 value)
{
    if (g_shim_applet_count >= SHIM_APPLET_QUEUE)
        return false;
    g_shim_applet_queue[(g_shim_applet_head + g_shim_applet_count++) % SHIM_APPLET_QUEUE] = (ShimAppletMsg){ type, value };
    return true;
}

SHIM_API bool shimAppletPostFocusState(AppletFocusState state)  { return shim_applet_post(AppletHookType_OnFocusState, (u32)state); }
SHIM_API bool shimAppletPostOperationMode(AppletOperationMode mode) { return shim_applet_post(AppletHookType_OnOperationMode, (u32)mode); }
SHIM_API bool shimAppletRequestExit(void)                      { return shim_applet_post(AppletHookType_OnExitRequest, 0); }
SHIM_API void shimAppletSetAutoPowerDownRequested(bool requested) { g_shim_auto_power_down = requested; }

/* Volta ao estado inicial (foco, modo, fila, pedido de saida, quadro 0). Os hooks continuam registrados. */
SHIM_API void shimAppletReset(void)
{
    g_shim_applet_head = g_shim_applet_count = 0;
    g_shim_focus = AppletFocusState_InFocus;
    g_shim_opmode = AppletOperationMode_Handheld;
    g_shim_focus_mode = AppletFocusHandlingMode_SuspendHomeSleep;
    g_shim_exit_requested = g_shim_exit_locked = g_shim_auto_power_down = false;
    g_shim_frame = 0;
}

static void shim_applet_process(void)
{
    while (g_shim_applet_count) {
        ShimAppletMsg m = g_shim_applet_queue[g_shim_applet_head];
        g_shim_applet_head = (g_shim_applet_head + 1) % SHIM_APPLET_QUEUE;
        g_shim_applet_count--;
        if (m.type == AppletHookType_OnFocusState) {
            if (g_shim_focus == (AppletFocusState)m.value)
                continue;
            g_shim_focus = (AppletFocusState)m.value;
        } else if (m.type == AppletHookType_OnOperationMode) {
            if (g_shim_opmode == (AppletOperationMode)m.value)
                continue;
            g_shim_opmode = (AppletOperationMode)m.value;
        } else if (m.type == AppletHookType_OnExitRequest) {
            g_shim_exit_requested = true;
        }
        shim_applet_call_hooks(m.type);
    }
}

/* O fim da execucao deterministica (SHIM_FRAMES quadros) e entregue como pedido de saida do sistema:
 * hooks OnExitRequest uma vez e false, como no Switch quando o usuario fecha o programa (com appletLockExit). */
SHIM_API bool appletMainLoop(void)
{
    shim_applet_process();
    if (g_shim_exit_requested)
        return false;
    if (g_shim_frame >= SHIM_FRAMES) {
        g_shim_exit_requested = true;
        shim_applet_call_hooks(AppletHookType_OnExitRequest);
        return false;
    }
    g_shim_frame++;
    return true;
}

SHIM_API AppletFocusState appletGetFocusState(void)         { return g_shim_focus; }
SHIM_API AppletOperationMode appletGetOperationMode(void)   { return g_shim_opmode; }
SHIM_API Result appletLockExit(void)                        { g_shim_exit_locked = true; return 0; }
SHIM_API Result appletUnlockExit(void)                      { g_shim_exit_locked = false; return 0; }
SHIM_API bool shimAppletIsExitLocked(void)                  { return g_shim_exit_locked; }   /* libnx: destravar antes de main() voltar */
SHIM_API AppletFocusHandlingMode shimAppletGetFocusHandlingMode(void) { return g_shim_focus_mode; }

SHIM_API Result appletSetFocusHandlingMode(AppletFocusHandlingMode mode)
{
    if ((u32)mode >= AppletFocusHandlingMode_Max)
        return LIBNXRESULT(BadInput);
    g_shim_focus_mode = mode;
    return 0;
}

/* Na libnx so funciona para SystemApplet (7.0.0+) ou LibraryApplet/OverlayApplet (15.0.0+); um homebrew
 * como Application recebe erro no Switch. Aqui devolve o valor de shimAppletSetAutoPowerDownRequested. */
SHIM_API Result appletIsAutoPowerDownRequested(bool *out)
{
    if (!out)
        return LIBNXRESULT(BadInput);
    *out = g_shim_auto_power_down;
    return 0;
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
    KernelError_OutOfHandles = 105,
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

enum { MemType_Unmapped = 0x00, MemType_Normal = 0x02, MemType_Heap = 0x05, MemType_MappedMemory = 0x0B,
       MemType_TransferMemIsolated = 0x0D, MemType_TransferMem = 0x0E };
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

/* ------------------------------------------------------------------ threads e sincronizacao (tipos) */
typedef void (*ThreadFunc)(void *);
typedef u32 Mutex;
typedef struct { Mutex lock; u32 counter; } RMutex;
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

/* ------------------------------------------------------------------ IPC (CMIF / HIPC) */
#define CMIF_IN_HEADER_MAGIC   0x49434653U  /* "SFCI" */
#define CMIF_OUT_HEADER_MAGIC  0x4F434653U  /* "SFCO" */
#define SFCI_MAGIC             CMIF_IN_HEADER_MAGIC
#define SFCO_MAGIC             CMIF_OUT_HEADER_MAGIC

#ifndef SHIM_TLS_IPC_SIZE
#define SHIM_TLS_IPC_SIZE      0x100
#endif
#ifndef SHIM_MAX_HANDLES
#define SHIM_MAX_HANDLES       64
#endif
#ifndef SHIM_HANDLE_BASE
#define SHIM_HANDLE_BASE       0x200U
#endif

/*
 * CMIF (Command/Message Interface Format) do Horizon OS:
 * Protocolo de mensagens sobre o buffer IPC localizado no inicio da TLS da thread (0x000-0x0FF).
 */
typedef struct {
    union {
        u32 magic;
        u32 Magic;
    };
    union {
        u32 version;
        u32 Version;
    };
    union {
        u32 command_id;
        u32 CommandId;
        u32 cmd_id;
    };
    union {
        u32 token;
        u32 Token;
    };
    union {
        u32 type;
        u32 Type;
    };
    union {
        u32 raw_size;
        u32 RawSize;
    };
    union {
        u32 objects;
        u32 Objects;
    };
} CmifInHeader;

typedef struct {
    union {
        u32 magic;
        u32 Magic;
    };
    union {
        u32 version;
        u32 Version;
    };
    union {
        Result result;
        Result Result;
    };
    union {
        u32 token;
        u32 Token;
    };
    union {
        u32 type;
        u32 Type;
    };
    union {
        u32 raw_size;
        u32 RawSize;
    };
    union {
        u32 objects;
        u32 Objects;
    };
} CmifOutHeader;

SHIM_API void *shimGetTls(void);

/* Decodifica um cabecalho CMIF de entrada a partir do buffer IPC. Retorna 0 em sucesso, -1 se invalido ou sem SFCI. */
static inline int cmifParseInHeader(const void *ipc_raw, size_t buf_size, CmifInHeader *out_hdr, size_t *out_offset)
{
    if (!ipc_raw || buf_size < 16)
        return -1;
    const u32 *words = (const u32 *)ipc_raw;
    size_t max_words = buf_size / sizeof(u32);
    if (max_words > 16)
        max_words = 16;
    for (size_t i = 0; i < max_words; i++) {
        if (words[i] == CMIF_IN_HEADER_MAGIC) {
            if ((i + 4) * sizeof(u32) > buf_size)
                return -1;
            if (out_hdr) {
                memset(out_hdr, 0, sizeof(*out_hdr));
                out_hdr->magic = words[i];
                out_hdr->version = words[i + 1];
                out_hdr->command_id = words[i + 2];
                out_hdr->token = words[i + 3];
                if (i >= 2) {
                    out_hdr->type = words[0] & 0xFFFF;
                    out_hdr->raw_size = words[1] & 0x3FF;
                    if (words[1] & (1U << 31)) {
                        out_hdr->objects = (words[2] >> 8) & 0xF;
                    }
                } else {
                    out_hdr->type = 4;
                    out_hdr->raw_size = 4;
                    out_hdr->objects = 0;
                }
            }
            if (out_offset)
                *out_offset = i * sizeof(u32);
            return 0;
        }
    }
    return -1;
}

/* Formata um cabecalho CMIF de saida no buffer IPC. Retorna 0 em sucesso, -1 se erro. */
static inline int cmifFormatOutHeader(void *ipc_raw, size_t buf_size, size_t offset, const CmifOutHeader *out_hdr)
{
    if (!ipc_raw || !out_hdr || offset + 16 > buf_size)
        return -1;
    u32 *words = (u32 *)((u8 *)ipc_raw + offset);
    words[0] = out_hdr->magic ? out_hdr->magic : CMIF_OUT_HEADER_MAGIC;
    words[1] = out_hdr->version;
    words[2] = (u32)out_hdr->result;
    words[3] = out_hdr->token;
    return 0;
}

typedef Handle (*ShimIpcAllocFn)(const char *name);

static inline Result shim_cmif_dispatch(const char *srv_name, u32 cmd_id, const u8 *payload_in,
                                        u8 *payload_out, Handle *out_created_handle,
                                        ShimIpcAllocFn alloc_fn)
{
    Result res = 0;
    Handle created_handle = INVALID_HANDLE;

    if (strcmp(srv_name, "sm:") == 0) {
        if (cmd_id == 0) {
            /* sm: Initialize(pid_placeholder) -> sucesso */
            res = 0;
        } else if (cmd_id == 1) {
            /* sm: GetService(name: 8 bytes) -> aloca novo handle para o servico solicitado */
            char target[16] = {0};
            memcpy(target, payload_in, 8);
            target[8] = '\0';

            if (alloc_fn)
                created_handle = alloc_fn(target);
            res = (created_handle != INVALID_HANDLE) ? 0 : KERNELRESULT(OutOfHandles);
        } else {
            res = 0;
        }
    } else if (strncmp(srv_name, "time", 4) == 0) {
        if (cmd_id == 0) {
            u64 now_sec = (u64)time(NULL);
            memcpy(payload_out, &now_sec, sizeof(now_sec));
        }
        res = 0;
    } else if (strncmp(srv_name, "set", 3) == 0) {
        if (cmd_id == 0) {
            u64 lang_code = 0x000053552d6e65ULL;
            memcpy(payload_out, &lang_code, sizeof(lang_code));
        }
        res = 0;
    } else if (strncmp(srv_name, "applet", 6) == 0) {
        res = 0;
    } else {
        res = 0;
    }

    if (out_created_handle)
        *out_created_handle = created_handle;
    return res;
}

#if !defined(__linux__)
typedef struct {
    int used;
    char service_name[16];
} ShimHostObj;

static ShimHostObj g_shim_host_objs[SHIM_MAX_HANDLES];
static pthread_mutex_t g_shim_host_lock = PTHREAD_MUTEX_INITIALIZER;

static Handle shim_ipc_alloc_session_host(const char *name)
{
    pthread_mutex_lock(&g_shim_host_lock);
    Handle h = INVALID_HANDLE;
    for (u32 k = 0; k < SHIM_MAX_HANDLES; k++) {
        if (!g_shim_host_objs[k].used) {
            memset(&g_shim_host_objs[k], 0, sizeof(g_shim_host_objs[k]));
            g_shim_host_objs[k].used = 1;
            if (name) {
                strncpy(g_shim_host_objs[k].service_name, name, sizeof(g_shim_host_objs[k].service_name) - 1);
                g_shim_host_objs[k].service_name[sizeof(g_shim_host_objs[k].service_name) - 1] = '\0';
            }
            h = SHIM_HANDLE_BASE + k;
            break;
        }
    }
    pthread_mutex_unlock(&g_shim_host_lock);
    return h;
}

SHIM_API Result svcConnectToNamedPort(Handle *session, const char *name)
{
    if (!name || strnlen(name, 12) > 11)
        return KERNELRESULT(OutOfRange);
    Handle h = shim_ipc_alloc_session_host(name);
    if (h == INVALID_HANDLE)
        return KERNELRESULT(OutOfHandles);
    *session = h;
    return 0;
}

SHIM_API Result svcCloseHandle(Handle handle)
{
    u32 k = handle - SHIM_HANDLE_BASE;
    pthread_mutex_lock(&g_shim_host_lock);
    if (handle < SHIM_HANDLE_BASE || k >= SHIM_MAX_HANDLES || !g_shim_host_objs[k].used) {
        pthread_mutex_unlock(&g_shim_host_lock);
        return KERNELRESULT(InvalidHandle);
    }
    g_shim_host_objs[k].used = 0;
    pthread_mutex_unlock(&g_shim_host_lock);
    return 0;
}

SHIM_API Result svcSendSyncRequest(Handle session)
{
    u32 k = session - SHIM_HANDLE_BASE;
    pthread_mutex_lock(&g_shim_host_lock);
    if (session < SHIM_HANDLE_BASE || k >= SHIM_MAX_HANDLES || !g_shim_host_objs[k].used) {
        pthread_mutex_unlock(&g_shim_host_lock);
        return KERNELRESULT(InvalidHandle);
    }
    char srv_name[16];
    strncpy(srv_name, g_shim_host_objs[k].service_name, sizeof(srv_name) - 1);
    srv_name[sizeof(srv_name) - 1] = '\0';
    pthread_mutex_unlock(&g_shim_host_lock);

    u8 *ipc_raw = (u8 *)shimGetTls();
    if (!ipc_raw)
        return KERNELRESULT(InvalidAddress);

    CmifInHeader in_hdr;
    size_t sfci_byte_off = 0;
    if (cmifParseInHeader(ipc_raw, SHIM_TLS_IPC_SIZE, &in_hdr, &sfci_byte_off) != 0) {
        return 0;
    }

    size_t sfci_word_idx = sfci_byte_off / sizeof(u32);
    u32 *ipc_words = (u32 *)ipc_raw;
    const u8 *payload_in = ipc_raw + sfci_byte_off + 16;
    u8 *payload_out = ipc_raw + sfci_byte_off + 16;
    Handle created_handle = INVALID_HANDLE;

    Result res = shim_cmif_dispatch(srv_name, in_hdr.command_id, payload_in, payload_out,
                                    &created_handle, shim_ipc_alloc_session_host);

    CmifOutHeader out_hdr;
    memset(&out_hdr, 0, sizeof(out_hdr));
    out_hdr.magic = CMIF_OUT_HEADER_MAGIC;
    out_hdr.version = 0;
    out_hdr.result = res;
    out_hdr.token = in_hdr.token;
    cmifFormatOutHeader(ipc_raw, SHIM_TLS_IPC_SIZE, sfci_byte_off, &out_hdr);

    if (created_handle != INVALID_HANDLE) {
        ipc_words[sfci_word_idx + 4] = created_handle;
    }

    if (sfci_word_idx > 0) {
        ipc_words[0] = 0;
        if (created_handle != INVALID_HANDLE) {
            ipc_words[1] = 4 | (1U << 31);
            ipc_words[2] = (1U << 8);
            ipc_words[3] = created_handle;
        } else {
            ipc_words[1] = 4;
        }
    }

    return 0;
}

SHIM_API Result threadCreate(Thread *t, ThreadFunc entry, void *arg, void *stack_mem, size_t stack_sz, int prio, int cpuid)
{
    (void)t; (void)entry; (void)arg; (void)stack_mem; (void)stack_sz; (void)prio; (void)cpuid;
    return 0;
}
SHIM_API Result threadStart(Thread *t) { (void)t; return 0; }
SHIM_API Result threadWaitForExit(Thread *t) { (void)t; return 0; }
SHIM_API Result threadClose(Thread *t) { (void)t; return 0; }

SHIM_API void mutexInit(Mutex *m) { if (m) *m = 0; }
SHIM_API void mutexLock(Mutex *m) { (void)m; }
SHIM_API bool mutexTryLock(Mutex *m) { (void)m; return true; }
SHIM_API void mutexUnlock(Mutex *m) { (void)m; }

SHIM_API void rmutexInit(RMutex *m) { if (m) { m->lock = 0; m->counter = 0; } }
SHIM_API void rmutexLock(RMutex *m) { (void)m; }
SHIM_API bool rmutexTryLock(RMutex *m) { (void)m; return true; }
SHIM_API void rmutexUnlock(RMutex *m) { (void)m; }

SHIM_API void svcSleepThread(s64 nano)
{
    if (nano <= 0) return;
#if defined(_WIN32)
    Sleep((DWORD)(nano / 1000000ULL));
#else
    struct timespec ts = { (time_t)(nano / 1000000000LL), (long)(nano % 1000000000LL) };
    nanosleep(&ts, NULL);
#endif
}

SHIM_API void svcSignalProcessWideKey(u32 *key, s32 num) { (void)key; (void)num; }
SHIM_API Result svcWaitProcessWideKeyAtomic(u32 *key, u32 *tag_location, u32 self_tag, u64 timeout)
{
    (void)key; (void)tag_location; (void)self_tag; (void)timeout;
    return 0;
}
SHIM_API void condvarInit(CondVar *c) { if (c) *c = 0; }
SHIM_API Result condvarWait(CondVar *c, Mutex *m) { (void)c; (void)m; return 0; }
SHIM_API Result condvarWaitTimeout(CondVar *c, Mutex *m, u64 timeout) { (void)c; (void)m; (void)timeout; return 0; }
SHIM_API Result condvarWake(CondVar *c, int num) { (void)c; (void)num; return 0; }
SHIM_API Result condvarWakeOne(CondVar *c) { (void)c; return 0; }
SHIM_API Result condvarWakeAll(CondVar *c) { (void)c; return 0; }
#endif

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

/* Raiz do host: valor do shim > variavel de ambiente > padrao da macro; sem barras no fim. 0 ou TooLongPath. */
static Result shim_fs_resolve_root(const char *setting, const char *env, const char *fallback, char *out)
{
    const char *root = setting[0] ? setting : getenv(env);
    if (!root || !*root)
        root = fallback;
    size_t n = strlen(root);
    while (n > 1 && (root[n - 1] == '/' || root[n - 1] == '\\'))
        n--;
    if (n >= SHIM_FS_ROOT_MAX)
        return FSRESULT(TooLongPath);
    memcpy(out, root, n);
    out[n] = '\0';
    return 0;
}

SHIM_API Result fsOpenSdCardFileSystem(FsFileSystem *out)
{
    char buf[SHIM_FS_ROOT_MAX];
    Result rc = shim_fs_resolve_root(g_shim_sdcard_root, "OBDC_SDCARD", OBDC_SDCARD_ROOT, buf);
    if (R_FAILED(rc))
        return rc;
    size_t n = strlen(buf);
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

/* ------------------------------------------------------------------ dispositivos: fsdev (sdmc:) e romfs: */
/*
 * Na libnx, caminhos "nome:/..." do stdio do newlib passam pelos devoptabs: fsdevMountSdmc monta "sdmc"
 * (o __appInit padrao ainda faz chdir("sdmc:/")) e romfsInit monta "romfs" (somente leitura, embutido no
 * NRO). Aqui cada dispositivo e uma raiz no host:
 *   fsdev (sdmc e fsdevMountDevice) -> raiz do FsFileSystem montado (a do cartao SD);
 *   romfs                           -> shimRomfsSetRoot(dir) > OBDC_ROMFS > OBDC_ROMFS_ROOT ("./romfs").
 * shimTranslatePath troca "nome:/caminho" pelo caminho do host com a mesma normalizacao do fs (".." nao
 * sai da raiz: EACCES). Caminho sem "nome:" segue direto para o host (no Windows, "C:" so vale como drive
 * se nao houver dispositivo "C"). No romfs, abrir para escrita, criar, apagar e renomear dao EROFS, como
 * no devoptab de romfs da libnx.
 *
 * Roteamento transparente do stdio: no fim deste header, fopen/opendir/stat/mkdir/rmdir/remove/unlink/
 * rename/access viram macros para os wrappers shim* (desligue com -DOBDC_NO_STDIO_REDIRECT). Inclua os
 * headers do sistema antes do shim. No Windows o shimFopen sempre abre em binario, como o newlib.
 * Diferenca: a libnx resolve caminhos relativos a partir de "sdmc:/" (chdir do __appInit); o shim os deixa
 * relativos ao diretorio atual do host.
 */
#ifndef OBDC_ROMFS_ROOT
#define OBDC_ROMFS_ROOT "./romfs"
#endif

#define SHIM_DEV_NAME_MAX 32      /* libnx: nome com ate 31 caracteres, sem ":" */
#define SHIM_MAX_DEVICES  8

enum { LibnxError_AlreadyMapped = 3, LibnxError_NotFound = 9 };

typedef struct {
    bool used;
    bool romfs;                   /* somente leitura, sem FsFileSystem */
    char name[SHIM_DEV_NAME_MAX];
    FsFileSystem fs;              /* fsdev: o dispositivo e dono do fs (fechado no unmount) */
    char root[SHIM_FS_ROOT_MAX];
} ShimDevice;

static ShimDevice g_shim_devices[SHIM_MAX_DEVICES];
static char g_shim_romfs_root[SHIM_FS_ROOT_MAX];     /* "" = OBDC_ROMFS ou OBDC_ROMFS_ROOT */

/* Chamar com o lock. name tem n caracteres (sem ":"). */
static ShimDevice *shim_dev_find(const char *name, size_t n)
{
    for (int k = 0; k < SHIM_MAX_DEVICES; k++) {
        ShimDevice *d = &g_shim_devices[k];
        if (d->used && strlen(d->name) == n && strncmp(d->name, name, n) == 0)
            return d;
    }
    return NULL;
}

/* Indice do novo dispositivo ou -1 (errno: EINVAL nome invalido, EEXIST ja montado, ENOMEM tabela cheia). */
static int shim_dev_add(const char *name, bool romfs, const FsFileSystem *fs, const char *root)
{
    size_t n = name ? strlen(name) : 0;
    if (n == 0 || n >= SHIM_DEV_NAME_MAX || strchr(name, ':') || strchr(name, '/')) {
        errno = EINVAL;
        return -1;
    }
    int slot = -1;
    shim_fs_lock();
    if (shim_dev_find(name, n)) {
        shim_fs_unlock();
        errno = EEXIST;
        return -1;
    }
    for (int k = 0; k < SHIM_MAX_DEVICES && slot < 0; k++)
        if (!g_shim_devices[k].used)
            slot = k;
    if (slot >= 0) {
        ShimDevice *d = &g_shim_devices[slot];
        memset(d, 0, sizeof *d);
        d->used = true;
        d->romfs = romfs;
        memcpy(d->name, name, n + 1);
        if (fs)
            d->fs = *fs;
        memcpy(d->root, root, strlen(root) + 1);
    }
    shim_fs_unlock();
    if (slot < 0)
        errno = ENOMEM;
    return slot;
}

/* Remove o dispositivo (romfs ou fsdev, conforme want_romfs); fecha o fs de um fsdev. 0 ou -1 (ENOENT). */
static int shim_dev_remove(const char *name, bool want_romfs)
{
    size_t n = name ? strlen(name) : 0;
    shim_fs_lock();
    ShimDevice *d = n ? shim_dev_find(name, n) : NULL;
    FsFileSystem fs = {0};
    bool close_fs = false;
    if (d && d->romfs == want_romfs) {
        fs = d->fs;
        close_fs = !d->romfs;
        d->used = false;
    } else {
        d = NULL;
    }
    shim_fs_unlock();
    if (!d) {
        errno = ENOENT;
        return -1;
    }
    if (close_fs)
        fsFsClose(&fs);
    return 0;
}

static int shim_fs_result_errno(Result rc)
{
    if (rc == FSRESULT(TooLongPath))           return ENAMETOOLONG;
    if (rc == FSRESULT(DirectoryUnobtainable)) return EACCES;
    if (rc == FSRESULT(NullptrArgument))       return EFAULT;
    return EINVAL;                                     /* InvalidCharacter, InvalidPathFormat */
}

/* "nome:resto" -> dispositivo + caminho do fs normalizado ("/a/b", raiz = ""). 1 = tem dispositivo,
 * 0 = caminho do host sem dispositivo, -1 = erro (errno). Copia o dispositivo para *out. */
static int shim_dev_split(const char *path, ShimDevice *out, char *norm)
{
    if (!path) {
        errno = EFAULT;
        return -1;
    }
    const char *colon = strchr(path, ':');
    const char *slash = strchr(path, '/');
    if (!colon || colon == path || (slash && slash < colon))
        return 0;
    size_t n = (size_t)(colon - path);
    shim_fs_lock();
    ShimDevice *d = shim_dev_find(path, n);
    if (d)
        *out = *d;
    shim_fs_unlock();
    if (!d) {
#if defined(_WIN32)
        if (n == 1)
            return 0;                                  /* "C:/..." = drive do Windows */
#endif
        errno = ENODEV;
        return -1;
    }
    char tmp[FS_MAX_PATH + 1];
    const char *rest = colon + 1;
    if (snprintf(tmp, sizeof tmp, "%s%s", rest[0] == '/' ? "" : "/", rest) >= (int)sizeof tmp) {
        errno = ENAMETOOLONG;
        return -1;
    }
    Result rc = shim_fs_normalize(tmp, norm);
    if (R_FAILED(rc)) {
        errno = shim_fs_result_errno(rc);
        return -1;
    }
    return 1;
}

/* Caminho do stdio ("sdmc:/x", "romfs:/x" ou do host) -> caminho do host. 0 ou -1 (errno). */
SHIM_API int shimTranslatePath(const char *path, char *host, size_t size, bool *read_only)
{
    ShimDevice d;
    char norm[FS_MAX_PATH];
    if (read_only)
        *read_only = false;
    if (!host || size == 0) {
        errno = EFAULT;
        return -1;
    }
    int r = shim_dev_split(path, &d, norm);
    if (r < 0)
        return -1;
    int w = r == 0 ? snprintf(host, size, "%s", path) : snprintf(host, size, "%s%s", d.root, norm);
    if (w < 0 || (size_t)w >= size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (read_only)
        *read_only = r == 1 && d.romfs;
    return 0;
}

/* ---- fsdev */
SHIM_API int fsdevMountDevice(const char *name, FsFileSystem fs)
{
    ShimFsMount *m = shim_fs_mount(&fs);
    int slot = m ? shim_dev_add(name, false, &fs, m->root) : -1;
    if (!m)
        errno = EINVAL;
    if (slot < 0)
        fsFsClose(&fs);                                /* como na libnx: fecha o fs tambem na falha */
    return slot;
}

SHIM_API int fsdevUnmountDevice(const char *name) { return shim_dev_remove(name, false); }

SHIM_API Result fsdevUnmountAll(void)
{
    for (int k = 0; k < SHIM_MAX_DEVICES; k++) {
        char name[SHIM_DEV_NAME_MAX] = "";
        shim_fs_lock();
        if (g_shim_devices[k].used && !g_shim_devices[k].romfs)
            memcpy(name, g_shim_devices[k].name, sizeof name);
        shim_fs_unlock();
        if (name[0])
            shim_dev_remove(name, false);
    }
    return 0;
}

SHIM_API Result fsdevMountSdmc(void)
{
    FsFileSystem sd;
    Result rc = fsOpenSdCardFileSystem(&sd);
    if (R_FAILED(rc))
        return rc;
    return fsdevMountDevice("sdmc", sd) < 0 ? LIBNXRESULT(AlreadyMapped) : 0;
}

/* Ponteiro para o FsFileSystem do dispositivo (valido ate o unmount); NULL se nao existe ou e romfs. */
SHIM_API FsFileSystem *fsdevGetDeviceFileSystem(const char *name)
{
    size_t n = name ? strlen(name) : 0;
    if (n && name[n - 1] == ':')
        n--;
    shim_fs_lock();
    ShimDevice *d = n ? shim_dev_find(name, n) : NULL;
    shim_fs_unlock();
    return d && !d->romfs ? &d->fs : NULL;
}

SHIM_API Result fsdevCommitDevice(const char *name)
{
    FsFileSystem *fs = fsdevGetDeviceFileSystem(name);
    return fs ? fsFsCommit(fs) : LIBNXRESULT(NotFound);
}

/* outpath (FS_MAX_PATH) recebe o caminho do fs; sem "nome:" vale o "sdmc" (o diretorio atual da libnx). */
SHIM_API int fsdevTranslatePath(const char *path, FsFileSystem **device, char *outpath)
{
    ShimDevice d;
    char norm[FS_MAX_PATH], tmp[FS_MAX_PATH + 8];
    int r = shim_dev_split(path, &d, norm);
    if (r < 0)
        return -1;
    if (r == 0) {
        if (snprintf(tmp, sizeof tmp, "sdmc:%s%s", path[0] == '/' ? "" : "/", path) >= (int)sizeof tmp) {
            errno = ENAMETOOLONG;
            return -1;
        }
        if (shim_dev_split(tmp, &d, norm) != 1)
            return -1;
    }
    if (d.romfs) {
        errno = ENODEV;
        return -1;
    }
    if (outpath)
        snprintf(outpath, FS_MAX_PATH, "%s", norm[0] ? norm : "/");
    if (device)
        *device = fsdevGetDeviceFileSystem(d.name);
    return 0;
}

/* ---- romfs */
/* Raiz do proximo romfsMountSelf; NULL volta a OBDC_ROMFS / OBDC_ROMFS_ROOT. */
SHIM_API Result shimRomfsSetRoot(const char *dir)
{
    if (!dir) {
        g_shim_romfs_root[0] = '\0';
        return 0;
    }
    size_t n = strlen(dir);
    if (n == 0)
        return FSRESULT(InvalidPathFormat);
    if (n >= SHIM_FS_ROOT_MAX)
        return FSRESULT(TooLongPath);
    memcpy(g_shim_romfs_root, dir, n + 1);
    return 0;
}

/* Num NRO monta a secao RomFS embutida; aqui monta o diretorio de assets do host (somente leitura).
 * Raiz inexistente -> LibnxError_NotFound; nome ja montado -> LibnxError_AlreadyMapped. */
SHIM_API Result romfsMountSelf(const char *name)
{
    char root[SHIM_FS_ROOT_MAX];
    Result rc = shim_fs_resolve_root(g_shim_romfs_root, "OBDC_ROMFS", OBDC_ROMFS_ROOT, root);
    if (R_FAILED(rc))
        return rc;
    if (shim_fs_kind(root, true) != FsDirEntryType_Dir)
        return LIBNXRESULT(NotFound);
    if (shim_dev_add(name, true, NULL, root) < 0)
        return errno == EEXIST ? LIBNXRESULT(AlreadyMapped) : LIBNXRESULT(BadInput);
    return 0;
}

SHIM_API Result romfsUnmount(const char *name) { return shim_dev_remove(name, true) ? LIBNXRESULT(NotFound) : 0; }
SHIM_API Result romfsInit(void) { return romfsMountSelf("romfs"); }
SHIM_API Result romfsExit(void) { return romfsUnmount("romfs"); }

/* ---- wrappers do stdio (alvo das macros do fim do header) */
static int shim_dev_host(const char *path, char *host, bool for_write)
{
    bool ro;
    if (shimTranslatePath(path, host, SHIM_FS_HOST_MAX, &ro))
        return -1;
    if (ro && for_write) {
        errno = EROFS;
        return -1;
    }
    return 0;
}

SHIM_API FILE *shimFopen(const char *path, const char *mode)
{
    char host[SHIM_FS_HOST_MAX], m[16];
    if (!mode || strlen(mode) + 2 > sizeof m) {
        errno = EINVAL;
        return NULL;
    }
    if (shim_dev_host(path, host, strpbrk(mode, "wa+") != NULL))
        return NULL;
    snprintf(m, sizeof m, "%s%s", mode, strchr(mode, 'b') ? "" : "b");   /* newlib nao traduz CRLF */
    return fopen(host, m);
}

SHIM_API DIR *shimOpendir(const char *path)
{
    char host[SHIM_FS_HOST_MAX];
    return shim_dev_host(path, host, false) ? NULL : opendir(host);
}

SHIM_API int shimStat(const char *path, struct stat *st)
{
    char host[SHIM_FS_HOST_MAX];
    return shim_dev_host(path, host, false) ? -1 : stat(host, st);
}

SHIM_API int shimMkdir(const char *path, int mode)
{
    char host[SHIM_FS_HOST_MAX];
    if (shim_dev_host(path, host, true))
        return -1;
#if defined(_WIN32)
    (void)mode;
    return _mkdir(host);
#else
    return mkdir(host, (mode_t)mode);
#endif
}

SHIM_API int shimRmdir(const char *path)
{
    char host[SHIM_FS_HOST_MAX];
    return shim_dev_host(path, host, true) ? -1 : shim_sys_rmdir(host);
}

SHIM_API int shimUnlink(const char *path)
{
    char host[SHIM_FS_HOST_MAX];
    return shim_dev_host(path, host, true) ? -1 : shim_sys_unlink(host);
}

SHIM_API int shimRemove(const char *path)
{
    char host[SHIM_FS_HOST_MAX];
    return shim_dev_host(path, host, true) ? -1 : remove(host);
}

SHIM_API int shimRename(const char *cur_path, const char *new_path)
{
    char a[SHIM_FS_HOST_MAX], b[SHIM_FS_HOST_MAX];
    if (shim_dev_host(cur_path, a, true) || shim_dev_host(new_path, b, true))
        return -1;
    return rename(a, b);
}

SHIM_API int shimAccess(const char *path, int mode)
{
    char host[SHIM_FS_HOST_MAX];
    if (shim_dev_host(path, host, (mode & W_OK) != 0))
        return -1;
    return access(host, mode);
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

static void shim_tmem_query(u64 addr, MemoryInfo *mi);   /* transfer memory: definido com a tabela de handles */

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
    shim_tmem_query(addr, &mi);
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

enum {
    KernelError_InvalidPriority = 112,
    KernelError_InvalidCoreId = 113,
    KernelError_TimedOut = 117,
};

#ifndef SHIM_MAX_HANDLES
#define SHIM_MAX_HANDLES 64
#endif
#ifndef SHIM_HANDLE_BASE
#define SHIM_HANDLE_BASE 0x200U   /* handles nao nulos e abaixo de HANDLE_WAIT_MASK */
#endif

typedef struct {
    int used, is_thread, started, signaled, closed, joined;
    pthread_t th;
    ThreadFunc entry;
    void *arg;
    int is_tmem;                 /* transfer memory (svcCreateTransferMemory) */
    u8 *tmem_src;                /* memoria de origem (protegida com tmem_perm enquanto o objeto existe) */
    u64 tmem_size;
    u32 tmem_perm;
    int tmem_fd;                 /* memfd que guarda a origem (-1: sem alias possivel) */
    off_t tmem_off;
    u8 *tmem_map;                /* destino do svcMapTransferMemory (NULL = nao mapeado) */
    char service_name[16];       /* nome da porta/servico IPC ("sm:", "time", "set", ...) */
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

/* Transfer memory: desfaz o mapeamento (se houver) e devolve Rw a origem. Chamar com g_shim_obj_lock. */
static void shim_tmem_release(ShimObj *o)
{
    if (o->tmem_map) {
        mmap(o->tmem_map, o->tmem_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
        o->tmem_map = NULL;
    }
    mprotect(o->tmem_src, o->tmem_size, PROT_READ | PROT_WRITE);
    o->is_tmem = 0;
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
    } else if (o->is_tmem) {
        shim_tmem_release(o);      /* desfaz o mapeamento e devolve Rw a origem */
        o->used = 0;
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

/* ------------------------------------------------------------------ IPC (CMIF / HIPC no Linux) */
static Handle shim_ipc_alloc_session_linux(const char *name)
{
    pthread_mutex_lock(&g_shim_obj_lock);
    Handle h = shim_alloc_obj(0);
    if (h != INVALID_HANDLE) {
        ShimObj *so = shim_obj(h);
        if (so && name) {
            strncpy(so->service_name, name, sizeof(so->service_name) - 1);
            so->service_name[sizeof(so->service_name) - 1] = '\0';
        }
    }
    pthread_mutex_unlock(&g_shim_obj_lock);
    return h;
}

/* svc 0x1F: cria sessao na tabela de handles associada a porta/servico nomeado. */
SHIM_API Result svcConnectToNamedPort(Handle *session, const char *name)
{
    if (!name || strnlen(name, 12) > 11)
        return KERNELRESULT(OutOfRange);
    Handle h = shim_ipc_alloc_session_linux(name);
    if (h == INVALID_HANDLE)
        return KERNELRESULT(OutOfHandles);
    *session = h;
    return 0;
}

/* svc 0x21: despacha mensagem IPC/CMIF atraves do session handle. */
SHIM_API Result svcSendSyncRequest(Handle session)
{
    pthread_mutex_lock(&g_shim_obj_lock);
    ShimObj *o = shim_obj(session);
    if (!o || o->is_thread || o->is_tmem) {
        pthread_mutex_unlock(&g_shim_obj_lock);
        return KERNELRESULT(InvalidHandle);
    }
    char srv_name[16];
    strncpy(srv_name, o->service_name, sizeof(srv_name) - 1);
    srv_name[sizeof(srv_name) - 1] = '\0';
    pthread_mutex_unlock(&g_shim_obj_lock);

    u8 *ipc_raw = (u8 *)shimGetTls();
    if (!ipc_raw)
        return KERNELRESULT(InvalidAddress);

    CmifInHeader in_hdr;
    size_t sfci_byte_off = 0;
    if (cmifParseInHeader(ipc_raw, SHIM_TLS_IPC_SIZE, &in_hdr, &sfci_byte_off) != 0) {
        /* Se nao contem 'SFCI', preserva retrocompatibilidade com chamadas dummy legadas */
        return 0;
    }

    size_t sfci_word_idx = sfci_byte_off / sizeof(u32);
    u32 *ipc_words = (u32 *)ipc_raw;
    const u8 *payload_in = ipc_raw + sfci_byte_off + 16;
    u8 *payload_out = ipc_raw + sfci_byte_off + 16;
    Handle created_handle = INVALID_HANDLE;

    Result res = shim_cmif_dispatch(srv_name, in_hdr.command_id, payload_in, payload_out,
                                    &created_handle, shim_ipc_alloc_session_linux);

    CmifOutHeader out_hdr;
    memset(&out_hdr, 0, sizeof(out_hdr));
    out_hdr.magic = CMIF_OUT_HEADER_MAGIC;
    out_hdr.version = 0;
    out_hdr.result = res;
    out_hdr.token = in_hdr.token;
    cmifFormatOutHeader(ipc_raw, SHIM_TLS_IPC_SIZE, sfci_byte_off, &out_hdr);

    if (created_handle != INVALID_HANDLE) {
        ipc_words[sfci_word_idx + 4] = created_handle;
    }

    if (sfci_word_idx > 0) {
        ipc_words[0] = 0;
        if (created_handle != INVALID_HANDLE) {
            ipc_words[1] = 4 | (1U << 31);
            ipc_words[2] = (1U << 8);
            ipc_words[3] = created_handle;
        } else {
            ipc_words[1] = 4;
        }
    }

    return 0;
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

/* ------------------------------------------------------------------ transfer memory (0x15 / 0x51 / 0x52) */
/*
 * No Horizon, transfer memory empresta memoria do processo a um servico (nvdrv recebe assim a memoria da
 * GPU no nvInitialize; o applet, buffers grandes). svcCreateTransferMemory cria o objeto sobre memoria ja
 * mapeada e, enquanto ele existe, a origem fica com a permissao `perm` no proprio processo (Perm_None =
 * so o servico enxerga) e atributo IsBorrowed. O servico mapeia o objeto com svcMapTransferMemory (Rw para
 * ele) e desfaz com svcUnmapTransferMemory; fechar o handle devolve Rw a origem.
 *
 * Aqui o "servico" e HLE e roda no mesmo processo: o mapeamento e um alias real (mmap MAP_SHARED do
 * mesmo memfd), entao o que um lado escreve o outro le. Isso exige que a origem esteja num memfd:
 *   - tmemCreate aloca a origem num memfd proprio (obdc-tmem);
 *   - a heap do svcSetHeapSize ja e um memfd;
 *   - outra memoria (ex.: malloc do Bionic) cria o objeto e protege a origem, mas svcMapTransferMemory
 *     devolve InvalidMemoryState (nao ha como fazer alias de memoria anonima privada).
 * O destino tem de estar na AliasRegion (como no svcMapMemory). A permissao do mapeamento tem de ser a
 * mesma da criacao (o kernel compara com a do dono: InvalidState).
 */
#define SHIM_TMEM_ALIAS_OFF (1ULL << 30)   /* tmemMap usa a AliasRegion a partir de +1 GiB */

typedef struct { u8 *addr; u64 size; int fd; } ShimTmemBacking;
static ShimTmemBacking g_shim_tmem_backing[SHIM_MAX_RANGES];   /* origens alocadas por tmemCreate */
static u64 g_shim_tmem_next;                                    /* proximo endereco do tmemMap (bump) */

typedef struct {
    Handle handle;
    size_t size;
    Permission perm;
    void *src_addr;
    void *map_addr;
} TransferMemory;

/* memfd + deslocamento que guardam [addr, addr + size), ou -1. Chamar com g_shim_obj_lock. */
static int shim_tmem_fd(u64 addr, u64 size, off_t *off)
{
    if (shim_in(g_shim_heap_base, g_shim_heap_size, addr, size)) {
        *off = (off_t)(addr - (u64)(uintptr_t)g_shim_heap_base);
        return g_shim_heap_fd;
    }
    for (int k = 0; k < SHIM_MAX_RANGES; k++) {
        const ShimTmemBacking *b = &g_shim_tmem_backing[k];
        if (b->addr && shim_in(b->addr, b->size, addr, size)) {
            *off = (off_t)(addr - (u64)(uintptr_t)b->addr);
            return b->fd;
        }
    }
    return -1;
}

static void shim_tmem_query(u64 addr, MemoryInfo *mi)
{
    pthread_mutex_lock(&g_shim_obj_lock);
    for (int k = 0; k < SHIM_MAX_HANDLES; k++) {
        const ShimObj *o = &g_shim_objs[k];
        if (!o->used || !o->is_tmem)
            continue;
        if (addr >= (u64)(uintptr_t)o->tmem_src && addr < (u64)(uintptr_t)o->tmem_src + o->tmem_size)
            mi->attr |= MemAttr_IsBorrowed;
        if (o->tmem_map && addr >= (u64)(uintptr_t)o->tmem_map && addr < (u64)(uintptr_t)o->tmem_map + o->tmem_size)
            mi->type = o->tmem_perm == Perm_None ? MemType_TransferMemIsolated : MemType_TransferMem;
    }
    pthread_mutex_unlock(&g_shim_obj_lock);
}

/* svc 0x15: cria o objeto sobre [addr, addr + size) e protege a origem com perm (None, R ou Rw). */
SHIM_API Result svcCreateTransferMemory(Handle *out, void *addr, size_t size, u32 perm)
{
    u64 a = (u64)(uintptr_t)addr;
    if (a % SHIM_PAGE)
        return KERNELRESULT(InvalidAddress);
    if (size == 0 || size % SHIM_PAGE)
        return KERNELRESULT(InvalidSize);
    if (perm != Perm_None && perm != Perm_R && perm != Perm_Rw)
        return KERNELRESULT(InvalidMemoryPermissions);
    pthread_mutex_lock(&g_shim_obj_lock);
    for (int k = 0; k < SHIM_MAX_HANDLES; k++) {
        const ShimObj *o = &g_shim_objs[k];
        if (o->used && o->is_tmem && a < (u64)(uintptr_t)o->tmem_src + o->tmem_size && (u64)(uintptr_t)o->tmem_src < a + size) {
            pthread_mutex_unlock(&g_shim_obj_lock);
            return KERNELRESULT(InvalidMemoryState);          /* ja emprestada */
        }
    }
    if (mprotect(addr, size, shim_prot(perm))) {
        pthread_mutex_unlock(&g_shim_obj_lock);
        return KERNELRESULT(InvalidMemoryState);              /* nao mapeada */
    }
    Handle h = shim_alloc_obj(0);
    ShimObj *o = shim_obj(h);
    if (o) {
        o->is_tmem = 1;
        o->tmem_src = (u8 *)addr;
        o->tmem_size = size;
        o->tmem_perm = perm;
        o->tmem_fd = shim_tmem_fd(a, size, &o->tmem_off);
    } else {
        mprotect(addr, size, PROT_READ | PROT_WRITE);
    }
    pthread_mutex_unlock(&g_shim_obj_lock);
    if (h == INVALID_HANDLE)
        return KERNELRESULT(OutOfHandles);
    *out = h;
    return 0;
}

/* svc 0x51: mapeia o objeto em addr (AliasRegion) com Rw para o "servico". perm = a da criacao. */
SHIM_API Result svcMapTransferMemory(Handle tmem_handle, void *addr, size_t size, u32 perm)
{
    u64 a = (u64)(uintptr_t)addr;
    if (a % SHIM_PAGE)
        return KERNELRESULT(InvalidAddress);
    if (size == 0 || size % SHIM_PAGE)
        return KERNELRESULT(InvalidSize);
    if (!shim_in(g_shim_alias_base, SHIM_ALIAS_REGION, a, size))
        return KERNELRESULT(InvalidMemoryRange);
    pthread_mutex_lock(&g_shim_obj_lock);
    ShimObj *o = shim_obj(tmem_handle);
    Result rc = 0;
    if (!o || !o->is_tmem)
        rc = KERNELRESULT(InvalidHandle);
    else if (size != o->tmem_size)
        rc = KERNELRESULT(InvalidSize);
    else if (perm != o->tmem_perm || o->tmem_map)
        rc = KERNELRESULT(InvalidState);
    else if (o->tmem_fd < 0 ||
             mmap(addr, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, o->tmem_fd, o->tmem_off) == MAP_FAILED)
        rc = KERNELRESULT(InvalidMemoryState);
    else
        o->tmem_map = (u8 *)addr;
    pthread_mutex_unlock(&g_shim_obj_lock);
    return rc;
}

/* svc 0x52: desfaz o svcMapTransferMemory (o destino volta a ser reserva PROT_NONE). */
SHIM_API Result svcUnmapTransferMemory(Handle tmem_handle, void *addr, size_t size)
{
    pthread_mutex_lock(&g_shim_obj_lock);
    ShimObj *o = shim_obj(tmem_handle);
    Result rc = 0;
    if (!o || !o->is_tmem)
        rc = KERNELRESULT(InvalidHandle);
    else if (!o->tmem_map || o->tmem_map != (u8 *)addr || o->tmem_size != size)
        rc = KERNELRESULT(InvalidMemoryRange);
    else {
        mmap(addr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
        o->tmem_map = NULL;
    }
    pthread_mutex_unlock(&g_shim_obj_lock);
    return rc;
}

/* ---- API da libnx (kernel/tmem.h) */
SHIM_API Result tmemCreateFromMemory(TransferMemory *t, void *buf, size_t size, Permission perm)
{
    memset(t, 0, sizeof *t);
    Result rc = svcCreateTransferMemory(&t->handle, buf, size, perm);
    if (R_SUCCEEDED(rc)) {
        t->size = size;
        t->perm = perm;
        t->src_addr = buf;
    }
    return rc;
}

/* Origem num memfd proprio (na libnx: memalign de 0x1000 na heap), para o mapeamento ser um alias real. */
SHIM_API Result tmemCreate(TransferMemory *t, size_t size, Permission perm)
{
    memset(t, 0, sizeof *t);
    if (size == 0 || size % SHIM_PAGE)
        return KERNELRESULT(InvalidSize);
    Result rc = shim_reserve_regions();                      /* AliasRegion para o tmemMap */
    if (R_FAILED(rc))
        return rc;
    int fd = (int)syscall(SYS_memfd_create, "obdc-tmem", 0);
    if (fd < 0 || ftruncate(fd, (off_t)size) != 0) {
        if (fd >= 0)
            close(fd);
        return LIBNXRESULT(OutOfMemory);
    }
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        close(fd);
        return LIBNXRESULT(OutOfMemory);
    }
    int slot = -1;
    pthread_mutex_lock(&g_shim_obj_lock);
    for (int k = 0; k < SHIM_MAX_RANGES && slot < 0; k++)
        if (!g_shim_tmem_backing[k].addr) {
            g_shim_tmem_backing[k] = (ShimTmemBacking){ (u8 *)p, size, fd };
            slot = k;
        }
    pthread_mutex_unlock(&g_shim_obj_lock);
    rc = slot < 0 ? LIBNXRESULT(OutOfMemory) : tmemCreateFromMemory(t, p, size, perm);
    if (R_FAILED(rc)) {
        pthread_mutex_lock(&g_shim_obj_lock);
        if (slot >= 0)
            g_shim_tmem_backing[slot].addr = NULL;
        pthread_mutex_unlock(&g_shim_obj_lock);
        munmap(p, size);
        close(fd);
    }
    return rc;
}

/* Objeto recebido de outro processo (so preenche a estrutura, como na libnx). */
SHIM_API void tmemLoadRemote(TransferMemory *t, Handle handle, size_t size, Permission perm)
{
    memset(t, 0, sizeof *t);
    t->handle = handle;
    t->size = size;
    t->perm = perm;
}

/* Mapeia na AliasRegion (operacao privilegiada no Horizon: aqui faz o papel do servico HLE). */
SHIM_API Result tmemMap(TransferMemory *t)
{
    if (t->map_addr)
        return KERNELRESULT(InvalidState);
    Result rc = shim_reserve_regions();
    if (R_FAILED(rc))
        return rc;
    pthread_mutex_lock(&g_shim_obj_lock);
    if (g_shim_tmem_next == 0 || g_shim_tmem_next + t->size > SHIM_ALIAS_REGION)
        g_shim_tmem_next = SHIM_TMEM_ALIAS_OFF;
    u8 *dst = g_shim_alias_base + g_shim_tmem_next;
    g_shim_tmem_next += (t->size + SHIM_PAGE - 1) & ~(SHIM_PAGE - 1);
    pthread_mutex_unlock(&g_shim_obj_lock);
    rc = svcMapTransferMemory(t->handle, dst, t->size, t->perm);
    if (R_SUCCEEDED(rc))
        t->map_addr = dst;
    return rc;
}

SHIM_API Result tmemUnmap(TransferMemory *t)
{
    Result rc = svcUnmapTransferMemory(t->handle, t->map_addr, t->size);
    if (R_SUCCEEDED(rc))
        t->map_addr = NULL;
    return rc;
}

SHIM_API Result tmemCloseHandle(TransferMemory *t)
{
    Result rc = 0;
    if (t->handle != INVALID_HANDLE) {
        rc = svcCloseHandle(t->handle);
        t->handle = INVALID_HANDLE;
    }
    return rc;
}

/* Espera a origem voltar a perm (na libnx: depois que o servico desfaz o mapeamento e o objeto morre). */
SHIM_API Result tmemWaitForPermission(TransferMemory *t, Permission perm)
{
    for (;;) {
        MemoryInfo mi;
        u32 pi;
        Result rc = svcQueryMemory(&mi, &pi, (u64)(uintptr_t)t->src_addr);
        if (R_FAILED(rc))
            return rc;
        if ((mi.perm & perm) == perm)
            return 0;
        svcSleepThread(100000);
    }
}

/* Desmapeia, fecha o handle, espera a origem voltar a Rw e libera a origem se ela foi alocada por tmemCreate. */
SHIM_API Result tmemClose(TransferMemory *t)
{
    Result rc = 0;
    if (t->map_addr)
        rc = tmemUnmap(t);
    if (R_SUCCEEDED(rc))
        rc = tmemCloseHandle(t);
    if (R_SUCCEEDED(rc) && t->src_addr) {
        rc = tmemWaitForPermission(t, Perm_Rw);
        pthread_mutex_lock(&g_shim_obj_lock);
        for (int k = 0; k < SHIM_MAX_RANGES; k++) {
            ShimTmemBacking *b = &g_shim_tmem_backing[k];
            if (b->addr == (u8 *)t->src_addr) {
                munmap(b->addr, b->size);
                close(b->fd);
                b->addr = NULL;
            }
        }
        pthread_mutex_unlock(&g_shim_obj_lock);
    }
    if (R_SUCCEEDED(rc))
        memset(t, 0, sizeof *t);
    return rc;
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

/* ------------------------------------------------------------------ TLS do Horizon (tpidrro_el0) */
/*
 * No Switch, tpidrro_el0 aponta para a regiao de TLS da thread: 0x200 bytes alinhados em 0x200.
 *   0x000-0x0FF  buffer das mensagens IPC (CMIF/HIPC) que svcSendSyncRequest envia;
 *   0x100-0x1DF  uso do usuario / sistema;
 *   0x1E0-0x1FF  ThreadVars da libnx (internal.h): magic, handle da thread, Thread*, reent do newlib e
 *                tls_tp em +0x1F8 (lido por __aarch64_read_tp).
 * No Android isso nao pode ser reproduzido no registrador:
 *   - mrs tpidrro_el0 em EL0 le 0 (o kernel nao publica TLS ali; sonda no A56);
 *   - tpidr_el0 pertence ao Bionic e x18 e a Shadow Call Stack: nenhum dos dois pode ser reaproveitado.
 * O shim guarda o bloco guest numa variavel __thread do C (o compilador a acessa via tpidr_el0 do Bionic,
 * sem x18), criado na primeira consulta da thread e liberado por um pthread_key quando a thread termina.
 * shimGetTls() / armGetTls() devolvem o endereco que o Horizon poria em tpidrro_el0; SHIM_MRS_TPIDRRO(x)
 * e a forma para codigo traduzido (no lugar de `mrs x, tpidrro_el0`). O shim nao tem assembly inline:
 * nenhum registrador e reservado, e x18 continua so do SCS (o NDK compila com -ffixed-x18 quando ativo).
 * Ainda sem segmento TLS do guest: tls_tp e thread_ptr ficam NULL; o handle e o da thread do HLE (Linux).
 */
#include <pthread.h>

#ifndef SHIM_TLS_SIZE
#define SHIM_TLS_SIZE     0x200
#endif
#ifndef SHIM_TLS_IPC_SIZE
#define SHIM_TLS_IPC_SIZE 0x100
#endif
#define THREADVARS_MAGIC  0x21545624   /* "!TV$" */

typedef struct {
    u32 magic;
    Handle handle;
    void *thread_ptr;          /* struct Thread * */
    void *reent;               /* struct _reent * do newlib (sem uso sobre o Bionic) */
    void *tls_tp;              /* +0x1F8 */
} ThreadVars;

_Static_assert(sizeof(ThreadVars) == 0x20, "ThreadVars da libnx tem 0x20 bytes (0x1E0..0x1FF)");

static __thread u8 *t_shim_tls;
static pthread_key_t g_shim_tls_key;
static pthread_once_t g_shim_tls_once = PTHREAD_ONCE_INIT;
static u32 g_shim_tls_live;                /* blocos vivos (teste de vazamento) */

static void shim_tls_free(void *raw)
{
    free(raw);
    __atomic_sub_fetch(&g_shim_tls_live, 1, __ATOMIC_ACQ_REL);
}

static void shim_tls_key_init(void) { pthread_key_create(&g_shim_tls_key, shim_tls_free); }

/* Endereco da TLS guest da thread atual (o que o Horizon poria em tpidrro_el0); NULL sem memoria. */
SHIM_API void *shimGetTls(void)
{
    if (t_shim_tls)
        return t_shim_tls;
    pthread_once(&g_shim_tls_once, shim_tls_key_init);
    void *raw = calloc(2, SHIM_TLS_SIZE);  /* alinhamento manual: o Bionic nao garante _Alignas(0x200) em __thread */
    if (!raw)
        return NULL;
    u8 *tls = (u8 *)(((uintptr_t)raw + SHIM_TLS_SIZE - 1) & ~(uintptr_t)(SHIM_TLS_SIZE - 1));
    ThreadVars *tv = (ThreadVars *)(tls + SHIM_TLS_SIZE - sizeof(ThreadVars));
    tv->magic = THREADVARS_MAGIC;
#if defined(__linux__)
    tv->handle = threadGetCurHandle();
#else
    tv->handle = INVALID_HANDLE;
#endif
    pthread_setspecific(g_shim_tls_key, raw);
    __atomic_add_fetch(&g_shim_tls_live, 1, __ATOMIC_ACQ_REL);
    t_shim_tls = tls;
    return tls;
}

/* libnx: armGetTls() le tpidrro_el0 (arm/tls.h). */
SHIM_API void *armGetTls(void) { return shimGetTls(); }

/* libnx: getThreadVars() (internal.h) = ultimos 0x20 bytes da TLS. */
SHIM_API ThreadVars *shimGetThreadVars(void)
{
    u8 *tls = (u8 *)shimGetTls();
    return tls ? (ThreadVars *)(tls + SHIM_TLS_SIZE - sizeof(ThreadVars)) : NULL;
}

/* Codigo traduzido: `mrs xN, tpidrro_el0` -> SHIM_MRS_TPIDRRO(xN). */
#define SHIM_MRS_TPIDRRO(dst) ((dst) = (u64)(uintptr_t)shimGetTls())

/* ------------------------------------------------------------------ time (time:u) */
/*
 * timeGetCurrentTime devolve segundos POSIX (UTC), como a libnx. Os tres relogios do Horizon (usuario,
 * rede, local) leem o mesmo CLOCK_REALTIME do host. shimTimeGetCurrentTimeUs da o mesmo instante em
 * microssegundos (na libnx o equivalente e gettimeofday do newlib). O steady clock usa CLOCK_MONOTONIC.
 * timeToCalendarTimeWithMyRule / timeToPosixTimeWithMyRule usam o fuso do host (localtime/mktime): no
 * Switch a "MyRule" e o fuso configurado no console.
 * Sem timeInitialize as consultas devolvem LibnxError_NotInitialized.
 */
typedef enum {
    TimeServiceType_User = 0, TimeServiceType_Menu = 1, TimeServiceType_System = 2,
    TimeServiceType_Repair = 3, TimeServiceType_SystemUser = 4,
} TimeServiceType;

typedef enum {
    TimeType_UserSystemClock,
    TimeType_NetworkSystemClock,
    TimeType_LocalSystemClock,
    TimeType_Default = TimeType_UserSystemClock,
} TimeType;

typedef struct { u8 uuid[0x10]; } Uuid;

typedef struct {
    s64 time_point;          /* segundos monotonicos */
    Uuid source_id;
} TimeSteadyClockTimePoint;

typedef struct {
    u16 year;
    u8 month;                /* 1..12 */
    u8 day;
    u8 hour;
    u8 minute;
    u8 second;
    u8 pad;
} TimeCalendarTime;

typedef struct {
    u32 wday;                /* 0 = domingo */
    u32 yday;                /* 0..365 */
    char timezoneName[8];
    u32 DST;
    s32 offset;              /* segundos em relacao ao UTC */
} TimeCalendarAdditionalInfo;

static int g_shim_time_refs;

SHIM_API Result timeInitialize(void) { g_shim_time_refs++; return 0; }
SHIM_API void timeExit(void) { if (g_shim_time_refs > 0) g_shim_time_refs--; }

static Result shim_time_realtime(TimeType type, struct timespec *ts)
{
    if (g_shim_time_refs <= 0)
        return LIBNXRESULT(NotInitialized);
    if ((u32)type > TimeType_LocalSystemClock)
        return LIBNXRESULT(BadInput);
    return clock_gettime(CLOCK_REALTIME, ts) ? LIBNXRESULT(IoError) : 0;
}

SHIM_API Result timeGetCurrentTime(TimeType type, u64 *timestamp)
{
    struct timespec ts;
    Result rc = timestamp ? shim_time_realtime(type, &ts) : LIBNXRESULT(BadInput);
    if (R_SUCCEEDED(rc))
        *timestamp = (u64)ts.tv_sec;
    return rc;
}

/* Mesmo relogio em microssegundos: timestamp_us / 1000000 == timeGetCurrentTime no mesmo instante. */
SHIM_API Result shimTimeGetCurrentTimeUs(TimeType type, u64 *timestamp_us)
{
    struct timespec ts;
    Result rc = timestamp_us ? shim_time_realtime(type, &ts) : LIBNXRESULT(BadInput);
    if (R_SUCCEEDED(rc))
        *timestamp_us = (u64)ts.tv_sec * 1000000ULL + (u64)ts.tv_nsec / 1000ULL;
    return rc;
}

SHIM_API Result timeGetStandardSteadyClockTimePoint(TimeSteadyClockTimePoint *out)
{
    static const Uuid source = { { 'o', 'b', 'd', 'c', '-', 's', 't', 'e', 'a', 'd', 'y', '-', 'c', 'l', 'k', 0 } };
    struct timespec ts;
    if (g_shim_time_refs <= 0)
        return LIBNXRESULT(NotInitialized);
    if (!out || clock_gettime(CLOCK_MONOTONIC, &ts))
        return LIBNXRESULT(BadInput);
    out->time_point = (s64)ts.tv_sec;
    out->source_id = source;
    return 0;
}

SHIM_API Result timeToCalendarTimeWithMyRule(u64 timestamp, TimeCalendarTime *caltime, TimeCalendarAdditionalInfo *info)
{
    if (g_shim_time_refs <= 0)
        return LIBNXRESULT(NotInitialized);
    if (!caltime)
        return LIBNXRESULT(BadInput);
    time_t t = (time_t)timestamp;
    struct tm tm;
#if defined(_WIN32)
    if (localtime_s(&tm, &t) != 0)
        return LIBNXRESULT(BadInput);
    s32 offset = (s32)(_mkgmtime(&tm) - t);
#else
    if (!localtime_r(&t, &tm))
        return LIBNXRESULT(BadInput);
    s32 offset = (s32)tm.tm_gmtoff;
#endif
    *caltime = (TimeCalendarTime){ (u16)(tm.tm_year + 1900), (u8)(tm.tm_mon + 1), (u8)tm.tm_mday,
                                   (u8)tm.tm_hour, (u8)tm.tm_min, (u8)tm.tm_sec, 0 };
    if (info) {
        memset(info, 0, sizeof *info);
        info->wday = (u32)tm.tm_wday;
        info->yday = (u32)tm.tm_yday;
        info->DST = tm.tm_isdst > 0;
        info->offset = offset;
#if defined(_WIN32)
        char tz[64] = "";                            /* nomes do Windows sao longos: corta em 7 caracteres */
        size_t n = 0;
        if (_get_tzname(&n, tz, sizeof tz, tm.tm_isdst > 0) != 0)
            tz[0] = '\0';
        snprintf(info->timezoneName, sizeof info->timezoneName, "%s", tz);
#else
        snprintf(info->timezoneName, sizeof info->timezoneName, "%s", tm.tm_zone ? tm.tm_zone : "");
#endif
    }
    return 0;
}

/* Calendario local -> segundos POSIX (1 resultado; o Horizon pode devolver 2 na virada de horario de verao). */
SHIM_API Result timeToPosixTimeWithMyRule(const TimeCalendarTime *caltime, u64 *timestamp_list, s32 timestamp_list_count, s32 *timestamp_count)
{
    if (g_shim_time_refs <= 0)
        return LIBNXRESULT(NotInitialized);
    if (!caltime || !timestamp_list || timestamp_list_count < 1 || !timestamp_count)
        return LIBNXRESULT(BadInput);
    struct tm tm = {0};
    tm.tm_year = caltime->year - 1900;
    tm.tm_mon = caltime->month - 1;
    tm.tm_mday = caltime->day;
    tm.tm_hour = caltime->hour;
    tm.tm_min = caltime->minute;
    tm.tm_sec = caltime->second;
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    if (t == (time_t)-1)
        return LIBNXRESULT(BadInput);
    timestamp_list[0] = (u64)t;
    *timestamp_count = 1;
    return 0;
}

/* ------------------------------------------------------------------ framebuffer linear (ponte de render por software) */
/*
 * Tela simulada em memoria. framebufferCreate configura a janela (NWindow) e aloca num_fbs buffers;
 * framebufferBegin devolve o buffer linear (framebufferMakeLinear) ou o buffer da vez; framebufferEnd
 * "apresenta": copia o quadro para o buffer da vez e o marca como a imagem na tela. O layout block linear
 * da Tegra nao e emulado: sem framebufferMakeLinear o buffer tambem e linear (na libnx seria swizzled).
 * Alinhamento: largura em multiplos de 64 pixels e altura de 128 linhas, como na libnx (stride 5120 em
 * 1280 x RGBA8888).
 * Validacao de oraculo: shimFramebufferChecksum = FNV-1a 64 dos bytes visiveis (largura x bpp por linha,
 * sem o preenchimento do stride) da ultima imagem apresentada; shimFramebufferDumpPPM grava essa imagem
 * como PPM P6 (RGB 8 bits; aceita sdmc:/ e romfs:/ via shimFopen).
 */
enum {
    PIXEL_FORMAT_RGBA_8888 = 1U,
    PIXEL_FORMAT_RGBX_8888 = 2U,
    PIXEL_FORMAT_RGB_565   = 4U,
    PIXEL_FORMAT_BGRA_8888 = 5U,
    PIXEL_FORMAT_RGBA_4444 = 7U,
};

#define RGBA8(r, g, b, a)          (((r) & 0xff) | (((g) & 0xff) << 8) | (((b) & 0xff) << 16) | (((u32)(a) & 0xff) << 24))
#define RGBA8_MAXALPHA(r, g, b)    RGBA8((r), (g), (b), 0xff)
#define RGBX8(r, g, b)             RGBA8((r), (g), (b), 0)
#define RGB565(r, g, b)            (((b) & 0x1f) | (((g) & 0x3f) << 5) | (((r) & 0x1f) << 11))
#define RGB565_FROM_RGB8(r, g, b)  RGB565((r) >> 3, (g) >> 2, (b) >> 3)
#define BGRA8(r, g, b, a)          RGBA8((b), (g), (r), (a))
#define BGRA8_MAXALPHA(r, g, b)    RGBA8((b), (g), (r), 0xff)
#define RGBA4(r, g, b, a)          (((r) & 0xf) | (((g) & 0xf) << 4) | (((b) & 0xf) << 8) | (((a) & 0xf) << 12))
#define RGBA4_MAXALPHA(r, g, b)    RGBA4((r), (g), (b), 0xf)

#define SHIM_FB_MAX_W 1920
#define SHIM_FB_MAX_H 1080

typedef struct NWindow {
    u32 width, height, format, bpp;
    bool has_fb;
    const u8 *presented;      /* imagem na tela (um dos buffers do framebuffer) */
    u32 stride;
    u64 present_count;
} NWindow;

typedef struct { u32 handle, id, size; void *cpu_addr; } NvMap;

typedef struct Framebuffer {
    NWindow *win;
    NvMap map;
    void *buf;                /* num_fbs buffers de fb_size bytes */
    void *buf_linear;         /* buffer de sombra (framebufferMakeLinear) */
    u32 stride;
    u32 width_aligned;
    u32 height_aligned;
    u32 num_fbs;
    u32 fb_size;
    bool has_init;
} Framebuffer;

static NWindow g_shim_window = { 1280, 720, PIXEL_FORMAT_RGBA_8888, 4, false, NULL, 0, 0 };
static u32 g_shim_fb_slot;    /* buffer da vez do framebuffer da janela padrao */

SHIM_API NWindow *nwindowGetDefault(void) { return &g_shim_window; }

SHIM_API Result framebufferCreate(Framebuffer *fb, NWindow *win, u32 width, u32 height, u32 format, u32 num_fbs)
{
    u32 bpp = format == PIXEL_FORMAT_RGBA_8888 || format == PIXEL_FORMAT_RGBX_8888 || format == PIXEL_FORMAT_BGRA_8888 ? 4
            : format == PIXEL_FORMAT_RGB_565 || format == PIXEL_FORMAT_RGBA_4444 ? 2 : 0;
    if (!fb || !win || !bpp || width == 0 || height == 0 || width > SHIM_FB_MAX_W || height > SHIM_FB_MAX_H ||
        num_fbs < 1 || num_fbs > 3)
        return LIBNXRESULT(BadInput);
    if (win->has_fb)
        return LIBNXRESULT(AlreadyInitialized);
    memset(fb, 0, sizeof *fb);
    fb->width_aligned = (width + 63) & ~63U;
    fb->height_aligned = (height + 127) & ~127U;
    fb->stride = fb->width_aligned * bpp;
    fb->fb_size = fb->stride * fb->height_aligned;
    fb->num_fbs = num_fbs;
    fb->buf = calloc(num_fbs, fb->fb_size);
    if (!fb->buf)
        return LIBNXRESULT(OutOfMemory);
    fb->win = win;
    win->width = width;
    win->height = height;
    win->format = format;
    win->bpp = bpp;
    win->stride = fb->stride;
    win->presented = NULL;
    win->present_count = 0;
    win->has_fb = true;
    g_shim_fb_slot = 0;
    fb->has_init = true;
    return 0;
}

SHIM_API Result framebufferMakeLinear(Framebuffer *fb)
{
    if (!fb || !fb->has_init)
        return LIBNXRESULT(NotInitialized);
    if (!fb->buf_linear) {
        fb->buf_linear = calloc(1, fb->fb_size);
        if (!fb->buf_linear)
            return LIBNXRESULT(OutOfMemory);
    }
    return 0;
}

SHIM_API void *framebufferBegin(Framebuffer *fb, u32 *out_stride)
{
    if (!fb || !fb->has_init)
        return NULL;
    if (out_stride)
        *out_stride = fb->stride;
    return fb->buf_linear ? fb->buf_linear : (u8 *)fb->buf + (size_t)g_shim_fb_slot * fb->fb_size;
}

SHIM_API void framebufferEnd(Framebuffer *fb)
{
    if (!fb || !fb->has_init)
        return;
    u8 *slot = (u8 *)fb->buf + (size_t)g_shim_fb_slot * fb->fb_size;
    if (fb->buf_linear)
        memcpy(slot, fb->buf_linear, fb->fb_size);
    fb->win->presented = slot;
    fb->win->present_count++;
    g_shim_fb_slot = (g_shim_fb_slot + 1) % fb->num_fbs;
}

SHIM_API void framebufferClose(Framebuffer *fb)
{
    if (!fb || !fb->has_init)
        return;
    fb->win->has_fb = false;
    fb->win->presented = NULL;
    free(fb->buf);
    free(fb->buf_linear);
    memset(fb, 0, sizeof *fb);
}

/* FNV-1a 64 da ultima imagem apresentada na janela padrao (0 se nada foi apresentado). */
SHIM_API u64 shimFramebufferChecksum(void)
{
    const NWindow *w = &g_shim_window;
    if (!w->has_fb || !w->presented)
        return 0;
    u64 h = 0xcbf29ce484222325ULL;
    for (u32 y = 0; y < w->height; y++) {
        const u8 *row = w->presented + (size_t)y * w->stride;
        for (u32 k = 0; k < w->width * w->bpp; k++)
            h = (h ^ row[k]) * 0x100000001b3ULL;
    }
    return h;
}

SHIM_API u64 shimFramebufferPresentCount(void) { return g_shim_window.present_count; }

static void shim_fb_rgb(const NWindow *w, const u8 *p, u8 rgb[3])
{
    u32 v = w->bpp == 4 ? (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 : (u32)p[0] | (u32)p[1] << 8;
    switch (w->format) {
    case PIXEL_FORMAT_BGRA_8888:
        rgb[0] = (u8)(v >> 16); rgb[1] = (u8)(v >> 8); rgb[2] = (u8)v;
        break;
    case PIXEL_FORMAT_RGB_565: {
        u32 r = (v >> 11) & 0x1f, g = (v >> 5) & 0x3f, b = v & 0x1f;
        rgb[0] = (u8)(r << 3 | r >> 2); rgb[1] = (u8)(g << 2 | g >> 4); rgb[2] = (u8)(b << 3 | b >> 2);
        break;
    }
    case PIXEL_FORMAT_RGBA_4444:
        rgb[0] = (u8)((v & 0xf) * 17); rgb[1] = (u8)(((v >> 4) & 0xf) * 17); rgb[2] = (u8)(((v >> 8) & 0xf) * 17);
        break;
    default:                                         /* RGBA8888 / RGBX8888 */
        rgb[0] = (u8)v; rgb[1] = (u8)(v >> 8); rgb[2] = (u8)(v >> 16);
        break;
    }
}

/* Grava a ultima imagem apresentada como PPM P6 (sem alfa). */
SHIM_API Result shimFramebufferDumpPPM(const char *path)
{
    const NWindow *w = &g_shim_window;
    if (!w->has_fb || !w->presented)
        return LIBNXRESULT(NotInitialized);
    FILE *f = shimFopen(path, "wb");
    if (!f)
        return LIBNXRESULT(IoError);
    u8 *row = (u8 *)malloc((size_t)w->width * 3);
    int ok = row && fprintf(f, "P6\n%u %u\n255\n", w->width, w->height) > 0;
    for (u32 y = 0; ok && y < w->height; y++) {
        const u8 *src = w->presented + (size_t)y * w->stride;
        for (u32 x = 0; x < w->width; x++)
            shim_fb_rgb(w, src + (size_t)x * w->bpp, row + (size_t)x * 3);
        ok = fwrite(row, 3, w->width, f) == w->width;
    }
    free(row);
    ok = fclose(f) == 0 && ok;
    return ok ? 0 : LIBNXRESULT(IoError);
}

/* ------------------------------------------------------------------ roteamento do stdio (sdmc:/ e romfs:/) */
/* Daqui em diante (codigo do homebrew), as chamadas de arquivo do stdio/POSIX traduzem "nome:/" pelos
 * dispositivos do shim. O codigo do proprio shim, acima, nao e afetado. */
#if !defined(OBDC_NO_STDIO_REDIRECT)
#undef access                                  /* o io.h do mingw define access como macro */
#define fopen(path, mode)  shimFopen((path), (mode))
#define opendir(path)      shimOpendir(path)
#define stat(path, st)     shimStat((path), (st))
#define mkdir(path, mode)  shimMkdir((path), (mode))
#define rmdir(path)        shimRmdir(path)
#define remove(path)       shimRemove(path)
#define unlink(path)       shimUnlink(path)
#define rename(a, b)       shimRename((a), (b))
#define access(path, mode) shimAccess((path), (mode))
#endif

#endif /* SWITCH_HLE_SHIM_H */
