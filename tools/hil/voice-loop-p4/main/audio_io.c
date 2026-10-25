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
#include "freertos/FreeRTOS.h"
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
	i2c_master_bus_handle_t bus;

	ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

	i2s_chan_config_t chan_cfg =
		I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
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
		.bus_handle = bus,
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
	return 0;
}

int aio_capture(int16_t *mono, size_t samples, uint32_t timeout_ms)
{
	/* 立体声帧分块采回取左声道（S31 验证过的槽位无关方案） */
	static int16_t st[3200];
	size_t got_frames = 0;

	while (got_frames < samples) {
		size_t want = samples - got_frames;
		size_t need, got = 0;

		if (want > sizeof(st) / 4)
			want = sizeof(st) / 4;
		need = want * 4;
		while (got < need) {
			size_t n;

			if (i2s_channel_read(s_rx, (char *)st + got,
					     need - got, &n,
					     pdMS_TO_TICKS(timeout_ms)) != ESP_OK ||
			    n == 0)
				break;
			got += n;
		}
		size_t f = got / 4;

		for (size_t i = 0; i < f; i++)
			mono[got_frames + i] = st[2 * i];
		got_frames += f;
		if (f < want)
			break; /* 超时/断流 */
	}
	return got_frames;
}

int aio_play(const int16_t *mono, size_t samples, uint32_t timeout_ms)
{
	/* 双声道复制送出：codec 锁左锁右都能拿到数据 */
	static int16_t st[3200];
	size_t done = 0;

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
	return done;
}
