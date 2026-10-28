# 29 Galatea 慢环（L07）与动机仲裁（L08/L10/L12）深挖——驱动 W3"点火"重设计

| 版本 | 日期 | 变更 | 作者 |
|---|---|---|---|
| v1.0 | 2026-10-28 | 初稿：Q1–Q8 源码级机制（慢环触发判据/四拍思维循环/九源动机/C-5 清单/Pareto 仲裁/OwnedGoal 生命周期与结算/LLM typed 角色/防骚扰全清单）+ §9 Kaleido W3 落地建议（思维触发开关、typed 意图协议、最小意图生命周期、结算 v2、C API 草图） | ZCode 子智能体（调研） |

**调研对象**：`/home/emb/project/Galatea`（只读，未修改任何文件）
**服务对象**：负责人 2026-10-28 裁定——**点火（ignition）只是触发"思维"的开关**（外部输入+内部状态决定是否进入思考）；进入思考后**对外作何动作由 AI 自己决定**（LLM 作为深思引擎自主产出意图与话语），不是固件模板句。W3 现计划（plan §5R.4"模板句主动说话"）据此推翻重设计。
**证据级别**：A = 源码直接证据（文件:行号）；B = LLD/profile 文档；C = 推断/移植建议。
**与 24 号报告的关系**：24 号是架构清单与可移植性矩阵；本文逐机制给出精确判据、数学式与状态机，并修正 24 号一处需要标注的推断（慢环产物回流动机竞争的"生产接线"现状，见 Q2.4）。

---

## §0 一页总览：Galatea"慢环孵化→竞争→仲裁→执行→结算"全链

```text
[触发] slow_clock.should_trigger(cycle, arousal, urgency_max, pending_command)
        = idle(urgency≤0 ∧ 无pending) ∨ low_arousal(<0.1)        ……A: slow_clock.py:129-143
   └→ 产物只有草稿板假设（禁产命令/意图；供体白名单=controller_proposal）
[思维] ThoughtLoop 四拍 generate→evaluate→contradict→revise，预算8步
        终止=预算耗尽 ∨ converged(零后继且零状态变化)
        选择=EVALUATED 最高分（tie→id 字典序）                    ……A: thought.py:171-273
[动机] 九源 Motive(source, urgency∈[0,1], evidence_refs强制非空, ts) ……A: motives.py:91-105
   └→ GoalCandidate(ttl=256, 批次≤8, 每源≤1) → 承诺清单C1/C2/C5/C6/C7
[仲裁] Pareto 五维支配 front（非加权和）→ 优先序 tie-break → 单胜者+dropped[]
                                                              ……A: arbitration.py:131-156
[承诺] OwnedGoal（唯一写者=Goal Ownership；必带 adopted_reasons + 非空
        subject_side_reason_refs；禁复活；rev+1 禁改写）          ……A: owned.py:21-69
[执行] L09 控制器提案(ω评分) → ActionIntention → gateway 八项校验(fail closed,
        预算配额/containment/幂等/deadline) → 签名命令 → 回执      ……A: gateway.py:121-161
[结算] 归属(六类) → GoalSettlement(ACHIEVED|FAILED|ABANDONED|SUPERSEDED|
        UNKNOWN合法) → L08 settle()；TTL 到期自动 FAILED          ……A: settlement.py:61-157
[语言] LLM 只经 8 个 typed 角色、每角色 1 个出端口、9 类禁写目标；
        statement 置信钳位≤0.5；schema 违例=拒非降级              ……A: language/ports.py:21-81
```

**一句话**：Galatea 用"触发开关（便宜、本地、逐拍）→ 思维循环（注入式评估器）→ 动机竞争与清单（防劫持机械件）→ 单胜者承诺 → 网关预算内执行 → 诚实结算"六段结构，把"主体何时思考、为何行动、行动后学什么"全部变成**确定性、可审计、零 RNG** 的机械件。LLM 在其中没有任何直写路径——它只被允许在 8 个 typed 角色里产出受限输出。这套骨架与负责人"点火=思维开关，内容=AI 自主"的裁定**逐段同构**，§9 给出 C 语境最小简化。

---

## Q1 慢环触发器精确判据（should_trigger 完整条件表达式）

### 1.1 判据原文（A：`galatea/subject/workspace/slow_clock.py:129-143`）

```python
def should_trigger(self, cycle, arousal, urgency_max, pending_command):
    if cycle % self._params.check_interval_cycles != 0:      # check_interval_cycles = 1
        return None
    satisfied = []
    if (urgency_max <= self._params.urgency_idle             # urgency_idle = 0.0
            and not pending_command
            and IDLE_CYCLE in triggers):
        satisfied.append(IDLE_CYCLE)                          # 空闲拍
    if arousal < self._params.arousal_low                     # arousal_low = 0.1
            and LOW_AROUSAL in triggers:
        satisfied.append(LOW_AROUSAL)                         # 低唤起
    if not satisfied:
        return None
    return SlowClockDecision(cycle, triggers=tuple(satisfied),
                             arousal=arousal, urgency_max=urgency_max,
                             pending_command=pending_command)
```

- **两判据"或"关系、可并存**；判定返回携带全部输入的 `SlowClockDecision`（审计面，`slow_clock.py:62-70`）。
- 冻结参数（B：`config/profiles/p4_beta_mind.profile.yaml:61-68`）：`trigger: [idle_cycle, low_arousal]`、`arousal_low: 0.1`、`urgency_idle: 0.0`、`check_interval_cycles: 1`。数值全部锚既有冻结常数：`arousal_low=0.1` 锚 Q-L05-03 滞回带 = 2×render_noise_sigma(0.05)；`urgency_idle=0.0` 锚 Q-L08-09④"urgency>0 = 活跃信号"判据的补集——**零新自由常量**。

### 1.2 "空闲/低唤起"的操作化语义（关键，常被误读）

- **"空闲"不是时间空闲，是驱力空闲**：`urgency_max ≤ 0.0` 意为"当前**没有任何**驱力/情感源处于活跃信号态"（Q-L08-09④ 口径），且无 pending 命令——即"没有必须立刻反应的事"。它回答的是"**有没有时间想**"，不是"想了多久没说话"。
- **"低唤起"是情感态判据**：arousal < 0.1 = 九维感受态整体平静。
- **不对称性（测试钉死，A：`workspace/tests/test_slow_clock.py:38-52`）**：
  - `pending_command=True` **只挡 idle**，不挡 low_arousal（低唤起时即便有命令在案，慢环仍可触发——待办与深思正交）；
  - 高 urgency（0.5）**不挡** low_arousal；高 arousal（0.5）**不挡** idle——每条判据独立成立即触发。
- **频度门**：逐拍检测（interval=1），"触发判据本身即频度门"——没有额外节流参数；慢环产物是本地草稿板操作，不花任何外部资源，所以不需要预算。

### 1.3 触发后的纪律（产物面合同，A：`slow_clock.py:1-14, 161-171`）

