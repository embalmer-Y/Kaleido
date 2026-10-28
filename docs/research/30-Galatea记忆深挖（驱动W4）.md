# 30 Galatea 记忆深挖（驱动 W4）

| 版本 | 日期 | 作者 | 备注 |
|---|---|---|---|
| v1.0 | 2026-10-28 | ZCode 子智能体（调研） | 应 W4"记忆→人格连续性"细化需求，源码级深挖 Galatea L06 memory 子系统；只读调研，未改动 /home/emb/project/Galatea 任何文件；信息访问日期 2026-10-28 |

> 调研问题：Galatea（/home/emb/project/Galatea，Python，MIT）L06 记忆子系统的事件形成/检索打分/巩固遗忘/晋升/重放/动机接口/容量治理七问，逐条给出 file:line 级源码证据，产出 kaleido_memory（km）W4 增补方案。
> 证据分级：**A** = 源码 file:line（本调研逐文件通读）；**B** = 文档（design/lld/L06_memory.md、config/profiles/*.yaml、design/open_questions.md、24 号报告）；**C** = 设计推断（明确标注）。
> 对照基线：21 号报告（kaleido_memory 设计出处）、24 号报告 §4.1（L06 简要盘点）、M2 计划 §5R.5（W4 现计划）、kaleido-fw/components/kaleido_memory/（h + c 已核）。

## 结论先行（≤12 行）

1. Galatea L06 = 八子系统 + 六类来源强制 + 六态状态机的 append-only 情景库，**没有"core 块"**；"人格事实沉淀"由 **semantic 抽象**（源必须是在库 EPISODIC，谱系断裂即拒）+ **TABLE 类 LearningProposal 走 promotion** 承担，且**巩固永不直写记忆库**（ADR-G-0020"记忆通路无模型"：无 LLM、无 embedding，摘要=确定性 Laplace 合并 α=8）。
2. 检索打分两套并存：M2 冻结面 `score=strength`（禁改）；C2 面**五分量等权** `cue_sim + strength + recency + |valence| + reliability`，`recency=max(0,1−age/64)` 线性窗、`reliability=strength`、cue=枚举匹配 0/1、tie-break `(score desc, subsystem, id)`、top_k=4——全部可手算零向量。
3. 重要性演化机制齐备且极简：初值 0.5 → 每次检索命中 **+0.125（上界 1.0）** → 每 T6 拍 ×(1−1/τ)（episodic τ=256）；M2 简化为每拍 ×0.9。**"越回忆越牢、久不用渐忘"一阶动力学即可实现，我们 km 的静态 importance 是最大缺口。**
4. 遗忘=显式状态转移：FORGOTTEN + tombstone 留痕（行保留可审计、默认退出检索集），不是删除；容量溢出=最旧 LABILE→ARCHIVED，全序 `(last_access_asc, item_id_asc)`，PROTECTED 不可降级（ProtectionViolation）。**M2 编码面容量满即拒写（异常），C2 面才是降级**——两段演进值得照抄。
5. 重放（做梦巩固）确认**仅合同冻结**（Q-L06-09，2026-10-17）：触发=静息期、产物=模型候选走学习门（做梦不直写任何面）、判据=睡眠增益；源码仅 P4-γ 探针级 `dream_samples()`（runs_p4gamma.py:263，缩规模冒烟），subject 运行时无实现。已实现的 `replay_select` 是**给 L11 学习选训练样本**（等权 1:1:1:1，每 T6 8 条，只读），不是"回忆给用户"。
6. 记忆→动机的接口比我们设想得**间接**：novelty=感知残差低通、belief_uncertainty=信念不确定度低通（均来自感知/世界模型，**非记忆**）；记忆参与动机靠三条边——情感标注提升检索分（δ）、T3 向工作空间供候选（显著性分量）、MEMORY_ENCODER 调制目标（枚举在位但 C2 未启用）。24 号"慢环种子=记忆 Top-3"是**我们的方案**，Galatea slow_clock.py 无记忆引用。
7. 容量纪律核心：容量全部 2 的幂（M2 episodic 128 → C2 episodic 1024），溢出降级而非拒写/删除，tombstone 域内永久。对 960KB littlefs：建议上限 1500 条 + 80% 触发压缩（低重要度旧条目降为摘要行），tombstone 随压缩收割，备份不占内部分区（走 USB/网络，21 号方案 B 精神）。
8. 对 W4 的最小增补 = 三个 API（访问增强、惰性衰减、晋升 core）+ 检索命中回写 + prompt 注入格式；明确不移植八子系统/metamemory/做梦重放/谱系签名（§8.5）。

---

## §1 调研对象总览

`galatea/subject/memory/` 全 11 文件 1053 行 + tests/ 11 件 691 行，全部通读【A】：

| 文件 | 行数 | 职责 | 交付期 |
|---|---|---|---|
| ports.py | 88 | 来源六类/八子系统/六态枚举、RecalledContent 层级守卫 | M2 |
| store.py | 195 | subject_memory.db 单写者、episodes 单表、m2 profile typed loader | M2 |
| items.py | 127 | MemoryItem、连续性签名、身份主张分型 | M2 |
| encode.py | 104 | working/episodic 编码、强度初值、M2 衰减 | M2 |
| retrieve.py | 119 | M2 冻结检索 + C2 五分量检索 | M2+C2 |
| forget.py | 139 | τ 幂梯衰减、访问增强、tombstone、溢出降级 | C2 |
| affective.py | 50 | AffectiveValueTag 唯一写者落盘 | C2 |
| semantic.py | 45 | 语义抽象（谱系强制防伪抽象） | C2 |
| prospective.py | 58 | 前瞻提醒（源限 L08 OwnedGoal） | C2 |
| metamemory.py | 65 | RECALLED/PERCEIVED 混淆 2×2 计数 | C2 |
| consolidate.py | 63 | T6 类型级 Laplace 合并 → LearningProposal | C5 |

演进三段：**M2 最小切片**（working+episodic 两子系统，连续性对照）→ **C2 次单元**（五子系统 + 检索/遗忘/情感标注全量，2026-10-06 裁决批）→ **C5**（巩固/重放/保护集/promotion，2026-10-08 裁决批）。测试即规格：test_c2_retrieval_weights.py（手算锚）、test_forget_tombstone.py（衰减/增强/tombstone/溢出全行为）、test_m2_continuity_contrast.py（四臂签名两两可辨）。

---

## §2 Q1 情节（episode）形成：条件、写入路径与数据结构

### 2.1 成为情节的条件——两条门，无显著性打分器

Galatea **没有独立的"显著性门"模块**，情节准入靠两条硬规则：

- **来源门（铁律）**：`SourceType.LANGUAGE_INPUT` 禁写 episodic/autobiographical 经历面——store 层机械拒绝【A：store.py:49-55】，encode 层二次拒绝【A：encode.py:56-59】，测试 test_language_to_episodic_blocked.py 锁死。即"语言说的事"永远不构成"亲历的事"（D-13 纪律族）。prospective 同族：语言非承诺来源，`goal_ref + origin_receipt` 双强制【A：prospective.py:37-44】。
- **节拍门（M2 实测路径）**：正式 runner 里 episodic 编码发生在 **T6 节拍（cycle % 64 == 0）且当拍有输入包**时批量写入；**显著事件（energy_depleted 危害）则当即编码、不等节拍**【A：evidence/control/runs.py:813-825】。即：常规经历按时间片采样落盘 + 高显著事件即时落盘——**显著性以"事件类别"显式表达，不是连续打分**。

LLD 层面的合同口径：`mem.encode_episodic(ep)` 的合法来源 = L09/L10（经历切片）+ L02/L03 帧引用，EpisodeSlice 携带 `activity_window_id, frames[], beliefs[], actions[], outcomes[], attributions[], ts`，**必须引用事件 id、禁拷贝真值**【B：design/lld/L06_memory.md §2.1 表】。L05 可经 MEMORY_ENCODER 调制编码强度，但 C2 未启用（`affect_modulation_c2: none`）【B：c2_memory.profile.yaml:40；A：modulation.py:44 目标枚举在位、issue_modulations enabled 缺省不含它（modulation.py:181）】。

### 2.2 写入路径与全字段

存储：SQLite 单表 `episodes`（列式索引 + payload JSON），`item_id` 主键，MemoryStore 独占写【A：store.py:36-40】。条目结构 MemoryItem【A：items.py:25-48】：

```python
MemoryItem(
  item_id: str, subsystem: MemorySubsystem,          # 八子系统枚举（ports.py:35-45）
  source_type: SourceType,                           # 六类强制（ports.py:24-32）
  content_ref: str,                                  # 大对象只读引用（禁内联真值）
  representation_level: RepresentationLevel,
  strength: float,                                   # [0,1]，初值 = profile.strength_initial = 0.5
  created_cycle: int, last_access_cycle: int,
  consolidation_state: ConsolidationState,           # LABILE→…→FORGOTTEN 六态（ports.py:48-56）
  lineage_refs: tuple[str, ...],                     # 源事件 id / checkpoint digest（连续性证据链）
  affective: AffectiveValueTag | None,               # 情感标注（L06 唯一写者落盘）
  access_count: int,                                 # 访问计数（审计面）
)
```

编码函数【A：encode.py:15-76】：容量检查（`count >= capacity` 即 `MemoryStoreViolation`，M2 期语义=拒写）→ 构造条目（strength=0.5、双时间戳=created_cycle）→ `store.put`。谱系锚：M2 场景每 episode 边界打 checkpoint（`lineage_refs=(boundary_anchor,)`）【A：runs.py:817】。

**我们缺的对应物**：km_append 无来源类型、无容量门、无谱系——在"单一用户、全部来源可信"的产品语境可接受，但建议至少保留一个简化 `origin` 字段（`user`/`self`（主动点火说的话）/`system`），为将来"设备不该把 TTS 模板句当成亲历"留防线（对应 D-13 精神，见 §8.5）。

---

## §3 Q2 检索与打分：公式全貌与 km_query 差距

### 3.1 M2 冻结面（签名禁改）

`retrieve`：`sorted(items, key=(-strength, item_id))` 纯强度排序，top_k=4；空命中返回 `Unknown(INSUFFICIENT_OBSERVATION)`——**禁静默空列表冒充命中**【A：retrieve.py:30-57】。冻结原因：SC6 正式 runner I1 复算依赖【B：retrieve.py:3-5 docstring】。

### 3.2 C2 五分量等权面（Q-L06-02 裁决）

```python
# retrieve.py:69-88【A】
cue_sim    = 1.0 if any(c in {content_ref, *lineage_refs, subsystem名} for c in cues) else 0.0  # 枚举匹配
recency    = max(0.0, 1.0 - (now - last_access_cycle) / 64.0)   # 线性窗，窗长=T6 拍，零新常数
valence_abs= 0.0 if affective is None else abs(affective.valence)  # 无标注缺省中性
score      = cue_sim + strength + recency + valence_abs + strength  # reliability=strength（M2 映射）
```

- 权重 1:1:1:1:1（c2_memory.profile.yaml:34 `weights: {cue_sim: 1.0, ...}`）【B】；
- tie-break 全序 `(score desc, subsystem, memory_id)` 保证跨平台确定性【A：retrieve.py:98-99】；
- 手算锚（测试即规格）：strength=0.5、age=0、无 cue、无标注 → score=0.5+1+0+0.5=**2.0**【A：test_c2_retrieval_weights.py:37-47】；age=32→recency 0.5、age≥64→0【A：同文件:50-59】；标注 valence=−0.8 → score 差分 +0.8【A：同文件:75-89】；
- 输出 RecalledContent 的 `level` 恒 RECALLED，构造面机械禁提升为知觉/事实【A：ports.py:63-88】——下游（含 LLM）只能以"回忆"语气消费；
- 检索挂点：T4 拍服务 L03/L04/L07/L08/L09 查询【B：L06_memory.md §5；m2_continuity.profile.yaml:32 `retrieval_cadence: per_T4`】。

### 3.3 与我们 km_query 三因子的差距

我们现行公式【A：kaleido-fw/components/kaleido_memory/include/kaleido_memory.h:50；src/kaleido_memory.c:218-223】：

```c
recency = clampf(1.0 - age_days/30.0, 0, 1);        /* 30 天线性窗 */
score   = 0.5*recency + 0.3*clampf(importance,0,1) + match;   /* match ≤ 0.2：tag 命中+0.2、keyword+0.2 上界封顶 */
```

| 维度 | Galatea C2 | km 现状 | 差距评估 |
|---|---|---|---|
| 时间项 | `max(0,1−age/64)`，锚 T6 拍；age 自 **last_access** 起（越回忆越"新"） | `1−age_days/30`，锚墙钟；age 自 **created** 起（不可刷新） | **结构性差异**：Galatea 的 recency 随回忆刷新（last_access 更新），与访问增强形成正反馈；km 是单调陈旧化 |
| 重要性 | `strength` 动态量：初值 0.5，命中 +0.125，每 T6 ×255/256 | `importance` 静态量：写入时定，缺省 0.3，云端打分回填 | **km 最大缺口**：无"越回忆越牢"通道（§8.1 API-1 补） |
| 相关性 | cue 枚举匹配 0/1（等权 1 份） | tag/keyword 命中 0~0.2（弱权） | km 的 0.2 封顶使"命中"几乎不影响排序——Galatea 等权设计下命中一票顶 strength 一票；建议 km 提到 0.3~0.4 或改等权归一 |
| 情感分量 | \|valence\| 独立 1 份 | 无 | km schema 无情感字段；W4 顺手加（§8.2） |
| 确定性 | tie-break 全序 + 空命中 Unknown | 无 tie-break（同分按扫描序）；空命中返回 0 | 低危但建议补 `(score desc, seq asc)` |
| 可靠度 | reliability=strength（等价复用） | 无 | km 场景单一来源，可不做 |

**裁决建议（C，对照 Galatea）**：km 保持"本地粗排可手算"路线正确（与 24 号 §4.1 分层结论一致：设备端 Galatea 式、云端 cos_sim 精排）；改三点——① recency 基准从 created 改 last_accessed（命中即刷新）；② 增加 strength 动态量（惰性衰减+命中增强，§8.1）；③ 命中权重上调并补 tie-break。

---

## §4 Q3 巩固（consolidation）与遗忘：T5/T6 拍上做什么

### 4.1 拍结构

T5（每 16 拍）= 响应 L11 `ReplayRequest`（只读产出重放候选，见 §6）；T6（每 64 拍）= `consolidation_tick`，**巩固/遗忘/语义抽象的唯一触发点**【B：L06_memory.md §2.1 端口表、§5 调度表；c5_continued_learning.profile.yaml:34 `trigger: every_t6`——**不设强度阈**（强度只影响重放选择权重，不影响巩固资格，Q-L06-04 裁决【B：open_questions.md:144】）】。

### 4.2 衰减（遗忘的物理层）

两代实现并存：

- **M2 简化版**：每 T6 拍 `strength ×= 0.9`（decay_factor_per_t6），仅作用 LABILE 条目【A：encode.py:79-104；m2_continuity.profile.yaml:30】。
- **C2 τ 幂梯版**：每 T6 拍 `strength × (1 − 1/τ_sub)`，τ 谱 = working 4 / episodic+affective_value 256 / 其余 1024；PROTECTED 豁免、FORGOTTEN 跳过（tombstone 稳定不衰减）【A：forget.py:41-63；B：c2_memory.profile.yaml:43-50】。测试锚：τ=256 → 1.0×255/256 逐位【A：test_forget_tombstone.py:41-52】。

半衰期直观量级：衰减算子**每 T6 拍执行一次**（consolidation_tick 唯一触发），episodic τ=256 → 每次 T6 ×0.9961（衰 0.39%），≈ 每 64 cycle 衰半个百分点——千拍尺度缓降；真正的"保鲜/遗忘"分野来自访问增强 +0.125/次与 last_access 刷新（被回忆的条目 recency 归零重启、strength 回升，久不回忆者单调缓降）。

### 4.3 重要性演化（"随回忆提升、随时间衰减"的精确机制）

- **提升**：检索命中即 `boost_on_access`：`strength = min(1.0, strength + 0.125)`，同时刷新 `last_access_cycle`、`access_count += 1`【A：forget.py:66-83；测试 test_forget_tombstone.py:83-100（0.95+0.125→1.0 clip）】。步长 0.125 锚 Q-L04-10 Laplace 等价步长先例——**被回忆的记忆更牢、更"新"（recency 基准也刷新）**。
- **衰减**：见 4.2。PROTECTED（保护集）免疫；情感标注通过 `AffectiveValueTag.persistence` 决定 AFFECTIVE_VALUE 伴生条目初始强度【A：affective.py:43】，并经 δ 分量永久提升该条目检索分（|valence| 不衰减）。
- **晋升初值**：情感事件天然更牢——persistence 高 → 伴生条目 strength 高 → 重放选择优先（§6）。

### 4.4 "遗忘"的实现方式：状态转移而非删除

- `forget_item`：置 `FORGOTTEN` + `lineage_refs += ("tombstone:{reason}",)`，**行保留**（可审计"曾经记得但忘了"），幂等；默认检索集排除（`list_subsystem` 过滤 FORGOTTEN/ARCHIVED）【A：forget.py:86-107、store.py:91-104】。
- 容量溢出 `enforce_overflow`：超容量 → 最旧非 PROTECTED 的 LABILE 条目 `ARCHIVED`，降级序 `(last_access_cycle asc, item_id asc)` 确定性全序，留痕 `"demote:overflow"`；候选不足（全是 PROTECTED）→ `ProtectionViolation` 拒绝【A：forget.py:110-139；测试:103-135】。ARCHIVED 退出活跃检索集但行还在（可"回忆更早的事"时显式 include）。
- tombstone 保留期 = run 生命周期内永久（MW0 per-trial 库天然有界）；库体积治理工具属 H1 平台硬化，C5 只锁"不删除"语义【B：open_questions.md:146 Q-L06-07】。

### 4.5 T6 巩固流水线（C5）——重放/衰减/摘要升级各做什么

LLD 固定顺序【B：L06_memory.md §4.4】：

```
选取候选（LABILE 且被重放选中——无强度阈，Q-L06-04）
→ 语义抽象（同类情景 → SEMANTIC 条目，parent_digests 指向源情景）
→ 程序性提炼（成功经验 → PROCEDURAL，仅供 L09 提案参考，不写策略权限）
→ 保护集检查 → 状态迁移 + ConsolidationReceipt
```

实现面（consolidate.py）是**类型级确定性 Laplace 合并**：`merged = (α·prior + positives) / (α + count)`，α=8 冻结【A：consolidate.py:14,32-36】；"同 patch 类型 × 同 regime 的经验计数"（Q-L06-04②），**无向量聚类、无 embedding**（ADR-G-0020"记忆通路无模型"架构结论）【B：c5_continued_learning.profile.yaml:35-37 注释】。产物 = `LearningProposal(kind=TABLE)` **走 promotion，永不直写记忆库**【A：consolidate.py:39-63 docstring"巩固永不直写"】。

---

## §5 Q4 情节→core 的晋升：Galatea 的"人格事实沉淀"

**Galatea 没有 core 块**（无 MemGPT 式常驻人格层）。对应物有三条，全部无 LLM：

1. **semantic 抽象（最直接对应 km_core_write）**：`encode_semantic(store, item_id, content_ref, source_item_ids, ...)` 强制全部源为**在库 EPISODIC** 条目，`lineage_refs = source ids`，缺源/源不在库 → `LineageBreak`（"禁无源抽象"/防伪抽象）【A：semantic.py:15-45】。source_type=SELF_GENERATED（主体提炼），PERCEIVED 链由谱系承载。**这就是"情节→结论"的晋升通道，且晋升体必须可回溯到源情节**。
2. **类型级巩固（§4.5）**：批量情节 → 统计规律（TABLE 提案）→ promotion 门（`pareto_with_protection_hard_constraint`，sealed held-out `min_paired_d ≥ 0.5`、任一维退化 ≥0.5 即拒）【B：c5_continued_learning.profile.yaml:41-47】——晋升要有"held-out 不退化"证据，不是拍脑袋。
3. **autobiographical 自传体**：自我连续性叙事，源限 L04 `SelfModelView` + `derivation_receipt` 强制【B：L06_memory.md §2.1】；C2 期 deferred 未实现【B：c2_memory.profile.yaml:16】。跨重启"我还是我"的连续性主张由 ContinuitySignature/claim_continuity 机械判定：延续主张要求 episodic 在案 ∧ 谱系完整 ∧ gap=0；伪造锚（期望链之外）→ 两型主张皆拒【A：items.py:75-120；测试 test_m2_continuity_contrast.py:22-47】。

**摘要由谁做**：Galatea **无 LLM**（P4 前语言面仅模板语库；ADR-G-0020 明确记忆通路无模型），摘要/泛化 = 确定性算法（Laplace 计数合并 + 谱系指向）。**Kaleido 有云端 LLM（BYOK）**——这是我们对 Galatea 的结构性优势：摘要、事实抽取、重要度打分都可以在云端做，设备端只做"写入、检索、衰减、晋升落盘"（与 21 号 §4.1 写入路径设计一致）。Galatea 的可借鉴核心不是它的算法，而是**纪律**：① 晋升必须带源引用（core 条目可回溯到情节 seq）；② 晋升不改源条目（append-only，语义结论与源情景并存）；③ 晋升有门槛/可回滚（promotion/rollback 二阶纠偏思想）。

---

## §6 Q5 重放（replay）：合同冻结 vs 实现

**24 号报告结论确认成立**：离线环（做梦巩固）仅合同冻结（Q-L06-09，2026-10-17），subject 运行时无实现【B：24 号 §2/§4.1；A：subject/memory/ 与 subject/learning/ 无 dream 代码】。合同五要点【B：open_questions.md:221】：

1. **触发** = 静息期（低唤起 ∧ 无 pending——慢环触发超集）+ 实验控制显式 dream 命令；
2. **重放** = 情景库/流 buffer 确定性采样 + RSSM imagine 生成混合（生成比例冻结 profile）；
3. **产物** = 模型候选，走 Q-L11-09 学习门原样——**做梦不直写任何面**（重开条件明言"巩固需记忆直写 → 经新 Q（禁——L06 巩固永不直写既有面优先）"）；
4. **判据** = 睡眠增益（两相任务对照：仅新任务微调的旧任务遗忘 > 重放保护微调的遗忘，差 ≥ 冻结值）；
5. **审计** = dream 会话录制（Q-L13-20 同式）。

实现现状的两层：① **已实现**：`replay_select`（subject/learning/replay.py）——注意它**不是回忆接口**，是 L11 训练数据选择器：等权 `affective_tag + novelty + error_magnitude + time_diversity` 1:1:1:1、budget 每 T6 8 条、纯排序零采样（tie 按 id 升序）、只读 L06 事件引用、PROTECTED 不可降级、情感标注只入优先级不入目标函数【A：replay.py:18-60；B：c5 profile:25-31】。② **探针级**：`dream_samples()`（evidence/control/runs_p4gamma.py:263-282）——RSSM 自 imagine 生成混合样本，仅入缩规模冒烟（run_smoke，"dream/gate 结构面"），非主体运行时子系统【A】。

**对 Kaleido 的合同设计要点（C）**：我们 W4 的"深夜/充电反思"应照抄其**形状**而非其模型——触发=静息判据（充电 ∧ 无对话 ∧ 时刻窗）；产物=**候选**（core 增补提案、情节摘要）走"云端 LLM 产出 + 设备端写库 + 用户可查可删"的门，绝不静默改写；判据可用"次日召回命中率/对话连续性主观评分"替代"睡眠增益"；全部反思产物留痕（来源标记 `reflection`）。

---

## §7 Q6 记忆与情感/动机的接口

### 7.1 novelty / belief_uncertainty 的供给方——**不是记忆**

AffectPipeline 两维来源【A：affect/pipeline.py:142-160】：

```python
novelty      ← lowpass(sensory_residual_mean)      # 感知预测残差（感知/世界模型）
uncertainty  ← lowpass(belief_uncertainty_mean)    # 信念不确定度（L04 信念态）
# 输入缺源 → 保持旧值 + Unknown(MISSING_SIGNAL)，禁编造
```

即 Galatea 里这两个供给是**感知/世界模型**的产物；记忆不直接喂 novelty。这与 22 号方案（记忆供给 novelty/belief_uncertainty）不同——Kaleido 无设备端世界模型，用记忆（"这话/这事以前见过吗"）替代预测误差作 novelty 供给是**合理的降级替代**（C），但要注意 Galatea 的纪律启示：**供给缺失时保持旧值 + 显式 Unknown，禁编造**——km 无命中时应输出"无相关记忆"信号而非静默。

### 7.2 记忆参与动机的三条边（Galatea 实际接线）

1. **情感→记忆（写侧）**：L05 评价产生 `AffectiveValueTag(memory_ref, valence, arousal, persistence)` → L06 **唯一写者**落盘：绑定到目标条目 + 伴生 AFFECTIVE_VALUE 条目（strength=persistence、lineage 指向目标、禁凭空标注）【A：affective.py:20-50】。情感事件因此**检索更靠前（δ 分量）且更持久**。
2. **记忆→工作空间→竞争（读侧）**：T3 拍记忆向 L07 提供"当前相关项 + 显著性分量"候选，消费广播后写 working；候选显著性五分量 `goal_relevance/affect/novelty/uncertainty/info_value` 各 [0,1]【B：L06_memory.md §5 T3 行；A：workspace/propose.py:15-29】。记忆本身不带写权限（提案无写路径字段）。
3. **情感→记忆编码调制（未启用）**：MEMORY_ENCODER 在调制目标六类枚举中【A：modulation.py:44】，但 C2 发放门 enabled 缺省 = {gain, believe_threshold, salience}，编码调制留 C5 promotion 面【A：modulation.py:181；B：c2_memory.profile.yaml:40】。调制合同本身值得抄：因子界 [0.5,2.0]、TTL 4 拍、无回执=未发生【A：modulation.py:50-54】。

**memory_hook（点火因子）对照**：24 号设计的"慢环草稿种子=记忆 Top-3"在 Galatea 源码**无对应物**（slow_clock.py/draft.py 零记忆引用【A：grep 证】）——那是 22 号方案的 Kaleido 原生设计。Galatea 最接近的机制是 T3 候选供给了。因此 W3 的 memory_hook 因子不必对标 Galatea，其自然定义是：**km_query（以当前语境为 cue）top1 分数 > 阈 → 因子升高**（"有值得主动提起的记忆"）——语义与 Galatea T3"当前相关项"一致（C）。

---

## §8 Kaleido W4 落地建议

### 8.1 km 组件增补 API 草案（最小实现）

```c
/* ── API-1 访问增强（对应 Galatea forget.py:66-83）──
 * 检索命中即调：strength = min(1.0, strength + 0.125)，刷新 last_accessed。
 * km 的 JSONL 不可原地改 → 两选一：
 *  a) 内存 strength 覆盖表（{seq→strength,last} 启动重建，≥N 条刷 NVS）；推荐
 *  b) 检索后整行重写（写放大，不推荐）
 * recency 基准同步从 t 改为 last_accessed（kaleido_memory.c:218 相应改）。 */
