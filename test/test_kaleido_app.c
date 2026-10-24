/*
 * test_kaleido_app.c — KApp 框架单测（Unity，纯逻辑，不依赖硬件）
 *
 * 覆盖：manifest JSON 解析、校验、权限判定、生命周期状态机、崩溃禁用回滚。
 * 运行：M1 接入 idf.py 单测（constraints §6.5 语句覆盖 ≥70% 目标的先行核心集）。
 */
#include <string.h>
#include <stdio.h>

#include "unity.h"
#include "kaleido_app.h"

static const char *MANIFEST_OK =
    "{"
    "\"id\":\"com.kaleido.pomodoro\","
    "\"name\":\"番茄钟\","
    "\"version\":\"1.0.0\","
    "\"api\":1,"
    "\"entry\":\"main.lua\","
    "\"icon\":\"icon.png\","
    "\"permissions\":[\"ui\",\"audio_play\"],"
    "\"desc\":\"25 分钟专注\""
    "}";

static kapp_app_t make_app(const char *id)
{
    kapp_app_t a = {0};
    strlcpy(a.manifest.id, id, KAPP_ID_MAX);
    strlcpy(a.manifest.name, "t", KAPP_NAME_MAX);
    strlcpy(a.manifest.version, "1.0.0", KAPP_VER_MAX);
    a.manifest.api = KAPP_API_VERSION;
    a.manifest.permissions = KPERM_UI;
    return a;
}

void test_manifest_parse_ok(void)
{
    kapp_manifest_t m;
    TEST_ASSERT_EQUAL(0, kapp_manifest_parse_json(MANIFEST_OK, strlen(MANIFEST_OK), &m));
    TEST_ASSERT_EQUAL_STRING("com.kaleido.pomodoro", m.id);
    TEST_ASSERT_EQUAL_STRING("main.lua", m.entry);
    TEST_ASSERT_EQUAL_UINT32(KPERM_UI | KPERM_AUDIO_PLAY, m.permissions);
}

void test_manifest_parse_bad_json(void)
{
    kapp_manifest_t m;
    TEST_ASSERT_EQUAL(-3, kapp_manifest_parse_json("{oops", 5, &m));
}

void test_manifest_parse_missing_required(void)
{
    kapp_manifest_t m;
    const char *j = "{\"name\":\"x\",\"version\":\"1\",\"api\":1}";
    TEST_ASSERT_EQUAL(-4, kapp_manifest_parse_json(j, strlen(j), &m));
}

void test_manifest_parse_bad_api(void)
{
    kapp_manifest_t m;
    const char *j = "{\"id\":\"a.b\",\"name\":\"x\",\"version\":\"1\",\"api\":2}";
    TEST_ASSERT_EQUAL(-5, kapp_manifest_parse_json(j, strlen(j), &m));
}

void test_manifest_validate_rejects_bad_id(void)
{
    kapp_manifest_t m = {0};
    strlcpy(m.id, "No Dot!", KAPP_ID_MAX);
    strlcpy(m.name, "x", KAPP_NAME_MAX);
    strlcpy(m.version, "1", KAPP_VER_MAX);
    m.api = KAPP_API_VERSION;
    TEST_ASSERT_EQUAL(-1, kapp_manifest_validate(&m));
}

void test_lifecycle_run_pause_stop(void)
{
    kapp_app_t a = make_app("test.life");
    TEST_ASSERT_EQUAL(0, kapp_register(&a));
    TEST_ASSERT_EQUAL(0, kapp_transition(&a, KAPP_RUNNING));
    TEST_ASSERT_EQUAL(KAPP_RUNNING, a.state);
    TEST_ASSERT_EQUAL(0, kapp_transition(&a, KAPP_PAUSED));
    TEST_ASSERT_EQUAL(0, kapp_transition(&a, KAPP_RUNNING));
    TEST_ASSERT_EQUAL(0, kapp_transition(&a, KAPP_INSTALLED));
    TEST_ASSERT_EQUAL(KAPP_INSTALLED, a.state);
}

static int fail_on_start(kapp_app_t *app) { (void)app; return -1; }

void test_crash_disable_and_reenable(void)
{
    kapp_app_t a = make_app("test.crash");
    a.on_start = fail_on_start;
    kapp_register(&a);

    TEST_ASSERT_EQUAL(-10, kapp_transition(&a, KAPP_RUNNING));
    TEST_ASSERT_EQUAL_INT(1, a.crash_count);
    TEST_ASSERT_EQUAL(KAPP_LOADED, a.state); /* 1 次失败未禁用 */

    TEST_ASSERT_EQUAL(-10, kapp_transition(&a, KAPP_RUNNING));
    TEST_ASSERT_EQUAL(KAPP_DISABLED, a.state); /* 第 2 次 → 自动禁用 */
    TEST_ASSERT_EQUAL(-10, kapp_transition(&a, KAPP_RUNNING)); /* 禁用后拒绝 */

    TEST_ASSERT_EQUAL(0, kapp_reenable(&a));
    TEST_ASSERT_EQUAL(KAPP_INSTALLED, a.state);
}

void test_perm_check(void)
{
    kapp_app_t a = make_app("test.perm");
    a.manifest.permissions = KPERM_UI | KPERM_NET;
    TEST_ASSERT_TRUE(kapp_has_perms(&a, KPERM_UI));
    TEST_ASSERT_TRUE(kapp_has_perms(&a, KPERM_UI | KPERM_NET));
    TEST_ASSERT_FALSE(kapp_has_perms(&a, KPERM_AUDIO_PLAY));
    TEST_ASSERT_FALSE(kapp_has_perms(&a, KPERM_ALL));
}
