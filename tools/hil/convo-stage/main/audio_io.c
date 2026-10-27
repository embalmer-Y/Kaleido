/*
 * audio_io.c — ES8311 采集/播放（osptek P4C5 底板）
 *
 * 引脚依据 26 号调研 A1（原理图矢量走线三重验证）：
 *   I2C SDA=7 SCL=8（与 in-cell 触摸共总线，内部上拉兜底）；I2S MCLK=13
 *   BCLK=12 WS=10 DOUT=9（P4→DAC）DIN=11（ADC→P4）；NS4150 STD=GPIO53。
 * mono slot（P4 EV 板 BSP 同款形态）；采集/播放直通 I2S DMA，
 * 静态路径零每调用 malloc（B3 改进）。
 */
#include "audio_io.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/i2c_master.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "kaleido_core.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#define AIO_I2C_SDA	7
#define AIO_I2C_SCL	8
#define AIO_MCLK	13
#define AIO_BCLK	12
#define AIO_WS		10
#define AIO_DOUT	9
#define AIO_DIN		11
#define AIO_PA_PIN	53
#define AIO_SR		16000

static const char *TAG = "aio";
static i2s_chan_handle_t s_tx, s_rx;
static esp_codec_dev_handle_t s_in, s_out;
static i2c_master_bus_handle_t s_i2c_bus;

/* V-45：触摸与 ES8311 共享 I2C0（同 SDA7/SCL8 物理总线）——
 * 第二次 i2c_new_master_bus 同口必失败（INVALID_STATE abort） */
i2c_master_bus_handle_t aio_i2c_bus(void)
{
	return s_i2c_bus;
}
/* 立体声交织静态缓冲 st[] 是各函数私有状态：两个调用者同时进同一
 * 函数即互相踩写（wakenet 采集 vs 'g' 轮采集；beep vs TTS 播放）。
 * 采集/播放各一把互斥锁串行化（评审 2026-10-26 BUG#4）。
 * 锁语义为"尽力串行"：取锁失败返回 0 样本，调用方按超时处理 */
static SemaphoreHandle_t s_play_mux;

/* V-46a 采音所有权重构（V-45 缺陷族治本）：
 * 历史实现 aio_capture 整个多轮 I2S 读循环持 s_cap_mux——wakenet feed
 * （优先级 5）紧循环下低优先级消费者（菜单探针）2000ms 拿不到锁，
 * 实测 'l' capture=0（饿死）；浸泡期 TTS 喊话声学 miss 疑同族。
 * 现架构：单一采音任务 aio_cap 独占 I2S RX，左声道写 PSRAM 环；
 * aio_capture 语义不变（顺序流+超时），但退化为"环快照拷出"——
 * 等待在锁外、拷贝临界区微秒级，饥饿绝迹。V-46c（AEC）的 TX 参考
 * 环按同构落位即可。
 *   游标语义：单读游标 r（消费者仍互斥，所有权纪律在调用方/FSM）；
 *   掉队超过半环（新会话/探针）跳到最新，不吃陈旧积压。 */
#define AIO_RING_ORDER	17	/* 2^17 采样 = 8.2s @16k */
#define AIO_RING_MASK	((1u << AIO_RING_ORDER) - 1)
#define AIO_CAP_CHUNK	320	/* 20ms 立体声帧/写轮 */

static int16_t *s_ring;		/* PSRAM 单声道环 */
static volatile uint32_t s_w;	/* 写游标（绝对值，单调） */
static uint32_t s_r;		/* 读游标（消费者互斥下访问） */
static portMUX_TYPE s_ring_mux = portMUX_INITIALIZER_UNLOCKED;

