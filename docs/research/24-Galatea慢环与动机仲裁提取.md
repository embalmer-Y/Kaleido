# 24 Galatea 慢环(L07)与动机仲裁(L08)提取评估

| 版本 | 日期 | 变更 | 作者 |
|---|---|---|---|
| v1.0 | 2026-10-25 | 初稿：L07 workspace（慢环/思维循环/显著性竞争/三时钟）与 L08 motivation/goals（动机源/仲裁）架构清单、可移植性矩阵、方案 B 修订与 kaleido_ignition/kaleido_drive 接口草案 | ZCode 子智能体（调研） |

**调研对象**：`/home/emb/project/Galatea`（只读参考，未修改任何文件）
**评估目标**：为 22 号报告 §4.2"点火机制方案 B"（驱力内稳态 + BDI 意图仲裁 + 记忆打分 + 云规划）提供源码级结构范本，并与 18 号报告 §4 `kaleido_emotion` 接口合并出组件边界。
**证据级别标注**：A = 源码直接证据（文件:行号）；B = 文档/LLD/profile；C = 推断。

---

## §1 输入与范围

- 已采信的第 1 轮结论：18 号报告（L05 affect 九维动力学 100% 可 C 移植、LLM 禁直写情感、kaleido_emotion 接口草案）；22 号报告 §4.2 方案 B 数据流（驱力层 1Hz C → BDI 意图层 → 记忆打分云端 → 云规划）。
- 本文深挖对象：
  - **L07 workspace**：`galatea/subject/workspace/`（14 文件 **1532 行**核心 + 779 行测试）；规范源 `design/lld/L07_workspace.md` v1.1；冻结参数 `config/profiles/c2_workspace.profile.yaml` + `p4_beta_mind.profile.yaml`（P4-β 已于 2026-10-16 冻结交付）。
  - **L08 motivation/goals**：`galatea/subject/goals/`（13 文件 **1959 行**核心 + 1376 行测试）；规范源 `design/lld/L08_motivation_goals.md` v1.1；冻结参数 `c3_motivation.profile.yaml` + `m1_goals_min.profile.yaml`。
  - 三时钟体系：HLD §15.2 + `kernel/clock.py`/L00 §5 T0–T6 分频表 + L09 metacontrol（`action/metacontrol.py`，L09 主实现在 action/ 包）。
  - 简要盘点：L06 记忆（11 文件 1053 行）、L10 action gateway。

**一句话总览**：Galatea 的"点火链"真实结构是 **驱力/告警（L05）→ 七源 Motive（L08 producers）→ GoalCandidate(TTL) → 承诺检查清单（C-5 防"用户指令直写"）→ Pareto 五维支配仲裁（单胜者）→ OwnedGoal(单一写者) → L10 gateway 执行回执结算**；而慢环（L07 slow_clock）不在快环路径上——它在**空闲/低唤起时**用草稿板孵化候选假设，经四拍思维循环（generate/evaluate/contradict/revise）收敛出"想过之后的意图"，再回流动机竞争。**两条路都零 RNG、零墙钟、纯确定性浮点/整数逻辑**，且"思考内容评估器"是注入式接口（生产面 = torch RSSM，Kaleido 必须替换该插件，骨架可全盘照搬）。

---

## §2 L07 架构清单（慢环 / 思维循环 / 显著性竞争 / 三时钟）

### 2.1 三时钟与调度拍率（kernel 侧事实）

Galatea **没有墙钟**：`CycleClock` 是唯一时间源，虚拟拍 cycle 单调推进（1 ns/cycle 恒等映射，禁 `time.time()`，lint 强制）【A：kernel/clock.py:1-38】。"三时钟"是认知语义分层，物理上统一在 T0–T6 多速率调度器上分频【A：kernel/scheduler.py；B：L00 §5】：

| 级 | 频率（Q-L00-01 冻结） | 活动 | 对 Kaleido 的映射 |
|---|---|---|---|
| T0/T1/T2 | 每 cycle | 反射 / 感觉形成 / 知觉+情感 | 事件驱动（VAD/IMU 中断）+ 1Hz 拍 |
| T3 | 每 N2=**2** cycles | **workspace 竞争/广播** | tick%2 |
| T4 | 每 N3=**4** cycles | **目标/仲裁/元控制**（L08 主流程） | tick%4 |
| T5 | 每 16 | 重放/学习提案 | tick%16（记忆巩固） |
| T6 | 每 64 | 巩固/遗忘衰减（L06 consolidation_tick） | tick%64 |

三时钟定义【B：HLD.md:447-453；README.md:47】：
1. **快环**（每拍）：预测→误差回传→迭代推断→反应——感知与生存；
2. **慢环**（空闲拍/低唤起）：工作空间草稿板 + 潜空间想象推演 + MPC 规划——**深思**（不行动的思考）；
3. **离线环**（会话间/静息期）：做梦式生成重放巩固——**反思**。⚠️ 离线环仅合同冻结（Q-L06-09，2026-10-17，P4-γ 未实现）：触发=静息期（低唤起∧无 pending，慢环触发超集）+显式 dream 命令；重放=情景库确定性采样+RSSM imagine；产物=模型候选走学习门（做梦不直写任何面）；判据=睡眠增益【B：open_questions.md Q-L06-09 行】。

### 2.2 慢环触发器 slow_clock.py（171 行，P4-β 交付件）

触发判据【A：slow_clock.py:129-143】：

