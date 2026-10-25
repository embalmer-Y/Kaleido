# V-32 esp-claw 真机内存实测

| 日期 | 板卡 | 固件 | 环境 |
|---|---|---|---|
| 2026-10-25 | ESP32-S31-Korvo-1 V1.1 @ /dev/ttyUSB0 115200 | edge_agent（ESP-Claw 0.1.0, git 3177144 = 9b23c15+PR#157+TLS1.3 修复钉版），4,272,592 B | ESP-IDF v6.1-1-gc55d13c6（W-2 backport），IP 192.168.2.239，LLM=bigmodel/glm-5.3-flash via router（key 已配置，本文档全程脱敏） |

## 1 目的与方法

回填 `docs/research/23-S31资源预算与感官管线调研.md` §2.1 预算行 **#5（esp-claw agent 基座，原估乐观 1,500 / 保守 3,000 KB，标注"第2轮必须实测"）**，并顺带记录 LLM 对话端到端延迟。

测量通道（全部真实串口/HTTP 数据，无估算混入）：
1. **串口全量 tee**（`logs/v32-serial-full.log`，含 wall-clock 时间戳，`<<HH:MM:SS.mmm>>` 为采集机时间，其余 `I (ms)` 为设备开机毫秒）；
2. **设备内 1 Hz Lua 采样器**（`logs/v32_memmon.lua`，`lua --run-async` 后台运行，`system.heap.get_info(1024/2048)` 打印 `V32MEM` 行，含 `minimum_free_size` 高水位；共 2×300 样本；注：IDF v6.1 `MALLOC_CAP_SPIRAM=1<<10=1024`、`MALLOC_CAP_INTERNAL=1<<11=2048`）；
3. **控制台能力调用** `cap call get_system_info {"sections":["memory"]}`（cap_system JSON，含 internal/psram 的 free/min/largest）。

测得状态：A 空闲联网稳态 → B 后台 Lua 常驻 → C/D/E 三轮 LLM 对话（单轮 / session 多轮 / session 历史累积）→ F 对话后稳态。

## 2 环境（固件/分区/测量工具）

- **烧录恢复**：板子此前为 V-04 kd_wakenet（factory@0x10000，表@0x8000）。本次刷回 edge_agent 布局：bootloader@0x2000 + 分区表@0x9000 + otadata@0x10000（写全 FF `ota_data_initial.bin` → 引导回退至 ota_0）+ **ota_0@0x20000**（4.07MB，--no-stub 431s，`Hash of data verified`，`logs/v32-flash-esptool.log`）。**NVS@0xA000-0x10000 全程未触碰**（烧前读回快照 `logs/v32-flash-partition-before.bin` 确认页头 ACTIVE），WiFi 凭据与 LLM key 存活，上电自动联网（sta ip 192.168.2.239，SNTP 同步正常）。启动日志确认从 ota_0 引导：`3 ota_0 OTA app 00020000 00500000`。
- **PSRAM 池**（boot 证据，serial log L50-74）：16MB Octal@200MHz（vendor 0x1a）；`.rodata xip on psram` + `.text xip on psram`；**进入 heap 分配器的池 = 12,248K+9K+48K = 12,306.6 KB（12,602,120 B）**，其余 4,077.4 KB 被固件镜像 XIP 占用（≈固件体积，见 §4 新发现）。
- **内部 RAM 池**：327K+16K+RTCRAM 31K，`internal_total`=417,283 B（407.5 KB），另预留 32K DMA 保护区。

## 3 数据（三态表 + 日志摘录）

### 3.1 三态总表（B 为字节；KB=B/1024）

| 状态 | PSRAM free (B) | PSRAM free (KB) | 内部 free (B) | PSRAM 占用 (KB)† | 最大连续块 (B) | 证据 |
|---|---|---|---|---|---|---|
| A 空闲联网稳态（boot+2.5min，WiFi+SNTP 完成，无 Lua） | 10,320,888 | 10,078.9 | 134,611 | 2,227.7 | 10,223,616 | log@12:29:50 JSON |
| B 空闲+1 个后台 Lua job | 10,240,512 | 9,999.5 | 134,495 | 2,307.1 | 10,223,616 | V32MEM i=6 |
| C 对话 R1 峰值水位（单轮 single-turn） | **10,077,520** | 9,841.3 | 134,495（min 95,400 持平） | 2,465.3 | 9,961,472 | V32MEM ps_min |
| D 对话 R2 峰值水位（session 首轮，历史 92 B） | 10,028,164 | 9,793.1 | 134,103 | 2,513.5 | 9,961,472 | V32MEM ps_min |
| E 对话 R3 峰值水位（session 第 2 轮，历史 2,206 B，**全日最深**） | **10,007,196** | 9,772.7 | 134,007 | 2,534.0 | 9,961,472 | V32MEM ps_min |
| F 对话后稳态（三轮对话完、Lua job 释放，+2.5min） | 10,288,912 | 10,047.8 | 134,123 | 2,258.8 | 9,961,472 | log@12:43:05 JSON |

