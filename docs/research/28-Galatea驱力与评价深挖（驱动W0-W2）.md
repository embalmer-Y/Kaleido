# 28 Galatea 驱力与评价深挖（驱动 W0-1 kaleido_drive 与 W2 情感→会话）

| 版本 | 日期 | 变更 | 作者 |
|---|---|---|---|
| v1.0 | 2026-10-28 | 初稿：L05 affect 驱力/评价/情感子系统源码级力学提取（Q1-Q8）+ kaleido_drive API 与 W2 链路设计 + kaleido_emotion 移植偏差审计 | ZCode 子智能体（调研） |

**调研对象**：`/home/emb/project/Galatea`（只读参考，未修改任何文件）
**服务对象**：M2 计划 §5R 的 W0-1（kaleido_drive 驱力层）与 W2（情感→会话）
**证据级别标注**：A = 源码直接证据（文件:行号）；B = 文档/LLD/profile；C = 推断/设计建议。
**与 18 号的关系**：18 号给了架构清单与可移植性结论；本篇给出**可供 C 移植对照的精确力学**（逐算子公式、逐参数数值、逐输入字段语义），并对我方 `kaleido_emotion.c` 已移植代码做逐项偏差审计（§4.3、§9.5）。

---

## §0 结论先行（八条，均有 A 级证据）

1. **驱力只有 3 个且从未扩展**：energy/integrity/load（MW0 最小集），其中 load ≡ 0 纯 schema 占位——**实际活跃驱力只有 2 个**。Kaleido 的三驱力（social/stim/rest）按 Galatea 纪律已属"扩展集合"，必须立自己的冻结 profile。
2. **Galatea 驱力水平没有时间动力学**：energy/integrity 只被**离散事件**改变（动作成本/风暴损害/进食休息恢复），**不存在被动泄漏或向 set-point 回归**。W0-1 计划中"水平随时间向 setpoint 回归"的表述与源模式不符，应改为"每拍常数代谢 + 事件恢复"（§2.1、§9.2）。
3. **驱力误差聚合是"求和 + 取最大"，没有 mean**：valence 吃 `E = Σ w_d·e_d`（无归一，域 [−2,2]）；arousal/urgency 吃 `e_max`。我方 `ke_inputs.drive_error_mean`（/n）是移植发明，导致 valence 幅值被压到源的 1/3、永不饱和——这是 ke 伦理阈被迫重定为 −0.20 的根源（§4.3 偏差 D1）。
4. **appraisal 不是独立环节**：C2 中不存在"ReceivedSignal → 九维评分函数"；所谓评价 = `dynamics.py:77-88` docstring 钉死的**固定来源表**（每维一个来源、零自由增益），`AppraisalEvent` 只是 step 末尾的**审计记录**（引用 state 现值 + 当拍 raw valence）。W2 要做的"会话事件→评价"映射表在 Galatea 中没有对应物，属 Kaleido 增量（§3）。
5. **controllability 与驱力误差互不耦合**：回执→controllability 是独立低通（τ_dec=16，事件拍才更新）；没有任何"失控感加深沮丧"的跨维反馈。九维之间唯一耦合路径是 persistence ← |valence|（§4.1、§6）。
6. **伦理触发在源动力学下构造性不可达**：valence 是有界误差的变化率 ⇒ 64 拍窗 raw 均值 ≥ −E_range/64，持续 ≤ −0.8 不可能——守卫"零误触发"由构造保证。Kaleido 改用事件直注误差后可达性改变，我方 −0.20/4 拍的替代阈**必须**仿真重推导，且当前闩锁无解锁路径（§4.3 D3）。
7. **我方 `ke_hysteresis` 带宽语义与源差一倍**：源 `band=0.1` 指半带宽（ON ≥ 0.55 / OFF ≤ 0.45）；ke 实现为 ±band（0.6/0.4）。当前 ke 未调用该算子（潜伏缺陷），但导出 API 已语义漂移（§4.3 D6）。
8. **重评价（reappraisal）源语义是一次性精确 delta**：`delta = two_step(original) − two_step(revised)` **立即**加到 valence 积分器（构造式锚 0.0875）；我方 TTL 级联低通斜坡的**实际施加量 ≠ 请求 delta**（增益 <1 且依赖 ttl），回执也未携带 applied 值（§4.3 D4）。

---

## §1 Q1 驱力集合：有哪些、语义、是否扩展过

### 1.1 集合定义

【A：ports.py:23-28】

```python
class DriveKind(enum.StrEnum):
    ENERGY = "energy"
    INTEGRITY = "integrity"
    LOAD = "load"          # Q-L01-09⑥ load≡0（schema 保留不生效）
```

- 注释原文："MW0 驱力最小集（L05 §4.5）"。【A：ports.py:24】
- L05 规范："驱力集合 MW0 期最小集：ENERGY、INTEGRITY、LOAD；**后续可扩展但须新 profile**。"【B：L05_drive_appraisal_affect.md:125】
- **扩展史：没有**。检索 `config/profiles/` 全部 21 个 profile，c2 之后（c3/c4/c5/p4）无任何新增 DriveKind；`social_relevance` 是九维**维度**不是驱力，MW0 期恒 0 + Unknown，C4 后由 L12 typed 语言通道供线索【A：pipeline.py:164-169；B：L05:244 Q-L05-08】。

### 1.2 set-point 与语义

【A：c2_affect.profile.yaml:23-32；A：ports.py:83-88】

| 驱力 | set-point | 语义 | 活跃性 |
|---|---|---|---|
| energy | 1.0（= world.body.energy_max） | 电源余量比（设备侧：`energy = 1 − used/max(budget,1)`）【A：env/tessera_bridge.py:64-68】 | 活跃 |
| integrity | 1.0（= integrity_max） | 完好度（风暴损害递减，M0 无再生）【A：env/world/mw0.py:223-224】 | 活跃 |
| load | 0.0 | 负载；**恒 0**，非 0 构造即拒 | 占位 |

- "set-point = 冻结满值（m0_mw0 派生；**初始态零误差**）"【A：yaml:26-29】——初始 level = set-point ⇒ 初始无误差、valence 无输入。
- 权重 w_d = 1:1:1（Q-L09-08 零参数先例），`weight_source: profile_frozen` 禁在线自改；`appraisal_adjustment_c2: 0.0`（防价值自我放大）【A：yaml:25,31-32；A：c2profile.py:84-87 强制校验】。

