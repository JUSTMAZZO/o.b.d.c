#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "switch_hle_shim.h"

typedef struct { uint32_t f0; uint32_t f4; } buf_x19_t;  /* layout recuperado de ld2/st2 */

int main(void) {
  buf_x19_t *buf_x19;
  void *p0;
  uint32_t w20;
  uint32_t w21;
  uint32_t w23;
  uint64_t x20;
  uint64_t x22;
  uint64_t x22_2;
  uint64_t x24;
  uint64_t x25;
  PadState sp_0x10;  /* objeto local na pilha (sp+0x10) */
  uint32_t boost;
  uint64_t checksum;

  consoleInit(0);
  w20 = 1;
  padConfigureInput(1, 31);
  padInitializeDefault(&sp_0x10);
  puts("[+] Switch AoS Bench (ld2/st2, libnx HLE shim) iniciado no Android!");
  p0 = malloc(800000);
  if (p0 == 0) {
    return w20;
  }
  buf_x19 = (buf_x19_t *)p0;

  // LOOP_NEON_1
  for (uint32_t i = 0; i < 100000; i++) {
    buf_x19[i].f0 = (i * 3);
    buf_x19[i].f4 = (i & 0xff);
  }

  x20 = armGetSystemTick();
  w21 = 0;
  while (appletMainLoop()) {
    padUpdate(&sp_0x10);
    x22 = padGetButtonsDown(&sp_0x10);
    boost = (((x22 & 1) != 0) ? 65 : 1);

    // LOOP_NEON_2
    for (uint32_t i = 0; i < 100000; i++) {
      buf_x19[i].f4 = (((buf_x19[i].f4 + boost) & 0xffff) ^ (i >> 2));
      buf_x19[i].f0 = ((buf_x19[i].f0 - (buf_x19[i].f4 >> 3)) + (buf_x19[i].f4 << 1));
    }

    w21 = (w21 + 1);
    consoleUpdate(0);
    if ((x22 & 0x400) != 0) break;
  }

  x22_2 = armGetSystemTick();
  checksum = 0;

  // LOOP_NEON_3
  for (uint32_t i = 0; i < 99992; i++) {
    checksum += buf_x19[i].f0;
  }

  w23 = buf_x19[99999].f0;
  x25 = (((((((uint64_t)buf_x19[99993].f0 + (uint64_t)buf_x19[99994].f0) + (uint64_t)buf_x19[99995].f0) + (uint64_t)buf_x19[99996].f0) + (uint64_t)buf_x19[99997].f0) + (uint64_t)buf_x19[99998].f0) + (uint64_t)w23);
  x24 = (checksum + (uint64_t)buf_x19[99992].f0);
  printf("[+] Frames: %u\n", (unsigned int)w21);
  printf("[+] Checksum: %llu\n", (unsigned long long)(x24 + x25));
  printf("[+] Ultima particula: %u\n", (unsigned int)w23);
  printf("[+] Tempo (60 frames): %.4f ms\n", ((double)(uint64_t)(armTicksToNs((x22_2 - x20))) / 1000000.0));
  free(buf_x19);
  consoleExit(0);
  w20 = 0;
  return w20;
}
