/*
 * kaleido_app.h — KApp 框架公共 API
 *
 * 设计依据：docs/design/kaleido-claw-架构.md §2（App 模型）
 *  - App = manifest + 权限位图 + 生命周期（承 Tessera 思想，简化鉴权）
 *  - 内置 App（C 静态注册）与 Lua App（M1 接入）同一套接口
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 权限位图（8 类，架构 §2.3）---- */
typedef enum {
    KPERM_UI       = (1 << 0),  /* LVGL 画布/页面栈            */
    KPERM_AUDIO_PLAY = (1 << 1),/* 播放队列                    */
    KPERM_AUDIO_REC = (1 << 2), /* 录音（含唤醒接管）           */
    KPERM_NET      = (1 << 3),  /* HTTP/WebSocket              */
    KPERM_STORAGE  = (1 << 4),  /* App 沙箱目录                */
    KPERM_SENSOR   = (1 << 5),  /* IMU/摄像头读                */
    KPERM_ACCESSORY = (1 << 6), /* 扩展配件读写                */
    KPERM_SYSTEM   = (1 << 7),  /* 系统信息/设置写             */
} kperm_bit_t;

#define KPERM_ALL (0xFFu)

/* ---- 生命周期（架构 §2.4）---- */
typedef enum {
    KAPP_INSTALLED = 0,   /* 已登记，未加载                   */
    KAPP_LOADED,          /* 资源就绪（Lua VM/资产）          */
    KAPP_RUNNING,         /* 运行中（前台或后台）             */
    KAPP_PAUSED,          /* 被暂停（LRU 或用户）            */
    KAPP_DISABLED,        /* 崩溃≥2 次自动禁用（回滚保护）    */
} kapp_state_t;

/* App 来源（无鉴权链，仅标记，UI 徽标呈现） */
typedef enum {
    KAPP_SRC_BUILTIN = 0, /* 出厂内置                        */
    KAPP_SRC_CLAW,        /* Claw agent 生成                 */
    KAPP_SRC_USB,         /* USB 侧载                        */
    KAPP_SRC_WIFI,        /* WiFi 侧载                       */
} kapp_source_t;

#define KAPP_ID_MAX     64
#define KAPP_NAME_MAX   48
#define KAPP_VER_MAX    16
#define KAPP_DESC_MAX  128

/* ---- manifest（对应 manifest.json 字段，架构 §2.2）---- */
typedef struct {
    char     id[KAPP_ID_MAX];        /* com.kaleido.xxx 反域名      */
    char     name[KAPP_NAME_MAX];
    char     version[KAPP_VER_MAX];  /* semver 字符串               */
    uint8_t  api;                    /* KApp API 版本，当前 =1      */
    char     entry[64];              /* Lua: main.lua；内置: 填 "c" */
    uint32_t permissions;            /* kperm_bit_t 位或            */
    kapp_source_t source;
    char     desc[KAPP_DESC_MAX];
} kapp_manifest_t;

/* ---- App 实现回调（内置 App 用 C；Lua App 由宿主适配器桥接）---- */
typedef struct kapp_app {
    kapp_manifest_t manifest;
    kapp_state_t    state;
    uint8_t         crash_count;
    /* 生命周期回调，均可为 NULL（框架按需驱动） */
    int (*on_load)(struct kapp_app *app);    /* INSTALLED→LOADED   */
    int (*on_start)(struct kapp_app *app);   /* LOADED/PAUSED→RUNNING */
    void (*on_pause)(struct kapp_app *app);  /* RUNNING→PAUSED     */
    void (*on_stop)(struct kapp_app *app);   /* →INSTALLED（卸载前）*/
    void *user_ctx;
} kapp_app_t;

/* ---- API 版本 ---- */
#define KAPP_API_VERSION 1

/* ---- 注册表 ---- */

/**
 * 注册一个 App（内置 App 启动期调用；动态 App 安装时同入口）。
 * 幂等：重复 id 返回已存在条目。
 * 成功返回 0；注册表满/manifest 非法返回 -1/-2。
 */
int kapp_register(kapp_app_t *app);

/** 按 id 查找，未找到返回 NULL。 */
kapp_app_t *kapp_find(const char *id);

/** 注册表容量与当前数量（KShell 网格渲染用）。 */
size_t kapp_count(void);

/* ---- manifest 校验与解析 ---- */

/** 结构级校验（id 格式/权限位范围/api 版本）。0=合法。 */
int kapp_manifest_validate(const kapp_manifest_t *m);

/**
 * 从 JSON 文本解析 manifest（安装链路用，cJSON 实现）。
 * 缺省字段容忍（entry/desc 可空），id/name/version 必填。
 * 返回 0 成功；-3 JSON 语法错误；-4 必填缺失；-5 api 版本不识别。
 */
int kapp_manifest_parse_json(const char *json, size_t len, kapp_manifest_t *out);

/* ---- 权限 ---- */

/** App 是否持有全部 want 位。 */
bool kapp_has_perms(const kapp_app_t *app, uint32_t want);

/* ---- 生命周期驱动 ---- */

/** 推进到目标状态（幂等；跨多步时按序执行回调）。
 *  回调失败或崩溃计数达 2 → 进入 DISABLED 并返回 -10。
 *  禁用 App 只能经 kapp_reenable()（用户在设置中操作）。 */
int kapp_transition(kapp_app_t *app, kapp_state_t target);

/** 用户手动重新启用（清崩溃计数，回到 INSTALLED）。 */
int kapp_reenable(kapp_app_t *app);

/** 生命周期事件钩子（审计/Toast，KShell 注册）。 */
typedef void (*kapp_event_cb_t)(kapp_app_t *app, kapp_state_t from, kapp_state_t to);
void kapp_set_event_listener(kapp_event_cb_t cb);

#ifdef __cplusplus
}
#endif
