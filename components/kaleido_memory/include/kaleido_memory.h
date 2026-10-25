/*
 * kaleido_memory.h — 长期记忆系统（M2.0，方案 v1.5 §4/21号 §4）
 *
 * 分层（21号 §4.2 裁剪为固件核心）：
 *  - core：persona/用户事实块（单 JSON 对象，常驻注入 prompt）
 *  - episodic：情节 JSONL 追加主存储（id/seq/t/summary/facts/tags/importance）
 *  - raw：对话原文（引用，本组件只登记指针路径，不管理其文件）
 * 检索：本地初筛（时间窗 × tag/关键词命中 × importance 线性衰减）——
 * 云端 embedding 精排由上层编排（BYOK，可关=纯本地初筛）。
 * 备份：manifest（seq 水位/条数/sha256 由上层工具补齐）。
 *
 * 存储：littlefs/-fatfs 之上的 POSIX 文件 API（路径由 km_init 给出），
 * 追加写 + 显式 flush；无动态分配大块（逐行缓冲 ≤ KM_LINE_MAX）。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KM_LINE_MAX	512
#define KM_FACTS_MAX	8
#define KM_TAGS_MAX	6
#define KM_QUERY_TOP	5

struct km_episode {
	uint64_t seq;
	time_t t;
	char summary[192];
	char facts[KM_FACTS_MAX][48];
	char tags[KM_TAGS_MAX][12];
	float importance;      /* [0,1]（写入时由云端 LLM 打分，缺省 0.3） */
	char raw_ref[96];      /* raw 层路径（可空）                      */
};

struct km_query {
	const char *tag;         /* 命中 tag（NULL=不限）          */
	const char *keyword;     /* 摘要子串（NULL=不限，ASCII/CJK 逐字节） */
	uint32_t within_days;    /* 时间窗（0=不限）                */
	float min_importance;    /* 门槛（0=不限）                  */
};

struct km_hit {
	struct km_episode ep;
	float score;             /* 0.5·time + 0.3·importance + 0.2·命中 */
};

int km_init(const char *episodic_path, const char *core_path);
uint64_t km_seq_current(void);

/* 追加一条情节（seq 自增；返回新 seq，<0 错误） */
int64_t km_append(const struct km_episode *ep);

/* 初筛检索：填 hits（按 score 降序），返回命中数（≤top_n） */
int km_query(const struct km_query *q, struct km_hit *hits, int top_n);

/* core 块：整读/整写（JSON 文本，调用方保证 ≤KM_LINE_MAX*4） */
int km_core_write(const char *json);
int km_core_read(char *buf, size_t bufsz);

/* 备份清单：把 episodic 全量 + core 复制到 backup_path（简单整拷贝；
 * 版本化/加密由上层工具基于本接口实现） */
int km_backup(const char *backup_path);

#ifdef __cplusplus
}
#endif
