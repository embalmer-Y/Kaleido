/*
 * aec_probe.c — V-46b AEC 参考信号探针
 *
 * 原理：esp-skainet 官方"软件伪参考"路线——参考信号=播放流镜像
 * （audio_io 参考环，b-1），与麦克风数据一起按 [mic,ref] 交织喂 AFE
 * （"MR" + aec_init=true）。本探针只做测量，不动现有 wakenet/会话链。
 *
 * 对齐（b-2）：稳态播放时参考环"在飞深度"=TX DMA 队列（8×320=2560 帧）
 * +FIFO/codec 群延迟。注意两个计数器不同轴（麦计数连续走、参考计数只在
 * 播放时走），脉冲法在空闲期测不到稳态深度（V-46b 首测实证三次漂移
 * ~1.2s/窗）——改用相关法：播放期采麦，参考按候选延迟扫互相关峰，
 * 峰位即稳态在飞深度。喂入时 ref 窗锚定=「此刻 aio_ref_now() − 深度」。
 * 深度不准 AEC 自适应滤波器（几十 ms 窗）追不上回声路径，抑制失效。
 *
 * 测量（b-3）：播放期内累计 AEC 前后两路 RMS，抑制量=20log10(raw/aec)；
 * 产出同时含 AFE 资源增量（b-4 成本审计）。V-46c 判据：抑制 ≥15dB。
 *
 * 已知限制（探针口径，故意接受）：独立 esp_srmodel_init 产生的模型表
 * 在探针结束不回收（一次性测量，复位即清）；与 wakenet 并存时两者
 * 各自持 AFE 实例（'w' 先起会推高内存，探针打印的成本数含此背景）。
 */
#include <math.h>
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
#include "esp_task_wdt.h"
#include "kaleido_core.h"
#include "aec_probe.h"

#define PULSE_N		480	/* 30ms @16k：峰值搜索窗足够窄 */
#define CAP_SPAN	19200	/* 对齐采音窗 1.2s */
#define RMS_WIN		64	/* 峰值搜索子窗 4ms */

static int32_t s_offset = -1;	/* 最近实测对齐（样本数） */

int32_t aec_align_offset(void)
{
	return s_offset;
}

/* 相关法对齐：播提示音（6s）同时采麦；结束后参考环以
 * 「写尾游标 rw_end」为锚，按候选延迟 D ∈ [DMIN,DMAX] 取参考窗与
 * 麦克风窗做互相关（4 抽样降密），峰位 D*=稳态在飞深度。
 * 返回 D*，失败返回负值 */
/* 相关对齐（单任务乒乓版）：交替 capture/play 各 1600 样本——
 * capture 等待的 100ms 恰为 play 的实时排水期，天然同拍。
 * 并发双任务版（采音任务+join）在显示管线共存下反复触发 HP WDT
 * 整机锁死（V-46b 实测两轮），弃用——教训：大块 PSRAM 操作与
 * 显示并发的维度越多越碰缓存维护竞态（V-45 同族）。
 * 窗长=内置提示音全长（68800=4.3s），扫 D∈[DMIN,DMAX] 互相关，
 * |峰|=稳态在飞深度（TX DMA 队列+群延迟）。 */
#define DMIN		2048
#define DMAX		3328
#define DSTEP		32

static int16_t *s_amic, *s_aref;	/* 常驻复用：大块 PSRAM churn ×
	 * 显示管线 = 死锁同族风险，一次性分配不再释放 */