### 1.3 对 Kaleido 的直接推论

Galatea 的驱力语义是"**身体资源余量**"（电量/完好度），不是"心理需求"。social/stim/rest 三驱力是 Kaleido 自造集合（C 级推断的合法扩展，18 号 §3.1 已论证），按源纪律需要：(a) 自己的冻结参数表；(b) 每驱力明确的"资源"解释（§9.2 给出）。三驱力数量 > MW0 的 2 个活跃驱力，属于首批真实扩展——这正是本组件存在的意义，但要自觉"零无源自由常量"纪律：**每个参数写明锚**。

---

## §2 Q2 驱力动力学：精确方程

### 2.1 驱力**水平**的演化（世界侧，Galatea 称 body 状态）

关键事实：**MW0 世界没有被动能量泄漏**。energy 只在动作结算时变化【A：env/world/mw0.py:199-217；B：m0_mw0.profile.yaml:23-28】：

```text
eat   : energy += 0.15   （前置：stored_food ≥ 1，即"有输入才有恢复"）
rest  : energy += 0.10
move  : energy -= 0.01
forage: energy -= 0.02
look  : energy -= 0.005
钳位：energy = clamp(energy, 0, 1.0)          # floor_semantics.clamp_at_zero
integrity：风暴命中 -0.10/次，无再生（integrity_regen: none）
load    ≡ 0
```

- `_spend_energy` / `_gain_energy` 实现（clamp 0 / clamp max）【A：mw0.py:208-217】。
- **没有任何"每拍 −x"的代谢项**——energy 的消耗完全由动作承载。"活着就有消耗"在 MW0 里被建模为"活着就要动作（最低限度 look −0.005）"。
- **对 W0-1 的修正意义**：桌面伴侣不存在"动作选择"自由度，它的"活着"= 时间流逝本身。因此 Galatea-faithful 的时间化翻译是：**每拍恒定代谢消耗（≈ look 成本的时间化）+ 交互事件恢复（≈ eat/rest）**。计划 §5R.1 "水平随时间向 setpoint 回归"的表述应改为该模型——若按"向 setpoint 回归"实现（level 越闲越高），social 驱力会在无人时自愈，孤独语义反转（C 级推断，证据为上式缺省无回归项）。

### 2.2 驱力**误差**的定义（affect 侧）

【A：dynamics.py:93-111】

```python
e_d = set_points[kind] - v          # 无 max(0,·) 钳位！
# 缺席（None）→ errors[kind] = prev.get(kind, 0.0) + Unknown(MISSING_SIGNAL)
```

- **单侧性由值域构造保证，不是公式钳位**：body 值被 ports 构造面强制 ∈ [0,1]、load ∈ {None, 0.0}【A：ports.py:83-88】⇒ e_energy/e_integrity ≥ 0、e_load ≡ 0。`HomeostaticAlert` 另有 `error ≥ 0` 校验【A：drives.py:63-65】。
- **缺席语义**：保持上一有效值 + typed Unknown（禁编造），初值 0.0【A：dynamics.py:96-108】。

### 2.3 聚合：sum 与 max，没有 mean

【A：pipeline.py:102-103；A：dynamics.py:66】

```python
weighted_e = sum(p.drive_weights[k] * errors[k] for k in errors)   # E = Σ w·e，字面无归一
e_max      = max(errors.values())                                   # arousal/urgency 输入
```

- raw valence 域由此为 [−2, 2]（energy+integrity 同时全幅翻转；load≡0），`AppraisalEvent` 构造面显式断言 `-2.0 ≤ valence_shift ≤ 2.0`【A：appraisal.py:31-34】，发布面再 clip [−1,1]。
- **全代码库无驱力 mean 聚合**。唯一的 mean 是上游喂入的 `sensory_residual_mean`（L02 窗均值）与 `belief_uncertainty_mean`（L03 1−p_max 均值）——C2 冻结树中这两者**没有生产者**（仅 evidence 实验装置供给；affect 侧因此才有"缺源保持"分支，pipeline.py:143-162）。
- 供 W3 的告警分级：`severity_of(e, band)`：`e ≥ 0.8 → CRITICAL；e ≥ 0.5 → HIGH；e > band(0.1) → LOW；否则 None`【A：drives.py:28-40】；告警携带 `trend = e_now − e_prev`【A：pipeline.py:105-114】。下游映射：HIGH/CRITICAL → urgency 1.0，LOW → 0.5【A：goals/producers.py:75】。

---

## §3 Q3 appraisal 与情感的边界：appraisal 在哪、信号怎么映射

### 3.1 结论：appraisal 是 affect 内部的记账面，不是独立环节

- L05 §2.2 设计上确有 `aff.appraisal` 输出端口（`AppraisalEvent(target_ref, dims{}, valence_shift, controllability_est)`，带证据引用）【B：L05:45】。
- C2 实现中它由 `pipeline.step()` **末尾**一次构造：novelty/uncertainty/controllability 三维取自**当拍更新后的 state 现值**，valence_shift = 当拍 raw，target = `cycle.{n}`（当拍本身），evidence = body 读数引用【A：pipeline.py:189-197】。`build_appraisal` 是纯记录构造器【A：appraisal.py:39-58】。
- **不存在**"ReceivedSignal → 维度"的评分函数/增益表。真实映射是 `AffectEngineCore` docstring 钉死的**固定来源表**（"全部机械钉死、零自由常量"）【A：dynamics.py:74-88】：

| 维 | 来源（低通输入 x） | τ | 符号/增益 |
|---|---|---|---|
| valence | raw = −(E_t − E_{t−1}) | τ_int=4 | 负差分；发布 clip [−1,1] |
| arousal | e_max | τ_int=4 | +1（域天然 [0,1]） |
| urgency | max(0, (e_max − band)/(1 − band)) | τ_int=4 | band=0.1 上方线性归一 |
| persistence | min(1, \|valence_published\| / 0.8) | τ_dec=16 | 0.8 = 伦理阈归一 |
| controllability | 1{action_receipt_success}（仅事件拍） | τ_dec=16 | 指示函数 |
| novelty | sensory_residual_mean（=mean\|residual\|） | τ_int=4 | +1 |
| uncertainty | belief_uncertainty_mean（=mean(1−p_max)） | τ_int=4 | +1 |
| social_relevance | 恒 0.0 + Unknown(CAPABILITY_LIMIT) | — | D-14 |

