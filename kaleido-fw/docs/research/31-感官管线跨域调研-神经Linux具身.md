# 31 — 感官管线跨域调研：神经分层 × Linux 收包路径 × 具身智能（驱动 W6b v2.2）

> 负责人指示（2026-10-29）：感官层参考大脑结构（大脑/小脑/脑干/脊椎）
> 与 Linux 内核驱动机制（NAPI/LRO/GRO）及最新具身智能技术，重点解决
> **数据发送频率**与**数据压缩**。本文三域调研 → 机制映射 → W6b v2.2
> 设计修订（plan §5R.7 同步）。

## 1. 三域关键结论

### 1.1 神经分层（脊椎/脑干/小脑/大脑）

| 中枢 | 职能（文献口径） | 对我们的启示 |
|---|---|---|
| 脊椎 | 反射弧：ms 级短环本地反应，不经皮层 | 本地反射旁路：触摸→即时反馈不进 1Hz 灵魂环 |
| 脑干 | 感觉网关 + 网状激活系统调警觉/增益（sensory gating） | 上行守门层：什么值得上报 + 全局唤醒度调制上行增益 |
| 小脑 | 前向模型预测与时序，预测误差驱动学习 | per-entity 轻量预测器：只让**预测残差**上行 |
| 大脑皮层 | 分层预测编码：高层发预测下行，**只有残差上行**（surprise 最小化） | soul 1Hz 收"剩余惊讶"；attention 加权注入 |

关键理论：预测编码（predictive coding）——各层只把**未被预测的部分**
（prediction error/surprise）向上传播，预测准确时上游近乎静默。这
正是感官数据压缩的生物学答案：**压缩比=可预测度**。皮层-皮层下
（脊髓/脑干/小脑）层级间同样遵循误差传播（Chen et al. 2023 疼痛
回路跨层预测编码）。精度加权（precision weighting）：残差按"注意
力/警觉度"加权——脑干唤醒度决定哪些残差被放大上行。

### 1.2 Linux 收包路径（NAPI / LRO / GRO）

| 机制 | 内核行为 | 对我们的映射 |
|---|---|---|
| NAPI | 低流量=中断驱动；**高流量=关中断切轮询**（软中断 budget 上限），避免中断风暴；队列排干再回中断。2025 优化：下一包不明朗时挂起轮询等中断 | 频率适配：事件率超阈→自动加宽合并窗+轮询批处理+每 tick 事件预算；低于阈→事件驱动 |
| LRO | 硬件收包合并（NIC 内） | 传感器**硬件侧批处理**（IMU FIFO/传感器自带聚合=esp 传感器组件硬件能力） |
| GRO | 软件包合并（NAPI poll 路径）：同流多包并成大段描述符再进协议栈 | 聚合器把 N 个样本合并为**段描述符**（min/max/mean/trend/n/last/anomaly）上行 |
| budget | 每 softirq 周期固定处理配额 | 每实体每 tick 事件数上限，超出折叠为 storm 摘要 |

### 1.3 具身智能（事件相机 / 快慢分层）

| 技术 | 要点 | 对我们的映射 |
|---|---|---|
| 事件相机/神经形态传感 | 只传"变化超过阈值"的事件（逐像素死区），异步微秒时间戳；静态场景零数据、零功耗 | **死区增量编码**：entity 精度字段=deadband（+滞回）；绝对值仅周期快照刷新；事件带硬件时间戳 |
| H-JEPA 分层世界模型 | 每层预测视界更长、运行时标更慢；低层高频反应式，高层低频慎思式 | 四档时标分工（反射 ms / 门控 10-100ms / 预测 100ms-1s / 灵魂 1Hz-分钟），层间只传表示不传原始流 |

## 2. 机制 → 设计映射总表（W6b v2.2 的骨架）

