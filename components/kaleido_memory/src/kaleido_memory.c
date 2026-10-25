/*
 * kaleido_memory.c — 三层记忆核心实现（POSIX 文件 + 线性扫描初筛）
 *
 * 评分：score = 0.5·recency + 0.3·importance + 0.2·match
 * recency = max(0, 1 − age_days/30)（21号 §4.3 线性窗；
 * Generative-Agents cos_sim 精排留云端，22号 §3.1）
 */
#include "kaleido_memory.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "cJSON.h"

static char s_path_ep[96];
static char s_path_core[96];
static uint64_t s_seq;

static float clampf(float x, float lo, float hi)
{
	return x < lo ? lo : (x > hi ? hi : x);
}

int km_init(const char *episodic_path, const char *core_path)
{
	strlcpy(s_path_ep, episodic_path, sizeof(s_path_ep));
	strlcpy(s_path_core, core_path, sizeof(s_path_core));
	s_seq = 0;

	FILE *f = fopen(s_path_ep, "r");

	if (!f)
		return 0;              /* 首次：空库                */
	char line[KM_LINE_MAX];

	while (fgets(line, sizeof(line), f)) {
		cJSON *root = cJSON_Parse(line);
		cJSON *seq;

		if (!root)
			continue;
		seq = cJSON_GetObjectItem(root, "seq");
		if (seq && cJSON_IsNumber(seq) &&
		    (uint64_t)seq->valuedouble > s_seq)
			s_seq = (uint64_t)seq->valuedouble;
		cJSON_Delete(root);
	}
	fclose(f);
	return 0;
}

uint64_t km_seq_current(void)
{
	return s_seq;
}

static void ep_to_json(const struct km_episode *ep, char *out,
		       size_t outsz)
{
	cJSON *root = cJSON_CreateObject();
	cJSON *facts = cJSON_CreateArray();
	cJSON *tags = cJSON_CreateArray();
	char *s;

	cJSON_AddNumberToObject(root, "seq", (double)ep->seq);
	cJSON_AddNumberToObject(root, "t", (double)ep->t);
	cJSON_AddStringToObject(root, "summary", ep->summary);
	for (int i = 0; i < KM_FACTS_MAX && ep->facts[i][0]; i++)
		cJSON_AddItemToArray(facts,
			cJSON_CreateString(ep->facts[i]));
	for (int i = 0; i < KM_TAGS_MAX && ep->tags[i][0]; i++)
		cJSON_AddItemToArray(tags,
			cJSON_CreateString(ep->tags[i]));
	cJSON_AddItemToObject(root, "facts", facts);
	cJSON_AddItemToObject(root, "tags", tags);
	cJSON_AddNumberToObject(root, "importance", ep->importance);
	if (ep->raw_ref[0])
		cJSON_AddStringToObject(root, "raw", ep->raw_ref);
	s = cJSON_PrintUnformatted(root);
	strlcpy(out, s, outsz);
	cJSON_free(s);
	cJSON_Delete(root);
}

static bool ep_from_json(const char *line, struct km_episode *ep)
{
	cJSON *root = cJSON_Parse(line);
	const cJSON *v;

	if (!root)
		return false;
	memset(ep, 0, sizeof(*ep));
	v = cJSON_GetObjectItem(root, "seq");
	if (v && cJSON_IsNumber(v))
		ep->seq = (uint64_t)v->valuedouble;
	v = cJSON_GetObjectItem(root, "t");
	if (v && cJSON_IsNumber(v))
		ep->t = (time_t)v->valuedouble;
	v = cJSON_GetObjectItem(root, "summary");
	if (v && cJSON_IsString(v))
		strlcpy(ep->summary, v->valuestring,
			sizeof(ep->summary));
	v = cJSON_GetObjectItem(root, "importance");
	if (v && cJSON_IsNumber(v))
		ep->importance = (float)v->valuedouble;
	v = cJSON_GetObjectItem(root, "raw");
	if (v && cJSON_IsString(v))
		strlcpy(ep->raw_ref, v->valuestring,
			sizeof(ep->raw_ref));
	v = cJSON_GetObjectItem(root, "facts");
	if (cJSON_IsArray(v)) {
		int i = 0;
		const cJSON *it;

		cJSON_ArrayForEach(it, v) {
			if (i >= KM_FACTS_MAX)
				break;
			strlcpy(ep->facts[i++], it->valuestring, 48);
		}
	}
	v = cJSON_GetObjectItem(root, "tags");
	if (cJSON_IsArray(v)) {
		int i = 0;
		const cJSON *it;

		cJSON_ArrayForEach(it, v) {
			if (i >= KM_TAGS_MAX)
				break;
			strlcpy(ep->tags[i++], it->valuestring, 12);
		}
	}
	cJSON_Delete(root);
	return ep->seq != 0;
}