### 3.2 一次事件的完整链路（以"eat 恢复 0.15"为例）【A：pipeline.py:89-219】

```text
gateway 授权命令 → world.apply 结算（requested 与 effective 分开记录；
  sham/coercion/no_op 干预在此显形）【A：env/world/dynamics.py:124-166】
  ├─→ EffectFact → 执行回执台账 → 下拍 inputs.action_receipt_success=True
  │     └→ controllability 低通向 1（τ_dec=16）                    [事件通道]
  └─→ world energy +0.15 → interoceptive 通道（delay 0）→ sensorium 形成
        （INTERO 连续特征可被增益调制 ×factor 后 clip [0,1]）
        【A：sensorium/gain.py:55-99】→ build_body_belief（模态白名单强制，
        非 INTERO 帧即拒）【A：models/body.py:115-152】
        └→ 下拍 AffectCycleInputs.body_energy ↑ → e_energy 骤降 0.15
              ├→ E 下降 → raw valence = +0.15 尖峰 → 低通 τ=4 → published 正脉冲
              ├→ e_max 下降 → arousal/urgency 缓慢回落
              └→ appraisal 记录当拍（novelty/unc/ctl 现值 + valence_shift=+0.15）
```

注意三点：
1. **脉冲到 valence 是"差分"不是"水平"**——误差稳定在高位时 valence → 0（"误差大但稳定 → valence → 0"有测试钉死）【A：tests/test_valence_direction.py:41-49】。改善的**速率**才产生正效价。
2. **效果在一拍内到达**（interoceptive delay_cycles = 0）【B：m0_mw0.profile.yaml:65】；exteroceptive/outcome 通道 delay 1 拍。
3. `sensory_residual_mean / belief_uncertainty_mean` 在生产树无供给者 ⇒ C2 实跑中 novelty/uncertainty 实际恒保持初值 0（全零输入协议下）——Galatea 自己也还没把"世界意外"接进情感。

---

## §4 Q4 驱力误差 → 情感的精确耦合 + 我方移植偏差审计

### 4.1 源耦合公式（逐行）

【A：valence.py:32-37；A：pipeline.py:117-124】

```python
# valence（内稳态价值，非 reward——无累积量/折扣/最大化算子，结构即证明）
raw       = -(error_now - error_prev)          # error = E = Σ w·e（求和！）
smoothed  = lowpass(prev_smoothed, raw, 4)     # s ← s + (x−s)/τ
published, hit = saturate(smoothed, -1.0, 1.0) # clip + 饱和计数

# E 的历史推进是无条件的（缺席拍因误差保持 ⇒ raw 自然 = 0）
self._prev_weighted_e = weighted_e             # pipeline.py:208，每拍执行

# arousal / urgency（注意：吃 e_max，不吃 E）
arousal  = saturate(lowpass(arousal,  e_max, 4), 0, 1)
urgency  = saturate(lowpass(urgency, max(0,(e_max-0.1)/0.9), 4), 0, 1)

# persistence（唯一跨维耦合：吃 |published valence|）
persistence = saturate(lowpass(persistence, min(1,|v_pub|/0.8), 16), 0, 1)
```

手算锚（测试钉死，可作 C 单测黄金值）【A：tests/test_dynamics_identifiability.py；A：tests/test_valence_direction.py】：
- 阶跃 x=1、τ=4：s₃=0.578125，s₄=0.68359375（t63=4 拍），24 拍 0.99899661；
- 衰减 τ=16：(15/16)^16 = 0.35607413 逐位；
- two_step(0.4)=0.175、two_step(0.2)=0.0875；
- 恒定误差 0.5：40 拍后 valence≈0（残余 ~0.25·0.75³⁹）、arousal≈0.5。

### 4.2 耦合的边界（明确"没有"什么）

- 驱力误差**不**直接进 controllability/uncertainty/novelty（后者各有独立来源，§3.1 表）。
- controllability/urgency **不**反馈 valence（无"习得性无助"回路）。
- valence → persistence 是九维间**唯一**跨维耦合边。
- 误差的**水平**只表达在 arousal/urgency；valence 只表达**变化率**（§3.2 注意点 1）。

### 4.3 我方 kaleido_emotion.c 移植偏差审计（逐项，含修复建议）

对照对象：`/home/emb/project/Kaleido/kaleido-fw/components/kaleido_emotion/src/kaleido_emotion.c`。

