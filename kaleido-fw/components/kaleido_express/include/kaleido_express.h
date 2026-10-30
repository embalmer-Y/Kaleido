/*
 * kaleido_express.h — 表达基座 L0（V-45 / M1.6 骨架）
 *
 * 职责（本层只管"表达态"，不管怎么画）：
 *  - 持有唯一的表达状态（五态）；
 *  - 状态变化时回调渲染挂点（在 set 调用者上下文执行，须非阻塞）；
 *  - 转换遥测（计数+最近转换），供门禁与回归判分。
 *
 * 五态 = 灵魂架构方案 §9 三态矩阵的"无 key"态 + 会话四态
 * （idle 呼吸 / listening 倾听 / thinking 思考 / speaking 说话）。
 * EAF 23 词表映射、口型、动效分级（T0/T1/T2）属 M1.6 后续——落
 * kex_set_detail() 扩展位，L0 不实现。
 *
 * 线程契约：set 可来自任意任务（会话 FSM / OOBE）；回调须自备同步
 * （HIL 桥在 LVGL 定时器里轮询 set，天然串行）。
 */
#ifndef KALEIDO_EXPRESS_H
#define KALEIDO_EXPRESS_H

enum kex_state {
	KEX_UNCONFIGURED = 0,	/* 无 key 引导态（OOBE §9 第一列） */
	KEX_IDLE,		/* 呼吸待机 */
	KEX_LISTENING,		/* 倾听（唤醒后采音） */
	KEX_THINKING,		/* 思考（LLM/云端往返） */
	KEX_SPEAKING,		/* 说话（TTS 播放） */
};

/* 渲染挂点：状态变化时回调（user 为 kex_init 注册值） */
typedef void (*kex_state_cb)(void *user, enum kex_state st);

int kex_init(kex_state_cb cb, void *user);
/* 设状态（幂等；变化才回调+日志）。未 init 时只记状态 */
void kex_set_state(enum kex_state st);
enum kex_state kex_get_state(void);
const char *kex_state_name(enum kex_state st);
/* 累计转换次数（回归判分用） */
int kex_transition_count(void);

#endif /* KALEIDO_EXPRESS_H */
