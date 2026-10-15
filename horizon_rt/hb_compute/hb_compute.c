/*
 * hb_compute.c - homebrew minimo do Switch para o primeiro marco da Fase 3.
 *
 * Mesma fisica do switch_particle_bench (100.000 particulas x 60 frames, impulso no frame 30),
 * sem pad e sem console: a saida vai por svcOutputDebugString, no formato "[+] Rotulo: valor"
 * que o gate do o.b.d.c compara. __appInit/__appExit vazios: a libnx nao abre nenhum servico.
 *
 * Switch (devkitA64 16.1.0 + libnx 2.10.0, flags do template oficial):
 *   ARCH="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE"
 *   aarch64-none-elf-gcc $ARCH -g -Wall -O2 -ffunction-sections -D__SWITCH__ -I $DEVKITPRO/libnx/include -c hb_compute.c
 *   aarch64-none-elf-gcc -specs=$DEVKITPRO/libnx/switch.specs -g $ARCH hb_compute.o -L $DEVKITPRO/libnx/lib -lnx -o hb_compute.elf
 *   elf2nro hb_compute.elf hb_compute.nro
 * Oraculo: o mesmo arquivo compilado com -DOBDC_ORACLE -I .. usa o switch_hle_shim.h
 *   (Android via NDK ou host via llvm-mingw): Frames 60, Checksum 88196450180, Ultima particula 1734747.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef OBDC_ORACLE
#include "switch_hle_shim.h"
#else
#include <switch.h>

void __appInit(void) {}
void __appExit(void) {}
#endif

#define N 100000
#define FRAMES 60
#define FRAME_IMPULSE 30

static void out(const char *s) { svcOutputDebugString(s, strlen(s)); }

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    char line[96];
    out("[+] hb_compute iniciado\n");

    uint32_t *pos_x = malloc(N * sizeof(uint32_t));
    uint32_t *vel_x = malloc(N * sizeof(uint32_t));
    if (!pos_x || !vel_x) {
        out("[!] malloc falhou\n");
        return 1;
    }
    for (uint32_t i = 0; i < N; i++) {
        pos_x[i] = i * 3;
        vel_x[i] = i & 0xFF;
    }

    u64 start = armGetSystemTick();
    for (uint32_t frame = 1; frame <= FRAMES; frame++) {
        uint32_t boost = frame == FRAME_IMPULSE ? 65 : 1;
        for (uint32_t i = 0; i < N; i++) {
            vel_x[i] = ((vel_x[i] + boost) ^ (i >> 2)) & 0xFFFF;
            pos_x[i] += (vel_x[i] << 1) - (vel_x[i] >> 3);
        }
    }
    u64 end = armGetSystemTick();

    uint64_t checksum = 0;
    for (uint32_t i = 0; i < N; i++)
        checksum += pos_x[i];

    snprintf(line, sizeof line, "[+] Frames: %u\n", FRAMES);
    out(line);
    snprintf(line, sizeof line, "[+] Checksum: %llu\n", (unsigned long long)checksum);
    out(line);
    snprintf(line, sizeof line, "[+] Ultima particula: %u\n", pos_x[N - 1]);
    out(line);
    snprintf(line, sizeof line, "[+] Tempo (60 frames): %.4f ms\n", armTicksToNs(end - start) / 1e6);
    out(line);

    free(pos_x);
    free(vel_x);
    return 0;
}
