/*
 * emote_stage.c — 表情舞台 L0（esp_emote_gen_player + 乐鑫猫伴表情包）
 *
 * 链路：esp_emote_gfx 渲染任务 --flush_cb(RGB565 脏区)--> 持 LVGL 互斥把
 * 区域拷入单张 lv_canvas(RGB565, 325²) 并 invalidate，LVGL 直接模式把它
 * 合成进 DPI fb（KEX 表达环叠画布外圈，见 stage_express.c）。
 * 素材：dl.espressif.com/AE/emote_assets.bin（22 片段猫表情，MMAP 容器，
 * EAF 矢量格式，mmap 闪存直读零拷贝），源文件 Emote Pack（GIF/Lottie，
 * 848²）经浏览器工具 ESP Emote GFX Packer NEXT 打包——自制表情走同链。
 *
 * 锁纪律（V-45 死锁教训成文，勿破坏）：
 *   gfx 渲染任务：flush_cb 持 lvgl 锁（顺序 gfx→lvgl，超时 50ms 丢帧自保）
 *   emote 任务：  只碰 gfx API（anim 切换/resize/stop），不碰 lvgl
 *   LVGL 任务：   只碰 lvgl
 *   KEX 回调运行于 LVGL 上下文——只入队，绝不直呼 gfx API（防 lvgl→gfx 反向嵌套）
 *
 * 尺寸注：player 解析器给每个片段硬编码 200×150（demo 屏口径）且每次
 * anim_now 都按 index 重置——每次切换后由 emote 任务持 gfx 锁一次性
 * set_size(325,325)+居中拉正（resize_after_switch）。上游 0.0.2 不解析
 * index 的 width/height 键，已列 V-47 上游回馈项。
 */
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "emote_gen_player.h"
#include "esp_lvgl_port.h"
#include "core/gfx_core.h"
#include "core/gfx_obj.h"
#include "widget/gfx_anim.h"

#include "kaleido_core.h"

#include "emote_stage.h"

extern esp_lcd_panel_handle_t lcd_panel;

static const char *TAG = "emote";

#define STAGE_W		325
#define STAGE_H		325
#define STAGE_FPS	24	/* 与 SDIO/PSRAM 带宽预算对齐（V-45 结论沿用） */

static lv_obj_t *s_img;			/* lv_canvas：合成载体 */
static uint16_t *s_canvas_buf;		/* 画布像素（init 后不变，gfx 任务直用） */
static lv_timer_t *s_stat_timer;
static emote_gen_player_handle_t s_player;
static QueueHandle_t s_q;
static SemaphoreHandle_t s_pause_ack;

/* update_cb 缓存的动画对象（每帧事件刷新；resize 专用） */
static const void *volatile s_anim_obj;
static volatile uint32_t s_frame_cnt;	/* fps 仪表：update_cb 帧计数 */

/* ---- KEX 五态 → 猫伴片段（index.json 实名，含尾下划线） ---- */
static const struct {
	enum kex_state st;
	const char *clip;
} kex_clip[] = {
	{ KEX_UNCONFIGURED, "question_05s" },	/* 还差一把钥匙：疑惑 */
	{ KEX_IDLE,         "smile_05s" },	/* 待命：微笑 */
	{ KEX_LISTENING,    "investigate_" },	/* 在听：探头好奇 */
	{ KEX_THINKING,     "ponder_05s" },	/* 在想：思考 */
	{ KEX_SPEAKING,     "confident_08" },	/* 在说：自信 */
};

static void resize_after_switch(void);

/* ---- 消息（全部经队列进 emote 任务执行） ---- */
enum msg_type { MSG_STATE, MSG_CYCLE, MSG_PAUSE, MSG_RESUME };

struct msg {
	enum msg_type type;
	enum kex_state st;
};

