# V-34×V-35 动画与 XIP 交叉压测（M1.4 前置门）

| 版本 | 日期 | 作者 | 状态 |
|---|---|---|---|
| v1.0 | 2026-10-25 | ZCode（主线直测；委派 3 次因平台并发限制中断后改为直接执行） | 完成——判定回填方案 v1.3 §5 |

**目的**：给灵魂架构方案 §5（预算表 v2 / 动画分级 T0/T1/T2 / XIP 取舍 / 双轨条件④）提供真机数据。
**方法**：独立 headless 基准工程 `kaleido-fw/tools/hil/soul-bench/`（esp32s31 preview、320MHz、PSRAM OCT@200MHz、flash 80MHz DIO、WIFI/BT 关闭、freertos 1kHz、ALWAYSINTERNAL=0+RESERVE 32K 对齐 edge_agent 实测配置、任务 WDT 关）。同一固件两个变体：`SPIRAM_XIP_FROM_PSRAM=y/n`，仅刷 app 至 ota_0@0x20000（bootloader/分区表/NVS 未动）。基准线程为 pthread（64KB 内部栈， unpinned；见 §2 排障记录）。ThorVG 资产 = 组件官方示例 emoji-animation.json（231KB，op=48 帧，1 层）。原始日志：`docs/gates/logs/v34_xip_{on,off}_v6c_serial.log`（各含完整一次运行，`SB,ALL,DONE` 达成）。

---

## §1 判定回填（先读这节）

| # | 方案 §5 判定项 | 实测结论 |
|---|---|---|
| 1 | **XIP off 全负载代价是否可接受（阈值 <15%）** | **可接受（贴线）**：ThorVG 渲染 +4.6%（460²）~+14.5%（230²）；memcpy/FIR/SHA256 **零退化**（±0.1%）；cJSON 解析反而 **-33%**（202→135ms/百次，flash cache 命中优于 PSRAM 取指）。换 +0.87MB（本应用）/ +4.08MB（edge_agent 级固件，V-32）PSRAM 池，**净收益为正** → 维持方案主路径"关 XIP"。⚠ 正式固件代码足迹大 5 倍，flash cache 抖动可能高于本口径——M1.5 集成后在正式固件上复核一次 |
| 2 | **T1 态（对话动画）单核 <85%？** | **通过**：半屏 Lottie 325²@24fps = **56.8%（on）/62.1%（off）** 单核；1/4 屏 230²@24fps = 36.7/42.1%。叠加 LVGL flush（15-30%）后仍在 85% 门内。**T1=骨架+EAF 主、325² Lottie 可用** |
| 3 | **T2（高光全屏）** | **限时可行**：460²@20fps = 79.6/83.2% 单核（贴满，须降级并发）；**@24fps = 95.5/99.9% 超红线；@30fps = 119/125% 判死**（与 20/23 号预测完全一致） |
| 4 | **32KB 栈假设（红线④）** | **不成立（好消息）**：pthread C-API 同步渲染实测栈峰值仅 **~4.4KB**（64KB 栈 HWM 61,184B）。红线④放宽为"内部 16KB 栈即可"，无需 PSRAM 栈 |
| 5 | **预算表 v2 #3 行（ThorVG 引擎 0.1-1MB）** | **吻合**：engine+animation 持久占用 286KB(230²)/505KB(325²)/931KB(460²)（随画布复杂度增长）；画布 212/423/846KB 精确如预期。另测得**每帧 malloc/free churn 431KB(230²)~1.14MB(460²)**（瞬时反复，非净增长）——**必须 ALWAYSINTERNAL=0 路径**，否则内部堆瞬间打穿（实测曾至 12B） |
| 6 | **双轨条件④** | **未触发**：T1 达标、XIP off 代价可接受、无新增 S31 阻断项。S31 维持主线 |
| 7 | **C 组（JPEG 帧序列参考）** | esp_jpeg **软解** 460×460 RGB565 = **131.7-132.4 ms/帧**（两变体一致）→ **全屏 JPEG 帧序列 20fps 不可行（软解）**；S31 有 JPEG 硬解（SOC_JPEG_DECODE_SUPPORTED=1，soc_caps.h:625，A 级）→ **V-36 验证项：esp_driver_jpeg 硬解吞吐**；EAF RLE 路径未测（esp_lv_eaf_player 依赖 LVGL，见 §5） |

## §2 排障记录（三轮根因，均已闭环，登记为工程事实）

| 轮次 | 现象 | 根因 | 处置 |
|---|---|---|---|
| v2 | CPU1 task_wdt 恐慌重启 ×4-5 | ① cJSON 小节点内部分配打穿内部堆至 12B；② WDT 5s 恐慌放大 | ALWAYSINTERNAL=0 + RESERVE 32K；WDT 关（bench 语境） |
| v4/v5 | `assert failed: pthread_self pthread.c:583` ×5-7 | **espressif/thorvg 组件（meson 强制 pthread 依赖）在原生 FreeRTOS 任务中调用 pthread API 会 assert**——与 `esp_pthread_set_cfg`（v4 已删）及 thorvg 内部 mutex 均相关；**thorvg 必须在 pthread 上下文调用** | 基准线程改 `pthread_create`（attr 栈 64KB）。**工程事实 W-6**：esp_lvgl_port 的 LVGL 任务同为原生 FreeRTOS 任务——**lv_lottie 在 LVGL 任务内跑 thorvg 是否同样受限，M1.6 必须首先验证**；若受限，方案=渲染独立 pthread + LVGL 合成（或 thorvg 调用收敛到单一渲染 pthread） |
| v6 | 官方 lottie 资产 `load_data rc=1`（手写资产则 Load access fault） | **flash 映射的 EMBED 数据无 NUL 终止且只读**，lottie 解析器按需读越界/校验失败 | 拷入 NUL 结尾可写缓冲 + `copy=true`（已固化在 bench_b.c） |