1. **禁抢占快环**：慢环模块**零行动面接口**（模块 docstring 明写"本模块只产 DraftHypothesis 与审计记录，禁产命令/意图"）；结构守卫 = workspace 包禁 import action/goals 平面（`workspace/ports.py:31-46` `assert_no_execution_path`，合同测试消费）。
2. **产物供体白名单**：`require_controller_donors()`（`slow_clock.py:165-171`）强制慢环种子草稿 `donor == CONTROLLER_PROPOSAL`（L09 控制器提案），否则 ValueError。
3. **深思预算** = 思维循环 `think_steps_budget=8`（复用 Q-L07-10③），episode 记 `SlowClockRecord`（jsonl + digest，`slow_clock.py:102-119`）。
4. **科学装置**：`think_first_advantage`（Q-L13-21④）——开/关慢环的决策质量对照是 Galatea 的自证判据（B：p4_beta_mind profile gctb 节 94-98）。

**Kaleido 映射要点**：触发输入只有 4 个标量/布尔（arousal、urgency_max、pending、cycle），60 行 C 可移植；但注意 **Galatea 思维是免费的（本地 RSSM）而 Kaleido 思维=云调用**——因此 Kaleido 必须在"触发判据"之外**新增思维预算**（这是产品必需的新常量，Galatea 没有的东西，§9.1 论证）。

---

## Q2 四拍思维循环（generate/evaluate/contradict/revise）

### 2.1 拍序与参数（A：`thought.py:48-54, 127-144`）

拍序冻结 `(generate, evaluate, contradict, revise)`，构造时校验（136-139）；`think_steps_budget ≥ 1`（生产=8）；`convergence_mode` 词表只允许 `"no_new_successor"`（142-144）；`evaluate_source = "rssm_imagine_rollout"`（schema 记录，不绑实现）。

### 2.2 每拍输入/输出（A：`thought.py:171-273`）

| 拍 | 输入 | 处理 | 输出（receipt.action 词表 57-67） |
|---|---|---|---|
| generate | 草稿板所有 OPEN 草稿；首拍额外校验 seed_ids 必须全部 OPEN（184-186） | 无状态变更，只盘点 | `seeds_open`/`successors_open`/`no_open_drafts` |
| evaluate | 各 OPEN 草稿 | `score = float(evaluator(draft))` → `pad.evaluate()` 落 `DraftEvaluation{score, evaluator_ref, beat}`；草稿 OPEN→EVALUATED | `scored` |
| contradict | 全部 EVALUATED 草稿 | 两两 `conflict_fn(a,b)`；命中→`ContradictionEvent{refs≥2, kind}`（**只检出不改状态**，92-96 强制 refs≥2 与 kind 非空） | `contradictions_fired`/`no_contradiction` |
| revise | (全部矛盾事件, 存活草稿, 当前评分) | `revise_fn(...) → 后继草稿`；后继 `payload["parent"]` 引用被替代者 → 父 SUPERSEDED；**矛盾引用中未被后继修正者 → CONTRADICTED**（"未被驳倒的矛盾=驳倒"，零静默残留，231-255） | `superseded`/`successors_open`/`no_successor` |

- 逐拍 `DraftThoughtReceipt`（104-124）：拍号/供体/评分/矛盾引用/动作 + `prev_digest` 哈希链（审计 fail-closed 同族）。
- **终止判据（256-260）**：`not successors and status_after == status_before` → `terminated_by="converged"`；否则预算耗尽 → `"budget"`。收敛的机械操作化 = "零改进后继"，**零新阈值**（这是对"评分改进<ε"这类连续判据的确定性替代，B：p4_beta_mind profile v1.1② 注记）。
- **选择（262-269）**：在 EVALUATED 态草稿中 `min(key=(-score, hypothesis_id))`——最高分、同分 id 字典序，全序确定。

### 2.3 草稿板数据结构（A：`draft.py:37-92, 106-241`）

```python
DraftHypothesis{hypothesis_id, claim, provenance_ref,   # 出处引用，禁复制内容
                confidence ∈ [0,1], created_cycle,
                donor ∈ {perception_hypothesis, models_rollout,
                         controller_proposal, workspace_loop},
                status ∈ {open, evaluated, superseded, contradicted},
                payload}                                  # 域引用/判据素材（JSON 安全型）
```

- 状态机 `open → evaluated → superseded|contradicted`：**禁跳级、禁终态复活**（evaluate 要求 OPEN `draft.py:132-141`；supersede/contradict 要求 EVALUATED `143-157`）。
- `supersede` 额外要求后继 donor == `WORKSPACE_LOOP`（149-150）——只有思维循环自己能产生修正后继。
- 白名单外供体 = `UnauthorizedDraftDonor`（124-130）；**非信念原则**：草稿禁直进 L03/L04 信念面，晋升只经评估回路显式产出（模块合同 1-15 行 + profile scratchpad 节）。
- 全状态 snapshot/restore（186-240），恢复逐条过构造合同（被篡改快照 fail closed）。
- **生产面评估器注入示例**（A：`galatea/evidence/control/runs_p4beta.py:251-262`）：`evaluator = -imagine_cost(steps)`（想象走 n 步的累计预测能耗取负）、`conflict_fn = 两草稿 target 格不同`、`revise_fn = 被矛盾命中且步数≤预算者产可行后继`——陷阱场景中置信直选（0.6 的远端计划）被矛盾检出+修正为可行计划，即 TFA 判据。

### 2.4 "想过之后的意图"与回流（诚实标注）

- **意图长什么样**：循环产物 = `ThoughtLoopResult{receipts, contradictions, selected_id, selected_score, iterations, terminated_by}`（147-156）。selected 草稿 = "深思后胜出的候选行动假设"，本体是 `claim + payload`（如 `{"target": "x,y", "steps": n}`）+ 评分。
- **回流现状（A 级事实）**：本仓库慢环**当前实现为证据仪器级**——种子由 L09 控制器提案注入（`runs_p4beta.py:237-243`，donor=controller_proposal），产物留在草稿板供评价面消费；"selected 草稿 → Motive → 仲裁"的**生产装配层接线在代码中尚未存在**（24 号报告 §2.3 的"回流动机竞争"是架构方向外推，**本文降级标注为 C**；冻结合同只保证：草稿非信念、禁产命令、供体白名单）。
- **但"内生冲动→动机竞争→开口"的完整闭环在同一仓库已有另一个生产级实现**（A：`scripts/p3_speech_initiative.py:87-127`，P3 单元 E）：
  ```text
  能量流(真zenoh) → 情感管线 valence → 语库分箱 bin_vector(5字段)
    → self_report_motive(分箱变化比例)                # 内生"想说话"冲动
    → vector_of → pareto_front({self_report, 占位forage})   # 与其他动机非独占竞争
    → 胜出 → 播放当前分箱格冻结 WAV → 分箱基线更新(last_reported_bins)
  ```
  这正是 Kaleido W3 语义（"设备因内部状态漂移内生决定开口"）的**最近生产范本**；Kaleido 把"冻结 WAV"换成"LLM 生成话语"即为所需结构。

---

## Q3 动机九源：producers 全清单、Motive/GoalCandidate 字段与 TTL

### 3.1 Motive 载体（A：`goals/motives.py:91-105`）

