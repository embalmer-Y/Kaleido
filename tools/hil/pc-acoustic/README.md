# PC 声学验证链（无人值守语音测试）

替代人工语音配合：PC 合成语音 → PC 扬声器外放 → 板载麦克风，
实现对话轮/唤醒的自动化测试。2026-10-30 建链并定案能力边界。

## 文件

- `pc_voice.py` — MiniMax t2a_v2 合成 WAV（音色/音量/语速可调；
  密钥读 `/home/emb/key/minimax.txt`，绝不入库）
- `sample/wake_slow.wav` — 唤醒词（tianmei 0.75x 慢速=命中率最高变体）
- `sample/question.wav` — 标准测试问题（对话轮用）

## 用法

```bash
# 合成
python3 pc_voice.py "你好小智" /tmp/wake.wav [vol0-100]
# 对话轮（PC 扮演用户）：板上发 'g'，6s 后播放问题
# 播放务必强制模拟口（默认路由可能进 HDMI/耳机）：
aplay -q -D plughw:0,0 /tmp/question.wav
```

## 能力边界（实测定案）

| 能力 | 状态 |
|---|---|
| 对话轮全链（ASR 转写/LLM/TTS） | ✅ 可靠——ASR 一字不差 |
| 电平标定（Master% → 板子收声 dB） | ✅ 45%≈-24dB / 98%≈-16dB（@增益42） |
| 唤醒触发 | ⚠️ 机器音色边际（模型特性）：快语速 0/8，慢速变体 ~1/5；金标准仍是真人抽测 |

## 坑位记录

1. `aplay` 默认设备可能不是外放（本机曾静默路由错误）——必须
   `-D plughw:0,0`；
2. 板载增益 42 下 >-10dB 的近场源会削顶，模型对失真语音全灭——
   PC 音量保持 45-65%；
3. 机器音色筛选结论：语速慢 > 音色换（male/shaonv/yujie 与默认差异小）；
4. DashScope cosyvoice REST 路径已失效（400 url error），别再试。
