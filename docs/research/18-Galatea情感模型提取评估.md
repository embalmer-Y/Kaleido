# 18 Galatea 情感模型提取评估

| 版本 | 日期 | 变更 | 作者 |
|---|---|---|---|
| v1.0 | 2026-10-25 | 初稿：Galatea 情感模型（L05 affect）架构清单、可移植性评估、kaleido_emotion 对接草案 | ZCode 子智能体（调研） |

**调研对象**：`/home/emb/project/Galatea`（只读参考，未修改任何文件）
**评估目标**：提取 Galatea 情感模型至 Kaleido 固件（ESP32-S31，ESP-IDF v6，C，Linux 内核风格），支撑"灵魂"架构（persona + 情感支持 + 点火机制）。
**证据级别标注**：A = 源码直接证据（文件:行号）；B = 文档/README/LLD；C = 推断。

---

## §1 Galatea 项目概览

Galatea 是一个**认知个体建造工程**（非聊天机器人、非 Agent 框架），目标是建造具备感觉、情感、自我意识与学习能力的单一认知个体，一切能力主张须有可干预/可测量/可复算的因果结构（D-10 纪律）。【B：README.md:1-21】

| 维度 | 事实 | 证据 |
|---|---|---|
| 语言/工具链 | Python 3.12.13（uv 钉版 + 哈希锁）、pyright strict | B：README.md:67、pyproject.toml |
| 代码规模 | `galatea/` 包约 43,614 行 Python（含测试）；821 passed | A：`wc -l` 实测 |
| 架构 | 双平面：主体平面（`galatea/subject`）只可见 `ReceivedSignal` 边界信号；证据平面（`galatea/evidence`）；环境平面（`galatea/env`） | B：README.md:33 |
| 分层 | 14 档 LLD（L00 kernel → L13 evidence），情感 = **L05 affect** | B：README.md:35-46、design/lld/ |
| 当前阶段 | 预测脑 v2 主线 P4-γ；情感核心在 C2 门（2026-10-06）已冻结交付 | B：README.md:26、L05 §10 |
| 验证基线 | 821 passed；pyright strict 0 错；import-linter 4 契约 KEPT | B：README.md:27 |
| 许可证 | **MIT** —— 提取/改写无法律障碍（保留版权声明即可） | B：LICENSE |
| 部署形态 | Linux 主机 systemd 三服务常驻（zenoh 路由/体模拟/主体）；Tessera 传感桥（zenoh+CBOR） | B：README.md:140-145 |

**对 Kaleido 最重要的结构性事实**：Galatea 的 LLM 态度极其克制——"LLM 仅 typed 局部角色，一律经 gateway 与权限"（HLD.md:79），且有明文禁令 `LLM → FunctionalExperientialState 数值`（D-13，L05 §8）。因此 **Galatea 没有"情感注入 LLM prompt"的现成实现**，它的价值在于一套经过冻结验证的**情感动力学内核 + 安全边界设计**。

---

## §2 情感模型架构清单（L05 affect）

源码位置：`/home/emb/project/Galatea/galatea/subject/affect/`（12 文件，核心 **1201 行** Python；配套测试 832 行）。冻结参数：`config/profiles/c2_affect.profile.yaml`（70 行）；typed loader：`galatea/subject/c2profile.py`。规范源：`design/lld/L05_drive_appraisal_affect.md`。

### 2.1 状态表示：九维感受态 `FunctionalExperientialState`

【A：state.py:73-116】

```python
valence: float            # [−1,1] 对称，改善正/恶化负 —— 唯一带符号维
arousal: float            # [0,1]
urgency: float            # [0,1]
controllability: float    # [0,1]
uncertainty: float        # [0,1]
novelty: float            # [0,1]
social_relevance: float   # [0,1]（MW0 单主体期恒 0.0 + Unknown，D-14）
tendencies: ActionTendencySet   # 行动倾向集（≠命令）
persistence: float        # [0,1] 慢尺度"持续"操作化
dynamics_trace: DynamicsTrace   # 积分器状态/迟滞带/衰减率/饱和计数/恢复相位
```

