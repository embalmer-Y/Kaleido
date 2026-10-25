/*
 * kaleido_persona.h — .kpersona 角色包解析/校验/注册表（M2.1）
 *
 * 设计依据：docs/research/25-角色包与情感表达子系统设计.md §2
 *  - persona.json = 纯数据包（人设卡/音色绑定/动画索引/情感参数/点火风格）
 *  - 校验纪律：id/name/version/voice.fallback_voice_id 必填——
 *    无兜底音色拒装（25号 §2.4）；资产路径禁绝对路径与 ".."
 *  - 注册表：文件型索引（index.json），全局至多一个激活角色
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KP_ID_MAX	64
#define KP_NAME_MAX	48
#define KP_VER_MAX	16
#define KP_VOICE_MAX	96
#define KP_PROMPT_MAX	512
#define KP_EMOTIONS_MAX	12
#define KP_TAG_MAX	16

struct kp_voice {
	char provider[16];              /* dashscope|selfhosted        */
	char model[48];
	char voice_id[KP_VOICE_MAX];
	char fallback_voice_id[KP_VOICE_MAX]; /* 必填（拒装条件）  */
};

struct kp_emotion {
	char tag[KP_TAG_MAX];
	char asset[96];
	bool loop;
	uint8_t fps;
};

struct kp_affect {
	float valence, arousal;         /* 初值                        */
	float tau_int, tau_dec;         /* 情感参数档（承 18号 §4.5）  */
};

struct kp_ignition {
	uint8_t proactivity;            /* 0-10（0=完全被动）          */
	uint8_t daily_quota;            /* 覆盖系统默认                */
};

struct kp_persona {
	char id[KP_ID_MAX];
	char name[KP_NAME_MAX];
	char version[KP_VER_MAX];
	char system_prompt_ref[96];     /* card/system_prompt.md 路径  */
	struct kp_voice voice;
	struct kp_emotion emotions[KP_EMOTIONS_MAX];
	int emotion_cnt;
	struct kp_affect affect;
	struct kp_ignition ignition;
};

/* 错误码 */
enum {
	KP_OK = 0,
	KP_E_PARSE = -1,      /* JSON 语法                  */
	KP_E_FIELD = -2,      /* 必填缺失（含 fallback 音色） */
	KP_E_PATH = -3,       /* 资产路径越界               */
	KP_E_RANGE = -4,      /* 数值越界                   */
	KP_E_IO = -5,
	KP_E_EXISTS = -6,
	KP_E_NOTFOUND = -7,
};

/*
 * 解析+校验 persona.json 文本 → kp_persona。
 * 返回 KP_OK 或负错误码；err_detail（可 NULL）给出字段名。
 */
int kp_parse(const char *json, size_t len, struct kp_persona *out,
	     const char **err_detail);

/* 注册表（目录下 index.json + 各包目录） */
int kp_registry_init(const char *registry_dir);
int kp_install(const struct kp_persona *p);       /* 按 id 去重 */
int kp_set_active(const char *id);
int kp_active(struct kp_persona *out);            /* 0=有激活   */
int kp_count(void);

#ifdef __cplusplus
}
#endif
