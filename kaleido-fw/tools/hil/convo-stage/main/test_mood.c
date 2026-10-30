/*
 * test_mood.c — mood 剥离主机回归（纯函数；run_all.sh 直编）
 *
 * 覆盖：直通/整标签/纯标签句/跨子句切分/双标签取首/字面 '<' 不误吞/
 * 未闭合尾巴轮末丢弃且不泄漏/剥离后空尾清理。
 */
#include <string.h>

#include "mood.h"
#include "unity.h"

#define CAP	128

static float s_got;
static int s_got_n;

static void on_mood(float d)
{
	s_got = d;
	s_got_n++;
}

/* 处理一条子句的便捷包装（模拟 pump 回调路径） */
static size_t proc(char *buf, const char *in)
{
	strcpy(buf, in);
	return mood_process(buf, CAP, strlen(buf), on_mood);
}

static void test_passthrough_no_tag(void)
{
	char b[CAP];

	s_got_n = 0;
	mood_round_reset();
	TEST_ASSERT_EQUAL_UINT32(strlen("你好呀。"),
				 proc(b, "你好呀。"));
	TEST_ASSERT_EQUAL_STRING("你好呀。", b);
	TEST_ASSERT_EQUAL_INT(0, s_got_n);
	TEST_ASSERT_FALSE(mood_round_end());
}

static void test_full_tag_stripped(void)
{
	char b[CAP];

	s_got_n = 0;
	mood_round_reset();
	TEST_ASSERT_EQUAL_UINT32(strlen("今天真开心！"),
				 proc(b, "今天真开心！"
				     "<mood d=\"+0.1\" t=\"6\"/>"));
	TEST_ASSERT_EQUAL_STRING("今天真开心！", b);
	TEST_ASSERT_EQUAL_INT(1, s_got_n);
	TEST_ASSERT_TRUE(s_got > 0.09f && s_got < 0.11f);
}

static void test_tag_only_clause(void)
{
	char b[CAP];

	s_got_n = 0;
	mood_round_reset();
	TEST_ASSERT_EQUAL_UINT32(0, proc(b, "<mood d=\"-0.2\" t=\"4\"/>"));
	TEST_ASSERT_EQUAL_STRING("", b);
	TEST_ASSERT_EQUAL_INT(1, s_got_n);
	TEST_ASSERT_TRUE(s_got < -0.19f);
}

/* 跨子句切分：'<moo' + 'd d=...' 各半 */
static void test_split_across_clauses(void)
{
	char b[CAP];

	s_got_n = 0;
	mood_round_reset();
	TEST_ASSERT_EQUAL_UINT32(strlen("太好笑了"),
				 proc(b, "太好笑了<moo"));
	TEST_ASSERT_EQUAL_STRING("太好笑了", b);
	TEST_ASSERT_EQUAL_INT(0, s_got_n);	/* 尚未完整 */
	TEST_ASSERT_EQUAL_UINT32(strlen("哈哈"),
				 proc(b, "d d=\"+0.2\" t=\"8\"/>哈哈"));
	TEST_ASSERT_EQUAL_STRING("哈哈", b);
	TEST_ASSERT_EQUAL_INT(1, s_got_n);
	TEST_ASSERT_TRUE(s_got > 0.19f);
}

/* 双标签：只转发首个，两个都剥净 */
static void test_two_tags_first_only(void)
{
	char b[CAP];

	s_got_n = 0;
	mood_round_reset();
	TEST_ASSERT_EQUAL_UINT32(strlen("好的"),
				 proc(b, "<mood d=\"+0.1\" t=\"4\"/>好的"
				     "<mood d=\"-0.1\" t=\"4\"/>"));
	TEST_ASSERT_EQUAL_STRING("好的", b);
	TEST_ASSERT_EQUAL_INT(1, s_got_n);
	TEST_ASSERT_TRUE(s_got > 0.09f);
}

/* 字面 '<'（非 mood 前缀）不误吞 */
static void test_literal_lt_kept(void)
{
	char b[CAP];

	s_got_n = 0;
	mood_round_reset();
	TEST_ASSERT_EQUAL_UINT32(strlen("a<b 且 x<y"),
				 proc(b, "a<b 且 x<y"));
	TEST_ASSERT_EQUAL_STRING("a<b 且 x<y", b);
	TEST_ASSERT_EQUAL_INT(0, s_got_n);
}

/* 未闭合尾巴：轮末丢弃，不泄漏进下一轮 */
static void test_unclosed_tail_dropped(void)
{
	char b[CAP];

	s_got_n = 0;
	mood_round_reset();
	proc(b, "好<mood d=\"+0.1\"");
	TEST_ASSERT_TRUE(mood_round_end());	/* 报告丢弃 */
	s_got_n = 0;
	mood_round_reset();
	TEST_ASSERT_EQUAL_UINT32(strlen("下一轮干净起句"),
				 proc(b, "下一轮干净起句"));
	TEST_ASSERT_EQUAL_STRING("下一轮干净起句", b);
}

/* 剥离后残留换行清理：空尾不入 TTS */
static void test_strip_leaves_clean_tail(void)
{
	char b[CAP];

	s_got_n = 0;
	mood_round_reset();
	TEST_ASSERT_EQUAL_UINT32(strlen("好。"),
				 proc(b, "<mood d=\"+0.1\" t=\"4\"/>\r\n"
				     "好。\n"));
	TEST_ASSERT_EQUAL_STRING("好。", b);
}

void mood_tests_main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_passthrough_no_tag);
	RUN_TEST(test_full_tag_stripped);
	RUN_TEST(test_tag_only_clause);
	RUN_TEST(test_split_across_clauses);
	RUN_TEST(test_two_tags_first_only);
	RUN_TEST(test_literal_lt_kept);
	RUN_TEST(test_unclosed_tail_dropped);
	RUN_TEST(test_strip_leaves_clean_tail);
	UNITY_END();
}
