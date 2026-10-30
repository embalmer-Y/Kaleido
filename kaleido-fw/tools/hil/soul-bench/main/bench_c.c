// C group: esp_jpeg (tjpgd) decode of embedded 460x460 JPEG, N frames.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "jpeg_decoder.h"
#include "bench_util.h"
#include "bench_c.h"

extern const uint8_t test460_jpg_start[] asm("_binary_test460_jpg_start");
extern const uint8_t test460_jpg_end[]   asm("_binary_test460_jpg_end");

static const char *TAG = "soulbench.c";

void bench_c_run(int n_frames) {
    size_t jpg_len = test460_jpg_end - test460_jpg_start;
    size_t out_sz = 460 * 460 * 2; // RGB565
    uint8_t *out = heap_caps_malloc(out_sz, MALLOC_CAP_SPIRAM);
    if (!out) { ESP_LOGE(TAG, "SB,RESULT,c_jpeg460,ERROR=outbuf_alloc"); return; }

    esp_jpeg_image_cfg_t cfg = {
        .indata = (uint8_t *)test460_jpg_start,
        .indata_size = jpg_len,
        .outbuf = out,
        .outbuf_size = out_sz,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
        .flags = { .swap_color_bytes = 0 },
        .advanced = { .working_buffer = NULL, .working_buffer_size = 0 },
        .priv = { .read = 0 },
    };
    esp_jpeg_image_output_t img = { 0 };

    uint32_t ps0 = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    // warmup x3
    for (int i = 0; i < 3; i++) {
        if (esp_jpeg_decode(&cfg, &img) != ESP_OK) { ESP_LOGE(TAG, "SB,RESULT,c_jpeg460,ERROR=warmup_decode"); heap_caps_free(out); return; }
    }
    uint32_t ps1 = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    double *samples = calloc(n_frames, sizeof(double));
    int ok = 0;
    for (int i = 0; i < n_frames; i++) {
        int64_t t0 = esp_timer_get_time();
        esp_err_t e = esp_jpeg_decode(&cfg, &img);
        int64_t t1 = esp_timer_get_time();
        if (e == ESP_OK) ok++;
        samples[i] = (t1 - t0) / 1000.0;
    }
    bench_stat_t st;
    bench_stat_compute(&st, samples, n_frames);
    ESP_LOGI(TAG, "SB,RESULT,c_jpeg460_rgb565,frames=%d,ok=%d,jpg_bytes=%u,out_wh=%ux%u,avg_ms=%.3f,p50_ms=%.3f,min_ms=%.3f,max_ms=%.3f",
             st.n, ok, (unsigned)jpg_len, img.width, img.height, st.avg_ms, st.p50_ms, st.min_ms, st.max_ms);
    ESP_LOGI(TAG, "SB,CPU,c_jpeg460_rgb565,cpu_pct_20fps=%.1f,cpu_pct_24fps=%.1f,fps_cap=%.1f,cksum=%u",
             100.0 * st.avg_ms / 50.0, 100.0 * st.avg_ms / (1000.0 / 24.0), 1000.0 / st.avg_ms,
             (unsigned)bench_checksum32(out, out_sz / 64));
    ESP_LOGI(TAG, "SB,HEAPC,c_jpeg460_rgb565,ps_before=%u,ps_after=%u,ps_delta=%u",
             ps0, ps1, (ps0 > ps1) ? (ps0 - ps1) : 0);
    heap_caps_free(out);
    free(samples);
}