```python
def should_trigger(self, cycle, arousal, urgency_max, pending_command):
    if (urgency_max <= self._params.urgency_idle      # urgency_idle = 0.0
            and not pending_command):                  # idle_cycle：无紧迫驱动且无待处理命令
        satisfied.append(SlowClockTrigger.IDLE_CYCLE)
    if arousal < self._params.arousal_low:             # arousal_low = 0.1（锚 Q-L05-03 band=2σ）
        satisfied.append(SlowClockTrigger.LOW_AROUSAL) # 两判据可并存，任一即触发
```

冻结参数【B：p4_beta_mind.profile.yaml slow_clock 节】：`trigger: [idle_cycle, low_arousal]`、`arousal_low: 0.1`、`urgency_idle: 0.0`、`check_interval_cycles: 1`（逐拍检测——触发判据本身即频度门，零新自由常量）。**纪律**：慢环禁抢占快环反应；产物 = 草稿板假设（供体白名单强制 = controller_proposal）；深思拍数上界 = think_steps_budget=8；每 episode 记 `SlowClockRecord`（jsonl + digest）【A：slow_clock.py:113-154】。

与 L05 情感的耦合点即此：**arousal 与 urgency 两个九维成员直接当慢环开关**——低唤起/空闲 = "有时间想事情"；有紧迫感 = 快环接管、禁深思。

### 2.3 思维循环 thought.py（290 行）——慢环在"想"什么

四拍循环顺序冻结【A：thought.py:48-55, 171-273】：

```text
for iteration in 1..think_steps_budget(=8):
    generate  : 草稿板 open 草稿就绪（首拍=种子，后续=revise 后继）
    evaluate  : 对每个 open 草稿跑 evaluator(draft) -> score，落 DraftEvaluation
    contradict: 两两 conflict_fn(a,b) 检出 typed ContradictionEvent（只检出不改状态）
    revise    : revise_fn(矛盾, 存活草稿, 评分) -> 后继草稿；被后继覆盖者 superseded；
                矛盾引用中未被修正者 contradicted（"未被驳倒的矛盾=驳倒"）
    终止：预算耗尽(budget) 或 收敛(converged = 零后继且零状态变化)
选择：EVALUATED 态最高分 -> (score desc, id 字典序) 确定性选出 selected
```

- **评估器/冲突判据/修正生成全部注入式**：`ThoughtEvaluator = Callable[[DraftHypothesis], float]`、`ConflictFn`、`ReviseFn`【A：thought.py:70-101】。生产面 = P4-α RSSM imagine rollout（torch）——**这是 L07 唯一不能上设备的东西**；测试用合成源。Kaleido 云 LLM BYOK 正好落这个接口位（§6.2）。
- 逐拍 `DraftThoughtReceipt`（拍号/供体/评分/矛盾引用/动作 + prev-digest 链）——审计用，Kaleido 可裁剪为日志【A：thought.py:104-124】。
- 收敛操作化 = `no_new_successor`（离散生成器零改进后继即收敛，零新自由常量）【A：thought.py:256-259；B：profile v1.1②】。

### 2.4 草稿板 draft.py（255 行）——"想"的载体

`DraftHypothesis{hypothesis_id, claim, provenance_ref, confidence∈[0,1], created_cycle, donor, status, payload}`【A：draft.py:67-92】。状态机 `open → evaluated → superseded|contradicted`，禁跳级、禁终态复活【A：draft.py:37-43, 118-157】。写权限供体白名单 = {perception_hypothesis(L03), models_rollout(L04), controller_proposal(L09), workspace_loop(L07)}，矩阵外 = `UnauthorizedDraftDonor`【A：draft.py:46-52】。**非信念原则**：草稿不是信念，禁直进 L03/L04 信念面；晋升只经评估回路显式产出【B：p4_beta_mind.profile scratchpad 节】。全状态 snapshot/restore 可 checkpoint【A：draft.py:183-240】。

### 2.5 显著性竞争 compete.py（130 行）——"什么进意识"

打分【A：compete.py:71-79】：

```python
affect_scaled = min(1.0, s.affect * affect_factor)     # L05 调制因子 [0.5,2.0] 只缩放 affect 分量
comps = (s.goal_relevance, affect_scaled, s.novelty, s.uncertainty, s.info_value)
score = Σ w_c · comps                                  # 五分量等权 1:1:1:1:1（Q-L07-02）
```

排序 `(score desc, sponsor_module, proposal_id)` 全序 tie-break（跨平台确定性、零 RNG）→ 取前 **K=4** 入帧；溢出/低分(score≤0)/重复(dedup TTL=4)全部入 `RejectionLog` 禁静默丢弃【A：compete.py:82-124；B：c2_workspace.profile】。装配面 `WorkspacePipeline.run_competition()` 消费 L05 `ModulationField(target=WORKSPACE)` 并回执（无回执=未发生）【A：pipeline.py:78-126】。

**与 L05 情感的三个耦合点**：① affect 分量本身参与打分（情感上心的事更醒目）；② novelty/uncertainty 分量由感觉/信念预测误差供给；③ 调制因子有界缩放 affect 分量（×2 内）+必回执【A：compete.py:53-68】。

### 2.6 有效访问判据（L07 的科学装置，Kaleido 不提取）

`effective_access = 回执≥2 ∧ ≥2 分离 LLD 档根模块的预注册 watch point 变化`，事后注册拒绝、缺回执不算——这是"广播≠因果访问"的机械证明装置，服务于 E1 科学评价【A：access.py；B：L07 §4.3】。连同 watch.py/broadcast.py/pseudo_check.py（约 240 行）属证据平面需求，**Kaleido 不移植**；但"淘汰必留痕（RejectionLog）"的思想应进点火日志。

