# 19 云端 ASR/TTS 与音色定制调研

| 版本 | 日期 | 作者 | 触发 |
|---|---|---|---|
| v1.0 | 2026-10-25 | ZCode（调研子代理） | 设备语音链路选型：离线唤醒(已验证 WakeNet) → 录音 → 云端流式 ASR → LLM(已有, OpenAI 兼容 BYOK) → 云端流式 TTS(需指定/定制音色) → I2S 播放；产品要求角色音色可定制 |

**判定一句话（详见 §5）：首推"阿里百炼一站式 BYOK"起步（Paraformer/fun-asr 实时 WS + CosyVoice/Qwen-TTS 双向流式 WS + 免费零样本复刻，Bearer key 直用零签名），架构上预留"自托管语音网关 URL"覆盖位（FunASR + CosyVoice3，克隆完全自由、零边际成本）；火山豆包作为音质备选第二云。**

证据分级：A=官方文档/项目内实测；B=权威第三方/社区有据报告；C=推断或估算（给区间+假设）。
外部来源均标注 URL + 访问日期（本文所有外部访问日期 = **2026-10-25**）。项目内路径为本地事实。

---

## 1 需求与约束

### 1.1 链路与硬约束（项目内事实，A级）

| 约束 | 内容 | 来源 |
|---|---|---|
| 主控 | ESP32-S31，16MB PSRAM，FreeRTOS C 固件，mbedTLS | docs/research/23-S31资源预算与感官管线调研.md |
| TLS | 已实测部分国内云 NLB 只接受 TLS1.3/TLS1.3 优先 ClientHello；已开 `CONFIG_MBEDTLS_SSL_PROTO_TLS1_3=y` 解决一例（W-4），真机 HTTPS LLM 链路已通 | docs/gates/M1-真机验证记录.md §7 |
| 唤醒 | WakeNet 离线唤醒已闭环（wn10 int16），唤醒链峰值 ~1.3MB | docs/gates/V-04唤醒实测结果.md |
| LLM | 已有 OpenAI 兼容协议 BYOK（用户自填 key） | 项目既有设计 |
| 播放 | I2S 输出；音频组件已在 vendor：`espressif__esp_audio_codec` v2.5.0（含 `esp_opus_dec.h`）、`espressif__esp_websocket_client` v1.8.0、esp_player/media_lib_sal/gmf 全套 | /home/emb/project/Kaleido/vendor/esp-claw/application/edge_agent/managed_components/（本地核实） |
| esp-claw | 乐鑫 agent 框架**无任何语音输入能力**，ASR/TTS 需完全自研集成 | 项目既有结论（README/源码无 ASR/TTS 模块） |
| 用户 | 中国大陆为主，BYOK=用户自填 API key；自托管 NAS 也是一种 BYOK | 需求输入 |
| 目标 | 对话首响应（唤醒/说话结束 → 首音出声）**< 1.5s** | 需求输入 |

### 1.2 选型关键问题

1. **鉴权复杂度**：API key 直用（Bearer header / URL 参数）≈ 0 成本；HMAC 签名（讯飞/腾讯）在 ESP32 上可实现（mbedTLS 有 HMAC-SHA1/256）但字典序排序+URL 编码+Date 头易踩坑；JWT 签发（MiniMax）需设备端 HS256，中等复杂度。
2. **音频格式**：TTS 若能直出 **PCM 16k/24k 单声道**，ESP32 可**零解码**直推 I2S；opus/mp3 则需解码器（vendor 已有，见 §4）。
3. **音色定制**：能否（a）用户 10~20 秒录音自助复刻，（b）复刻创建费用，（c）复刻音色的合成单价。
4. **TLS**：各云 wss 端点是否接受 TLS1.2 的 ClientHello（我们已开 TLS1.3，风险收敛但需逐家实测）。
5. **自托管**：能否用自家 NAS/服务器绕开云协议/签名复杂性，ESP32 只连一个自定协议网关。

---

## 2 厂商矩阵

### 2.1 阿里云百炼（DashScope）——首推候选

**TTS（CosyVoice / Qwen-TTS-Realtime）**
- 双向流式 WebSocket：新域名 `wss://{WorkspaceId}.cn-beijing.maas.aliyuncs.com/api-ws/v1/inference`（旧 dashscope.aliyuncs.com 迁移中）。协议：`run-task` → `task-started` → 多个 `continue-task`（流式喂 LLM 文本）→ 服务端**自动分句**、整句立即合成、二进制帧返回音频 → `finish-task`。同一 task_id 贯穿。[A] https://help.aliyun.com/zh/model-studio/cosyvoice-websocket-api （2026-10-25）
- **鉴权：`Authorization: Bearer <API Key>`，握手期校验，无需任何签名/临时 token**——ESP32 最友好。[A] 同上
- 输出格式：示例 mp3/22050Hz；参数化 format/sample_rate/volume/rate/pitch（pcm 等格式未在该页示例出现，接入前以参数表实测为准）[A/C] 同上
- 模型：cosyvoice-v3-flash（**1 元/万字符**）、cosyvoice-v3.5-plus（1.5 元/万）、cosyvoice-clone-v1（2 元/万）；新线 qwen-tts-realtime / qwen-audio-3.1-tts-flash（低延迟定位）；2026-09 有语音系列降价通知。[B] https://help.aliyun.com/zh/model-studio/model-pricing 、https://www.aliyun.com/notice/118685 （2026-10-25）
- 延迟：CosyVoice2 官方流式 150ms 首包（开源模型口径）；qwen-tts-realtime 面向实时交互。[A/B] https://www.siliconflow.com/zh/models/funaudiollm-cosyvoice2-0-5b 、https://help.aliyun.com/zh/model-studio/interactive-process-of-qwen-tts-realtime-synthesis （2026-10-25）
- Realtime API 概述页还包含 fun-asr-realtime（开源 FunASR 的托管版）与 qwen-audio-3.1-realtime-plus 语音对话。[A] https://help.aliyun.com/zh/model-studio/realtime-api-overview （2026-10-25）