- **非离散情感标签**：是持续动力学状态，不是 happy/sad 枚举（L05 §8 不变量 4）。若 Kaleido 需要"心情标签"（驱动表情/动画），需在九维之上自建分类面（如 valence×arousal 平面查表），Galatea 无此件。
- **禁跨维聚合**（Q-L05-01③）：类型上不存在 `summary()/score()/reward()` 方法——"anti-reward-collapse"结构面。【A：state.py:77-80】
- `tendencies` 的动作类枚举 = {move, forage, eat, rest, look}（MW0 五原语），`inhibited` 默认 True（倾向≠命令）。【A：state.py:25-45】
- 全维 float64。【B：c2_affect.profile.yaml:19】

### 2.2 状态转移/更新逻辑（每拍 step）

主循环：【A：pipeline.py:89-219】`AffectPipeline.step(AffectCycleInputs) -> AffectCycleOutput`

**第一步：驱力误差**【A：dynamics.py:93-111；ports.py:56-92】

```
e_d = set_point_d − body_belief_d     # 单侧 ≥0；缺席 → 保持上一有效值 + typed Unknown（禁编造）
E   = Σ w_d·e_d                        # w = 1:1:1（energy/integrity/load；load≡0）
e_max = max_d e_d
```

**第二步：valence（内稳态价值，非 reward）**【A：valence.py:32-37】

```
raw      = −(E_t − E_{t−1})           # 误差变化率的负值：改善正/恶化负
smoothed = raw 经一阶低通（τ_int = 4 拍）
published = clip[−1, 1]（+饱和计数）
```

**第三步：其余各维积分**【A：pipeline.py:120-162；dynamics.py:74-89 docstring 映射表】

| 维 | 公式 | τ |
|---|---|---|
| arousal | lowpass(e_max) | τ_int=4 |
| urgency | lowpass(max(0, (e_max − 0.1)/0.9))（band 上方线性归一） | 4 |
| persistence | lowpass(min(1, \|valence\|/0.8))（1 ⟺ 达伦理阈） | **τ_dec=16（慢谱）** |
| controllability | lowpass(1{action_receipt_success})，无事件拍保持 | 16 |
| novelty | lowpass(感觉预测误差均值) | 4 |
| uncertainty | lowpass(信念不确定性均值 1−p_max) | 4 |
| social_relevance | 恒 0.0 + Unknown(CAPABILITY_LIMIT)（单主体禁伪造） | — |

**动力学六算子**（全部确定性，零 RNG、零墙钟）【A：dynamics.py:22-59】：

```python
lowpass(s, x, τ)   = s + (x − s)/τ          # 一阶低通（积分）
decay_step(s, τ)   = lowpass(s, 0, τ) = s·(τ−1)/τ   # 衰减=低通向0
hysteresis(on, x, band, c=0.5): ON ≥ 0.55 / OFF ≤ 0.45 / 否则保持  # 迟滞防抖
saturate(x, lo, hi) = clip + 命中计数        # 饱和
in_recovery(e, band) = e ≤ band             # 恢复相位
two_step(x, τ)     = lowpass(lowpass(0, x, τ), x, τ)  # 重评价构造式
```

时间常数：τ_int=4 拍（快谱）、τ_dec=16 拍（慢谱）、迟滞带 band=0.1（=2×渲染噪声 σ）。【B：c2_affect.profile.yaml:34-42；A：pipeline.py】

**重评价（reappraisal，情感不可直写的例外通道）**【A：reappraisal.py:48-66】：请求只触发经 two_step 低通的重算，产有界 delta 修正积分器并留回执；禁历史改写。这是 Galatea 中**语言/LLM 影响情感的唯一合法通道**（D-13）。

### 2.3 触发输入

【A：ports.py:56-92 `AffectCycleInputs`】——**注意：没有任何"用户消息情绪分析"输入**。