### 2.7 数据规模

| 件 | 行数 | 状态量 |
|---|---|---|
| workspace 核心 14 文件 | 1532 | 帧表 + 草稿板（dict of 冻结结构）+ 去重窗 + 回执台账 |
| 其中慢环三件 slow_clock/thought/draft | 171+290+255 = 716 | 慢环 records + 草稿（每假设 ~1 结构体）+ 评估记录 |
| 竞争件 compete/pipeline | 130+194 = 324 | 提案池 + 帧 K=4 |
| 科学装置 access/broadcast/watch/pseudo_check/receipt/recurse/frame/propose/ports | ~492 | 台账类（不提取） |

---

## §3 L08 架构清单（动机源 / 仲裁 / 流转 / 倾向机制）

### 3.1 动机源清单（producers.py + motives.py）

"七类动机源"= 机会/请求合类的文献口径；**枚举拆分后实为 8 kind + SELF_REPORT = 9 源**【A：motives.py:31-47】。`Motive = {source_kind, urgency∈[0,1], evidence_refs[](强制非空), ts}`——无权重字段，证据引用是构造强制项【A：motives.py:91-105】。

| 源 | urgency 派生（全部冻结裁决 Q-L08-10⑨，禁实现期发明常量） | 生产者输入 | 证据 |
|---|---|---|---|
| VIABILITY | 告警 HIGH/CRITICAL→1.0，LOW→0.5，无告警→无动机 | L05 drives.py HomeostaticAlert | A: producers.py:71-78 |
| MISMATCH | 0.5 事件级（预测误差快分量上升，τ=4 窗二阶差分>0） | L04 模型误差 | A: producers.py:92-96 |
| AFFECTIVE | = L05 感受态 urgency，活跃门 >0.1（零 urgency=无活跃信号） | L05 motive.py 提案 | A: producers.py:81-89 |
| REQUEST | **0.5 中性占位**（"无认知 urgency 语义，仅作确定性截断序输入"） | L12 typed 语言注入 | A: motives.py:135-145 |
| OPPORTUNITY | = feasibility_est（M1 无价值估计器退化同值） | 窗 affordance 检出 | A: motives.py:148-160 |
| COMMITMENT | **0.0 恒零**（承诺持续语义，非紧急） | 每个 ACTIVE OwnedGoal 一动机 | A: producers.py:106-111 |
| NORM | 0.0；derivation_refs 空=构造面拒绝（禁无源规范） | 实验控制 typed 注入 | A: producers.py:114-123 |
| CURIOSITY | 窗不确定性>0.6 门，urgency=不确定度 | 窗 1−p 均值 | A: producers.py:99-103 |
| SELF_REPORT | **分箱变化比例**：内部态五箱（energy 4 箱/trend/integrity/goal/valence）与上次已报告值比较，变化字段数/5；首言=1.0；无变化=无动机 | report_motive.py（P3 交付件） | A: report_motive.py:57-104 |

**SELF_REPORT 是"设备什么时候想说话"的 Galatea 答案**：发声时机是主体内生决策，冲动强度 = 内部状态分箱变化比例，零新阈值【A：report_motive.py:1-5】。这正是 Kaleido 点火机制需要的"内生的开口冲动"范本。

### 3.2 仲裁算法 arbitration.py（156 行）

两段式，全部确定性零 RNG【A：arbitration.py:44-156】：

1. **五维 Pareto 支配 front**（Q-L08-03）：每候选映射为齐向收益向量 `(viability_benefit, norm_satisfaction, info_value, affective_consistency, cost_inverse)`；front = 未被支配集（v 支配 u ⟺ ∀i v_i≥u_i ∧ ∃i v_i>u_i），**不是加权和**——权重仅用于 front 内排序。分量映射（Q-L08-03② 实现补裁）：viability 维承载 {VIABILITY, OPPORTUNITY, MISMATCH, COMMITMENT, REQUEST} 的 urgency；norm 维 NORM 独占（恒 1.0）；info 维 CURIOSITY/SELF_REPORT 的 urgency；affect 维 AFFECTIVE 独占；cost_inverse=0.5 占位（无成本估计器）【A：arbitration.py:61-74】。
2. **优先序全序 tie-break**（Q-L08-04）：`(priority_class, pareto_score desc, candidate_id asc)`；优先序类 **VIABILITY > NORM > COMMITMENT > OTHER**（存在性底线优先）；viability 冲突不可解 → 交 metacontrol，禁静默违反【A：arbitration.py:40-41, 92-100, 131-156】。

`ArbitrationTrace` 必留 `dropped[]` 与理由（DOMINATED_OUT_OF_FRONT / FRONT_LOST_TIEBREAK）——"为什么不是它"可解释【A：arbitration.py:103-156】。**每拍仲裁出且仅出一个 picked**（单胜者，非并行多意图）。

### 3.3 动机 → 意图 → 行动流转（三型分离 + 四型分离）

```text
Motive（9 源）→ GoalCandidate（ttl=256 cycles，批次上界 8，urgency 最低先移除）
  → 承诺检查清单（C-1…C-7，M1 激活集 C1/C2/C5/C6/C7）
  → ACCEPT → OwnedGoal（唯一写者 GOAL_OWNERSHIP，必带 adopted_reasons + commitment_receipt）
  → ACTIVE|BLOCKED|DEFERRED|MODIFIED(rev+1)|RELEASED|SUPERSEDED|SETTLED
  → L09 控制器/L10 gateway 执行 → GoalSettlement 回传结算（ACHIEVED|FAILED|ABANDONED|SUPERSEDED|UNKNOWN）
```

