#include <stdio.h>
#include <time.h>

int main() {
  printf("[+] Execucao nativa ARM64 iniciada no Android!\n");

  const int iterations = 10000000;
  long long accumulator = 0;

  clock_t start = clock();
  for (int i = 0; i < iterations; ++i) {
    accumulator += (i ^ (i >> 3)) * 3;
  }
  clock_t end = clock();

  double time_spent = (double)(end - start) / CLOCKS_PER_SEC;

  printf("[+] Resultado: %lld\n", accumulator);
  printf("[+] Tempo decorrido: %.4f s\n", time_spent);
  printf("[+] Executado nativamente no hardware AArch64.\n");

  return 0;
}