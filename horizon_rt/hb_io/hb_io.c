/*
 * hb_io.c - homebrew libnx que usa o cartao SD (fs) e le botoes + analogicos (pad) por 60 frames (Fase 3).
 *
 * Exercita a API de alto nivel que o switch_hle_shim.h intercepta: fsOpenSdCardFileSystem, fsFs* (criar,
 * listar, renomear, apagar), fsFile* (escrita com Append, leitura posicional) e padUpdate/padGetStickPos.
 * Escreve 1 MiB em /obdc_hb_io/dados.bin em blocos de 4 KiB, le de volta calculando FNV-1a 64 e apaga tudo
 * no fim (pode rodar varias vezes).
 *
 * Switch (devkitA64 + libnx, flags do template oficial):
 *   ARCH="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE"
 *   aarch64-none-elf-gcc $ARCH -g -Wall -Wextra -O2 -ffunction-sections -D__SWITCH__ -I $DEVKITPRO/libnx/include -c hb_io.c
 *   aarch64-none-elf-gcc -specs=$DEVKITPRO/libnx/switch.specs -g $ARCH hb_io.o -L $DEVKITPRO/libnx/lib -lnx -o hb_io.elf
 *   elf2nro hb_io.elf hb_io.nro
 * Oraculo: -DOBDC_ORACLE -I ../.. (cartao SD = ./sdcard ou OBDC_SDCARD). No oraculo o pad segue um roteiro:
 * A no frame 30, Plus no 60, analogico esquerdo em rampa e direito no maximo do frame 20 ao 39.
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

#define DATA_SIZE (1u << 20)
#define CHUNK     4096u
#define DIR_PATH  "/obdc_hb_io"
#define DATA_PATH DIR_PATH "/dados.bin"

static u8 s_buf[CHUNK];

static void out(const char *s) { svcOutputDebugString(s, strlen(s)); }

static int step(const char *name, Result rc)
{
    char line[128];
    if (R_FAILED(rc)) {
        snprintf(line, sizeof line, "[!] %s: 0x%x\n", name, rc);
        out(line);
    }
    return R_SUCCEEDED(rc);
}

static u8 pattern(u32 i) { return (u8)((i * 131u + 7u) ^ (i >> 7)); }

#ifdef OBDC_ORACLE
static void oracle_input_script(void)
{
    static ShimInputEvent script[60];
    for (u32 f = 1; f <= 60; f++) {
        ShimInputEvent *e = &script[f - 1];
        e->frame = f;
        e->state.buttons = f == 30 ? HidNpadButton_A : f == 60 ? HidNpadButton_Plus : 0;
        e->state.sticks[0].x = (s32)f * 512 - 16384;
        e->state.sticks[0].y = 16384 - (s32)f * 256;
        e->state.sticks[1].x = f >= 20 && f < 40 ? JOYSTICK_MAX : 0;
        e->state.sticks[1].y = 0;
    }
    shimInputSetScript(script, 60);
}
#endif

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    char line[128];
    out("[+] hb_io iniciado\n");
    if (!step("smInitialize", smInitialize()) || !step("appletInitialize", appletInitialize()) ||
        !step("hidInitialize", hidInitialize()) || !step("fsInitialize", fsInitialize()))
        return 1;
#ifdef OBDC_ORACLE
    oracle_input_script();
#endif

    FsFileSystem sd;
    Result rc = fsOpenSdCardFileSystem(&sd);
    snprintf(line, sizeof line, "[+] fsOpenSdCardFileSystem: 0x%x\n", rc);
    out(line);
    if (R_FAILED(rc))
        return 1;

    /* escreve 1 MiB (Append estende o arquivo) */
    FsFile f;
    fsFsDeleteDirectoryRecursively(&sd, DIR_PATH);         /* sobra de uma execucao interrompida */
    if (!step("fsFsCreateDirectory", fsFsCreateDirectory(&sd, DIR_PATH)) ||
        !step("fsFsCreateFile", fsFsCreateFile(&sd, DATA_PATH, 0, 0)) ||
        !step("fsFsOpenFile(w)", fsFsOpenFile(&sd, DATA_PATH, FsOpenMode_Write | FsOpenMode_Append, &f)))
        return 1;
    for (u32 off = 0; off < DATA_SIZE; off += CHUNK) {
        for (u32 k = 0; k < CHUNK; k++)
            s_buf[k] = pattern(off + k);
        if (!step("fsFileWrite", fsFileWrite(&f, off, s_buf, CHUNK, FsWriteOption_None)))
            return 1;
    }
    step("fsFileFlush", fsFileFlush(&f));
    fsFileClose(&f);

    /* le de volta: FNV-1a 64 */
    s64 size = 0;
    u64 hash = 0xcbf29ce484222325ULL, total = 0, got = 0;
    if (!step("fsFsOpenFile(r)", fsFsOpenFile(&sd, DATA_PATH, FsOpenMode_Read, &f)) ||
        !step("fsFileGetSize", fsFileGetSize(&f, &size)))
        return 1;
    do {
        if (!step("fsFileRead", fsFileRead(&f, (s64)total, s_buf, CHUNK, FsReadOption_None, &got)))
            return 1;
        for (u64 k = 0; k < got; k++)
            hash = (hash ^ s_buf[k]) * 0x100000001b3ULL;
        total += got;
    } while (got == CHUNK);
    fsFileClose(&f);
    snprintf(line, sizeof line, "[+] Arquivo: %lld bytes, lidos %llu\n", (long long)size, (unsigned long long)total);
    out(line);
    snprintf(line, sizeof line, "[+] FNV-1a: 0x%016llx\n", (unsigned long long)hash);
    out(line);

    /* resultado em texto, renomeado; o diretorio fica com 2 arquivos */
    int n = snprintf(line, sizeof line, "%016llx\n", (unsigned long long)hash);
    if (!step("fsFsCreateFile(txt)", fsFsCreateFile(&sd, DIR_PATH "/resultado.tmp", 0, 0)) ||
        !step("fsFsOpenFile(txt)", fsFsOpenFile(&sd, DIR_PATH "/resultado.tmp", FsOpenMode_Write | FsOpenMode_Append, &f)) ||
        !step("fsFileWrite(txt)", fsFileWrite(&f, 0, line, (u64)n, FsWriteOption_Flush)))
        return 1;
    fsFileClose(&f);
    step("fsFsRenameFile", fsFsRenameFile(&sd, DIR_PATH "/resultado.tmp", DIR_PATH "/resultado.txt"));
    FsDir d;
    s64 entries = -1;
    if (step("fsFsOpenDirectory", fsFsOpenDirectory(&sd, DIR_PATH, FsDirOpenMode_ReadFiles, &d))) {
        step("fsDirGetEntryCount", fsDirGetEntryCount(&d, &entries));
        fsDirClose(&d);
    }
    snprintf(line, sizeof line, "[+] Arquivos em " DIR_PATH ": %lld\n", (long long)entries);
    out(line);

    /* 60 frames de pad: botoes e analogicos */
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    u32 frames = 0, presses = 0;
    s64 lx = 0, ly = 0, rx = 0;
    while (appletMainLoop() && frames < 60) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad))
            presses++;
        HidAnalogStickState l = padGetStickPos(&pad, 0), r = padGetStickPos(&pad, 1);
        lx += l.x;
        ly += l.y;
        rx += r.x;
        frames++;
    }
    snprintf(line, sizeof line, "[+] Frames: %u, botoes pressionados: %u\n", frames, presses);
    out(line);
    snprintf(line, sizeof line, "[+] Analogicos (soma): esquerdo x=%lld y=%lld, direito x=%lld\n",
             (long long)lx, (long long)ly, (long long)rx);
    out(line);

    /* limpeza */
    FsDirEntryType type;
    rc = fsFsDeleteDirectoryRecursively(&sd, DIR_PATH);
    snprintf(line, sizeof line, "[+] Limpeza: 0x%x, %s\n", rc,
             fsFsGetEntryType(&sd, DIR_PATH, &type) != 0 ? "diretorio removido" : "diretorio ainda existe");
    out(line);
    fsFsClose(&sd);

    fsExit();
    hidExit();
    appletExit();
    smExit();
    return 0;
}