int64_t km_append(const struct km_episode *ep)
{
	char line[KM_LINE_MAX];
	struct km_episode e = *ep;
	FILE *f;

	e.seq = ++s_seq;
	if (!e.t)
		e.t = time(NULL);
	ep_to_json(&e, line, sizeof(line));
	f = fopen(s_path_ep, "a");
	if (!f)
		return -1;
	fprintf(f, "%s\n", line);
	fclose(f);
	return (int64_t)e.seq;
}

static bool match_query(const struct km_episode *ep,
			const struct km_query *q, float *match_out)
{
	float m = 0.0f;

	if (q->min_importance > 0 && ep->importance < q->min_importance)
		return false;
	if (q->within_days) {
		double age = difftime(time(NULL), ep->t) / 86400.0;

		if (age > q->within_days)
			return false;
	}
	if (q->tag) {
		bool hit = false;

		for (int i = 0; i < KM_TAGS_MAX; i++)
			if (!strcmp(ep->tags[i], q->tag))
				hit = true;
		if (!hit)
			return false;
		m += 0.2f;
	}
	if (q->keyword) {
		if (!strstr(ep->summary, q->keyword))
			return false;   /* keyword 语义=过滤+加分     */
		m += 0.2f;
	}
	*match_out = m;
	return true;
}

int km_query(const struct km_query *q, struct km_hit *hits, int top_n)
{
	FILE *f = fopen(s_path_ep, "r");
	char line[KM_LINE_MAX];
	int n = 0;

	if (!f || !q || !hits || top_n <= 0)
		return f ? -1 : -2;
	while (fgets(line, sizeof(line), f)) {
		struct km_episode ep;
		float match;

		if (!ep_from_json(line, &ep))
			continue;
		if (!match_query(&ep, q, &match))
			continue;
		double age_days =
			difftime(time(NULL), ep.t) / 86400.0;
		float recency =
			clampf(1.0f - (float)age_days / 30.0f, 0.0f, 1.0f);
		float score = 0.5f * recency +
			      0.3f * clampf(ep.importance, 0, 1) + match;

		if (n == top_n && score <= hits[top_n - 1].score)
			continue;
		/* 有序降序插入（top_n ≤ KM_QUERY_TOP，规模极小） */
		int i = n < top_n ? n : top_n - 1;

		while (i > 0 && hits[i - 1].score < score) {
			hits[i] = hits[i - 1];
			i--;
		}
		hits[i].ep = ep;
		hits[i].score = score;
		if (n < top_n)
			n++;
	}
	fclose(f);
	return n;
}

int km_core_write(const char *json)
{
	FILE *f = fopen(s_path_core, "w");

	if (!f)
		return -1;
	fputs(json, f);
	fclose(f);
	return 0;
}

int km_core_read(char *buf, size_t bufsz)
{
	FILE *f = fopen(s_path_core, "r");
	size_t n;

	if (!f)
		return -1;
	n = fread(buf, 1, bufsz - 1, f);
	buf[n] = '\0';
	fclose(f);
	return (int)n;
}

int km_backup(const char *backup_path)
{
	FILE *src = fopen(s_path_ep, "r");
	FILE *dst;
	char buf[256];
	size_t n;

	if (!src)
		return -1;
	dst = fopen(backup_path, "w");
	if (!dst) {
		fclose(src);
		return -2;
	}
	while ((n = fread(buf, 1, sizeof(buf), src)) > 0)
		fwrite(buf, 1, n, dst);
	fclose(src);
	fclose(dst);
	return 0;
}
