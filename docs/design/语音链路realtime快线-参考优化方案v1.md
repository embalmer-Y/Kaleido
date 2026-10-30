# 语音链路 realtime 快线——参考优化方案 v1（端到端快线 × 灵魂双线架构）

| 版本 | 日期 | 状态 | 作者 |
|---|---|---|---|
| v1.0 | 2026-10-30 | **参考优化方案（未裁决、未排期）**——非现行架构组成部分，不改变组合 C 级联线的任何既定决策；供负责人评估。触发条件/止损线见 §9，PoC 切片见 §8 | ZCode |

**输入**：现行级联语音链实测数据（V-38 §5-6、M1.5 延迟冲刺）；云端 ASR/TTS 调研（19 号）；灵魂架构方案 v1（含 2026-10-28 表达链路废止裁决）；2026-10-30 网络调研（百炼 Realtime API 官方文档、豆包 Seeduplex、ElevenLabs 延迟优化基准、LiveKit/Deepgram endpointing 基准，来源见附录 C）。
**本文职责**：回答"若采用端到端 realtime 语音模型（方案一），灵魂系统（角色/情绪/记忆/点火思考环）如何与之共存"的架构设计问题，并给出可验证的实施切片。**结论与建议仅供裁决，默认不执行。**

---

## §0 结论先行

1. **不动灵魂，只换嘴巴**：realtime 会话（端到端 S2S 模型）只承担"应答式对话"这一延迟敏感角色（快线）；驱力/情绪/仲裁/记忆/思考环全部留在设备侧慢环（慢线），站在会话外面**对着**会话说话，而不是搬进会话里面。这与 Galatea 双过程（快反射/慢思考）的灵魂架构原旨一致，且所有确定性可测试模块（黄金轨迹回归）零改动。
2. **三通道注入替代"全家桶 system prompt"**：persona+情绪快照走会话级 `instructions`（每次开会刷新）；慢环事件走轮级 system 项注入；记忆走 function calling 懒加载（`memory_search`/`memory_write`/`set_emotion` 三工具）。记忆从"推"改"拉"，绕开 realtime 会话 50 轮/300 秒音频的上下文上限。
3. **思考环（点火→思考→决策）原样保留**，只换最后的执行器：会话活着→注入 system 项 + `response.create`（模型用角色音色说）；会话没开→**走现有级联 TTS 线一行不改**。关键洞察：主动搭话没有延迟预算压力（没有用户等首音），慢线产物天然不需要 realtime。
4. **四项承重能力已核实存在**（百炼官方文档，2026-10-30 查证）：`instructions` 可随 session.update 刷新；`conversation.item.create` 支持注入 system/user/assistant 文本项（官方用途即"灵活注入历史上下文"）；客户端可无语音主动 `response.create`；function calling 完整回环（call_id 回写 + 二轮推理）。音色复刻支持 realtime 模型为 target_model（`voice-enrollment → voice_id → voice 参数`，仅首次 session.update 可设）。
5. **风险集中且可一日止损**：本方案最大不确定点是听感人格度、计费形状、instructions 尺寸上限（均❓待实测），PoC P1 切片一天内可全部探明；级联线完整保留为回退路径，双线可运行时切换（挂 kaleido_speech provider 抽象，设计文档 §2 组合 C 本就预留多云位）。

## §1 背景与现状基线

### 1.1 现行级联链实测（capture_end → first_audio 口径）

V-38 §6，qwen-plus 轮分解：ASR 定稿 0.5s + LLM 首 token 0.75s + **qwen-plus 首子句流式吐字 1.3s** + TTS 合成 0.5s + 出声 0.05s ≈ **3.0s**（P50，esp_hosted 调优后）。管线侧优化（TTS 预开连接藏进采音窗、子句级切送、流式早切 V-44）已榨干，剩余大头是云侧模型吐字速度。