static void emote_task(void *arg)
{
	bool paused = false;

	(void)arg;
	for (;;) {
		struct msg m;

		if (xQueueReceive(s_q, &m, portMAX_DELAY) != pdTRUE)
			continue;
		switch (m.type) {
		case MSG_STATE:
			if (paused)
				break;
			for (size_t i = 0; i < sizeof(kex_clip) /
			     sizeof(kex_clip[0]); i++) {
				if (kex_clip[i].st != m.st)
					continue;
				/* fade：当前段播完再切，表情过渡不跳变 */
				if (emote_gen_player_anim_fade_name(s_player,
						kex_clip[i].clip, true) ==
				    ESP_ERR_NOT_FOUND)
					emote_gen_player_anim_fade_name(
						s_player, "smile_05s", true);
				kf_log("VL,EMOTE,CLIP,%s\r\n", kex_clip[i].clip);
				kf_ev("emote", "clip=%s", kex_clip[i].clip);
				resize_after_switch();
				break;
			}
			break;
		case MSG_CYCLE: {
			/* 人工目验：顺序轮播全部片段 */
			static size_t idx;
			size_t n = emote_gen_player_get_index_count(s_player);

			if (n == 0)
				break;
			idx = (idx + 1) % n;
			emote_gen_player_anim_now(s_player, idx, true);
			const emote_gen_player_index_entry_t *e =
				emote_gen_player_get_index_entry(s_player, idx);

			kf_log("VL,EMOTE,CLIP,%s\r\n",
			       e ? e->name : "?");
			resize_after_switch();
			break;
		}
		case MSG_PAUSE:
			if (!paused) {
				paused = true;
				/* gfx 锁必持：渲染任务此刻可能正在 tick 同一
				 * 对象（评审#1——多字段状态竞态） */
				gfx_emote_lock(emote_gen_player_get_gfx_handle(
							s_player));
				if (s_anim_obj)
					gfx_anim_stop((gfx_obj_t *)s_anim_obj);
				gfx_emote_unlock(emote_gen_player_get_gfx_handle(
							 s_player));
			}
			xSemaphoreGive(s_pause_ack);
			break;
		case MSG_RESUME: {
			struct msg idle = { .type = MSG_STATE,
					    .st = KEX_IDLE };

			if (paused) {
				paused = false;
				gfx_emote_lock(emote_gen_player_get_gfx_handle(
							s_player));
				if (s_anim_obj)
					gfx_anim_start((gfx_obj_t *)s_anim_obj);
				gfx_emote_unlock(emote_gen_player_get_gfx_handle(
								 s_player));
				xQueueSend(s_q, &idle, 0);
			}
			break;
		}
		}
	}
}

/* 切换后一次性拉正：播放器每次 anim_now 都按 index 重置 200×150，
 * 须在每次切换后补 set_size(325²)+居中并隐藏 tip 标签（播放器会在
 * 舞台顶部显示片段名，产品形态不需要）。持 gfx 锁改对象（评审#1） */
static void resize_after_switch(void)
{
	gfx_handle_t gh = emote_gen_player_get_gfx_handle(s_player);

	if (!s_anim_obj)
		return;
	gfx_emote_lock(gh);
	gfx_obj_set_size((gfx_obj_t *)s_anim_obj, STAGE_W, STAGE_H);
	gfx_obj_align((gfx_obj_t *)s_anim_obj, GFX_ALIGN_CENTER, 0, 0);
	gfx_obj_t *tip = emote_gen_player_get_tip_label(s_player);

	if (tip)
		gfx_obj_set_visible(tip, false);
	gfx_emote_unlock(gh);
	kf_log("VL,EMOTE,RESIZED,%dx%d\r\n", STAGE_W, STAGE_H);
}

/* ---- gfx 回调（渲染任务上下文） ---- */

