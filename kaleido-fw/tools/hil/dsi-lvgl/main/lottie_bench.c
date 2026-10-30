/*
 * lottie_bench.c — V-35R：ThorVG/Lottie 在 P4 的离屏渲染标定
 *
 * 移植自 S31 soul-bench bench_b（同资产/同分档/同口径，数字直接可比）：
 * 230/325/460 三档 ARGB8888 PSRAM 画布，各 300 帧计时 + 双核 mt1 对照。
 * W-6（thorvg 需 pthread 上下文）：本基准整体跑在 pthread 里，
 * LVGL 任务内的首验由场景合成阶段另行验证。
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "thorvg_capi.h"

extern const char test_lottie_json_start[] asm("_binary_test_lottie_json_start");
extern const char test_lottie_json_end[] asm("_binary_test_lottie_json_end");

static const char *TAG = "lottie";

static Tvg_Result render_frame(Tvg_Canvas canvas, Tvg_Animation anim,
			       int total, int i)
{
	Tvg_Result r = tvg_animation_set_frame(anim, (float)(i % total));

	if (r != TVG_RESULT_SUCCESS && r != TVG_RESULT_INSUFFICIENT_CONDITION)
		return r;
	r = tvg_canvas_update(canvas);
	if (r != TVG_RESULT_SUCCESS)
		return r;
	r = tvg_canvas_draw(canvas, false);
	if (r != TVG_RESULT_SUCCESS)
		return r;
	return tvg_canvas_sync(canvas);
}

static int cmp_dbl(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return (x > y) - (x < y);
}

static int tier_run(const char *tag, int w, int h, int frames)
{
	size_t buf_sz = (size_t)w * h * 4;
	uint32_t *buf = heap_caps_malloc(buf_sz, MALLOC_CAP_SPIRAM);

	if (!buf) {
		ESP_LOGE(TAG, "DSI,LOTTIE,%s,ERROR=alloc", tag);
		return -1;
	}
	memset(buf, 0, buf_sz);

	Tvg_Canvas canvas = tvg_swcanvas_create(TVG_ENGINE_OPTION_DEFAULT);

	if (!canvas) {
		ESP_LOGE(TAG, "DSI,LOTTIE,%s,ERROR=canvas", tag);
		goto out;
	}
	if (tvg_swcanvas_set_target(canvas, buf, w, w, h,
				    TVG_COLORSPACE_ARGB8888) !=
	    TVG_RESULT_SUCCESS) {
		ESP_LOGE(TAG, "DSI,LOTTIE,%s,ERROR=target", tag);
		goto out;
	}

	/* EMBED 数据只读且无 NUL 终止，lottie 解析需要可写副本（S31 教训） */
	size_t json_len = test_lottie_json_end - test_lottie_json_start;
	char *json = malloc(json_len + 1);

	if (!json)
		goto out;
	memcpy(json, test_lottie_json_start, json_len);
	json[json_len] = '\0';

	Tvg_Animation anim = tvg_lottie_animation_new();
	Tvg_Paint pic = tvg_animation_get_picture(anim);
	Tvg_Result r = tvg_picture_load_data(pic, json, json_len, "lot",
					     NULL, true);

	free(json);
	if (r != TVG_RESULT_SUCCESS) {
		ESP_LOGE(TAG, "DSI,LOTTIE,%s,ERROR=load rc=%d", tag, r);
		goto out;
	}
	tvg_picture_set_size(pic, (float)w, (float)h);
	tvg_canvas_add(canvas, pic);

	float total_f = 0;

	tvg_animation_get_total_frame(anim, &total_f);
	int total = (total_f > 0) ? (int)total_f : 90;

	for (int i = 0; i < 10; i++) {
		if (render_frame(canvas, anim, total, i) != TVG_RESULT_SUCCESS) {
			ESP_LOGE(TAG, "DSI,LOTTIE,%s,ERROR=warmup", tag);
			goto out;
		}
	}

	double *samples = calloc(frames, sizeof(double));
	int n_ok = 0;

	for (int i = 0; i < frames; i++) {
		int64_t t0 = esp_timer_get_time();

		if (render_frame(canvas, anim, total, i) != TVG_RESULT_SUCCESS)
			break;
		samples[n_ok++] = (esp_timer_get_time() - t0) / 1000.0;
	}
	double avg = 0, min_s = 1e9, max_s = 0;

	for (int i = 0; i < n_ok; i++) {
		avg += samples[i];
		if (samples[i] < min_s)
			min_s = samples[i];
		if (samples[i] > max_s)
			max_s = samples[i];
	}
	avg /= n_ok ? n_ok : 1;
	qsort(samples, n_ok, sizeof(double), cmp_dbl);
	double p50 = n_ok ? samples[n_ok / 2] : 0;

	printf("DSI,LOTTIE,%s,frames=%d,avg_ms=%.3f,p50_ms=%.3f,min_ms=%.3f,"
	       "max_ms=%.3f,fps_cap=%.1f\r\n",
	       tag, n_ok, avg, p50, min_s, max_s, avg > 0 ? 1000.0 / avg : 0);
	fflush(stdout);

	free(samples);
	tvg_canvas_destroy(canvas);
	tvg_animation_del(anim);
out:
	heap_caps_free(buf);
	return 0;
}

static void *lottie_pthread(void *arg)
{
	printf("DSI,LOTTIE,START,single-thread tiers\r\n");
	fflush(stdout);
	if (tvg_engine_init(0) != TVG_RESULT_SUCCESS) {
		printf("DSI,LOTTIE,ERROR=engine_init0\r\n");
		fflush(stdout);
		return NULL;
	}
	tier_run("230_st", 230, 230, 300);
	tier_run("325_st", 325, 325, 300);
	tier_run("460_st", 460, 460, 300);
	tvg_engine_term();

	printf("DSI,LOTTIE,START,mt1(dual-core)\r\n");
	fflush(stdout);
	if (tvg_engine_init(1) != TVG_RESULT_SUCCESS) {
		printf("DSI,LOTTIE,ERROR=engine_init1\r\n");
		fflush(stdout);
		return NULL;
	}
	tier_run("460_mt1", 460, 460, 300);
	tvg_engine_term();

	printf("DSI,LOTTIE,DONE\r\n");
	fflush(stdout);
	return NULL;
}

int lottie_bench_run(void)
{
	/* W-6：thorvg 断言 pthread 上下文，基准线程走 pthread（64K 栈） */
	pthread_attr_t attr;
	pthread_t th;

	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 64 * 1024);
	int rc = pthread_create(&th, &attr, lottie_pthread, NULL);

	if (rc) {
		ESP_LOGE(TAG, "DSI,LOTTIE,ERROR=pthread rc=%d", rc);
		return -1;
	}
	pthread_detach(th);
	return 0;
}