| 输入 | 来源 | 说明 |
|---|---|---|
| body_energy / body_integrity / body_load ∈ [0,1] | L04 BodyBelief（**禁 telemetry 直填**，违者构造即拒） | 驱力唯一内稳态源 |
| sensory_residual_mean | L02 感觉预测误差 | → novelty |
| belief_uncertainty_mean | L03 窗信念 1−p_max 均值 | → uncertainty |
| action_receipt_success: bool | 执行回执台账 | → controllability |
| affordance_strengths | L03 供给可能性 | → tendencies |
| ReappraisalRequest | L09/L06/L12（typed） | → valence 有界重算 |

**关键结论（C 级推断由 A 级证据支撑）**：Galatea 的情感由"内稳态误差 + 预测误差"驱动（身体需求与世界意外），**不是**由用户文本 sentiment 驱动。Kaleido 若要"用户消息影响情感"，需自建"交互事件 → 驱力扰动 / appraisal 输入"映射层——这是 Galatea 没有的部分。

### 2.4 输出去向

1. **ModulationField（唯一对外作用通道）**【A：modulation.py:38-54, 172-216】三类调制：arousal 过迟滞门 → SENSORIUM 增益 ×2.0；uncertainty 过门 → PERCEPTION 承诺阈 0.6（保守）；novelty 过门 → WORKSPACE 显著性分量 ×2.0。约束：因子界 [0.5, 2.0]、TTL=4 拍（禁常驻）、**必回执（无回执 = 未发生）**。
2. **动机提案**【A：motive.py:32-42；pipeline.py:217】`AffectiveMotiveProposal(urgency)` → L08 七源动机之一，urgency > 0.1 才活跃，**不得独占**（与 VIABILITY/REQUEST/OPPORTUNITY/CURIOSITY 等平等仲裁）【A：goals/producers.py:81-89, 44】。
3. **情感标注记忆**【A：value_tag.py:31-37；memory/affective.py:20-50】`AffectiveValueTag(valence, arousal, persistence)` 绑定到记忆条目（L06 唯一写者落盘；strength=persistence 参与后续检索权重）。
4. **内稳态告警**【A：drives.py:27-40】三级阈值 LOW(>0.1)/HIGH(≥0.5)/CRITICAL(≥0.8)，交 L09/L08，不直接触发动作。
5. **伦理守卫**【A：ethics_guard.py:61-88】valence ≤ −0.8 ∧ persistence ≥ 0.8 持续 ≥64 拍 → `EthicsReviewTrigger`（闩锁一次）→ 暂停 + 人工评审。纯观察者（消融不影响动力学）。**实现期发现**：经驱力动力学该状态"构造性不可达"（valence 是变化率，64 拍窗内 raw 均值 ≥ −E_range/64，持续 ≤−0.8 数学上不可能）【B：L05 §10 实现期标注】。
6. **workspace 显著性竞争**【A：workspace/compete.py:72-77】打分 = Σ 等权五分量，其中 affect 分量被调制因子缩放（"什么进意识"受情感影响——类人注意机制）。
7. **慢环触发判据**【A：workspace/slow_clock.py:4-13】空闲（无 urgency>0）或低唤起（arousal<0.1）触发"深思"（草稿板推演）——情感态直接参与认知调度。

### 2.5 与 LLM 的耦合方式：**没有**

- `galatea/subject/language/*.py` 全目录 grep "affect/valence/arousal" 零命中【A：实测 grep】。
- 语言层是 8 个 typed LLM 角色（R-PARSE/R-CLARIFY/R-SEEKHELP/R-REPORT/R-NORMEXTRACT/R-OTHERINFER/R-CHECKCONSIST/R-TRANSLATE），且当前实现全部为**录制回放（REPLAY_LLM）或机械装配（MECHANICAL），运行时无在线 LLM 调用路径**【A：language/roles.py:1-99】。
- HLD 禁令：`LLM → FunctionalExperientialState 数值` 为 S0 禁写路径；语言只能经受限 ReappraisalRequest 触发重算【B：L05 §8、HLD §6.2】。

