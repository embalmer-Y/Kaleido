// B group (V-34): ThorVG/Lottie offscreen rendering tier matrix.
// Canvas 230x230 / 325x325 / 460x460, ARGB8888 in PSRAM, 300 frames per tier.
// Reports per-frame avg/p50/min/max ms, CPU% at 20/24/30fps, heap deltas and peaks.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "thorvg_capi.h"
#include "bench_util.h"
#include "bench_b.h"

extern const char test_lottie_json_start[] asm("_binary_test_lottie_json_start");
extern const char test_lottie_json_end[]   asm("_binary_test_lottie_json_end");

static const char *TAG = "soulbench.b";

static uint32_t g_ps_free_before_engine = 0;

// Render one full frame: set_frame + update + draw + sync
static Tvg_Result render_frame(Tvg_Canvas canvas, Tvg_Animation anim, int total, int i) {
    Tvg_Result r = tvg_animation_set_frame(anim, (float)(i % total));
    if (r != TVG_RESULT_SUCCESS && r != TVG_RESULT_INSUFFICIENT_CONDITION) return r;
    r = tvg_canvas_update(canvas);
    if (r != TVG_RESULT_SUCCESS) return r;
    r = tvg_canvas_draw(canvas, false);
    if (r != TVG_RESULT_SUCCESS) return r;
    return tvg_canvas_sync(canvas);
}

// One tier: name tag, canvas w x h, N frames. Returns 0 on success.
static int tier_run(const char *tag, int w, int h, int frames, int heap_probe_frames) {
    size_t buf_sz = (size_t)w * h * 4;
    uint32_t *buf = heap_caps_malloc(buf_sz, MALLOC_CAP_SPIRAM);
    if (!buf) { ESP_LOGE(TAG, "SB,RESULT,b_%s,ERROR=canvas_alloc_failed bytes=%u", tag, (unsigned)buf_sz); return -1; }
    memset(buf, 0, buf_sz);
    uint32_t ps_after_canvas = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    Tvg_Canvas canvas = tvg_swcanvas_create(TVG_ENGINE_OPTION_DEFAULT);
    if (!canvas) { ESP_LOGE(TAG, "SB,RESULT,b_%s,ERROR=canvas_create", tag); heap_caps_free(buf); return -1; }
    if (tvg_swcanvas_set_target(canvas, buf, w, w, h, TVG_COLORSPACE_ARGB8888) != TVG_RESULT_SUCCESS) {
        ESP_LOGE(TAG, "SB,RESULT,b_%s,ERROR=set_target", tag); heap_caps_free(buf); return -1;
    }

    size_t json_len = test_lottie_json_end - test_lottie_json_start;
    /* flash 映射的 EMBED 数据无 NUL 终止且只读：lottie 解析器需要可写/终止的输入
     *（手写资产越界崩溃 v34_xip_on_v6、官方资产 rc=1 均源于此） */
    char *json = malloc(json_len + 1);
    if (!json) { ESP_LOGE(TAG, "SB,RESULT,b_%s,ERROR=json_alloc", tag); heap_caps_free(buf); return -1; }
    memcpy(json, test_lottie_json_start, json_len);
    json[json_len] = '\0';
    Tvg_Animation anim = tvg_lottie_animation_new();
    Tvg_Paint pic = tvg_animation_get_picture(anim);
    // mimetype "lot"/"lottie+json" per tvgLoaderMgr.cpp _convert
    Tvg_Result r = tvg_picture_load_data(pic, json, json_len, "lot", NULL, true);
    free(json);
    if (r != TVG_RESULT_SUCCESS) {
        ESP_LOGE(TAG, "SB,RESULT,b_%s,ERROR=load_data rc=%d json_bytes=%u", tag, r, (unsigned)json_len);
        heap_caps_free(buf); return -1;
    }
    tvg_picture_set_size(pic, (float)w, (float)h);
    tvg_canvas_add(canvas, pic);

    float total_f = 0;
    tvg_animation_get_total_frame(anim, &total_f);
    int total = (total_f > 0) ? (int)total_f : 90;
    uint32_t ps_after_load = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    // warmup (not timed)
    for (int i = 0; i < 10; i++) {
        if (render_frame(canvas, anim, total, i) != TVG_RESULT_SUCCESS) {
            ESP_LOGE(TAG, "SB,RESULT,b_%s,ERROR=warmup_frame_%d", tag, i);
            heap_caps_free(buf); return -1;
        }
    }

    // timing pass
    double *samples = calloc(frames, sizeof(double));
    for (int i = 0; i < frames; i++) {
        int64_t t0 = esp_timer_get_time();
        if (render_frame(canvas, anim, total, i) != TVG_RESULT_SUCCESS) {
            ESP_LOGE(TAG, "SB,RESULT,b_%s,ERROR=frame_%d", tag, i);
            break;
        }
        int64_t t1 = esp_timer_get_time();
        samples[i] = (t1 - t0) / 1000.0;
    }
    bench_stat_t st;
    bench_stat_compute(&st, samples, frames);
    uint32_t ck = bench_checksum32(buf, buf_sz / 64);
    ESP_LOGI(TAG, "SB,RESULT,b_thorvg_%s,frames=%d,total_frames=%d,avg_ms=%.3f,p50_ms=%.3f,min_ms=%.3f,max_ms=%.3f,cksum=%u",
             tag, st.n, total, st.avg_ms, st.p50_ms, st.min_ms, st.max_ms, ck);
    ESP_LOGI(TAG, "SB,CPU,b_thorvg_%s,cpu_pct_20fps=%.1f,cpu_pct_24fps=%.1f,cpu_pct_30fps=%.1f,fps_cap=%.1f",
             tag, 100.0 * st.avg_ms / 50.0, 100.0 * st.avg_ms / (1000.0 / 24.0), 100.0 * st.avg_ms / (1000.0 / 30.0),
             1000.0 / st.avg_ms);

    // heap-sampled pass (timing discarded): tracks PSRAM/internal min free during render
    uint32_t ps_min = UINT32_MAX, int_min = UINT32_MAX;
    for (int i = 0; i < heap_probe_frames; i++) {
        render_frame(canvas, anim, total, i);
        uint32_t pf = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        uint32_t inf = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        if (pf < ps_min) ps_min = pf;
        if (inf < int_min) int_min = inf;
    }
    uint32_t ps_delta_engine_plus_anim = g_ps_free_before_engine - ps_after_load;
    ESP_LOGI(TAG, "SB,HEAPB,b_thorvg_%s,canvas_bytes=%u,json_bytes=%u,ps_after_canvas_alloc=%u,ps_after_load=%u,"
             "ps_min_during=%u,int_min_during=%u,engine_plus_anim_persistent=%u,per_frame_alloc_churn=%u",
             tag, (unsigned)buf_sz, (unsigned)json_len, ps_after_canvas, ps_after_load,
             ps_min, int_min, ps_delta_engine_plus_anim,
             (ps_after_load > ps_min) ? (ps_after_load - ps_min) : 0);

    tvg_canvas_destroy(canvas);
    tvg_animation_del(anim);
    free(samples);
    heap_caps_free(buf);
    return 0;
}