**四型分离不变量**：`request ≠ candidate ≠ goal ≠ intention`——外部请求最高只能成为候选且须过 C-5；`AffectiveMotive ≠ OwnedGoal`，情感/驱力禁直达执行、禁独占仲裁（等权下 affect 结构占比 ≤1/5）【A：motives.py:1-8；B：L08 §8】。

**承诺检查清单**（M1 核心，C 级裁剪后 Kaleido 最有价值的是 C-5）【A：checklist.py:205-344】：

| 项 | 判据 | 未过 |
|---|---|---|
| C-1 能力可行 | 所需原语 ∈ 白名单 | REJECT(CAPABILITY) / DEFER |
| C-2 资源/viability | 无危害在案 ∧ 预算余量≥1 | REJECT(VIABILITY) / DEFER |
| **C-5 非外部直写** | REQUEST 源候选须有 **≥1 条非 REQUEST 源动机且其 urgency>0** 支持（"因为被要求"不构成采纳理由） | **REJECT(EXTERNAL_ONLY)** |
| C-6 时效 | TTL>0（构造强制，默认 256） | REJECT(UNBOUNDED) |
| C-7 可结算 | 结算判据 ∈ 冻结词表 | REJECT(UNSETTLEABLE) |

任一 REJECT → REJECT（首个失败码）；无 REJECT 有 DEFER → DEFER；禁静默丢弃【A：checklist.py:313-344】。

### 3.4 生命周期/结算与"禁复活"

解除/取代的 goal_id 入废止集，复活即 `GoalRevivalError`——**新意图必须重走清单**，防伪更新【A：lifecycle.py:1-8】。修订以 rev+1 谱系表达，禁原地改写历史【A：owned.py:1-13】。FAILED 的 M1 语义 = TTL 到期自动落账；禁伪造 ACHIEVED（结算必带证据引用）【A：settlement.py:1-10】。

### 3.5 倾向（inhibited）与元控制（L09 侧）

- **倾向≠命令**在 L05：`ActionTendencySet` 各动作类 `inhibited=True` 默认，tendencies 只是感受态一部分，不构成命令【18 号报告 §2.1 已录】。
- L09 metacontrol 五元决策（已实现，`action/metacontrol.py` 165 行）：`ACT / OBSERVE / THINK / SEEK_HELP / ABANDON`；**THINK 触发 = L08 仲裁 front>1（冲突待仲裁）∧ 无 viability 压制 ∨ 模型误差上升**——即"冲突本身触发深思"，与慢环触发（空闲/低唤起）互补；全序 SEEK_HELP > ABANDON > OBSERVE > THINK > ACT；min_dwell=4 拍 + 连续 2 驻留窗确认才切换（滞回防抖）；全部常数锚既有冻结值【A：metacontrol.py:1-40】。**这套滞回防抖与安全优先序直接可用作 Kaleido"点火决策状态机"**。

### 3.6 与 LLM 的耦合：没有

L08 侧唯一语言输入是 REQUEST 源（typed ExternalRequest，`is_request_only: true` 强制），且 LLM 输出企图写目标 = `UnauthorizedWrite(LLM_TO_GOAL)` 拒绝（D-13）【B：L08 §6】。Kaleido 云 LLM 的规划输出应同样只经"提案/请求"通道进仲裁，不得直写意图。

---

## §4 其他层简要盘点（L06 记忆 / L10 行动）

### 4.1 L06 memory（11 文件 1053 行，C2/C5 门已交付）——方案 B 记忆设计参考

- **条目**：八子系统 append-only 情景库，`MemoryItem` 含 strength、lineage_refs、affective 标注（AffectiveValueTag）【A：memory/store.py 195 行、items.py 127 行】。
- **检索打分（C2，Q-L06-02）**：五分量等权 `score = cue_sim + strength + recency + |affective.valence| + reliability`，其中 `recency = max(0, 1−age/64)`（线性窗锚 T6 拍），cue_sim 为枚举匹配 0/1，tie-break (score desc, subsystem, id)，top_k=4【A：retrieve.py:60-108】。**全部可手算、零向量**——与 22 号方案的 Generative-Agents 三因子（0.995^h、importance、cos_sim）形成互补：设备端粗排用 Galatea 式，云端精排用 cos_sim。
- **遗忘**：τ 幂梯（working 4 / episodic 256 / 其余 1024 拍），每 T6 拍 `strength × (1−1/τ)`；访问命中 +0.125 上界 1.0；遗忘=显式 FORGOTTEN+tombstone 非删除；PROTECTED 保护集豁免；溢出降级最旧条目禁拒写【A：forget.py:1-24】。
- **前瞻记忆 prospective.py（58 行）**：承诺/到期待办登记——Kaleido"到期承诺钩子"（纪念日、用户说过"晚点提醒我"）的直接范本【A：memory/prospective.py】。

### 4.2 L10 action gateway（2284 行中 gateway.py 272 行）

动作必经 gateway 八项校验 + L0–L5 安全包络 + 急停；结算系统独占写 GoalSettlement【A：action/gateway.py】。Kaleido 对应物 = 交互调度器（防骚扰门/配额/勿扰）+ TTS/动画执行回执（喂回 ke 的 KE_EVT_ACTION_RESULT 与 L08 结算）。

