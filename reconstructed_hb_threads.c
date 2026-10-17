#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "switch_hle_shim.h"

static uint64_t out(void *a0) {
  void *p19;

  p19 = a0;
  return svcOutputDebugString(p19, strlen(a0));
}

int main(void) {
  uint8_t *b19;
  uint8_t *b20;
  uint8_t *b20_2;
  uint8_t *b21;
  uint32_t *buf_x0;
  uint32_t *buf_x19;
  void *p0;
  uint32_t w0;
  uint32_t w0_2;
  uint32_t w22;
  uint32_t w24;
  uint64_t x19;
  uint64_t x21;
  char sp_0x60[352];  /* objeto local na pilha (sp+0x60), passado por endereco */
  char sp_0x80[320];  /* objeto local na pilha (sp+0x80), passado por endereco */
  char sp_0xe0[224];  /* objeto local na pilha (sp+0xe0), passado por endereco */

  out("[+] hb_threads iniciado\n");
  buf_x19 = (uint32_t *)malloc(400000);
  *(uint64_t *)(0x117ec0) = buf_x19;
  buf_x0 = (uint32_t *)malloc(400000);
  *(uint64_t *)(0x117eb8) = buf_x0;
  if ((buf_x0 == 0) || (buf_x19 == 0)) {
    out("[!] malloc falhou\n");
    w0_2 = 1;
    return w0_2;
  }
  for (uint32_t i = 0; i < 100000u; i++) {
    buf_x0[i] = (i & 0xff);
    buf_x19[i] = ((i << 1) + i);
  }
  b19 = (uint8_t *)sp_0xe0;
  *(uint32_t *)(0x117ea8) = 0;
  *(uint32_t *)(0x117eb0) = 0;
  b20 = (uint8_t *)sp_0x60;
  b21 = b19;
  w22 = 0;
  w0 = 0;
  for (;;) {
    w24 = (w0 + 25000);
    *(uint32_t *)((uint8_t *)b20) = w0;
    *(uint32_t *)((uint8_t *)b20 + 4) = ((w22 != 3) ? w24 : 100000);
    if (threadCreate(b21, 0x1004f0, b20, 0, 65536, 44, 0xfffffffe) != 0) break;
    if (threadStart(b21) != 0) break;
    w22 = (w22 + 1);
    w0 = w24;
    b20 = ((uint8_t *)b20 + 8);
    b21 = ((uint8_t *)b21 + 56);
    if (w22 == 4) break;
  }
  svcSleepThread(1000000);
  x21 = armGetSystemTick();
  mutexLock(0x117ea8);
  *(uint32_t *)(0x117eac) = 1;
  svcSignalProcessWideKey(0x117eb0, 0xffffffff);
  b20_2 = ((uint8_t *)b19 + 224);
  mutexUnlock(0x117ea8);
  for (;;) {
    threadWaitForExit(b19);
    p0 = b19;
    b19 = ((uint8_t *)b19 + 56);
    threadClose(p0);
    if (b20_2 == b19) break;
  }
  x19 = armGetSystemTick();
  snprintf(sp_0x80, 96, "[+] Frames: %u\n", (unsigned int)(60));
  out(sp_0x80);
  snprintf(sp_0x80, 96, "[+] Threads: %u\n", (unsigned int)(*(uint32_t *)(0x117ed0)));
  out(sp_0x80);
  snprintf(sp_0x80, 96, "[+] Checksum: %llu\n", (unsigned long long)(*(uint64_t *)(0x117ec8)));
  out(sp_0x80);
  snprintf(sp_0x80, 96, "[+] Ultima particula: %u\n", (unsigned int)(*(uint32_t *)((uint8_t *)(*(uint64_t *)(0x117ec0) + 393216) + 6780)));
  out(sp_0x80);
  snprintf(sp_0x80, 96, "[+] Tempo (60 frames): %.4f ms\n", ((double)(int64_t)(((uint64_t)(((unsigned __int128)(((x19 - x21) * 625)) * (0xaaaaaaaaaaaaaaab)) >> 64) >> 3)) / 1000000.0));
  out(sp_0x80);
  free(*(uint64_t *)(0x117ec0));
  free(*(uint64_t *)(0x117eb8));
  w0_2 = 0;
  return w0_2;
  out("[!] threadCreate/threadStart falhou\n");
  w0_2 = 1;
  return w0_2;
}