**口径之外还有一块体感大头**：VAD 静音尾（用户说完→系统确认说完）为固定 1000-1200ms（convo-stage 1000ms / voice-loop-p4 1200ms），灵魂架构方案 §82 的预算写的 300ms 未达成。用户真实"闭嘴→听到回答"体感 ≈ 4.0-4.2s。

### 1.2 业界基准（2026-10 调研）

| 方案 | 延迟口径 | 出处 |
|---|---|---|
| ElevenLabs 级联管线优化基准 | 端到端 TTFA P50 ≈ 680ms / P95 ≈ 1.56s（其中 endpointing+STT ≈ 180ms、LLM 首 token ≈ 250-600ms、TTS 首音频 ≈ 110-220ms） | 官方博客 2026-06 |
| OpenAI GPT-Realtime-2.1 | 说毕→首音 P50 ~420ms；TTFA 200-250ms（第三方实测） | Samcom/AutoInterviewAI 2026 |
| Gemini 3.8 Live | TTFA 180-220ms | 同上 |
| 豆包端到端实时语音（Seeduplex 3.0） | 官方整体 ~1s / 裸模型 ~700ms；RTC 原生接入宣称 300ms 级 | 火山引擎产品页 |
| 百炼 qwen-audio-3.1-realtime | 官方未标数值（❓PoC 实测） | 百炼文档 |
| LiveKit Turn Detector（语义端点检测） | 300ms 延迟预算下误切率 9.9%（对照组 Deepgram Flux 12.9%、纯 VAD 27.7%） | LiveKit 官方 |

**判读**：级联不是死局（业界可进 1s 内），但 Kaleido 的 3.0s 里最大单项（qwen-plus 吐字 1.3s）只能换模型或换架构；静音尾 1s 是最便宜的体感优化点（与本方案独立，见附录 A）。

## §2 核心裁决建议：双线架构，分界 = 延迟敏感性

**原则：realtime 会话只当"嘴和耳"，不当"脑"。** 灵魂系统（W0-W4）是确定性设备侧模块，不搬进一个上下文 50 轮/300s、人格深度不及 MiniMax-M3 的云模型。

| 走快线（realtime 会话内） | 走慢线（现有架构原样） |
|---|---|
| 用户唤醒后的应答式对话（延迟敏感） | 点火触发→思考环（soul_think → ks_llm_chat → ki_intent_parse 门控） |
| 对话中的记忆检索/写入（工具调用） | 情绪九维动力学、驱力内稳态、Pareto 仲裁（1Hz 拍） |
| 会话中的情绪事件回报（set_emotion → 表达层） | 记忆固化/摘要（会后批量） |
| 服务端 VAD 轮次管理 + 打断 | 闲置期主动搭话的出声（级联 TTS 线，无延迟压力） |
| | 麦克风隐私门（WakeNet + 硬件静音开关 D-7-9） |

```text
用户语音 ⇄ ┌─ 快线：realtime 会话（延迟敏感，qwen-audio-3.1-realtime-*）─────┐
           │ instructions: persona 卡 + 情绪/驱力快照（开会时刷新）         │
           │ tools: memory_search / memory_write / set_emotion（按需拉）    │
           │ transcript 双流 ──┬────────────────────┬──────────────────────│
           └───────────────────┼────────────────────┼──────────────────────┘
                               ▼                    ▼
                     记忆写入队列（会后固化 W4）   情绪事件→Lottie（表达层）
                               ▲                    ▲（有界重评价，见 §4.3）
                               │                    │
慢线（不延迟敏感）: 点火→思考(M3/qwen 文本 LLM)→intent 门控─┬→ 会话 LIVE：注入 system 项 + response.create
   驱力/评价(W0-W2)·动机仲裁(W3)·记忆(W4)                └→ 会话 DORMANT：现有级联 TTS 线（零改动）
```

## §3 快线设计：realtime 会话

### 3.1 Provider 与协议

