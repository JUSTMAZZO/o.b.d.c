#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

int main(void) {
  double d8;
  uint64_t x19;
  uint64_t checksum;

  puts("[+] Execucao nativa ARM64 iniciada no Android!");
  x19 = clock();
  checksum = 0;

  /* <LOOP_NEON_1> */
  for (uint32_t i = 0; i < 10000000; i++) {
    checksum += (((i >> 3) ^ i) * 3);
  }

  d8 = ((double)(int64_t)((clock() - x19)) / 1000000.0);
  printf("[+] Resultado: %lld\n", (long long)checksum);
  printf("[+] Tempo decorrido: %.4f s\n", d8);
  puts("[+] Executado nativamente no hardware AArch64.");
  return 0;
}
