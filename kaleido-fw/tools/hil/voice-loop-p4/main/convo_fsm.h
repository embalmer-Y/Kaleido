/*
 * convo_fsm.h — 免按键会话状态机（V-43，M2 语音主体）
 *
 * 状态：OFF（未启）→ IDLE（wakenet 持麦监听）→ LISTENING（唤醒后
 * 采音，VAD-lite 提前截断）→ THINKING（ASR 定稿→LLM）→ SPEAKING
 * （TTS 播放）→ 回 IDLE。
 *
 * 音频所有权（评审 2026-10-26 §7 前置项）：
 *  - 采音单一所有者：wakenet 仅在 IDLE 喂麦（feed_task 查本状态机），
 *    LISTENING 起让渡给轮采集——AEC 落地（V-46）前，自唤醒免疫
 *    由这个让渡结构性获得（播放期根本无人听麦）。
 *  - 放音单一通路：全部声音（beep/TTS）经 s_pb 播放流。
 */
#ifndef VOICE_LOOP_P4_CONVO_FSM_H
#define VOICE_LOOP_P4_CONVO_FSM_H

#include <stdbool.h>

#include "kaleido_speech.h"

enum convo_state {
	CONVO_OFF = 0,
	CONVO_IDLE,
	CONVO_LISTENING,
	CONVO_THINKING,
	CONVO_SPEAKING,
};

enum convo_state convo_state_get(void);
bool convo_active(void);
int convo_round_count(void);

/* 启动（幂等）：创建常驻 FSM 任务并进入 IDLE。false=参数/资源失败 */
bool convo_start(struct ks_asr_cfg *asr, struct ks_tts_cfg *tts,
		 struct ks_llm_cfg *llm);
void convo_stop(void);

/* wakenet 检出唤醒词时的通知入口（detect_task 调，非阻塞） */
void convo_notify_wake(void);

/* 自驱回归（门禁判据）：TTS 过空气喊唤醒词 + 提问，N 轮无人值守。
 * 需先 convo_start。返回已启动的轮数 */
int convo_soak_start(int rounds);

#endif /* VOICE_LOOP_P4_CONVO_FSM_H */
