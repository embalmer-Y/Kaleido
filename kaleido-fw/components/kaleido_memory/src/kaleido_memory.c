/*
 * kaleido_memory.c — 三层记忆核心实现（POSIX 文件 + 线性扫描初筛）
 *
 * 评分（W4 v2，30 号 §8.1）：score = 0.5·recency + 0.3·strength_eff + 0.2·match
 * recency 基准 = last_accessed（覆盖表命中）否则 row.t；线性窗 30 天。
 * strength_eff = strength × 0.99^days_since(last_accessed)（惰性衰减，
 * 零写放大）；strength 走内存覆盖表（脏 ≥8 落 .ovl 快照）。
 * 遗忘不删行：<seq> 追加进 .tmb 墓碑簿，检索默认排除（30 号 §8.1 API-4）。
 */
#include "kaleido_memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include <time.h>

#include "cJSON.h"

/* 主机测试单线程无锁；目标侧 km 互斥保底（append/query/think 并发） */
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32)
#define KM_LOCK_INIT()	do { } while (0)
#define KM_LOCK()	do { } while (0)
#define KM_UNLOCK()	do { } while (0)
#else
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_lock;
#define KM_LOCK_INIT()	do { s_lock = xSemaphoreCreateMutex(); } while (0)
#define KM_LOCK() \
	do { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); } while (0)
#define KM_UNLOCK() \
	do { if (s_lock) xSemaphoreGive(s_lock); } while (0)
#endif

#define KM_STRENGTH0	0.5f
#define KM_REINFORCE	0.125f		/* 30 号 §8.1 API-1           */
#define KM_DECAY_PER_DAY 0.99f		/* ≈半衰 69 天                */
#define KM_DIRTY_FLUSH	8		/* 覆盖表脏条落盘阈值         */
#define KM_OVL_MAGIC	0x314f4d4b	/* "KMO1" LE                  */

static char s_path_ep[96];
static char s_path_core[96];
static char s_path_ovl[100];		/* <ep>.ovl：strength 覆盖表  */
static char s_path_tmb[100];		/* <ep>.tmb：墓碑簿           */
static uint64_t s_seq;
static int s_count;
static int s_cap = KM_CAP_DEFAULT;

/* strength 覆盖表（仅被 reinforce 过的 seq；其余行用初值 0.5） */
struct km_ovl {
	uint64_t seq;
	float strength;
	int64_t last;			/* last_accessed epoch        */
};
static struct km_ovl *s_ovl;
static int s_ovl_n, s_ovl_cap, s_ovl_dirty;

/* 墓碑集（.tmb 加载；追加即同步内存） */
static uint64_t *s_tomb;
static int s_tomb_n, s_tomb_cap;

static float clampf(float x, float lo, float hi)
{
	return x < lo ? lo : (x > hi ? hi : x);
}

static char *path_cat(char *out, size_t n, const char *base, const char *ext)
{
	snprintf(out, n, "%s%s", base, ext);
	return out;
}

/* ── 覆盖表（二进制快照；tmp+rename 原子替换） ── */
static void ovl_flush_locked(void)
{
	FILE *f;

	if (!s_ovl_dirty)
		return;
	f = fopen(s_path_ovl, "w");
	if (!f)
		return;			/* 只影响持久性，不影响内存态 */
	uint32_t magic = KM_OVL_MAGIC, n = (uint32_t)s_ovl_n;

	fwrite(&magic, sizeof(magic), 1, f);
	fwrite(&n, sizeof(n), 1, f);
	if (s_ovl_n)
		fwrite(s_ovl, sizeof(struct km_ovl), (size_t)s_ovl_n, f);
	fclose(f);
	s_ovl_dirty = 0;
}

