/*
 * kd_wakenet — V-04 唤醒词真机实测固件（ESP32-S31-Korvo-1 V1.1）
 *
 * 链路：2×模拟麦 → ES8389 ADC（I2C0@0x20, SDA=0/SCL=1）→ I2S0 std
 *       （BCLK=3, WS=4, DIN=6, 16kHz 立体声）→ esp-sr AFE → WakeNet9 (Hi,乐鑫)
 * 输出：检出事件（词/模型/通道/音量/计数/内存）串口打印，供 HIL 统计。
 * 板级事实：kaleido-fw/docs/korvo1-板级事实.md
 */
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8389_codec.h"

#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_wn_iface.h"
#include "model_path.h"

static const char *TAG = "kdwn";

#define I2C_SDA 0
#define I2C_SCL 1
#define I2S_BCLK 3
#define I2S_WS 4
#define I2S_DIN 6
#define RECORD_SR 16000

static esp_codec_dev_handle_t s_codec;
static i2s_chan_handle_t s_rx_chan;
static const esp_afe_sr_iface_t *s_afe;
static esp_afe_sr_data_t *s_afe_data;
static volatile int s_detect_count;

static int codec_init(void)
{
	i2c_master_bus_config_t bus_cfg = {
	    .i2c_port = 0,
	    .sda_io_num = I2C_SDA,
	    .scl_io_num = I2C_SCL,
	    .clk_source = I2C_CLK_SRC_DEFAULT,
	    .glitch_ignore_cnt = 7,
	};
	i2c_master_bus_handle_t bus;
	ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

	/* 全双工对：esp_codec_dev 的 RX 时钟依赖 TX 通道存在 */
	i2s_chan_handle_t tx = NULL, rx = NULL;
	i2s_chan_config_t chan_cfg =
	    I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
	chan_cfg.dma_desc_num = 6;
	chan_cfg.dma_frame_num = 320;
	ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx, &rx));

	i2s_std_config_t std_cfg = {
	    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(RECORD_SR),
	    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
		I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
	    .gpio_cfg =
		{
		    .mclk = I2S_GPIO_UNUSED,
		    .bclk = I2S_BCLK,
		    .ws = I2S_WS,
		    .dout = I2S_GPIO_UNUSED,
		    .din = I2S_DIN,
		},
	};
	ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx, &std_cfg));
	ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx, &std_cfg));
	ESP_ERROR_CHECK(i2s_channel_enable(tx));
	ESP_ERROR_CHECK(i2s_channel_enable(rx));

	audio_codec_i2s_cfg_t i2s_cfg = {
	    .port = 0, .rx_handle = rx, .tx_handle = tx};
	const audio_codec_data_if_t *data_if =
	    audio_codec_new_i2s_data(&i2s_cfg);
	if (!data_if)
		return -1;
	s_rx_chan = rx; /* 采集直读 I2S（codec_dev 仅做寄存器配置） */

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
	    .codec_mode = ESP_CODEC_DEV_WORK_MODE_ADC,
	    .master_mode = false,
	    .use_mclk = false,
	    .no_dac_ref = true,
	};
	const audio_codec_if_t *codec_if = es8389_codec_new(&es_cfg);
	if (!codec_if)
		return -3;

	esp_codec_dev_cfg_t dev_cfg = {
	    .dev_type = ESP_CODEC_DEV_TYPE_IN,
	    .codec_if = codec_if,
	    .data_if = data_if,
	};
	s_codec = esp_codec_dev_new(&dev_cfg);
	if (!s_codec)
		return -4;

	ESP_ERROR_CHECK(esp_codec_dev_set_in_gain(
	    s_codec, 24.0)); /* 42dB 会削顶，降到 24dB */
	esp_codec_dev_sample_info_t fs = {
	    .sample_rate = RECORD_SR,
	    .channel = 2,
	    .bits_per_sample = 16,
	};
	ESP_ERROR_CHECK(esp_codec_dev_open(s_codec, &fs));
	return 0;
}

static void detect_task(void *arg)
{
	(void)arg;
	int loops = 0;
	TickType_t last_diag = xTaskGetTickCount();
	while (1) {
		afe_fetch_result_t *res = s_afe->fetch(s_afe_data);
		if (!res || res->ret_value == ESP_FAIL) {
			vTaskDelay(pdMS_TO_TICKS(5));
			continue;
		}
		loops++;
		if (res->wakeup_state == WAKENET_DETECTED) {
			s_detect_count++;
			ESP_LOGW(TAG,
				 "★ WAKE #%d word_idx=%d model_idx=%d ch=%d "
				 "vol=%.1f dBFS heap=%" PRIu32,
				 s_detect_count, res->wake_word_index,
				 res->wakenet_model_index,
				 res->trigger_channel_id, res->data_volume,
				 esp_get_free_heap_size());
		}
		/* 诊断：每 3 秒打印 AFE 输出状态（验证处理链活着） */
		if ((xTaskGetTickCount() - last_diag) > pdMS_TO_TICKS(3000)) {
			last_diag = xTaskGetTickCount();
			ESP_LOGI(TAG,
				 "diag loops=%d state=%d vad=%d vol=%.1f "
				 "wlen=%d det=%d",
				 loops, (int)res->wakeup_state,
				 (int)res->vad_state, res->data_volume,
				 res->wake_word_length, s_detect_count);
		}
	}
}

