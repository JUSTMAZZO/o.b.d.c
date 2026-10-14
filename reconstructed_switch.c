#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "switch_hle_shim.h"

int main(void) {
  PadState pad;
  consoleInit(NULL);
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&pad);
  puts("[+] Switch Particle Bench (libnx HLE shim) iniciado no Android!");
  uint32_t *pos_x = (uint32_t *)malloc(100000 * sizeof(uint32_t));
  uint32_t *vel_x = (uint32_t *)malloc(100000 * sizeof(uint32_t));
  if (!pos_x || !vel_x) return 1;

  // LOOP_NEON_1
  for (uint32_t i = 0; i < 100000; i++) {
    pos_x[i] = (i * 3);
    vel_x[i] = (i & 0xff);
  }

  u64 start_tick = armGetSystemTick();
  uint32_t frames = 0;
  while (appletMainLoop()) {
    padUpdate(&pad);
    u64 kDown = padGetButtonsDown(&pad);
    uint32_t boost = (kDown & HidNpadButton_A) ? 65 : 1;

    // LOOP_NEON_2
    for (uint32_t i = 0; i < 100000; i++) {
      vel_x[i] = (((vel_x[i] + boost) & 0xffff) ^ (i >> 2));
      pos_x[i] = ((pos_x[i] - (vel_x[i] >> 3)) + (vel_x[i] << 1));
    }

    frames++;
    consoleUpdate(NULL);
    if (kDown & HidNpadButton_Plus) break;
  }

  u64 end_tick = armGetSystemTick();
  uint64_t checksum = 0;

  // LOOP_NEON_3
  for (uint32_t i = 0; i < 100000; i++) {
    checksum += pos_x[i];
  }

  printf("[+] Frames: %u\n", frames);
  printf("[+] Checksum: %llu\n", (unsigned long long)checksum);
  printf("[+] Ultima particula: %u\n", pos_x[100000 - 1]);
  printf("[+] Tempo (60 frames): %.4f ms\n", (double)armTicksToNs(end_tick - start_tick) / 1000000.0);

  free(pos_x);
  free(vel_x);
  consoleExit(NULL);
  return 0;
}
