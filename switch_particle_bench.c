/*
 * switch_particle_bench.c - homebrew no padrao libnx (fisica de particulas) rodando
 * no Android AArch64 via switch_hle_shim.h. Fase 3 do o.b.d.c.
 *
 * Build: aarch64-linux-android35-clang -O2 switch_particle_bench.c -o switch_particle_bench
 */
#include <stdlib.h>
#include "switch_hle_shim.h"

#define N 100000
#define BOOST_BASE    1u
#define BOOST_IMPULSE 64u

int main(void)
{
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);

    PadState pad;
    padInitializeDefault(&pad);

    puts("[+] Switch Particle Bench (libnx HLE shim) iniciado no Android!");

    uint32_t *pos_x = malloc(N * sizeof(uint32_t));
    uint32_t *vel_x = malloc(N * sizeof(uint32_t));
    if (!pos_x || !vel_x)
        return 1;

    for (uint32_t i = 0; i < N; i++) {
        pos_x[i] = i * 3;
        vel_x[i] = i & 0xFF;
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
            vel_x[i] = ((vel_x[i] + boost) ^ (i >> 2)) & 0xFFFF;
            pos_x[i] += (vel_x[i] << 1) - (vel_x[i] >> 3);
        }
        frames++;

        consoleUpdate(NULL);
        if (kDown & HidNpadButton_Plus)
            break;
    }

    u64 end_tick = armGetSystemTick();

    uint64_t checksum = 0;
    for (uint32_t i = 0; i < N; i++)
        checksum += pos_x[i];

    printf("[+] Frames: %u\n", frames);
    printf("[+] Checksum: %llu\n", (unsigned long long)checksum);
    printf("[+] Ultima particula: %u\n", pos_x[N - 1]);
    printf("[+] Tempo (60 frames): %.4f ms\n", armTicksToNs(end_tick - start_tick) / 1e6);

    free(pos_x);
    free(vel_x);
    consoleExit(NULL);
    return 0;
}
