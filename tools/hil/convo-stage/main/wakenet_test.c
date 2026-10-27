/*
 * wakenet_test.c — WakeNet 单麦唤醒（M2 前置，esp-sr 2.6.0）
 *
 * V-46c 定形：语音前端常驻——"MR"+AEC 双通道 AFE（V-46b 基线参数：
 * 连续排布/对齐 offset/默认档），撤 V-43 播放期静音窗口，自唤醒免疫
 * 改由 AEC 结构性承担。唤醒路由：IDLE/OFF=常规唤醒（双短鸣）；SPEAKING=
 * 真打断（清播放+转听，≤1s 门禁）；LISTENING 中命中忽略（防重触发）。
 * 'w'：确认常驻（已驻留则幂等）
 * 'W'：TTS 回环自验——语义随 AEC 反转：设备自说的唤醒词被 AEC 抑制，
 *      hits=自唤醒泄漏率（越低越好，门禁 ≤1/10）
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "model_path.h"

#include "audio_io.h"
#include "convo_fsm.h"
#include "esp_lvgl_port.h"
#include "kaleido_core.h"
#include "kaleido_speech.h"
#include "emote_stage.h"

/* convo_stage_main：V-46c 打断——播放缓冲排干 + TX 中止 */
void pb_abort_playback(void);
/* V-46b 对齐 offset（'R' 实测；未测返回负 → feed 用 DMA 深度兜底） */
int32_t aec_align_offset(void);

/* voice_loop_main.c 的 TTS PCM → 扬声器缓冲（s_pb）转发 */
extern void on_tts_pcm(void *user, const uint8_t *pcm, size_t len);
void wakenet_start(struct ks_tts_cfg *tts, bool selftest,
		   void (*on_wake)(void));

static const esp_afe_sr_iface_t *s_afe;
static esp_afe_sr_data_t *s_afe_data;
static volatile bool s_running;		/* 重入守卫：AFE/模型只初始化一次 */
static volatile bool s_selftest;	/* 'W' 自检进行中（主菜单互斥门用） */
static void (*s_on_wake)(void);		/* FSM 挂点：唤醒即通知（可 NULL） */
static volatile int s_wake_cnt;
static volatile int64_t s_speak_us;	/* TTS 开口时刻（延迟基准） */
static StackType_t *s_say_stk;		/* wn_say PSRAM 栈槽 */
static StaticTask_t s_say_tcb;
static int16_t *s_beep;			/* 300ms 提示音（9.6KB，PSRAM 懒分配
					 * ——V-46c 内部水位线） */

static void beep_play_task(void *arg)
{
	(void)arg;
	aio_play(s_beep, 4800, 3000);
	vTaskDelay(pdMS_TO_TICKS(80));
	aio_play(s_beep, 4800, 3000);	/* 双短鸣 = 已唤醒 */
	vTaskDelete(NULL);
}

static void feed_task(void *arg)
{
	int chunk = s_afe->get_feed_chunksize(s_afe_data);
	int16_t *buf = malloc(chunk * sizeof(int16_t));
	static uint32_t feed_cur;	/* V-46a：私有采音游标 */

	(void)arg;
	if (!buf) {
		printf("VL,WN,ERROR=feed_alloc\r\n");
		fflush(stdout);
		vTaskDelete(NULL);
		return;
	}
	printf("VL,WN,FEED,chunk=%d\r\n", chunk);
	fflush(stdout);
	/* 核 0 喂音 / 核 1 检测（创建处绑核）。静音窗口恢复（V-46c
	 * 回退）：AEC 驻留被 esp-sr AEC3 FFT 病理阻断（追记#7），
	 * 自唤醒免疫暂由静音窗承担；AEC 修复后撤窗换打断监听 */
	for (;;) {
		while (convo_state_get() != CONVO_OFF &&
		       convo_state_get() != CONVO_IDLE)
			vTaskDelay(pdMS_TO_TICKS(50));
		int n = aio_capture_stream(buf, chunk, 2000, &feed_cur);

		if (n < chunk)
			memset(buf + n, 0, (chunk - n) * sizeof(int16_t));
		s_afe->feed(s_afe_data, buf);
	}
}