| # | 项 | 源语义（证据） | ke 现状 | 判定 | 建议 |
|---|---|---|---|---|---|
| D1 | 驱力误差聚合 | `E = Σ w·e` 无归一，raw 域 [−2,2]（pipeline.py:102；appraisal.py:31-34） | `drive_error_mean = Σw·e/n`（kaleido_emotion.h:50），raw 域 [−2/3,2/3] | **偏差（影响面最大）** | kaleido_drive 输出 `error_sum`；ke 字段语义改 sum（或改名）。否则 valence 永不饱和、18 号引用的"饱和计数"动力学面失效 |
| D2 | 伦理阈 −0.20 的依据 | 源阈 −0.8 锚"域边缘 20%"（yaml:59），且在 sum 语义下构造性不可达（test_ethics_trigger.py 模块 docstring） | −0.20/4 拍为"可达域"经验值（kaleido_emotion.c:24-26 注释自认待裁决） | 偏差衍生 | D1 修复后按 sum 语义重推导（§4.1 手算锚：sum 下单驱力全幅翻转首拍 dip = −0.25，两驱力 = −0.5）→ 建议 P0 阶用仿真扫阈 |
| D3 | 伦理闩锁结构 | 双条件 `valence ≤ −0.8 ∧ persistence ≥ 0.8` 连续 ≥64 拍，闩锁一次，恢复须 owner 批准（ethics_guard.py:61-88） | 单条件 valence ≤ −0.20 连续 4 拍即闩锁，**无解锁 API**（ke_reset 全清除外），无 persistence 合取 | **偏差** | 至少补 `ke_ethics_clear()`（对应"owner 批准"语义，可由云端确认/长正交互后调用）；是否恢复 persistence 合取随 D2 一起裁决 |
| D4 | 重评价施加 | `delta = two_step(orig) − two_step(rev)` 一次性加到积分器；回执携带 delta（reappraisal.py:48-66） | delta 钳 ±0.2 后经 TTL 级联低通斜坡施加；实际施加量 < 请求值且依赖 ttl；回执只有 done 布尔（kaleido_emotion.c:60-78,108-115） | **偏差** | 改一次性施加（保持 ±0.2 钳位即可，钳位替代了源"两输入差分"的有界性）；回执记录 applied delta 供 SOUL 遥测审计 |
| D5 | 缺席驱力的保持 | 逐驱力保持上一有效值 ⇒ e_max 保持 ⇒ arousal/urgency 积分器**保持**（dynamics.py:96-108） | `!have_drive` 时 e_max=0 ⇒ arousal/urgency 向 0 **衰减**（kaleido_emotion.c:122,133） | **偏差** | `!have_drive` 时跳过 arousal/urgency 更新（hold）。kaleido_drive 常供时不触发，属防御性修正 |
| D6 | 迟滞算子带宽 | `band` = **半带宽**：ON ≥ 0.5+band/2、OFF ≤ 0.5−band/2；band=0.1 ⇒ 0.55/0.45，边界含（dynamics.py:34-40；test_dynamics_identifiability.py:35-59） | `ke_hysteresis` 用 ±band：>0.6 / <0.4，边界不含（kaleido_emotion.c:51-58） | **偏差（潜伏，当前无调用点）** | 改为 `band/2` 或文档声明参数为半宽。注意 W1 表达映射若复用此算子会得到 2 倍宽迟滞 |
| D7 | novelty/uncertainty 缺席语义 | None → 保持 + Unknown（pipeline.py:143-162） | float 直喂，喂 0 即主动衰减向 0（kaleido_emotion.c:149-154） | 契约缺口 | ke_inputs 加 `have_residual/have_uncertainty` 位，或在头文件钉死"调用方必须自行保持上次值"契约 |
| D8 | urgency 门迟滞 | 纯比较 `urgency > 0.1`（producers.py:44,83），无迟滞 | ON 0.10 / OFF 0.05 自造迟滞（kaleido_emotion.c:22-23,130-131） | 有意附加 | 保留（防点火抖动合理），注释标注"Kaleido 附加，非源语义" |
| D9 | social_bond 维 | social_relevance 恒 0+Unknown（D-14） | 事件 +0.125 / 衰减 τ=1024 拍 | 有意替换（18 号已决策） | KE_BOND_DECAY=1024 无冻结来源，与 τ 谱（4/16）差 64 倍——参数入 kd/ke 冻结表并给产品理由（1Hz 下 1024 拍 ≈ 17 分钟衰减时间常数，"冷得慢"符合陪伴语义即可） |
| D10 | tendencies 维 | 九维之一（affordance 查表生成，inhibited 默认 True） | **缺失**（ke_state 无该字段） | 缺失 | 见 §5.2——W3 ignition 恰好需要这个面，建议随 W0-1 一并立类型 |
| — | valence 差分/低通、arousal 公式、urgency 归一、persistence 归一、controllability 三态回执、two_step、低通算子、τ=4/16、band=0.1 | 见 §4.1 | 逐项一致（A1/G1/G2 返工项与源吻合） | **一致** | — |

---

## §5 Q5 行动倾向（tendencies）：生成、抑制与 Kaleido 借用

### 5.1 源机制

- 类型：`Tendency(action_class, strength, inhibited=True)`；枚举 = MW0 五原语 `{move, forage, eat, rest, look}`（引用 L10 冻结合同不自造）；`inhibited` **默认 True**（不变量 5：倾向 ≠ 命令）【A：state.py:25-45；B：L05:85,183】。
- 生成：纯查表——`tendency_set(affordance_strengths)`：affordance 置信度直接作 strength；缺源时 move 恒 1.0（无前置）、其余 0.0；inhibited 恒 True【A：dynamics.py:114-121】。
- 抑制/释放：C2 期**没有释放路径**——tendencies 只进 state 发布面；释放要经 L09 仲裁 = C3 接线【A：state.py:7-8】。测试钉死：tendencies 与 valence **解耦**（affordance 变化不改效价；效价不影响倾向）【A：tests/test_anti_reward_collapse.py:77-90】。
- 类型隔离：`AffectMotiveProposal` 无 goal_id/commitment/deadline/intention 字段（构造面断言）【A：tests/test_affect_not_command.py:43-56】；affect 包 import 图禁入 action/goals/workspace 平面【A：tests/test_affect_not_command.py:14-28】。
- 下游两处消费：
  1. **动机提案**：`AffectiveMotiveProposal(urgency=state.urgency, evidence_refs, state_digest)`——urgency 原样传递不新造标量；门 = `urgency > 0.1` 才活跃（`_AFFECTIVE_ACTIVE_GATE = 0.1`），且**只是七源动机之一，不得独占**【A：motive.py:32-42；A：goals/producers.py:43-44,81-89,15-16】。
  2. **动机 urgency 映射**：VIABILITY 告警 HIGH/CRITICAL → urgency 1.0、LOW → 0.5【A：producers.py:75】。

### 5.2 Kaleido 借用设计（喂 W3 ignition）

把"对外动作候选"建成 Galatea 同构的**提案类型**，Kaleido 五原语建议：`{speak, play, approach, soothe, rest}`：

```c
struct kd_tendency {                 /* 或 ke_tendency，随 W0-1 立类型 */
        const char *action_class;    /* "speak"/"play"/...          */
        float strength;              /* [0,1] = 来源供给强度        */
        bool inhibited;              /* 默认 true；仅 ignition 仲裁解锁 */
};
```

- 强度来源建议（对应源"affordance 置信"）：speak ← social 驱力告警级（LOW→0.5、HIGH/CRITICAL→1.0，producers.py:75 口径）；play ← stim 驱力；approach ← presence（人声活动）；soothe ← ethics_latched 时 1.0；rest ← 反向（活跃时长）。
- inhibited=true 保证情感层只能"提议"；ki 五因子仲裁是唯一解锁点——与 §5R 架构（ignition_candidate → ki 裁决 → 主动动作）完全同构，等于给 W3 的"情感动机"因子一个正式类型，而不只是一个布尔。

---

## §6 Q6 回执（receipt）与 controllability 的精确耦合