static void stage_flush_cb(int x1, int y1, int x2, int y2, const void *data,
			   emote_gen_player_handle_t mgr)
{
	/* RGB565 脏区拷入画布。持 LVGL 互斥保证不与 LVGL 渲染并发读写
	 * 画布；50ms 拿不到锁就丢本帧（自保：绝不反向等 gfx 锁） */
	if (!s_canvas_buf || x1 < 0 || y1 < 0 || x2 > STAGE_W ||
	    y2 > STAGE_H || x2 <= x1 || y2 <= y1) {
		emote_gen_player_notify_flush_finished(mgr);
		return;
	}
	if (lvgl_port_lock(50)) {
		const uint16_t *src = data;
		int w = x2 - x1;

		for (int y = y1; y < y2; y++)
			memcpy(&s_canvas_buf[y * STAGE_W + x1],
			       &src[(y - y1) * w], w * sizeof(uint16_t));
		/* 只脏本分片：整画布 invalidate 曾令 LVGL 每刷新周期
		 * 全量重绘 325²（~20 倍超量，卡顿根因之一） */
		lv_area_t dirty = { .x1 = x1, .y1 = y1,
				    .x2 = x2 - 1, .y2 = y2 - 1 };

		lv_obj_invalidate_area(s_img, &dirty);
		lvgl_port_unlock();
	}
	emote_gen_player_notify_flush_finished(mgr);
}

static void stage_update_cb(gfx_disp_event_t event, const void *obj,
			    emote_gen_player_handle_t mgr)
{
	(void)mgr;
	/* 只缓存对象指针，不做任何 gfx 变更/入队：帧事件每帧都来，
	 * 若在此驱动 resize 会形成 resize→invalidate→渲染→帧事件的
	 * 自旋闭环，把整机 CPU 饿死（V-46a 实测教训）。拉正时机=
	 * 切换动作后一次性执行（见 emote_task） */
	if (obj && (event == GFX_DISP_EVENT_ONE_FRAME_DONE ||
		    event == GFX_DISP_EVENT_ALL_FRAME_DONE)) {
		s_anim_obj = obj;
		s_frame_cnt++;
	}
}

/* ---- 心跳：FPS + FB 采样（debug 态） + 堆巡检 ---- */

static void stat_timer_cb(lv_timer_t *t)
{
	static int s_patrol;
	uint32_t frames = s_frame_cnt;

	s_frame_cnt = 0;
	(void)t;
	kf_log("DSI,EMOTE,FPS,%u\r\n", (unsigned)(frames * 2 / 3));
	if (kf_dbg_get() && lcd_panel) {
		void *fbs[2] = { 0 };
		static uint16_t *fb0, *fb1;

		if (!fb0 &&
		    esp_lcd_dpi_panel_get_frame_buffer(lcd_panel, 2,
				&fbs[0], &fbs[1]) == ESP_OK) {
			fb0 = fbs[0];
			fb1 = fbs[1];
		}
		if (fb0)
			kf_log("DSI,EMOTE,FB,y300=%04X/%04X,y600=%04X/%04X"
			       "\r\n",
			       fb0[300 * 800 + 440], fb1[300 * 800 + 440],
			       fb0[600 * 800 + 440], fb1[600 * 800 + 440]);
	}
	/* 堆完整性巡检（~30s 一次）：PSRAM 越界写取证 */
	if (++s_patrol % 20 == 0) {
		bool ok = heap_caps_check_integrity_all(true);

		kf_log("VL,HEAP,%s\r\n", ok ? "OK" : "CORRUPT");
	}
	kf_beat("emote");
}

/* ---- 对外 API ---- */

void emote_stage_set_state(enum kex_state st)
{
	struct msg m = { .type = MSG_STATE, .st = st };

	if (s_q)
		xQueueSend(s_q, &m, 0);
}

void emote_stage_cycle(void)
{
	struct msg m = { .type = MSG_CYCLE, .st = 0 };

	if (s_q)
		xQueueSend(s_q, &m, 0);
}

/* 返回 true=暂停已确认（gfx 渲染停）。false=ack 超时——渲染可能仍在
 * 跑，调用方（wakenet 隔离）必须显式记日志决策，不许静默降级 */
bool emote_stage_pause(bool paused)
{
	if (!s_q)
		return !paused;
	struct msg m = { .type = paused ? MSG_PAUSE : MSG_RESUME, .st = 0 };

	xSemaphoreTake(s_pause_ack, 0);		/* 清残留 */
	xQueueSend(s_q, &m, portMAX_DELAY);
	if (!paused)
		return true;
	return xSemaphoreTake(s_pause_ack, pdMS_TO_TICKS(1000)) == pdTRUE;
}