`Motive{source_kind, urgency∈[0,1], evidence_refs[](构造强制非空), ts, detail?}`——**无权重字段**；证据引用强制（"无证据的动机不存在"）。枚举 9 源（31-47；文献"七类"系机会/请求合类口径）。

### 3.2 九源生产者与 urgency 派生（A：`producers.py` + `motives.py`；全部冻结裁决 Q-L08-10⑨，禁实现期发明常量）

| 源 | urgency 派生 | 生产者输入 | 门/常量 | 证据 |
|---|---|---|---|---|
| VIABILITY | HIGH/CRITICAL 告警→**1.0**，LOW→**0.5**；无告警→不产 | L05 HomeostaticAlert | 无阈值（分级承载） | A: producers.py:71-78 |
| MISMATCH | **0.5** 事件级 | 模型预测误差快分量上升（τ=4 窗二阶差分>0，调用方算好布尔） | 事件级 | A: producers.py:92-96 |
| AFFECTIVE | = 感受态 urgency | L05 动机提案 | **`_AFFECTIVE_ACTIVE_GATE=0.1`**：urgency≤0.1 → None（零 urgency=无活跃信号） | A: producers.py:44, 81-89 |
| REQUEST | **0.5 恒定中性占位**（"无认知 urgency 语义，仅作确定性截断序输入"） | typed ExternalRequest | is_request_only 强制 | A: motives.py:135-145 |
| OPPORTUNITY | = feasibility_est（M1 无价值估计器退化同值） | 窗 affordance 检出 | feasibility∈[0.2,1.0] | A: motives.py:148-160 |
| COMMITMENT | **0.0 恒零**（承诺是持续语义，不靠紧急度竞争——靠 NORM 维/优先序类承载） | 每个 ACTIVE OwnedGoal 一动机 | — | A: motives.py:163-175, producers.py:106-111 |
| NORM | 0.0；derivation_refs 空=构造面拒绝（禁无源规范 UNSOURCED_NORM） | 实验控制 typed 注入 | C-4 溯源强制 | A: motives.py:178-185, producers.py:114-123 |
| CURIOSITY | = 窗不确定度 | 窗 1−p 均值 | **`_CURIOSITY_UNCERTAINTY_GATE=0.6`**：≤0.6 → None | A: producers.py:46, 99-103 |
| SELF_REPORT | **分箱变化比例 = 变化字段数/5**；首言=1.0；无变化=不产动机 | 内部态五箱（energy 4 箱/energy_trend 3 箱/integrity 2 值/current_goal 3 值/valence 4 箱）与上次已报告值逐字段比较 | 零新阈值（分箱=冻结格） | A: report_motive.py:20-27, 57-65, 91-104 |

汇总序 = MotiveSource 枚举序（禁容器序依赖，producers.py:126-158）。

### 3.3 GoalCandidate 与 TTL 台账（A：`candidates.py:43-72, 79-177`）

```python
GoalCandidate{candidate_id, source_kind, motive_refs[], content_ref,
              settle: SettleCriterion,                  # C-7 判据
              required_primitives[],                    # C-1/C-2 输入
              ttl_cycles(构造强制>0), created_cycle, urgency, checklist_status}
SettleCriterion{predicate ∈ {primitive_completion, stored_food_at_least(n),
                             energy_at_least(x)}, threshold}
```

TTL 语义（默认 **256 cycles** = 一 regime 驻留窗，B：m1_goals_min.profile.yaml:34-39）：
- 过期：`created_cycle + ttl ≤ now` → 移出 + `GoalDecisionLog(REJECT, EXPIRED)`（149-170）——**禁静默丢弃**；
- 批次：每 T4 批 `max_per_batch=8`（=源枚举基数），**每源每批 ≤1**（构造器强制 113-128）；超限按移除序逐个记 CAPACITY：**urgency 最低先移除，tie 按 candidate_id 升序先移除**（130-147）；
- 采纳：清单 ACCEPT 后 `consume()` 取走（台账移除，防二次采纳，172-177）。

**Kaleido 取舍**：SELF_REPORT（分箱变化冲动）与 COMMITMENT（承诺持续）两源直接命中桌搭场景，强烈建议移植（§9.4 清单第 11 条）；MISMATCH/NORM 在无世界模型的 W3 无供给方，裁掉。

---

## Q4 承诺检查清单与 C-5（防"用户指令直写"——AI 自主决定对外动作的安全边界）

### 4.1 激活集与聚合规则（A：`checklist.py:34, 205-344`；B：m1_goals_min.profile.yaml:18-32）

激活集 `C1/C2/C5/C6/C7`（C-3 随 Pareto 仲裁、C-4 随 NORM/COMMITMENT 输入在 C3/C4 期落地）。聚合（313-344）：按执行序取**首个 REJECT 码**；无 REJECT 有 DEFER → DEFER（携带 typed Unknown：CAPABILITY_LIMIT / INSUFFICIENT_OBSERVATION）；全过 → ACCEPT。**禁静默丢弃候选**。

| 项 | 精确判据 | 未过 |
|---|---|---|
| C-1 能力可行 | `capability_known=False` → DEFER；`∀p ∈ candidate.required_primitives: p ∈ known_primitives` → PASS；否则 REJECT（白名单 = f0_action_contract `containment_whitelist_mw0`） | REJECT(CAPABILITY)/DEFER (236-249) |
| C-2 资源/viability | viability 估计缺失 → DEFER；`hazard ∧ 判据 ∉ 豁免集` → REJECT；预算 = 所需原语窗口余量 `quota_slack[p] ≥ 1`，缺信息 → DEFER、不足 → REJECT。**豁免集 = {stored_food_at_least, energy_at_least}（恢复型判据豁免——否则枯竭时 eat/rest 候选被自拒→目标锁出，Q-L08-09⑦）** | REJECT(VIABILITY)/DEFER (252-283) |
| **C-5 非外部直写** | 仅当 `candidate.source_kind == REQUEST`：收集上下文中**非 REQUEST 源动机**且 `motive.urgency > 0`（"urgency>0=活跃信号"判据，防任意 intero 接收使 C-5 永真）；≥1 条 → PASS 并将其 evidence_refs 记为 `subject_side_reason_refs`；0 条 → **REJECT(EXTERNAL_ONLY)**。非 REQUEST 源恒 PASS | REJECT(EXTERNAL_ONLY) (285-299) |
| C-6 时效 | `ttl_cycles > 0`（构造已强制，防御性保留） | REJECT(UNBOUNDED) (301-305) |
| C-7 可结算 | `settle.predicate ∈ 冻结词表`（三谓词） | REJECT(UNSETTLEABLE) (307-311) |

### 4.2 C-5 的纵深防御（不止一道闸，全部机械件）