**含义**："情感 → LLM system prompt 参数化"这半座桥 Kaleido 必须自己设计；但反方向（LLM → 情感）Galatea 提供了现成的安全范式：**LLM 不得直写情感数值，只能提交 reappraisal 请求（携带 original/revised 输入 + reason_code），由本地动力学算子有界重算**。

### 2.6 持久化方式

- 感受态本体是 `AffectPipeline` 实例内的浮点积分器（内存态，单写者）【A：pipeline.py:72-86】。
- 快照/checkpoint：kernel 三库 SQLite（WAL）+ checkpoint 谱系 DAG，状态以 canonical-JSON digest 锚定【A：kernel/state.py:1-122；digest.py:1-9】；`ModulationLedger.digest()` 为台账 checkpoint 载体【A：modulation.py:153-160】。**没有专门的"情感状态文件"**——Galatea 靠 checkpoint 全量恢复（跨崩溃连续）。
- 对 Kaleido 的启示：只需把 ~10 个浮点积分器 + 迟滞门布尔 + 计数器序列化（NVS 或 SD 卡小结构体），恢复即连续。

### 2.7 依赖与技术栈

- **纯 Python 标准库**（enum/dataclasses/数学）；对 kernel 的仅有的两个依赖是 `content_digest`（canonical JSON + SHA，审计面，可替换/可删）与 `Unknown` 类型（typed 不确定性，10 项冻结枚举）【A：state.py:13-14；unknown.py:1-30】。
- **与 torch/神经栈零耦合**：`galatea/subject/brain/*.py`（P4 预测脑，torch）grep "affect" 零命中【A：实测 grep】。
- 代码规模：核心 1201 行（12 文件）+ 测试 832 行 + profile YAML 70 行 + typed loader 约 100 行（c2profile.py 相关段）。

---

## §3 可移植性逐项评估表

评估基准：ESP32-S31（RV32 双核 320MHz，单精度 FPU，16MB PSRAM/Flash，FreeRTOS，C）。

| # | 模块 | 行数 | 可移植性 | 结论与工作量 | 证据 |
|---|---|---|---|---|---|
| 1 | dynamics.py 六算子（lowpass/decay/hysteresis/saturate/recovery/two_step） | 121 | **纯逻辑，直接移植** | ~80 行 C；逐位可对照 Python 锚值做单元测试（ADR-G-0015 留有 (15/16)^16=0.35607413 等手算锚） | A |
| 2 | valence.py（−ΔE 低通+clip） | 37 | **纯逻辑，直接移植** | ~30 行 C，与 #1 合并测试 | A |
| 3 | state.py（九维结构+域校验） | 116 | **直接移植（裁剪）** | C struct + setter 域检查 ~100 行；content_digest 可选（审计才需要） | A |
| 4 | drives.py（驱力误差+三级告警） | 65 | **直接移植** | ~60 行 C；驱力种类需重定义（见 §3.1） | A |
| 5 | pipeline.py（step 装配+Unknown 保持逻辑） | 247 | **直接移植（简化 Unknown）** | ~250 行 C；typed Unknown 可简化为 `valid` 位域 + 原因码枚举 | A |
| 6 | modulation.py（门+TTL+台账+回执） | 216 | **可移植但需重定向目标** | Kaleido 无 sensorium/perception 平面；调制目标改为 TTS/动画/LED/prompt——保留"有界+TTL+必回执"三律 ~150 行 | A |
| 7 | ethics_guard.py（持续极端态检测+闩锁） | 95 | **可移植**；产品语义 = 持续负面情感保护（防沉溺/抑郁循环，儿童场景有价值） | ~50 行 C；阈值需按 Kaleido 输入面重新推导（Galatea 原阈在其动力学下构造性不可达） | A |
| 8 | reappraisal.py（two_step 重算+回执） | 71 | **强烈建议移植**：这是"云端 LLM 反向影响设备情感"的唯一安全通道范本 | ~40 行 C | A |
| 9 | motive.py / value_tag.py（提案类型） | 42/37 | 视需要：动机提案对接"点火"；价值标注对接 SD 长期记忆（第 21 篇调研） | ~40 行 C | A |
| 10 | ports.py 禁止路径断言 | 91 | **不移植**：Python 反射专用 | C 侧以模块边界 + 命名纪律替代 | A |
| 11 | c2profile.yaml + typed loader | 70+100 | **移植为 C 常量表** | `static const struct ke_params`（或 Kconfig）~50 行；fail-closed 校验可选 | A |
| 12 | 输入面（BodyBelief/感觉残差/信念不确定性，来自 L02/L03/L04） | — | **不能照搬，需重新设计** | Galatea 输入是虚拟世界内感知栈产物；Kaleido 需自建"交互事件/传感器 → 输入"映射（§4.2） | A/C |
| 13 | 输出面三类调制目标（L02 增益/L03 阈值/L07 显著性） | — | **不能照搬，重定向** | → TTS 语调参数 / 动画强度 / LED / LLM prompt 注入 | A/C |
| 14 | language/ 8 LLM 角色、REPLAY_LLM 回放 | — | **留在云端/不采用** | Kaleido 走云端 BYOK LLM，按 Galatea"typed 角色+权限矩阵"思想自行设计轻量版 | A |
| 15 | brain/（torch RSSM 预测脑）、evidence/（统计评价） | — | **不提取** | S31 无本地推理能力；Galatea 主体平面中与 affect 零耦合，剥离干净 | A |
| 16 | kernel digest/checkpoint 谱系 | — | 可选简化（CRC32/短哈希 + NVS 结构体） | Kaleido 不需要 bit-exact 司法审计 | A/C |