**ASR（Paraformer 实时 / fun-asr-realtime）**
- WebSocket 接入（接口地址+请求头+交互流程文档齐全）；paraformer-realtime-v2 支持任意采样率（iOS SDK 文档口径）。[A] https://help.aliyun.com/zh/model-studio/websocket-for-paraformer-real-time-service （2026-10-25）；[B] 采样率说明
- 鉴权：长期 API Key 直调（推荐）或 60s 临时 token（安全场景）。[A] 同上
- 价格：Paraformer 录音文件版 0.288 元/小时；实时版按秒计（第三方口径 ~0.00022 元/秒 ≈ 0.79 元/小时）；旧智能语音交互线 3.5 元/小时起。**接入前以价格页实测为准**。[B] https://help.aliyun.com/zh/model-studio/model-pricing （2026-10-25）

**音色复刻（voice-enrollment）**
- 流程：纯 API（`create_voice` 上传参考音频 + target_model + 音色名前缀 → 轮询 `query_voice` 至 OK），无控制台入口。[A] https://help.aliyun.com/zh/model-studio/voice-cloning-user-guide （2026-10-25）
- 录音要求：10~20s（≤60s，≤10MB），wav(16bit)/mp3/m4a，≥16kHz；创建**免费**；每账号上限 1000 个音色；复刻音色绑定 target_model（cosyvoice v2/v3-plus/v3-flash/v3.5 系），合成按目标模型计费（即 1~2 元/万字符）。[A] 同上；HTTP API 参考 https://help.aliyun.com/zh/model-studio/voice-clone-design-http-api
- ESP32/SDK：无官方 ESP32 SDK；官方多语言示例仓库 alibabacloud-bailian-speech-demo（WS 协议为 JSON 文本帧 + 二进制音频帧，纯 C 可实现）。[A] https://github.com/aliyun/alibabacloud-bailian-speech-demo
- TLS 版本要求：官方未公开声明，属标准 CDN 端点；结合本项目 W-4 经验，保持 TLS1.3 开启并首轮实测（C）。

**小结**：鉴权最简（Bearer 直用）+ 双向流式自动分句 + 复刻免费且 10-20s 样本 + TTS 1~1.5 元/万字符国内最低档 → BYOK 心智最顺。

### 2.2 火山引擎豆包（Seed-ASR / Seed-TTS）——音质备选

**TTS（双向流式 WebSocket）**
- `wss://openspeech.bytedance.com/api/v3/tts/bidirection`；事件式协议（StartConnection/StartSession/TaskRequest 文本流式输入 → 二进制音频帧）；另有单向流式 V3（WS）与 **HTTP chunked** 单向流式版（一次性文本、流式音频）。[A] https://www.volcengine.com/docs/DoubaoVoice/bidirectional-streaming-text-to-speech-websocket 、https://www.volcengine.com/docs/DoubaoVoice/unidirectional-streaming-text-to-speech-http （2026-10-25，正文经 web_reader 提取）
- **鉴权：请求头 `X-Api-Key` + `X-Api-Resource-Id`（seed-tts-2.0；复刻音色走 seed-icl-2.0）**——同为 key 直用，无签名。[A] 同上
- 音频：mp3(默认)/pcm/ogg_opus(仅 48k)/wav；采样率 8000-48000；voice_type 指定音色；支持 context/指令控制情感方言。[A] 同上
- 延迟：官方宣传端到端方案首包 ~100ms 级。[B] https://www.volcengine.com/product/tts
- 价格：合成 2.0 字数包 **2.8 元/万字符**（后付费 5 元/万）；流式 ASR 4.5 元/小时；声音复刻推理 8 元/万字符。[A] https://docs.volcengine.com/docs/DoubaoVoice/Billinginstructions-21 （2026-10-25）

**ASR（大模型流式识别）**
- WebSocket；鉴权 X-Api-App-Key + X-Api-Access-Key（控制台 token，另有双 token/签名可选）；音频 PCM 16k/16bit/单声道常见要求。[A/B] https://www.volcengine.com/docs/DoubaoVoice/LargemodelstreamingautomaticspeechrecognitionAPI 、https://www.volcengine.com/docs/DoubaoVoice/Authenticationmethod-1 （2026-10-25）

**声音复刻**
- 大模型复刻 2.0：开放环境 5~10s 录音、秒级复刻，体验最好；但**音色购买约 7000 元/个（企业级，含 10 万字）**，免费试用仅 1 音色+10 次训练+5000 字符 → **个人 BYOK 用户复刻成本极高**。[B] https://www.volcengine.com/product/voicecloning 、https://www.volcengine.com/docs/DoubaoVoice/Soundreplicationorderingandusageguide （2026-10-25）

**小结**：音质/延迟标杆（Seed-TTS 2.0 + ogg_opus），key 直用；但 ASR 单价（4.5 元/h）与复刻门槛（7000 元/音色）对个人 BYOK 不友好。作第二云/高质量角色音预置位。

### 2.3 腾讯云——签名复杂度劝退

- 实时 ASR WS：`wss://asr.cloud.tencent.com/asr/v2/<appid>?{参数}&signature=`；**鉴权=HMAC-SHA1+Base64+URL 编码**（SecretID/SecretKey/APPID，参数字典序拼接）；音频 16k/8k、16bit、单声道，pcm/wav/opus/speex/silk/mp3/m4a/aac；要求 1:1 实时率发送（每 200ms 发 200ms）。[A] https://cloud.tencent.com/document/product/1093/48982 （2026-10-25）
- 流式 TTS WS 协议同样带 URL 签名算法；HTTP 接口则用 TC3-HMAC-SHA256（更重）。[A] https://cloud.tencent.com/document/product/862/133241
- 价格：大模型 TTS 实时对话场景 ~3 元/万字符；TTS 免费额度 800 万字符/3 个月（通用接口）。[B] https://cloud.tencent.com/document/product/1073/34112 、https://cloud.tencent.com/document/product/647/115755
- 声音复刻：需企业实名认证流程，个人 BYOK 不友好。[B] 产品文档口径
- ESP32：HMAC-SHA1 mbedTLS 可做，但排序/URL 编码/时间窗逻辑易错，且 ASR 强制 1:1 实时率对固件节拍有约束。[C] 集成复杂度评级：中高。