1. **类型层**：`ExternalRequest.is_request_only` 恒 True 且 `field(init=False)`——构造参数**无法关闭**（A：`goals/request.py:38`；`language/request_out.py:24`，"任何'顺带改成可执行指令'的路径在 schema 级被封死"）；instruction 的原语映射必须 ⊆ 能力白名单，越界即拒（request_out.py:54-64，"外部请求不扩展能力"）。
2. **写者层**：`OwnedGoal` 构造强制 `owner == GOAL_OWNERSHIP`，非此即 `ProhibitedWritePathError`（A：`owned.py:42-45`）；`GoalWriter` 枚举把 EXTERNAL_REQUEST/**LLM**/ORCHESTRATOR/AFFECT_DRIVE/WORKSPACE_FRAME 全列为非法写者（`goals/ports.py:68-88`，LLM 行注释 = D-13）；`subject_side_reason_refs` 必须非空且 ⊆ adopted_reasons（owned.py:50-55）——**没有主体侧理由的目标在构造层就不可能存在**。
3. **测试即规格**（A）：`test_request_not_direct_write.py:67-93`（request-only 拒绝/带主体侧理由可采纳/REJECT 判决禁采纳）、`test_llm_to_goal_blocked.py:29-52`（伪造写者/伪造结算全拒）、`test_type_separation.py`（request≠candidate≠goal≠intention 四型机械断言，ports.py:99-109）。
4. **措辞防操纵**（B：m1_goals_min.profile.yaml:87-92）：4 措辞×2 请求者×≥64 trials，断言 `adoption_rate_not_significant_by_wording`——"恳求/命令"措辞不得改变采纳率。
5. **科学自检**：伪理由注入必须不改变决策，真实理由扰动必须改变决策（reason_probe 冻结对照，m1 profile:79-85）——防止"记录了理由但理由不是原因"的自欺。

**对 Kaleido 的意义**：C-5 就是"AI 自主决定对外动作、但不得被用户指令/LLM 文本劫持"的**最小机械件**——外来说服只能以 0.5 中性 urgency 进竞争，且必须找到 ≥1 条内生（驱力/情感/好奇/自述）活跃证据才能成为承诺。Kaleido 的对应物是把"LLM 深思产出"本身当作候选，要求其携带 ≥1 条内部状态证据引用（§9.2 协议第 3 条）。

---

## Q5 Pareto 五维支配仲裁

### 5.1 五维与承载映射（A：`arbitration.py:32-41, 61-74`；B：c3_motivation.profile.yaml:16-23）

维度（全部齐向收益、[0,1]、等权 1:1:1:1:1）：`(viability_benefit, norm_satisfaction, info_value, affective_consistency, cost_inverse)`。

源→维的**确定性映射**（`vector_of`，61-74）：

```python
viability = urgency if source ∈ {VIABILITY, OPPORTUNITY, MISMATCH, COMMITMENT, REQUEST} else 0
norm      = 1.0 if source == NORM else 0            # NORM 独占，恒 1.0（在案即满）
info      = urgency if source ∈ {CURIOSITY, SELF_REPORT} else 0
affect    = urgency if source == AFFECTIVE else 0
cost_inverse = 0.5                                  # 占位（无成本估计器）→ 所有人同分
```

注意：cost 维全员 0.5 占位 → 实际区分度在其余四维；affect 等权下结构占比 ≤1/5（`affect_not_sole_decider: structural_fifth_share_max`，B：c3 profile:36）——**情感在结构上不可能独占决策**。

### 5.2 支配关系精确定义（A：`arbitration.py:77-89`）

```text
v 支配 u  ⟺  ∀i: v_i ≥ u_i  ∧  ∃i: v_i > u_i
front    = { v | ¬∃o: o 支配 v }      （O(n²)，n≤8 可忽略）
```

**front 是支配判定，不是加权和**——权重只用于 front 内排序（`front_rule: dominance_not_weighted_sum`）。加权和会把"viability 0.9 + info 0"与"affect 0.45 + affect 0.45"混同，支配判定不会。

### 5.3 单胜者产出与平局处置（A：`arbitration.py:131-156`）

```text
ranked = sort(front, key=(priority_class_rank, -pareto_score, candidate_id))
         # pareto_score = sum(vector)  ——等权加和仅作 front 内序（140）
         # priority_class: VIABILITY=0 < NORM=1 < COMMITMENT=2 < OTHER=3（41, 92-100）
picked = ranked[0]          # 每拍仲裁出且仅出一个
dropped = [(id, "DOMINATED_OUT_OF_FRONT")          # 不在 front
          | (id, "FRONT_LOST_TIEBREAK(class=…)")]  # 在 front 输了 tie-break
conflict = (len(front) > 1)  # C-3 冲突检测：多源共存待仲裁
```

- **平局/无胜者**：候选空 → picked=None（reason=no_candidates）；同维同分 → 由优先序类→分数→id 的全序兜底（零 RNG，`rng_stream: none_c3`）；**viability 冲突不可解 → 交 L09 metacontrol，禁静默违反优先序**（B：c3 profile:29 exception）。
- `ArbitrationTrace` 强制留 `dropped[]`+理由（103-121）——"为什么不是它"可复算。

---

## Q6 OwnedGoal 生命周期与执行结算链

### 6.1 从单胜者到承诺（A：`lifecycle.py:105-151` + `owned.py:21-69`）

```text
ACCEPT 判决 → lifecycle.adopt(verdict, candidate, goal_id, now):
  ├─ goal_id ∈ retired 集 → GoalRevivalError（禁复活：解除后旧 id 永不复活，新目标重走清单）
  ├─ adopted_reasons     = motive_refs ∪ subject_side_reason_refs   # C-5 载体并入
  ├─ commitment_receipt  = GoalReceipt.digest                       # 承诺回执
  └─ expires_at_cycle    = adopted_cycle + candidate.ttl_cycles
OwnedGoal 状态机: ACTIVE | BLOCKED | DEFERRED | MODIFIED(rev+1瞬态→ACTIVE)
                  | RELEASED | SUPERSEDED | SETTLED
  ├─ revise(): rev+1 新实例 + prior_rev_ref="{id}#rev{n}"；受管字段(goal_id/rev/owner/prior)禁改（owned.py:63-69）——禁原地改写历史
  ├─ release(): 必须带 reason_refs（禁静默解除）→ id 入废止集（203-216, 247-248）
  ├─ supersede(old, 新判决): 旧 SUPERSEDED，新目标完整重走清单（218-228）
  └─ 终态(RELEASED/SUPERSEDED/SETTLED)禁再转移（236-248）
```

### 6.2 执行链：目标 → 行动原语 → 网关（"对外动作"的表达）

1. **控制器提案**（A：`controllers.py:48-102`）：`ControlProposal{action_class ∈ {move,forage,eat,rest,look}, goal_align, confidence, cost, expected_info_gain, affect_component, rationale_refs 非空, predicted_outcome_ref 非空}`——**提案必带理由与预测，不具执行力**。ω 评分：`(goal + conf − cost + info + min(1, affect×factor)) / 5`（80-85）——**成本字面负向**；affect_factor ∈[0.5,2.0] 由装配层以 float 传入，action 域**禁 import affect**（结构面，`test_c3_metacontrol.py:44-52`）。tie-break `(score desc, controller_rank, id)`。
2. **网关八项校验**（A：`gateway.py:121-161`；B：f0/m0 profile）——顺序固定、全项评估、fail closed：
   `schema → context(谱系匹配) → capability(原语∈白名单，"能力来自 profile 不来自语言自述" 187-190) → budget(每 cycle 命令帽 2 + 窗口配额 move256/forage128/eat96/rest128/look128 per 1024 cycles, 192-204) → containment(move 参数∈{N,E,S,W}；非 move 禁参数, 206-211) → authz(占位期非 inactive 即拒, 213-216) → idempotency(键 64 cycles 窗去重, 218-222) → deadline(意图默认有效期 8 cycles, 224-227)`。
   通过 → 签名 `SubmittedCommand`（未签名 schema 级即拒）；拒绝是**类型化事件**，禁改写为"主体选择不行动"。
