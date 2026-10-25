/* host 回归：append→query(tag/keyword/time)→backup→seq 重载 */
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include "kaleido_memory.h"

int main(void)
{
	struct km_episode e = { 0 };
	struct km_hit hits[KM_QUERY_TOP];
	int n;

	unlink("/tmp/v33/kmtest/ep.jsonl");
	unlink("/tmp/v33/kmtest/core.json");
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

	strlcpy(e.tags[0], "misc", sizeof(e.tags[0]));
	assert(km_append(&e) == 4);
	printf("KM_HOST_TEST,PASS,seq=%llu\n",
	       (unsigned long long)km_seq_current());
	return 0;
}