### 2.4 科大讯飞——签名+套餐制，社区 ESP32 案例多

- 流式听写 WS：鉴权 = hmac-sha256 签名（base64 的 Authorization 头 + Date/Host/request-line）；音频 16k/16bit/mono pcm 或 speex，~40ms 分片。[A] https://www.xfyun.cn/doc/asr/voicedictation/API.html （2026-10-25）
- 在线合成/超拟人 TTS WS：同套签名鉴权；超拟人推荐 raw(pcm)/lame(mp3)、24kHz。[A] https://www.xfyun.cn/doc/tts/online_tts/API.html 、https://www.xfyun.cn/doc/spark/super%20smart-tts.html
- 每日 500 次免费额度（TTS），超出买套餐。[B] 同上
- 声音复刻：一句话复刻需企业资质+朗读指定文本，个人自助差。[B]
- ESP32 社区验证案例（星火听写 hmac-sha256 移植）：[B] https://www.cnblogs.com/FBsharl/p/19170481
- 评价：老牌稳定、方言强；签名鉴权+套餐制+复刻门槛，综合不如百炼。

### 2.5 MiniMax（speech-02/2.6/2.8）——克隆性价比高但鉴权要 JWT

- T2A v2 WebSocket 逐句流式（文本分句进、音频流出），单次 ≤10000 字符；海外平台另有 api-uw 域名降低 TTFA。[A] https://platform.minimax.io/docs/guides/speech-t2a-websocket 、https://platform.minimax.io/docs/api-reference/speech-t2a-websocket （2026-10-25）
- 国内平台 platform.minimaxi.com（api.minimax.chat）；**鉴权=API Key 换 JWT（HS256）+ group_id**——设备端需实现 JWT 签发或经中转。[B] https://platform.minimaxi.com/document/T2A%20V2?key=66719005a427f0c8a5701643
- 价格（国内第三方整理）：speech-02/2.6 turbo **2 元/万字符**、hd 3.5 元/万；**复刻音色创建 50 元/个**，10s 音频宣称 99% 相似。[B] https://github.com/hkai-ai/LLM_OFFICIAL_DOCUMENTATION/blob/main/minimax/pricing.md 、https://zhuanlan.zhihu.com/p/21588943560
- 有趣事实：**阿里百炼也代售 MiniMax 语音模型**（复刻音频仅公网 URL 上传）。[A] https://help.aliyun.com/zh/model-studio/voice-cloning-user-guide
- ASR：无主打实时 ASR API（Realtime 交互 API 面向企业）→ 需搭配他家 ASR。[C]
- 评价：克隆音质口碑好、50 元/音色可接受；JWT 鉴权与无 ASR 是两个减分项。

### 2.6 智谱（GLM-TTS / GLM-ASR）——协议最简单，与 GLM LLM 同 key 生态

- GLM-TTS：HTTP POST `/api/paas/v4/audio/speech`，`stream:true` 走 SSE 返回 **base64 PCM 分片（24kHz）**，wav/pcm 可选；`Authorization: Bearer API Key`；**官方标注流式首帧 <400ms**；价格 2 元/万字符（定价页口径）。[A] https://docs.bigmodel.cn/cn/guide/models/sound-and-video/glm-tts 、[B] https://docs.bigmodel.cn/cn/guide/start/pricing （2026-10-25）
- GLM-TTS-Clone（音色克隆）6 元/次——计费语义（按克隆创建 or 按克隆音色合成调用）需二轮确认。[B] 同上
- GLM-ASR：HTTP 转写（`client.audio.transcriptions.create`，wav；`stream:true` 增量出文本 delta），**非音频流入 WS**——适合"VAD 判停后整段上传"链路；0.06 元/分钟（=3.6 元/小时），中英+8 方言；并发限制 V0-V3 为 5-20 在途请求（免费档并发低）。[A] https://docs.bigmodel.cn/cn/guide/models/sound-and-video/glm-asr 、[B] https://www.datalearner.com/blog/zhipu-glm-asr-nano-speech-recognition
- GLM-Realtime（实时音视频）已上线，未来可切换。[A] https://docs.bigmodel.cn/cn/guide/models/sound-and-video/glm-realtime
- 评价：HTTP+SSE、pcm 直出、无签名，是**集成工作量最小**的一家；短板是无流式 ASR（长句延迟吃亏）、音色数少、克隆能力较新。

### 2.7 SiliconFlow（硅基流动）——OpenAI 风格聚合，国内直连

- REST + `Authorization: Bearer <API_KEY>`；TTS（FunAudioLLM/CosyVoice2-0.5B 等）**支持实时流式音频输出**，response_format 可选 mp3/wav/pcm。[A] https://api-docs.siliconflow.cn/docs/userguide/capabilities/text-to-speech （2026-10-25）
- 价格：CosyVoice2 加速版 **¥105/百万 UTF-8 字节**（中文 3 字节/字 ≈ **3.15 元/万汉字**）。[B] https://zhuanlan.zhihu.com/p/15714942389 、https://www.siliconflow.cn/pricing
- ASR（SenseVoice-Small / Fun-ASR）HTTP 转写、按分钟计费——**非实时流式**，与 GLM-ASR 同属"整段上传"型。[A/B] 同上
- 无复刻音色管理（CosyVoice2 本身支持参考音频 zero-shot，平台侧封装待查）。[C]
- 评价：key 直用+OpenAI 生态心智；适合当 TTS 备选与整段转写备选，撑不起完整实时链路。