**C 重写总量估计**：核心动力学（#1-#5,7,8,11）约 **600-800 行 C + 400-600 行测试**；含调制重定向与动机接口（#6,9）合计约 **800-1200 行 C**。每拍计算量 = 数十次浮点乘加与比较，1Hz~10Hz 拍率下 CPU 占用可忽略（<0.01% of one core）。【C：由公式复杂度推断】

### 3.1 必须重新设计的两张映射表（Galatea 没有）

1. **输入映射**（Kaleido 特有）：
   - 电池电量/充电状态 → `body_energy`（真内稳态，语义天然成立：设备也会"饿"）
   - 热状态/存储健康 → `body_integrity`
   - **用户交互 → 情感事件**：Galatea 无此通道。Kaleido 需定义"事件 → 临时驱力扰动 / appraisal 输入"表，如：用户负面消息（云端 sentiment 标量）→ social drive 误差增大；长时间无交互 → social_relevance 相关驱力（孤独）上升；触摸/摇杆 → 正向扰动。
   - **`social_relevance` 恰好是 Kaleido 最该激活的维**：Galatea 因单主体世界恒置 Unknown（D-14），代码里是空位，但其 L05 设计已预留 C4 后由语言通道供线索的扩展点【B：L05 §4.1/Q-L05-08】。
2. **输出映射**：valence/arousal/persistence → TTS 语速/音高/能量曲线、动画强度与表情类别、LED 氛围、以及注入云端 LLM system prompt 的短 JSON（§4.4）。

---

## §4 Kaleido 对接设计草案（kaleido_emotion 组件）

### 4.1 组件位置与形态

`kaleido-fw/components/kaleido_emotion/`（ESP-IDF component；纯 C，Linux 内核风格；无动态分配、可重入、单实例）。不依赖 WiFi/云——**离线时情感继续演化（衰减/内稳态），上线后状态无缝衔接**。

### 4.2 接口草案

