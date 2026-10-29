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

#include "soul.h"

#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "model_path.h"

#include "audio_io.h"
#include "clause_pump.h"
#include "convo_fsm.h"
#include "kaleido_core.h"
#include "kaleido_speech.h"

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
	int16_t *mic = heap_caps_aligned_alloc(16, chunk * 2,
					       MALLOC_CAP_SPIRAM);
	int16_t *ref = heap_caps_aligned_alloc(16, chunk * 2,
					       MALLOC_CAP_SPIRAM);
	int16_t *buf = heap_caps_aligned_alloc(16, chunk * 2 * 2,
					       MALLOC_CAP_SPIRAM);
	static uint32_t feed_cur;	/* V-46a：私有采音游标 */

	(void)arg;
	if (!mic || !ref || !buf) {
		kf_log("VL,WN,ERROR=feed_alloc\r\n");
		vTaskDelete(NULL);
		return;
	}
	/* 阈值 0.35（V-46 沿用值；mono 路径下 -44dB 真人声全命中，余量
	 * 充足——0.05 探底值已完成历史使命回滚） */
	s_afe->set_wakenet_threshold(s_afe_data, 0, 0.35f);
	kf_log("VL,WN,FEED,chunk=%d,mono40,thresh=0.35\r\n", chunk);
	uint32_t feed_dbg = 0;
	/* 核 0 喂音 / 核 1 检测（创建处绑核）。静音窗口撤销（V-46c 终局
	 * ——FD_LOW_COST 实现分支绕开 SR 档 FFT 病理，杀手 soak 10/10）：
	 * 全天候喂麦，自唤醒免疫由 AEC 承担，SPEAKING 命中=BARGE 打断 */
	for (;;) {
		int n = aio_capture_stream(mic, chunk, 2000, &feed_cur);

		if (n < chunk)
			memset(mic + n, 0, (chunk - n) * sizeof(int16_t));
#if WN_V40_MONO
		/* M 单声道：无参考通道，直接喂麦 */
		if (kf_dbg_get() && ++feed_dbg % 25 == 0) {
			int64_t sq = 0;

			for (int i = 0; i < chunk; i++)
				sq += (int32_t)mic[i] * mic[i];
			kf_log("WN,DBGIN,n=%d,rms=%.1f,mono40", chunk,
			       sqrtf((float)sq / chunk));
		}
		s_afe->feed(s_afe_data, mic);
		continue;
#endif
		int32_t off = aec_align_offset();

		if (off < 0)
			off = 2560;
		if (!aio_ref_get(ref, chunk,
				 aio_ref_now() - (uint32_t)off -
				 (uint32_t)chunk))
			memset(ref, 0, chunk * 2);
		memcpy(buf, mic, chunk * 2);
		memcpy(buf + chunk, ref, chunk * 2);
		/* 调试仪表：喂入侧 RMS（与 detect 侧 DBGVOL 对账，分界
		 * AFE 内外）；ref=RMS 参考通道（唤醒修复验证：静默期应
		 * 为 0，若持续非零=冻结窗口残影仍在） */
		if (kf_dbg_get() && ++feed_dbg % 25 == 0) {
			int64_t sq = 0, rq = 0;

			for (int i = 0; i < chunk; i++) {
				sq += (int32_t)mic[i] * mic[i];
				rq += (int32_t)ref[i] * ref[i];
			}
			kf_log("WN,DBGIN,n=%d,rms=%.1f,ref=%.1f", chunk,
			       sqrtf((float)sq / chunk),
			       sqrtf((float)rq / chunk));
		}
		s_afe->feed(s_afe_data, buf);
	}
}