### 2.8 fish-audio——便宜+克隆自由，海外端点

- TTS Live WebSocket 流式（`/v1/tts/live`）+ REST；按 UTF-8 字节计费 ~$15/MB（≈3.2 元/万汉字@7.1 汇率）；S1 将于 2026-12-31 下线，新集成直接用 S2/S2.1。[A] https://docs.fish.audio/api-reference/endpoint/websocket/tts-live 、https://docs.fish.audio/developer-guide/models-pricing/pricing-and-rate-limits 、[B] https://saascrmreview.com/fish-audio-review
- 克隆：10~15s 音频、13 语言，工作台自助，社区音色库大。[B]
- 开源 fish-speech 可自托管（消费级 GPU 分钟级复刻）。[A] https://github.com/fishaudio/fish-speech
- 大陆直连 docs.fish.audio / api 端点不稳定，需代理（C）→ 海外备选。

### 2.9 OpenAI——次要（国内需代理）

- TTS：gpt-4o-mini-tts，HTTP chunked 流式输出（pcm 24k 等），~$0.60/百万文本 token（≈$0.015/分钟语音）；支持 voice instructions 风格控制，**无用户级音色克隆**。[B] https://platform.openai.com/docs/pricing
- ASR：gpt-4o-transcribe / whisper，HTTP（支持流式文本出），$0.003~0.006/分钟。[B] 同上
- Realtime API：WS + WebRTC，需后端换 **ephemeral token**；官方 Cookbook 有 **ESP32-S3 + Secure WebSocket 实时语音完整案例（ElatoAI）**——ESP32 端可行性最硬的海外证据。[A] https://developers.openai.com/api/docs/guides/voice-webrtc 、https://developers.openai.com/cookbook/examples/voice_solutions/running_realtime_api_speech_on_esp32_arduino_edge_runtime_elatoai
- 大陆访问需代理 → 与"中国用户为主"冲突，仅作海外用户 BYOK 选项。

### 2.10 ElevenLabs——海外音质标杆

- Flash v2.5 TTFA ~75ms、~$40/百万字符；REST `/stream` + WebSocket stream-input；Instant Voice Cloning 需 $22/月 Creator 档起。[A/B] https://elevenlabs.io/docs/overview/models 、https://elevenlabs.io/pricing
- 大陆访问受限（C）。作海外高端选项。

### 2.11 Azure Speech（含中国区）

- Neural TTS ~$15-16/百万字符，SDK WS 流式；**语音克隆（Personal Voice / Custom Neural Voice）中国区（世纪互联）不可用**，需全球订阅+合规申请 → 国内 BYOK 场景基本出局。[A] https://learn.microsoft.com/en-us/answers/questions/5544816/ 、https://azure.microsoft.com/en-us/pricing/details/speech

### 2.12 对比总表

| 厂商 | 流式协议 | TTS 音频格式 | 鉴权（ESP32 复杂度） | 首包延迟 | ASR | 定价量级 | 音色定制 | ESP32/社区验证 |
|---|---|---|---|---|---|---|---|---|
| **阿里百炼** | TTS 双向 WS；ASR 实时 WS | mp3/22050 示例（pcm 待实测） | **Bearer key 直用（最简）** | CosyVoice 150ms 级 | Paraformer/fun-asr-rt WS | TTS 1~2 元/万；ASR ~0.8 元/h | **API 复刻免费，10-20s 样本，1000 音色** | 无官方 SDK；协议简单可 C 实现；官方 demo 仓库 |
| 火山豆包 | TTS 双向 WS + HTTP chunked；ASR WS | **pcm/mp3/ogg_opus/wav 8-48k** | X-Api-Key 直用（简） | 官传 ~100ms 级 | Seed-ASR WS 4.5 元/h | TTS 2.8 元/万(包) | 复刻体验最好但 **7000 元/音色**（企业） | 火山开发者社区有小智方案文章 |
| 腾讯云 | ASR WS + TTS WS | pcm/mp3 等 | **HMAC-SHA1 URL 签名（中高）** | 未公开 | 实时 WS 强 | TTS ~3 元/万 | 复刻需企业认证 | 签名移植易错 |
| 科大讯飞 | WS（听写/合成） | pcm/mp3/speex，24k | **hmac-sha256 签名头（中）** | 中 | 强（方言） | 套餐制+每日免费 500 次 | 一句话复刻需企业 | **ESP32 社区案例多** |
| MiniMax | T2A v2 WS 逐句 | mp3/pcm 等 | **API Key→JWT(HS256)（中）** | turbo 低延迟档 | 无主打实时 ASR | turbo 2 元/万 | **50 元/音色，10s 克隆** | 海外 TTFA 榜单中上 |
| 智谱 | TTS HTTP SSE 流式；ASR HTTP 整段 | **pcm 24k 直出** | **Bearer key（最简）** | 官方 <400ms 首帧 | 非流式（整段 0.06 元/min） | TTS 2 元/万 | GLM-TTS-Clone 6 元/次（语义待确认） | 与 GLM LLM 同 key |
| SiliconFlow | REST 流式响应 | mp3/wav/pcm | **Bearer key（最简）** | 中 | HTTP 非实时 | TTS ≈3.15 元/万汉字 | 无平台级复刻 | OpenAI 风格 |
| fish-audio | WS TTS Live | 多格式 | API key（简） | 中 | 无 | ≈3.2 元/万汉字 | 10-15s 自助克隆 | 海外端点需代理 |
| OpenAI | HTTP chunked / Realtime WS+WebRTC | pcm 24k | Bearer；Realtime 需 ephemeral token | Realtime 极低 | Transcribe 流式文本 | TTS ~$0.015/min | 无用户克隆 | **官方 ESP32-S3 Cookbook 案例** |
| ElevenLabs | REST/WS 流式 | mp3/pcm | xi-api-key（简） | **~75ms** | 无 | $40/M chars | IVC 需 $22/月 | 海外端点 |
| Azure | SDK WS | 多格式 | region+key | 低 | 强 | $15-16/M chars | **中国区无克隆** | 企业向 |