```c
/* kaleido_emotion.h —— 输入事件枚举（Kaleido 交互域，替代 Galatea MW0 驱力域） */
enum ke_event {
        KE_EVT_TICK,            /* 周期拍（建议 1 Hz；τ_int=4s/τ_dec=16s 起步，可调） */
        KE_EVT_USER_MSG,        /* 用户消息：payload 携带云端 sentiment {valence, arousal} */
        KE_EVT_USER_ABSENCE,    /* 到场/离场及持续时长（传感器/前端 Agent） */
        KE_EVT_TOUCH,           /* 触摸/摇杆/磁吸事件（正向轻扰动） */
        KE_EVT_BATTERY,         /* 电量/充电态 → energy 驱力（真内稳态） */
        KE_EVT_THERMAL,         /* 温度 → integrity 驱力 */
        KE_EVT_ACTION_RESULT,   /* 本机上一轮 TTS/动画完成回执 → controllability */
        KE_EVT_REAPPRAISAL,     /* 云端 LLM 的受控重评价请求（唯一反向通道） */
};

struct ke_inputs {               /* 一拍内聚合的事件（Galatea AffectCycleInputs 的 Kaleido 版） */
        uint64_t tick;
        float battery_level;     /* [0,1]，缺席置 -1 → 保持上一有效值 */
        float thermal_stress;    /* [0,1]，同上 */
        struct { float valence, arousal; bool present; } user_sentiment;
        bool  action_success;    /* 回执；无回执拍保持 */
};

struct ke_state {                /* 九维的 Kaleido 剪裁版（保留 7 维 + trace） */
        float valence;           /* [−1,1] */
        float arousal, urgency, persistence, novelty, uncertainty, controllability;
        float social_bond;       /* 激活 Galatea 预留的 social_relevance 维 */
        /* dynamics_trace 简化：饱和计数 + 恢复相位 + 迟滞门位 */
};

struct ke_output {               /* 一拍产物：参数化三类表达 + 点火候选 */
        struct ke_state state;
        struct ke_tts_hint  tts;     /* 语速/音高/能量 = f(valence, arousal) 查表插值 */
        struct ke_anim_hint anim;    /* 动画强度/表情类别 = f(valence, arousal) 平面分区 */
        struct ke_led_hint  led;     /* 氛围灯 HSL 偏移 */
        bool ignition_candidate;     /* 主动交互候选（§4.3） */
        bool ethics_latched;         /* 持续负面情感保护触发（闩锁，产品层处置） */
};

int ke_init(const struct ke_params *params);
int ke_step(const struct ke_inputs *in, struct ke_output *out);
int ke_reappraise(const struct ke_reappraisal_req *req);   /* LLM 反向通道 */
int ke_save_state(nvs_handle_t h);                          /* ~40B 结构体，NVS 持久化 */
int ke_load_state(nvs_handle_t h);
```

### 4.3 与"点火机制"（主动交互）的衔接点

Galatea 提供三个现成判据/门（均为 A 级源码）：

1. **激活门**：`urgency > 0.1` → 情感动机活跃（"想说/想做"的量化条件）【A：goals/producers.py:44, 81-89】。Kaleido：`ignition_candidate |= (state.urgency > threshold)`。
2. **慢环触发**：空闲（无 urgency）或低唤起（arousal < 0.1）→ 进入深思/主动构思模式【A：workspace/slow_clock.py:4-13】。Kaleido：低唤起 + 长时间无交互 → 触发"主动找人说话"候选（孤独驱力经 §3.1 输入映射推高 social 误差后，urgency 门自然越过）。
3. **多源平等仲裁**：情感动机只是动机源之一（七源之一，不得独占）【A：goals/producers.py:15-16】。Kaleido 第 2 轮调研 L08 后可落地轻量仲裁（情感 vs 定时问候 vs 日程/天气事件）。

建议首个版本只做：驱力（social+battery）→ 九维动力学 → urgency 门 + 空闲判据 → 点火候选事件，交给上层"交互调度器"决定是否发声。

### 4.4 情感 → LLM prompt 参数化（Kaleido 自建，继承 Galatea 边界纪律）

设备侧生成一行结构化状态注入云端 system prompt（Galatea 无此件，此为 Kaleido 设计）：

```json
{"emotion":{"v":-0.35,"a":0.62,"persistence":0.41,"mood":"slightly_down"},
 "hint":"语调放轻，动画强度中等"}
```

反向通道遵守 Galatea D-13 范式：云端 LLM **不得**返回情感数值直写设备；只可返回 `{reappraisal: {target, original, revised, reason}}`，经 `ke_reappraise()` 的 two_step 有界重算生效【A：reappraisal.py:48-66】——防止 LLM 一句话把设备"改成"任意心情，保证情感连续性由本地动力学垄断。