【A：ports.py:63-64,75；A：pipeline.py:130-140；A：env/world/dynamics.py:8-9,124-166】

```text
输入：action_receipt_success: bool | None
       True/False = 本拍有归属结果（成功/失败）；None = 本拍无事件（保持）
公式：if not None:  s_ctl ← lowpass(s_ctl, 1{success}, τ_dec=16)
语义：requested==effective 的归属统计代理（Q-L08-09⑩ 同源）；C3 后由 L08 结算面接管
```

- **世界侧保证 requested/effective 分离**：`no_op`（requested 记录但 effective="none"）、`sham`（报告成功但世界未变）、`coercion`（effective 带 `coerced:` 前缀 = 外因标记）【A：dynamics.py:134-159】——"成功"的定义是**归属后的**结果，不是"命令发出了"。
- 首个回执之前：`Unknown(INSUFFICIENT_OBSERVATION)`——controllability 不可估计【A：pipeline.py:135-140】。
- 成功/失败**对称**拉向 1/0，τ_dec=16 慢谱（归属是统计量，不随单事件跳变）。
- **没有** controllability → 其他维的反馈（§4.2）；它只进 state 发布面与 appraisal 记录，供上游读者（L07 竞争/L08 仲裁）自行使用。
- 我方 G1 三态移植（KE_RECEIPT_NONE/SUCCESS/FAIL，事件拍才低通，τ_dec）与源**逐位一致**【A：kaleido_emotion.c:141-147】。
- Kaleido 侧对接（§5R.3.4 已规划，确认口径）：轮 rc=0 → SUCCESS；ASR/LLM/TTS 失败路径 → FAIL；**轮尚未结束的所有拍 → NONE**（一整轮对话只在结算拍产一次回执，与源"事件驱动归属统计"语义一致；不要每拍都喂 SUCCESS，否则 controllability 失去事件性）。

---

## §7 Q7 AffectCycleInputs 全字段清单 vs 我方 ke_inputs

【A：ports.py:56-92】

| # | 字段 | 类型/域 | 缺省语义 | → 消费 | 我方 ke_inputs | 缺失损失 |
|---|---|---|---|---|---|---|
| 1 | cycle | int | — | 审计/id 派生 | tick ✓ | 无 |
| 2 | body_energy | float \| None ∈[0,1] | None=保持+Unknown | 驱力误差（逐驱力） | have_drive + error_mean/max **预聚合** | 逐驱力告警/趋势（W3 VIABILITY 因子输入）|
| 3 | body_integrity | 同上 | 同上 | 同上 | （并入聚合） | 同上 |
| 4 | body_load | 0.0 \| None | 恒 0，非 0 即拒 | 恒 0 误差 | 无（不需要） | 无 |
| 5 | telemetry_flagged | bool | False | True 构造即拒（S1 留痕） | 无需（C 无反射面） | 无 |
| 6 | sensory_residual_mean | float \| None | None=保持+Unknown | novelty | sensory_residual_mean（无有效性位） | D7 契约缺口 |
| 7 | belief_uncertainty_mean | float \| None | 同上 | uncertainty | belief_uncertainty_mean（同上） | 同上 |
| 8 | action_receipt_success | bool \| None | None=保持 | controllability | receipt 三态 ✓ | 无 |
| 9 | affordance_strengths | tuple[(str,float)] | ()=空集 | tendencies | **无** | 行动倾向提案面（§5.2，W3 需要） |

**不在 AffectCycleInputs 内的合法入口**：
- `ReappraisalRequest`：带外方法 `handle_reappraisal()`（pipeline.py:222-229），不占每拍输入；
- L05 §2.1 定义但 **C2 未接线**的端口：`aff.goal_progress`（L08 目标进展）、`aff.settlement`（L10 结算）、`aff.regime_context`（regime 感知投影）【B：L05:30-36】——字段面不存在，是 C3+ 扩展位。Kaleido 对应物：W4 记忆钩子/日程事件未来可走类似扩展位（设计上预留 union/版本字段即可，现在不建）。

**结论**：ke_inputs 覆盖了 C2 全部**有效**输入（9 字段中缺 affordance/tendencies 与逐驱力粒度）；C2 树中真正无供给的 novelty/uncertainty 源在我方由云端 sentiment/环境事件补位（W2 映射，§9.4）——这是 Kaleido 对 Galatea "空位" 的第一性扩展，注意保留有效性位语义（D7）。

---

## §8 Q8 冻结参数总表

### 8.1 c2_affect.profile.yaml（affect 域）【B：全文，v1.0 frozen 2026-10-06】

| 参数 | 值 | 语义 | 锚 |
|---|---|---|---|
| domains.valence | [−1,1] 对称 | 唯一带符号维 | Q-L05-01 |
| domains.other_dims | [0,1] | 其余八维（含 tendencies 强度） | Q-L05-01 |
| numeric_type | float64 | 全维精度 | Q-T10 |
| cross_dim_aggregation | forbidden | 禁跨维聚合（无 summary/score/reward API） | Q-L05-01③ |
| drives.set | [energy, integrity, load] | MW0 最小集 | L05 §4.5 |
| drives.weights | 1.0 : 1.0 : 1.0 | 等权 | Q-L09-08 |
| drives.set_points | 1.0 / 1.0 / 0.0 | 冻结满值；初始态零误差 | m0_mw0 |
| drives.error_definition | set_point_minus_value | 单侧 ≥0（值域构造保证） | Q-L05-02② |
| dynamics.tau_integrator_cycles | 4 | 快谱 τ（T3 同拍） | Q-L00-01 |
| dynamics.tau_decay_cycles | 16 | 慢谱 τ（T4 拍） | Q-L00-01 |
| dynamics.hysteresis_band | 0.1 | **半带宽**（ON 0.55/OFF 0.45）= 2×render_noise_sigma(0.05) | Q-L05-03③ |
| dynamics.saturation | domain_clip | clip + saturation_hits 计数 | — |
| dynamics.recovery_phase | error_within_band_of_set_point | e ≤ band 即恢复 | Q-L05-03⑤ |
| dynamics.rng_operators | none_c2 | 零 RNG 零墙钟 | Q-G00-10 |
| valence.formula | negative_weighted_error_rate_of_change | −d/dt Σw·e 有界平滑 | §4.2 |
| valence.input | body_belief_only | 禁 telemetry 直填 | 不变量 2 |
| valence.not_reward | true | 无 reward 结构面 | HLD §11.1 |
| modulation 因子界 | [0.5, 2.0] | 越界目标侧裁剪 + CLIPPED | Q-L02-02① |
| modulation.believe_threshold 界 | [0.4, 0.6]（=0.5±band） | 保守方向提高承诺阈 | Q-L03-02 |
| modulation TTL | 4 cycles | 到期归零，禁常驻 | Q-L07-04②（2×N2） |
| modulation 回执 | required | TTL 内无回执 = 未发生 | L05 §6 行 3 |
| 调制门 center | 0.5（band 同上） | 三门迟滞（arousal/uncertainty/novelty） | ADR 层A |
| ethics.valence_le | −0.8 | 域边缘 20% 分位 | Q-L05-05 |
| ethics.persistence_ge | 0.8 | 同上（**双条件**） | Q-L05-05 |
| ethics.sustained_cycles_ge | 64 | T6 拍 | Q-L00-01 |
| ethics.action | pause + 人工评审 | 触发即暂停 claim-relevant | Q-G00-05 |