---

## 3 自托管方案（NAS/自有服务器 BYOK）

### 3.1 组件选型

| 组件 | 角色 | 关键事实 | 证据 |
|---|---|---|---|
| **FunASR runtime（funasr-runtime-sdk）** | 流式 ASR 服务 | 官方 Docker 镜像（GPU/CPU 版），WebSocket 端口 **10095**，2pass 流式（流式出字+尾句修正）+ VAD + 标点，客户端含 C++/Python/Java/Web 示例 | [A] https://github.com/modelscope/FunASR/blob/main/runtime/quick_start.md |
| **CosyVoice 3（开源）** | 流式 TTS + 零样本克隆 | 3 秒音频零样本克隆；官方称首包较 v2 再降 50%、支持双向流式；模型 Fun-CosyVoice3-0.5B-2512 | [A] https://github.com/FunAudioLLM/CosyVoice 、https://huggingface.co/FunAudioLLM/Fun-CosyVoice3-0.5B-2512 |
| | 实测延迟 | v2 zeroshot 社区实测 RTT 0.46s、端侧首包 <1s；vLLM 加速版（async_cosyvoice）首包 150-250ms（4070 级卡）；社区一键包 6GB 显存可跑 | [C] https://github.com/QwenAudio/CosyVoice/issues/755 、https://github.com/qi-hua/async_cosyvoice |
| **GPT-SoVITS** | 克隆 TTS（备选） | 5s 零样本/1min 微调；v2ProPlus RTF 0.028@4060Ti、0.014@4090、**M4 CPU 0.526（近实时）**；官方 api_v2 流式在标点切分下**首包 3-4s**（对话场景偏慢，issue 已知） | [A] https://github.com/RVC-Boss/GPT-SoVITS 、https://github.com/RVC-Boss/GPT-SoVITS/issues/1523 |
| **sherpa-onnx** | 轻量 ASR/TTS（CPU/NAS 友好） | 官方自带 `sherpa-onnx-online-websocket-server`（zipformer 流式）与 TTS websocket server，有 Dockerfile；纯 CPU 可跑 | [A] https://github.com/k2-fsa/sherpa-onnx 、https://k2-fsa.github.io/sherpa/onnx/ |
| **小智生态（参考架构）** | ESP32↔服务器协议先例 | xiaozhi-esp32：ESP32-S3 端 **WebSocket + Opus 二进制帧 + JSON 控制消息**；服务端 xiaozhi-esp32-server（Python/Go 社区版+商业版）对接豆包等；火山开发者社区有官方文章 | [A/B] https://github.com/78/xiaozhi-esp32 、https://xujiwei.com/blog/2025/04/ai-xiaozhi-esp32-protocol 、https://developer.volcengine.com/articles/7540133843739934774 |

### 3.2 自托管能否绕开云协议复杂性？——能

- ESP32 只连**自家一个 WebSocket 网关**（自定极简协议：JSON 控制帧 + 二进制 PCM/Opus 帧双向），网关再做协议适配（云 or 本地模型）。签名/JWT/各家私有协议全部收敛到服务器侧 Python——设备固件零特判。
- TLS：网关用 Let's Encrypt 域名证书（或局域网 ws:// 明文+本地鉴权），彻底规避国内云 NLB 的 TLS1.3 兼容问题与自签证书分发问题（自签则需 App 配网页上传 CA PEM，见 §5 组合 B）。
- BYOK 配置姿势：用户在 App"语音服务"页选择"自托管"，填 `wss://home.example.com:10095`（ASR）与 `wss://home.example.com:9100/tts`（TTS）+ 可选 token；设备侧与"云 BYOK 填 key"走同一抽象层。

### 3.3 自托管体验上限（C 级估算）

| 用户硬件 | ASR（FunASR 2pass） | TTS（CosyVoice3） | 端到端可行性 |
|---|---|---|---|
| 纯 CPU NAS（无 GPU，J4125/N100 级） | RTF<1 可流式（官方 CPU 镜像） | 首包 1-3s 或更差（v3 无 GPU 不现实，退 GPT-SoVITS CPU RTF~0.5 → 首包数秒） | 能用但难达 1.5s 目标 |
| 入门 GPU（6-8GB，3060/4060 级） | 富余 | 首包 300ms-1s（社区口径） | 可达 1.5-2s |
| 中端 GPU（4070/4090 级） | 富余 | 首包 150-300ms（async/vLLM 加速） | **可达 <1.5s** |

---

## 4 ESP32 端集成评估（解码/内存/延迟）

### 4.1 组件现状（项目内事实，A级）

- vendor 已含 **esp_audio_codec v2.5.0**（`include/decoder/impl/esp_opus_dec.h` 证实含 Opus 解码器；组件支持 AAC/MP3/AMR/VORBIS/OPUS 等，Dual-Level API 适合流式输入）、**esp_websocket_client v1.8.0**（支持自定义 header + wss）、esp_player/media_lib_sal 全套。路径：/home/emb/project/Kaleido/vendor/esp-claw/application/edge_agent/managed_components/。
- esp_audio_codec 上游 v2.6.0 changelog：ESP32-S3 的 Opus 编解码新增汇编优化（S31 同为 Xtensa LX7 系，预计受益，需实测）。[A] https://components.espressif.com/components/espressif/esp_audio_codec/versions/2.6.0/changelog
- libopus 参考内存：每并发实例 ~120KB（esp-libopus 口径，大缓冲放 PSRAM）；esp_audio_codec 为轻量实现。小智生态在 ESP32-S3 上量产验证了"libopus 编码 16k 单声道 60ms 帧 + WS"路线。 [B] https://github.com/kahrendt/esp-libopus 、https://github.com/78/xiaozhi-esp32

