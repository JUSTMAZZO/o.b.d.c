/*
 * hb_fb.c - homebrew libnx com relogio (time:u), ciclo de vida do applet e framebuffer linear (Fase 3).
 *
 * Desenha 60 quadros 1280x720 RGBA8888 por software: barras de cor rolando (faixa de cima), degrade
 * animado e um quadrado branco em movimento. A animacao depende so do numero do quadro (nao do tick):
 * assim o checksum e deterministico; o tick mede o tempo de render (linha "Tempo", fora da comparacao).
 * Por quadro, o app soma os pixels ponderados pela posicao (Σ px[i] * (i + 1) mod 2^64); no fim calcula
 * o FNV-1a 64 do ultimo quadro. Os dois saem do proprio buffer linear: mesmo calculo no Switch.
 * O laco e o padrao com appletLockExit + hook OnExitRequest: termina quando o sistema pede a saida (ou com
 * Plus no Switch). No oraculo o "sistema" pede a saida depois do quadro 60 e a tela apresentada (shim) e
 * conferida e gravada em sdmc:/obdc_hb_fb.ppm. Referencia: ref_hb_fb.py (numpy).
 *
 * Switch (devkitA64 + libnx, flags do template oficial):
 *   ARCH="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE"
 *   aarch64-none-elf-gcc $ARCH -g -Wall -Wextra -O2 -ffunction-sections -D__SWITCH__ -I $DEVKITPRO/libnx/include -c hb_fb.c
 *   aarch64-none-elf-gcc -specs=$DEVKITPRO/libnx/switch.specs -g $ARCH hb_fb.o -L $DEVKITPRO/libnx/lib -lnx -o hb_fb.elf
 *   elf2nro hb_fb.elf hb_fb.nro
 * Oraculo: -DOBDC_ORACLE -I ../.. (cartao SD = ./sdcard ou OBDC_SDCARD).
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#ifdef OBDC_ORACLE
#include "switch_hle_shim.h"
#else
#include <switch.h>
#endif

#define W 1280
#define H 720
#define BAR_H 120
#define SQ 64

static const u32 s_bars[8] = {
    RGBA8_MAXALPHA(0xFF, 0xFF, 0xFF), RGBA8_MAXALPHA(0xFF, 0xFF, 0x00), RGBA8_MAXALPHA(0x00, 0xFF, 0xFF),
    RGBA8_MAXALPHA(0x00, 0xFF, 0x00), RGBA8_MAXALPHA(0xFF, 0x00, 0xFF), RGBA8_MAXALPHA(0xFF, 0x00, 0x00),
    RGBA8_MAXALPHA(0x00, 0x00, 0xFF), RGBA8_MAXALPHA(0x10, 0x10, 0x10),
};

static int s_exit_hooks, s_focus_hooks;

static void on_applet(AppletHookType hook, void *param)
{
    (void)param;
    if (hook == AppletHookType_OnExitRequest)
        s_exit_hooks++;
    else if (hook == AppletHookType_OnFocusState)
        s_focus_hooks++;
}

static void out(const char *s) { svcOutputDebugString(s, strlen(s)); }

/* Desenha o quadro f (1..) e devolve Σ px[i] * (i + 1) mod 2^64 (i = y * W + x). */
static u64 render(u32 *px, u32 stride_px, u32 f)
{
    u32 sx = (f * 20) % (W - SQ), sy = 300 + (f * 7) % 200;
    u64 sum = 0;
    for (u32 y = 0; y < H; y++) {
        u32 *row = px + (size_t)y * stride_px;
        for (u32 x = 0; x < W; x++) {
            u32 c;
            if (x - sx < SQ && y - sy < SQ)
                c = RGBA8_MAXALPHA(0xFF, 0xFF, 0xFF);
            else if (y < BAR_H)
                c = s_bars[((x + f * 8) / 160) & 7];
            else
                c = RGBA8_MAXALPHA((x * 255 / (W - 1) + f * 4) & 0xFF, ((y - BAR_H) * 255 / (H - BAR_H - 1) + f * 2) & 0xFF,
                                   ((x ^ y) + f * 3) & 0xFF);
            row[x] = c;
            sum += (u64)c * ((u64)y * W + x + 1);
        }
    }
    return sum;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    char line[160];
    out("[+] hb_fb iniciado\n");

    /* relogio */
    u64 now = 0;
    TimeCalendarTime cal = {0};
    TimeCalendarAdditionalInfo info;
    Result rc = timeInitialize();
    if (R_SUCCEEDED(rc))
        rc = timeGetCurrentTime(TimeType_Default, &now);
    if (R_SUCCEEDED(rc))
        rc = timeToCalendarTimeWithMyRule(now, &cal, &info);
    s64 skew = (s64)now - (s64)time(NULL);
    snprintf(line, sizeof line, "[+] Relogio (time:u): 0x%x, %s\n", rc,
             R_SUCCEEDED(rc) && now >= 1700000000ULL && cal.year >= 2023 && skew >= -2 && skew <= 2 ? "coerente com time()" : "INCOERENTE");
    out(line);

    /* applet */
    AppletHookCookie cookie;
    appletLockExit();
    appletHook(&cookie, on_applet, NULL);
    bool apd = false;
    Result rc_apd = appletIsAutoPowerDownRequested(&apd);
    snprintf(line, sizeof line, "[+] Foco: %d (InFocus = 1), modo: %d, AutoPowerDown: 0x%x / %d\n", (int)appletGetFocusState(),
             (int)appletGetOperationMode(), rc_apd, (int)apd);
    out(line);

    /* framebuffer */
    Framebuffer fb;
    rc = framebufferCreate(&fb, nwindowGetDefault(), W, H, PIXEL_FORMAT_RGBA_8888, 2);
    if (R_SUCCEEDED(rc))
        rc = framebufferMakeLinear(&fb);
    snprintf(line, sizeof line, "[+] framebufferCreate + MakeLinear: 0x%x\n", rc);
    out(line);
    if (R_FAILED(rc))
        return 1;

#ifdef OBDC_ORACLE
    static const ShimInputEvent no_input[1];
    shimInputSetScript(no_input, 0);                      /* sem entrada: quem encerra e o pedido de saida */
#endif
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    u32 frames = 0, stride = 0;
    u64 total = 0, ticks = 0;
    u32 *px = NULL;
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
            break;
        px = (u32 *)framebufferBegin(&fb, &stride);
        u64 t0 = armGetSystemTick();
        total += render(px, stride / 4, frames + 1);
        ticks += armGetSystemTick() - t0;
        framebufferEnd(&fb);
        frames++;
#ifdef OBDC_ORACLE
        if (frames == 60)
            shimAppletRequestExit();
#endif
    }

    u64 fnv = 0xcbf29ce484222325ULL;
    for (u32 y = 0; px && y < H; y++) {
        const u8 *row = (const u8 *)(px + (size_t)y * (stride / 4));
        for (u32 k = 0; k < W * 4; k++)
            fnv = (fnv ^ row[k]) * 0x100000001b3ULL;
    }
    snprintf(line, sizeof line, "[+] Frames: %u, saida: %s (OnExitRequest x%d, foco x%d)\n", frames,
             s_exit_hooks ? "pedido do sistema" : "botao Plus", s_exit_hooks, s_focus_hooks);
    out(line);
    snprintf(line, sizeof line, "[+] Soma ponderada (todos os frames): 0x%016llx\n", (unsigned long long)total);
    out(line);
    snprintf(line, sizeof line, "[+] FNV-1a do ultimo frame: 0x%016llx\n", (unsigned long long)fnv);
    out(line);
    snprintf(line, sizeof line, "[+] Tempo de render: %.3f ms/frame\n", frames ? armTicksToNs(ticks) / 1e6 / frames : 0.0);
    out(line);

#ifdef OBDC_ORACLE
    u64 screen = shimFramebufferChecksum();
    snprintf(line, sizeof line, "[+] Tela apresentada (shim): %s, %llu apresentacoes\n", screen == fnv ? "igual ao ultimo frame" : "DIFERENTE",
             (unsigned long long)shimFramebufferPresentCount());
    out(line);
    Result rc_ppm = fsInitialize();
    if (R_SUCCEEDED(rc_ppm))
        rc_ppm = fsdevMountSdmc();
    if (R_SUCCEEDED(rc_ppm))
        rc_ppm = shimFramebufferDumpPPM("sdmc:/obdc_hb_fb.ppm");
    snprintf(line, sizeof line, "[+] PPM sdmc:/obdc_hb_fb.ppm: 0x%x\n", rc_ppm);
    out(line);
    fsdevUnmountAll();
    fsExit();
#endif

    framebufferClose(&fb);
    appletUnhook(&cookie);
    appletUnlockExit();
    timeExit();
    return 0;
}