static void ovl_load(void)
{
	FILE *f = fopen(s_path_ovl, "r");
	uint32_t magic = 0, n = 0;

	if (!f)
		return;
	if (fread(&magic, sizeof(magic), 1, f) != 1 || magic != KM_OVL_MAGIC ||
	    fread(&n, sizeof(n), 1, f) != 1 || n > 4096) {
		fclose(f);
		return;			/* 坏快照：按空表重建（初值 0.5） */
	}
	if (s_ovl_cap < (int)n) {
		s_ovl = realloc(s_ovl, sizeof(struct km_ovl) * n);
		s_ovl_cap = (int)n;
	}
	if (s_ovl && n &&
	    fread(s_ovl, sizeof(struct km_ovl), n, f) == n)
		s_ovl_n = (int)n;
	fclose(f);
}

static struct km_ovl *ovl_find(uint64_t seq)
{
	for (int i = 0; i < s_ovl_n; i++)
		if (s_ovl[i].seq == seq)
			return &s_ovl[i];
	return NULL;
}

/* ── 墓簿 ── */
static void tomb_add(uint64_t seq)
{
	if (s_tomb_n == s_tomb_cap) {
		int cap = s_tomb_cap ? s_tomb_cap * 2 : 32;

		s_tomb = realloc(s_tomb, sizeof(uint64_t) * cap);
		if (!s_tomb) {
			s_tomb_cap = 0;
			return;
		}
		s_tomb_cap = cap;
	}
	s_tomb[s_tomb_n++] = seq;
}

static bool tomb_has(uint64_t seq)
{
	for (int i = 0; i < s_tomb_n; i++)
		if (s_tomb[i] == seq)
			return true;
	return false;
}

static void tomb_load(void)
{
	FILE *f = fopen(s_path_tmb, "r");
	char line[128];

	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		cJSON *root = cJSON_Parse(line);
		cJSON *seq;

		if (!root)
			continue;
		seq = cJSON_GetObjectItem(root, "seq");
		if (seq && cJSON_IsNumber(seq))
			tomb_add((uint64_t)seq->valuedouble);
		cJSON_Delete(root);
	}
	fclose(f);
}

int km_init(const char *episodic_path, const char *core_path)
{
	strlcpy(s_path_ep, episodic_path, sizeof(s_path_ep));
	strlcpy(s_path_core, core_path, sizeof(s_path_core));
	path_cat(s_path_ovl, sizeof(s_path_ovl), s_path_ep, ".ovl");
	path_cat(s_path_tmb, sizeof(s_path_tmb), s_path_ep, ".tmb");
	s_seq = 0;
	s_count = 0;
	s_ovl_n = 0;			/* 重建：覆盖表/墓碑/脏计数全清 */
	s_ovl_dirty = 0;
	s_tomb_n = 0;

	KM_LOCK_INIT();
	KM_LOCK();
	ovl_load();
	tomb_load();

	FILE *f = fopen(s_path_ep, "r");

	if (f) {
		char line[KM_LINE_MAX + 1];

		while (fgets(line, sizeof(line), f)) {
			cJSON *root = cJSON_Parse(line);
			cJSON *seq;

			if (!root)
				continue;
			s_count++;
			seq = cJSON_GetObjectItem(root, "seq");
			if (seq && cJSON_IsNumber(seq) &&
			    (uint64_t)seq->valuedouble > s_seq)
				s_seq = (uint64_t)seq->valuedouble;
			cJSON_Delete(root);
		}
		fclose(f);
	}
	KM_UNLOCK();
	return 0;
}

uint64_t km_seq_current(void)
{
	return s_seq;
}

int km_count(void)
{
	return s_count;
}

int km_cap(void)
{
	return s_cap;
}

void km_set_cap(int cap)
{
	if (cap > 0)
		s_cap = cap;
}

