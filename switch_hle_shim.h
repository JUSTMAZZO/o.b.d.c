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
