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

static uint64_t open_service(void *a0, uint32_t a1) {
  void *p3;
  uint32_t w4;
  uint32_t sp_0x1c;
  char sp_0x20[96];  /* objeto local na pilha (sp+0x20), passado por endereco */

  w4 = a1;
  p3 = a0;
  sp_0x1c = w4;
  snprintf(sp_0x20, 96, "[+] %s: 0x%x\n", (const char *)(p3), (unsigned int)(w4));
  out(sp_0x20);
  return ((sp_0x1c == 0) ? 1 : 0);
}

int main(void) {
  uint32_t w19;
  uint32_t w20;
  uint32_t w21;
  uint32_t w21_2;
  uint32_t w21_3;
  uint32_t w21_4;
  PadState sp_0x38;  /* objeto local na pilha (sp+0x38) */
  char sp_0x70[96];  /* objeto local na pilha (sp+0x70), passado por endereco */

  w20 = 0;
  w19 = 0;
  out("[+] hb_services iniciado\n");
  w21 = open_service("smInitialize", smInitialize());
  w21_2 = (w21 + (uint32_t)(open_service("appletInitialize", appletInitialize())));
  w21_3 = (w21_2 + (uint32_t)(open_service("hidInitialize", hidInitialize())));
  w21_4 = (w21_3 + (uint32_t)(open_service("fsInitialize", fsInitialize())));
  padConfigureInput(1, 31);
  padInitializeWithMask(&sp_0x38, 0x100000001);
  while (appletMainLoop()) {
    if (!appletMainLoop()) break;
    if (w19 == 60) break;
    w19 = (w19 + 1);
    padUpdate(&sp_0x38);
    w20 = (((*(uint64_t *)((uint8_t *)&sp_0x38 + 16) & (~*(uint64_t *)((uint8_t *)&sp_0x38 + 24))) != 0) ? (w20 + 1) : w20);
  }
  snprintf(sp_0x70, 96, "[+] Servicos abertos: %d\n", (int)(w21_4));
  out(sp_0x70);
  snprintf(sp_0x70, 96, "[+] Frames: %u\n", (unsigned int)(w19));
  out(sp_0x70);
  snprintf(sp_0x70, 96, "[+] Botoes pressionados: %u\n", (unsigned int)(w20));
  out(sp_0x70);
  fsExit();
  hidExit();
  appletExit();
  smExit();
  return 0;
}