static int32_t corr_align(void)
{
	extern const char test_prompt_start[] asm(
		"_binary_test_prompt_pcm_start");
	extern const char test_prompt_end[] asm(
		"_binary_test_prompt_pcm_end");
	const int16_t *p = (const void *)test_prompt_start;
	size_t total = (test_prompt_end - test_prompt_start) / 2;

	if (!s_amic) {
		s_amic = heap_caps_malloc((total + 1600) * 2,
					  MALLOC_CAP_SPIRAM);
		s_aref = heap_caps_malloc((total + DMAX) * 2,
					  MALLOC_CAP_SPIRAM);
	}
	if (!s_amic || !s_aref)
		return -1;

	/* 乒乓：capture(n) 等 100ms == play(n) 排水 100ms，同拍互不阻塞 */
	uint32_t cur = 0;
	size_t got = 0;
	int64_t t0 = esp_timer_get_time();

	for (size_t off = 0; off < total; off += 1600) {
		size_t n = total - off;

		if (n > 1600)
			n = 1600;
		size_t c1 = aio_capture_stream(s_amic + got, n, 1000,
					       &cur);

		got += c1;
		aio_play(p + off, n, 1000);
		esp_task_wdt_reset();
		if ((off / 1600) % 16 == 0)
			printf("VL,AEC,ALIGN,prog,off=%d,cap=%d,ms=%lld"
			       "\r\n", (int)off, (int)c1,
			       (esp_timer_get_time() - t0) / 1000);
	}
	fflush(stdout);
	/* DMA 排空尾（≤160ms）也采进窗，保证参考窗尾覆盖播放结束 */
	vTaskDelay(pdMS_TO_TICKS(50));
	got += aio_capture_stream(s_amic + got, 1600, 400, &cur);
	uint32_t rw_end = aio_ref_now();
	esp_task_wdt_reset();
	printf("VL,AEC,ALIGN,post,got=%d,rw=%lu\r\n", (int)got,
	       (unsigned long)rw_end);
	fflush(stdout);

	if (got < total / 2) {
		kf_ev("aec", "align cap %d", (int)got);
		return -2;
	}
	/* 锚点=rw_end−got−DMAX 可能因"采多于播"（排空尾+尾块）下溢：
	 * 负值时对称截短两窗（参考从 0 起，麦窗丢弃尾部等量样本） */
	int64_t pos64 = (int64_t)rw_end - (int64_t)got - DMAX;
	size_t corr_n = got;

	if (pos64 < 0) {
		corr_n += (size_t)pos64;	/* 减 |pos64| */
		pos64 = 0;
	}
	/* 容量钳制：corr_n+DMAX ≤ s_aref 容量(total+DMAX)。rep≥1 时
	 * rw_end 随 rep 累加使锚点为正、此钳制被跳过，而 aio_ref_get
	 * 按 n 直写 dst——曾致每 rep 溢出 3.2KB 腐坏 PSRAM 堆（V-46b
	 * 实证：全系统怪症+菜单任务僵死的真因） */
	if (corr_n > total)
		corr_n = total;
	if (corr_n < (size_t)DMAX * 4) {
		kf_ev("aec", "align short n=%d", (int)corr_n);
		return -3;
	}
	/* 跨步紧凑拷贝（1/4 抽取）：整块 272KB 拷贝与显示管线
	 * esp_cache_msync 并发曾非确定停摆（TWDT 回溯实证卡在
	 * memcpy←aio_ref_get）；相关只消费 1/4 样本，紧凑域直接算 */
	size_t cn4 = corr_n / 4;
	size_t got4 = aio_ref_get_strided(s_aref, cn4 + DMAX / 4 + 1,
					  (uint32_t)pos64, 4);

	esp_task_wdt_reset();
	printf("VL,AEC,ALIGN,refgot,n4=%d,pos=%lld\r\n", (int)got4,
	       pos64);
	fflush(stdout);
	if (got4 < cn4) {
		kf_ev("aec", "align refshort %d", (int)got4);
		return -3;
	}
	int best_d = -1;
	int64_t best_c = 0;

	{
		int n = 0;

		/* 重计算期临时提权：EAF 渲染吃核0 ~95%（24fps），main
		 * 优先级1 钉核0 会被饿到分钟级并连带饿死 IDLE0 触发
		 * INT_WDT（V-46b 全部"锁死"现象的统一根因）。探针期
		 * 提权 + 逐候选让出，双保险；EAF 单核占比入 V-46c 账 */
		vTaskPrioritySet(NULL, 10);

		for (int d = DMIN; d <= DMAX; d += DSTEP) {
		/* 参考窗尾（rw_end-d）对齐麦窗尾；|c| 极大：声学链路
		 * 反相（NS4150/麦布线），AEC 不惧反相，判据须极化无关 */
		/* 参考窗尾在缓冲内偏移 corr_n+DMAX−d → 起点偏移 DMAX−d
		 * （V-46b 曾漏减窗长致 OOB 读 ~500KB → HP WDT 锁死，
		 * 兼为此前"全负相关"假象来源） */
		const int16_t *rp = s_aref + (DMAX / 4 - d / 4);
		int64_t c = 0;

		for (size_t i = 0; i < cn4; i++)
			c += (int32_t)(s_amic[i * 4]) * rp[i];
		int64_t mag = c < 0 ? -c : c;

		if (mag > best_c) {
			best_c = mag;
			best_d = d;
		}
		esp_task_wdt_reset();
		printf("VL,AEC,ALIGN,cand,d=%d,c=%lld,ms=%lld\r\n", d,
		       c, (esp_timer_get_time() - t0) / 1000);
		fflush(stdout);
		vTaskDelay(1);	/* 让出：保 IDLE0/渲染节拍 */
		}
		/* 细扫：粗扫步长 32（2ms）峰判定漂移实测 ±28ms——AEC
		 * 自适应窗只有几 ms，须精化到 4 样本（0.25ms）级 */
		if (best_d > 0) {
			int lo = best_d - 48 > DMIN ? best_d - 48 : DMIN;
			int hi = best_d + 48 < DMAX ? best_d + 48 : DMAX;

			for (int d = lo; d <= hi; d += 4) {
				if (d == best_d)
					continue;
				const int16_t *rp = s_aref +
						    (DMAX / 4 - d / 4);
				int64_t c = 0;

				for (size_t i = 0; i < cn4; i++)
					c += (int32_t)(s_amic[i * 4]) *
					     rp[i];
				int64_t mag = c < 0 ? -c : c;

				if (mag > best_c) {
					best_c = mag;
					best_d = d;
				}
				if ((d / 4) % 8 == 0) {
					esp_task_wdt_reset();
					vTaskDelay(1);
				}
			}
		}
	}
	vTaskPrioritySet(NULL, 1);
	kf_ev("aec", "align corr d=%d", best_d);
	return best_d;
}

