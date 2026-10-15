#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "switch_hle_shim.h"

int main(void) {
  uint32_t *buf_x19;
  uint32_t *buf_x20;
  void *p0;
  uint32_t w21;
  uint32_t w22;
  uint64_t x21;
  uint64_t x23;
  uint64_t x23_2;
  uint8_t sp_0x10[32];  /* objeto local na pilha (sp+0x10), passado por endereco */
  uint32_t boost;
  uint64_t checksum;

  consoleInit(0);
  w21 = 1;
  padConfigureInput(1, 31);
  padInitializeDefault(sp_0x10);
  puts("[+] Switch Particle Bench (libnx HLE shim) iniciado no Android!");
  buf_x19 = (uint32_t *)malloc(400000);
  p0 = malloc(400000);
  if (buf_x19 == 0) {
    return w21;
  }
  buf_x20 = (uint32_t *)p0;
  if (p0 == 0) {
    return w21;
  }

  // LOOP_NEON_1
  for (uint32_t i = 0; i < 100000; i++) {
    buf_x19[i] = (i * 3);
    buf_x20[i] = (i & 0xff);
  }

  x21 = armGetSystemTick();
  w22 = 0;
  while (appletMainLoop()) {
    padUpdate(sp_0x10);
    x23 = padGetButtonsDown(sp_0x10);
    boost = (((x23 & 1) != 0) ? 65 : 1);

    // LOOP_NEON_2
    for (uint32_t i = 0; i < 100000; i++) {
      buf_x20[i] = (((buf_x20[i] + boost) & 0xffff) ^ (i >> 2));
      buf_x19[i] = ((buf_x19[i] - (buf_x20[i] >> 3)) + (buf_x20[i] << 1));
    }

    w22 = (w22 + 1);
    consoleUpdate(0);
    if ((x23 & 0x400) != 0) break;
  }

  x23_2 = armGetSystemTick();
  checksum = 0;

  // LOOP_NEON_3
  for (uint32_t i = 0; i < 100000; i++) {
    checksum += buf_x19[i];
  }

  printf("[+] Frames: %u\n", (unsigned int)w22);
  printf("[+] Checksum: %llu\n", (unsigned long long)checksum);
  printf("[+] Ultima particula: %u\n", (unsigned int)buf_x19[99999]);
  printf("[+] Tempo (60 frames): %.4f ms\n", ((double)(uint64_t)(armTicksToNs((x23_2 - x21))) / 1000000.0));
  free(buf_x19);
  free(buf_x20);
  consoleExit(0);
  w21 = 0;
  return w21;
}