- **模型**：`qwen-audio-3.1-realtime-flash`（首选，支持 function calling + 联网搜索同开、成本低）与 `qwen-audio-3.1-realtime-plus`（音色列表更大）备选。同一 DashScope BYOK key（NVS `ds_key`，kaleido_secret 体系零改动）。地域北京/新加坡。
- **协议 v1 用 WebSocket**：复用 `esp_websocket_client` 现有栈（sp_dashscope.c 的 TLS 锚定 GlobalSign R46 经验直接继承）。**AOQ 协议**（内置 AEC/降噪/弱网对抗/快建连）二期评估——v1 阶段客户端 AFE AEC（convo-stage "MR"+FD_LOW_COST）继续服役。
- **音频规格**：上行 16kHz/16bit/mono PCM（与现行 ASR 上行完全一致，audio_io 采集链零改动）；**下行 24kHz/16bit/mono PCM**（❗与现行 16k 播放链不同，见 §5 音频层改动）。
- **对照组**（PoC 期一并探针，不实施）：豆包 Seeduplex 3.0（火山为既定第二云预留）、MiniMax 实时交互 API（官方有 xiaozhi-minimax-esp32-server 开源 ESP32 参考实现可抄）。

### 3.2 三通道注入（解决"输入不只是用户语音"）

**通道一：会话级 instructions（慢变层）**——每次开会话时注入，模板：

```text
[角色卡] persona.json character_card（人设、口头禅、说话风格、禁忌）
[当前状态] 情绪九维快照（引擎输出，非 LLM 生成）+ 驱力摘要 + 时间/场景
[行为准则] 短答复优先；不确定就调 memory_search 而不是编造；
          情绪变化时调 set_emotion 上报；禁止提及自己是模型/系统提示
```

尺寸预算：现行动态 system prompt 实测 2432B（soul.c），realtime instructions 上限❓待实测（P1 验证项）。超限对策：行为准则下沉到工具 description，角色卡压缩。

**通道二：轮级 system 项注入（事件层）**——慢环事件发生时 `conversation.item.create` 注入 system 角色文本项（如"用户刚触摸了你的头，你的被抚慰驱力刚得到满足"）。支持 `previous_item_id` 定位插入。

**通道三：工具层（按需拉）**——session.update 注册三工具：

```jsonc
memory_search:  { query: string, tag?: string[], k?: int }        // 检索记忆，返回 top-k 条目
memory_write:   { content: string, tags: string[], weight?: int } // 写入情景记忆
set_emotion:    { emotion: string, intensity: float, cause: string } // 情绪事件回报（非直写，见 §4.3）
```

行为契约（写进工具 description）：检索不到就直说不知道；每轮至多一次 memory_write；set_emotion 的 emotion 必须取自固定枚举表（23 词表多对一映射沿用表达层现行口径）。

### 3.3 会话生命周期状态机

```text
DORMANT ──WakeNet 命中──▶ WAKING ──WS建连+session.update（快照+音色+工具）──▶ LIVE
LIVE ──空闲超时 T_idle（建议 45s，实测调）──▶ CLOSING ──task-finished+close──▶ DORMANT
LIVE ──轮次/时长超限（50轮/300s）──▶ ROLLOVER：带记忆摘要重开新会话（无缝切换）
LIVE ──再喊唤醒词──▶ 强制打断/退出手势（会话控制，非对话内容）
DORMANT 后处理：transcript 落盘 → 记忆固化(W4) → 情绪/驱力更新 → 快照就绪
```

- 开会延迟（唤醒→可说话）= WS/TLS 建连 + session.update 往返，落在用户预期"设备在听"的窗口内，体感无害；实测数值❓（P1 标定）。
- **WakeNet 角色不变**：继续当开线门 + 隐私/功耗门（麦克风纯硬件静音 D-7-9 故事完整保留）；LIVE 态继续喂 WakeNet 作为"退出/强打断"手势（AFE AEC 防 TTS 自唤醒，convo-stage 已解决此问题）。
- `voice` 仅首次 session.update 生效、`turn_detection` 仅首音频前可改——都由"会话随唤醒生灭"天然消化；音色复刻的 voice_id 持久化在 persona.json（`voice.provider/model/voice_id/fallback_voice_id` 字段已定义，kaleido_persona 解析器已就绪待接线）。

