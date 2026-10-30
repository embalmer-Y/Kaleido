/*
 * audio_io.c — ES8389 采集/播放（I2S0 全双工，V-04 采集路径 + DAC 补全）
 */
#include "audio_io.h"

#include <string.h>

#include "driver/i2s_std.h"
#include "driver/i2c_master.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8389_codec.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "esp_log.h"

#define AIO_I2C_SDA	0
#define AIO_I2C_SCL	1
#define AIO_BCLK	3
#define AIO_WS		4
#define AIO_DOUT	5
#define AIO_DIN		6
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
			.mclk = I2S_GPIO_UNUSED,
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
		.addr = ES8389_CODEC_DEFAULT_ADDR,
		.bus_handle = bus,
		.clock_speed_hz = 400000,
	};
	const audio_codec_ctrl_if_t *ctrl_if =
		audio_codec_new_i2c_ctrl(&i2c_cfg);
	if (!ctrl_if)
		return -2;

	es8389_codec_cfg_t es_cfg = {
		.ctrl_if = ctrl_if,
		.codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
		.master_mode = false,
		.use_mclk = false,
		.no_dac_ref = true,
	};
	const audio_codec_if_t *codec_if = es8389_codec_new(&es_cfg);
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
	esp_codec_dev_set_in_gain(s_in, 24.0); /* V-04：42dB 削顶→24dB */
	esp_codec_dev_set_out_vol(s_out, 70.0);
	esp_codec_dev_open(s_in, &fs);
	esp_codec_dev_open(s_out, &fs);
	ESP_LOGI(TAG, "es8389 ready (adc+dac, 16k stereo slots)");
	return 0;
}

int aio_capture(int16_t *mono, size_t samples, uint32_t timeout_ms)
{
	size_t frames = samples;             /* 1 frame = 2 ch × 16bit */
	int16_t *st = malloc(frames * 4);

	if (!st)
		return -1;
	size_t got = 0;
	while (got < frames * 4) {
		size_t n;
		if (i2s_channel_read(s_rx, (char *)st + got,
				     frames * 4 - got, &n,
				     pdMS_TO_TICKS(timeout_ms)) != ESP_OK ||
		    n == 0)
			break;
		got += n;
	}
	size_t frames_got = got / 4;
	for (size_t i = 0; i < frames_got; i++)
		mono[i] = st[2 * i];          /* 左声道           */
	free(st);
	return frames_got;
}

int aio_play(const int16_t *mono, size_t samples, uint32_t timeout_ms)
{
	int16_t *st = malloc(samples * 4);

	if (!st)
		return -1;
	for (size_t i = 0; i < samples; i++) {
		st[2 * i] = mono[i];          /* DAC 走左声道出   */
		st[2 * i + 1] = mono[i];
	}
	size_t wrote = 0;
	while (wrote < samples * 4) {
		size_t n;
		if (i2s_channel_write(s_tx, (char *)st + wrote,
				      samples * 4 - wrote, &n,
				      pdMS_TO_TICKS(timeout_ms)) != ESP_OK ||
		    n == 0)
			break;
		wrote += n;
	}
	free(st);
	return wrote / 4;
}