| 感官问题 | 采纳机制 | 来源域 | 落法 |
|---|---|---|---|
| 高频原始流压缩 | 预测残差上行（小脑预测器） | 神经 | per-entity EWMA/自适应阈值预测器，surprise=|x−pred|/precision，>τ 才上行 |
| 静态值重复上报 | 死区+滞回增量编码 | 具身 | entity.precision 即 deadband；滞回防边界抖动；快照仅低频心跳刷新 |
| 样本量合并 | GRO 段描述符 | Linux | 窗口聚合输出 {t_first,t_last,n,min,max,mean,last,trend,anomaly} |
| 传感器侧减载 | LRO 硬件批 | Linux | 驱动能力位 cap.hw_batch（FIFO 批读，如 IMU） |
| 事件风暴 | NAPI 频率适配 | Linux | 事件率超阈→加宽合并窗+轮询批+budget；storm 折叠单条摘要 |
| 什么值得打扰大脑 | 脑干门控+精度加权 | 神经 | 全局唤醒度（presence/arousal）调制上行增益：互动中阈值低、安静时高 |
| 即时反应 | 脊椎反射弧 | 神经 | 反射旁路（ms 级本地动作经 claw/表达，日志留痕不进 soul tick） |
| 注入预算 | attention top-K | 神经+LLM | 每轮只注入 top-K 惊讶度实体的段摘要（≤500tok 纪律不变） |

## 3. 时标分层（H-JEPA 口径 × 神经中枢对照）

```
L0 驱动/外设层   LRO 位：硬件 FIFO 批读（µs-ms，纯硬件）
L1 脊椎反射环    ms 级：本地动作（触摸→本地反馈），不经大脑
L2 脑干网关      10-100ms：死区/滞回编码+事件门控+唤醒度增益
L3 小脑预测器    100ms-1s：EWMA 残差+GRO 段合并→实体快照
L4 大脑皮层      soul 1Hz（novelty=惊讶积分）+ think 分钟级
```

层间契约：**只传表示（事件/残差/段摘要），永不传原始流**；每层有
频率预算，超预算向上折叠（storm 摘要）。

## 4. 对既有设计的修正与保留

- **保留**：entity 元模型（id/单位/隐私级/source）、≤500tok 注入、
  private 不进云、三消费口（ke/ki/注入器）、触发三段式——v2.2 是
  管线内核的分层与压缩机制升级，不动消费面契约。
- **修正**：聚合器从"固定 500Hz→≤1Hz"单级压扁，升级为五级流水
  （反射旁路/门控/预测/合并/注入各司其职）；"精度"字段语义升格为
  deadband+滞回参数。
- **新增字段草案**：entity{deadband(=precision), hysteresis, tier 位图
  （反射挂点）, cap.hw_batch}；段结构 ksense_seg{t_first,t_last,n,
  min,max,mean,last,trend,anomaly,surprise}。

## 5. 文献

- 预测编码：[Wikipedia: Predictive coding](https://en.wikipedia.org/wiki/Predictive_coding)；
  [Chen et al. 2023, 分层预测编码跨脊髓/脑干/小脑/皮层](https://pmc.ncbi.nlm.nih.gov/articles/PMC10020379)；
  [Khan et al., 非对称连接与反馈清理](https://journals.plos.org/ploscompbiol/article?id=10.1371/journal.pcbi.1014435)；
  [Mikulasch et al. 2023, 精度加权解剖连接](https://pure.mpg.de/rest/items/item_3490857_4/component/file_3492320/content)
- Linux：[内核文档 NAPI](https://docs.kernel.org/networking/napi.html)；
  [kernel-internals: NAPI 流程](https://kernel-internals.org/net/napi)；
  [LWN 2025, 更聪明的 IRQ 挂起](https://lwn.net/Articles/1008399)；
  [SoftIRQ/GRO 调优实践](https://medium.com/@majidbasharat21/softirq-tuning-for-high-throughput-linux-networking-napi-rps-xps-gro-gso-e6662e14dfe2)
- 具身：[Benosman(CMU) 事件感知教程 2022](https://www.edge-ai-vision.com/wp-content/uploads/2022/05/T1T02_Benosman_CMU_2022.pdf)；
  [Putra et al., 机器人神经形态 AI](http://icarcv-proceedings.s3.amazonaws.com/files/0232.pdf)；
  [H-JEPA 分层世界模型（LeCun 蓝图）](https://x.com/ylecun/status/1541492685760069633)；
  [AMI Labs H-JEPA 开源](https://aicoder.com/news/news-20261012-ami-labs-h-jepa-hierarchical-world-model-open-weights)