### 3.4 transcript 双流挂钩

输入转写（`input_audio_transcription.completed`）与输出转写（`response.audio_transcript.done`）双事件：进记忆写入队列（会后由 W4 固化，替代现行"LLM 文本流喂记忆"钩子）；同时作表达层情绪分类的文本兜底输入。

### 3.5 设备侧卸载与保留

| 卸载（快线激活时） | 保留 |
|---|---|
| VAD-lite（server VAD/smart_turn 管轮次） | WakeNet（开线门+退出手势） |
| clause_pump 子句泵（模型内合成） | AFE AEC（"MR"+FD_LOW_COST，防自唤醒） |
| ASR/LLM/TTS 三 provider 的快线实例化 | 播放任务、kaleido_secret、慢线全套（见 §4） |

## §4 慢线设计：灵魂慢环接法（改动最小化）

### 4.1 思考环：决策不变，只换执行器

`soul_think()`（W3）→ `ks_llm_chat()`（文本 LLM，M3 主选/qwen 回落）→ `ki_intent_parse()` 门控——**链路零改动**。变的只是 intent 里"说出来"的分支：

- 会话 LIVE：决策文本 `conversation.item.create`（system 项）+ `response.create` → 模型以角色音色表达（风格与快线统一）；❗需处理与用户正在说话的竞争（response.create 排队/丢弃策略，P4 验证项）。
- 会话 DORMANT：现有 clause_pump → ks_tts_feed 级联线原样（**这就是该链路保留的第二理由**）。

### 4.2 情绪双向桥

- **慢环→快线**：九维引擎快照写进 instructions（只读注入，现行安全范式）。
- **快线→慢环**：`set_emotion` 工具回报**不是直写情绪**——它是"有界重评价请求"（`<mood>` 通道同语义），进九维引擎的有界调制 + TTL + 回执管线（与 2026-10-28"LLM 禁直写情感"裁决严格一致），再由引擎驱动 Lottie。回报同时驱动表达层的即时预览通道（两级：引擎确认前的短时预览动画 + 确认后的正式状态）。

### 4.3 记忆双写一致性

快线记忆 = transcript 会后批量固化（主）；慢线 think 轮的自省记忆 = 现行路径（辅）。两会话不相交（快线 LIVE 时 think 的产物若需立即影响对话，走 §4.1 的 system 项注入而非改记忆）。

## §5 组件落点（映射现有代码）

| 模块 | 现状 | 本方案下角色 | 改动量 |
|---|---|---|---|
| `kaleido_speech` | asr/llm/tts 三 provider + 抽象 | 新增 `ks_rt_*` 会话式接口（第 4 种形态；非对称塞进三件套——realtime 是有状态长会话，抽象层次不同） | 新增 ~600-800 行（新文件 sp_realtime.c + 事件分发） |
| `convo_fsm.c` | IDLE→LISTENING→THINKING→SPEAKING | 新增 DORMANT/LIVE 会话态与双线仲裁入口；级联 FSM 完整保留为慢线 | 中 |
| `clause_pump.c` | 子句泵→TTS | 慢线专用（主动搭话），不删 | 零 |
| `soul.c` `soul_think.c` | 情绪/驱力/思考环 | 零改动；`soul_convo_bind()` 增加快线分支（快照→instructions、transcript→记忆队列） | 小 |
| `wakenet_test.c` | AFE MR+AEC+唤醒 | 保留 AEC；LIVE 态唤醒词语义改为"退出/强打断"手势 | 小 |
| `audio_io.c` | 16k 采集/播放 | 采集零改动；播放支持 24k（快线）/16k（慢线）**运行时切换采样率**——两线互斥不同时出声，I2S/ES8311 按线切换即可（ES8311 支持 8-96k），无需重采样器 | 小（采样率参数化） |
| `kaleido_persona` | 解析器就绪未接线 | 快线音色绑定（voice_id）首个消费方 | 小 |
| `kaleido_secret` | ds_key/mm_key NVS | 快线复用 ds_key | 零 |
| `server/voice-gateway` | DashScope WS 透传预留 | 未来可扩为 realtime 透传/多云代理（与 19 号 §多云构想衔接），本期不动 | 零 |

