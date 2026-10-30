#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_timer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    double avg_ms;
    double min_ms;
    double max_ms;
    double p50_ms;
    int    n;
} bench_stat_t;

// Compute stats over samples[i] (ms), i in [0,n)
void bench_stat_compute(bench_stat_t *st, const double *samples, int n);
void bench_report(const char *tag, const char *name, const bench_stat_t *st);

// One-line heap snapshot (internal / psram / largest blocks), tagged
void heap_snapshot(const char *tag, const char *name);

uint32_t bench_checksum32(const void *data, size_t len); // defeat optimizer + sanity

#ifdef __cplusplus
}
#endif