void aec_align_probe(void)
{
	int32_t offs[3];
	int n = 0;

	printf("VL,AEC,ALIGN,begin,corr\r\n");
	fflush(stdout);
	/* 订阅 TWDT：再卡死则 7s 后吐任务名单与寄存器现场（HP WDT 静默
	 * 锁死的取证 substitute）——V-46b 调试利器 */
	esp_task_wdt_add(NULL);
	/* 表情渲染隔离：实测 gfx_render 会在此场景自旋饿死 main/IDLE0
	 * （TWDT 寄存器现场：核0 PSRAM 热循环）。探针不需要表情同跑，
	 * V-46c 集成时再查两活共存的病理 */
	{
		extern bool emote_stage_pause(bool);
		emote_stage_pause(true);
	}
	for (int i = 0; i < 3; i++) {
		offs[i] = corr_align();
		printf("VL,AEC,ALIGN,rep=%d,offset=%d\r\n", i, (int)offs[i]);
		fflush(stdout);
		kf_ev("aec", "align rep=%d off=%d", i, (int)offs[i]);
		if (offs[i] > 0)
			n++;
		vTaskDelay(pdMS_TO_TICKS(300));
	}
	{
		extern bool emote_stage_pause(bool);
		emote_stage_pause(false);
	}
	esp_task_wdt_delete(NULL);
	if (n) {
		/* 中位数：3 取样抗单次离群 */
		int32_t a = offs[0], b = offs[1], c = offs[2], med;

		if ((a >= b) != (a >= c))
			med = a;
		else if ((b >= a) != (b >= c))
			med = b;
		else
			med = c;
		s_offset = med;
		kf_ev("aec", "align offset=%d", (int)med);
		printf("VL,AEC,ALIGN,result,offset=%d\r\n", (int)med);
	} else {
		printf("VL,AEC,ALIGN,result,FAIL,no_peak\r\n");
	}
	fflush(stdout);
}

