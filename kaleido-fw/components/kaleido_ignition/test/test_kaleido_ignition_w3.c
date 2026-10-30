/*
 * test_kaleido_ignition_w3.c — W3 套件（思维闸/意图解析/出口门/结算六态）
 *
 * 独立文件避开 shell-heredoc 转义损耗；与存量 8 用例分套件互不污染。
 * 锚：29 号 §9（slow_clock 判据/C-5/结算六态/KI_PARSE_* 全码）。
 */
#include <string.h>

#include "kaleido_ignition.h"
#include "unity.h"

static void think_gate_triggers(void)
{
	struct ki_think_gate g;

	/* 驱力空闲（urgency≤0 ∧ 无 pending）→ idle 位触发 */
	ki_think_gate(&(struct ki_think_ctx){ .arousal = 0.9f,
			.now_min = 100 }, &g);
	TEST_ASSERT_TRUE(g.trigger);
	TEST_ASSERT_EQUAL_UINT8(1u << 0, g.reason_mask);

	/* pending 只挡 idle（slow_clock 语义：高 urgency 不挡低唤起） */
	ki_think_gate(&(struct ki_think_ctx){ .arousal = 0.9f,
			.pending_command = true, .now_min = 100 }, &g);
	TEST_ASSERT_FALSE(g.trigger);
	ki_think_gate(&(struct ki_think_ctx){ .arousal = 0.05f,
			.urgency_max = 0.9f, .pending_command = true,
			.now_min = 100 },
		      &g);
	TEST_ASSERT_TRUE(g.trigger);
	TEST_ASSERT_EQUAL_UINT8(1u << 1, g.reason_mask);

	/* 分箱跃迁 / 新事件 / salient 各自独立触发；无判据不触发 */
	ki_think_gate(&(struct ki_think_ctx){ .arousal = 0.9f,
			.urgency_max = 0.9f, .pending_command = true,
			.bin_changed = true, .now_min = 100 }, &g);
	TEST_ASSERT_TRUE(g.trigger);
	ki_think_gate(&(struct ki_think_ctx){ .arousal = 0.9f,
			.urgency_max = 0.9f, .pending_command = true,
			.new_event = true, .now_min = 100 }, &g);
	TEST_ASSERT_TRUE(g.trigger);
	ki_think_gate(&(struct ki_think_ctx){ .arousal = 0.9f,
			.urgency_max = 0.9f, .pending_command = true,
			.score = 0.6f, .now_min = 100 }, &g);
	TEST_ASSERT_TRUE(g.trigger);            /* ≥0.50 salient */
	ki_think_gate(&(struct ki_think_ctx){ .arousal = 0.9f,
			.urgency_max = 0.9f, .pending_command = true,
			.score = 0.49f, .now_min = 100 }, &g);
	TEST_ASSERT_FALSE(g.trigger);

	/* W4→W3 记忆钩子：top1≥0.5 独立触发（bit5）；低于阈不触发 */
	ki_think_gate(&(struct ki_think_ctx){ .arousal = 0.9f,
			.urgency_max = 0.9f, .pending_command = true,
			.mem_hook = 0.5f, .now_min = 100 }, &g);
	TEST_ASSERT_TRUE(g.trigger);
	TEST_ASSERT_EQUAL_UINT8(1u << 5, g.reason_mask);
	ki_think_gate(&(struct ki_think_ctx){ .arousal = 0.9f,
			.urgency_max = 0.9f, .pending_command = true,
			.mem_hook = 0.49f, .now_min = 100 }, &g);
	TEST_ASSERT_FALSE(g.trigger);
}

static void think_gate_budget(void)
{
	struct ki_think_gate g;
	struct ki_think_ctx c = { .arousal = 0.0f, .now_min = 100,
				  .last_think_min = 95 };

	/* 间隔 <10min → 拦（思维=云调用必须节流） */
	ki_think_gate(&c, &g);
	TEST_ASSERT_FALSE(g.trigger);
	TEST_ASSERT_EQUAL_UINT8(1u << 1, g.blocked_mask);
	c.last_think_min = 90;                  /* 恰 10min → 过 */
	ki_think_gate(&c, &g);
	TEST_ASSERT_TRUE(g.trigger);

	/* 日帽 */
	c.thought_today = KI_THINK_DAILY_MAX;
	ki_think_gate(&c, &g);
	TEST_ASSERT_FALSE(g.trigger);
	TEST_ASSERT_EQUAL_UINT8(1u << 2, g.blocked_mask);
	c.thought_today = 0;

	/* 勿扰（思维也省云） */
	c.quiet_hours = true;
	ki_think_gate(&c, &g);
	TEST_ASSERT_FALSE(g.trigger);
	c.quiet_hours = false;

	/* 新意门：上轮沉默+无跃迁+无新事件+分数无起色 → 不再想 */
	c.last_was_silent = true;
	c.last_silent_score = 0.2f;
	c.score = 0.2f;
	ki_think_gate(&c, &g);
	TEST_ASSERT_FALSE(g.trigger);
	TEST_ASSERT_EQUAL_UINT8(1u << 3, g.blocked_mask);
	c.score = 0.3f;                         /* 分数起色 → 放行 */
	ki_think_gate(&c, &g);
	TEST_ASSERT_TRUE(g.trigger);
	c.score = 0.2f;
	c.new_event = true;                     /* 新事件 → 放行 */
	ki_think_gate(&c, &g);
	TEST_ASSERT_TRUE(g.trigger);
}

