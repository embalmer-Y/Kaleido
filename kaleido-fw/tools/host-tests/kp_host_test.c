/* host 回归：persona 解析（合法/各拒装路径）+ 注册表安装/激活/去重 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "kaleido_persona.h"

#define P(s) #s

static const char *GOOD =
	"{\"schema_version\":\"1\","
	"\"id\":\"com.kaleido.persona.momo\","
	"\"name\":\"momo\",\"version\":\"1.0.0\","
	"\"character_card\":{\"system_prompt\":\"card/sp.md\"},"
	"\"voice\":{\"provider\":\"dashscope\","
	"\"model\":\"cosyvoice-v3-flash\","
	"\"voice_id\":\"clone-xxx\","
	"\"fallback_voice_id\":\"longanyang\"},"
	"\"expression\":{\"emotions\":["
	"{\"tag\":\"neutral\",\"asset\":\"emo/n.eaf\","
	"\"loop\":true,\"fps\":20},"
	"{\"tag\":\"happy\",\"asset\":\"emo/h.eaf\","
	"\"loop\":true,\"fps\":20},"
	"{\"tag\":\"sad\",\"asset\":\"emo/s.eaf\","
	"\"loop\":true,\"fps\":12}]},"
	"\"affect\":{\"initial\":{\"valence\":0.1,"
	"\"arousal\":0.3},"
	"\"tau\":{\"int\":4,\"dec\":16}},"
	"\"ignition\":{\"proactivity\":5,"
	"\"daily_quota\":4}}";

int main(void)
{
	struct kp_persona p;
	const char *err = "";
	int rc;

	rc = kp_parse(GOOD, strlen(GOOD), &p, &err);
	if (rc) { printf("FAIL good: %d %s\n", rc, err); return 1; }
	if (strcmp(p.id, "com.kaleido.persona.momo") ||
	    strcmp(p.voice.fallback_voice_id, "longanyang") ||
	    p.emotion_cnt != 3 || !p.emotions[0].loop ||
	    p.emotions[2].fps != 12 ||
	    p.affect.tau_dec != 16.0f || p.ignition.proactivity != 5) {
		printf("FAIL fields\n");
		return 1;
	}

	/* 必填缺失：无 fallback_voice_id → 拒装 */
	const char *NOFB =
		"{\"schema_version\":\"1\",\"id\":\"a\","
		"\"name\":\"b\",\"version\":\"1\","
		"\"voice\":{\"voice_id\":\"x\"}}";

	rc = kp_parse(NOFB, strlen(NOFB), &p, &err);
	if (rc != KP_E_FIELD || strcmp(err, "voice.fallback_voice_id")) {
		printf("FAIL nofb: %d %s\n", rc, err);
		return 1;
	}

	/* 路径越界（asset 含 ..） */
	const char *EVIL =
		"{\"schema_version\":\"1\",\"id\":\"a\","
		"\"name\":\"b\",\"version\":\"1\","
		"\"voice\":{\"fallback_voice_id\":\"f\"},"
		"\"expression\":{\"emotions\":[{\"tag\":\"x\","
		"\"asset\":\"../../etc/passwd\"}]}}";

	rc = kp_parse(EVIL, strlen(EVIL), &p, &err);
	if (rc != KP_E_PATH) { printf("FAIL evil: %d\n", rc); return 1; }

	/* S2：id 路径逃逸（"../escape"）→ 拒 */
	const char *ESCAPE =
		"{\"schema_version\":\"1\",\"id\":\"../escape\","
		"\"name\":\"b\",\"version\":\"1\","
		"\"voice\":{\"fallback_voice_id\":\"f\"}}";

	rc = kp_parse(ESCAPE, strlen(ESCAPE), &p, &err);
	if (rc != KP_E_PATH || strcmp(err, "id")) {
		printf("FAIL escape: %d %s\n", rc, err);
		return 1;
	}

	/* G11：缺 schema_version → 拒 */
	const char *NOSCH =
		"{\"id\":\"a\",\"name\":\"b\",\"version\":\"1\","
		"\"voice\":{\"fallback_voice_id\":\"f\"}}";

	rc = kp_parse(NOSCH, strlen(NOSCH), &p, &err);
	if (rc != KP_E_FIELD) { printf("FAIL nosch: %d\n", rc); return 1; }

	/* 注册表 */
	const char *DIR = "/tmp/kptest_reg";
	char cmd[256];

	snprintf(cmd, sizeof(cmd), "rm -rf %s", DIR);
	if (system(cmd))
		return 1;
	if (kp_registry_init(DIR)) { printf("FAIL reg init\n"); return 1; }
	if (kp_parse(GOOD, strlen(GOOD), &p, &err))
		return 1;
	if (kp_install(&p) != KP_OK) { printf("FAIL install\n"); return 1; }
	if (kp_install(&p) != KP_E_EXISTS) { printf("FAIL dup\n"); return 1; }
	if (kp_set_active("ghost") != KP_E_NOTFOUND) {
		printf("FAIL ghost active\n");
		return 1;
	}
	if (kp_set_active(p.id) != KP_OK) {
		printf("FAIL active\n");
		return 1;
	}
	if (kp_count() != 1) { printf("FAIL count\n"); return 1; }

	snprintf(cmd, sizeof(cmd), "mkdir -p %s/%s", DIR, p.id);
	if (system(cmd))
		return 1;
	snprintf(cmd, sizeof(cmd), "%s/%s/persona.json", DIR, p.id);
	FILE *f = fopen(cmd, "w");

	if (!f)
		return 1;
	fputs(GOOD, f);
	fclose(f);
	struct kp_persona act;

	if (kp_active(&act) != 1 || strcmp(act.id, p.id)) {
		printf("FAIL active read\n");
		return 1;
	}
	printf("KP_HOST_TEST,PASS,count=%d,active=%s\n",
	       kp_count(), act.id);
	return 0;
}