/* ---- b-3：MR+AFE 测量探针 ---- */

static const esp_afe_sr_iface_t *p_afe;
static esp_afe_sr_data_t *p_data;
static volatile bool p_running;
static volatile bool p_playing;
static int p_chunk;
static int64_t p_raw_sq, p_aec_sq;	/* 平方和累计 */
static int64_t p_raw_n, p_aec_n;
static int64_t p_feed_us, p_feed_n;
/* 累计窗=p_playing（两侧对称）：预热靠播放前的 500ms 静默期——
 * AFE/AEC 在无声里收敛，不污染测量 */

static void probe_feed_task(void *arg)
{
	int16_t *mic = heap_caps_malloc(p_chunk * 2, MALLOC_CAP_SPIRAM);
	int16_t *ref = heap_caps_malloc(p_chunk * 2, MALLOC_CAP_SPIRAM);
	int16_t *feed = heap_caps_malloc(p_chunk * 2 * 2, MALLOC_CAP_SPIRAM);
	uint32_t cur = 0;

	(void)arg;
	if (!mic || !ref || !feed) {
		p_running = false;
		vTaskDelete(NULL);
		return;
	}
	while (p_running) {
		int n = aio_capture_stream(mic, p_chunk, 2000, &cur);

		if (n <= 0)
			continue;
		int64_t t0 = esp_timer_get_time();

		/* 参考窗锚定「此刻写游标 − 稳态在飞深度」：麦块刚到=
		 * 当下声场，当下正被喇叭播放的样本=rw−在飞深度 */
		uint32_t rw = aio_ref_now();
		uint32_t rpos = rw - (uint32_t)s_offset - (uint32_t)n;

		if (!aio_ref_get(ref, n, rpos))
			memset(ref, 0, n * 2);	/* 陈旧/未起播：全零参考 */
		/* 连续排布 [整块 mic][整块 ref]：esp-sr 的
		 * afe_parse_data("交织→连续")即为此准备（0.5dB 首测
		 * 教训：交织排布 AEC 不生效）；若仍无效再试 [ref][mic] */
		memcpy(feed, mic, n * 2);
		memcpy(feed + n, ref, n * 2);
		p_afe->feed(p_data, feed);
		p_feed_us += esp_timer_get_time() - t0;
		p_feed_n++;
		if (!p_playing)
			continue;
		for (int i = 0; i < n; i++) {
			int32_t v = mic[i];

			p_raw_sq += v * v;
		}
		p_raw_n += n;
	}
	heap_caps_free(mic);
	heap_caps_free(ref);
	heap_caps_free(feed);
	vTaskDelete(NULL);
}

static void probe_fetch_task(void *arg)
{
	(void)arg;
	while (p_running) {
		afe_fetch_result_t *r = p_afe->fetch(p_data);

		if (!r || r->ret_value == ESP_FAIL)
			continue;
		for (int i = 0; i < r->data_size / 2; i++) {
			int32_t v = r->data[i];

			p_aec_sq += v * v;
		}
		p_aec_n += r->data_size / 2;
	}
	vTaskDelete(NULL);
}