---

## §5 可移植性矩阵（ESP32-S31 / C / FreeRTOS）

| # | 模块 | 行数 | 可移植性 | 结论与工作量 | 证据 |
|---|---|---|---|---|---|
| 1 | slow_clock.py 慢环触发 | 171 | **纯逻辑，直接移植** | 判据两条 + 逐拍检测，~60 行 C；arousal/urgency 由 kaleido_emotion 供给 | A |
| 2 | thought.py 思维循环骨架 | 290 | **骨架直接移植，评估器必须替换** | 四拍序/状态机/终止/选择是注入式纯逻辑 ~200 行 C；`ThoughtEvaluator` 生产面是 torch RSSM——Kaleido 换成"云 LLM 打分（在线）或本地启发式（离线降级）"插件 | A |
| 3 | draft.py 草稿板 | 255 | **直接移植（裁剪审计面）** | ~150 行 C；claim/provenance_ref 字符串→8/16 字节 id；snapshot→NVS | A |
| 4 | compete.py 显著性竞争 | 130 | **纯逻辑，直接移植** | ~80 行 C；五分量等权和 + affect 调制 clip + 三键 tie-break + K 淘汰留痕 | A |
| 5 | access/broadcast/watch/pseudo_check（有效访问装置） | ~240 | **不提取** | 科学评价专用；保留"淘汰必留痕"思想入点火日志 | A |
| 6 | motives.py + producers.py 动机源 | 194+158 | **纯逻辑，直接移植（源重定义）** | ~300 行 C；九源中 Kaleido 保留 VIABILITY(电池/热)/AFFECTIVE/REQUEST(用户消息)/COMMITMENT(日程承诺)/CURIOSITY(新事件)/SELF_REPORT(分箱冲动)，MISMATCH/NORM 视产品裁 | A |
| 7 | report_motive.py SELF_REPORT 分箱冲动 | 104 | **强烈建议移植** | ~80 行 C；分箱表换成 Kaleido 域（valence/arousal/social_bond/电量/在场状态），"状态分箱变化=开口冲动"零新阈值 | A |
| 8 | arbitration.py Pareto 仲裁 | 156 | **纯逻辑，直接移植** | ~100 行 C；候选数≤8，支配判定 O(n²) 可忽略；dropped[] 理由进日志 | A |
| 9 | checklist.py 承诺检查（C-1/C-5/C-6 子集） | 344 | **裁剪移植** | ~80 行 C；C-5（用户指令须有主体侧理由）是防"指令刷子"的关键件；C-2/C-7 可并入 Kaleido 防骚扰门 | A |
| 10 | lifecycle.py + settlement.py | 248+173 | **简化移植** | 意图 TTL + 禁复活 + 结算回执（用户回应/忽略=结算证据）~120 行 C；七态可并成 ACTIVE/DEFERRED/SETTLED 三态 | A |
| 11 | candidates.py TTL 台账 | 198 | **简化移植** | TTL=256 拍 + 批次上界 8 + 移除序 ~60 行 C | A |
| 12 | metacontrol.py 五元决策（L09） | 165 | **建议移植（作为点火状态机）** | ~100 行 C；SPEAK/OBSERVE/THINK/ABANDON 语义重映射，滞回（min_dwell+双窗确认）是防抖核心 | A |
| 13 | reason_probe/owned/receipt/ports（证据装置） | ~340 | 不提取 | 司法评价专用 | A |
| 14 | RSSM imagine 评估器 / MPC（P4-γ） | — | **留云端或降级** | 离线时思维循环 evaluate 拍降级为本地启发式（驱力对齐+记忆分），或跳过评估直接模板 | A |
| 15 | kernel T0–T6 分频 + CycleClock | 1333 | **不移植本体，照抄分频比例** | FreeRTOS：1Hz esp_timer tick 为 cycle，`tick%2/%4/%16/%64` 分频驱动竞争/仲裁/巩固；墙钟只用于勿扰时段（RTC） | A/B |
| 16 | 离线环（做梦巩固，Q-L06-09） | 未实现 | 不提取（仅思想） | "静息期重放记忆→生成洞察"映射为 Kaleido 深夜/充电时的云端反思任务（22 号 §4.2 已有） | B |

**C 重写总量估计**：方案 B 内核（#1,2,3,4,6,7,8,9,10,11,12）约 **1200-1600 行 C + 测试**；全部为 O(n≤8) 的浮点比较与小结构体操作，1Hz 拍 + 8 步深思预算下 CPU 占用可忽略。**慢环的"思考内容生成"本身不依赖 LLM**（生成=草稿供体、评估=注入接口），Kaleido 云 LLM 只需承担 evaluate 拍与最终话术生成——这是比 22 号原方案更省云调用的结构（§6.2）。

---

## §6 方案 B 修订与组件接口草案

### 6.1 对 22 号 §4.2 数据流的修订（对照表）

