/* host 回归：append→query(tag/keyword/time)→backup→seq 重载
 * W4 v2 增补：schema 往返（affect/origin）/reinforce 上界/惰性衰减/
 * tombstone 遗忘/覆盖表持久化/容量拒写（30 号 §8.1，V-48e 主机侧） */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <time.h>
#include "kaleido_memory.h"

int main(void)
{
	struct km_episode e = { 0 };
	struct km_hit hits[KM_QUERY_TOP];
	int n;

	/* 封闭性：目录自建（km 组件不 mkdir；tmpfs 重启后旧目录不在） */
	assert(mkdir("/tmp/v33", 0755) == 0 || errno == EEXIST);
	assert(mkdir("/tmp/v33/kmtest", 0755) == 0 || errno == EEXIST);
	unlink("/tmp/v33/kmtest/ep.jsonl");
	unlink("/tmp/v33/kmtest/core.json");
	unlink("/tmp/v33/kmtest/ep.jsonl.ovl");
	unlink("/tmp/v33/kmtest/ep.jsonl.tmb");
	unlink("/tmp/v33/kmtest/backup.jsonl");
	assert(km_init("/tmp/v33/kmtest/ep.jsonl",
		       "/tmp/v33/kmtest/core.json") == 0);
	assert(km_seq_current() == 0);

	strlcpy(e.summary, "用户提到本周项目评审压力大", sizeof(e.summary));
	strlcpy(e.facts[0], "本周有评审", sizeof(e.facts[0]));
	strlcpy(e.tags[0], "work", sizeof(e.tags[0]));
	e.importance = 0.7f;
	assert(km_append(&e) == 1);

	strlcpy(e.summary, "聊到了周末去公园的计划", sizeof(e.summary));
	strlcpy(e.tags[0], "life", sizeof(e.tags[0]));
	e.importance = 0.3f;
	assert(km_append(&e) == 2);

	strlcpy(e.summary, "用户说喜欢喝咖啡", sizeof(e.summary));
	strlcpy(e.tags[0], "food", sizeof(e.tags[0]));
	e.importance = 0.5f;
	assert(km_append(&e) == 3);

	/* tag 检索 */
	n = km_query(&(struct km_query){ .tag = "work" }, hits,
		     KM_QUERY_TOP);
	assert(n == 1);
	assert(!strcmp(hits[0].ep.summary, "用户提到本周项目评审压力大"));
	assert(hits[0].ep.seq == 1 && hits[0].ep.importance > 0.6f);

	/* keyword 检索 */
	n = km_query(&(struct km_query){ .keyword = "咖啡" }, hits,
		     KM_QUERY_TOP);
	assert(n == 1 && hits[0].ep.seq == 3);

	/* 时间窗+importance 组合（全部今天写入 → 全命中，按分排序） */
	n = km_query(&(struct km_query){ .min_importance = 0.0f,
			.within_days = 30 }, hits, 2);
	assert(n == 2);
	assert(hits[0].score >= hits[1].score);

	/* core 块读写 */
	assert(km_core_write("{\"persona\":\"momo\"}") == 0);
	char buf[256];
	assert(km_core_read(buf, sizeof(buf)) > 0);
	assert(strstr(buf, "momo"));

	/* 备份拷贝 */
	assert(km_backup("/tmp/v33/kmtest/backup.jsonl") == 0);
	FILE *f = fopen("/tmp/v33/kmtest/backup.jsonl", "r");
	int lines = 0;
	char line[512];
	while (fgets(line, sizeof(line), f))
		lines++;
	fclose(f);
	assert(lines == 3);

	/* 重载 seq 水位 */
	assert(km_init("/tmp/v33/kmtest/ep.jsonl",
		       "/tmp/v33/kmtest/core.json") == 0);
	assert(km_seq_current() == 3);

	strlcpy(e.summary, "聊到了周末加班的事", sizeof(e.summary));
	strlcpy(e.tags[0], "misc", sizeof(e.tags[0]));
	e.facts[0][0] = '\0';		/* 卫生：复用结构体须清残留 */
	assert(km_append(&e) == 4);

	/* ── W4 v2：schema 往返（affect/origin）── */
	strlcpy(e.summary, "我主动说了今晚降温记得添衣", sizeof(e.summary));
	e.origin = KM_ORIGIN_SELF;
	e.affect_v = -64;		/* -0.5 */
	e.affect_a = 96;		/* 0.75 */
	strlcpy(e.tags[0], "evening", sizeof(e.tags[0]));
	assert(km_append(&e) == 5);
	assert(km_init("/tmp/v33/kmtest/ep.jsonl",
		       "/tmp/v33/kmtest/core.json") == 0);
	n = km_query(&(struct km_query){ .tag = "evening" }, hits,
		     KM_QUERY_TOP);
	assert(n == 1 && hits[0].ep.seq == 5);
	assert(hits[0].ep.origin == KM_ORIGIN_SELF);
	assert(hits[0].ep.affect_v == -64 && hits[0].ep.affect_a == 96);

	/* origin 非法值兜底 = USER */
	e.origin = 0;
	strlcpy(e.tags[0], "oddlane", sizeof(e.tags[0]));
	strlcpy(e.summary, "兜底行", sizeof(e.summary));
	assert(km_append(&e) == 6);
	assert(km_init("/tmp/v33/kmtest/ep.jsonl",
		       "/tmp/v33/kmtest/core.json") == 0);
	n = km_query(&(struct km_query){ .tag = "oddlane" }, hits,
		     KM_QUERY_TOP);
	assert(n == 1 && hits[0].ep.origin == KM_ORIGIN_USER);

	/* ── reinforce：+0.125 步进、1.0 上界 ── */
	float s = km_reinforce(1);

	assert(fabsf(s - 0.625f) < 1e-4f);
	for (int i = 0; i < 10; i++)
		s = km_reinforce(1);
	assert(fabsf(s - 1.0f) < 1e-4f);	/* 上界钳位 */

	/* ── 惰性衰减：0.99/天，last_accessed 起算 ── */
	struct km_episode probe = { .seq = 1, .t = time(NULL) };
	time_t now = time(NULL);

	assert(fabsf(km_effective_strength(&probe, now) - 1.0f) < 1e-4f);
	float decayed = km_effective_strength(&probe, now + 69 * 86400);

	/* 69 天 ≈ 0.99^69 ≈ 0.5005（半衰量级，30 号 §8.1 API-2） */
	assert(decayed > 0.48f && decayed < 0.52f);
	/* 未 reinforce 的 seq：初值 0.5，按 row.t 衰减 */
	struct km_episode probe2 = { .seq = 2, .t = now };

	assert(fabsf(km_effective_strength(&probe2, now) - 0.5f) < 1e-4f);

	/* ── 覆盖表持久化：km_housekeeping 落 .ovl，重 init 重建 ── */
	km_housekeeping();		/* 板上由 recorder 1Hz 驱动      */
	assert(km_init("/tmp/v33/kmtest/ep.jsonl",
		       "/tmp/v33/kmtest/core.json") == 0);
	assert(fabsf(km_effective_strength(&probe, time(NULL)) - 1.0f)
	       < 1e-4f);

	/* ── tombstone 遗忘：排除检索、主行保留、幂等、不存在拒 ── */
	assert(km_forget(3, "host_test") == 0);
	assert(km_forget(3, "again") == 0);	/* 幂等           */
	assert(km_forget(999, "nope") == KM_PROTECTED);
	n = km_query(&(struct km_query){ .keyword = "咖啡" }, hits,
		     KM_QUERY_TOP);
	assert(n == 0);			/* 检索默认排除           */
	assert(km_count() == 6);	/* 主行不删               */
	f = fopen("/tmp/v33/kmtest/ep.jsonl", "r");
	lines = 0;
	while (fgets(line, sizeof(line), f))
		lines++;
	fclose(f);
	assert(lines == 6);
	/* 重载后墓碑仍生效（.tmb 重建） */
	assert(km_init("/tmp/v33/kmtest/ep.jsonl",
		       "/tmp/v33/kmtest/core.json") == 0);
	n = km_query(&(struct km_query){ .keyword = "咖啡" }, hits,
		     KM_QUERY_TOP);
	assert(n == 0);

	/* ── 容量：满即停写（KM_FULL），上限可调 ── */
	km_set_cap(6);
	strlcpy(e.summary, "容量满拒写", sizeof(e.summary));
	strlcpy(e.tags[0], "cap", sizeof(e.tags[0]));
	assert(km_append(&e) == KM_FULL);
	km_set_cap(7);
	assert(km_append(&e) == 7);
	assert(km_count() == 7);

	/* ── W4.5：core 晋升（带源/append-only/上限）── */
	int pn = km_core_promote(1, "用户的本周项目评审已结束");
	assert(pn == 2);		/* core 已有 persona 一行 */
	assert(km_core_read(buf, sizeof(buf)) > 0);
	assert(strstr(buf, "评审已结束") && strstr(buf, "\"seq\":1"));
	assert(km_core_promote(999, "无源晋升") == KM_PROTECTED);
	assert(km_core_promote(0, "") == KM_ERR_ARG);
	for (int i = pn; i < KM_CORE_MAX; i++) {
		char f[64];

		snprintf(f, sizeof(f), "事实填充第%d条", i);
		assert(km_core_promote(2, f) == i + 1);
	}
	assert(km_core_promote(2, "超上限") == KM_FULL);
	/* 重载后 core 持久 */
	assert(km_init("/tmp/v33/kmtest/ep.jsonl",
		       "/tmp/v33/kmtest/core.json") == 0);
	assert(km_core_read(buf, sizeof(buf)) > 0);
	assert(strstr(buf, "评审已结束"));

	printf("KM_HOST_TEST,PASS,seq=%llu\n",
	       (unsigned long long)km_seq_current());
	return 0;
}