static void intent_parse_good(void)
{
	struct ki_intent it;
	enum ki_parse_err e;

	e = ki_intent_parse("{\"act\":\"speak\",\"content\":\"你在忙吗？\","
			    "\"confidence\":0.8,"
			    "\"evidence\":[\"drive:social HIGH\"],"
			    "\"reason\":\"好久没聊\"}", 200, &it);
	TEST_ASSERT_EQUAL_INT(KI_PARSE_OK, e);
	TEST_ASSERT_EQUAL_INT(KI_SPEAK, it.act);
	TEST_ASSERT_EQUAL_STRING("你在忙吗？", it.content);
	TEST_ASSERT_TRUE(it.confidence > 0.79f && it.confidence < 0.81f);
	TEST_ASSERT_EQUAL_UINT8(1u << 0, it.evidence_mask);
	TEST_ASSERT_EQUAL_STRING("好久没聊", it.reason);

	/* 沉默一等输出：无需 content/证据 */
	e = ki_intent_parse("{\"act\":\"stay_silent\",\"confidence\":0.9,"
			    "\"evidence\":[],\"reason\":\"没话说\"}",
			    120, &it);
	TEST_ASSERT_EQUAL_INT(KI_PARSE_OK, e);
	TEST_ASSERT_EQUAL_INT(KI_STAY_SILENT, it.act);

	/* 置信钳位 [0,1] */
	e = ki_intent_parse("{\"act\":\"stay_silent\",\"confidence\":2.5,"
			    "\"evidence\":[]}", 100, &it);
	TEST_ASSERT_EQUAL_INT(KI_PARSE_OK, e);
	TEST_ASSERT_TRUE(it.confidence == 1.0f);
}

static void intent_parse_rejects(void)
{
	struct ki_intent it;

	/* schema 违例=拒，绝不猜 */
	TEST_ASSERT_EQUAL_INT(KI_PARSE_JSON_BAD,
			      ki_intent_parse("not json", 8, &it));
	/* act 越词表 */
	TEST_ASSERT_EQUAL_INT(KI_PARSE_ACT_ILLEGAL,
			      ki_intent_parse("{\"act\":\"dance\"}",
					      20, &it));
	/* speak 空内容 */
	TEST_ASSERT_EQUAL_INT(KI_PARSE_CONTENT_EMPTY,
			      ki_intent_parse("{\"act\":\"speak\","
				  "\"content\":\"\",\"evidence\":"
				  "[\"drive:x\"]}", 60, &it));
	/* speak 无证据（C-5：无内生证据不可成承诺） */
	TEST_ASSERT_EQUAL_INT(KI_PARSE_NO_EVIDENCE,
			      ki_intent_parse("{\"act\":\"speak\","
				  "\"content\":\"嗨\"}", 40, &it));
	/* 数字主张无 mem 引用（UncitedClaim 移植） */
	TEST_ASSERT_EQUAL_INT(KI_PARSE_UNCITED_NUMBER,
			      ki_intent_parse("{\"act\":\"speak\","
				  "\"content\":\"已经 3 天没见了\","
				  "\"evidence\":[\"drive:social\"]}",
				  120, &it));
	/* 带 mem 引用 → 数字放行 */
	TEST_ASSERT_EQUAL_INT(KI_PARSE_OK,
			      ki_intent_parse("{\"act\":\"speak\","
				  "\"content\":\"记得那 3 件事\","
				  "\"evidence\":[\"mem:3天\"]}", 120,
				  &it));
	/* 超长内容：截断留痕（非拒），不劈 UTF-8 多字节 */
	{
		char long_json[384];
		int i;

		strcpy(long_json, "{\"act\":\"speak\",\"content\":\"");
		for (i = 0; i < 50; i++)
			strcat(long_json, "好好");      /* 300B 载荷 > 80 */
		strcat(long_json, "\",\"evidence\":[\"drive:x\"]}");
		TEST_ASSERT_EQUAL_INT(KI_PARSE_CONTENT_LONG,
				      ki_intent_parse(long_json,
					      sizeof(long_json), &it));
		TEST_ASSERT_TRUE(strlen(it.content) <= KI_CONTENT_MAX);
		TEST_ASSERT_EQUAL_UINT8(1u << 0, it.evidence_mask);
	}
}