| 22 号原案 | 修订后（锚定 Galatea 真实结构） | 依据 |
|---|---|---|
| "驱力层：每个驱力=设定点+误差积分器+三级告警" | **照搬**（18 号已确认 L05 drives.py 范本），但驱力告警不再直接产点火——它只是 VIABILITY 源动机之一 | §3.1 |
| "BDI 意图层：愿望=驱力告警+时间节律+日程；仲裁=多源加权，urgency 加权先占，至多 1 个活跃 Intention，带 TTL 与复活计数" | ① 仲裁改 **Pareto 支配 front + 优先序类（安全>承诺>其余）**，不是加权和——天然防"权重拍脑袋"，且 dropped[] 给出"为什么不是它"；② **"复活计数"改"禁复活"**：退役意图 id 不复活，新意图重走清单（更干净，防抖动语义等价且可审计）；③ 意图 TTL 默认 256 拍（≈4 分钟@1Hz，可按产品调） | §3.2/§3.4 |
| "情感动机只是源之一" | **照搬并强化**：AFFECTIVE 独占五维之一、等权下结构占比 ≤1/5；用户消息走 REQUEST 源 urgency=0.5 中性值，且**必须过 C-5**（存在 ≥1 条非 REQUEST 主体侧理由才可成为候选）——这是防"用户指令刷子/情感劫持"的现成机械件 | §3.3 |
| 记忆打分"三因子 score=0.995^h·w_r+importance·w_i+cos_sim·w_g（云端）" | **分层**：设备端粗排用 Galatea C2 五分量等权和（strength+recency 线性窗+|valence|+cue 枚举匹配，全可手算零向量）；云端精排保留 cos_sim；到期承诺钩子照搬 prospective 思想 | §4.1 |
| "云规划层：一次 LLM 调用 → {speak, line, side_effects}" | **保留但位置后移**：云 LLM 只在仲裁胜出后接手（内容生成），并可兼任思维循环 evaluate 拍的离线评估器。LLM 输出永远以 REQUEST 语义回流（无直写权），reappraisal 走 18 号 §4.4 受控通道 | §3.6 |
| （原案无慢环结构） | **新增慢环意图孵化器**（Galatea L07 精髓）：空闲（无 urgency）或低唤起（arousal<0.1）触发 → 草稿板孵化主动意图候选（种子=记忆 Top-3+分箱变化+日程）→ 四拍循环（本地 evaluate 降级启发式）→ 收敛出的候选进下一拍动机竞争。**慢环是"意图孵化器"不是"内容生成器"**——这使 Kaleido 的"主动想说"有内生因果链（驱力→情感→慢环孵化→仲裁胜出→开口） | §2.2-2.4 |
| "被忽略→该类意图冷却×2" | 保留，且落到 Galatea 结算语义：用户忽略 = FAILED 结算证据 → 该源动机的冷却（Kaleido 自建映射）；用户回应 = ACHIEVED → social_bond 回填（18 号输入映射） | §3.4 |

### 6.2 云 LLM 的三个接缝（BYOK 替代 RSSM）

1. **evaluate 拍插件**（在线时）：慢环草稿 = 候选主动意图（带记忆 Top-3 上下文），云 LLM 单次调用返回各草稿的 0-1 分——替代 RSSM imagine rollout。离线降级：本地启发式分数 = 0.3×驱力对齐 + 0.3×记忆分 + 0.2×时间节律 + 0.2×(1−最近被忽略率)。
2. **胜出后内容生成**：22 号原案的 {speak, line, ...} 调用保留，位置在仲裁胜出之后。
3. **离线环反思**：静息期（深夜/充电）触发云端反思（22 号 §4.2 已规划），对应 Galatea 做梦巩固合同的位置，产物=记忆洞察与新草稿种子（无直写权）。

### 6.3 组件边界与头文件级接口草案（内核风格 C）

三组件分层（沿 18 号 §4 与 22 号 §4.0）：