### 4.2 音频格式策略（关键结论）

1. **首选：TTS 直出 PCM 16k/24k 单声道 → 零解码直推 I2S**。智谱（pcm 24k）、火山（pcm 8-48k）、SiliconFlow（pcm）均支持；百炼 CosyVoice 需实测 pcm 参数。16k mono PCM = 32KB/s，WiFi 无压力；省下解码 RAM/CPU，喂给 Lottie/显示器。这正是选"支持 pcm 输出"厂商的第一理由。
2. 次选 opus（火山 ogg_opus 48k、小智生态路线）：省 10 倍带宽，但 TTS 场景带宽不敏感，反而引入 48k 采样 + ogg 容器解析 + ~100KB 级解码内存——不划算，仅当链路走蜂窝/远程自托管时考虑。
3. mp3（百炼默认）：esp_audio_codec 支持，帧边界切片比 pcm 麻烦（需处理帧头对齐），RAM 小；作为百炼 pcm 不可用时的兜底。
4. ASR 上行：各家几乎都收 **PCM 16k/16bit/mono**（火山/腾讯/讯飞明示），设备端免编码直传（32KB/s）；若走自托管网关且广域网，可换 opus 编码（vendor 已有 esp_opus_enc）。

### 4.3 流式播放与缓冲策略

- 环形缓冲：≥ 4-6 个 100ms chunk（16k mono = 3.2KB/chunk，共 ~20KB，PSRAM 无压力）；起播阈值 = 2 chunk（~200ms）+ 自适应水位（网络抖动大时升到 4 chunk）。
- 时钟源：I2S DMA 时钟为主钟，缓冲水位驱动读速（标准做法，esp_player 可复用）[C]。
- 打断（barge-in）：唤醒词触发即 flush TTS 缓冲、断开/挂起 TTS WS 任务，预留 ~50ms 静音淡出防爆音 [C]。
- 任务布局：ASR-WS、LLM(SSE/WS)、TTS-WS、I2S 播放四任务；TLS 会话内存（mbedTLS ~40-50KB/连接）×2 并发连接需计入；16MB PSRAM 下无压力，但注意内部 RAM 保留给 WiFi/lwIP 快速路径（sdkconfig 已有 `SPIRAM_MALLOC_RESERVE_INTERNAL=32768`，沿用 doc23 绑核策略）[A/C]。

### 4.4 首响应 <1.5s 的延迟预算（C 级拆解，假设：普通话短句 5-15 字、WiFi 就近接入）

| 环节 | 预算 | 说明/达成条件 |
|---|---|---|
| VAD 判停尾长 | 300-500ms | 尾点静音窗；可用 AFE VAD 参数压到 ~300ms（牺牲误判率） |
| ASR 尾句定稿 | 100-400ms | 流式 ASR 尾包即出；若走"整段上传"（智谱/SiliconFlow 型）再 +上传 0.3-1s（15s 音频 480KB@16kpcm） |
| LLM 首 token | 300-800ms | BYOK 用户所选模型决定，产品不可控；**首句（~10 字）即触发 TTS** 是关键 |
| TTS 首包 | 150-500ms | 百炼/火山 150ms 级；智谱 <400ms |
| 网络+抖动缓冲 | 150-250ms | TLS 握手可提前预热（唤醒瞬间建连，长连接复用） |
| I2S/起播 | ~50ms | 2 chunk 水位 |
| **合计** | **1.05-2.5s** | **达成 <1.5s 的条件组合**：① 流式 ASR（非整段上传）② LLM 首句即送 TTS 双向流式 ③ TTS 直出 pcm ④ 唤醒时预热 TLS/WS 长连接 ⑤ 尾长 300ms ⑥ 就近云区域（北京/上海） |

---

## 5 候选组合与推荐

### 组合 A（首推起步）：国内云一站式 BYOK = 阿里百炼

- **构成**：ASR=Paraformer-realtime（或 fun-asr-realtime）WS + TTS=CosyVoice-v3.5/qwen-tts-realtime 双向流式 WS + 复刻=voice-enrollment API（10-20s 录音，免费创建）+ LLM=既有 OpenAI 兼容 BYOK。
- **体验上限**：高。TTS 150ms 级首包+服务端自动分句（天然适配 LLM 流式文本）；复刻音色合成价=普通价；延迟目标 1.5s 可达（§4.4 条件）。
- **月成本（中度使用：ASR 5h + TTS 30 万字符）**：ASR ~4 元 + TTS 30-45 元 ≈ **35-50 元/月/用户**（用户自付，[C] 估算）。
- **集成工作量**：**10-14 人日**（ASR WS 客户端 2-3d；TTS duplex WS+分句+打断 3-4d；I2S 流式播放+环形缓冲 3d；复刻 HTTP+App 录音上传配套 1-2d；TLS1.3 兼容/断线重连/弱网 1-2d）。每追加一家云 +3-4 人日。
- **风险**：DashScope WS 为私有 JSON 协议（非 OpenAI 兼容，无官方 C SDK）；CosyVoice pcm 直出需实测确认（否则退 mp3）；复刻音色绑定 target_model（模型升级需重建音色）；单一云依赖。

### 组合 B：全自托管 = FunASR(2pass WS) + CosyVoice3 + 极简网关（用户 NAS）