† PSRAM 占用 = 12,306.6 KB − free。对话"峰值水位"取 `minimum_free_size` 单调高水位（即本轮对话期间瞬时最低 free）。

- **内部 RAM 高水位**：`internal_min_free`=95,400 B（93.2 KB）为 **boot 阶段**形成，三轮对话期间再未下探（ir_min 恒 95,400）——**对话管线内存全部落在 PSRAM**（`wifi_init: WiFi/LWIP prefer SPIRAM`）。最大内部连续块恒 62,464 B（61.0 KB）。
- **交叉验证**：F 态 JSON `heap_min_free − internal_min_free = 10,102,596 − 95,400 = 10,007,196`，与 Lua 采样器 R3 的 `ps_min` **逐字节一致** ✓。
- **对话瞬时增量（dip，临测基线→水位）**：R1 10,218,360→10,077,520 = **137.5 KB**；R2 10,217,828→10,028,164 = **185.2 KB**；R3 10,204,028→10,007,196 = **192.2 KB**。含 TLS 会话×2-4（见 §6）+ HTTP 客户端缓冲 + 上下文/工具 JSON 组装（tools 13,259 B + skills 3,318 B + 记忆 2,628 B）+ 回复文本。
- **对话持久增量**：F 态相对 A 态 −32 KB（session 历史落盘缓存 + Lua 模块暖态）；**最大连续块永久分裂 10,223,616→9,961,472（−262 KB）**——碎片化对"最大单块申请"（预算行 #3 的 846KB 画布）仍无压力（余 9.7MB 连续）。
- **后台 Lua job 增量**：A→B ≈ **80 KB**/个（async job + 脚本环境；采样器自身 GC 波动 ±30 KB，见 V32MEM 曲线锯齿，不影响水位读数）。

### 3.2 原始日志摘录

```
# 空闲态 JSON（12:29:50，A 态）
{"memory":{"internal_free":134611,"internal_total":417283,"internal_min_free":95400,
"internal_largest_free_block":62464,"heap_free":10455687,"heap_min_free":10415252,
"psram_available":true,"psram_free":10320888,"psram_total":12602120,
"psram_largest_free_block":10223616}}

# 对话 R1（12:32:41 发起）
I (374062..374114) claw_core: context_loaded request=1 (Profile 1269B + Long-term 1359B + Skills 3318B + tools 13259B)
I (374224→374480)  mbedtls: => handshake … <= handshake      # TLS ① 256ms
I (374487→375786)  mbedtls: => handshake … <= handshake      # TLS ② 主握手 1299ms
I (376437/376610)  mbedtls: handshake 60ms/57ms              # TLS ③④（重连/多址尝试）
I (383764) claw_core: completion request=1 status=done raw=Hi, I'm ESP-Claw — …
# 对话期间水位（V32MEM ps_min 字段）
<<12:33:08 前后>> ps_min=10077520（基线 10218360，dip 137.5KB）

# 对话 R3（session 历史累积）
I (689682) claw_core: context_cached request=3 provider=Session History context_len=2206
I (701105) claw_core: completion request=3 status=done raw=Yes—within this session I can see our last exchange…
<<ps_min=10007196>>（dip 192.2KB，全日最深）

# 对话后稳态（12:43:05，F 态）
{"memory":{…"heap_min_free":10102596…,"psram_free":10288912,"psram_total":12602120,
"psram_largest_free_block":9961472}}
```

## 4 换算与预算回填

**换算**（16,384 KB 总 PSRAM 的分解）：

| 项 | KB | 说明 |
|---|---|---|
| 固件 XIP 镜像（.text+.rodata 映射 PSRAM） | **4,077.4** | `CONFIG_SPIRAM_XIP_FROM_PSRAM=1`（esp-claw 默认开启）；23 号表**未建模** |
| 可分配池 | 12,306.6 | `psram_total` 实测 |
| esp-claw 基座动态占用（空闲联网态，含 WiFi/lwIP 缓冲+mDNS+codec/I2S 初始化+全 16 能力组） | **2,227.7** | 12,306.6 − 10,078.9 |
| 对话峰值再叠加（TLS×N+HTTP+上下文+回复） | +137.5 ~ +192.2 | 三轮 dip |
| 对话后持久 | +32 | session 历史+暖态 |
| 空闲剩余（free） | 10,078.9 | — |

**23 号 §2.1 #5 行修正为：乐观 1,600 KB / 保守 2,200 KB（纯 agent 基座，空闲态；计入对话 TLS 峰值后 1,750 / 2,400 KB）**。
推导：实测"基座+WiFi/lwIP"合计 2,227.7 KB；预算表中 WiFi/lwIP 已单列为 #12 行（300-600 KB），扣除该归属后基座净值 1,628-1,928 KB，取整即上行修正值。原估 1,500/3,000 中**保守列偏高约 0.8-1.4 MB**；该行不确定度从 ±1.5 MB 收敛到约 ±0.3 MB（残留源：①WiFi/lwIP 与基座的归属拆分未实测隔离；②TG/feishu/wechat 等外部 IM 长连接未激活；③更长 session 历史/多技能堆叠未压测）。原 23 号 §6 风险 1 **关闭**。

