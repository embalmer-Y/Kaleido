/*
 * lottie_stage.c — 角色舞台 L0：pthread 渲染（thorvg 1.1.2）+ LVGL 合成并行
 *
 * 30fps 架构（V-35R + 30fps 冲刺）：
 *   渲染 pthread：325² ARGB8888 双缓冲（PSRAM）按 30fps 节拍渲染，
 *     只写后台缓冲，完成即置就绪标志（从不碰前台指针）。
 *   LVGL 16ms 定时器：在 LVGL 任务内采纳最新就绪帧（换 dsc.data +
 *     invalidate），合成与渲染并行、指针仅在 LVGL 上下文切换——
 *     消除首版"合成中途换前台指针"的撕裂与闪烁根源。
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "thorvg_capi.h"

extern const char test_lottie_json_start[] asm("_binary_test_lottie_json_start");
extern const char test_lottie_json_end[] asm("_binary_test_lottie_json_end");

static const char *TAG = "stage";

#define STAGE_W		325
#define STAGE_H		325
#define STAGE_FPS	30

#define STAGE_NBUF	3
static uint32_t *s_buf[STAGE_NBUF];
/* 邮箱：s_ready_idx>=0 且 !=s_disp_idx 表示有就绪帧；
 * 渲染写"既不在屏也非就绪"的缓冲，LVGL 采纳就绪帧后置 -1。
 * 三缓冲消除双缓冲下的生产/消费相位失锁（实测渲染率腰斩） */
static volatile int s_ready_idx = -1;
static volatile int s_disp_idx;
static lv_image_dsc_t s_img_dsc = {
	.header = {
		.w = STAGE_W,
		.h = STAGE_H,
		.cf = LV_COLOR_FORMAT_ARGB8888,
	},
	.data = NULL,
	.data_size = STAGE_W * STAGE_H * 4,
};
static lv_obj_t *s_img;
static lv_timer_t *s_stage_timer;
static volatile int s_render_cnt;
static volatile int s_disp_cnt;
static volatile int s_adopted;	/* 当前展示的缓冲下标 */

static void *stage_pthread(void *arg)
{
	size_t json_len = test_lottie_json_end - test_lottie_json_start;
	char *json = malloc(json_len + 1);
	if (!json)
		return NULL;
	memcpy(json, test_lottie_json_start, json_len);
	json[json_len] = '\0';

	if (tvg_engine_init(0) != TVG_RESULT_SUCCESS) {
		printf("DSI,STAGE,ERROR=engine\r\n");
		fflush(stdout);
		free(json);
		return NULL;
	}
	Tvg_Canvas canvas = tvg_swcanvas_create(TVG_ENGINE_OPTION_DEFAULT);

	if (!canvas) {
		printf("DSI,STAGE,ERROR=canvas\r\n");
		fflush(stdout);
		free(json);
		return NULL;
	}
	Tvg_Animation anim = tvg_lottie_animation_new();
	Tvg_Paint pic = tvg_animation_get_picture(anim);

	if (tvg_picture_load_data(pic, json, json_len, "lot", NULL,
				  true) != TVG_RESULT_SUCCESS) {
		printf("DSI,STAGE,ERROR=load\r\n");
		fflush(stdout);
		free(json);
		return NULL;
	}
	free(json);
	tvg_picture_set_size(pic, (float)STAGE_W, (float)STAGE_H);
	tvg_canvas_add(canvas, pic);

	float total_f = 0;

	tvg_animation_get_total_frame(anim, &total_f);
	int total = (total_f > 0) ? (int)total_f : 90;
	printf("DSI,STAGE,START,%dx%d@%dfps,total_frames=%d\r\n",
	       STAGE_W, STAGE_H, STAGE_FPS, total);
	fflush(stdout);

	int frame = 0;
	for (;;) {
		int64_t t0 = esp_timer_get_time();
		int back = -1;

		for (int i = 0; i < STAGE_NBUF; i++) {
			if (i != s_disp_idx && i != s_ready_idx) {
				back = i;
				break;
			}
		}
		if (back >= 0) {
			tvg_swcanvas_set_target(canvas, s_buf[back], STAGE_W,
						STAGE_W, STAGE_H,
						TVG_COLORSPACE_ARGB8888);
			tvg_animation_set_frame(anim,
						(float)(frame % total));
			tvg_canvas_update(canvas);
			tvg_canvas_draw(canvas, false);
			if (tvg_canvas_sync(canvas) != TVG_RESULT_SUCCESS)
				break;
			s_ready_idx = back;
			frame++;
			s_render_cnt++;
			/* 诊断：单帧渲染耗时 */
			{
				static int s_n;
				static int64_t s_sum;

				s_sum += esp_timer_get_time() - t0;
				if (++s_n % 30 == 0) {
					printf("DSI,STAGE,RENDER_MS,avg=%.1f\r\n",
					       (double)s_sum / s_n / 1000.0);
					fflush(stdout);
					s_n = 0;
					s_sum = 0;
				}
			}
		}

		int64_t el = esp_timer_get_time() - t0;
		int64_t period = 1000000LL / STAGE_FPS;

		if (el < period)
			vTaskDelay(pdMS_TO_TICKS((period - el) / 1000));
	}
	return NULL;
}

static void stage_timer_cb(lv_timer_t *t)
{
	static int sec;

	s_disp_cnt++;

	/* LVGL 任务内采纳最新就绪帧：指针切换只发生在 LVGL 上下文 */
	int cand = s_ready_idx;

	if (cand >= 0 && cand != s_disp_idx) {
		s_img_dsc.data = (const uint8_t *)s_buf[cand];
		lv_image_set_src(s_img, &s_img_dsc);
		lv_obj_invalidate(s_img);
		s_disp_idx = cand;
		s_ready_idx = -1;
	}

	if (++sec % 30 == 0) {
		printf("DSI,STAGE,STAT,render=%d,disp=%d\r\n", s_render_cnt,
		       s_disp_cnt);
		fflush(stdout);
		s_render_cnt = 0;
		s_disp_cnt = 0;
	}
}

int lottie_stage_start(void)
{
	pthread_attr_t attr;
	pthread_t th;

	for (int i = 0; i < STAGE_NBUF; i++) {
		s_buf[i] = heap_caps_malloc(STAGE_W * STAGE_H * 4,
					    MALLOC_CAP_SPIRAM);
		if (!s_buf[i]) {
			ESP_LOGE(TAG, "DSI,STAGE,ERROR=buf_alloc");
			return -1;
		}
		memset(s_buf[i], 0, STAGE_W * STAGE_H * 4);
	}
	s_disp_idx = 0;
	s_ready_idx = -1;

	lv_obj_t *scr = lv_obj_create(NULL);
	lv_obj_t *title = lv_label_create(scr);

	lv_obj_set_style_bg_color(scr, lv_color_hex(0x101828), 0);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

	s_img = lv_image_create(scr);
	s_img_dsc.data = (const uint8_t *)s_buf[0];
	lv_image_set_src(s_img, &s_img_dsc);
	lv_obj_align(s_img, LV_ALIGN_CENTER, 0, 40);
	lv_label_set_text(title, "Kaleido stage L0 (parallel 30fps)");
	lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);
	lv_screen_load(scr);

	s_stage_timer = lv_timer_create(stage_timer_cb, 16, NULL);

	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 64 * 1024);
	int rc = pthread_create(&th, &attr, stage_pthread, NULL);

	if (rc) {
		ESP_LOGE(TAG, "DSI,STAGE,ERROR=pthread rc=%d", rc);
		return -1;
	}
	pthread_detach(th);
	return 0;
}