int  km_reinforce(uint64_t seq);

/* ── API-2 惰性衰减（对应 forget.py:41-63，MCU 化改写）──
 * 不做周期全库扫描；检索/注入时现算有效强度：
 *   strength_eff = strength0 * powf(DECAY_PER_DAY, days_since(last_accessed))
 *   DECAY_PER_DAY 建议 0.99（≈半衰 69 天；Galatea episodic τ=256@T6 折算同量级）
 * 写落盘仅在 reinforce/晋升/压缩时发生 → 零写放大。PROTECTED 免衰减。 */
float km_effective_strength(const struct km_episode *ep, time_t now);

/* ── API-3 晋升 core（对应 semantic.py + promotion 纪律）──
 * 云端 LLM 从情节抽事实句（带源 seq 引用）→ 设备端核验追加：
 *   ① 源 seq 必须在库（禁无源晋升，LineageBreak 精神）
 *   ② core 条目 append-only，源情节不删不改
 *   ③ core 条目带 protected 标志：km_forget 拒绝（ProtectionViolation 精神）
 *   ④ core 上限 100 条，满则提示云端合并（LLM 重写 core，用户可见可删） */
int  km_core_promote(uint64_t src_seq, const char *fact_json);

/* ── API-4 显式遗忘（对应 forget.py:86-107）──
 * 不删行：state 字段置 "forgotten" + reason 留痕（lineage 尾加 "tombstone:<reason>"），
 * 检索默认排除；PROTECTED 拒绝。幂等。 */