void aec_probe_run(void)
{
	srmodel_list_t *models;
	afe_config_t *cfg;
	uint32_t i0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
	uint32_t q0 = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
	TaskHandle_t tf = NULL, td = NULL;

	printf("VL,AEC,PROBE,begin,offset=%d\r\n", (int)s_offset);
	fflush(stdout);
	if (s_offset < 0) {
		printf("VL,AEC,PROBE,abort,run-R-first\r\n");
		fflush(stdout);
		return;
	}
	models = esp_srmodel_init("model");	/* 一次性探针：模型表不回收 */
	cfg = afe_config_init("MR", models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
	cfg->aec_init = true;
	/* 实测定参（V-46b）：HIGH_PERF+长滤波器抑制持平（10.5dB）而成本
	 * ×5（feed 24.9ms=78% 预算）——默认档（LOW_COST 映射）为 V-46c
	 * 基线。10dB 平台的残差=PA 非线性失真，线性核+NLP 不可消，
	 * 降播报音量/感知门限属 V-46c 打断阈值调参范畴 */
	cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
	cfg = afe_config_check(cfg);
	p_afe = esp_afe_handle_from_config(cfg);
	p_data = p_afe ? p_afe->create_from_config(cfg) : NULL;
	if (!p_data) {
		printf("VL,AEC,PROBE,abort,afe_create\r\n");
		fflush(stdout);
		return;
	}
	p_chunk = p_afe->get_feed_chunksize(p_data);
	printf("VL,AEC,PROBE,afe_ok,chunk=%d,ch=%d\r\n", p_chunk,
	       p_afe->get_feed_channel_num(p_data));
	fflush(stdout);

	p_running = true;
	p_playing = false;
	p_raw_sq = p_aec_sq = p_raw_n = p_aec_n = p_feed_us = p_feed_n = 0;
	xTaskCreatePinnedToCore(probe_feed_task, "aecf", 4096, NULL, 5, &tf, 0);
	xTaskCreatePinnedToCore(probe_fetch_task, "aecd", 4096, NULL, 5, &td, 1);
	vTaskDelay(pdMS_TO_TICKS(500));	/* AFE 静默预热（不计入测量） */

	/* 播放窗：内置提问 8s（过 aio_play→参考环自动镜像）；
	 * 尾部 +500ms 覆盖 TX DMA 排空期 */
	{
		extern const char test_prompt_start[] asm(
			"_binary_test_prompt_pcm_start");
		extern const char test_prompt_end[] asm(
			"_binary_test_prompt_pcm_end");
		const int16_t *p = (const void *)test_prompt_start;
		size_t total = (test_prompt_end - test_prompt_start) / 2;

		p_playing = true;
		for (size_t off = 0; off < total; off += 1600) {
			size_t n = total - off;

			if (n > 1600)
				n = 1600;
			aio_play(p + off, n, 1000);
		}
		vTaskDelay(pdMS_TO_TICKS(500));
		p_playing = false;
	}
	vTaskDelay(pdMS_TO_TICKS(300));
	p_running = false;
	vTaskDelay(pdMS_TO_TICKS(200));

	float raw = p_raw_n ? sqrtf((float)p_raw_sq / p_raw_n) : 0;
	float aec = p_aec_n ? sqrtf((float)p_aec_sq / p_aec_n) : 0;
	float sup = (raw > 0 && aec > 0) ?
		    20.0f * log10f(raw / aec) : 0;
	float feed_ms = p_feed_n ?
			(double)p_feed_us / p_feed_n / 1000.0 : 0;

	printf("VL,AEC,RESULT,raw_rms=%.0f,aec_rms=%.0f,suppression_db=%.1f,"
	       "int=-%uKB,ps=-%uKB,feed_ms=%.1f\r\n",
	       raw, aec, sup,
	       (unsigned)((i0 - heap_caps_get_free_size(
				  MALLOC_CAP_INTERNAL)) >> 10),
	       (unsigned)((q0 - heap_caps_get_free_size(
				  MALLOC_CAP_SPIRAM)) >> 10),
	       feed_ms);
	fflush(stdout);
	kf_ev("aec", "sup=%.1fdb raw=%.0f", sup, raw);

	p_afe->destroy(p_data);	/* iface 有 destroy op：实例与内部任务回收 */
	p_data = NULL;
	p_afe = NULL;
}