static void cap_task(void *arg)
{
	static int16_t st[AIO_CAP_CHUNK * 2]; /* 立体声中转（左=奇? 左=偶） */
	size_t frames;

	(void)arg;
	for (;;) {
		size_t got = 0;

		while (got < sizeof(st)) {
			size_t n;

			if (i2s_channel_read(s_rx, (char *)st + got,
					     sizeof(st) - got, &n,
					     pdMS_TO_TICKS(100)) !=
			    ESP_OK || n == 0)
				break;
			got += n;
		}
		frames = got / 4;
		if (!frames)
			continue;
		/* 覆盖写：只推进写游标，落后读者按掉队策略自跳 */
		portENTER_CRITICAL(&s_ring_mux);
		for (size_t i = 0; i < frames; i++)
			s_ring[(s_w + i) & AIO_RING_MASK] = st[2 * i];
		s_w += frames;
		portEXIT_CRITICAL(&s_ring_mux);
		kf_beat("aio_cap");
	}
}

int aio_init(void)
{
	i2c_master_bus_config_t bus_cfg = {
		.i2c_port = 0,
		.sda_io_num = AIO_I2C_SDA,
		.scl_io_num = AIO_I2C_SCL,
		.clk_source = I2C_CLK_SRC_DEFAULT,
		.glitch_ignore_cnt = 7,
		.flags = { .enable_internal_pullup = 1 },
	};
	ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_i2c_bus));

	i2s_chan_config_t chan_cfg =
		I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
	/* 欠载时 DMA 默认循环重发最后一个缓冲区（实测：语音末字无限复读、
	 * 短音后持续蜂音）——必须开自动清零，欠载发静音。
	 * dma 8 描述符 × 320 帧 ≈160ms 缓冲：吸收播放任务被抢占的抖动 */
	chan_cfg.auto_clear = true;
	chan_cfg.dma_desc_num = 8;
	chan_cfg.dma_frame_num = 320;
	ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx, &s_rx));

	i2s_std_config_t std_cfg = {
		.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AIO_SR),
		.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
			I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
		.gpio_cfg = {
			.mclk = AIO_MCLK,
			.bclk = AIO_BCLK,
			.ws = AIO_WS,
			.dout = AIO_DOUT,
			.din = AIO_DIN,
		},
	};
	ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &std_cfg));
	ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx, &std_cfg));
	ESP_ERROR_CHECK(i2s_channel_enable(s_tx));
	ESP_ERROR_CHECK(i2s_channel_enable(s_rx));

	audio_codec_i2s_cfg_t i2s_cfg = {
		.port = 0, .rx_handle = s_rx, .tx_handle = s_tx,
	};
	const audio_codec_data_if_t *data_if =
		audio_codec_new_i2s_data(&i2s_cfg);
	if (!data_if)
		return -1;

	audio_codec_i2c_cfg_t i2c_cfg = {
		.addr = ES8311_CODEC_DEFAULT_ADDR,
		.bus_handle = s_i2c_bus,
		.clock_speed_hz = 400000,
	};
	const audio_codec_ctrl_if_t *ctrl_if =
		audio_codec_new_i2c_ctrl(&i2c_cfg);
	if (!ctrl_if)
		return -2;

	es8311_codec_cfg_t es_cfg = {
		.ctrl_if = ctrl_if,
		.codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
		.master_mode = false,
		.use_mclk = true,
		.digital_mic = false,
		.pa_pin = AIO_PA_PIN,
		.pa_reverted = false,
		.hw_gain = { .pa_voltage = 5.0 },
	};
	const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
	if (!codec_if)
		return -3;

	esp_codec_dev_cfg_t in_cfg = {
		.dev_type = ESP_CODEC_DEV_TYPE_IN,
		.codec_if = codec_if,
		.data_if = data_if,
	};
	s_in = esp_codec_dev_new(&in_cfg);
	esp_codec_dev_cfg_t out_cfg = {
		.dev_type = ESP_CODEC_DEV_TYPE_OUT,
		.codec_if = codec_if,
		.data_if = data_if,
	};
	s_out = esp_codec_dev_new(&out_cfg);
	if (!s_in || !s_out)
		return -4;
	s_play_mux = xSemaphoreCreateMutex();
	if (!s_play_mux)
		return -5;
	/* V-46a：PSRAM 环（256KB/8.2s）。⚠ 采音任务必须最后创建：
	 * esp_codec_dev_open 会重配 I2S 通道（全双工主从切换），并发
	 * i2s_channel_read 会与通道重配死锁（实测开机冻结于 i2s_common
	 * 告警行） */
	s_ring = heap_caps_malloc((1u << AIO_RING_ORDER) * sizeof(int16_t),
				  MALLOC_CAP_SPIRAM);
	if (!s_ring)
		return -6;

	esp_codec_dev_sample_info_t fs = {
		.sample_rate = AIO_SR,
		.channel = 2,
		.bits_per_sample = 16,
	};
	esp_codec_dev_set_in_gain(s_in, 30.0); /* 硅麦；首验后标定 */
	esp_codec_dev_set_out_vol(s_out, 60.0);
	esp_codec_dev_open(s_in, &fs);
	esp_codec_dev_open(s_out, &fs);
	/* 实证补丁（V-38）：es8311 驱动对 pa_pin 的 gpio 配置在本板不生效
	 *（疑似与启动期全脚 sleep 隔离配置冲突）——codec open 后手动重配
	 * 该脚为推挽输出并拉高，'p'/'l' 探测两法均证实此路径出声 */
	{
		gpio_config_t g = {
			.pin_bit_mask = 1ULL << AIO_PA_PIN,
			.mode = GPIO_MODE_OUTPUT,
		};

		gpio_config(&g);
		gpio_set_level(AIO_PA_PIN, 1);
	}
	ESP_LOGI(TAG, "es8311 ready (adc+dac, 16k stereo-slot, pa=%d forced)",
		 AIO_PA_PIN);
	if (xTaskCreate(cap_task, "aio_cap", 4096, NULL, 5, NULL) != pdPASS)
		return -7;	/* codec 已起，任务失败由调用方处置 */
	return 0;
}

