/*
 * bg_jpeg.c — V-36：JPEG 硬解能力封存（P4 JPEGDEC，esp_driver_jpeg）
 *
 * 产品定稿为纯色背景后本模块休眠（V-36 门禁：11ms/800×1280 已验证），
 * 壁纸/相框场景启用时：EMBED 资产 → bg_jpeg_init(资产指针, 长度) →
 * bg_jpeg_dsc() 垫到舞台底层。全屏合成只发生一次（首次全屏失效），
 * 之后每帧仍只重绘舞台 325² 区域，背景常驻 fb，逐帧开销不变。
 * 入参化为调用方传缓冲（而非自带 EMBED 符号），休眠期不占 flash。
 */
#include <stdio.h>
#include <string.h>

#include "driver/jpeg_decode.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

static const char *TAG = "bgjpeg";

static lv_image_dsc_t s_bg_dsc = {
	.header = {
		.w = 800,
		.h = 1280,
		.cf = LV_COLOR_FORMAT_RGB565,
	},
	.data = NULL,
	.data_size = 800 * 1280 * 2,
};

const lv_image_dsc_t *bg_jpeg_dsc(void)
{
	return s_bg_dsc.data ? &s_bg_dsc : NULL;
}

int bg_jpeg_init(const void *jpg, size_t jpg_len)
{
	const uint8_t *jpg_src = jpg;
	int64_t t0 = esp_timer_get_time();

	if (!jpg || !jpg_len) {
		ESP_LOGE(TAG, "DSI,BG,ERROR=no asset (dormant capability)");
		return -1;
	}

	/* 硬解输入/输出走 64 对齐 PSRAM（flash XIP 区不作 DMA 源） */
	uint8_t *jpg_buf = heap_caps_aligned_alloc(64, jpg_len,
						   MALLOC_CAP_SPIRAM);
	uint8_t *out = heap_caps_aligned_alloc(64, s_bg_dsc.data_size,
					       MALLOC_CAP_SPIRAM);
	if (!jpg_buf || !out) {
		ESP_LOGE(TAG, "DSI,BG,ERROR=alloc jpg=%d out=%d",
			 jpg_buf != NULL, out != NULL);
		heap_caps_free(jpg_buf);
		heap_caps_free(out);
		return -1;
	}
	memcpy(jpg_buf, jpg_src, jpg_len);

	jpeg_decoder_handle_t eng = NULL;
	jpeg_decode_engine_cfg_t eng_cfg = {
		.timeout_ms = 3000,
	};
	jpeg_decode_picture_info_t info = {};
	jpeg_decode_cfg_t dec_cfg = {
		.output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
		.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
		.conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
	};
	uint32_t out_size = 0;
	esp_err_t r = jpeg_new_decoder_engine(&eng_cfg, &eng) ||
		jpeg_decoder_get_info(jpg_buf, jpg_len, &info) ||
		jpeg_decoder_process(eng, &dec_cfg, jpg_buf, jpg_len, out,
				     s_bg_dsc.data_size, &out_size);

	if (r == ESP_OK) {
		jpeg_del_decoder_engine(eng);
	} else {
		if (eng)
			jpeg_del_decoder_engine(eng);
		heap_caps_free(jpg_buf);
		heap_caps_free(out);
		ESP_LOGE(TAG, "DSI,BG,ERROR=hw_decode rc=0x%x", r);
		return -1;
	}
	heap_caps_free(jpg_buf);

	int64_t ms = (esp_timer_get_time() - t0) / 1000;

	s_bg_dsc.data = out;
	printf("DSI,BG,DEC,ok,%ux%u,out=%u,jpg=%u,ms=%lld\r\n",
	       (unsigned)info.width, (unsigned)info.height,
	       (unsigned)out_size, (unsigned)jpg_len, ms);
	fflush(stdout);

	/* 字节序自检：资产左上角红/绿/蓝色块，期望 F800/07E0/001F */
	{
		static const int rows[] = { 28, 76, 124 };
		const uint16_t *px16 = (const uint16_t *)out;

		for (unsigned r = 0; r < 3; r++) {
			printf("DSI,BG,COLOR,row=%d:", rows[r]);
			for (int x = 16; x <= 48; x += 8)
				printf("%04X ", px16[rows[r] * 800 + x]);
			printf("\r\n");
		}
		fflush(stdout);
	}

	if (info.width != 800 || info.height != 1280) {
		ESP_LOGW(TAG, "DSI,BG,WARN,unexpected %ux%u", info.width,
			 info.height);
		return -1;
	}
	return 0;
}