static void detect_task(void *arg)
{
	(void)arg;
	uint32_t dbg_n = 0;

	for (;;) {
		afe_fetch_result_t *r = s_afe->fetch(s_afe_data);

		if (!r)
			continue;
		/* 调试仪表（'v' 态）：每 50 块报 wakenet 实际听到的响度，
		 * 区分"音频未达检测"与"模型不识别"；out=AFE 输出 RMS
		 * （模型真正吃的数据——唤醒回归排查 2026-10-29：输入侧
		 * data_volume 正常而 0.05 阈值零触发，需分辨 AEC/NLP 是否
		 * 在输出侧吞掉一切） */
		if (kf_dbg_get() && ++dbg_n % 50 == 0) {
			int64_t osq = 0;
			int on = r->data_size / 2;

			for (int i = 0; i < on; i++)
				osq += (int32_t)r->data[i] * r->data[i];
			kf_log("WN,DBGVOL,vol=%.1f,out=%.1f,n=%d",
			       r->data_volume,
			       on ? sqrtf((float)osq / on) : -1.0f, on);
		}
		if (r->wakeup_state != WAKENET_DETECTED)
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
			kf_log("VL,WN,BARGE,cnt=%d,vol=%.1f\r\n",
			       s_wake_cnt, r->data_volume);
			kf_ev("wn", "barge-in");
			soul_on_barge();	/* W1：强在场信号 */
			if (s_on_wake)
				s_on_wake();
			continue;	/* 打断即听，不鸣笛抢话 */
		}
		if (st == CONVO_LISTENING || st == CONVO_THINKING)
			continue;	/* 会话进行中命中：防重触发 */
		kf_log("VL,WN,WAKE,cnt=%d,lat_ms=%lld,vol=%.1f\r\n",
		       s_wake_cnt, lat, r->data_volume);
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
				.on_error = tts_err_telemetry,
			}) != 0) {
			kf_log("VL,WN,ERROR=tts_open,i=%d\r\n", i);
			vTaskDelay(pdMS_TO_TICKS(3000));
			continue;
		}
		kf_log("VL,WN,SPEAK,i=%d\r\n", i);
		s_speak_us = esp_timer_get_time();
		ks_tts_feed("你好小智", false, 15000);
		ks_tts_feed(NULL, true, 15000);
		ks_tts_close();
		vTaskDelay(pdMS_TO_TICKS(7000));
	}
	vTaskDelay(pdMS_TO_TICKS(1500));	/* 等尾音抵达检测 */
	/* hits 语义（V-46c 反转）：设备自说的唤醒词应被 AEC 抑制，
	 * hits=自唤醒泄漏率，越低越好（门禁 ≤1/10） */
	kf_log("VL,WN,SELFTEST,done,hits=%d/%d,leak=lower-better\r\n",
	       s_wake_cnt - hits0, cycles);
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
			kf_log("VL,WN,LISTENING,selftest\r\n");
			return;
		}
		kf_log("VL,WN,ALREADY_RUNNING\r\n");
		return;
	}
	s_running = true;
	s_selftest = selftest;
	s_on_wake = on_wake;

	kf_log("VL,WN,STAGE,srmodel_begin\r\n");
	models = esp_srmodel_init("model");
	kf_log("VL,WN,STAGE,srmodel_end,n=%d\r\n",
	       models ? models->num : -1);

	if (!models || models->num == 0) {
		kf_log("VL,WN,ERROR=no_model,check CONFIG_SR_WN_*\r\n");
		goto out_unfreeze;
	}
	/* AEC 驻留挂起（V-46c）：esp-sr 预编译 AEC3 在真实参考数据下
	 * FFT 无限循环（wn_feed 热旋、TWDT 回溯钉 dl_rfft；PSRAM/内部
	 * 分配两态对照同挂，convo 栈排除）——证据包入门禁追记#7，
	 * 待上游修复或下轮专攻。暂回 "M" 无 AEC + 静音窗（已证稳定），
	 * 打断管线保留（pb/TX 中止+路由），AEC 修复即配置翻转 */
	/* V-46c 产品态（37b2d2ae 验证）：MR + FD_LOW_COST——真免疫 0/10，
	 * 常驻无静音窗；FD 分支绕开 SR 档 FFT 病理（追记#10） */
	/* 【V-40 对照实验 2026-10-30 凌晨】：唤醒在 MR+AEC 路线下对真人
	 * 全灭（四种参考状态 × 0.05 地板阈值均零触发），翻回 V-40 验证过
	 * 真人全命中的 "M" 单声道无 AEC 配置做 A/B。若 M 模式唤醒成功
	 * ⇒ MR+AEC 路线自 V-46 起从未在本板对真人有效（后续测试均按键
	 * 驱动掩盖），产品决策：M 模式先行 + AEC 集成另修 */