- **构成**：NAS/小主机 Docker 跑 funasr-runtime-sdk（:10095）+ CosyVoice3（流式）+ 一个 ~200 行的 FastAPI/WebSocket 网关统一设备协议（JSON 控制 + PCM 帧）；设备端只实现这一个协议。
- **体验上限**：有 GPU（4070 级）时媲美云（首包 150-300ms）；纯 CPU NAS 时 TTS 首包 1-3s+，1.5s 目标基本不可达（退 GPT-SoVITS 亦慢，issue 实测 3-4s）。克隆完全自由（3s 样本、无限音色、零费用、隐私最好）。
- **月成本**：云调用 0 元（电费+硬件折旧）。
- **集成工作量**：设备侧 3-4 人日（协议比云更简单）+ 服务端 docker-compose/网关 3-5 人日 + 用户部署文档与 App 配置页 1-2 人日 ≈ **7-11 人日**（可与 A 共用播放管线）。
- **BYOK 姿势**：App"语音服务→自托管"填 `wss://<域名>:<端口>`（+可选 token）；自签证书时配网页上传 CA PEM 到 NVS/SPIFFS 并挂入 mbedTLS 信任链 [C]。
- **风险**：用户运维门槛（端口映射/证书/模型下载 5-10GB）；家庭上行带宽；无公网 IP 用户需 frp/Tailscale 中转；模型/驱动版本漂移。

### 组合 C（推荐演进）：A 起步 + 预留自托管网关覆盖位（混合）

- 固件抽象一个 `speech_provider` 接口（两种实现：dashscope 直连 / 自定义 WS 网关），App 默认引导百炼填 key，高级用户可切自托管 URL；复刻默认走百炼 voice-enrollment（免费），自托管用户走 CosyVoice3 上传样本。
- **增量成本**：在 A 之上 +3-4 人日（网关协议实现+配置页）；服务端组件（组合 B 的 docker-compose）作为开源附件交付社区。
- 长期还能以同一网关挂火山（音质第二档）、MiniMax（克隆音色）做"云端多云"，设备固件不再改。

### 推荐结论

**首推组合 A（阿里百炼）立即启动，架构按组合 C 预留自托管 URL 覆盖位；火山豆包作为第二云在网关层接入（复刻仅用于官方预置角色音，不做用户自助克隆）。** 一句话理由：Bearer key 直用零签名（ESP32/mbedTLS 最省事）、双向流式+服务端自动分句天然适配 LLM 流式输出、复刻免费且 10-20 秒样本自助创建、TTS 1-1.5 元/万字符为国内最低档——BYOK"填个 key 就能用+能克隆"体验最顺。

---

## 6 风险与第 2 轮建议

### 6.1 风险清单

| # | 风险 | 等级 | 缓解 |
|---|---|---|---|
| R1 | 各云 wss 端点 TLS 握手兼容性（W-4 同类问题：仅 TLS1.3/1.3 优先） | 中 | 已开 TLS1.3；第 2 轮用 S31 真机对百炼/火山/智谱做握手矩阵实测（每家 30 分钟） |
| R2 | 百炼 CosyVoice pcm 直出未在文档示例出现 | 中 | 第 2 轮实测 format=pcm 参数；不行则 mp3+esp_audio_codec 解码兜底（组件已在 vendor） |
| R3 | DashScope 迁移新域名（maas.aliyuncs.com）协议细节与旧版差异 | 低 | 固件按新域名实现；跟踪官方迁移公告 |
| R4 | LLM TTFT 不可控（BYOK 用户模型差异）导致 1.5s 失守 | 高（产品级） | 首句即送 TTS+唤醒期预热连接+App 引导低延迟模型；文案上以"1.5-2.5s"为承诺口径 |
| R5 | 复刻音色合规（声音权益/内容审核）：国内云复刻均需实名+授权声明 | 中 | App 内录制同意声明+文字朗读提示；百炼复刻走官方 voice-enrollment（自带合规链路） |
| R6 | 自托管用户比例低但支持成本高 | 中 | 组合 C：自托管社区自助（docker-compose+文档），不进官方 SLA |
| R7 | 智谱 GLM-TTS-Clone"6 元/次"计费语义未明 | 低 | 第 2 轮读定价页+实测账单 |
| R8 | 火山/腾讯签名或 JWT 类鉴权（若第二云选腾讯） | 中 | 优先火山（X-Api-Key 直用）；腾讯排后 |

### 6.2 第 2 轮行动建议（实测清单）

1. **S31 真机 TLS 握手矩阵**：百炼（cn-beijing.maass + 旧 dashscope 域名）、火山 openspeech、智谱 open.bigmodel、SiliconFlow——各测 TLS1.2-only / 1.3 两种 ClientHello（复用 W-4 方法论）。
2. **百炼 TTS pcm 实测**：format=pcm + sample_rate=16000/24000 是否可用；首包延迟（wireshark/时间戳打点）。
3. **百炼 ASR 实时 WS 端到端**：16k pcm 分片上行，测尾句定稿延迟。
4. **复刻全流程**（App 原型）：录音 15s → base64 上传 create_voice → 轮询 → 用复刻音色 WS 合成。
5. **esp_audio_codec opus/mp3 解码实测**（若 R2 兜底触发）：RAM 峰值与 S31 CPU 占用（Lottie 并发时）。
6. **I2S 流式播放管线 PoC**：环形缓冲+水位起播+打断 flush，测量唤醒→首音全链延迟（目标分布 P50<1.5s / P90<2.2s）。
7. 自托管 PoC（可延后）：FunASR CPU 镜像 + CosyVoice3 在一张 8GB 卡上的首包与并发。

#### 6.4 附录B：TLS 握手矩阵实测（PC 端 openssl s_client，2026-10-25，主线 ZCode 补记）