static void detect_task(void *arg)
{
	(void)arg;
	for (;;) {
		afe_fetch_result_t *r = s_afe->fetch(s_afe_data);

		if (!r || r->wakeup_state != WAKENET_DETECTED)
			continue;
		int64_t lat = s_speak_us ?
			(esp_timer_get_time() - s_speak_us) / 1000 : -1;
		enum convo_state st = convo_state_get();

		s_wake_cnt++;
		if (st == CONVO_SPEAKING) {
			/* V-46c 真打断：播放中止（aio_tx_abort 100ms 块粒
			 * 切写 + pb 排干）+ 唤醒事件（轮收尾回 IDLE 后
			 * 即开 LISTENING）。≤1s 停播转听门禁 */
			pb_abort_playback();
			printf("VL,WN,BARGE,cnt=%d,vol=%.1f\r\n",
			       s_wake_cnt, r->data_volume);
			fflush(stdout);
			kf_ev("wn", "barge-in");
			if (s_on_wake)
				s_on_wake();
			continue;	/* 打断即听，不鸣笛抢话 */
		}
		if (st == CONVO_LISTENING || st == CONVO_THINKING)
			continue;	/* 会话进行中命中：防重触发 */
		printf("VL,WN,WAKE,cnt=%d,lat_ms=%lld,vol=%.1f\r\n",
		       s_wake_cnt, lat, r->data_volume);
		fflush(stdout);
		if (s_on_wake)
			s_on_wake();	/* 通知 FSM（免按键轮起点） */
		xTaskCreatePinnedToCore(beep_play_task, "beep", 4096,
					NULL, 6, NULL, 0);
	}
}

/* TTS 喊话任务：周期说唤醒词，喂给回环自验 */
static void announce_task(void *arg)
{
	struct ks_tts_cfg *cfg = arg;
	const int cycles = 10;
	int hits0 = s_wake_cnt;

	for (int i = 0; i < cycles; i++) {
		/* dashscope 限制：finish 后同会话续喂新文本报缺 task_group，
		 * 每轮独立会话（自验场景多花 ~0.5s 开销可接受） */
		if (ks_tts_open(cfg, &(struct ks_tts_events){
				.on_pcm = on_tts_pcm,
			}) != 0) {
			printf("VL,WN,ERROR=tts_open,i=%d\r\n", i);
			fflush(stdout);
			vTaskDelay(pdMS_TO_TICKS(3000));
			continue;
		}
		printf("VL,WN,SPEAK,i=%d\r\n", i);
		fflush(stdout);
		s_speak_us = esp_timer_get_time();
		ks_tts_feed("你好小智", false, 15000);
		ks_tts_feed(NULL, true, 15000);
		ks_tts_close();
		vTaskDelay(pdMS_TO_TICKS(7000));
	}
	vTaskDelay(pdMS_TO_TICKS(1500));	/* 等尾音抵达检测 */
	/* hits 语义（V-46c 反转）：设备自说的唤醒词应被 AEC 抑制，
	 * hits=自唤醒泄漏率，越低越好（门禁 ≤1/10） */
	printf("VL,WN,SELFTEST,done,hits=%d/%d,leak=lower-better\r\n",
	       s_wake_cnt - hits0, cycles);
	fflush(stdout);
	s_selftest = false;
	vTaskDelete(NULL);
}

bool wakenet_selftest_busy(void)
{
	return s_selftest;
}