**比 #5 更大的预算新发现——XIP 行缺失**：23 号表按 16,384 KB 池排布（保守总计 13,864 KB），但 esp-claw 默认 XIP 使实际池只有 **12,306.6 KB** → 保守总计直接**穿仓 1.56 MB**，红线（#13 行 2,400 KB 余量）失守。对策二选一（建议写入 G2 决策）：
1. **关闭 XIP**（`CONFIG_SPIRAM_XIP_FROM_PSRAM/FETCH_INSTRUCTIONS/RODATA=n`）：代码回 flash 执行（16MB flash、ota 槽 5MB 容纳 4-4.5MB 固件无压力），池回到 ~16 MB 级，23 号表维持原排布即可；需实测关闭后音频/动画 flash 带宽竞争（XIP 的动机通常是把取指搬离 flash 总线）。
2. 保留 XIP：23 号表新增"固件 XIP 镜像 ~4.1 MB"行，并等量压缩其余行（如 ASR 预滚窗、双 Lottie 层等触发器全部禁用仍不够，需再砍 ~1.5 MB）——**不可行性偏高**。

其他行附带校准：#8 行 Lua VM——实测每 async job 常驻 ~80 KB > 设计配额 64 KB（×1.25 修正）；#12 行 WiFi/lwIP——归入实测合计无法单独拆分，维持原区间。

## 5 结论与偏差说明

1. **esp-claw 基座在 S31 真机实测空闲联网占用 2,227.7 KB（含 WiFi/lwIP），对话峰值 2,534.0 KB**——落在 23 号原估区间中部，偏乐观侧；#5 行修正为 1,600/2,200 KB。
2. 内部 RAM 充裕且对话不消耗（min 93.2 KB 为 boot 水位，free 稳 131 KB，最大块 61 KB）——基座内存策略整体走 PSRAM，符合预期。
3. 碎片化轻微：全日压测后最大连续块 9.73 MB，846 KB 级单块申请无压力。
4. **未取得项**（如实记录）：①WiFi 连接前后 heap 差——控制台 5.2 s 才就绪而 WiFi 4.1 s 已连上，窗口内无法采样（联网总态 A 已测，不影响预算）；②TLS 会话单独隔离值——1 Hz 采样无法拆分亚秒级握手，已并入对话 dip（下界 137.5 KB 含 ≥2 个 TLS 会话）；③WebIM/WS 通道增量——本次经控制台 `ask` 驱动（同一 claw_core 管线与 TLS 出站），WS 会话缓冲未计入（量级估 <10 KB）；④claw_memory 异步抽取的内存峰值——本轮三段短对话未触发抽取任务（M1 mock 场景未复现）；⑤首 token 流式延迟——见 §6。
5. 安全纪律执行：未 erase_flash、未动 NVS、未改 vendor 源码（本轮无需重打 W-1，构建零改动直接复用 10-24 产物）；API key 未写入任何文档。

## 6 附：LLM 端到端延迟记录（供语音链路预算参考）

| 轮次 | 上下文 | 端到端（submit→completion done） | 其中 TLS 主握手 | LLM 生成（服务端） |
|---|---|---|---|---|
| R1 单轮 | 系统提示+tools 13.3KB | **9.7 s**（374.1→383.8s） | 1.30 s | 8.0 s |
| R2 session 首轮 | +历史 92 B | **18.0 s**（641.9→659.9s） | ≈1 s | ≈17 s（模型侧波动，回复较长） |
| R3 session 第 2 轮 | +历史 2,206 B | **11.4 s**（689.7→701.1s） | ≈1 s | ≈10 s |

- 上下文加载本身仅 ~52 ms；单次请求观测到 2-4 次 TLS 握手（含重连/多址尝试，256/1299/60/57 ms）。
- **claw LLM 传输层为非流式**（`claw_llm_http_transport` 无 stream 分支，`claw_core` 一次性收全量 completion）——**"首 token 延迟"在当前固件不可测且概念上不存在**，表中为整句完成延迟。语音对话链路若需秒级首响应，G2 需实现流式 SSE 解析（上游候选第 5 份 issue），或按整句 8-18 s 规划 TTS 入队策略。

## 附：原始证据文件（docs/gates/logs/）

- `v32-serial-full.log`（264.8 KB，串口全量，600 个 V32MEM 样本 + 3 轮对话全程含 mbedtls 握手明细）
- `v32-flash-esptool.log`（烧录与校验）；`v32-flash-partition-before.bin`（烧前 flash 布局快照，证明 NVS 未动）
- `v32_memmon.lua` / `v32_probe.lua`（测量脚本，可复跑：`lua --run-async --path /fatfs/temp/v32_memmon.lua`）

测量完成后板子保持 edge_agent 固件运行（ota_0，联网态）。
