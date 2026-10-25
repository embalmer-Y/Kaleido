#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// A1: PSRAM->PSRAM memcpy 4MB block, N iters
void bench_a1_memcpy_psram(int n_iters);
// A2: cJSON parse+delete of ~100KB JSON, N iters
void bench_a2_cjson(int n_iters);
// A3: float FIR 1M MAC per iter, N iters
void bench_a3_fir(int n_iters);
// A5: mbedtls SHA256 over 4MB PSRAM buffer, N iters
void bench_a5_sha256(int n_iters);

#ifdef __cplusplus
}
#endif