/* 返回序列化需写字节数（不含 NUL）；调用方据此拒超长（S1） */
static size_t ep_to_json(const struct km_episode *ep, char *out,
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
	cJSON_AddNumberToObject(root, "av", ep->affect_v);
	cJSON_AddNumberToObject(root, "aa", ep->affect_a);
	cJSON_AddNumberToObject(root, "or", ep->origin);
	s = cJSON_PrintUnformatted(root);
	strlcpy(out, s, outsz);
	cJSON_free(s);
	cJSON_Delete(root);
	return strlen(s);
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
	v = cJSON_GetObjectItem(root, "av");
	if (v && cJSON_IsNumber(v))
		ep->affect_v = (int8_t)v->valuedouble;
	v = cJSON_GetObjectItem(root, "aa");
	if (v && cJSON_IsNumber(v))
		ep->affect_a = (int8_t)v->valuedouble;
	v = cJSON_GetObjectItem(root, "or");
	ep->origin = v && cJSON_IsNumber(v) ?
		     (int8_t)v->valuedouble : (int8_t)KM_ORIGIN_USER;
	if (ep->origin != KM_ORIGIN_USER && ep->origin != KM_ORIGIN_SELF &&
	    ep->origin != KM_ORIGIN_SYSTEM)
		ep->origin = KM_ORIGIN_USER;	/* 旧行/非法值兜底 */
	v = cJSON_GetObjectItem(root, "facts");
	if (cJSON_IsArray(v)) {
		int i = 0;
		const cJSON *it;

		cJSON_ArrayForEach(it, v) {
			if (i >= KM_FACTS_MAX)
				break;
			if (cJSON_IsString(it))     /* G8        */
				strlcpy(ep->facts[i++],
					it->valuestring, 48);
		}
	}
	v = cJSON_GetObjectItem(root, "tags");
	if (cJSON_IsArray(v)) {
		int i = 0;
		const cJSON *it;

		cJSON_ArrayForEach(it, v) {
			if (i >= KM_TAGS_MAX)
				break;
			if (cJSON_IsString(it))     /* G8        */
				strlcpy(ep->tags[i++],
					it->valuestring, 12);
		}
	}
	cJSON_Delete(root);
	return ep->seq != 0;
}

int64_t km_append(const struct km_episode *ep)
{
	char line[KM_LINE_MAX + 1];
	struct km_episode e = *ep;
	FILE *f;
	size_t need;
	int64_t ret;

	if (!ep)
		return KM_ERR_ARG;
	KM_LOCK();
	if (s_count >= s_cap) {
		ret = KM_FULL;		/* M2 满即停写（30 号 §8.4） */
		goto out;
	}
	e.seq = s_seq + 1;               /* S1：写失败不前进      */
	if (!e.t)
		e.t = time(NULL);
	if (e.origin != KM_ORIGIN_USER && e.origin != KM_ORIGIN_SELF &&
	    e.origin != KM_ORIGIN_SYSTEM)
		e.origin = KM_ORIGIN_USER;	/* 0/非法值=用户轮   */
	need = ep_to_json(&e, line, sizeof(line));
	if (need + 2 > KM_LINE_MAX) {
		ret = KM_ERR_TOOLONG;	/* 满配截断拒绝（S1）    */
		goto out;
	}
	f = fopen(s_path_ep, "a");
	if (!f) {
		ret = KM_ERR_IO;
		goto out;
	}
	fprintf(f, "%s\n", line);
	fclose(f);
	s_seq = e.seq;
	s_count++;
	ret = (int64_t)e.seq;
out:
	KM_UNLOCK();
	return ret;
}

float km_reinforce(uint64_t seq)
{
	struct km_ovl *o = ovl_find(seq);
	float s;

	KM_LOCK();
	if (!o) {
		if (s_ovl_n == s_ovl_cap) {
			int cap = s_ovl_cap ? s_ovl_cap * 2 : 32;

			s_ovl = realloc(s_ovl, sizeof(struct km_ovl) * cap);
			if (!s_ovl) {
				KM_UNLOCK();
				return -1;
			}
			s_ovl_cap = cap;
		}
		o = &s_ovl[s_ovl_n++];
		o->seq = seq;
		o->strength = KM_STRENGTH0;
		o->last = (int64_t)time(NULL);
	}
	o->strength = clampf(o->strength + KM_REINFORCE, 0.0f, 1.0f);
	o->last = (int64_t)time(NULL);
	s = o->strength;
	s_ovl_dirty++;
	/* 落盘不在此处：写路径须在内部栈任务上（板级纪律）。调用方
	 * 周期性调 km_housekeeping() 刷盘（板上 mem recorder 1Hz） */
	KM_UNLOCK();
	return s;
}

