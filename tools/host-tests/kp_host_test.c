/* host 回归：parse（合法包/各拒装路径）+ 注册表安装/激活/去重 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "kaleido_persona.h"

static const char *GOOD =
"{\"id\":\"com.kaleido.persona.momo\",\"name\":\"墨墨\",\"version\":\"1.0.0\","
"\"character_card\":{\"system_prompt\":\"card/sp.md\"},"
"\"voice\":{\"provider\":\"dashscope\",\"model\":\"cosyvoice-v3-flash\","
"\"voice_id\":\"clone-xxx\",\"fallback_voice_id\":\"longanyang\"},"
"\"expression\":{\"emotions\":[{\"tag\":\"happy\",\"asset\":\"emo/happy.eaf\",\"loop\":true,\"fps\":20},"
"{\"tag\":\"sad\",\"asset\":\"emo/sad.eaf\",\"loop\":true,\"fps\":12}]},"
"\"affect\":{\"initial\":{\"valence\":0.1,\"arousal\":0.3},\"tau\":{\"int\":4,\"dec\":16}},"
"\"ignition\":{\"proactivity\":5,\"daily_quota\":4}}";

int main(void)
{
	struct kp_persona p;
	const char *err = "";
	int rc;

	rc = kp_parse(GOOD, strlen(GOOD), &p, &err);
	if (rc) { printf("FAIL good: %d %s\n", rc, err); return 1; }
	if (strcmp(p.id, "com.kaleido.persona.momo") ||
	    strcmp(p.voice.fallback_voice_id, "longanyang") ||
	    p.emotion_cnt != 2 || !p.emotions[0].loop ||
	    p.emotions[1].fps != 12 ||
	    p.affect.tau_dec != 16.0f || p.ignition.proactivity != 5) {
		printf("FAIL fields\n");
		return 1;
	}

	/* 必填缺失：无 fallback_voice_id → 拒装 */
	const char *NOFB = "{\"id\":\"a\",\"name\":\"b\",\"version\":\"1\","
		"\"voice\":{\"voice_id\":\"x\"}}";
	rc = kp_parse(NOFB, strlen(NOFB), &p, &err);
	if (rc != KP_E_FIELD || strcmp(err, "voice.fallback_voice_id")) {
		printf("FAIL nofb: %d %s\n", rc, err);
		return 1;
	}

	/* 路径越界 */
	const char *EVIL = "{\"id\":\"a\",\"name\":\"b\",\"version\":\"1\","
		"\"voice\":{\"fallback_voice_id\":\"f\"},"
		"\"expression\":{\"emotions\":[{\"tag\":\"x\","
		"\"asset\":\"../../etc/passwd\"}]}}";
	rc = kp_parse(EVIL, strlen(EVIL), &p, &err);
	if (rc != KP_E_PATH) { printf("FAIL evil: %d\n", rc); return 1; }

	/* 注册表：init/install/dup/active/count */
	const char *DIR = "/tmp/v33/kptest/reg";
	char buf[256];
	snprintf(buf, sizeof(buf), "rm -rf %s", DIR);
	if (system(buf)) {}
	if (kp_registry_init(DIR)) { printf("FAIL reg init\n"); return 1; }
	if (kp_parse(GOOD, strlen(GOOD), &p, &err)) { return 1; }
	if (kp_install(&p) != KP_OK) { printf("FAIL install\n"); return 1; }
	if (kp_install(&p) != KP_E_EXISTS) { printf("FAIL dup\n"); return 1; }
	if (kp_set_active(p.id) != KP_OK) { printf("FAIL active\n"); return 1; }
	if (kp_count() != 1) { printf("FAIL count\n"); return 1; }

	/* 激活包读取：需按注册表布局落盘 persona.json */
	char path[192];
	snprintf(path, sizeof(path), "mkdir -p %s/%s", DIR, p.id);
	if (system(path)) {}
	snprintf(path, sizeof(path), "%s/%s/persona.json", DIR, p.id);
	FILE *f = fopen(path, "w");
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
