#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
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
  void *p0;
  void *p0_2;
  void *p0_3;
  void *p0_4;
  void *p0_5;
  void *p0_6;
  void *p0_7;
  void *p0_8;
  void *p0_9;
  void *p20;
  void *p24;
  void *p3;
  uint32_t w1;
  uint32_t w19;
  uint32_t w20;
  uint32_t w20_2;
  uint32_t w20_3;
  uint32_t w21;
  uint32_t w22;
  uint32_t w25;
  uint32_t w3;
  uint32_t w4;
  uint64_t x0;
  uint64_t x0_2;
  uint64_t x0_3;
  uint64_t x0_4;
  uint64_t x0_5;
  uint64_t x0_6;
  uint64_t x0_7;
  uint64_t x0_8;
  uint64_t x2;
  uint64_t x20;
  uint64_t x22;
  uint64_t x23;
  uint64_t x26;
  uint64_t x27;
  const char *x3;
  const char *x3_2;
  uint64_t x6;
  uint64_t x7;
  uint64_t sp_0x68;
  uint32_t sp_0x74;
  uint64_t sp_0x78;
  char sp_0x80[8];  /* objeto local na pilha (sp+0x80), passado por endereco */
  char sp_0x88[16];  /* objeto local na pilha (sp+0x88), passado por endereco */
  char sp_0x98[16];  /* objeto local na pilha (sp+0x98), passado por endereco */
  char sp_0xa8[64];  /* objeto local na pilha (sp+0xa8), passado por endereco */
  struct stat sp_0xe8;  /* objeto local na pilha (sp+0xe8) */
  char sp_0x150[128];  /* objeto local na pilha (sp+0x150), passado por endereco */

  w19 = 1;
  out("[+] hb_assets iniciado\n");
  x0 = open_service("smInitialize", smInitialize());
  if (x0 == 0) {
    return w19;
  }
  w19 = x0;
  x0_2 = open_service("fsInitialize", fsInitialize());
  if (x0_2 == 0) {
    return w19;
  }
  x0_3 = open_service("fsdevMountSdmc", fsdevMountSdmc());
  w19 = x0_3;
  if (x0_3 == 0) {
    w19 = x0_2;
    return w19;
  }
  x0_4 = open_service("romfsInit", romfsInit());
  if (x0_4 == 0) {
    return w19;
  }
  sp_0x68 = (uintptr_t)"romfs:/data/bloco.bin";
  x0_5 = stat((const char *)((uintptr_t)("romfs:/data/bloco.bin")), (struct stat *)(&sp_0xe8));
  w19 = x0_5;
  if (x0_5 != 0) {
    w19 = x0_4;
    out("[!] stat(romfs:/data/bloco.bin) falhou\n");
    return w19;
  }
  p0 = fopen((const char *)((uintptr_t)(sp_0x68)), (const char *)((uintptr_t)("rb")));
  if (p0 == 0) {
    out("[!] fopen(romfs:/data/bloco.bin) falhou\n");
    w19 = x0_4;
    return w19;
  }
  for (uint32_t n = 0; n < 256; n++) {
    uint32_t c = n;
    for (int k = 0; k < 8; k++)
      c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    ((uint32_t *)0x120ed0)[n] = c;
  }
  x20 = 0xcbf29ce484222325;
  x6 = 0x1212d0;
  x26 = armGetSystemTick();
  x23 = 0;
  w4 = 0xffffffff;
  x7 = 0x120ed0;
  for (;;) {
    x0_6 = x6;
    p3 = p0;
    sp_0x68 = x6;
    sp_0x74 = w4;
    sp_0x78 = x7;
    x0_7 = fread((void *)((uintptr_t)(x0_6)), 1, 4096, (FILE *)((uintptr_t)(p3)));
    x6 = sp_0x68;
    x7 = sp_0x78;
    w4 = sp_0x74;
    if (x0_7 == 0) break;
    x2 = 0;
    for (;;) {
      w1 = *(uint8_t *)((uint8_t *)x6 + x2);
      x2 = (x2 + 1);
      x20 = (((uint64_t)(w1) ^ x20) * 0x100000001b3);
      w4 = (*(uint32_t *)((uint8_t *)x7 + (((uint64_t)((w1 ^ w4)) & 0xff) << 2)) ^ (w4 >> 8));
      if (x0_7 == x2) break;
    }
    x23 = (x23 + x0_7);
  }
  x27 = armGetSystemTick();
  *(uint32_t *)sp_0x80 = 0;
  w25 = (~w4);
  if (fseek((FILE *)((uintptr_t)(p0)), 65536, 0) == 0) {
    x22 = fread(sp_0x80, 1, 4, (FILE *)((uintptr_t)(p0)));
    fclose((FILE *)((uintptr_t)(p0)));
    p24 = sp_0x150;
    snprintf(sp_0x150, 128, "[+] Bloco: %lld bytes (stat), %llu lidos\n", (long long)(sp_0xe8.st_size), (unsigned long long)(x23));
    out(sp_0x150);
    snprintf(sp_0x150, 128, "[+] CRC-32: 0x%08x\n", (unsigned int)(w25));
    out(sp_0x150);
    snprintf(sp_0x150, 128, "[+] FNV-1a: 0x%016llx\n", (unsigned long long)(x20));
    out(sp_0x150);
    x3 = ((x22 == 4) ? "" : "(fseek falhou) ");
  } else {
    fclose((FILE *)((uintptr_t)(p0)));
    p24 = sp_0x150;
    snprintf(sp_0x150, 128, "[+] Bloco: %lld bytes (stat), %llu lidos\n", (long long)(sp_0xe8.st_size), (unsigned long long)(x23));
    out(sp_0x150);
    snprintf(sp_0x150, 128, "[+] CRC-32: 0x%08x\n", (unsigned int)(w25));
    out(sp_0x150);
    snprintf(sp_0x150, 128, "[+] FNV-1a: 0x%016llx\n", (unsigned long long)(x20));
    out(sp_0x150);
    x3 = "(fseek falhou) ";
  }
  snprintf(p24, 128, "[+] Bytes em 0x10000: %s%02x%02x%02x%02x\n", x3, (unsigned int)(*(uint8_t *)sp_0x80), (unsigned int)(*(uint8_t *)((uint8_t *)sp_0x80 + 1)), (unsigned int)(*(uint8_t *)((uint8_t *)sp_0x80 + 2)), (unsigned int)(*(uint8_t *)((uint8_t *)sp_0x80 + 3)));
  out(p24);
  snprintf(p24, 128, "[+] Tempo de leitura: %.3f ms\n", ((double)(int64_t)((((x27 - x26) * 625) / 12)) / 1000000.0));
  out(p24);
  *(unsigned __int128 *)((uint8_t *)sp_0xa8) = 0;
  *(unsigned __int128 *)((uint8_t *)sp_0xa8 + 16) = 0;
  *(unsigned __int128 *)((uint8_t *)sp_0xa8 + 32) = 0;
  *(unsigned __int128 *)((uint8_t *)sp_0xa8 + 32 + 16) = 0;
  p0_2 = fopen((const char *)((uintptr_t)("romfs:/texto/info.txt")), (const char *)((uintptr_t)("r")));
  if (p0_2 != 0) {
    sp_0x68 = (uintptr_t)p0_2;
    if (fgets(sp_0xa8, 64, (FILE *)((uintptr_t)(p0_2))) == 0) {
      *(uint8_t *)sp_0xa8 = 0;
    }
    fclose((FILE *)((uintptr_t)(sp_0x68)));
  }
  w20 = 0;
  snprintf(p24, 128, "[+] info.txt: %s", (const char *)(((*(uint8_t *)sp_0xa8 == 0) ? "(vazio)\n" : sp_0xa8)));
  out(p24);
  p0_3 = opendir((const char *)((uintptr_t)("romfs:/")));
  if (p0_3 != 0) {
    for (;;) {
      p0_4 = readdir(p0_3);
      if (p0_4 == 0) break;
      p0_5 = ((struct dirent *)p0_4)->d_name;
      sp_0x68 = (uintptr_t)p0_5;
      x0_8 = strcmp((const char *)((uintptr_t)(p0_5)), (const char *)((uintptr_t)(".")));
      if (x0_8 != 0) {
        x0_8 = (((uint32_t)(strcmp((const char *)((uintptr_t)(sp_0x68)), (const char *)((uintptr_t)("..")))) != 0) ? 1 : 0);
      }
      w20 = (w20 + (uint32_t)(x0_8));
    }
    closedir(p0_3);
  }
  snprintf(p24, 128, "[+] Entradas em romfs:/: %d\n", (int)(w20));
  out(p24);
  p0_6 = fopen((const char *)((uintptr_t)("romfs:/novo.bin")), (const char *)((uintptr_t)("wb")));
  w20_2 = ((p0_6 == 0) ? 1 : 0);
  if (p0_6 != 0) {
    fclose((FILE *)((uintptr_t)(p0_6)));
  }
  p0_7 = fopen((const char *)((uintptr_t)("romfs:/../hb_assets.c")), (const char *)((uintptr_t)("rb")));
  w20_3 = ((p0_7 == 0) ? (w20_2 + 1) : w20_2);
  if (p0_7 != 0) {
    fclose((FILE *)((uintptr_t)(p0_7)));
  }
  snprintf(p24, 128, "[+] Acessos recusados no romfs: %d/2\n", (int)(w20_3));
  out(p24);
  w3 = w25;
  *(uint64_t *)sp_0x88 = 0;
  *(uint64_t *)((uint8_t *)sp_0x88 + 8) = 0;
  *(uint64_t *)sp_0x98 = 0;
  *(uint64_t *)((uint8_t *)sp_0x98 + 8) = 0;
  snprintf(sp_0x88, 16, "%08x\n", (unsigned int)(w3));
  p0_8 = fopen((const char *)((uintptr_t)("sdmc:/obdc_hb_assets.txt")), (const char *)((uintptr_t)("w")));
  if (p0_8 == 0) {
    p20 = fopen((const char *)((uintptr_t)("sdmc:/obdc_hb_assets.txt")), (const char *)((uintptr_t)("r")));
    if (p20 != 0) {
      fclose((FILE *)((uintptr_t)(p20)));
    }
    x3_2 = "falhou";
  } else {
    w22 = fputs((const char *)((uintptr_t)(sp_0x88)), (FILE *)((uintptr_t)(p0_8)));
    fclose((FILE *)((uintptr_t)(p0_8)));
    p0_9 = fopen((const char *)((uintptr_t)("sdmc:/obdc_hb_assets.txt")), (const char *)((uintptr_t)("r")));
    p20 = p0_9;
    if ((p0_9 == 0) || (w22 < 0)) {
      if (p20 != 0) {
        fclose((FILE *)((uintptr_t)(p20)));
      }
      x3_2 = "falhou";
    } else {
      if (fgets(sp_0x98, 16, (FILE *)((uintptr_t)(p0_9))) == 0) {
        fclose((FILE *)((uintptr_t)(p20)));
        x3_2 = "falhou";
      } else {
        w21 = strcmp((const char *)((uintptr_t)(sp_0x98)), (const char *)((uintptr_t)(sp_0x88)));
        fclose((FILE *)((uintptr_t)(p20)));
        if (w21 != 0) {
          x3_2 = "falhou";
        } else {
          if (remove((const char *)((uintptr_t)("sdmc:/obdc_hb_assets.txt"))) == 0) {
            x3_2 = (((uint32_t)(stat((const char *)((uintptr_t)("sdmc:/obdc_hb_assets.txt")), (struct stat *)(&sp_0xe8))) != 0) ? "ok" : "falhou");
          } else {
            x3_2 = "falhou";
          }
        }
      }
    }
  }
  snprintf(p24, 128, "[+] sdmc:/ gravar/ler/apagar: %s\n", x3_2);
  out(p24);
  romfsUnmount("romfs");
  fsdevUnmountAll();
  fsExit();
  smExit();
  return w19;
}