int aio_capture_stream(int16_t *mono, size_t samples, uint32_t timeout_ms,
		       uint32_t *cursor)
{
	int64_t deadline = esp_timer_get_time() +
			   (int64_t)timeout_ms * 1000;

	if (!s_ring || !cursor)
		return 0;
	/* 消费者私有游标：无锁读环（写者只覆盖落后整环的数据，游标
	 * 掉队超半环即自跳最新——调用方因此天然"从现在开始听"） */
	uint32_t c = *cursor;
	uint32_t w = s_w;

	if (w - c > (1u << (AIO_RING_ORDER - 1)))
		c = w;
	size_t want = samples;

	while (w - c < want) {
		if (esp_timer_get_time() >= deadline) {
			want = w - c;	/* 超时：有多少给多少 */
			break;
		}
		vTaskDelay(pdMS_TO_TICKS(2));
		w = s_w;
		if (w - c > (1u << (AIO_RING_ORDER - 1)))
			c = w;		/* 写者已绕圈覆盖：跳最新 */
	}
	for (size_t off = 0; off < want; ) {
		uint32_t pos = (c + off) & AIO_RING_MASK;
		size_t n = want - off;

		if (n > (uint32_t)(AIO_RING_MASK + 1) - pos)
			n = (uint32_t)(AIO_RING_MASK + 1) - pos;
		memcpy(mono + off, s_ring + pos, n * sizeof(int16_t));
		off += n;
	}
	*cursor = c + want;
	return want;
}

int aio_play(const int16_t *mono, size_t samples, uint32_t timeout_ms)
{
	/* 双声道复制送出：codec 锁左锁右都能拿到数据 */
	static int16_t st[3200];
	size_t done = 0;

	if (!s_play_mux ||
	    xSemaphoreTake(s_play_mux, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
		return 0;
	while (done < samples) {
		size_t n = samples - done;

		if (n > sizeof(st) / 4)
			n = sizeof(st) / 4;
		for (size_t i = 0; i < n; i++) {
			st[2 * i] = mono[done + i];
			st[2 * i + 1] = mono[done + i];
		}
		size_t wrote = 0;

		while (wrote < n * 4) {
			size_t m;

			if (i2s_channel_write(s_tx,
					      (const char *)st + wrote,
					      n * 4 - wrote, &m,
					      pdMS_TO_TICKS(timeout_ms)) != ESP_OK ||
			    m == 0)
				break;
			wrote += m;
		}
		if (wrote < n * 4)
			break;
		done += n;
	}
	xSemaphoreGive(s_play_mux);
	return done;
}