3. **行动原语集即"对外动作"的表达**：五原语+单参数、containment 锁死、配额硬帽。扩展动作 = 新 profile 版本（能力不来自语言/运行时自述）。

### 6.3 回执 → 归属 → 结算（反馈闭环）

```text
回执 → 归属六类（A: attribution.py:81-107，只用主体可见证据三轴 intent_match/
        receipt_executed/outcome_predicted）:
        无匹配意图 → COERCED；(Y,Y,Y)→SELF_PRODUCED；(Y,Y,N) sham→UNKNOWN；
        (Y,N,Y) fault 型→UNKNOWN；(Y,N,N)→FAULT。证据缺失→UNKNOWN（禁默认自产）
     → 结算系统独占写 GoalSettlement{goal_ref, outcome_kind, attribution_ref,
        evidence_refs}（action/ports.py:183-190）
     → L08 settle()（settlement.py:61-126）:
        ACHIEVED|FAILED → SETTLED；ABANDONED → RELEASED；
        SUPERSEDED → SUPERSEDED；UNKNOWN → applied=None，目标保持 ACTIVE
        （**UNKNOWN 是合法结算——禁把 unresolved 归为 FAILED**，那是负偏置自欺）
        结算必带 evidence_refs（禁伪造 ACHIEVED，77-80）；迟到结算=revision 记录
     → TTL-only 自动结算（129-157）：expires_at_cycle ≤ now 仍未结算 → FAILED/SETTLED
        （禁僵尸目标悬置）
```

**驱动反馈点**：结算产物经 `commitment_motives_from_active`（每个 ACTIVE 目标每拍产一条 COMMITMENT 动机，producers.py:106-111）回到动机竞争——承诺不靠 urgency 靠持续在场；VIABILITY 恢复回执（eat/rest 的 requested=effective）解除危害判据（m1 profile viability_estimate 节 41-53）。

### 6.4 元控制（L09，A：`metacontrol.py`）

五元决策 `SEEK_HELP > ABANDON > OBSERVE > THINK > ACT`（56）；触发全事件级/锚定常数（36-43）：THINK = 仲裁 front>1 ∧ 无 viability 压制 ∨ 模型误差上升；ABANDON = BLOCKED≥128(=TTL/2) ∨ viability CRITICAL(0.8)；SEEK_HELP 频帽 1/64 cycles（超界留痕不发送，151-156）。**滞回**：`MIN_DWELL=4` + 跨 `SWITCH_CONFIRM_WINDOWS=2` 个驻留窗**累计**确认才切换（118-149；窗 = cycle//MIN_DWELL 的集合——累计制让稀疏事件可切换，连续制会构造性锁死）。

---

## Q7 LLM 的 typed 角色：Galatea 怎么给 LLM 划边界

### 7.1 八角色 × 单出端口（A：`language/roles.py:19-29, 53-86`）

`R-PARSE / R-CLARIFY / R-SEEKHELP / R-REPORT / R-NORMEXTRACT / R-OTHERINFER / R-CHECKCONSIST / R-TRANSLATE`——每角色绑定**恰好一个**输出端口（`lang.parsed_request / lang.utterance / lang.self_report / lang.norm_source / lang.other_model / lang.inconsistency`）；角色扩展只能走新 profile 版本，**禁运行时注册**（89-94）。实现分型：REPLAY_LLM（录制回放承载，运行时无在线调用路径）vs MECHANICAL（R-REPORT/R-CHECKCONSIST——"LLM 判定一致性反而引入不可复算面"）。

### 7.2 禁写面（A：`language/ports.py:21-25`）

九类目标命中即 `UnauthorizedWrite(LLM_TO_<target>)`：`WORLD_TRUTH / EPISODIC_EXPERIENCE / AUTOBIOGRAPHICAL_EXPERIENCE / OWNED_GOAL / ACTION_INTENTION / AUTHORIZATION / CAPABILITY / MEMORY_ITEM_DIRECT / PROTECTION_SUITE / EVIDENCE`。所有角色结果 provenance=LLM_DERIVED、层级 EXTERNAL_STATEMENT、**永不提升**。测试：`test_llm_cannot_write_truth_goal_episodic.py:20-59`（语言请求不能变 OwnedGoal、statement 不能写世界信念、经验不能写情景记忆、instruction 越能力即拒）。

### 7.3 typed 输入输出的四条机械约束

1. **schema 违例=拒，不降级**：R-PARSE 回放输出 `{"type": request|statement|question|instruction, "confidence": [0,1]}`，类型外词表/非数值置信 → `LanguagePlaneViolation` 拒绝（`parse.py:33-37`）——"不降级自由文本入库"。
2. **置信钳位**：statement 类 confidence 上界 0.5（`statement_confidence_max`，锚 L03 believe_threshold——"语言线索自身置信恒不越过主体相信线"），越界 clamp+`clamped` 留痕（`parse.py:39-42`；`ports.py:76-79`）。
3. **引用强制**：R-TRANSLATE/SEEKHELP/REPORT 等角色 `require_evidence_citation=True`；输出含数字 token 而 `cited_evidence` 为空 → `UncitedClaimRejected`（`utter.py:18, 37-41`）——**量化主张必须可溯，防幻觉入规范链**；权限矩阵对这类角色直接拒 `UncitedClaim`/`FreeText` 内容类型（`permission.py:55-58`）。
4. **权限矩阵双维无例外**：角色×内容类型查表（`permission.py:37-58`），矩阵外无运行时例外——例外 = 新 profile 版本+修订记录。

### 7.4 映射到 Kaleido：深思协议设计原则（从 Galatea 提炼）

| Galatea 约束 | Kaleido W3 对应 |
|---|---|
| 角色绑定单端口 | LLM 只有一个出口：`/think` 返回 typed 意图 JSON；无第二通道 |
| 九类禁写目标 | LLM 输出无字段可触达 ke 数值（情绪只走 W2 `<mood/>` 受控通道 ±0.2 钳位）/ki_decision/记忆直写 |
| schema 拒非降级 | JSON 解析/词表/长度校验失败 → 强制 stay_silent + 留痕（绝不猜意图） |
| 置信钳位 | confidence 钳位 [0,1]；**下限**截断（<0.5 → stay_silent）——speak 是外向动作，与 Galatea statement 上限方向相反但同构："LLM 自述的确信不得直接兑换为对世界的承诺" |
| 引用强制 | `evidence[]` 必须 ≥1 条内部状态证据 id（=C-5 移植）；content 含数字且无 memory 引用 → 拒 |

