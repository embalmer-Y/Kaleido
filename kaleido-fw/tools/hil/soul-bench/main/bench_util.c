#include "bench_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

void bench_stat_compute(bench_stat_t *st, const double *samples, int n) {
    double *tmp = malloc(sizeof(double) * n);
    memcpy(tmp, samples, sizeof(double) * n);
    qsort(tmp, n, sizeof(double), cmp_double);
    double sum = 0;
    for (int i = 0; i < n; i++) sum += samples[i];
    st->n = n;
    st->avg_ms = sum / n;
    st->min_ms = tmp[0];
    st->max_ms = tmp[n - 1];
    st->p50_ms = (n % 2) ? tmp[n / 2] : 0.5 * (tmp[n / 2 - 1] + tmp[n / 2]);
    free(tmp);
}

void bench_report(const char *tag, const char *name, const bench_stat_t *st) {
    ESP_LOGI(tag, "SB,RESULT,%s,n=%d,avg_ms=%.3f,p50_ms=%.3f,min_ms=%.3f,max_ms=%.3f",
             name, st->n, st->avg_ms, st->p50_ms, st->min_ms, st->max_ms);
}

void heap_snapshot(const char *tag, const char *name) {
    multi_heap_info_t info_int, info_ps;
    heap_caps_get_info(&info_int, MALLOC_CAP_INTERNAL);
    heap_caps_get_info(&info_ps, MALLOC_CAP_SPIRAM);
    ESP_LOGI(tag, "SB,HEAP,%s,int_free=%u,int_largest=%u,ps_free=%u,ps_largest=%u,ps_min_free=%u,int_min_free=%u",
             name,
             (unsigned)info_int.total_free_bytes, (unsigned)info_int.largest_free_block,
             (unsigned)info_ps.total_free_bytes, (unsigned)info_ps.largest_free_block,
             (unsigned)info_ps.minimum_free_bytes, (unsigned)info_int.minimum_free_bytes);
}

uint32_t bench_checksum32(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}