int emote_stage_start(void)
{
	/* 画布缓冲：RGB565 211KB，PSRAM（较 thorvg ARGB 版省 634KB） */
	uint16_t *buf = heap_caps_malloc(STAGE_W * STAGE_H * sizeof(uint16_t),
					 MALLOC_CAP_SPIRAM);

	if (!buf) {
		ESP_LOGE(TAG, "DSI,EMOTE,ERROR=canvas_alloc");
		return -1;
	}
	memset(buf, 0, STAGE_W * STAGE_H * sizeof(uint16_t));

	lv_obj_t *scr = lv_obj_create(NULL);

	/* V-36 产品定稿底色沿用 */
	lv_obj_set_style_bg_color(scr, lv_color_hex(0x101828), 0);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

	s_img = lv_canvas_create(scr);
	lv_canvas_set_buffer(s_img, buf, STAGE_W, STAGE_H,
			     LV_COLOR_FORMAT_RGB565);
	lv_obj_align(s_img, LV_ALIGN_CENTER, 0, 0);
	lv_screen_load(scr);
	s_canvas_buf = buf;

	s_q = xQueueCreate(8, sizeof(struct msg));
	s_pause_ack = xSemaphoreCreateBinary();
	if (!s_q || !s_pause_ack) {
		ESP_LOGE(TAG, "DSI,EMOTE,ERROR=queue");
		return -1;
	}
	/* emote 任务先起（后续 player 事件有归宿）；队列驱动型，PSRAM 栈 */
	static StackType_t *s_emote_stk;
	static StaticTask_t s_emote_tcb;

	if (!kf_task_psram("emote", 4096, emote_task, NULL, 4, -1,
			   (void **)&s_emote_stk, &s_emote_tcb)) {
		ESP_LOGE(TAG, "DSI,EMOTE,ERROR=task");
		return -1;
	}

	/* 播放器：显示口径=舞台 325²；缓冲分片走 PSRAM */
	emote_gen_player_config_t pc = {
		.flags = { .swap = false, .double_buffer = false,
			   .buff_dma = false, .buff_spiram = true },
		.gfx_emote = { .h_res = STAGE_W, .v_res = STAGE_H,
			       .fps = STAGE_FPS },
		/* 109 行/分片：gfx 每帧按 buf_pixels 切分片、每片阻塞等
		 * 我方 flush——16 行/片=20 次锁往返+20 次 invalidate/帧
		 * （实测卡顿主因）；3 片/帧后 LVGL 重绘次数同步降一个量级 */
		.buffers = { .buf_pixels = STAGE_W * 109 },
		.task = { .task_priority = 5, .task_stack = 6 * 1024,
			  .task_affinity = -1,
			  /* V-46c 内存布局：gfx 渲染栈进 PSRAM（组件原生
			   * 选项），内部 RAM 留给 DMA/驱动 */
			  .task_stack_in_ext = true },
		.flush_cb = stage_flush_cb,
		.update_cb = stage_update_cb,
	};

	s_player = emote_gen_player_init(&pc);
	if (!s_player) {
		ESP_LOGE(TAG, "DSI,EMOTE,ERROR=player_init");
		return -1;
	}
	emote_gen_player_data_t src = {
		.type = EMOTE_GEN_PLAYER_SOURCE_PARTITION,
		.source.partition_label = "emote_gen",
		.flags.mmap_enable = 1,
	};
	esp_err_t rc = emote_gen_player_mount_assets(s_player, &src);

	if (rc != ESP_OK) {
		ESP_LOGE(TAG, "DSI,EMOTE,ERROR=mount rc=%d", rc);
		return -1;
	}
	size_t n = emote_gen_player_get_index_count(s_player);

	kf_log("VL,EMOTE,MOUNTED,clips=%d\r\n", (int)n);
	kf_ev("emote", "mounted clips=%d", (int)n);
	/* 首播走队列（此刻 app_main 持 LVGL 锁，emote 任务里切播+拉正
	 * 才符合锁序；kex idle 回调稍后也会入队，幂等） */
	emote_stage_set_state(KEX_IDLE);

	s_stat_timer = lv_timer_create(stat_timer_cb, 1500, NULL);
	return 0;
}
