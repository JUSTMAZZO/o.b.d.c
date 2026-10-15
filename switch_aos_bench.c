/*
 * switch_aos_bench.c - homebrew no padrao libnx com Array of Structs (AoS), para que o
 * Clang -O2 vetorize com ld2/st2 (campos intercalados). Fase 2 do o.b.d.c.
 *
 * Build: aarch64-linux-android35-clang -O2 switch_aos_bench.c -o switch_aos_bench
 */
#include <stdlib.h>
#include "switch_hle_shim.h"

#define N 100000
#define BOOST_BASE    1u
#define BOOST_IMPULSE 64u

typedef struct {
    uint32_t x;
    uint32_t vx;
} Particle;

int main(void)
{
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);

    PadState pad;
    padInitializeDefault(&pad);

    puts("[+] Switch AoS Bench (ld2/st2, libnx HLE shim) iniciado no Android!");

    Particle *p = malloc(N * sizeof(Particle));
    if (!p)
        return 1;

    for (uint32_t i = 0; i < N; i++) {
        p[i].x = i * 3;
        p[i].vx = i & 0xFF;
    }

    u32 frames = 0;
    u64 start_tick = armGetSystemTick();

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);

        uint32_t boost = BOOST_BASE;
        if (kDown & HidNpadButton_A)
            boost += BOOST_IMPULSE;

        for (uint32_t i = 0; i < N; i++) {
            p[i].vx = ((p[i].vx + boost) ^ (i >> 2)) & 0xFFFF;
            p[i].x += (p[i].vx << 1) - (p[i].vx >> 3);
        }
        frames++;

        consoleUpdate(NULL);
        if (kDown & HidNpadButton_Plus)
            break;
    }

    u64 end_tick = armGetSystemTick();

    uint64_t checksum = 0;
    for (uint32_t i = 0; i < N; i++)
        checksum += p[i].x;

    printf("[+] Frames: %u\n", frames);
    printf("[+] Checksum: %llu\n", (unsigned long long)checksum);
    printf("[+] Ultima particula: %u\n", p[N - 1].x);
    printf("[+] Tempo (60 frames): %.4f ms\n", armTicksToNs(end_tick - start_tick) / 1e6);

    free(p);
    consoleExit(NULL);
    return 0;
}