int  km_forget(uint64_t seq, const char *reason);
```

**明确不移植清单及理由（MCU 预算语境）**：

| Galatea 机制 | 不移植理由 | 保留的内核 |
|---|---|---|
| 八子系统（working/semantic/procedural/autobiographical/metamemory 等） | working=RAM 会话态；semantic 由云端摘要+core 承担；procedural 无技能面；metamemory 是评价实验装置 | core/episodic/raw 三层（21 号既定） |
| metamemory 2×2 混淆协议 | E1 评价装置，无在线消费 | "回忆不得当事实"进 prompt 纪律（§8.3） |
| ContinuitySignature 四臂对照 | 为对抗"伪更新"的实验契约；我们以 seq 单调+备份 manifest 达成同等可审计性 | 跨重启 seq 续接（V-48e 已有） |
| 做梦重放（RSSM imagine） | 无设备端世界模型，判据（睡眠增益）不可测 | 深夜云端反思候选走门（§6 要点） |
| LineageBreak DAG/parent_digests 全谱系 | 复杂度高、单一设备语境收益低 | 晋升带 src_seq 单引用即可 |
| 保护套件 112 场景 | 研究级防灾设施 | protected 标志 + 拒删两条规则 |

### 8.2 每轮对话→情节的字段映射表

| km_episode 字段 | 来源（W 系列已有材料） | 规则/建议 |
|---|---|---|
| `seq` | `km_seq_current()+1` | 启动扫描 JSONL 尾行重建水位（V-48e 跨重启连续） |
| `t` | SNTP 定稿时刻（W3 已接 SNTP） | 无网退化=上次已知墙钟+monotonic 差 |
| `summary` | **ASR 定稿文本**（裁剪 ≤192B，UTF-8 边界安全） | 原文优先——"用户说了什么"是对话记忆的主体 |
| `facts[0]` | **LLM 回复首句**（≤48B） | 设备端零成本摘要；云端批处理可回填更多 |
| `tags[]` | 语境：时段（morning/evening…）、事件（barge-in/ignition/round_fail） | 本地即时可定；话题标签留云端回填 |
| `importance` | 缺省 0.3；云端 LLM 打分 [0,1] **异步回填** | 回填载体建议：下轮会话建立时云端顺带返回 `{seq:score}` 覆盖表进 NVS（JSONL 不可原地改）；或随压缩期重写落盘 |
| `affect`（**新增**） | 九维快照当拍 valence/arousal（各 1B 有符号定点即可） | 对应 Galatea AffectiveValueTag 的 δ 分量：情感轮检索加权；`affect_epoch`（轮类型：playful/distress/quiet）可选 |
| `raw_ref` | raw 层日文件路径+行号 | 21 号层 3 设计，km 只存指针 |
| 轮 `rc`（写入门槛） | rc==0 正常轮才写；rc!=0 写 `tag=error, importance=0.1` 或不写 | 防失败轮污染检索集（Galatea"空命中禁冒充"的镜像纪律） |
| （新增建议）`origin` | user / self（主动点火所说）/ system | D-13 精神的最小防线：self/system 条目不得标为用户亲历 |
| 时长/轮数 | 会话元数据 | 显著度门参考：单轮 ≥N 秒 或 累计 ≥K 轮的会话才值得情节化（对齐 Galatea"常规按拍采样+显著事件即时"——我们把"轮"当拍、把"用户明确情绪表达/纪念日提及"当危害级事件即时编码） |

### 8.3 检索注入 prompt 的格式建议

W2 已动态组装系统提示词，W4 在其后追加（总预算 ≤400 中文字符）：

```
[你的长期记忆（回忆，未必精确）]
- 3天前：用户提到项目评审压力大，你安慰过TA（这段回忆让你印象深）
- 上周：用户喜欢在晚上聊天文……
[关于用户的既定事实]
- 用户在做Kaleido项目；用户的名字叫……
```

纪律（对应 Galatea 层级守卫 ports.py:76-78 + L06 §4.2"禁近邻冒充"）：

1. 情节段措辞用"你记得/回忆"语气，**与 core 事实段分开**——LLM 不得把回忆当当下事实陈述（RECALLED 禁提升的产品化表达）；
2. 每条一行的确定性排序：`(score desc, seq asc)`；条目 ≤5（top_k 对齐 Galatea 4~5）；**无命中时写"[你还没有和TA的共同回忆]"而非省略段落**——给 LLM 显式的空态，防其编造（Unknown 语义）；
3. 时间相对化（"3 天前"而非时间戳），减少 token 且拟人；
4. 高情感轮（|valence| 大）可加"（印象深）"后缀——δ 分量的可解释呈现。

### 8.4 960KB 预算下的容量纪律

- **行尺寸实测口径**：km_episode 最大 ~750B（summary 192 + facts 384 + tags 72 + 头部/JSON 开销），典型 ~400B。
- **条数上限**：建议 **1500 条**（典型 ~600KB，最坏 ~1.1MB 触发强制压缩）——量级对齐 Galatea C2 episodic 1024 的"千条"尺度；对应重度使用 ~3-6 个月轮级记忆（150 轮/月），配合压缩可无限延续。
- **淘汰/压缩（80% 触发，闲时执行）**：照抄 Galatea 溢出语义"**降级不删除**"——最旧且 `strength_eff < 0.2` 的非 protected 条目 → 重写为摘要行（保留 seq/t/summary 前 64B/tags，丢 facts，state=archived，原行由重写收割）；tombstone 行随压缩物理收割（Galatea 域内永久是因 per-trial 库天然有界，我们 960KB 必须收割——但保留"墓碑簿"NVS 位图 seq 列表维持可审计性，C 推断）。
- **写入磨损**：轮级 append 40KB/天 ≈ 15MB/年，压缩重写 ~600KB/次、每周 ≤2 次 ≈ 60MB/年——对 960KB 分区 littlefs 动态磨损均衡（NOR 10 万次 P/E）完全无压力；巡检只报"分区使用率 + 压缩次数"即可（V-48e 已有水位项）。
- **备份节奏**：备份**不占内部 storage 分区**（21 号方案 B：USB MSC/局域网导出）；`km_backup` 整拷贝时顺带产出 manifest（last_seq/条数/sha256）。设备内部仅保留 NVS 中 last_seq 水位 + 每日一次"备份陈旧天数"健康巡检字段。
- **两段演进**（对齐 M2→C2 先例）：W4 先实现"容量满即停写 + 告警"（Galatea M2 encode.py:26-29 的拒写语义，实现最简、fail-safe），W5/后续再升级"满则降级压缩"（C2 enforce_overflow 语义）。

### 8.5 W4 计划（§5R.5）具体修改建议

1. 增补项：`km_reinforce` + `km_effective_strength`（API-1/2，一次实现即补齐"重要性演化"短板）；`km_core_promote` 可降级为 W4.5（先手写 core.json 也能跑通注入链路，但字段结构按 §8.1 冻结）。
2. `km_append` 的 `importance` 字段注明"异步回填"，阻塞路径禁等云端。
3. prompt 注入按 §8.3 格式与空态纪律执行，V-48e 门禁增加"无记忆轮的空态日志"检查项。
4. 检索排序公式在 kaleido_memory.h:50 注释同步修订（recency 基准 last_accessed、命中权重上调、tie-break），并加 Unity 手算锚测试（对齐 test_c2_retrieval_weights.py 的"测试即规格"实践）。

---

## §9 来源清单

**Galatea 源码（A 级，均为 2026-10-28 通读）**：
- /home/emb/project/Galatea/galatea/subject/memory/：ports.py、store.py、items.py、encode.py、retrieve.py、forget.py、affective.py、semantic.py、prospective.py、metamemory.py、consolidate.py（全文）
- /home/emb/project/Galatea/galatea/subject/memory/tests/：test_c2_retrieval_weights.py、test_forget_tombstone.py、test_m2_continuity_contrast.py（其余 8 件抽样核对）
- /home/emb/project/Galatea/galatea/subject/affect/：pipeline.py（:142-160 供给）、modulation.py（调制合同）、appraisal.py、ports.py
- /home/emb/project/Galatea/galatea/subject/workspace/：propose.py（显著性五分量）、slow_clock.py、draft.py（零记忆引用 grep 证）
- /home/emb/project/Galatea/galatea/subject/learning/replay.py（重放选择器）
- /home/emb/project/Galatea/galatea/subject/c2profile.py（C2MemoryParams :158-176）
- /home/emb/project/Galatea/galatea/evidence/control/：runs.py（:780-835 SC6 编码路径）、runs_p4gamma.py（:263-307 dream 探针）

**Galatea 文档（B 级）**：
- /home/emb/project/Galatea/design/lld/L06_memory.md（v1.0.4）、L11_learning_consolidation.md（联动面）
- /home/emb/project/Galatea/config/profiles/：c2_memory.profile.yaml（v1.0）、m2_continuity.profile.yaml（v1.0）、c5_continued_learning.profile.yaml（v1.1）
- /home/emb/project/Galatea/design/open_questions.md：Q-L06-01/02/03/06（AI-v2 2026-10-06 批）、Q-L06-04/05/07/08（2026-10-08 批）、Q-L06-09（2026-10-17 批，:221）

**Kaleido 项目内**：docs/research/21-长期记忆系统与SD卡调研.md、docs/research/24-Galatea慢环与动机仲裁提取.md（§2/§4.1/§5）、kaleido-fw/docs/plan-2026-10-26-M2阶段开发计划.md（§5R.5 :241-250）、kaleido-fw/components/kaleido_memory/include/kaleido_memory.h、kaleido-fw/components/kaleido_memory/src/kaleido_memory.c（:180-241）。
