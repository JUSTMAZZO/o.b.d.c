#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include "switch_hle_shim.h"

static const uint32_t DAT_120500[8] = {
    0xffffffff, 0xff00ffff, 0xffffff00, 0xff00ff00,
    0xffff00ff, 0xff0000ff, 0xffff0000, 0xff101010
};
static uint8_t DAT_12a000[0x1000];

static void out(void *a0) {
  void *p19;

  p19 = a0;
  svcOutputDebugString(p19, strlen(a0));
  return;
}

static void on_applet(uint64_t a0) {
  if ((uint32_t)(a0) == 3) {
    *(uint32_t *)(((uint8_t *)DAT_12a000 + 744)) = (*(uint32_t *)(((uint8_t *)DAT_12a000 + 744)) + 1);
    return;
  }
  if (a0 == 0) {
    *(uint32_t *)(((uint8_t *)DAT_12a000 + 748)) = (*(uint32_t *)(((uint8_t *)DAT_12a000 + 748)) + 1);
    return;
  }
  return;
}

int main(void) {
  uint8_t *b1;
  uint8_t *b19;
  uint8_t *b7;
  double d0;
  void *p19;
  uint32_t w0;
  uint32_t w0_2;
  uint32_t w0_3;
  uint32_t w0_4;
  uint32_t w0_5;
  uint32_t w1;
  uint32_t w10;
  uint32_t w12;
  uint32_t w15;
  uint32_t w16;
  uint32_t w19;
  uint32_t w19_2;
  uint32_t w2;
  uint32_t w20;
  uint32_t w21;
  uint32_t w25;
  uint32_t w26;
  uint32_t w28;
  uint32_t w3;
  uint32_t w30;
  uint32_t w3_2;
  uint32_t w4;
  uint32_t w5;
  uint32_t w5_2;
  uint32_t w6;
  uint32_t w8;
  uint32_t w9;
  uint64_t x0;
  uint64_t x0_2;
  uint64_t x0_3;
  uint64_t x0_4;
  uint64_t x0_5;
  uint64_t x0_6;
  uint64_t x1;
  uint64_t x13;
  uint64_t x17;
  uint64_t x20;
  uint64_t x20_2;
  uint64_t x21;
  uint64_t x22;
  uint64_t x23;
  uint64_t x28;
  const char *x4;
  bool sp_0x63;
  uint32_t sp_0x64;
  uint64_t sp_0x68;
  TimeCalendarTime sp_0x70;  /* objeto local na pilha (sp+0x70) */
  TimeCalendarAdditionalInfo sp_0x78;  /* objeto local na pilha (sp+0x78) */
  AppletHookCookie sp_0x90;  /* objeto local na pilha (sp+0x90) */
  PadState sp_0xa8;  /* objeto local na pilha (sp+0xa8) */
  Framebuffer sp_0xe0;  /* objeto local na pilha (sp+0xe0) */
  char sp_0x130[160];  /* objeto local na pilha (sp+0x130), passado por endereco */

  out("[+] hb_fb iniciado\n");
  x0 = timeInitialize();
  if (x0 == 0) {
    x0_3 = timeGetCurrentTime(x0, &sp_0x68);
    w19 = x0_3;
    if (x0_3 == 0) {
      w19 = timeToCalendarTimeWithMyRule(sp_0x68, &sp_0x70, &sp_0x78);
      x0_6 = time(0);
      if (w19 != 0) {
        x4 = "INCOERENTE";
      } else {
        if (sp_0x68 > 0x6553f0ff) {
          if (sp_0x70.year > 2022) {
            x4 = ((((x20 - x0_6) + 2) < 5) ? "coerente com time()" : "INCOERENTE");
          }
        }
      }
    } else {
      time(0);
      x4 = "INCOERENTE";
    }
  } else {
    w19 = x0;
    time(0);
    x4 = "INCOERENTE";
  }
  snprintf(sp_0x130, 160, "[+] Relogio (time:u): 0x%x, %s\n", (unsigned int)(w19), x4);
  out(sp_0x130);
  appletLockExit();
  appletHook(&sp_0x90, (AppletHookFn)on_applet, 0);
  w20 = appletIsAutoPowerDownRequested(&sp_0x63);
  w19_2 = appletGetFocusState();
  snprintf(sp_0x130, 160, "[+] Foco: %d (InFocus = 1), modo: %d, AutoPowerDown: 0x%x / %d\n", (int)(w19_2), (int)(appletGetOperationMode()), (unsigned int)(w20), (int)(sp_0x63));
  out(sp_0x130);
  x0_2 = framebufferCreate(&sp_0xe0, nwindowGetDefault(), 1280, 720, 1, 2);
  if (x0_2 != 0) {
    snprintf(sp_0x130, 160, "[+] framebufferCreate + MakeLinear: 0x%x\n", (unsigned int)(x0_2));
    out(sp_0x130);
    w0 = 1;
    return w0;
  }
  x0_4 = framebufferMakeLinear(&sp_0xe0);
  snprintf(sp_0x130, 160, "[+] framebufferCreate + MakeLinear: 0x%x\n", (unsigned int)(x0_4));
  out(sp_0x130);
  if (x0_4 != 0) {
    w0 = 1;
    return w0;
  }
  p19 = 0;
  w21 = 20;
  x20_2 = 0;
  x22 = 0;
  w25 = 0;
  w26 = 0;
  padConfigureInput(1, 31);
  padInitializeWithMask(&sp_0xa8, 0x100000001);
  if (appletMainLoop()) {
    for (;;) {
      padUpdate(&sp_0xa8);
      x23 = ((sp_0xa8.buttons_cur & (~sp_0xa8.buttons_old)) & 0x400);
      if (((sp_0xa8.buttons_cur & (~sp_0xa8.buttons_old)) & 0x400) != 0) break;
      p19 = framebufferBegin(&sp_0xe0, &sp_0x64);
      x17 = armGetSystemTick();
      w26 = (w26 + 1);
      w1 = ((w26 << 3) - w26);
      w16 = (w26 << 1);
      w10 = (w26 << 2);
      w9 = (w16 + w26);
      b7 = ((uint8_t *)p19 - 4);
      w4 = 0;
      x13 = 1;
      w12 = 0xffff8878;
      w25 = (w25 + 8);
      w15 = (0xfffffed4 - (w1 - ((uint32_t)((((uint64_t)(w1) * 0x51eb851f) >> 38)) * 200)));
      w6 = (~(w21 - (1216 * (uint32_t)((((uint64_t)((w21 >> 6)) * 0xd794361) >> 32)))));
      for (;;) {
        w8 = (w15 + w4);
        w3 = 0;
        x1 = x13;
        w2 = 0;
        w5 = (((((uint32_t)((((uint64_t)(w12) * 0xdad16a6b) >> 41)) + w16) & 0xff) << 8) | 0xff000000);
        for (;;) {
          if (((w6 + (uint32_t)(x1)) | w8) <= 63) {
            x0_5 = 0xffffffff;
            w28 = 0xffffffff;
            w2 = (w2 + 1);
            *(uint32_t *)((uint8_t *)b7 + (x1 << 2)) = w28;
            x23 = (x23 + (x0_5 * x1));
            w3 = (w3 + 255);
            x1 = (x1 + 1);
            if (w2 == 1280) break;
          } else {
            if (w4 <= 119) {
              w28 = *(uint32_t *)((uint8_t *)DAT_120500 + (((((uint64_t)((w25 + w2)) * 0xcccccccd) >> 39) & 7) << 2));
              x0_5 = w28;
              w2 = (w2 + 1);
              *(uint32_t *)((uint8_t *)b7 + (x1 << 2)) = w28;
              x23 = (x23 + (x0_5 * x1));
              w3 = (w3 + 255);
              x1 = (x1 + 1);
              if (w2 == 1280) break;
            } else {
              w0_2 = (w4 ^ w2);
              w2 = (w2 + 1);
              x28 = (((uint64_t)(w3) * 0x99eb9585) >> 32);
              w30 = (w3 - (uint32_t)(x28));
              w3 = (w3 + 255);
              w0_3 = ((((w0_2 + w9) & 0xff) << 16) | ((w10 + (((uint32_t)(x28) + (w30 >> 1)) >> 10)) & 0xff));
              w0_4 = (w0_3 | w5);
              *(uint32_t *)((uint8_t *)b7 + (x1 << 2)) = (w0_3 | w5);
              x23 = (x23 + ((uint64_t)(w0_4) * x1));
              x1 = (x1 + 1);
              if (w2 == 1280) break;
            }
          }
        }
        w4 = (w4 + 1);
        w12 = (w12 + 255);
        b7 = (uint8_t *)(b7 + (((uint64_t)(sp_0x64) & 0xfffffffc) + 0xffffffffffffec00));
        x13 = (x13 + 1280);
        w6 = (w6 - 1280);
        if (w4 == 720) break;
      }
      x22 = (x22 + x23);
      w21 = (w21 + 20);
      x20_2 = ((x20_2 - x17) + armGetSystemTick());
      framebufferEnd(&sp_0xe0);
      if (!appletMainLoop()) break;
    }
  }
  if (p19 == 0) {
    x21 = 0xcbf29ce484222325;
  } else {
    x21 = 0xcbf29ce484222325;
    b19 = ((uint8_t *)p19 + 5120);
    w3_2 = 720;
    for (;;) {
      b1 = ((uint8_t *)b19 - 5120);
      for (;;) {
        w0_5 = *(uint8_t *)(b1);
        b1 = ((uint8_t *)b1 + 1);
        x21 = (((uint64_t)(w0_5) ^ x21) * 0x100000001b3);
        if (b19 == b1) break;
      }
      w3_2 = (w3_2 - 1);
      b19 = (uint8_t *)(b19 + ((uint64_t)(sp_0x64) & 0xfffffffc));
      if (w3_2 == 0) break;
    }
  }
  w5_2 = *(uint32_t *)(((uint8_t *)DAT_12a000 + 744));
  snprintf(sp_0x130, 160, "[+] Frames: %u, saida: %s (OnExitRequest x%d, foco x%d)\n", (unsigned int)(w26), ((w5_2 == 0) ? "botao Plus" : "pedido do sistema"), (int)(w5_2), (int)(*(uint32_t *)(((uint8_t *)DAT_12a000 + 748))));
  out(sp_0x130);
  snprintf(sp_0x130, 160, "[+] Soma ponderada (todos os frames): 0x%016llx\n", (unsigned long long)(x22));
  out(sp_0x130);
  snprintf(sp_0x130, 160, "[+] FNV-1a do ultimo frame: 0x%016llx\n", (unsigned long long)(x21));
  out(sp_0x130);
  if (w26 != 0) {
    d0 = (((double)(int64_t)(((x20_2 * 625) / 12)) / 1000000.0) / (double)(uint64_t)(w26));
    snprintf(sp_0x130, 160, "[+] Tempo de render: %.3f ms/frame\n", d0);
    out(sp_0x130);
    framebufferClose(&sp_0xe0);
    appletUnhook(&sp_0x90);
    appletUnlockExit();
    timeExit();
    w0 = 0;
    return w0;
  }
  d0 = 0.0;
  snprintf(sp_0x130, 160, "[+] Tempo de render: %.3f ms/frame\n", d0);
  out(sp_0x130);
  framebufferClose(&sp_0xe0);
  appletUnhook(&sp_0x90);
  appletUnlockExit();
  timeExit();
  w0 = 0;
  return w0;
}