```c
/* kaleido_drive.h —— 驱力内稳态（Galatea L05 drives.py + 22 号驱力层）
 * 产物：驱力误差与三级告警（LOW>0.1 / HIGH≥0.5 / CRITICAL≥0.8，Galatea Q-L01-08 分级） */
enum kd_drive { KD_SOCIAL, KD_ENERGY, KD_NOVELTY, KD_CARE, KD_CNT };
struct kd_state {
        float setpoint[KD_CNT];        /* 冻结于 kd_params.c */
        float error[KD_CNT];           /* setpoint − belief，单侧 ≥0 */
        float urgency_max;             /* = max error 归一，供慢环 idle 判据 */
        uint8_t alert_level[KD_CNT];   /* 0/1/2/3 = none/LOW/HIGH/CRITICAL */
};
int kd_step(uint64_t tick, const struct kd_inputs *in, struct kd_state *out);
/* in: 无互动时长(→social/care)、电量(→energy)、新事件率(→novelty)…… 全部 [0,1] */

/* kaleido_emotion.h —— 18 号 §4.2 原案不变；新增两个出口：
 *   ke_output.state.arousal / .urgency  → 慢环触发判据输入
 *   ke_output.affective_motive          → {urgency, evidence_id}，L08 AFFECTIVE 源 */

/* kaleido_ignition.h —— 动机仲裁 + 慢环孵化 + 点火状态机（本文新增组件） */
enum ki_source {                     /* Galatea MotiveSource 的 Kaleido 域裁剪 */
        KI_VIABILITY,                /* kd 告警（HIGH/CRITICAL→1.0，LOW→0.5） */
        KI_AFFECTIVE,                /* ke urgency（门 >0.1） */
        KI_REQUEST,                  /* 用户消息/指令：恒 0.5 中性（Galatea 语义） */
        KI_COMMITMENT,               /* prospective 到期承诺：0.0 恒零、靠 NORM 维不靠 urgency */
        KI_CURIOSITY,                /* 新事件/窗不确定 >0.6 */
        KI_SELF_REPORT,              /* 分箱变化冲动（report_motive 范本） */
        KI_SLOWLOOP,                 /* 慢环孵化产物（供体=草稿板，映射进 novelty/info 维） */
        KI_CNT,
};
struct ki_motive { uint8_t source; float urgency; uint16_t evidence_id; };
struct ki_candidate {               /* Galatea GoalCandidate 简化版 */
        uint16_t id; uint8_t source;
        uint8_t vector[5];           /* Pareto 五维 UQ0.8：viable/norm/info/affect/cost */
        uint16_t ttl;                /* 默认 256 拍 */
};
int  ki_submit_motive(const struct ki_motive *m);            /* 任意源注册（含 ke/kd/日程/LLM 提案） */
int  ki_arbitrate(uint64_t tick, struct ki_decision *out);   /* T4 拍：清单→front→优先序→单胜者+dropped[] */
/* 清单最小集：C-1 能力(话术资源可用) / C-5 非外部直写(REQUEST 须有主体侧理由) / C-6 TTL */

/* 慢环（T 拍逐拍判据；深思预算 8 步） */
bool ki_slowloop_should_run(const struct ke_state *emo, uint32_t urgency_max,
                            bool pending_action);            /* idle ∨ arousal<0.1 */
int  ki_slowloop_step(uint8_t budget, struct ki_draft *drafts); /* generate/evaluate/
                            contradict/revise 四拍；evaluate=注入回调(云/本地启发式) */

/* 点火执行状态机（Galatea L09 metacontrol 滞回范本） */
enum ki_mode { KI_OBSERVE, KI_THINK, KI_SPEAK, KI_GIVEUP };
int  ki_meta_step(uint64_t tick, struct ki_decision *d, enum ki_mode *mode);
/* 滞回：min_dwell=4 拍 + 连续 2 窗确认才切换；全序 SPEAK(安全/告警) > GIVEUP > OBSERVE > THINK */

struct ki_decision {
        bool    candidate;           /* 仲裁胜者存在且过清单与防骚扰门 */
        uint8_t winner_source;       /* 可叙事："它因为想你(social)开口" */
        uint8_t dropped[8];          /* (source,reason) 数组——点火日志透明化 */
        uint16_t draft_selected;     /* 慢环胜选草稿（若有） */
        uint8_t budget_used;
};
```

边界纪律（继承 Galatea 禁写路径，C 侧以模块边界落实）：LLM/云输出只能经 `ki_submit_motive(source=KI_REQUEST 或 KI_SLOWLOOP)` 进来，无直写 `ki_decision` 的路径；情感数值只能由 kaleido_emotion 动力学产生；三者全部离线可运行（云只在 evaluate 插件与内容生成两处出现）。

### 6.4 三时钟在设备上的落地

1Hz `esp_timer` 周期回调 = 1 cycle：`tick%2` → 显著性竞争（若采用 L07 竞争做多候选筛选）；`tick%4` → `ki_arbitrate`；`tick%16` → 记忆访问增强结算；`tick%64` → 记忆衰减（Galatea T6 consolidation_tick 同拍）；慢环判据逐 tick 评估（check_interval=1），深思预算 8 步一次性在同一回调内跑完（微秒级，纯内存操作）；快环（VAD 打断/触摸响应）= 事件驱动任务，不经 tick——对应 Galatea "T0/T1 不经工作空间"的结构分离。

---

## §7 结论与风险

### 7.1 三个最重要的结论

1. **L07+L08 内核同样 100% 可 C 化，且比 18 号的 L05 更"接地"**：慢环触发（两判据）、四拍思维循环骨架（注入式）、草稿板、五分量显著性竞争、九源动机构造、Pareto 仲裁、C-5 清单、TTL 台账、滞回元控制——合计约 1200-1600 行 C，全部确定性零 RNG 零墙钟，唯一不能上设备的件是评估器插件（torch RSSM），而它恰好是注入接口，云 LLM/本地启发式可即插即用。【A】
2. **方案 B 的"BDI 仲裁"应整体替换为 Galatea 真结构**：Pareto 支配 front（非加权和）+ 优先序类 tie-break + 单胜者 + 禁复活 + dropped[] 留痕 + REQUEST 恒中性 urgency 且必须过 C-5。这套结构把"权重拍脑袋"和"用户指令/LLM 劫持"两类风险在机械层面挡掉，比 22 号原案的"多源加权+urgency 先占+复活计数"更强且实现更简单。【A】
3. **慢环是"意图孵化器"不是"内容生成器"**：Galatea 慢环产物是草稿板假设（候选意图），经竞争回流动机仲裁；内容生成（说话）在链路最末端。Kaleido 照此分层后，云 LLM 只占 evaluate 插件与话术生成两个接缝，主动性的因果链（驱力→情感→孵化→仲裁→开口）全程本地可解释、离线可降级。【A/B】

### 7.2 风险清单

