# PC 声学验证链（无人值守语音测试）

替代人工语音配合：PC 合成语音 → PC 扬声器外放 → 板载麦克风，
实现对话轮/唤醒的自动化测试。2026-10-30 建链并定案能力边界。

## 文件

- `pc_voice.py` — MiniMax t2a_v2 合成 WAV（音色/音量/语速可调；
  密钥读 `/home/emb/key/minimax.txt`，绝不入库）
- `sample/wake_auto.wav` — **标准自动唤醒音频**（female-shaonv @0.7x，
  speech-2.5-hd）：10 连确认 **8/10 命中**（未中 2 次系前轮排干时序，
  非音频缺陷）；未屏蔽首播命中率 ~100%
- `sample/wake_slow.wav` — 备选（tianmei 0.75x，命中率略低）
- `sample/question.wav` — 标准测试问题（对话轮用）
- `wake_test.sh` — 自动唤醒测试（播放侧；串口抓取另配）

## 用法

```bash
# 自动唤醒测试（板子 READY 后；Master 55%）：
tools/hil/pc-acoustic/wake_test.sh 5        # 播 5 次，28s 间隔
# 归因统计（对串口抓取日志）：
#   grep -a "WN,WAKE\|PLAY" capture.log 的相邻行即命中归属
# 对话轮（PC 扮演用户）：板上发 'g'，6s 后播放问题
aplay -q -D plughw:0,0 /tmp/question.wav
```

**唤醒播放协议**：命中会触发真实对话轮（LISTENING ~12s + 收尾），
轮内命中被防重触发丢弃——**播放间隔 ≥28s**（或从日志确认回到 IDLE），
否则统计被轮屏蔽污染（首筛矩阵 33% 假象的来源）。

## 能力边界（实测定案）

| 能力 | 状态 |
|---|---|
| 对话轮全链（ASR 转写/LLM/TTS） | ✅ 可靠——ASR 一字不差 |
| 电平标定（Master% → 板子收声 dB） | ✅ 45%≈-24dB / 98%≈-16dB（@增益42） |
| 唤醒触发（wake_auto.wav @28s 间隔） | ✅ **8/10**，可作无人值守回归 oracle |

## 坑位记录

1. `aplay` 默认设备可能不是外放（本机曾静默路由错误）——必须
   `-D plughw:0,0`；
2. 板载增益 42 下 >-10dB 的近场源会削顶，模型对失真语音全灭——
   PC 音量保持 45-65%；
3. 机器音色筛选结论：语速慢 > 音色换（male/shaonv/yujie 与默认差异小）；
4. DashScope cosyvoice REST 路径已失效（400 url error），别再试。
