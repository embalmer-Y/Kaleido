/*
 * kaleido_memory.h — 长期记忆系统（M2.0，方案 v1.5 §4/21号 §4；W4 v2）
 *
 * 分层（21号 §4.2 裁剪为固件核心）：
 *  - core：persona/用户事实块（单 JSON 对象，常驻注入 prompt）
 *  - episodic：情节 JSONL 追加主存储（id/seq/t/summary/facts/tags/importance）
 *  - raw：对话原文（引用，本组件只登记指针路径，不管理其文件）
 * 检索：本地初筛（时间窗 × tag/关键词命中 × 有效性线性衰减）——
 * 云端 embedding 精排由上层编排（BYOK，可关=纯本地初筛）。
 * 备份：manifest（seq 水位/条数/sha256 由上层工具补齐）。
 *
 * 存储：littlefs/-fatfs 之上的 POSIX 文件 API（路径由 km_init 给出），
 * 追加写 + 显式 flush；无动态分配大块（逐行缓冲 ≤ KM_LINE_MAX）。
 *
 * W4 v2 增补（30 号 §8.1/8.2，重要性演化三件套）：
 *  - schema 增补 affect（valence/arousal 1B 定点）+ origin（user/self/
 *    system——D-13 精神：self/system 条目不得标为用户亲历）
 *  - strength 动态量：初值 0.5，检索命中 +0.125（上界 1.0，"越回忆越
 *    牢"）；惰性衰减 ×0.99/天（检索/注入时现算，零写放大）
 *  - 行不可原地改 → strength/last_accessed 走内存覆盖表，脏 ≥8 条
 *    整体落 <ep>.ovl 二进制快照（启动重建）；tombstone 走 <ep>.tmb
 *    追加留痕（非删行），检索默认排除
 *  - 容量：条数上限 km_set_cap（缺省 1500，30 号 §8.4），满即停写
 *    （km_append 返回 KM_FULL——M2 拒写语义，压缩演进留 W5）
 *  - 线程约定：调用方串行化（板上 km 内部互斥保底；主机测试单线程）
 */
#ifndef KALEIDO_MEMORY_H
#define KALEIDO_MEMORY_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KM_LINE_MAX	768	/* 行含 affect/origin 增补后预算（30 号 §8.4
				 * 最坏 ~750B；km_append 超长拒写不截断）  */
#define KM_FACTS_MAX	8
#define KM_TAGS_MAX	6
#define KM_QUERY_TOP	5
#define KM_CAP_DEFAULT	1500	/* 30 号 §8.4：960KB 分区纪律 */
#define KM_CORE_MAX	100	/* 30 号 §8.1 API-3：core 上限，满则提示
				 * 云端合并（LLM 重写，用户可见可删）  */

/* km_append / km_core_* 返回码 */
#define KM_OK		 0
#define KM_ERR_IO	-1
#define KM_ERR_TOOLONG	-2
#define KM_FULL		-3	/* 容量满：拒写（M2 停写语义）      */
#define KM_ERR_ARG	-4
#define KM_PROTECTED	-5	/* protected/不存在：遗忘被拒        */

enum km_origin {
	KM_ORIGIN_USER	= 'u',	/* 用户亲历（ASR 轮）               */
	KM_ORIGIN_SELF	= 's',	/* 主动点火所说（W3）               */
	KM_ORIGIN_SYSTEM = 'y',	/* 系统事件                         */
};

struct km_episode {
	uint64_t seq;
	time_t t;
	char summary[192];
	char facts[KM_FACTS_MAX][48];
	char tags[KM_TAGS_MAX][12];
	float importance;      /* [0,1]（写入时由云端 LLM 打分，缺省 0.3） */
	char raw_ref[96];      /* raw 层路径（可空）                      */
	int8_t affect_v;       /* valence 定点：[-127,127]（真实值×127）  */
	int8_t affect_a;       /* arousal 定点：[0,127]                   */
	int8_t origin;         /* enum km_origin（旧行缺省=USER）         */
};

struct km_query {
	const char *tag;         /* 命中 tag（NULL=不限）          */
	const char *keyword;     /* 摘要子串（NULL=不限，ASCII/CJK 逐字节） */
	uint32_t within_days;    /* 时间窗（0=不限，基准 last_accessed）    */
	float min_importance;    /* 门槛（0=不限）                  */
};

struct km_hit {
	struct km_episode ep;
	float score;             /* 0.5·recency + 0.3·strength_eff + 0.2·命中 */
};

int km_init(const char *episodic_path, const char *core_path);
uint64_t km_seq_current(void);

/* 追加一条情节（seq 自增；返回新 seq，<0 错误；KM_FULL=容量满拒写） */
int64_t km_append(const struct km_episode *ep);

/* 初筛检索：填 hits（按 score 降序），返回命中数（≤top_n）。
 * 返回的命中已自动 km_reinforce（"越回忆越牢"），tombstone 排除 */
int km_query(const struct km_query *q, struct km_hit *hits, int top_n);

/* ── W4 v2：重要性演化（30 号 §8.1 API-1/2/4）── */
/* 检索命中即调：strength = min(1.0, strength + 0.125)，
 * last_accessed = now。返回新 strength（<0 错误）。 */
float km_reinforce(uint64_t seq);

/* 惰性有效强度：strength × 0.99^days_since(last_accessed)。
 * 覆盖表未命中 = 初值 0.5；last 基准 = row.t。零写放大。 */
float km_effective_strength(const struct km_episode *ep, time_t now);

/* 显式遗忘：tombstone 追加留痕（<ep>.tmb），检索默认排除；幂等。
 * 不删主行。KM_PROTECTED=seq 不在库。 */
int km_forget(uint64_t seq, const char *reason);

/* W4.5 晋升 core（30 号 §8.1 API-3）：①源 seq 必须在库（禁无源晋升）
 * ②core 为 JSONL 追加（append-only，源情节不删不改）③条目受保护
 * （km_forget 只作用于 episodic）④满 KM_FULL → 提示云端合并。
 * fact 为事实短句（内部 cJSON 编码落行，≤96B）。返回 core 行数。 */
int km_core_promote(uint64_t src_seq, const char *fact);

/* 容量水位（V-48e 健康巡检项）：当前条数 / 上限（可调） */
int km_count(void);
int km_cap(void);
void km_set_cap(int cap);

/* 覆盖表脏条落盘（写 flash——板上只许内部栈任务周期调用，mem
 * recorder 1Hz；主机测试在 re-init 前显式调用以验持久化） */
void km_housekeeping(void);

/* core 块：整读/整写（JSON 文本，调用方保证 ≤KM_LINE_MAX*4） */
int km_core_write(const char *json);
int km_core_read(char *buf, size_t bufsz);

/* 备份清单：把 episodic 全量 + core 复制到 backup_path（简单整拷贝；
 * 版本化/加密由上层工具基于本接口实现） */
int km_backup(const char *backup_path);

#ifdef __cplusplus
}
#endif
#endif /* KALEIDO_MEMORY_H */