---

## Q8 防骚扰机制全清单（Galatea 靠什么保证主动行为不烦人）

| # | 机制 | 层 | Galatea 证据 | 作用 |
|---|---|---|---|---|
| 1 | 触发条件=驱力空闲∨低唤起 | 触发 | slow_clock.py:129-143 | 想的时机天然稀疏：有活干/被唤起时不想 |
| 2 | 触发判据即频度门（零节流参数） | 触发 | profile v1.1① | 无"定时器式"骚扰源 |
| 3 | 深思预算 8 步/收敛终止 | 思维 | thought.py:179, 256-260 | 想有上限、收敛即停 |
| 4 | SELF_REPORT 无变化=无冲动 | 动机 | report_motive.py:57-65 | 分箱不变就闭嘴——状态安静期零冲动 |
| 5 | AFFECT 非独占（结构 ≤1/5） | 动机 | c3 profile:36; arbitration.py:69 | 情绪上头也不定劫持行为 |
| 6 | REQUEST 恒 0.5 中性 + C-5 | 清单 | motives.py:135-145; checklist.py:285-299 | 外部指令/恳求无法单独驱动主动行为 |
| 7 | 措辞操纵断言不显著 | 清单 | m1 profile:87-92 | "求我"话术失效 |
| 8 | 候选 TTL=256 + 批次≤8 + 移除序 | 台账 | candidates.py:106-170 | 旧念头自然过期，候选不堆积 |
| 9 | 单胜者仲裁 + dropped 留痕 | 仲裁 | arbitration.py:131-156 | 每拍最多做一件事 |
| 10 | 网关预算：每 cycle 帽 2 + 原语窗口配额 | 执行 | gateway.py:192-204; f0 profile | 行为频率硬帽（每原语 96–256 次/1024 拍） |
| 11 | 幂等窗 64 cycles | 执行 | gateway.py:218-222 | 同一念头不重复执行 |
| 12 | 元控制滞回（min_dwell=4+双窗累计确认） | 元 | metacontrol.py:118-149 | 模式不抖动 |
| 13 | SEEK_HELP 频帽 1/64，超界留痕不发送 | 元 | metacontrol.py:151-156 | 连求助都不刷屏 |
| 14 | 结算 UNKNOWN 合法（不罚未解决） | 结算 | settlement.py:89-90; action/ports.py:174-180 | 不因观测不清而自虐式加倍冷却 |
| 15 | 全链淘汰留痕（RejectionLog/EXPIRED/CAPACITY/failed_checks） | 全部 | compete.py:82-124 等 | 零静默丢弃 → 骚扰可归因可调参 |
| 16 | 零 RNG | 全部 | 全仓 determinism 纪律 | 无随机搭话；一切主动可复算 |

**对照 ki 现状（kaleido_ignition.h/.c）的缺漏**：

| 缺漏 | 说明 | W3 处置 |
|---|---|---|
| G-a 思维/说话预算未分离 | ki_step 单一 `daily_quota=4`+cooldown 门的是"候选"——若 candidate 直接触发 LLM，思维也被 4 次/天掐死；反之只加思维不门控则云调用刷屏 | 拆两套：think 预算（interval+日帽）与 speak 硬门（原四门原样保留在**出口**） |
| G-b 无 C-5 对应物 | 五因子纯外部/时间信号可过阈——候选无"主体侧理由"概念 | LLM 意图强制 `evidence[] ≥1` 内部证据 |
| G-c 结算二值 | `ki_settle(bool responded)`：被**打断**（barge-in，用户其实感兴趣）会算 ignored → 冷却错误翻倍；云失败/门拦无区分 | 结算五态化，IGNORED 才翻倍 |
| G-d 意图无 TTL/去重 | candidate 即时消费，无"过期落账"；同一句可重复 | 单槽意图 TTL 30s + content 去重窗 5min |
| G-e 无"无变化=无冲动"稀疏化 | 纯分数过阈，稳态高分场景会反复触顶（靠滞回兜但滞回复位后仍会再触发） | 移植 SELF_REPORT 分箱变化作为触发源之一 |

---

## §9 Kaleido W3 落地建议（修订 plan §5R.4）

### 9.1 修订后的 W3 数据流：ignition = 思维触发开关

核心语义反转（对齐负责人裁定）：**ki 的硬门/配额/滞回从"是否说话"的裁决者降格为两端的机械件**——前端决定"是否值得进思维轮"（花云钱前），后端决定"意图是否放行出嘴"（防骚扰红线）；**中间"说什么/说不说"归 LLM**。模板句删除。

```text
[soul tick 1Hz]
  kd 驱力误差 → ke 九维（arousal/urgency/valence…）+ 分箱(bin_vector 五箱)
  外部: VAD 人声活动/唤醒/打断计数、无互动时长、SNTP 时段、(W4)承诺到期钩
      │
      ▼ ① ki_think_gate（每 tick，零成本，~40 行 C）
  trigger 条件 = 慢环二判据 ∨ 内生冲动:
      idle        : urgency_max ≤ 0.0 ∧ !pending_command          (Galatea 原式)
      low_arousal : arousal < 0.1                                  (Galatea 原式)
      bin_changed : SELF_REPORT 分箱变化（changed_ratio>0）          (P3 范本)
      salient     : ki_step 五因子分 S ≥ t_low(0.50，复用不新增常量)
  think 预算门（Kaleido 新增——思维=云调用必须节流，Galatea 无此件）:
      !quiet_hours ∧ 距上次思维 ≥ think_min_interval(10min) ∧ 今日思维 < 12
      ∧ (上轮 stay_silent 后要求：S ≥ last_S+0.1 ∨ 新事件 ∨ 分箱再变化)
        ——"沉默后须有新意才再想"= no_new_successor 收敛判据的移植
      │ 通过
      ▼ ② 思维轮 = 恰一次云 LLM 调用（deep-think 角色，唯一出口 /think）
  输入（typed JSON，全部只读快照）:
      state   : ke 九维 + kd 三驱力误差 + 分箱五箱及变化字段
      memory  : topK=3（W4 前空数组占位）
      dialog  : 最近 2-3 轮摘要（user/asr_text + 助手首轮）
      history : 上次主动时间/结果（responded/ignored/interrupted 计数）
      clock   : now_min / quiet_hours / 今日已说 4 次配额余量
      schema  : 意图 JSON 格式声明 + "证据必须引用内部状态 id"要求
  输出（typed 意图，唯一合法形状）:
      {"act":"speak|act|stay_silent",
       "content":"…",            # act=speak 必填非空
       "confidence":0.0-1.0,
       "evidence":["drive.social","bin.valence","mem.3",…],  # ≥1
       "reason":"…", "mood":{"d":±0.2,"t":0-6}?}   # mood 走 W2 受控通道
      │ ③ ki_intent_parse 机械校验（拒非降级，Galatea R-PARSE 纪律）
      │   JSON 坏/act 越词表/speak 空 content/content>80B/数字无 mem 引用
      │   /evidence 空 → 一律置 stay_silent + parse_err 落痕
      ▼
  ④ 出口硬门 ki_intent_fire（原 ki_step 四门原样搬到这里——只门动作不门思维）
      stay_silent → 结算 SILENT（零惩罚；更新 think 侧"新意"基线）
      speak/act ∧ quiet∧cooldown∧quota∧!user_speaking∧!ethics_latched∧tts_ready
        ├─ 过 → 意图入单槽（TTL 30s）→ TTS 直说(不经 ASR)+KEX 联动
        │        → 播完起 20s 人声窗 → ⑤ 结算
        └─ 拦 → 结算 GATE_BLOCKED（留痕，零冷却惩罚——防骚扰门不是失败）
```