static void fire_gates_w3(void)
{
	struct ki_intent it = { .act = KI_SPEAK, .confidence = 0.9f };
	struct ki_decision d;
	struct ki_fire_ctx c = { .now_min = 100, .last_fire_min = 0,
			       .tts_ready = true };
	const struct ki_config *cfg = ki_config_default();

	ki_settle2(KI_SETTLE_RESPONDED);        /* 冷却归基线 */
	ki_intent_fire(&it, &c, cfg, &d);
	TEST_ASSERT_TRUE(d.candidate);
	TEST_ASSERT_EQUAL_UINT8(0, d.gates.gate_mask);

	c.quiet_hours = true;
	ki_intent_fire(&it, &c, cfg, &d);
	TEST_ASSERT_FALSE(d.candidate);
	TEST_ASSERT_EQUAL_UINT8(1u << 0, d.gates.gate_mask);
	c.quiet_hours = false;

	c.now_min = 100;
	c.last_fire_min = 85;                   /* <30min 冷却 */
	ki_intent_fire(&it, &c, cfg, &d);
	TEST_ASSERT_EQUAL_UINT8(1u << 1, d.gates.gate_mask);
	c.last_fire_min = 70;                   /* ≥30 → 过 */
	ki_intent_fire(&it, &c, cfg, &d);
	TEST_ASSERT_TRUE(d.candidate);

	c.fired_today = cfg->daily_quota;       /* 配额尽 */
	ki_intent_fire(&it, &c, cfg, &d);
	TEST_ASSERT_EQUAL_UINT8(1u << 2, d.gates.gate_mask);
	c.fired_today = 0;

	c.user_speaking = true;
	ki_intent_fire(&it, &c, cfg, &d);
	TEST_ASSERT_EQUAL_UINT8(1u << 3, d.gates.gate_mask);
	c.user_speaking = false;

	c.ethics_latched = true;
	ki_intent_fire(&it, &c, cfg, &d);
	TEST_ASSERT_EQUAL_UINT8(1u << 4, d.gates.gate_mask);
	c.ethics_latched = false;

	c.tts_ready = false;
	ki_intent_fire(&it, &c, cfg, &d);
	TEST_ASSERT_EQUAL_UINT8(1u << 5, d.gates.gate_mask);
}

static void settle2_six_states(void)
{
	/* 唯一翻倍路径=IGNORED；其余不罚（禁把未解决归 FAILED——
	 * 修"越感兴趣越沉默"缺陷） */
	ki_settle2(KI_SETTLE_RESPONDED);
	TEST_ASSERT_EQUAL_UINT16(30, ki_cooldown_current());
	TEST_ASSERT_EQUAL_UINT16(60, ki_settle2(KI_SETTLE_IGNORED));
	TEST_ASSERT_EQUAL_UINT16(120, ki_settle2(KI_SETTLE_IGNORED));
	TEST_ASSERT_EQUAL_UINT16(240, ki_settle2(KI_SETTLE_IGNORED));
	TEST_ASSERT_EQUAL_UINT16(240, ki_settle2(KI_SETTLE_IGNORED));
	TEST_ASSERT_EQUAL_UINT16(240,
				 ki_settle2(KI_SETTLE_INTERRUPTED));
	TEST_ASSERT_EQUAL_UINT16(240, ki_settle2(KI_SETTLE_SILENT));
	TEST_ASSERT_EQUAL_UINT16(240,
				 ki_settle2(KI_SETTLE_GATE_BLOCKED));
	TEST_ASSERT_EQUAL_UINT16(240,
				 ki_settle2(KI_SETTLE_TTL_EXPIRED));
	TEST_ASSERT_EQUAL_UINT16(30, ki_settle2(KI_SETTLE_RESPONDED));
}

void ignition_w3_tests_main(void)
{
	UNITY_BEGIN();
	RUN_TEST(think_gate_triggers);
	RUN_TEST(think_gate_budget);
	RUN_TEST(intent_parse_good);
	RUN_TEST(intent_parse_rejects);
	RUN_TEST(fire_gates_w3);
	RUN_TEST(settle2_six_states);
	UNITY_END();
}