void app_main(void)
{
	ESP_LOGI(TAG, "V-04 wakenet boot, heap=%" PRIu32,
		 esp_get_free_heap_size());

	srmodel_list_t *models = esp_srmodel_init("model");
	assert(models);
	ESP_LOGI(TAG, "models=%d", models->num);
	for (int i = 0; i < models->num; i++) {
		ESP_LOGI(TAG, "  model[%d]=%s", i, models->model_name[i]);
	}

	afe_config_t *afe_cfg =
	    afe_config_init("MM", models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
	assert(afe_cfg);
	afe_config_print(afe_cfg);

	const esp_afe_sr_iface_t *afe = esp_afe_handle_from_config(afe_cfg);
	assert(afe);

	ESP_LOGI(TAG, "codec init…");
	int rc = codec_init();
	if (rc != 0) {
		ESP_LOGE(TAG, "codec init failed rc=%d", rc);
		return;
	}
	ESP_LOGI(TAG, "codec OK, heap=%" PRIu32, esp_get_free_heap_size());

	s_afe = afe;
	s_afe_data = afe->create_from_config(afe_cfg);
	assert(s_afe_data);
	afe->print_pipeline(s_afe_data);
	/* S31 自动挂载缺失——显式添加 WakeNet 模型 */
	int wn_added = afe->add_wakenet_model(s_afe_data, "wn9s_hiesp");
	ESP_LOGW(TAG, "add_wakenet_model(wn9s_hiesp) → %d", wn_added);
	afe->print_pipeline(s_afe_data);
	afe->set_wakenet_threshold(s_afe_data, 1,
				   0.60); /* 诊断期放宽，正式参数待标定 */

	const int chunk = afe->get_feed_chunksize(s_afe_data);
	const int ch_num = afe->get_feed_channel_num(s_afe_data);
	const int rate = afe->get_samp_rate(s_afe_data);
	ESP_LOGI(TAG, "AFE chunk=%d feed_ch=%d rate=%d", chunk, ch_num, rate);
	ESP_LOGW(TAG, "=== 现在请对板子说「Hi,乐鑫」或「Hi,ESP」 ===");

	xTaskCreatePinnedToCore(detect_task, "det", 4096, NULL, 5, NULL, 1);

	const int frame_bytes = chunk * ch_num * sizeof(int16_t);
	int16_t *buf = malloc(frame_bytes);
	assert(buf);
	int reads = 0;
	TickType_t last_hb = xTaskGetTickCount();
	int peakL = 0, peakR = 0;
	int64_t sum_sq = 0;
	int samples = 0;
	int consec_fail = 0;
	while (1) {
		/* 分片续读：部分数据不丢弃；顽固失败重启 DMA 通道 */
		size_t total = 0;
		bool ok = true;
		TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(600);
		while (total < (size_t)frame_bytes) {
			size_t got = 0;
			esp_err_t err = i2s_channel_read(
			    s_rx_chan, (uint8_t *)buf + total,
			    frame_bytes - total, &got, pdMS_TO_TICKS(200));
			total += got;
			if (err != ESP_OK && got == 0) {
				ok = false;
				break;
			}
			if ((int32_t)(deadline - xTaskGetTickCount()) <= 0 &&
			    total < (size_t)frame_bytes) {
				ok = false;
				break;
			}
		}
		if (!ok) {
			consec_fail++;
			if (consec_fail % 20 == 1) {
				ESP_LOGE(TAG, "i2s starved x%d — recycle DMA",
					 consec_fail);
				i2s_channel_disable(s_rx_chan);
				vTaskDelay(pdMS_TO_TICKS(5));
				i2s_channel_enable(s_rx_chan);
			}
			vTaskDelay(pdMS_TO_TICKS(10));
			continue;
		}
		consec_fail = 0;
		/* 电平统计：L/R 交错 */
		int n_sm = frame_bytes / 2 / 2;
		for (int i = 0; i < n_sm; i++) {
			int l = buf[2 * i], r = buf[2 * i + 1];
			int al = l < 0 ? -l : l, ar = r < 0 ? -r : r;
			if (al > peakL)
				peakL = al;
			if (ar > peakR)
				peakR = ar;
			sum_sq += (int64_t)l * l;
			samples++;
		}
		afe->feed(s_afe_data, buf);
		reads++;
		if ((xTaskGetTickCount() - last_hb) > pdMS_TO_TICKS(15000)) {
			last_hb = xTaskGetTickCount();
			int rms = samples ? (int)(sum_sq / samples) : 0;
			ESP_LOGI(TAG,
				 "alive reads=%d det=%d peakL=%d peakR=%d "
				 "rms=%d heap=%" PRIu32,
				 reads, s_detect_count, peakL, peakR, rms,
				 esp_get_free_heap_size());
			peakL = peakR = 0;
			sum_sq = 0;
			samples = 0;
		}
	}
}
