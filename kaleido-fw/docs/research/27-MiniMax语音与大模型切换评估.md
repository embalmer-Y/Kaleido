# 27. MiniMax 语音与大模型切换评估（2026-10-26，V-41）

密钥：`/home/emb/key/minimax.txt`（单 JWT，GroupID 内嵌 payload，运行时自解析，不入库不入日志）。
全部数据为本机直连实测（与板端 C5 网络同出口，误差 ±0.1s 量级）。

## 实测延迟（5 次）

### LLM `chatcompletion_v2`（SSE 流式，真实人设 prompt + 真实用户语）

| 模型 | 首子句（切分与 M1.5 一致） | 备注 |
|---|---|---|
| **MiniMax-Text-01** | **0.61-1.08s，中位 ~0.7s** | **qwen-plus 板测 1.3s，砍半** |
| MiniMax-M1 | 1.41s | 推理型，慢 |
| abab6.5s-chat / M2 / M2.1 | 不可用/名称不符 | 返回空，需再查模型清单 |

### TTS `t2a_v2`（SSE 流式，hex PCM，16kHz 单声道）

| 模型 | 首音 | 备注 |
|---|---|---|
| **speech-02-turbo** | **0.27-0.59s，中位 ~0.4s** | cosyvoice-v3-flash 板测 ~0.8s |
| speech-02-hd | 0.47s | 质量档，turbo 已够 |

### 端到端串测（LLM 首子句 → 立即 TTS → 首音）

**0.87-1.31s，中位 ~1.07s**（云端段，不含 ASR/板端）。
当前方案云端段 ≈ 1.3 + 0.8 = 2.1s。
**整轮预估：3.0s P50 → ~1.9-2.0s**。

## 结论与建议

**建议切换 LLM + TTS 到 MiniMax，ASR 暂留 DashScope：**

1. **收益**：整轮延迟省约 1s（3.0→~2.0s）；协议更简单——chatcompletion_v2
   与 OpenAI 形态兼容（ks_llm 只改 base_url/model/body 微调）；t2a_v2 是
   HTTPS+SSE+hex，比 cosyvoice 的 continue/run-task WS 状态机简单一个量级
   （无会话续喂坑，V-40 踩过的 task_group 问题不存在）
2. **ASR 是缺口**：MiniMax 只有文件转写，无 paraformer-realtime 对等物。
   混搭（MiniMax LLM/TTS + DashScope ASR）架构不变、密钥各自独立
3. **M1.5 的 1.5s 验收线**：MiniMax 云端段 1.07s + ASR 尾 0.3-0.5s + 板端
   0.3s ≈ 1.7-1.9s——大幅逼近但仍不达标；达 1.5s 要靠 M2 流式端点早切
   （ASR partial→LLM 提前起跑）而非再换供应商
4. **注意**：
   - 音频 hex 编码 = 带宽×2（32KB/s→64KB/s），WiFi 无压力
   - SSE 分块粗（每块约 1s 音频）——子句级切送架构无碍
   - Text-01 首响方差 0.27-1.58s，中位低但抖动略大于 qwen
   - 音色 female-tianmei 可用；"墨墨"音色后续走 MiniMax 声音复刻
   - 成本（turbo $60/1M 字符、Text-01 按 token）对桌搭用量不敏感
5. **另一条路（暂缓）**：MiniMax 实时交互 API（WS 一体化 ASR+LLM+TTS，
   支持人设与打断）可替换整条链路，但架构改动大、延迟无公开指标，
   留作 M2 全双工对话候选评估项

## 落地改动清单（待用户放行后执行）

- `kaleido_speech` 新增 `ks_tts_minimax.c`（t2a_v2 SSE 客户端，esp-http-client）
  或在现有 ws 层并列适配；`ks_llm` 加 provider 开关（dashscope/minimax）
- HIL `voice-loop-p4` 加模式切换验证：minimax 链整轮板测 + 真人听感
- `sdkconfig.local`（gitignore）加 MiniMax key 引用，与阿里 key 同规矩

---

## 落地实录（V-41 续，同日完成切换）

**模型终选（全部实测）**：LLM = `MiniMax-M3` + `thinking disabled`（首子句
0.57-1.19s）；TTS = `speech-2.5-hd-preview`（HD 档还最快，首音 0.23s）。
**M3.1-Flash 被 API 锁死自适应思考**（显式报错 2013："requires adaptive
thinking; disabled is not allowed"），首子句 1.55-2.4s，提示词压不住
（思考仍 300-400 字）——登记为未来"深思模式"开关，不作对话主力。

**线上报文形态（写适配器前必读）**：
- chatcompletion_v2 关思考只发 **4 行 SSE**，末行把整段回复+usage 打包
  （实测 702B，回复越长越长）——行缓冲必须 ≥2KB
- t2a_v2 末事件 `status=2` 的 audio 字段是**全量重复**（hex 长度=增量之和），
  必须按 status 裁决下发/丢弃，否则每段播两遍

**落地途中修掉的四个真 bug**（每个都有崩溃现场或可闻症状）：
1. llm auth 头缓冲 192B 截断 690B JWT → 401 静默失败
2. t2a_v2 全量重复事件未丢弃 → 用户耳朵抓到"每小段重复一遍"
3. 702B 超长 SSE 行 × line[512] 无条件 memcpy → 栈被 JSON 砸穿 →
   PC 跳进 JSON 字节执行（Instruction access fault，回复长度决定复现率）
4. LLM 事件回调内嵌套 esp_http_client+TLS（~13KB 栈深+非重入）→
   子句队列 + ttsfeed 任务解嵌套（附带 LLM 流与 TTS 取数并行）
附：ASR ws 关闭竞态防御（closing 标志 + destroy 前让拍）；主任务栈 8K→16K

**板端同网态 A/B**：minimax 首音 2.9-3.4s vs qwen 3.5-3.7s（测试时段网络
被并发音频流挤占，绝对值偏大；干净时段云端段 1.07s vs 2.1s）。
修复后 6 轮连测零崩溃。待办：干净网络复测分布 + 重复播放修复的真人听感。