#define WN_V40_MONO 1
#if WN_V40_MONO
	afe_config_t *cfg = afe_config_init("M", models, AFE_TYPE_SR,
					    AFE_MODE_HIGH_PERF);
	cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
#else
	afe_config_t *cfg = afe_config_init("MR", models, AFE_TYPE_SR,
					    AFE_MODE_HIGH_PERF);

	cfg->aec_init = true;
	cfg->aec_mode = AEC_MODE_FD_LOW_COST;
	cfg->aec_filter_length = 4;
	cfg->aec_nlp_level = AEC_NLP_LEVEL_NORMAL;
	cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
#endif
	/* 无 afe_config_check：V-40 实证 9/10 命中的原版路径未调用它，
	 * 其"修正"在本板配置下会破坏唤醒检测（V-46c 真人测试日 diff
	 * 定位——与原版唯一实质差异即此行） */
	kf_log("VL,WN,STAGE,handle_begin\r\n");
	s_afe = esp_afe_handle_from_config(cfg);
	if (!s_afe) {
		kf_log("VL,WN,ERROR=no_afe_handle\r\n");
		goto out_unfreeze;
	}
	kf_log("VL,WN,STAGE,create_begin,int=%uk\r\n",
	       (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10));
	s_afe_data = s_afe->create_from_config(cfg);
	kf_log("VL,WN,STAGE,create_end,rc=%d\r\n", s_afe_data ? 0 : -1);
	if (!s_afe_data) {
		kf_log("VL,WN,ERROR=afe_create\r\n");
		goto out_unfreeze;
	}
out_unfreeze:
	if (!models || models->num == 0 || !s_afe || !s_afe_data)
		return;
	kf_ev("wn", "loaded int=-%uKB", (unsigned)(
		      (i0 - heap_caps_get_free_size(MALLOC_CAP_INTERNAL)) >> 10));
	kf_log("VL,WN,INIT,ok,model=%s,int=-%uKB,ps=-%uKB\r\n",
	       models->model_name[0] ? models->model_name[0] : "?",
	       (unsigned)((i0 - heap_caps_get_free_size(MALLOC_CAP_INTERNAL)) >> 10),
	       (unsigned)((p0 - heap_caps_get_free_size(MALLOC_CAP_SPIRAM)) >> 10));

	if (!s_beep)
		s_beep = heap_caps_malloc(sizeof(int16_t) * 4800,
					  MALLOC_CAP_SPIRAM);
	if (!s_beep) {
		kf_log("VL,WN,ERROR=beep_alloc\r\n");
	} else
	for (size_t i = 0; i < 4800; i++)
		s_beep[i] = (int16_t)(6000.0f *
				sinf(2.0f * (float)M_PI * 880.0f * (float)i /
				     16000.0f));

	/* 优先级 6 > gfx(5)：音频喂入/检测是墙钟节拍路径，EAF 渲染
	 * 不得挤压（V-46c 真人测试日：核 0 被 EAF 吃满致 wakenet 失聪） */
	xTaskCreatePinnedToCore(feed_task, "wn_feed", 4096, NULL, 6, NULL, 0);
	xTaskCreatePinnedToCore(detect_task, "wn_det", 8192, NULL, 6, NULL, 1);
	if (selftest)
		/* 10240 栈：与 ttsfeeder 同款——announce 内跑完整
		 * MiniMax HTTPS+TLS（esp_http_client+mbedtls） */
		xTaskCreatePinnedToCore(announce_task, "wn_say", 10240, tts,
					4, NULL, 0);
	kf_log("VL,WN,LISTENING%s\r\n", selftest ? ",selftest" : "");
}