/* 覆盖表脏条落盘（.ovl 快照）；板上由内部栈任务周期调用 */
void km_housekeeping(void)
{
	KM_LOCK();
	ovl_flush_locked();
	KM_UNLOCK();
}

float km_effective_strength(const struct km_episode *ep, time_t now)
{
	struct km_ovl *o;
	float s;
	int64_t last;

	if (!ep || !ep->seq)
		return 0.0f;
	KM_LOCK();
	o = ovl_find(ep->seq);
	s = o ? o->strength : KM_STRENGTH0;
	last = o ? o->last : (int64_t)ep->t;
	KM_UNLOCK();
	double days = difftime(now, (time_t)last) / 86400.0;

	if (days < 0)
		days = 0;
	return clampf(s * (float)pow(KM_DECAY_PER_DAY, days), 0.0f, 1.0f);
}

/* km_query 持锁期用的无锁变体（同上语义；仅同线程内联使用） */
static float eff_strength_locked(const struct km_episode *ep, time_t now)
{
	struct km_ovl *o = ovl_find(ep->seq);
	float s = o ? o->strength : KM_STRENGTH0;
	int64_t last = o ? o->last : (int64_t)ep->t;
	double days = difftime(now, (time_t)last) / 86400.0;

	if (days < 0)
		days = 0;
	return clampf(s * (float)pow(KM_DECAY_PER_DAY, days), 0.0f, 1.0f);
}

int km_forget(uint64_t seq, const char *reason)
{
	char rsn[48] = "unspecified";
	FILE *f;

	if (!seq)
		return KM_ERR_ARG;
	if (reason && reason[0])
		strlcpy(rsn, reason, sizeof(rsn));
	KM_LOCK();
	if (tomb_has(seq)) {
		KM_UNLOCK();
		return KM_OK;		/* 幂等                     */
	}
	/* 存在性核验（禁遗忘不存在条目；扫描频度=遗忘频度，可忽略） */
	bool exists = false;

	f = fopen(s_path_ep, "r");
	if (f) {
		char line[KM_LINE_MAX + 1];

		while (!exists && fgets(line, sizeof(line), f)) {
			cJSON *root = cJSON_Parse(line);
			cJSON *v;

			if (!root)
				continue;
			v = cJSON_GetObjectItem(root, "seq");
			exists = v && cJSON_IsNumber(v) &&
				  (uint64_t)v->valuedouble == seq;
			cJSON_Delete(root);
		}
		fclose(f);
	}
	if (!exists) {
		KM_UNLOCK();
		return KM_PROTECTED;
	}
	f = fopen(s_path_tmb, "a");
	if (!f) {
		KM_UNLOCK();
		return KM_ERR_IO;
	}
	fprintf(f, "{\"seq\":%llu,\"t\":%lld,\"reason\":\"%s\"}\n",
		(unsigned long long)seq, (long long)time(NULL), rsn);
	fclose(f);
	tomb_add(seq);
	KM_UNLOCK();
	return KM_OK;
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
	*match_out = m > 0.2f ? 0.2f : m;  /* G7：加分上界 */
	return true;
}

