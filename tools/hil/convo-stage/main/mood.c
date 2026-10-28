/*
 * mood.c — mood 标签剥离实现（纯函数；stdio/string 依赖，Unity 可测）
 */
#include <stdio.h>
#include <string.h>

#include "mood.h"

#define MOOD_TAIL_MAX	32
#define MOOD_PFX	"<mood"

static char s_tail[MOOD_TAIL_MAX];
static bool s_seen;	/* 每轮只转发首个标签 */

void mood_round_reset(void)
{
	s_tail[0] = 0;
	s_seen = false;
}

bool mood_round_end(void)
{
	bool dropped = s_tail[0] != 0;

	s_tail[0] = 0;
	return dropped;
}

size_t mood_process(char *work, size_t cap, size_t len,
		    void (*on_mood)(float delta))
{
	char *gt;
	char *tag;

	(void)cap;
	/* 1) 接回上一子句扣下的尾巴 */
	if (s_tail[0]) {
		size_t tl = strlen(s_tail);

		memmove(work + tl, work, len + 1);
		memcpy(work, s_tail, tl);
		len += tl;
		s_tail[0] = 0;
	}
	/* 2) 未闭合的 "<mood" 前缀尾巴扣下（完整或部分前缀皆算） */
	{
		char *lt = memchr(work, '<', len);
		char *last = NULL;
		size_t lt_len;

		/* 取最后一个 '<'（防句中既有 '<' 又有半个标签） */
		while (lt) {
			last = lt;
			lt = memchr(lt + 1, '<', len - (size_t)(lt - work) - 1);
		}
		lt = last;
		if (lt) {
			size_t off = (size_t)(lt - work);

			gt = memchr(lt, '>', len - off);
			if (!gt) {
				lt_len = len - off;
				if (lt_len < MOOD_TAIL_MAX &&
				    (lt_len >= 5 ?
				     strncmp(lt, MOOD_PFX, 5) == 0 :
				     strncmp(MOOD_PFX, lt, lt_len) == 0)) {
					memcpy(s_tail, lt, lt_len);
					s_tail[lt_len] = 0;
					len = off;
				}
			}
		}
	}
	work[len] = 0;
	/* 3) 剥完整标签 → 转发首个 delta */
	while ((tag = strstr(work, MOOD_PFX)) != NULL) {
		gt = strchr(tag, '>');
		if (!gt)
			break;		/* 未闭合（尾巴已扣则不达此处） */
		if (!s_seen) {
			float d = 0.0f;
			unsigned t8 = 0;

			if (sscanf(tag, MOOD_PFX " d=\"%f\" t=\"%u\"",
				   &d, &t8) >= 1) {
				s_seen = true;
				if (on_mood)
					on_mood(d);
			}
		}
		memmove(tag, gt + 1, strlen(gt + 1) + 1);
	}
	/* 4) 换行清理（剥离后残留的首/尾 \r\n 不入 TTS） */
	len = strlen(work);
	{
		size_t lead = 0;

		while (lead < len && (work[lead] == '\n' ||
				      work[lead] == '\r'))
			lead++;
		if (lead) {
			memmove(work, work + lead, len - lead + 1);
			len -= lead;
		}
	}
	while (len && (work[len - 1] == '\n' || work[len - 1] == '\r'))
		work[--len] = 0;
	work[len] = 0;
	return len;
}