void wakenet_start(struct ks_tts_cfg *tts, bool selftest,
		   void (*on_wake)(void))
{
	uint32_t i0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
	uint32_t p0 = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
	srmodel_list_t *models;

	/* 二次进入会重复 esp_srmodel_init（泄模型表）+ 二次 create AFE
	 * （数百 KB）并覆盖 s_afe/s_afe_data——旧 feed/detect 任务仍在
	 * 按新指针跑，双重喂入 + OOM。V-46c 常驻后 'w'/'c' 均走此分支 */
	if (s_running) {
		/* 已在跑：补挂 FSM 回调；'W' 允许原地起自验（不重建 AFE） */
		if (on_wake)
			s_on_wake = on_wake;
		if (selftest && !s_selftest) {
			s_selftest = true;
			s_speak_us = 0;
			kf_task_psram("wn_say", 10240, announce_task, tts,
				      4, 0, (void **)&s_say_stk,
				      &s_say_tcb);
			printf("VL,WN,LISTENING,selftest\r\n");
			fflush(stdout);
			return;
		}
		printf("VL,WN,ALREADY_RUNNING\r\n");
		fflush(stdout);
		return;
	}
	s_running = true;
	s_selftest = selftest;
	s_on_wake = on_wake;

	/* V-45 第二阻断隔离包装：死亡点在 srmodel_init/create_from_config
	 * 且仅显示链活着时发生。顺序严格：①先停表情动画（同步等 gfx
	 * 任务 ack，此刻无人再碰 LVGL/PSRAM 渲染）②再持 LVGL 互斥干净
	 * 停掉 LVGL 刷新。不得反过来（lvgl→gfx 反向嵌套会与 flush_cb
	 * 的 gfx→lvgl 成 ABBA 死锁——锁纪律见 emote_stage.c 文件头） */
	if (!emote_stage_pause(true)) {
		/* 隔离降级不许静默：记日志后中止本次加载（渲染未停就跑
		 * 模型加载=重蹈 V-45 缓存维护死锁——宁可让用户重按） */
		kf_ev("wn", "pause_timeout");
		printf("VL,WN,ERROR=emote_pause_timeout\r\n");
		fflush(stdout);
		s_running = false;
		return;
	}
	bool lvgl_held = lvgl_port_lock(2000);
	printf("VL,WN,STAGE,srmodel_begin\r\n");
	fflush(stdout);
	models = esp_srmodel_init("model");
	printf("VL,WN,STAGE,srmodel_end,n=%d\r\n",
	       models ? models->num : -1);
	fflush(stdout);

	if (!models || models->num == 0) {
		printf("VL,WN,ERROR=no_model,check CONFIG_SR_WN_*\r\n");
		fflush(stdout);
		goto out_unfreeze;
	}
	/* AEC 驻留挂起（V-46c）：esp-sr 预编译 AEC3 在真实参考数据下
	 * FFT 无限循环（wn_feed 热旋、TWDT 回溯钉 dl_rfft；PSRAM/内部
	 * 分配两态对照同挂，convo 栈排除）——证据包入门禁追记#7，
	 * 待上游修复或下轮专攻。暂回 "M" 无 AEC + 静音窗（已证稳定），
	 * 打断管线保留（pb/TX 中止+路由），AEC 修复即配置翻转 */
	afe_config_t *cfg = afe_config_init("M", models, AFE_TYPE_SR,
					    AFE_MODE_HIGH_PERF);

	cfg->aec_init = false;
	cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
	cfg = afe_config_check(cfg);
	printf("VL,WN,STAGE,handle_begin\r\n");
	fflush(stdout);
	s_afe = esp_afe_handle_from_config(cfg);
	if (!s_afe) {
		printf("VL,WN,ERROR=no_afe_handle\r\n");
		fflush(stdout);
		goto out_unfreeze;
	}
	printf("VL,WN,STAGE,create_begin,int=%uk\r\n",
	       (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10));
	fflush(stdout);
	s_afe_data = s_afe->create_from_config(cfg);
	printf("VL,WN,STAGE,create_end,rc=%d\r\n", s_afe_data ? 0 : -1);
	fflush(stdout);
	if (!s_afe_data) {
		printf("VL,WN,ERROR=afe_create\r\n");
		fflush(stdout);
		goto out_unfreeze;
	}
out_unfreeze:
	if (lvgl_held)
		lvgl_port_unlock();
	emote_stage_pause(false);
	if (!models || models->num == 0 || !s_afe || !s_afe_data)
		return;
	kf_ev("wn", "loaded int=-%uKB", (unsigned)(
		      (i0 - heap_caps_get_free_size(MALLOC_CAP_INTERNAL)) >> 10));
	printf("VL,WN,INIT,ok,model=%s,int=-%uKB,ps=-%uKB\r\n",
	       models->model_name[0] ? models->model_name[0] : "?",
	       (unsigned)((i0 - heap_caps_get_free_size(MALLOC_CAP_INTERNAL)) >> 10),
	       (unsigned)((p0 - heap_caps_get_free_size(MALLOC_CAP_SPIRAM)) >> 10));
	fflush(stdout);

	if (!s_beep)
		s_beep = heap_caps_malloc(sizeof(int16_t) * 4800,
					  MALLOC_CAP_SPIRAM);
	if (!s_beep) {
		printf("VL,WN,ERROR=beep_alloc\r\n");
		fflush(stdout);
	} else
	for (size_t i = 0; i < 4800; i++)
		s_beep[i] = (int16_t)(6000.0f *
				sinf(2.0f * (float)M_PI * 880.0f * (float)i /
				     16000.0f));

	xTaskCreatePinnedToCore(feed_task, "wn_feed", 4096, NULL, 5, NULL, 0);
	xTaskCreatePinnedToCore(detect_task, "wn_det", 8192, NULL, 5, NULL, 1);
	if (selftest)
		/* 10240 栈：与 ttsfeeder 同款——announce 内跑完整
		 * MiniMax HTTPS+TLS（esp_http_client+mbedtls） */
		xTaskCreatePinnedToCore(announce_task, "wn_say", 10240, tts,
					4, NULL, 0);
	printf("VL,WN,LISTENING%s\r\n", selftest ? ",selftest" : "");
	fflush(stdout);
}
