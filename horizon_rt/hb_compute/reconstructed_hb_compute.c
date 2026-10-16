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
  uint32_t *buf_x19;
  uint32_t *buf_x20;
  void *p0;
  uint32_t w0;
  uint32_t w0_2;
  uint32_t w2;
  uint64_t x22;
  uint64_t x23;
  char sp_0x40[96];  /* objeto local na pilha (sp+0x40), passado por endereco */
  uint32_t boost;
  uint64_t checksum;

  out("[+] hb_compute iniciado\n");
  buf_x19 = (uint32_t *)malloc(400000);
  p0 = malloc(400000);
  if ((buf_x19 == 0) || (p0 == 0)) {
    out("[!] malloc falhou\n");
    w0_2 = 1;
    return w0_2;
  }
  buf_x20 = (uint32_t *)p0;
  for (uint32_t i = 0; i < 100000u; i++) {
    buf_x20[i] = (i & 0xff);
    buf_x19[i] = ((i << 1) + i);
  }
  x23 = armGetSystemTick();
  w2 = 1;
  w0 = w2;
  for (;;) {
    boost = w0;
    for (uint32_t i = 0; i < 100000u; i++) {
      buf_x20[i] = (((boost + buf_x20[i]) ^ (i >> 2)) & 0xffff);
      buf_x19[i] = (((buf_x20[i] << 1) + buf_x19[i]) - (buf_x20[i] >> 3));
    }
    w2 = (w2 + 1);
    if (w2 == 61) break;
    w0 = ((w2 == 30) ? 65 : 1);
  }
  x22 = armGetSystemTick();
  checksum = 0;
  for (uint32_t i = 0; i < 100000u; i++) {
    checksum += buf_x19[i];
  }
  snprintf(sp_0x40, 96, "[+] Frames: %u\n", (unsigned int)60);
  out(sp_0x40);
  snprintf(sp_0x40, 96, "[+] Checksum: %llu\n", (unsigned long long)checksum);
  out(sp_0x40);
  snprintf(sp_0x40, 96, "[+] Ultima particula: %u\n", (unsigned int)buf_x19[99999]);
  out(sp_0x40);
  snprintf(sp_0x40, 96, "[+] Tempo (60 frames): %.4f ms\n", ((double)(int64_t)(((uint64_t)(((unsigned __int128)(((x22 - x23) * 625)) * (0xaaaaaaaaaaaaaaab)) >> 64) >> 3)) / 1000000.0));
  out(sp_0x40);
  free(buf_x19);
  free(buf_x20);
  w0_2 = 0;
  return w0_2;
}