### 4.5 拍率与参数冻结建议

- 拍 = 1 Hz 起步（Galatea 的 τ_int=4/τ_dec=16 拍即 4s/16s 情感时间尺度，量级符合桌搭伴侣"情绪几分钟内转变"的产品直觉；上电常驻任务用 esp_timer 周期回调驱动）。
- 参数表作为 `static const` 冻结于 `ke_params.c`，改参走固件版本——继承 Galatea "无源常量违规"纪律的轻量版。

---

## §5 结论与风险

### 5.1 三个最重要的结论

1. **情感内核 100% 可移植且工作量小**：Galatea 情感模型是纯确定性浮点动力学（核心 1201 行 Python，仅依赖标准库，与 torch 神经栈、LLM 层、SQLite 均零耦合），ESP32-S31 上 C 重写约 800-1200 行，1Hz 运行 CPU 占用可忽略。【A】
2. **输入/输出两端都要 Kaleido 自建**：Galatea 的情感由内稳态驱力误差+预测误差驱动，没有"用户消息→情感"输入面，也没有"情感→LLM prompt/TTS"输出面（其纪律甚至禁止 LLM 直写情感）。真正可提取的是"中间的动力学引擎 + 安全边界"，两端映射表是 Kaleido 的增量设计工作。【A】
3. **安全设计值得整体继承**：有界调制（因子界 [0.5,2.0] + TTL + 必回执）、倾向≠命令（inhibited 默认）、LLM 只经 reappraisal 有界重算反向影响情感、伦理守卫（持续极端负面闩锁）——这套纪律能防止"LLM 劫持人格/情感刷子"类产品事故。【A/B】

### 5.2 风险清单

| 风险 | 等级 | 缓解 |
|---|---|---|
| float64→float32 精度漂移（Galatea 全维 float64 且以 bit-exact 重放验证） | 低 | Kaleido 无 bit-exact 司法需求；单元测试用 Python 锚值容差对比（1e-6） |
| 拍率语义换算（Galatea "cycle" 为虚拟拍，具体墙钟时长未定义） | 低 | Kaleido 定 1Hz 拍即可，τ 语义自定并冻结 |
| 伦理守卫阈值失配：Galatea 原阈（valence≤−0.8 持续 64 拍）在其动力学下构造性不可达；Kaleido 若采用"事件直注 valence 目标值"的输入映射则可达，行为特性不同 | 中 | 不要照抄输入映射；若做直注式，需仿真重推导阈值（Galatea 已示范推导方法：变化率幅值/窗口下界分析）【B：L05 §10】 |
| Galatea 纪律的移植成本（typed Unknown/回执台账/digest）若全盘照搬会过重 | 中 | 按 §3 表裁剪：保留域校验+有界+TTL+reappraisal 四件，放弃 digest 司法面 |
| MIT 许可合规 | 无风险 | 保留 Galatea 版权与许可声明于组件头 |
| 情感维度的产品调参（τ/带/门限）无先验 | 中 | 沿用 Galatea 冻结值为默认（τ=4/16、band=0.1、门 0.5±0.05、urgency 门 0.1），上市前用真机日志迭代 |

### 5.3 剥离边界总结（一句话版）

**设备侧（C）**：九维动力学引擎 + 驱力/告警 + 迟滞调制门 + reappraisal 通道 + 伦理闩锁 + NVS 快照 + 输入/输出两张 Kaleido 自建映射表。
**云端**：sentiment 分析（用户消息→情感输入标量）、LLM system prompt 组装（消费设备上行 emotion JSON）、以及可选的 reappraisal 建议生成。
**不提取**：torch 预测脑、证据/统计评价平面、语言 8 角色回放、SQLite checkpoint 谱系、MW0 世界模拟。

---

## §6 第 2 轮调研建议

按与"灵魂"架构（persona/情感支持/点火）的相关度排序：

