/*
 * hb_threads.c - homebrew multi-thread no padrao libnx (Fase 3): threads, Mutex, CondVar e sleep.
 *
 * 4 threads da libnx (threadCreate/threadStart) simulam cada uma uma fatia das 100.000 particulas
 * (mesma fisica do hb_compute: 60 frames, impulso no frame 30). Elas esperam num portao de largada
 * (CondVar + Mutex) que o main abre depois de um svcSleepThread, e somam o checksum parcial sob o
 * Mutex. Como cada particula e independente, o total e o mesmo do hb_compute: 88196450180.
 *
 * Switch (devkitA64 + libnx, flags do template oficial):
 *   ARCH="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE"
 *   aarch64-none-elf-gcc $ARCH -g -Wall -O2 -ffunction-sections -D__SWITCH__ -I $DEVKITPRO/libnx/include -c hb_threads.c
 *   aarch64-none-elf-gcc -specs=$DEVKITPRO/libnx/switch.specs -g $ARCH hb_threads.o -L $DEVKITPRO/libnx/lib -lnx -o hb_threads.elf
 *   elf2nro hb_threads.elf hb_threads.nro
 * Oraculo: -DOBDC_ORACLE -I ../.. com o switch_hle_shim.h (threads sobre pthread/futex no Bionic).
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
#define WORKERS 4

static uint32_t *pos_x, *vel_x;
static Mutex g_lock;
static CondVar g_start;
static int g_go;
static uint64_t g_total;
static uint32_t g_done;

typedef struct { uint32_t begin, end; } Slice;

static void out(const char *s) { svcOutputDebugString(s, strlen(s)); }

static void worker(void *arg)
{
    const Slice *sl = arg;

    mutexLock(&g_lock);                 /* portao de largada */
    while (!g_go)
        condvarWait(&g_start, &g_lock);
    mutexUnlock(&g_lock);

    for (uint32_t frame = 1; frame <= FRAMES; frame++) {
        uint32_t boost = frame == FRAME_IMPULSE ? 65 : 1;
        for (uint32_t i = sl->begin; i < sl->end; i++) {
            vel_x[i] = ((vel_x[i] + boost) ^ (i >> 2)) & 0xFFFF;
            pos_x[i] += (vel_x[i] << 1) - (vel_x[i] >> 3);
        }
    }
    uint64_t part = 0;
    for (uint32_t i = sl->begin; i < sl->end; i++)
        part += pos_x[i];

    mutexLock(&g_lock);
    g_total += part;
    g_done++;
    mutexUnlock(&g_lock);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    char line[96];
    out("[+] hb_threads iniciado\n");

    pos_x = malloc(N * sizeof(uint32_t));
    vel_x = malloc(N * sizeof(uint32_t));
    if (!pos_x || !vel_x) {
        out("[!] malloc falhou\n");
        return 1;
    }
    for (uint32_t i = 0; i < N; i++) {
        pos_x[i] = i * 3;
        vel_x[i] = i & 0xFF;
    }

    mutexInit(&g_lock);
    condvarInit(&g_start);
    Thread th[WORKERS];
    Slice sl[WORKERS];
    for (int k = 0; k < WORKERS; k++) {
        sl[k].begin = (uint32_t)(N / WORKERS * k);
        sl[k].end = k == WORKERS - 1 ? N : (uint32_t)(N / WORKERS * (k + 1));
        if (R_FAILED(threadCreate(&th[k], worker, &sl[k], NULL, 0x10000, 0x2C, -2)) ||
            R_FAILED(threadStart(&th[k]))) {
            out("[!] threadCreate/threadStart falhou\n");
            return 1;
        }
    }

    svcSleepThread(1000000LL);          /* 1 ms: as threads ficam paradas no portao */
    u64 start = armGetSystemTick();
    mutexLock(&g_lock);
    g_go = 1;
    condvarWakeAll(&g_start);
    mutexUnlock(&g_lock);

    for (int k = 0; k < WORKERS; k++) {
        threadWaitForExit(&th[k]);
        threadClose(&th[k]);
    }
    u64 end = armGetSystemTick();

    snprintf(line, sizeof line, "[+] Frames: %u\n", FRAMES);
    out(line);
    snprintf(line, sizeof line, "[+] Threads: %u\n", g_done);
    out(line);
    snprintf(line, sizeof line, "[+] Checksum: %llu\n", (unsigned long long)g_total);
    out(line);
    snprintf(line, sizeof line, "[+] Ultima particula: %u\n", pos_x[N - 1]);
    out(line);
    snprintf(line, sizeof line, "[+] Tempo (60 frames): %.4f ms\n", armTicksToNs(end - start) / 1e6);
    out(line);

    free(pos_x);
    free(vel_x);
    return 0;
}