void bench_b_run(int frames_per_tier) {
    heap_snapshot(TAG, "b_before_engine");
    g_ps_free_before_engine = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    // ---- pass 1: single-thread engine (all raster on caller thread, CPU1) ----
    Tvg_Result r = tvg_engine_init(0);
    if (r != TVG_RESULT_SUCCESS) { ESP_LOGE(TAG, "SB,RESULT,b_group,ERROR=engine_init0 rc=%d", r); return; }
    ESP_LOGI(TAG, "SB,ENGINE,init0,ps_after=%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    tier_run("230_st", 230, 230, frames_per_tier, 100);
    heap_snapshot(TAG, "b_after_230");
    tier_run("325_st", 325, 325, frames_per_tier, 100);
    heap_snapshot(TAG, "b_after_325");
    tier_run("460_st", 460, 460, frames_per_tier, 100);
    heap_snapshot(TAG, "b_after_460");

    tvg_engine_term();

    // ---- pass 2: 1 worker thread (dual-core: caller CPU1 + worker CPU0) ----
    r = tvg_engine_init(1);
    if (r != TVG_RESULT_SUCCESS) {
        ESP_LOGE(TAG, "SB,RESULT,b_mt1,ERROR=engine_init1 rc=%d (dual-core pass unavailable)", r);
    } else {
        ESP_LOGI(TAG, "SB,ENGINE,init1,ps_after=%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        tier_run("230_mt1", 230, 230, frames_per_tier, 50);
        tier_run("460_mt1", 460, 460, frames_per_tier, 50);
        tvg_engine_term();
    }
    heap_snapshot(TAG, "b_after_engine_term");
    ESP_LOGI(TAG, "SB,STACK,bench_task,after_b,hwm_bytes=%u", (unsigned)uxTaskGetStackHighWaterMark(NULL));
}
