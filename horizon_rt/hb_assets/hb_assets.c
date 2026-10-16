/*
 * hb_assets.c - homebrew libnx que le assets do RomFS e grava no cartao SD pelo stdio (Fase 3).
 *
 * Caminho padrao de um homebrew: romfsInit + fsdevMountSdmc e depois so stdio/POSIX com "romfs:/" e
 * "sdmc:/" (fopen/fread/fseek/stat/opendir/remove). Le romfs:/data/bloco.bin (256 KiB) em blocos de
 * 4 KiB calculando CRC-32 e FNV-1a 64 (oraculo de integridade: gen_romfs.py), confere uma leitura com
 * fseek, le romfs:/texto/info.txt, lista romfs:/ e tenta escrever / sair da raiz do romfs (recusado).
 * Por fim grava o CRC em sdmc:/obdc_hb_assets.txt, le de volta e apaga.
 *
 * Assets: python gen_romfs.py  (gera romfs/ e imprime os valores esperados)
 * Switch (devkitA64 + libnx, flags do template oficial):
 *   ARCH="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE"
 *   aarch64-none-elf-gcc $ARCH -g -Wall -Wextra -O2 -ffunction-sections -D__SWITCH__ -I $DEVKITPRO/libnx/include -c hb_assets.c
 *   aarch64-none-elf-gcc -specs=$DEVKITPRO/libnx/switch.specs -g $ARCH hb_assets.o -L $DEVKITPRO/libnx/lib -lnx -o hb_assets.elf
 *   elf2nro hb_assets.elf hb_assets.nro --romfsdir=romfs
 * Oraculo: -DOBDC_ORACLE -I ../.. (romfs:/ = ./romfs ou OBDC_ROMFS; sdmc:/ = ./sdcard ou OBDC_SDCARD).
 */
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#ifdef OBDC_ORACLE
#include "switch_hle_shim.h"
#else
#include <switch.h>

void __appInit(void) {}
void __appExit(void) {}
#endif

#define CHUNK     4096u
#define BLOCK     "romfs:/data/bloco.bin"
#define SD_RESULT "sdmc:/obdc_hb_assets.txt"

static unsigned char s_buf[CHUNK];
static u32 s_crc_table[256];

static void out(const char *s) { svcOutputDebugString(s, strlen(s)); }

static int step(const char *name, Result rc)
{
    char line[96];
    snprintf(line, sizeof line, "[+] %s: 0x%x\n", name, rc);
    out(line);
    return R_SUCCEEDED(rc);
}

static void crc32_init(void)
{
    for (u32 n = 0; n < 256; n++) {
        u32 c = n;
        for (int k = 0; k < 8; k++)
            c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        s_crc_table[n] = c;
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    char line[128];
    out("[+] hb_assets iniciado\n");
    if (!step("smInitialize", smInitialize()) || !step("fsInitialize", fsInitialize()) ||
        !step("fsdevMountSdmc", fsdevMountSdmc()) || !step("romfsInit", romfsInit()))
        return 1;

    /* metadados + leitura sequencial do bloco */
    struct stat st;
    if (stat(BLOCK, &st) != 0) {
        out("[!] stat(" BLOCK ") falhou\n");
        return 1;
    }
    FILE *f = fopen(BLOCK, "rb");
    if (!f) {
        out("[!] fopen(" BLOCK ") falhou\n");
        return 1;
    }
    crc32_init();
    u32 crc = 0xFFFFFFFFu;
    u64 fnv = 0xcbf29ce484222325ULL, total = 0;
    u64 t0 = armGetSystemTick();
    size_t n;
    while ((n = fread(s_buf, 1, CHUNK, f)) > 0) {
        for (size_t k = 0; k < n; k++) {
            crc = s_crc_table[(crc ^ s_buf[k]) & 0xFF] ^ (crc >> 8);
            fnv = (fnv ^ s_buf[k]) * 0x100000001b3ULL;
        }
        total += n;
    }
    u64 t1 = armGetSystemTick();
    crc ^= 0xFFFFFFFFu;

    /* acesso aleatorio */
    unsigned char probe[4] = {0};
    int seek_ok = fseek(f, 0x10000, SEEK_SET) == 0 && fread(probe, 1, 4, f) == 4;
    fclose(f);

    snprintf(line, sizeof line, "[+] Bloco: %lld bytes (stat), %llu lidos\n", (long long)st.st_size, (unsigned long long)total);
    out(line);
    snprintf(line, sizeof line, "[+] CRC-32: 0x%08x\n", crc);
    out(line);
    snprintf(line, sizeof line, "[+] FNV-1a: 0x%016llx\n", (unsigned long long)fnv);
    out(line);
    snprintf(line, sizeof line, "[+] Bytes em 0x10000: %s%02x%02x%02x%02x\n", seek_ok ? "" : "(fseek falhou) ",
             probe[0], probe[1], probe[2], probe[3]);
    out(line);
    snprintf(line, sizeof line, "[+] Tempo de leitura: %.3f ms\n", armTicksToNs(t1 - t0) / 1e6);
    out(line);

    /* texto e listagem */
    char info[64] = "";
    f = fopen("romfs:/texto/info.txt", "r");
    if (f) {
        if (!fgets(info, sizeof info, f))
            info[0] = '\0';
        fclose(f);
    }
    snprintf(line, sizeof line, "[+] info.txt: %s", info[0] ? info : "(vazio)\n");
    out(line);
    int entries = 0;
    DIR *dir = opendir("romfs:/");
    struct dirent *e;
    while (dir && (e = readdir(dir)) != NULL)
        entries += strcmp(e->d_name, ".") && strcmp(e->d_name, "..");
    if (dir)
        closedir(dir);
    snprintf(line, sizeof line, "[+] Entradas em romfs:/: %d\n", entries);
    out(line);

    /* o romfs e somente leitura e confinado */
    int refused = 0;
    FILE *bad = fopen("romfs:/novo.bin", "wb");
    refused += bad == NULL;
    if (bad)
        fclose(bad);
    bad = fopen("romfs:/../hb_assets.c", "rb");
    refused += bad == NULL;
    if (bad)
        fclose(bad);
    snprintf(line, sizeof line, "[+] Acessos recusados no romfs: %d/2\n", refused);
    out(line);

    /* resultado no cartao SD */
    char text[16] = "", back[16] = "";
    snprintf(text, sizeof text, "%08x\n", crc);
    f = fopen(SD_RESULT, "w");
    int sd_ok = f && fputs(text, f) >= 0;
    if (f)
        fclose(f);
    f = fopen(SD_RESULT, "r");
    sd_ok = sd_ok && f && fgets(back, sizeof back, f) && strcmp(back, text) == 0;
    if (f)
        fclose(f);
    sd_ok = sd_ok && remove(SD_RESULT) == 0 && stat(SD_RESULT, &st) != 0;
    snprintf(line, sizeof line, "[+] sdmc:/ gravar/ler/apagar: %s\n", sd_ok ? "ok" : "falhou");
    out(line);

    romfsExit();
    fsdevUnmountAll();
    fsExit();
    smExit();
    return 0;
}