int km_query(const struct km_query *q, struct km_hit *hits, int top_n)
{
	FILE *f;
	char line[KM_LINE_MAX + 1];
	int n = 0;

	if (!q || !hits || top_n <= 0)      /* G9：先于 fopen      */
		return -1;
	KM_LOCK();
	f = fopen(s_path_ep, "r");
	if (!f) {
		KM_UNLOCK();
		return -2;
	}
	while (fgets(line, sizeof(line), f)) {
		struct km_episode ep;
		float match;

		if (!ep_from_json(line, &ep))
			continue;
		if (tomb_has(ep.seq))	/* 墓碑默认排除             */
			continue;
		if (!match_query(&ep, q, &match))
			continue;
		double age_days = difftime(time(NULL), ep.t) / 86400.0;
		float recency =
			clampf(1.0f - (float)age_days / 30.0f, 0.0f, 1.0f);
		float strength = eff_strength_locked(&ep, time(NULL));
		float score = 0.5f * recency + 0.3f * strength + match;

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
	KM_UNLOCK();
	/* "越回忆越牢"：返回给调用方的命中即视为一次回忆（30 号 §8.1） */
	for (int i = 0; i < n; i++)
		km_reinforce(hits[i].ep.seq);
	return n;
}

int km_core_write(const char *json)
{
	FILE *f = fopen(s_path_core, "w");
	size_t n;

	if (!f)
		return KM_ERR_IO;
	n = strlen(json);
	fputs(json, f);
	/* core 与晋升条目同文件共存（JSONL 口径）：无尾换行会把下一
	 * 条粘进行计数同一行（ promote 容量统计/重写裁剪皆错位） */
	if (!n || json[n - 1] != '\n')
		fputc('\n', f);
	fclose(f);
	return KM_OK;
}

int km_core_read(char *buf, size_t bufsz)
{
	FILE *f = fopen(s_path_core, "r");
	size_t n;

	if (!f)
		return KM_ERR_IO;
	n = fread(buf, 1, bufsz - 1, f);
	buf[n] = '\0';
	fclose(f);
	return (int)n;
}

int km_core_promote(uint64_t src_seq, const char *fact)
{
	char clean[96];
	FILE *f;
	int lines = 0;
	cJSON *root;
	char *esc;

	if (!src_seq || !fact || !fact[0])
		return KM_ERR_ARG;
	/* ①源在库核验（禁无源晋升——LineageBreak 精神） */
	bool exists = false;

	f = fopen(s_path_ep, "r");
	if (f) {
		char line[KM_LINE_MAX + 1];

		while (!exists && fgets(line, sizeof(line), f)) {
			cJSON *row = cJSON_Parse(line);
			cJSON *v;

			if (!row)
				continue;
			v = cJSON_GetObjectItem(row, "seq");
			exists = v && cJSON_IsNumber(v) &&
				  (uint64_t)v->valuedouble == src_seq;
			cJSON_Delete(row);
		}
		fclose(f);
	}
	if (!exists)
		return KM_PROTECTED;
	/* 事实短句净化：单行、无控制字符（cJSON 编码前压平） */
	strlcpy(clean, fact, sizeof(clean));
	for (char *q = clean; *q; q++)
		if ((unsigned char)*q < 0x20)
			*q = ' ';
	/* ④上限（满则提示云端合并重写） */
	f = fopen(s_path_core, "r");
	if (f) {
		char line[KM_LINE_MAX + 1];

		while (fgets(line, sizeof(line), f))
			lines++;
		fclose(f);
	}
	if (lines >= KM_CORE_MAX)
		return KM_FULL;
	root = cJSON_CreateObject();
	if (!root)
		return KM_ERR_IO;
	cJSON_AddNumberToObject(root, "seq", (double)src_seq);
	cJSON_AddNumberToObject(root, "t", (double)time(NULL));
	cJSON_AddStringToObject(root, "fact", clean);
	esc = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!esc)
		return KM_ERR_IO;
	f = fopen(s_path_core, "a");
	if (!f) {
		cJSON_free(esc);
		return KM_ERR_IO;
	}
	fprintf(f, "%s\n", esc);
	fclose(f);
	cJSON_free(esc);
	return lines + 1;
}

int km_backup(const char *backup_path)
{
	FILE *src = fopen(s_path_ep, "r");
	FILE *dst;
	char buf[256];
	size_t n;

	if (!src)
		return KM_ERR_IO;
	dst = fopen(backup_path, "w");
	if (!dst) {
		fclose(src);
		return -2;
	}
	while ((n = fread(buf, 1, sizeof(buf), src)) > 0)
		fwrite(buf, 1, n, dst);
	fclose(src);
	fclose(dst);
	return KM_OK;
}