### 8.2 相邻冻结常数（affect 直接消费/下游映射）【A 级】

| 参数 | 值 | 出处 |
|---|---|---|
| 告警 HIGH / CRITICAL | 0.5 / 0.8（LOW > band） | drives.py:28-29 |
| AFFECTIVE 动机门 | urgency > 0.1 | producers.py:44 |
| CURIOSITY 门 | 窗不确定性 > 0.6 | producers.py:46 |
| 慢环触发 | urgency ≤ 0（空闲）或 arousal < 0.1（低唤起） | workspace/slow_clock.py docstring |
| two_step 构造式锚 | two_step(0.4)=0.175 / two_step(0.2)=0.0875 | reappraisal.py + ADR 层A |
| 低通/衰减锚 | s₄(τ=4)=0.68359375；(15/16)¹⁶=0.35607413 | test_dynamics_identifiability.py |
| valence raw 域 | [−2, 2]（发布再 clip [−1,1]） | appraisal.py:31-34 |

### 8.3 世界侧（驱力水平）m0_mw0 关联参数【B：m0_mw0.profile.yaml】

action_energy_delta：move −0.01 / forage −0.02 / **eat +0.15** / **rest +0.10** / look −0.005；storm integrity −0.10/次；clamp [0,1]；无再生；load≡0。→ 这组是 Kaleido 三驱力**事件脉冲幅度**的天然锚（§9.2）。

---

## §9 Kaleido 落地建议

### 9.1 kaleido_drive 组件推荐 API（W0-1）

风格照抄 ke/ki：tab 缩进、零分配零 RNG、纯函数 + 文件级 static 状态、单写者（W0-3 soul limb 持有）。

```c
/*
 * kaleido_drive.h — 三驱力稳态层（social/stim/rest；Galatea L05 drives 的
 * Kaleido 扩展集）。模型 = Galatea MW0 的时间化翻译：
 *   水平只被两类项改变——每拍常数代谢（"活着 = look −0.005"的时间化）
 *   与事件脉冲恢复（"eat +0.15 / rest +0.10"的交互化）。
 * 无被动回归 setpoint、无泄漏项——与源模式一致（28号 §2.1）。
 * 误差 e_d = 1 − level（单侧 ≥0 由 [0,1] 值域构造保证，公式不加钳位）。
 * 拍语义：1Hz，与 ke_step 同拍（soul limb 内先 kd_step 后 ke_step）。
 */
#ifndef KALEIDO_DRIVE_H
#define KALEIDO_DRIVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum kd_drive {
	KD_SOCIAL = 0,   /* 社交联结：互动恢复 / 无人时代谢消耗        */
	KD_STIM,         /* 刺激：新事件恢复 / 单调环境代谢            */
	KD_REST,         /* 休憩：静息·充电恢复 / 活跃表达代谢         */
	KD_N
};

/* 事件脉冲（等效 action_energy_delta；幅度见 kd_params 冻结表） */
enum kd_event {
	KD_EVT_NONE = 0,
	KD_EVT_ROUND_OK,     /* 一轮对话圆满（rc=0）→ social 大恢复 */
	KD_EVT_ROUND_FAIL,   /* 轮失败（ASR/LLM/TTS 错）→ 仅小恢复  */
	KD_EVT_BARGE,        /* 打断：强社交在场信号                */
	KD_EVT_VOICE,        /* 人声活动拍：小恢复 + 在场证据       */
	KD_EVT_TOUCH,        /* 触摸/摇杆：小恢复                   */
};

/* 告警级（Galatea drives.severity_of 同构） */
enum kd_sev {
	KD_SEV_NONE = 0,  /* e ≤ band(0.1)          */
	KD_SEV_LOW,       /* e > 0.1                */
	KD_SEV_HIGH,      /* e ≥ 0.5                */
	KD_SEV_CRITICAL,  /* e ≥ 0.8                */
};

struct kd_params {           /* 冻结表（static const 落 kd_params.c） */
	float metabolic[KD_N];   /* 每拍水平消耗 ≥0（1Hz 拍）             */
	float pulse[6];          /* 按 kd_event 的 social 恢复幅度        */
	float stim_event;        /* ROUND_OK/BARGE/VOICE 对 stim 的恢复   */
	float rest_recover;      /* 静息拍 rest 恢复幅度                  */
	float band;              /* 0.1（恢复正常带，锚 Q-L05-03③）      */
};

struct kd_inputs {
	uint64_t tick;
	bool active;             /* 本拍在表达（TTS/动画播放中）→ rest 代谢 */
	bool quiet;              /* 本拍静息（idle 且无播放）→ rest 恢复   */
	bool charging;           /* 充电中 → rest 恢复（叠加）              */
	enum kd_event event;     /* 本拍交互事件（至多一个；多事件分拍喂） */
};

struct kd_output {
	float level[KD_N];       /* [0,1] 水平                              */
	float error[KD_N];       /* e_d = 1 − level（单侧）                 */
	float error_sum;         /* E = Σ e_d ——喂 ke valence（**sum 非 mean**，D1）*/
	float error_max;         /* max e_d ——喂 ke arousal/urgency        */
	enum kd_sev sev[KD_N];   /* 逐驱力告警级（→W3 VIABILITY 因子）      */
	float trend[KD_N];       /* e_t − e_{t−1}（正=恶化，随告警上遥测）  */
};

void kd_init(const struct kd_params *p);   /* NULL = 用内置冻结表 */
void kd_reset(void);                       /* level 全 1.0（初始态零误差语义） */
void kd_step(uint64_t tick, const struct kd_inputs *in,
	     struct kd_output *out);

#endif /* KALEIDO_DRIVE_H */
```

