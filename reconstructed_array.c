#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

int main(void)
{
  puts("[+] Teste NEON Memory/Array iniciado no Android!");
  int N = 1000000;
  uint32_t *in_buf = malloc(N * sizeof(uint32_t));
  uint32_t *out_buf = malloc(N * sizeof(uint32_t));
  uint64_t checksum = 0;

  // Loop 1
  for (int i = 0; i < N; i++) {
    in_buf[i] = ((i * 7) + 3);
  }

  clock_t start_time = clock();

  // Loop 2
  for (int i = 0; i < N; i++) {
    uint32_t transformed = ((in_buf[i] << 2) - (in_buf[i] >> 1)) ^ 0x5a;
    out_buf[i] = transformed;
    checksum += transformed;
  }

  clock_t end_time = clock();

  printf("[+] Checksum: %llu\n", (unsigned long long)checksum);
  printf("[+] Ultimo elemento: %u\n", out_buf[N - 1]);
  printf("[+] Tempo decorrido: %.4f s\n", (double)(end_time - start_time) / CLOCKS_PER_SEC);

  free(in_buf);
  free(out_buf);
  return 0;
}
