#include "bench_a.h"
#include "bench_util.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
// IDF v6.1 (TF-PSA-Crypto): legacy SHA256 API lives in mbedtls/private/sha256.h,
// declarations gated behind MBEDTLS_ALLOW_PRIVATE_ACCESS.
#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#include "mbedtls/private/sha256.h"

static const char *TAG = "soulbench.a";

#define MB4 (4u * 1024u * 1024u)

void bench_a1_memcpy_psram(int n_iters) {
    double *samples = NULL;
    uint8_t *src = heap_caps_malloc(MB4, MALLOC_CAP_SPIRAM);
    uint8_t *dst = heap_caps_malloc(MB4, MALLOC_CAP_SPIRAM);
    if (!src || !dst) {
        ESP_LOGE(TAG, "SB,RESULT,a1_memcpy4M,ERROR=alloc_failed src=%p dst=%p", src, dst);
        goto out;
    }
    memset(src, 0xA5, MB4);
    memset(dst, 0, MB4);
    samples = calloc(n_iters, sizeof(double));
    vTaskDelay(1);
    for (int i = 0; i < n_iters; i++) {
        int64_t t0 = esp_timer_get_time();
        memcpy(dst, src, MB4);
        int64_t t1 = esp_timer_get_time();
        samples[i] = (t1 - t0) / 1000.0;
    }
    bench_stat_t st;
    bench_stat_compute(&st, samples, n_iters);
    bench_report(TAG, "a1_memcpy4M_psram", &st);
    ESP_LOGI(TAG, "SB,RESULT,a1_memcpy4M_psram,throughput_MBps=%.1f", MB4 / 1024.0 / 1024.0 / (st.avg_ms / 1000.0));
    ESP_LOGI(TAG, "SB,RESULT,a1_memcpy4M_psram,checksum=%u", (unsigned)bench_checksum32(dst, 4096));
out:
    heap_caps_free(src);
    heap_caps_free(dst);
    free(samples);
}

static char *build_100k_json(void) {
    // ~100KB synthetic JSON: root object with array of records
    size_t cap = 110 * 1024;
    char *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!buf) return NULL;
    char *p = buf;
    p += snprintf(p, cap - (p - buf), "{\"records\":[");
    int n = 0;
    while (p - buf < cap - 300) {
        p += snprintf(p, cap - (p - buf),
                      n ? "," : "");
        p += snprintf(p, cap - (p - buf),
                      "{\"id\":%d,\"name\":\"kaleido_%d\",\"val\":%.3f,\"score\":%d,"
                      "\"tag\":\"expr\",\"weight\":%.4f,\"active\":%s}",
                      n, n, (n * 13) % 997 / 7.0, n % 100, (n * 31) % 883 / 9.0, (n & 1) ? "true" : "false");
        n++;
    }
    p += snprintf(p, cap - (p - buf), "]}");
    ESP_LOGI(TAG, "SB,JSON_BUILT,bytes=%d,records=%d", (int)(p - buf), n);
    return buf;
}

void bench_a2_cjson(int n_iters) {
    char *json = build_100k_json();
    if (!json) { ESP_LOGE(TAG, "SB,RESULT,a2_cjson100k,ERROR=json_alloc_failed"); return; }
    double *samples = calloc(n_iters, sizeof(double));
    volatile int ok = 0;
    for (int i = 0; i < n_iters; i++) {
        int64_t t0 = esp_timer_get_time();
        cJSON *root = cJSON_Parse(json);
        int64_t t1 = esp_timer_get_time();
        if (root) { ok++; cJSON_Delete(root); }
        samples[i] = (t1 - t0) / 1000.0;
    }
    bench_stat_t st;
    bench_stat_compute(&st, samples, n_iters);
    bench_report(TAG, "a2_cjson_parse_100k", &st);
    ESP_LOGI(TAG, "SB,RESULT,a2_cjson_parse_100k,parsed_ok=%d/%d", ok, n_iters);
    heap_caps_free(json);
    free(samples);
}

#define FIR_TAPS 64
#define FIR_MAC_PER_ITER (1000 * 1000)

void bench_a3_fir(int n_iters) {
    static float taps[FIR_TAPS];
    float *x = heap_caps_malloc((FIR_TAPS + 15625) * sizeof(float), MALLOC_CAP_SPIRAM);
    if (!x) { ESP_LOGE(TAG, "SB,RESULT,a3_fir1m,ERROR=alloc_failed"); return; }
    for (int i = 0; i < FIR_TAPS; i++) taps[i] = (float)(i + 1) * 0.001f;
    for (int i = 0; i < FIR_TAPS + 15625; i++) x[i] = (float)(i % 911) * 0.01f;
    double *samples = calloc(n_iters, sizeof(double));
    volatile float sink = 0;
    for (int i = 0; i < n_iters; i++) {
        int64_t t0 = esp_timer_get_time();
        float acc = 0;
        const float *p = x;
        for (int k = 0; k < FIR_MAC_PER_ITER / FIR_TAPS; k++) {
            float s = 0;
            for (int j = 0; j < FIR_TAPS; j++) s += p[j] * taps[j];
            acc += s;
            p++;
        }
        sink += acc;
        int64_t t1 = esp_timer_get_time();
        samples[i] = (t1 - t0) / 1000.0;
    }
    bench_stat_t st;
    bench_stat_compute(&st, samples, n_iters);
    bench_report(TAG, "a3_fir_1mmac", &st);
    ESP_LOGI(TAG, "SB,RESULT,a3_fir_1mmac,sink=%.3f", (double)sink);
    heap_caps_free(x);
    free(samples);
}

void bench_a5_sha256(int n_iters) {
    uint8_t *buf = heap_caps_malloc(MB4, MALLOC_CAP_SPIRAM);
    if (!buf) { ESP_LOGE(TAG, "SB,RESULT,a5_sha256_4m,ERROR=alloc_failed"); return; }
    for (size_t i = 0; i < MB4; i += 4) { buf[i] = (uint8_t)(i); buf[i+1] = (uint8_t)(i >> 8); buf[i+2] = 0x5A; buf[i+3] = 0xC3; }
    double *samples = calloc(n_iters, sizeof(double));
    uint8_t digest[32];
    for (int i = 0; i < n_iters; i++) {
        int64_t t0 = esp_timer_get_time();
        mbedtls_sha256_context ctx;
        mbedtls_sha256_init(&ctx);
        mbedtls_sha256_starts(&ctx, 0);
        mbedtls_sha256_update(&ctx, buf, MB4);
        mbedtls_sha256_finish(&ctx, digest);
        mbedtls_sha256_free(&ctx);
        int64_t t1 = esp_timer_get_time();
        samples[i] = (t1 - t0) / 1000.0;
    }
    bench_stat_t st;
    bench_stat_compute(&st, samples, n_iters);
    bench_report(TAG, "a5_sha256_4m", &st);
    ESP_LOGI(TAG, "SB,RESULT,a5_sha256_4m,throughput_MBps=%.1f,digest0=%02x%02x",
             MB4 / 1024.0 / 1024.0 / (st.avg_ms / 1000.0), digest[0], digest[1]);
    heap_caps_free(buf);
    free(samples);
}