| 端点 | TLS1.2-only ClientHello | TLS1.3 ClientHello | 备注 |
|---|---|---|---|
| dashscope.aliyuncs.com:443 | ✅ 协商 TLSv1.2（GlobalSign 链） | ✅ 协商 TLSv1.3 | 无 W-4 类 RST |
| cn-beijing.maas.aliyuncs.com:443 | — 基础域不解析 | 同左 | 实际端点为 `{WorkspaceId}.cn-beijing.maas.aliyuncs.com` 按工作区子域；**待取得真实 workspace 子域后补测** |
| openspeech.bytedance.com:443 | ✅（DigiCert/RapidSSL 链） | ✅ | |
| open.bigmodel.cn:443 | ✅（TrustAsia 链） | ✅ | |
| api.siliconflow.cn:443 | ✅（DigiCert 链） | ✅ | |

结论：R1 部分收敛——已测 4 端点均同时接受 TLS1.2-only 与 TLS1.3 ClientHello，无 W-4 型拒绝；固件已开 TLS1.3，风险进一步降低。口径注意：PC openssl 与设备 mbedTLS 的 ClientHello 扩展集不同，最终以 S31 真机握手为准。证书链根：GlobalSign/DigiCert/TrustAsia——需在 G2 核对设备端信任链（mbedTLS 内置 CA 集是否覆盖）。

## 6.3 主要来源索引（访问日期均为 2026-10-25）

- 百炼 CosyVoice WS：<https://help.aliyun.com/zh/model-studio/cosyvoice-websocket-api>（A）
- 百炼 Paraformer WS：<https://help.aliyun.com/zh/model-studio/websocket-for-paraformer-real-time-service>（A）
- 百炼声音复刻：<https://help.aliyun.com/zh/model-studio/voice-cloning-user-guide>、<https://help.aliyun.com/zh/model-studio/voice-clone-design-http-api>（A）
- 百炼价格：<https://help.aliyun.com/zh/model-studio/model-pricing>（A/B）
- 百炼语音 demo：<https://github.com/aliyun/alibabacloud-bailian-speech-demo>（A）
- 火山双向流式 TTS：<https://www.volcengine.com/docs/DoubaoVoice/bidirectional-streaming-text-to-speech-websocket>（A）
- 火山计费：<https://docs.volcengine.com/docs/DoubaoVoice/Billinginstructions-21>（A）
- 火山大模型流式 ASR：<https://www.volcengine.com/docs/DoubaoVoice/LargemodelstreamingautomaticspeechrecognitionAPI>（A）
- 火山声音复刻：<https://www.volcengine.com/product/voicecloning>（B）
- 腾讯实时 ASR WS：<https://cloud.tencent.com/document/product/1093/48982>（A）；流式 TTS WS：<https://cloud.tencent.com/document/product/862/133241>（A）
- 讯飞听写 WS：<https://www.xfyun.cn/doc/asr/voicedictation/API.html>（A）；超拟人 TTS：<https://www.xfyun.cn/doc/spark/super%20smart-tts.html>（A）；ESP32 案例：<https://www.cnblogs.com/FBsharl/p/19170481>（B）
- MiniMax T2A WS：<https://platform.minimax.io/docs/guides/speech-t2a-websocket>（A）；国内定价整理：<https://github.com/hkai-ai/LLM_OFFICIAL_DOCUMENTATION/blob/main/minimax/pricing.md>（B）；复刻实测：<https://zhuanlan.zhihu.com/p/21588943560>（B）
- 智谱 GLM-TTS：<https://docs.bigmodel.cn/cn/guide/models/sound-and-video/glm-tts>（A）；GLM-ASR：<https://docs.bigmodel.cn/cn/guide/models/sound-and-video/glm-asr>（A）；定价：<https://docs.bigmodel.cn/cn/guide/start/pricing>（B）
- SiliconFlow TTS 文档：<https://api-docs.siliconflow.cn/docs/userguide/capabilities/text-to-speech>（A）；CosyVoice2 价格：<https://zhuanlan.zhihu.com/p/15714942389>（B）
- fish-audio：<https://docs.fish.audio/api-reference/endpoint/websocket/tts-live>、<https://docs.fish.audio/developer-guide/models-pricing/pricing-and-rate-limits>（A）
- OpenAI Realtime/WebRTC：<https://developers.openai.com/api/docs/guides/voice-webrtc>（A）；ESP32 Cookbook ElatoAI：<https://developers.openai.com/cookbook/examples/voice_solutions/running_realtime_api_speech_on_esp32_arduino_edge_runtime_elatoai>（A/B）
- ElevenLabs：<https://elevenlabs.io/docs/overview/models>、<https://elevenlabs.io/pricing>（A/B）
- Azure 中国区克隆限制：<https://learn.microsoft.com/en-us/answers/questions/5544816/>（A）；定价：<https://azure.microsoft.com/en-us/pricing/details/speech>（A）
- CosyVoice 开源：<https://github.com/FunAudioLLM/CosyVoice>（A）；首包实测 issue：<https://github.com/QwenAudio/CosyVoice/issues/755>（C）；async_cosyvoice：<https://github.com/qi-hua/async_cosyvoice>（C）
- GPT-SoVITS：<https://github.com/RVC-Boss/GPT-SoVITS>（A）；流式首包 issue：<https://github.com/RVC-Boss/GPT-SoVITS/issues/1523>（A）
- FunASR runtime：<https://github.com/modelscope/FunASR/blob/main/runtime/quick_start.md>（A）
- sherpa-onnx：<https://github.com/k2-fsa/sherpa-onnx>、<https://k2-fsa.github.io/sherpa/onnx/>（A）
- 小智生态：<https://github.com/78/xiaozhi-esp32>（A）、协议分析 <https://xujiwei.com/blog/2025/04/ai-xiaozhi-esp32-protocol>（B）、火山方案文章 <https://developer.volcengine.com/articles/7540133843739934774>（B）
- esp_audio_codec：<https://components.espressif.com/components/espressif/esp_audio_codec/versions/2.4.0/readme>、v2.6.0 changelog（A）；esp-libopus 内存：<https://github.com/kahrendt/esp-libopus>（B）