实现要点（全部有源可依）：
1. 水平更新：`level ← clamp01(level − metabolic[d] + inflow)`；inflow 仅由 event/quiet/charging 产生。**没有**回归项。
2. `error_sum` 直接求和不过均值；ke 侧把 `drive_error_mean` 字段**语义改为 sum**（D1 修复，字段名可一并改 `drive_error_sum`）。
3. 告警/trend 纯阈值与差分，照抄 `severity_of`（drives.py:32-40）。
4. 状态 = `level[KD_N]` + `prev_error[KD_N]` 两个 float 数组，~40B，NVS 快照与 ke 同批。
5. Unity 测试串入 tools/hil/soul-test：用 §4.1 手算锚 +Galatea `test_valence_direction.py` 的协议（步进误差 → valence 固定点 −0.0057142857）做跨组件对拍。

### 9.2 三驱力剪裁与参数锚定建议（C 级——产品调参，非冻结引用）

| 驱力 | "资源"解释 | 恢复事件（脉冲锚） | 代谢条件 | 起步值建议 |
|---|---|---|---|---|
| social | "陪伴联结电量" | ROUND_OK +0.15（=eat）；BARGE +0.05；TOUCH +0.05；VOICE +0.005/拍（=rest 的 1/20）；ROUND_FAIL +0.03（互动发生但体验差） | 每拍 −kd_metabolic（无人时时钟即"活着"） | metabolic=2e−4/拍（满→HIGH 约 42min） |
| stim | "趣味电量" | ROUND_OK/BARGE +0.10；环境事件（W2 云端 surprise 标量）直喂 novelty 通道 | 每拍 −metabolic | metabolic=1e−4（约 1.4h，比 social 慢——独处发呆可接受） |
| rest | "体力" | quiet/charging 拍 +rest_recover(0.01) | active（TTS/动画中）拍 −metabolic | metabolic=3e−4（连续表达 ~1h 显疲态，anim 映射打哈欠/yawn_20s 片段） |

- set-point 一律 = 1.0（冻结满值语义）；上电 kd_reset 后 level 全 1.0 ⇒ 初始零误差 ⇒ valence 静默——与源"初始态零误差"一致（yaml:26 注释）。
- **免扰动原则**（源不变量 2 的产品化）：驱力输入只进 kd（kd→ke），任何云侧/LLM 数值不得绕过 kd 直写 ke 的 drive_error（对应"禁 telemetry 直填"）。

### 9.3 每拍输入源映射表（固件现有事件 → kd/ke，供 W0-3/W1 钩子实现）

| 主固件事件（§5R.1/§5R.2 口径） | kd 动作 | ke_inputs 映射 |
|---|---|---|
| 唤醒词命中 | KD_EVT_VOICE（脉冲） | social_event=true |
| 轮起手（开始采音） | —（无脉冲，避免重复计费） | — |
| 轮成 rc=0（TTS 播完） | KD_EVT_ROUND_OK | receipt=SUCCESS；social_event=true |
| ASR/LLM/TTS 失败 | KD_EVT_ROUND_FAIL | receipt=FAIL |
| barge-in（打断成功） | KD_EVT_BARGE | social_event=true |
| 人声活动拍（wakenet 仪表 > 阈） | KD_EVT_VOICE（限频：≥5s 一次防刷） | social_event=true |
| 无互动拍（无事件） | 代谢照常 | receipt=NONE；social_event=false |
| 触摸/摇杆（如具备） | KD_EVT_TOUCH | social_event=true |
| 表达播放中 | active=true | —（经 rest 驱力间接生效） |
| 充电插入/拔出 | charging 翻转 | — |
| 云端 LLM `<mood d t/>` | 不经 kd | ke_reappraisal_request（W2，§9.4） |
| 云端 surprise/新话题标记 | — | sensory_residual_mean（有效性位随 D7 补） |

防刷说明：VOICE 类小脉冲必须限频，否则连续在场 = 无限恢复，驱力失效。源的对应物是"eat 有前置（stored_food ≥1）"——恢复永远需要**前置条件**，不是自动的。

### 9.4 W2「事件→评价→情感」简化链路建议

按 §3 的结论（Galatea 无文本情绪分析面），W2 的正确形态是**自建三段映射 + 继承一段纪律**：

1. **轮前（状态出）**：soul limb `soul_snapshot()` → prompt 组装函数注入：
   - 九维一句自然语言（valence/arousal/urgency/persistence 四维足够，禁做跨维标量汇总——遵守 anti-reward-collapse 纪律的措辞版）；
   - ethics_latched → 附加"只允许安抚语气"约束（闩锁期的 prompt 表达）；
   - `<mood>` 协议说明（LLM 只能提议 delta）。
2. **轮中（评价入）**：会话事件按 §9.3 表映射 AffectCycleInputs 等价物。云端若返回"用户情绪标量"，**不得**直写 valence——两条合法路径：(a) 映射为 stimulus/社会事件脉冲（kd 层）；(b) 映射为 sensory_residual（novelty 通道，需有效性位）。
3. **轮尾（重评价）**：`<mood d="+0.1" t="6"/>` → 钳 ±0.2 → `ke_reappraisal_request`。**按 D4 修复为一次性施加**；子句泵剥离纪律不变（跨子句缓冲、绝不入 TTS）。
4. **轮结算**：rc → receipt（§6 的"一轮一回执"口径）。

Galatea 给的边界纪律（全部保留）：LLM 禁直写情感数值（D-13）；重评价必回执；modulation 若做（TTS 语速/音高/动画强度三通道）应有界（[0.5,2.0] 或绝对域钳位）+ TTL + 消费回执——ke 现有 tts_hint 每拍直出不受此限（它是读状态不是发调制），但未来若做"情感调制 ASR 灵敏度/LED"等跨子系统参数，按 modulation 三律实现。