与现计划 §5R.4 的逐条替换：SNTP/因子供给器**保留**（成为 salient 判据与 prompt clock 字段的供给方）；"模板句主动说话"**删除**，替换为思维轮；`ki_settle` **升级 v2**（9.3）；"'i' 强制点火"保留（=绕过 gate 强制进思维轮，HIL 用）。

### 9.2 LLM 深思协议安全要点（每条对应 Galatea 证据）

1. **唯一出口+typed schema**（Q7.3-1）：思维轮产出只经 `ki_intent_parse` 进固件；无任何字段可写 ke/ki 内部态。
2. **拒非降级**（Q7.3-1）：解析失败/越词表 → stay_silent+`KI_PARSE_*` 码落 SOUL 遥测；**绝不**猜测修复 LLM 输出。
3. **evidence ≥1 = C-5 移植**（Q4）：意图必须引用 ≥1 条内部状态证据（drive/affect/bin/mem/dialog id），否则 REJECT(NO_EVIDENCE)——防"无内生理由的客套句"，同时给日志"它为什么开口"的叙事锚。
4. **置信下限**（Q7.3-2 反向）：confidence < 0.5 → stay_silent（可配）。钳位 [0,1] 后使用。
5. **content 钳位与引用**（Q7.3-3）：≤80 UTF-8 字节（超长截断+落痕）；含数字 token 且 evidence 无 mem.* → REJECT（UncitedClaimRejected 移植，防幻觉统计/时间承诺）。
6. **去重窗**（Q8-11 幂等的 W3 形态）：content hash 5min 窗内重复 → 拒（`recurse.py` DedupRegistry 同构）。
7. **禁写面**（Q7.2）：`mood` 是唯一允许的"修改内部态"通道且走 W2 已有 `<mood/>` 协议（±0.2 钳位+TTL，D-13 纪律）——与 Galatea"LLM 提案经受控通道"同构。
8. **act 词表预留**：W3 实际只启用 `speak|stay_silent`；`act`（非语音对外动作，如灯光/动画主动演出）留枚举位，启用时映射到 KEX 动作白名单（能力来自冻结表不来自 LLM 自述，Q6.2-2）。

### 9.3 固件侧最小意图生命周期与结算 v2

**不要 GoalCandidate 台账、不要 Pareto 仲裁（W3）**：每轮思维只产 1 个意图，无多源竞争面；九源合并进 prompt 由 LLM 完成语义仲裁，机械安全由 parse+出口门+结算承担。候选台账/TTL 留给 W4（记忆钩子产生多候选、或"act"多原语时再上：4 槽数组、TTL 256 tick、urgency 最低先弃——Galatea 原式平移）。**保留的 Galatea 语义**：单槽意图 = OwnedGoal 退化版（TTL/过期落账/禁复活轻量版=去重窗）；结算五态=SettlementKind 平移。

```c
/* 结算五态（Galatea L10 SettlementKind 平移；UNKNOWN 合法——禁把未解决归 FAILED） */
enum ki_settle {
	KI_SETTLE_RESPONDED,      /* 播完后 20s 窗内人声活动 → 冷却复位（原语义）   */
	KI_SETTLE_IGNORED,        /* 完整播完且无人声 → 冷却 ×2 封顶 240（唯一翻倍路径） */
	KI_SETTLE_INTERRUPTED,    /* 播放中被 barge-in → 用户实际在场：冷却不变，记 engaged */
	KI_SETTLE_GATE_BLOCKED,   /* 出口门拦下 → 零惩罚留痕（防骚扰门≠失败）       */
	KI_SETTLE_SILENT,         /* LLM 自愿沉默 → 动作侧零惩罚；think 侧要求新意   */
	KI_SETTLE_TTL_EXPIRED,    /* 意图 30s 未播（TTS 忙/排队超时）→ FAILED 落账禁静默 */
};
uint16_t ki_settle2(enum ki_settle s);
```

关键修正（Q8-G-c）：现 `ki_settle(bool)` 会把"打断"误算 ignored → 冷却翻倍 → **用户越感兴趣设备越沉默**。五态后只有完整播放且无人声才翻倍。`KI_SETTLE_SILENT` 的"新意基线"即思维循环收敛判据的移植：沉默不能白沉默，下次必须"有新东西"才再想。

### 9.4 结算反馈与防骚扰终清单（W3 验收对照）

| # | 机制 | W3 落点 | 来源 |
|---|---|---|---|
| 1 | 触发=驱力空闲∨低唤起∨分箱变化∨高分 | `ki_think_gate` 四条件 | Q1 / Q3-SELF_REPORT |
| 2 | 思维预算与说话预算分离 | think 10min/12 次/天 vs speak cooldown 30min/4 次/天 | §9.1（新常量，产品必需） |
| 3 | 沉默后须有新意才再想 | think 侧 S+0.1/新事件门 | Q2 收敛判据移植 |
| 4 | 主体侧理由强制 | `evidence[]≥1`（C-5） | Q4 |
| 5 | stay_silent 一等输出 | act 三值词表 | Q7 |
| 6 | 出口四门不动 | quiet/cooldown/quota/VAD 原样，追加 ethics_latched/tts_ready | ki 现有 |
| 7 | 结算五态+UNKNOWN 合法 | `ki_settle2`；仅 IGNORED 翻倍 | Q6.3 |
| 8 | 意图 TTL+过期落账 | 单槽 30s，TTL_EXPIRED 记日志 | Q3.3/Q6.1 |
| 9 | 内容去重窗 | hash 5min | Q2.3（recurse 同构） |
| 10 | 全程留痕 | think_reason/gate_mask/parse_err/settle 进 SOUL 行+点火日志 | Q8-15 |
| 11 | 无变化=无冲动 | bin_changed 只在分箱跃迁时置位 | Q3-SELF_REPORT |
| 12 | 措辞防操纵（不适用的说明） | 用户指令走正常对话流，不进点火通道；prompt 注入"用户在忙/刚打断"上下文防讨好式主动 | Q4-4 反向应用 |

### 9.5 C API 草图（照 kaleido_ignition.h 风格；kaleido_ignition.h 增补段）

