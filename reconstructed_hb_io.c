#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "switch_hle_shim.h"

static uint64_t step(void *a0, uint32_t a1) {
  void *p3;
  uint32_t w4;
  uint32_t sp_0x1c;
  char sp_0x20[128];  /* objeto local na pilha (sp+0x20), passado por endereco */

  w4 = a1;
  if (a1 != 0) {
    p3 = a0;
    sp_0x1c = w4;
    snprintf(sp_0x20, 128, "[!] %s: 0x%x\n", (const char *)(p3), (unsigned int)(w4));
    svcOutputDebugString(sp_0x20, strlen(sp_0x20));
    return ((sp_0x1c == 0) ? 1 : 0);
  } else {
    return ((a1 == 0) ? 1 : 0);
  }
}

static uint64_t out(void *a0) {
  void *p19;

  p19 = a0;
  return svcOutputDebugString(p19, strlen(a0));
}

int main(void) {
  uint32_t w2;
  uint32_t w20;
  uint32_t w3;
  uint32_t w4;
  uint32_t w4_2;
  uint32_t w5;
  uint32_t w5_2;
  uint32_t w8;
  uint64_t x0;
  uint64_t x0_10;
  uint64_t x0_11;
  uint64_t x0_12;
  uint64_t x0_13;
  uint64_t x0_14;
  uint64_t x0_2;
  uint64_t x0_3;
  uint64_t x0_4;
  uint64_t x0_5;
  uint64_t x0_6;
  uint64_t x0_7;
  uint64_t x0_8;
  uint64_t x0_9;
  uint64_t x1;
  uint64_t x19;
  uint64_t x19_2;
  uint64_t x1_2;
  uint64_t x1_3;
  uint64_t x2;
  uint64_t x21;
  uint64_t x22;
  uint64_t x2_2;
  uint64_t x6;
  uint64_t x6_2;
  uint64_t x7;
  uint64_t x7_2;
  uint64_t x7_3;
  uint64_t sp_0x38;
  uint64_t sp_0x40;
  uint32_t sp_0x4c;
  int64_t sp_0x60;
  uint64_t sp_0x68;
  int64_t sp_0x70;
  char sp_0x5c[4];  /* objeto local na pilha (sp+0x5c), passado por endereco */
  FsFileSystem sp_0x78;  /* objeto local na pilha (sp+0x78) */
  FsFile sp_0x88;  /* objeto local na pilha (sp+0x88) */
  FsDir sp_0x98;  /* objeto local na pilha (sp+0x98) */
  PadState sp_0xa8;  /* objeto local na pilha (sp+0xa8) */
  char sp_0xe0[128];  /* objeto local na pilha (sp+0xe0), passado por endereco */
  uint32_t dup_v27;

  svcOutputDebugString("[+] hb_io iniciado\n", 19);
  x0 = smInitialize();
  if (x0 != 0) {
    snprintf(sp_0xe0, 128, "[!] %s: 0x%x\n", (const char *)("smInitialize"), (unsigned int)(x0));
    svcOutputDebugString(sp_0xe0, strlen(sp_0xe0));
    w5 = 1;
    return w5;
  }
  x0_2 = step("appletInitialize", appletInitialize());
  if (x0_2 == 0) {
    w5 = 1;
    return w5;
  }
  sp_0x38 = (uint32_t)(x0_2);
  x0_3 = step("hidInitialize", hidInitialize());
  w5 = sp_0x38;
  if (x0_3 == 0) {
    return w5;
  }
  sp_0x38 = (uint32_t)(x0_3);
  x0_4 = step("fsInitialize", fsInitialize());
  if (x0_4 == 0) {
    w5 = sp_0x38;
    return w5;
  }
  sp_0x38 = (uint32_t)(x0_4);
  x0_5 = fsOpenSdCardFileSystem(&sp_0x78);
  w20 = x0_5;
  snprintf(sp_0xe0, 128, "[+] fsOpenSdCardFileSystem: 0x%x\n", (unsigned int)(x0_5));
  out(sp_0xe0);
  w4 = sp_0x38;
  if (x0_5 != 0) {
    w5 = w4;
    return w5;
  }
  sp_0x40 = w4;
  sp_0x38 = (uintptr_t)"/obdc_hb_io";
  fsFsDeleteDirectoryRecursively(&sp_0x78, (const char *)((uintptr_t)("/obdc_hb_io")));
  x0_6 = step("fsFsCreateDirectory", fsFsCreateDirectory(&sp_0x78, (const char *)((uintptr_t)(sp_0x38))));
  w4 = sp_0x40;
  if (x0_6 == 0) {
    w5 = w4;
    return w5;
  }
  sp_0x38 = (uint32_t)(x0_6);
  sp_0x40 = (uintptr_t)"/obdc_hb_io/dados.bin";
  x0_7 = step("fsFsCreateFile", fsFsCreateFile(&sp_0x78, (const char *)((uintptr_t)("/obdc_hb_io/dados.bin")), 0, 0));
  w5 = sp_0x38;
  if (x0_7 == 0) {
    return w5;
  }
  sp_0x38 = (uint32_t)(x0_7);
  x0_8 = step("fsFsOpenFile(w)", fsFsOpenFile(&sp_0x78, (const char *)((uintptr_t)(sp_0x40)), 6, &sp_0x88));
  w5 = x0_8;
  if (x0_8 == 0) {
    w5 = sp_0x38;
    return w5;
  }
  x6 = 0;
  x7 = 0x11e000;
  for (;;) {
    dup_v27 = x6;
    for (uint32_t k = 0; k < 4096; k++) {
      ((uint8_t *)0x11eed0)[k] = (uint8_t)(((dup_v27 + k) * 131u + 7u) ^ ((dup_v27 + k) >> 7));
    }
    x1 = x6;
    sp_0x38 = w5;
    x2 = (x7 + 3792);
    sp_0x40 = x6;
    x19 = (x7 + 3792);
    w4_2 = step("fsFileWrite", fsFileWrite(&sp_0x88, x1, (const void *)((uintptr_t)(x2)), 4096, 0));
    w5 = sp_0x38;
    if (w4_2 == 0) break;
    x7 = 0x11e000;
    x6 = (sp_0x40 + 4096);
    if (x6 == 0x100000) break;
  }
  sp_0x38 = w4_2;
  step("fsFileFlush", fsFileFlush(&sp_0x88));
  fsFileClose(&sp_0x88);
  sp_0x60 = 0;
  sp_0x68 = 0;
  x0_9 = step("fsFsOpenFile(r)", fsFsOpenFile(&sp_0x78, (const char *)((uintptr_t)("/obdc_hb_io/dados.bin")), 1, &sp_0x88));
  if (x0_9 == 0) {
    w5 = sp_0x38;
    return w5;
  }
  sp_0x38 = (uint32_t)(x0_9);
  x0_10 = step("fsFileGetSize", fsFileGetSize(&sp_0x88, &sp_0x60));
  w5 = sp_0x38;
  w8 = x0_10;
  if (x0_10 == 0) {
    return w5;
  }
  x6_2 = 0xcbf29ce484222325;
  x7_2 = 0;
  for (;;) {
    x1_2 = x7_2;
    sp_0x38 = x7_2;
    sp_0x40 = x6_2;
    x2_2 = x19;
    sp_0x4c = w8;
    w5_2 = step("fsFileRead", fsFileRead(&sp_0x88, x1_2, (void *)((uintptr_t)(x2_2)), 4096, 0, &sp_0x68));
    w8 = sp_0x4c;
    if (w5_2 == 0) {
      w5 = w8;
      return w5;
    }
    x7_3 = sp_0x38;
    x6_2 = sp_0x40;
    x0_11 = 0;
    while (sp_0x68 != x0_11) {
      w2 = *(uint8_t *)((uint8_t *)x19 + x0_11);
      x0_11 = (x0_11 + 1);
      x6_2 = (((uint64_t)(w2) ^ x6_2) * 0x100000001b3);
    }
    x7_2 = (x7_3 + sp_0x68);
    if (sp_0x68 != 4096) break;
  }
  sp_0x38 = x6_2;
  sp_0x40 = x7_2;
  sp_0x4c = w5_2;
  fsFileClose(&sp_0x88);
  snprintf(sp_0xe0, 128, "[+] Arquivo: %lld bytes, lidos %llu\n", (long long)(sp_0x60), (unsigned long long)(sp_0x40));
  out(sp_0xe0);
  snprintf(sp_0xe0, 128, "[+] FNV-1a: 0x%016llx\n", (unsigned long long)(sp_0x38));
  out(sp_0xe0);
  snprintf(sp_0xe0, 128, "%016llx\n", (unsigned long long)(sp_0x38));
  x0_12 = step("fsFsCreateFile(txt)", fsFsCreateFile(&sp_0x78, (const char *)((uintptr_t)("/obdc_hb_io/resultado.tmp")), 0, 0));
  w5 = sp_0x4c;
  if (x0_12 == 0) {
    return w5;
  }
  sp_0x38 = (uint32_t)(x0_12);
  x0_13 = step("fsFsOpenFile(txt)", fsFsOpenFile(&sp_0x78, (const char *)((uintptr_t)("/obdc_hb_io/resultado.tmp")), 6, &sp_0x88));
  if (x0_13 == 0) {
    w5 = sp_0x38;
    return w5;
  }
  sp_0x38 = (uint32_t)(x0_13);
  w5 = sp_0x38;
  if (step("fsFileWrite(txt)", fsFileWrite(&sp_0x88, 0, sp_0xe0, 17, 1)) == 0) {
    return w5;
  }
  fsFileClose(&sp_0x88);
  step("fsFsRenameFile", fsFsRenameFile(&sp_0x78, (const char *)((uintptr_t)("/obdc_hb_io/resultado.tmp")), (const char *)((uintptr_t)("/obdc_hb_io/resultado.txt"))));
  sp_0x70 = 0xffffffffffffffff;
  if (step("fsFsOpenDirectory", fsFsOpenDirectory(&sp_0x78, (const char *)((uintptr_t)("/obdc_hb_io")), 2, &sp_0x98)) != 0) {
    step("fsDirGetEntryCount", fsDirGetEntryCount(&sp_0x98, &sp_0x70));
    fsDirClose(&sp_0x98);
  }
  snprintf(sp_0xe0, 128, "[+] Arquivos em /obdc_hb_io: %lld\n", (long long)(sp_0x70));
  x22 = 0;
  out(sp_0xe0);
  padConfigureInput(1, 31);
  padInitializeWithMask(&sp_0xa8, 0x100000001);
  x21 = 0;
  x19_2 = 0;
  w3 = 0;
  for (;;) {
    sp_0x38 = w3;
    if (!appletMainLoop()) break;
    if (sp_0x38 == 60) break;
    padUpdate(&sp_0xa8);
    x0_14 = *(uint64_t *)((uint8_t *)&sp_0xa8 + 16);
    x1_3 = *(uint64_t *)((uint8_t *)&sp_0xa8 + 24);
    w20 = (((x0_14 & (~x1_3)) != 0) ? (w20 + 1) : w20);
    w3 = (sp_0x38 + 1);
    x19_2 = (x19_2 + padGetStickPos(&sp_0xa8, 0).x);
    x21 = (x21 + padGetStickPos(&sp_0xa8, 0).y);
    x22 = (x22 + padGetStickPos(&sp_0xa8, 1).x);
  }
  snprintf(sp_0xe0, 128, "[+] Frames: %u, botoes pressionados: %u\n", (unsigned int)(sp_0x38), (unsigned int)(w20));
  out(sp_0xe0);
  snprintf(sp_0xe0, 128, "[+] Analogicos (soma): esquerdo x=%lld y=%lld, direito x=%lld\n", (long long)(x19_2), (long long)(x21), (long long)(x22));
  out(sp_0xe0);
  sp_0x40 = (uintptr_t)"/obdc_hb_io";
  sp_0x38 = (uint32_t)(fsFsDeleteDirectoryRecursively(&sp_0x78, (const char *)((uintptr_t)("/obdc_hb_io"))));
  snprintf(sp_0xe0, 128, "[+] Limpeza: 0x%x, %s\n", (unsigned int)(sp_0x38), (const char *)((((uint32_t)(fsFsGetEntryType(&sp_0x78, (const char *)((uintptr_t)(sp_0x40)), (FsDirEntryType *)(sp_0x5c))) == 0) ? "diretorio ainda existe" : "diretorio removido")));
  out(sp_0xe0);
  fsFsClose(&sp_0x78);
  fsExit();
  hidExit();
  appletExit();
  smExit();
  w5 = 0;
  return w5;
}