## §3 A 组数据（XIP on → off，均值；全部单线程、bench pthread 内）

| 负载 | XIP on | XIP off | off 代价 |
|---|---|---|---|
| PSRAM memcpy 4MB（30 次） | 76.576 ms（52.2 MB/s） | 76.671 ms | +0.1% |
| cJSON 解析 100KB（1118 记录，100 次） | 202.082 ms | 135.158 ms | **-33.1%（更快）** |
| 浮点 FIR 1M MAC（10 次） | 47.797 ms | 47.958 ms | +0.3% |
| SHA256 4MB（mbedTLS SW，10 次） | 930.0 ms（4.3 MB/s） | 931.3 ms | +0.1% |

池口径复核（boot 空闲）：XIP on PSRAM free **15,901,916B** vs off **16,774,436B** → 本应用 XIP 足迹 **872,520B**（与固件 728KB 量级一致；edge_agent 4.07MB 固件对应 4,077KB，V-32 口径互证）。内部 RAM：on 366.7KB / off 379.3KB 空闲，渲染期间最低 366-379KB 稳定（渲染分配全走 PSRAM）。

## §4 B 组数据（ThorVG/Lottie 档位矩阵，单线程同步渲染）

| 画布 | 变体 | avg ms/帧 | p50 | min/max | @20fps | @24fps | @30fps | fps 上限 |
|---|---|---|---|---|---|---|---|---|
| 230×230（1/4 屏） | on | 15.302 | 14.241 | 11.3/18.7 | 30.6% | **36.7%** | 45.9% | 65.4 |
| | off | 17.522 | 16.843 | 12.7/21.4 | 35.0% | **42.1%** | 52.6% | 57.1 |
| 325×325（约半屏） | on | 23.673 | 21.252 | 17.1/29.0 | 47.3% | **56.8%** | 71.0% | 42.2 |
| | off | 25.877 | 23.757 | 18.6/31.8 | 51.8% | **62.1%** | 77.6% | 38.6 |
| 460×460（全屏） | on | 39.804 | 34.688 | 28.1/48.7 | **79.6%** | 95.5% | 119.4% | 25.1 |
| | off | 41.618 | 36.837 | 29.6/51.1 | **83.2%** | 99.9% | 124.9% | 24.0 |

- **mt1（+1 worker 线程）无收益**：230²=15.7/17.7ms、460²=40.0/41.8ms，与单线程持平（该资产单层场景无可并行性；engine_plus_anim 反而 +21~25 万 B——**双核 Lottie 路线在本资产上不成立**，后续用多层真实角色资产复测一次再定论）。
- **heap（HEAPB 行，on/off 几乎相同）**：canvas 212/423/846KB；engine+anim 持久 286/505/931KB；每帧 churn 431/681/1,144KB。
- **渲染正确性**：三档 cksum 一致（on/off 同值）→ XIP off 不改变渲染结果。

## §5 C 组与未取得项

- C 组（esp_jpeg 软解）：460×460 RGB565 131.7（on）/132.4（off）ms/帧——见 §1.7 判定。
- **未取得**：① EAF（esp_lv_eaf_player）吞吐——组件依赖 LVGL widget/显示面板，headless 不便；降级路径 RLE 解码理论更轻，**转 M1.6 首个任务在真屏上测**；② JPEG **硬解**吞吐（V-36）；③ 真实多层角色 Lottie 资产的 460² 复测（官方示例仅 1 层，复杂度可能低估——美术打样后执行）。

## §6 对方案/双轨的最终建议（已同步方案 v1.3 §5）

1. **XIP 取舍：关（维持方案主路径）**——通用负载零退化、渲染 +5-15%、cJSON 显著更快、换 0.9-4MB 池；M1.5 在正式固件复核一次（代码足迹更大）。
2. **动画分级维持 T0/T1/T2**：T1 以 EAF/骨架为主、325² Lottie 24fps 可用（57-62% 单核）；T2 全屏 Lottie 仅 20fps 限时；30fps 全屏永久判死。**mt1 双核渲染暂不纳入设计**。
3. **红线④修订**：ThorVG C-API 同步渲染栈峰值 ~4.4KB——"32KB+ 大栈"警报解除；但 **W-6（thorvg 必须跑在 pthread 上下文）升级为 M1.6 首验项**，影响 lv_lottie 集成形态。
4. **ALWAYSINTERNAL=0 是 thorvg 的硬前提**（每帧 churn MB 级必须走 PSRAM）；此配置与 edge_agent/V-32 现状一致，无需改动。
5. 双轨条件④：**未触发**；下一次数据门 = M1.6 真屏 EAF/动画焦点联测。

## 附：复跑

见 `kaleido-fw/tools/hil/soul-bench/README.md`（两个 sdkconfig 变体、仅刷 app@0x20000、串口 115200 采集 `SB,` 前缀行）。