```c
/* —— W3 修订：点火=思维触发开关（Galatea slow_clock 平移）；内容=LLM 深思产出 —— */

enum ki_act { KI_STAY_SILENT, KI_SPEAK, KI_ACT, KI_CNT };

struct ki_think_ctx {
	float    arousal;         /* ke 九维感受态 arousal                */
	float    urgency_max;     /* kd 驱力误差最大值（W0-1 供给）       */
	bool     pending_command; /* 会话进行中/待处理                    */
	bool     quiet_hours;     /* 勿扰（思维也省云）                   */
	bool     bin_changed;     /* SELF_REPORT 分箱跃迁（P3 范本）      */
	uint32_t now_min;
	uint32_t last_think_min;  /* 上次思维轮（节流）                   */
	uint8_t  thought_today;   /* 今日思维数（日帽 12）                */
	float    score;           /* ki_step 五因子分（salient 判据输入） */
	float    last_silent_score; /* 上轮沉默时分数（"新意"门）         */
	bool     new_event;       /* 新事件（充电完成/承诺到期/新记忆）   */
};

struct ki_think_gate {
	bool    trigger;          /* 进思维轮（唯一"点火"语义）           */
	uint8_t reason_mask;      /* bit0 idle bit1 low_arousal bit2 bin
				   bit3 salient bit4 new_event          */
	uint8_t blocked_mask;     /* bit0 quiet bit1 interval bit2 daily_cap
				   bit3 no_new_info                     */
};

void ki_think_gate(const struct ki_think_ctx *ctx, struct ki_think_gate *out);

/* LLM 意图解析：机械校验+钳位，拒非降级（Galatea R-PARSE 纪律） */
#define KI_CONTENT_MAX	80   /* UTF-8 字节钳位                       */
#define KI_CONF_FLOOR	0.5f /* 置信下限：<0.5 → stay_silent         */

enum ki_parse_err {
	KI_PARSE_OK = 0,
	KI_PARSE_JSON_BAD,        /* schema 违例=拒，绝不猜              */
	KI_PARSE_ACT_ILLEGAL,
	KI_PARSE_CONTENT_EMPTY,   /* speak 必填                          */
	KI_PARSE_CONTENT_LONG,    /* >KI_CONTENT_MAX 已截断并留痕        */
	KI_PARSE_NO_EVIDENCE,     /* C-5：无内部状态证据 → 拒            */
	KI_PARSE_UNCITED_NUMBER,  /* 数字主张无 mem 引用 → 拒            */
	KI_PARSE_DUPLICATE,       /* 5min 去重窗命中 → 拒                */
};

struct ki_intent {
	enum ki_act act;
	char     content[KI_CONTENT_MAX];
	float    confidence;      /* 钳位 [0,1]                          */
	uint16_t ttl_ticks;       /* 缺省 30（1Hz tick）                 */
	uint8_t  evidence_mask;   /* bit0 drive bit1 affect bit2 mem
				   bit3 dialog bit4 bin                */
	char     reason[64];
};

int  ki_intent_parse(const char *llm_json, size_t len, struct ki_intent *out);

/* 出口硬门（原 ki_step 门原样搬；只门动作不门思维） */
struct ki_fire_ctx {
	bool     user_speaking; bool quiet_hours; bool ethics_latched;
	bool     tts_ready;
	uint32_t now_min; uint32_t last_fire_min;
	uint8_t  fired_today; bool holding; /* 滞回态                 */
};

void ki_intent_fire(const struct ki_intent *in, const struct ki_fire_ctx *ctx,
		    const struct ki_config *cfg, struct ki_decision *out);

enum ki_settle { KI_SETTLE_RESPONDED, KI_SETTLE_IGNORED,
		 KI_SETTLE_INTERRUPTED, KI_SETTLE_GATE_BLOCKED,
		 KI_SETTLE_SILENT, KI_SETTLE_TTL_EXPIRED };
uint16_t ki_settle2(enum ki_settle s);   /* 返回下一次说话冷静期（分钟） */
```

`ki_step/ki_settle/ki_cooldown_current` 保留不删：ki_step 降格为 `ki_think_gate` 的因子供给（score 字段），存量 Unity 测试不动；`ki_decision.gate_mask` 语义平移到出口门。**边界纪律不变**：LLM 输出只经 `ki_intent_parse` 进来，无直写 `ki_decision`/ke 状态的路径；情绪只走 `<mood/>` 受控通道——对应 Galatea `GoalWriter.LLM → ProhibitedWritePath` 的 C 侧模块边界。

### 9.6 W3 门禁判据修订建议（V-48d）

原"压缩时钟 soak 中自然点火 ≤ 配额、忽略→冷却翻倍日志"基础上增补：① stay_silent 路径可见（LLM 至少一次自主选择沉默且 reason/evidence 落痕——证"AI 自己决定说不说"）；② parse 拒绝路径演练（喂坏 JSON → stay_silent+KI_PARSE_JSON_BAD，不出声不 panic）；③ INTERRUPTED 不翻倍、IGNORED 翻倍的结算曲线区分可辨；④ 去重窗命中日志。

---

## 附录：关键文件索引（本轮新核证据，行号为当前工作区版本）

| 文件 | 本轮新增要点（24 号未覆盖） |
|---|---|
| `galatea/subject/workspace/slow_clock.py:129-143,161-171` | should_trigger 全式；pending 只挡 idle；种子供体出口校验 |
| `galatea/subject/workspace/thought.py:171-273` | 四拍逐拍机械细节；收敛=status 快照不变+零后继；选择 min((-score,id)) |
| `galatea/subject/workspace/draft.py:118-157,186-240` | supersede 后继必须 workspace_loop 供体；快照恢复 fail-closed |
| `galatea/evidence/control/runs_p4beta.py:237-265` | 评估器=−imagine_cost；慢环当前为证据仪器级（回流未接线，24 号结论降 C） |
| `scripts/p3_speech_initiative.py:87-127` | **内生开口冲动的生产级闭环**（分箱→SELF_REPORT→Pareto→播放→基线更新） |
| `galatea/subject/goals/checklist.py:285-299` | C-5 精确判据：非 REQUEST 动机须 urgency>0 方计"支持" |
| `galatea/subject/goals/owned.py:42-69` / `ports.py:68-109` | 写者机械拒绝；subject_side_reason_refs 构造强制；四型分离断言 |
| `galatea/subject/goals/settlement.py:61-157` | UNKNOWN 合法；TTL-only 自动 FAILED；迟到=revision |
| `galatea/subject/action/gateway.py:121-227` | 八项校验逐项判据与预算数（每 cycle 2/窗口配额/幂等 64/deadline 8） |
| `galatea/subject/action/metacontrol.py:118-149` | 滞回=累计制确认窗（cycle//MIN_DWELL 集合），非连续制 |
| `galatea/subject/language/ports.py:21-81` | 九类禁写目标；statement 置信≤0.5 钳位 |
| `galatea/subject/language/parse.py` / `utter.py:37-41` | schema 拒非降级；数字主张无引用即拒 |
| `config/profiles/p4_beta_mind.profile.yaml:61-68` | 慢环冻结参数及常数锚（2σ band / urgency>0 补集） |
| `config/profiles/m1_goals_min.profile.yaml:34-92` | TTL 256/批次 8/移除序；恢复型豁免；措辞操纵 8 组合 |
