/*
 * hb_services.c - homebrew libnx que abre e fecha os servicos basicos do Horizon por IPC (Fase 3):
 * sm (gerenciador de servicos), applet (appletMainLoop), hid (pad) e fs.
 *
 * Serve para o inventario estatico (nro_tool.py): quais SVCs de IPC e de memoria compartilhada a libnx
 * usa para falar com os servicos. A inicializacao fica no main (e nao no __appInit) para que o oraculo
 * pelo shim percorra o mesmo codigo: 60 frames com padUpdate, contando os botoes pressionados.
 *
 * Switch (devkitA64 + libnx, flags do template oficial):
 *   ARCH="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE"
 *   aarch64-none-elf-gcc $ARCH -g -Wall -O2 -ffunction-sections -D__SWITCH__ -I $DEVKITPRO/libnx/include -c hb_services.c
 *   aarch64-none-elf-gcc -specs=$DEVKITPRO/libnx/switch.specs -g $ARCH hb_services.o -L $DEVKITPRO/libnx/lib -lnx -o hb_services.elf
 *   elf2nro hb_services.elf hb_services.nro
 * Oraculo: -DOBDC_ORACLE -I ../.. (servicos dummy do switch_hle_shim.h; pad deterministico: A no frame 30, Plus no 60).
 */
#include <stdio.h>
#include <string.h>
#ifdef OBDC_ORACLE
#include "switch_hle_shim.h"
#else
#include <switch.h>

void __appInit(void) {}
void __appExit(void) {}
#endif

static void out(const char *s) { svcOutputDebugString(s, strlen(s)); }

static int open_service(const char *name, Result rc)
{
    char line[96];
    snprintf(line, sizeof line, "[+] %s: 0x%x\n", name, rc);
    out(line);
    return R_SUCCEEDED(rc);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    char line[96];
    out("[+] hb_services iniciado\n");

    int ok = 0;
    ok += open_service("smInitialize", smInitialize());
    ok += open_service("appletInitialize", appletInitialize());
    ok += open_service("hidInitialize", hidInitialize());
    ok += open_service("fsInitialize", fsInitialize());

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    u32 frames = 0, presses = 0;
    while (appletMainLoop() && frames < 60) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad))
            presses++;
        frames++;
    }

    snprintf(line, sizeof line, "[+] Servicos abertos: %d\n", ok);
    out(line);
    snprintf(line, sizeof line, "[+] Frames: %u\n", frames);
    out(line);
    snprintf(line, sizeof line, "[+] Botoes pressionados: %u\n", presses);
    out(line);

    fsExit();
    hidExit();
    appletExit();
    smExit();
    return 0;
}