### 9.5 ke_emotion 修正清单（P0 = W0 内完成，P1 = W2 前）

| 优先级 | 项 | 动作 |
|---|---|---|
| P0 | D1 sum/mean | kd 输出 error_sum；ke 头文件字段改名/改注释；soul-test 对拍 valence 固定点锚（−0.0057142857） |
| P0 | D2/D3 伦理阈 | D1 后重推导：事件直注下 valence 可持续负 → 建议阈值 −0.5 ± 仿真定档 + 连续拍数 N + 可选 persistence 合取；**必须补 ke_ethics_clear()**（云端确认/长正交互解锁），否则一次误闩锁到重启 |
| P0 | D5 缺席保持 | !have_drive → 跳过 arousal/urgency 更新 |
| P0 | D6 迟滞带宽 | ke_hysteresis 改 band/2 语义（或改文档+调用方传半宽）——W1 表达层迟回要用 |
| P1 | D4 重评价 | 一次性施加 + 回执携带 applied delta |
| P1 | D7 有效性位 | ke_inputs 增 have_residual/have_uncertainty |
| P1 | D10 tendencies | 随 W3 立 kd_tendency/提案类型（§5.2），不急 |
| P1 | D9 bond 参数 | KE_BOND_STEP/BOND_DECAY 入冻结表（soul_params），注释锚语义 |

---

## §10 风险与未决

| # | 风险/未决 | 级别 | 处置 |
|---|---|---|---|
| R1 | D1 修复改变 valence 量纲，已调好的 anim_tag 阈值（±0.15）与 tts_hint 全部需要重验 | 中 | W0 门禁 V-48a 对拍序列覆盖；阈值随仿真重定 |
| R2 | 事件直注误差下 valence 可持续负（源构造不可达性失效），ethics 误触发概率真实存在 | 中 | D2/D3 重推导 + clear 通道；H6 产品评审一并裁决 |
| R3 | 三驱力代谢/脉冲参数无先验，首版全为 C 级建议值 | 中 | 冻结表集中管理 + 'm' 注入器（W1）+ SOUL 遥测曲线迭代；版本化 profile |
| R4 | social_event/VOICE 限频不当 → 驱力被"刷满"或"饿死" | 低 | §9.3 限频前置（对应 eat 的 stored_food 前置语义）；HIL soak 观察 |
| R5 | Galatea 拍无墙钟语义，τ=4/16 拍在 1Hz 下 4s/16s——对"情绪几分钟转变"的产品直觉，快谱可能过快 | 低 | 保留 4/16 起步（18 号 §4.5 决策），真机日志后再动；改动只动冻结表 |
| Q-1 | stim 驱力是否需要独立的"环境意外"传感面（现仅会话事件），还是并入 novelty 通道即可 | 未决 | W2 落地后看 SOUL 遥测 novelty/urgency 相关性再裁 |
| Q-2 | rest 驱力与 ki 的 quiet_hours（W3 SNTP）是否需要耦合（夜间 rest 恢复 ×2 / 主动行为抑制） | 未决 | W3 因子供给器实现时一并考虑，避免双头调度 |

---

## 附录 A：证据文件索引（本次深挖新增，18 号附录未覆盖部分）

| 文件 | 本次用途 | 关键行 |
|---|---|---|
| `galatea/subject/affect/appraisal.py` | Q3：AppraisalEvent = 审计记录非独立环节；valence_shift 域 [−2,2] | 13-36, 39-58 |
| `galatea/subject/affect/pipeline.py` | Q2/Q3/Q4：step 装配全序、E 求和、e_max、urgency 归一、persistence 归一、回执分支 | 94-140, 164-169, 189-208 |
| `galatea/subject/affect/dynamics.py` | Q2：六算子 + C2 输入映射表（固定来源、零增益）+ 误差缺席保持 | 22-59, 74-121 |
| `galatea/subject/affect/ports.py` | Q1/Q7：DriveKind、AffectCycleInputs、值域构造面、load≡0 强制 | 23-92 |
| `galatea/subject/affect/drives.py` | Q2：告警三级阈值与 trend | 27-65 |
| `galatea/subject/affect/ethics_guard.py` | Q4：双条件、闩锁、纯观察者 | 44-95 |
| `galatea/subject/affect/reappraisal.py` | Q4-D4：two_step 一次性 delta、回执携带 delta | 22-66 |
| `galatea/subject/affect/modulation.py` | Q8：因子界/TTL/回执台账/三门迟滞 | 49-58, 106-216 |
| `galatea/subject/affect/tests/test_dynamics_identifiability.py` | 手算锚（0.578125/0.68359375/0.35607413/0.0875/0.55/0.45） | 全文 |
| `galatea/subject/affect/tests/test_valence_direction.py` | valence=变化率非水平（稳定误差→0） | 14-49 |
| `galatea/subject/affect/tests/test_anti_reward_collapse.py` | tendencies 与 valence 解耦；无标量汇总 API | 60-90 |
| `galatea/subject/affect/tests/test_ethics_trigger.py` | 构造性不可达论证 + 63/64 拍精确判据 | docstring, 46-88 |
| `galatea/subject/affect/tests/test_affect_not_command.py` | 提案类型隔离、import 图禁令 | 14-56 |
| `galatea/subject/goals/producers.py` | Q5：urgency 门 0.1、告警→urgency 映射、七源汇总 | 43-46, 71-89, 75 |
| `galatea/subject/workspace/slow_clock.py` | 慢环触发（urgency≤0 或 arousal<0.1） | docstring |
| `galatea/subject/sensorium/gain.py` | 调制接收面（clip+回执+特征缩放豁免规则） | 37-99 |
| `galatea/subject/models/body.py` | BodyBelief 形成（INTERO 白名单、禁 telemetry 的实现处） | 115-152 |
| `galatea/env/world/mw0.py` | Q2：驱力水平只被动作/事件改变（eat+0.15 等） | 199-224 |
| `galatea/env/world/dynamics.py` | requested/effective 分离、sham/coercion 归属标记 | 124-166 |
| `config/profiles/m0_mw0.profile.yaml` | action_energy_delta、风暴损害、clamp、delay_cycles | 18-46, 62-67 |
| `galatea/evidence/control/runs_c2.py` | 生产级喂入顺序与调制生效窗（[c+1, c+ttl]） | 212-246 |