## §6 延迟与成本预期（❓均为待实测标定项）

| 段 | 快线预估 | 依据 |
|---|---|---|
| 唤醒→会话就绪 | 0.5-1.5s（一次性，落在"设备在听"窗口） | WS+TLS 建连经验值；AOQ 更快（二期） |
| 用户说毕→模型开说（首轮/后续轮） | ❓ 0.3-0.8s（业界同侪 200-500ms） | GPT/Gemini/豆包基准外推 |
| 含工具调用的轮次 | +1 RTT（function_call 回环） | 协议固有 |
| 体感（含端点检测） | server smart_turn 替代固定静音尾，预期省 0.4-0.8s | LiveKit 300ms 预算基准 |

计费对照实验（P1 必做）：同 100 轮中文对话脚本，级联线（paraformer+qwen-plus+speech-2.5-hd）vs 快线（realtime 按时长计费）各跑一遍出账单对比。BYOK 下这是用户的钱，账单形状变化必须向用户透明。

## §7 风险清单与止损线

| # | 风险 | 检测点 | 止损动作 |
|---|---|---|---|
| 1 | 听感人格度不足（realtime 模型"演"不像角色） | P1 负责人盲听评审 | 双线制产品化：快线只做通用应答，人格表达留慢线 |
| 2 | 计费按时长显著贵于级联按量 | P1 账单对照 | 同上，或限快线为"短问答模式" |
| 3 | instructions 尺寸上限 < 2.5KB | P1 实测 | 角色卡压缩+行为准则下沉工具 description |
| 4 | realtime 模型工具调用不自觉（该检索不检索、编造记忆） | P2 专项测试集 | instructions 强化契约；兜底=会前预注入 top-3 记忆 |
| 5 | 打断回声（设备喇叭→麦克风）影响 server VAD 判定 | P1/P3 实测 | AFE AEC 保留（已验证）；极端情况退 AOQ |
| 6 | 24k 播放链切换引入爆音/毛刺 | P3 | 切换点静音 50ms 交叉淡变 |
| 7 | 慢线注入与用户抢话竞争 | P4 | 排队/丢弃策略；最坏退 DORMANT 慢线出声 |
| 8 | 供应商锁定/模型下线 | 持续 | provider 抽象隔离；豆包/MiniMax 对照探针保留 |
| 9 | C5 WiFi 断连时快线不可用 | 已有事实（esp_hosted 3.0.10 OTA 回归中） | 慢线级联同样依赖网络——真正的离线降级是 WakeNet+本地提示语（既定 §9 三态降级矩阵不变） |

**结构性保障：级联线一根线都不拆。** 任何时点可 Kconfig/运行时切回纯级联（组合 C 原样），本方案是加法不是替换。

## §8 PoC 切片与验收门（每片一天级，独立止损）

| 切片 | 内容 | 验收门 |
|---|---|---|
| **P1** | HIL 工程（建议名 voice-realtime-p4）：静态 persona instructions + 复刻音色 + WS 直连，跑通唤醒→对话→退出 | ①说毕→首音 P50 实测数 ②100 轮账单对照 ③instructions 2.4KB 实测通过 ④负责人盲听听感过审。任一不及格→方案止于此 |
| **P2** | + 三工具（本地 JSONL 假记忆库） | 工具调用自觉性测试集通过率；编造记忆 = 0 |
| **P3** | + 会话生命周期（T_idle/ROLLOVER）+ set_emotion→表达层预览 + 24k/16k 播放切换 | 闲置自动关会话无残留；切换无爆音；情绪事件→Lottie 端到端 |
| **P4** | + 慢线注入（response.create）+ 双线仲裁（W3 仲裁器扩"说话通道选择"输出，不新增模块） | 主动搭话双通道各验证 3 场景；抢话竞争处理 |

