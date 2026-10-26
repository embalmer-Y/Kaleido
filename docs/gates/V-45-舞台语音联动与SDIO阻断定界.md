# V-45 — 舞台×语音联动（M1.6 表达 L0）：工程落地 + 阻断定界（未完成）

日期：2026-10-26 深夜。结论先行：**工程主体已落地并实证到"全链同应用
完整启动"（显示+舞台+表达桥+语音菜单共存，舞台 30fps）；但首段重度
语音流量（TTS 喷发）触发 esp_hosted SDIO 写超时崩溃，经 11 轮对照实验
完成定界——阻断点在"显示组件参与链接的系统画像 × hosted SDIO 时序"，
非我方代码逻辑。V-45 门禁判据（四态联动 soak + 无新瑕疵）未达成，
登记为上游 issue 素材 + 恢复配方后挂起。**

## 已实证达成（build27 家族固件，2026-10-26）

| 项 | 证据 |
|---|---|
| 合并工程编译链接 | convo-stage（dsi-lvgl ∪ voice-loop-p4 全组件） |
| 完整启动 | `VL,MEM,boot int=288k` → 舞台 START → KEX,idle → wifi → VL,READY 全序列 |
| 舞台渲染预算 | RENDER_MS avg 25.9ms / renders 29-31/s / skip=0（V-39 水平） |
| 表达桥 | kaleido_express L0 组件 + stage_express 四态色环（idle 实证，listening/thinking/speaking 代码就绪待 soak） |
| 冷启动稳定 | 3/3（该配置下） |

## 阻断点：esp_hosted SDIO × 显示组件构建画像

**现象**：显示组件（lvgl/esp_lvgl_port/thorvg/jd9366）参与链接后，
首次重度语音流量（'W' 自检的 TTS 喊话 / soak 的 announce）必崩：
`sdio_write_task: Failed to send data: 2 0x102 66 66`（CMD53 写超时×2，
MAX_SDIO_WRITE_RETRY=2）→ `Unrecoverable host sdio state` → 重启。
无显示组件链接的同代码基线同板同刻**完全正常**（对照 soak 9/10 rc=0，
含一次真实声学唤醒）。

**实验矩阵（全部上板实证）**：

| # | 变量 | 结果 |
|---|---|---|
| 1 | 渲染线程冻结（'f'） | 仍崩 → 非 CPU |
| 2 | 面板 DPI 关闭（'X'，带宽归零） | 仍崩 → 非 DSI 带宽 |
| 3 | 显示栈完全不初始化（'D' 延迟） | 仍崩 → 非 LVGL 运行时 |
| 4 | ext_ram.bss 关闭（探针缓冲改运行期 PSRAM，boot 布局=基线） | 仍崩 → 非 PSRAM 静态布局 |
| 5 | 基线 sdkconfig 整体移植 | 仍崩 → 非构建配置 |
| 6 | hosted 组件 hash 比对 | 与基线逐位一致 → 非组件版本 |
| 7 | hosted 池：PSRAM / 内部 | PSRAM→写超时；内部→sdio_rx_get_buffer 对齐断言（sdio_drv.c:953）→ 两种池皆亡 |
| 8 | wakenet 前置到 boot | 崩点提前到 wifi 初始化 → 更糟 |
| 9 | 基线同板同刻复测 | 正常 → 非 C5/板卡状态 |
| 10 | thorvg 线程池关闭 / 舞台 24fps | 仍崩 → 非渲染负载 |

**结论**：显示组件**仅被链接**（静态 DRAM +85KB、二进制 +~1MB、IRAM/
cache 布局变化）即改变系统画像，使 hosted SDIO 在 TLS 突发下错过时序。
疑似上游缺陷（esp_hosted 2.12.9 SDIO 驱动对内存/cache 压力敏感），
最小复现配方：voice-loop-p4 + 链接 lvgl/esp_lvgl_port/thorvg（不调用）
+ 'W'。

## 内存工程结论（V-45 沉淀，均为实测）

1. 合并版静态 DRAM 预算：显示组件静态 +85KB；六块 32KB 探针音频缓冲
   占 192KB——改运行期 heap_caps_malloc(PSRAM) 用毕释放（兼得内部
   余量与 PSRAM 布局稳定，已写入合并版 main，见恢复配方⑥）。
2. 初始化次序铁律：aio（I2S DMA 连续块）→ 显示（LVGL 32K 栈需此刻
   余量）→ wifi 最后；wakenet 必须在 wifi 之后（前置会打断 hosted 初始化）。
3. LVGL 任务栈 32K 官方口径在合并版可承受；舞台渲染 pthread 16K
  （thorvg 峰值 4.4KB 实测）。
4. 表达环必须挂在舞台屏之后的活动屏（反序会挂在无人可见的默认屏）。
5. sdkconfig.defaults 不回填既有 sdkconfig（本夜踩 3 次）：改配置必须
   双写或在 sdkconfig 内就地改。

## 恢复配方（下一会话从此续）

1. `tools/hil/convo-stage`：CMakeLists/sdkconfig 已定格；main 文件集齐
   唯 convo_stage_main.c 在二分中被基线覆盖（流程教训：**破坏性二分前
   必须先提交工作态**）。
2. 重建 = 基线 main + 显示段合并：显示常量/句柄 + bsp_ldo/backlight +
   app_lcd_init/app_touch_init（触摸暂缓，I2C0 共享 aio 总线方案已备
   aio_i2c_bus()）+ app_lvgl_init（direct_mode+avoid_tearing，V-35R 口径）
   + app_main 注入（次序见内存结论 2）+ pb_pending()/voice_*_us 取值器
   + heapmon + 'E'/'X'/'n' 探针键 + 探针缓冲运行期化⑥ + wakenet 让渡
   回调（convo_notify_wake）。
3. 恢复后首验：完整启动序列 + 't' 内置轮（若 't' 活 'W' 死= TTS 特异，
   上游报告再精化）。
4. 上游 issue 提交（素材已齐）：esp_hosted SDIO timing sensitivity。
5. 若上游短期无解的工程 Plan-B：显示与语音分时（SPEAKING 期暂停 DPI
   扫描——产品不可接受，仅诊断用）；或降 SDIO 时钟（Kconfig 查 slot
   频率档）。

## 门禁状态

**未达成**（三态切换 soak / 无新瑕疵 / 用户目验均待阻断解除）。
已达成：合并编译、完整启动、渲染预算、表达桥代码路径、kaleido_express
L0 组件。用户曾报"频闪"（早期构建，当时表达环挂错屏+数组 ext_ram），
最新配置未经用户目验——阻断解除后一并复验。