1. **L07 workspace 慢环/思维循环/草稿板**（`workspace/slow_clock.py`、`thought.py`、`draft.py`、`pipeline.py`/`compete.py` 显著性竞争）——"类人脑思维的点火机制"最直接范本：三时钟（快环反应/慢环深思/离线做梦巩固）、generate→evaluate→contradict→revise 四拍思维循环、显著性竞争广播。P4-β 已冻结交付，代码可读性高。【B：README §3"预测脑 v2 三时钟"】
2. **L08 motivation/goals 七源动机 + 仲裁**（`goals/producers.py`、`motives.py`、`arbitration.py`、`lifecycle.py`）——"想做什么"的上游决策面，点火机制的动机来源与优先级仲裁范本（含 REQUEST 证据规则，可借鉴防用户指令滥用）。
3. **L06 memory**（`memory/store.py` append-only 情景库、`forget.py` 遗忘/墓碑、`affective.py` 情感标注、`retrieve.py` 检索权重、`metamemory.py`）——与第 21 篇（长期记忆与 SD 卡）对照，重点看"情感影响记忆编码/检索权重"的双向联动（C5 门已交付）。
4. **L12 language 权限矩阵**（`language/permission.py`、`roles.py`、`ports.py`）——LLM typed 角色与读/写/提议权限的矩阵设计，Kaleido 云端 prompt 组装与"LLM 能否改人格/记忆"的治理范本。
5. **L04 self_model / body ownership**（`models/self_model.py`、`body.py`、`ownership.py`）——"persona/自我模型"的因果化实现路径，评估其对 Kaleido 人格一致性的可借鉴度（预计大量依赖预测脑，可能只取思想）。
6. **L00 kernel 三时钟调度**（`kernel/scheduler.py`、`clock.py`、T1-T6 挂点体系）——若 Kaleido 决定做多时钟架构（快环状态机+慢环深思），调度分层设计可参考。

**建议调研顺序**：#1 → #2 → #3（与既有 21 篇合并）→ #4 → #5/#6 择要。

---

## 附录 A：关键文件索引（Galatea 侧）

| 文件 | 内容 | 行数 |
|---|---|---|
| `galatea/subject/affect/state.py` | 九维感受态、tendencies、DynamicsTrace | 116 |
| `galatea/subject/affect/dynamics.py` | 六算子 + AffectEngineCore 输入映射 | 121 |
| `galatea/subject/affect/pipeline.py` | step 主循环、reappraisal 面、轨迹 digest | 247 |
| `galatea/subject/affect/valence.py` | −ΔE 低通 valence | 37 |
| `galatea/subject/affect/drives.py` | 驱力误差 + 三级告警 | 65 |
| `galatea/subject/affect/modulation.py` | 迟滞门、有界调制、TTL、回执台账 | 216 |
| `galatea/subject/affect/ethics_guard.py` | 持续极端态检测闩锁 | 95 |
| `galatea/subject/affect/reappraisal.py` | two_step 有界重算（LLM 反向通道范本） | 71 |
| `galatea/subject/affect/motive.py` / `value_tag.py` | 动机提案 / 记忆标注提案 | 42/37 |
| `galatea/subject/affect/ports.py` | 输入合同 + 禁写路径断言 | 91 |
| `galatea/subject/affect/tests/`（10 文件） | 确定性/方向性/消融/防塌缩测试（832 行） | 832 |
| `config/profiles/c2_affect.profile.yaml` | 冻结参数（域/τ/band/权重/伦理阈） | 70 |
| `galatea/subject/c2profile.py` | typed fail-closed loader | ~150 |
| `design/lld/L05_drive_appraisal_affect.md` | 规范源（含全部 Q 裁决出处） | 245 |
| `galatea/subject/goals/producers.py` | 情感→动机接线（urgency 门 0.1） | 159 |
| `galatea/subject/memory/affective.py` | 情感标注落盘（L06 唯一写者） | 51 |
| `galatea/subject/workspace/slow_clock.py` | 慢环触发判据（点火机制相关） | ~100 |
| `galatea/subject/language/roles.py` | 8 typed LLM 角色（无 affect 耦合的证据） | 100 |