门禁编号建议沿用 V-xx 体系由负责人指派（本文不占号）。

## §9 触发条件与决策归属

- **值得切的信号**：①qwen-flash 分层复测后级联 P50 仍 >2s 且负责人不接受；②用户/负责人真人评测对 3.0s 体感给出负面结论；③产品需要"打电话式"连续对话体验（级联逐轮制做不到）。
- **不切的信号**：M1.5 验收线复议到 ~3s 并通过；或 P1 听感/计费任一不及格。
- **决策归属**：本方案所有裁决点归负责人（含是否启动 P1）。ZCode 侧仅维持本文档与附录 A 的独立快优化跟踪。

---

## 附录 A：与本方案独立的级联线快优化（不依赖 realtime，随时可做）

1. **endpointing 混合策略**（体感 -0.4~0.8s，零新依赖）：静音 600ms **且** 收到 ASR `sentence_end` 即定稿，否则 1200ms 兜底。向设计预算 §82 的 300ms 目标收敛。
2. **qwen-flash 首句分层**（口径内 -0.5~1s，等 429 限流解除）：首子句用 flash 起、长答复不换模型。
3. **MiniMax TTS 切 WS 版 t2a 逐句流式**：长会话省每子句建连（ElevenLabs 口径：空闲 TTS WS 几乎不占并发）。
4. **播放缓冲压缩**：稳定连接下 500ms→50-150ms；首包小 chunk。

## 附录 B：qwen-audio realtime WS 关键事件速查（2026-10-30 官方文档摘录）

- `session.update`：instructions / voice（仅首次生效）/ tools / turn_detection（仅首音频前可改）/ max_history_turns(1-50, 默认20)
- `conversation.item.create`：type=message（role: system/user/assistant，content `input_text`）/ function_call / function_call_output；`previous_item_id` 定位插入
- `response.create`：客户端可无用户语音主动触发（push-to-talk 示例即此用法；function calling 二轮同款）
- 工具回环：`response.function_call_arguments.done`（含 call_id）→ 客户端执行 → `conversation.item.create`(function_call_output) → `response.create`
- 上下文：服务端维护，50 轮/300s 音频自动丢弃更早；音频项查询仅返回转写文本
- smart_turn_v2：服务端轮次判定 + 防旁人插话（可传 ≤5 个声纹 URL 锁定说话人）
- 音频：入 16k/16bit PCM（100ms=3200B 官方推荐），出 24k/16bit PCM

## 附录 C：资料来源

- 百炼 Realtime API 概述 / Qwen-Audio 实时语音对话指南 / Qwen-Omni-Realtime（help.aliyun.com，2026-10-30 查证）
- 火山引擎：豆包端到端实时语音大模型产品页与 RTC 接入文档
- ElevenLabs: Voice Agent Latency Optimization（2026-06）
- LiveKit: Solving end-of-turn detection（Turn Detector v1.0 基准）；Deepgram: Turn Detection in Voice Agents；Evalgent 端点检测基准
- MiniMax 实时交互 API 发布公告；Speech 2.6 实测（知乎）；xiaozhi-minimax-esp32-server（官方开源）
- OpenAI GPT-Realtime-2.1 / Gemini 3.8 Live 第三方延迟基准（Samcom/AutoInterviewAI，2026）
- 项目内：V-33/V-38 门禁、19 号调研、灵魂架构方案 v1（§21 组合 C / §82 延迟预算 / 表达链路 2026-10-28 废止裁决）、M2 阶段计划（MiniMax 切换指示）
