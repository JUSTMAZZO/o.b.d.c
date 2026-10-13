#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

#define N 1000000

int main(void) {
    puts("[+] Teste NEON Memory/Array iniciado no Android!");
    uint32_t *in_buf = (uint32_t *)malloc(N * sizeof(uint32_t));
    uint32_t *out_buf = (uint32_t *)malloc(N * sizeof(uint32_t));

    for (int i = 0; i < N; i++) {
        in_buf[i] = (uint32_t)(i * 7 + 3);
    }

    clock_t start_time = clock();
    uint64_t checksum = 0;

    for (int i = 0; i < N; i++) {
        uint32_t val = in_buf[i];
        uint32_t transformed = ((val << 2) - (val >> 1)) ^ 0x5A;
        out_buf[i] = transformed;
        checksum += transformed;
    }

    clock_t end_time = clock();
    double elapsed = (double)(end_time - start_time) / CLOCKS_PER_SEC;

    printf("[+] Checksum: %llu\n", (unsigned long long)checksum);
    printf("[+] Ultimo elemento: %u\n", out_buf[N - 1]);
    printf("[+] Tempo decorrido: %.4f s\n", elapsed);

    free(in_buf);
    free(out_buf);
    return 0;
}
