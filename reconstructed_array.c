#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

int main(void) {
  uint32_t *buf_x19;
  uint32_t *buf_x20;
  double d8;
  uint64_t x21;
  uint64_t checksum;

  puts("[+] Teste NEON Memory/Array iniciado no Android!");
  buf_x19 = (uint32_t *)malloc(0x3d0900);
  buf_x20 = (uint32_t *)malloc(0x3d0900);

  // LOOP_NEON_1
  for (uint32_t i = 0; i < 1000000; i++) {
    buf_x19[i] = ((i * 7) + 3);
  }

  x21 = clock();
  checksum = 0;

  // LOOP_NEON_2
  for (uint32_t i = 0; i < 1000000; i++) {
    buf_x20[i] = (((buf_x19[i] << 2) - (buf_x19[i] >> 1)) ^ 0x5a);
    checksum += (((buf_x19[i] << 2) - (buf_x19[i] >> 1)) ^ 0x5a);
  }

  d8 = ((double)(int64_t)((clock() - x21)) / 1000000.0);
  printf("[+] Checksum: %llu\n", (unsigned long long)checksum);
  printf("[+] Ultimo elemento: %u\n", (unsigned int)buf_x20[999999]);
  printf("[+] Tempo decorrido: %.4f s\n", d8);
  free(buf_x19);
  free(buf_x20);
  return 0;
}