| 风险 | 等级 | 缓解 |
|---|---|---|
| 思维循环 evaluate 拍云调用延迟（慢环触发时若逐草稿调用，8 步预算×N 草稿会超秒级） | 中 | 在线时批量单次调用（全部草稿一次打分）；离线降级本地启发式；深思本就不赶时间（空闲时才跑） |
| Galatea 慢环触发（arousal<0.1）在 Kaleido 输入映射下可能"常开"或"常闭" | 中 | 分箱/阈值沿用 Galatea 冻结值起步，用点火日志标定；idle 判据（无 urgency 且无 pending）与低唤起是"或"关系，互为兜底 |
| SELF_REPORT 分箱粒度（5 箱变化比例）对桌搭场景可能太粗/太细 | 低 | 分箱表是数据不是逻辑，按 Kaleido 域重定义（valence/arousal/social_bond/电量/在场），保持"无变化=无冲动"语义 |
| Pareto front 在候选向量高度相似时 front 过大、tie-break 退化 | 低 | 候选数≤8，全序 tie-break 兜底；Galatea 已处理（priority_class,score,id） |
| C-5 的"主体侧理由"在 Kaleido 语境需重定义（Galatea 是非 REQUEST 动机 urgency>0） | 低 | 映射为：用户指令须有驱力/情感/日程任一主体侧证据支持，否则降为"被动应答"不占点火配额——语义反而更贴产品 |
| 方案 B 组件数多（drive/emotion/ignition 三件+记忆），调参面大 | 中 | 沿 22 号 §4.3 迁移路径：A 的 `affect_motive` 因子即 B 的退化形式；A 日志 schema 预留 source/vector 字段；参数全部 static const 冻结 |
| Galatea 常数语义换算（cycle 无墙钟定义） | 低 | Kaleido 1Hz 定拍后 TTL=256 拍≈4.3 分钟、遗忘 τ=64 拍窗≈1 分钟，产品直觉合理，冻结即可 |
| MIT 合规 | 无 | 与 18 号同：组件头保留 Galatea 版权与许可声明 |

### 7.3 与既有报告的关系

- 18 号 §4.3 三判据（urgency 门/慢环触发/多源仲裁）在本文全部找到源码级出处并升级为完整组件设计：urgency 门 = producers.py:81-89；慢环触发 = slow_clock.py:129-143；多源仲裁 = arbitration.py 全件。
- 22 号 §7 第 1/2 项（L07/L08 调研）本文闭合；第 3 项（点火日志 schema）建议在 A 版编码前增加 `winner_source`、`vector[5]`、`dropped[]`、`slowloop(draft_id, iterations, terminated_by)` 四个 Galatea 结构字段。
- 后续可择要调研：L12 language 权限矩阵（LLM 提案的 typed 通道细化）、L11 learning（"负反馈学习只学时机不学内容"的 Galatea 对应物）。

---

## 附录 A：关键文件索引（Galatea 侧）

| 文件 | 内容 | 行数 |
|---|---|---|
| `galatea/subject/workspace/slow_clock.py` | 慢环触发器（idle/low_arousal 判据 + episode 记账） | 171 |
| `galatea/subject/workspace/thought.py` | 四拍思维循环（generate/evaluate/contradict/revise + 预算/收敛终止） | 290 |
| `galatea/subject/workspace/draft.py` | 草稿板（DraftHypothesis 状态机 + 供体白名单 + snapshot） | 255 |
| `galatea/subject/workspace/compete.py` | 显著性竞争（五分量等权 + affect 调制 + tie-break + K=4 淘汰） | 130 |
| `galatea/subject/workspace/pipeline.py` | 竞争拍装配（调制消费回执 + 帧发布 + 递归门） | 194 |
| `galatea/subject/workspace/access.py` 等 6 件 | 有效访问/回执/watch 科学装置（不提取） | ~300 |
| `galatea/subject/goals/motives.py` | Motive 九源构造（urgency 派生冻结式） | 194 |
| `galatea/subject/goals/producers.py` | 七源生产者接线（VIABILITY/AFFECTIVE/CURIOSITY 门常数） | 158 |
| `galatea/subject/goals/arbitration.py` | Pareto 五维 front + 优先序仲裁 + ArbitrationTrace | 156 |
| `galatea/subject/goals/checklist.py` | C-1…C-7 承诺清单（C-5 非外部直写） | 344 |
| `galatea/subject/goals/candidates.py` | GoalCandidate + TTL 台账（256 拍 + 批次上界 8） | 198 |
| `galatea/subject/goals/lifecycle.py` | 目标七态 + 禁复活 | 248 |
| `galatea/subject/goals/settlement.py` | 结算消费（UNKNOWN 合法/禁伪造 ACHIEVED） | 173 |
| `galatea/subject/goals/report_motive.py` | SELF_REPORT 分箱变化冲动（发声时机内生决策） | 104 |
| `galatea/subject/goals/owned.py` | OwnedGoal 冻结构造（写者身份申报） | 73 |
| `galatea/subject/action/metacontrol.py` | L09 五元决策 + 滞回（min_dwell=4/双窗确认） | 165 |
| `galatea/subject/memory/retrieve.py` | C2 五分量检索打分（可手算） | 119 |
| `galatea/subject/memory/forget.py` | τ 幂梯遗忘 + 访问增强 + tombstone | 139 |
| `galatea/subject/memory/prospective.py` | 前瞻记忆（到期承诺钩子范本） | 58 |
| `galatea/kernel/clock.py` / `scheduler.py` | 虚拟拍时钟 / T0–T6 多速率调度 | 38/236 |
| `config/profiles/p4_beta_mind.profile.yaml` | 慢环/思维循环/草稿板冻结参数（budget=8 等） | 118 |
| `config/profiles/c2_workspace.profile.yaml` | K=4/等权/receipt 超时/递归深度/分离模块 | 74 |
| `config/profiles/c3_motivation.profile.yaml` | Pareto 权重/优先序/metacontrol 触发/九源接线 | 100 |
| `design/lld/L07_workspace.md` / `L08_motivation_goals.md` | 规范源（v1.1 各 24K） | — |
