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


---

## 追记（2026-10-27 凌晨）：上游对照命中，SDIO 阻断已解，暴露第二阻断

**上游搜索命中（负责人指示）**：esp-hosted-mcu 已知缺陷群（EHM-188
跟踪体系），与我方症状精确对应的三条根因：

1. **#219：PSRAM mempool + 128B 缓存行 → err 258**：TX 缓冲 1600B 步进，
   1600 % 128 = 64，**隔一个缓冲即 DMA 非法**——外观"随机/间歇"。
   确认修：`CONFIG_CACHE_L2_CACHE_LINE_64B=y`（社区实测 0 失败 vs 602）。
   我方显示基线恰好带 128B（osptek 例程口径），叠加 hosted PSRAM 池
   =教科书命中。这也解释了"显示组件参与链接即崩"的表象——真正的
   载体是显示工程带来的 128B 配置。
2. #167 追踪串（EHM-188）：CMD53 块模式 TX 缺陷（err 0x102，持续负载
   后 DAT0 反同步），workaround=H_SDIO_TX_BLOCK_ONLY_XFER 0（备选）。
3. Cache_WriteBack_Addr 线程安全缺陷（建议换 esp_cache_msync）。

**实测结果（64B 修正 + 合并版 main 按恢复配方重建）**：
- ✅ 合并应用**完整启动**：boot 101k → 舞台 24fps → KEX idle → wifi →
  READY（64B 行消耗更多内部 tag RAM，boot 内部余量 288k→101k）；
- ✅ **SDIO Unrecoverable / assert 954 全部消失**——#219 修法在我方
  复现上有效，第一阻断确认解除；
- ⚠ **第二阻断（新症状，独立缺陷）**：'W'/soak 的 announce TTS 段
  **硬锁死**（HP_SYS_HP_WDT_RESET，无 Guru/abort 转储 = CPU 整体停摆），
  发生于 pre-wifi 内部余量 46k 的 TLS 喷发路径。疑向：内部枯竭 ×
  esp-aes DMA × 64B 组合（128B 时代同路径报 esp-aes 分配失败，64B 下
  变硬锁）。

**第二阻断候选杠杆（下会话）**：挤内部（stage 渲染 pthread 栈再降/
PSRAM 栈、LVGL_PERF_MONITOR 关、探针更瘦）；H_SDIO_TX_BLOCK_ONLY_
XFER=0 头补丁；announce 栈与 TLS 时序排查；上游 Cache_WriteBack 补丁。

**配置定稿（当前已提交态）**：64B 行 + hosted PSRAM 池 + 预留池 8K +
LVGL 16K 栈 + 舞台 24fps + 探针缓冲运行期 PSRAM。

---

## 追记 #2（2026-10-26 深夜）：第二阻断攻破 + 频闪取证 + 内存清单

### 第二阻断：根因与修复（已验证）

**内存压力说证伪**：探针缓冲 5×32KB 静态→运行期 PSRAM（+160KB）+
`sw_rotate=false`（+2MB PSRAM）后，boot 101k→**283k**、pre-wifi
46k→**227k**，'W' 仍秒死（`rst:0x7`，连 IRAM panic 处理器都未及运行
——CONFIG_ESP_PANIC_HANDLER_IRAM=y 已开）。死点不在 TLS，在更早。

**二分定界**：'w'（仅加载 wakenet，无播报）同样秒死 → 凶手在
`esp_srmodel_init/create_from_config`；voice-loop 单机版同路径 30 轮
全过 → 差异变量 = **显示管线活着**（DPI DMA 扫描 + LVGL 刷新 + 舞台
线程）。'X' 关 DPI 探针本身有竞态（菜单线程裸调 disp_on_off × LVGL
draw_bitmap），弃用。

**根因**：wakenet 加载（mmap 闪存模型填充 + 大量 PSRAM 分配）× 显示
管线并发（LVGL flush 的 FB `esp_cache_msync` 回写 × DPI 扫描）→
缓存维护路径自旋锁互卡 → 双核死锁 → 3s INT-WDT panic 亦无法打印 →
HP WDT 静默复位。与上游 #167（Cache_WriteBack_Addr 线程不安全）同族。

**修复（wakenet_start 隔离包装）**：加载期间 ①冻结 thorvg 舞台线程
（lottie_stage_freeze）②持 LVGL 互斥（干净停刷新，无竞态）。DPI 扫描
仍在跑——若后续仍死则指向扫描层（届时再细化）。

**验证**：'w' 完整加载（分段打印全程 rc=0，模型 wn9_nihaoxiaozhi_tts，
int −6KB / PSRAM −393KB）；'W' 9 轮 TTS 播报全过零锁死；'C' 浸泡
200s 9+ 轮零复位；`VL,HEAP,OK` 巡检持续干净。

### 频闪取证（用户报"动画一直在频闪"）

| 发现 | 证据 | 处置 |
|---|---|---|
| **开机白闪成分**：LVGL 默认屏（#F7F7F7，RGB565=F7BE）先进 fb0 | 转储实证 fb0 整片 F7BE / fb1 全黑（calloc 初值）恰在 boot 早期 | ✅ 修复：默认屏压产品底色 + 背光推迟到舞台屏就绪后 |
| 复位引发的全屏闪烁 | 每次 HP WDT 复位 = 黑屏→白闪→恢复 | 第二阻断修复后复位源消失（见下"串口 artifact"） |
| **宿主侧 artifact**：PC 开关串口会使板子复位（CH343 DTR 毛刺） | 两次"自发复位"均发生在我方脚本 close/open 串口瞬间；7min 稳定连接 + 200s 浸泡零复位 | 取证纪律：监听窗口不关闭串口 |
| 持续频闪（运行时） | 修复版 FB 内联采样（STAT 心跳 y300/y600 × 双 fb）25s+ 全程同步无分叉 | **待用户目验复判**；仪器持续在岗 |
| 旧版曾见 fb0 舞台区整片纯白（修复前采样） | run2 采样实证 | 未再复现；若复发 FB 采样会捕获 |

### 新增开放项（移交后续会话）

1. **采音所有权调度缺陷族**：aio_capture 全程持锁 + feed_task 高优先级
   紧循环 → 低优先级消费者饿死（'l' 环回 capture=0 实证）；浸泡轮
   speech_ms=0（VAD 全程静音）疑同族但 el_ms=9444 表明 convo 采到了
   数据——机理待 V-46 采音所有权重构（单一分发任务）时一并解决。
   'a' 声学轮证明 wakenet 加载态下麦克风+扬声器+过空链路全部正常
   （air_snr_db=26.0）。
2. **DashScope ASR 云侧失败**：'t'/'a' 均 asr_open（MiniMax TTS 同网
   正常）——key 配额/有效期待查，非固件回归。
3. 四态 soak 判定需 ASR 恢复后重跑。

### 本轮资产

- `docs/mem-2026-10-26-内存占用清单.md`（组件/符号/任务栈/台阶/杠杆全表）
- FB 采样 + VL,HEAP 巡检 + wakenet 分段打印（常驻诊断仪表）
- 'X' 探针已修安全（持 LVGL 锁再动面板）

---

## 追记 #3（2026-10-27 凌晨）：表情定向落地——esp_emote_gfx + 猫伴表情包

**负责人定向**：表情资产采用 **ESP Emote GFX Packer NEXT**（浏览器打包工具，
https://emote-gfx-gen-tool-dev.pages.dev/ ，GIF/Lottie→EAF 矢量包），测试期
使用乐鑫开源**猫伴表情**（`dl.espressif.com/AE/Emote Pack.zip`，23 段 GIF 源
848²；预编译包 `emote_assets.bin` 5.3MB=22 片段，Apache-2.0）。

**工程落地（convo-stage）**：
- 组件 `espressif2022/esp_emote_gen_player 0.0.2` + `esp_emote_gfx 3.0.5`
  （P4 官方支持）；其 CMake 硬性 `REQUIRES json` 由本地垫片组件
  components/json 转发 cjson 满足（上游变通，零魔改 managed_components）
- 分区：storage 缩 7MB→1MB（无使用方）让位 `emote_gen`(data/spiffs 6MB
  @0x920000)，bin 直写该偏移；NVS/model 不动
- emote_stage.c：flush_cb(RGB565 脏区)持 LVGL 锁直写单张 325² lv_canvas
  （较 thorvg ARGB 三缓冲省 634KB PSRAM）；**锁纪律成文**（gfx→lvgl 单向，
  KEX 回调只入队，防 ABBA——V-45 死锁教训的制度化）
- 尺寸：解析器硬编码 200×150（demo 屏），首帧 update_cb 后 set_size(325²)
  +居中拉正，且每片段切换自愈重拉（播放器按 index 重置）；tip 标签隐藏
- KEX 五态映射：question(未配置)/smile(待命)/investigate(听)/ponder(想)/
  confident(说)；'e' 键轮播 22 片段供目验
- wakenet 隔离包装迁移：lottie 冻结 → emote_stage_pause（同步 ack，先停
  动画后持 LVGL 锁）；lottie_stage.c/thorvg 依赖退役（git 历史可溯）

**验证**：MOUNTED clips=22 → CLIP smile → RESIZED 325×325；'e' 轮播四段
像素随片段真实变化（FB 采样 FFFF↔0000）；'w' 加载全程 rc=0 且恢复后表情
回位；堆巡检 OK。**待用户目验**：猫伴动画质量/五态映射观感/播报共存。

**遗留**：烧录后首个 boot 曾现一次早期 rst:0x7（二次启动即恢复，未复现；
若再现按 V-45 追记#2 的取证纪律抓取）；上游回馈项——index.json 的
width/height 键不被解析器读取（现靠运行期 set_size 变通）。

---

## 追记 #4（2026-10-27）：评审整改 + ASR 悬垂指针修复 + 采音所有权重构（V-46a 提前落地）

**评审（子智能体，7/10）整改**：①emote 任务直改 gfx 对象补持
`gfx_emote_lock`（渲染竞态）；②`emote_stage_pause` 返回 ack 状态，
wakenet 隔离超时显式报错中止（不再静默降级）；③'e' 小写化（大写 'E'
双 TLS 探针解遮蔽）；④resize 改为**切换后一次性**执行（初版做成每帧
事件驱动 → resize→invalidate→渲染→帧事件自旋闭环把整机 CPU 饿死——
两个新教训入注释）；⑤acoustic_round 漏 free 修复；⑥json 垫片去符号
链接（纯 CMake 公共传播）；⑦文档/残留同步（stage_express.h 声明、
sdkconfig thorvg 行、内存清单任务表、计划 §4）。

**ASR 悬垂指针（V-45 期间 asr_open 真凶）**：`wc.uri = host` 的 host
为块作用域栈数组，出块即死；esp_websocket_client 1.8.0 解析晚于栈帧
复用 → **把 Authorization 头串当 URI 解析**（开 ws/transport 日志后
日志实锤）。合并重建时引入的回归（V-42~44 无此写法）。修复：uri/auth
堆持有随会话生命周期。修后 't' 全链复通（ASR 转写全文→LLM→TTS），
'C' 浸泡 **11/11 轮 rc=0 零复位**（wake_hits=1/10 声学命中）。

**V-46a 采音所有权重构（audio_io，调用方零语义变化）**：常驻 aio_cap
任务独占 I2S RX 写 PSRAM 环（2^17 采样/8.2s）；`aio_capture_stream`
带消费者私有游标——独立取流互不偷数据，掉队超半环自跳最新。
**三次实测迭代**（全部入注释成教训）：等待持锁内→仍饿死；共享游标→
大读者只捡屑（128 采样）；独立游标+锁外等待→**'w' 常驻时 'l' 47.7dB
PASS**（重构前 capture=0）。另修 cap_task 创建时序（与 codec open 的
I2S 通道重配并发死锁，开机冻结）。新架构下 'C' 浸泡 7/7 rc=0 无回归。

**V-45 状态**：四态 soak 判据达成（11/11 + 7/7），收口仅余用户目验
（猫伴五态/动画质量）。V-46b/c（AEC 参考环/打断）地基已就位。

---

## 追记 #5（2026-10-27 夜）：动画卡顿根因修复 + KF 应用框架 + crash 取证包

**负责人反馈**：动画时常卡顿（最差 4-5fps）；crash 类问题 debug 成本过高；
要求框架化+模块化+专用调试模式+全 flow 日志。

**卡顿根因（gfx_render 源码级）**：esp_emote_gfx 每帧把脏区切成
`buf_pixels/width` 行分片，**每片 flush 后阻塞等消费者确认**——配置
325×16=16 行/片 → 每帧 20 次「flush_cb→LVGL 锁→整画布 invalidate→
LVGL 全量重绘 325²→等 DPI vsync→notify」往返。双重放大：
①20 次锁往返/帧；②整画布 invalidate 使 LVGL 每刷新周期重绘整块
canvas（~20 倍超量）。gfx 与 LVGL 两渲染任务乒乓，帧预算全耗在重复劳动。

**修复**：分片 109 行（3 次/帧）+ 脏区 invalidate（只重绘本分片）。
**板上实测：FPS 20-25 稳定（空载与 wakenet/浸泡负载同），无卡顿特征**
（FPS 仪表 `DSI,EMOTE,FPS,n` 常驻）。

**KF 应用框架（components/kaleido_core，设计说明见其头文件）**：
- 事件环形日志 `kf_ev()`（192 条内部 RAM 环）：关键 flow 全接入
  （boot/emote/wakenet/convo/aio）；panic/WDT 复位后开机自动回放
  `VL,CRASH,last=...` + 死前 24 条——已在板上实战（本次 boot 抓获
  上个生命周期的 other_wdt，即追记#3 的首启 rst:0x7 观察项）。
- 调试模式 'v' 一键切换：关=发布口径（WARN 级、FB 采样等仪表关闭、
  串口安静）；开=全局 DEBUG+KF 流+诊断仪表。
- 任务心跳 `kf_beat()` + 'u' 健康转储（heap/心跳年龄/事件尾）——
  卡死类先看"谁没跳"。
- coredump 落 flash（新分区 coredump@0x910000 64KB，storage 缩至
  960KB）；解码命令入 tools/hil/convo-stage/README.md。

**遗留观察**：esptool 下载复位后首启偶发极早期 rst:0x7（app_main 之前，
事件环为空无法定位；KF 在岗，若带事件复发即回放）。模块化后续：
audio/convo 随 V-46c AEC 重构时再升组件（避免无谓 churn）。

---

## 追记 #6（2026-10-27 深夜）：V-46b 参考信号探针——通路、对齐、10.5dB 基线

**b-1 参考环**：audio_io 参考环（2^19 采样=32.8s PSRAM，aio_play 唯一
TX 写者挂钩全覆盖）+ `aio_ref_now/get/get_strided`。

**b-2 对齐**：脉冲法不可行（两计数器不同轴：麦计数连续走、参考计数只
在播放时走——首测三次漂移 ~1.2s/窗实证）→ **相关法**：播放期单任务
乒乓采音（capture 等 100ms 恰为 play 实时排水期），参考按候选延迟
互相关取 |峰|（声学链路反相，判据须极化无关），粗扫 32 + 细扫 4 样本。
**实测 offset≈2304-2504（≈144-156ms 在飞 = TX DMA 2560 深度内排水）**。

**b-3 AFE MR 测量**：`"MR"+aec_init=true` AFE 双通道（chunk=512, ch=2）。
首测 0.5dB → **连续排布**（`afe_parse_data` 的"交织→连续"语义）修正后
**10.5dB**——排布是决定性参数。精对齐/HIGH_PERF/长滤波器均不再提升
（10dB 平台=PA 非线性失真残差，线性核+NLP 不可消）。

**b-4 成本账（默认档定基线）**：内部 −14KB / PSRAM −496KB / feed 5.4ms
每 32ms 块（≈17% 单核）。HIGH_PERF 抑制持平而成本 ×5——弃。

**调试方法学战果（本轮最大资产）**：TWDT（`CONFIG_ESP_TASK_WDT` +
frame_pointer）把三次"静默锁死"变成三次精准定位——
① aio_cap 0 读紧旋饿死核0（已修：10ms 退避+zero-run 事件）；
② aio_ref_get 大块 memcpy × 显示管线 esp_cache_msync 并发非确定停摆
（已修：跨步紧凑拷贝 4 倍减压）；
③ aio_ref_get 调用方容量钳制缺失致每 rep 溢出 3.2KB 腐坏 PSRAM 堆
（已修）。另有 相关循环 OOB 读（漏减窗长，兼为"全负相关"假象）。
**HP WDT 复位会清 RAM → 内存事件环在硬锁场景天然失效**（框架边界，
如实入册；软复位路径回放有效）。

**V-46b 判据**：10.5dB < 15dB 目标——**带差距结案**：链路/对齐/排布/
成本全部定界清楚，差距根因（非线性失真）明确且 V-46c 有对策（打断
阈值按感知调、播放期音量策略）。V-46c 集成基线参数全齐。

---

## 追记 #7（2026-10-27 深夜）：V-46c 机制落地 + 两个深层缺陷定界（AEC 驻留挂起）

**V-46c 机制面（已落地并保留）**：
- 语音前端驻留 boot（'MR'/'M' 配置一处翻转），撤静音窗实验完成
- 真打断管线全通：`aio_tx_abort`（100ms 块粒切写）+ `pb_abort_playback`
  （排干）+ 唤醒路由（SPEAKING→BARGE 即听不鸣笛/LISTENING 忽略防重
  触发）+ 'n' 键 SPEAKING 态=打断机制测试入口
- **内存布局系统优化（负责人指令）**：`kf_task_psram` 生成器入框架；
  convo(16K)/wn_say(10K)/asr_close/emote/tlisten 栈迁 PSRAM；gfx 渲染
  栈走组件 `task_stack_in_ext`；beep/cue/stats/s_body/VAD 暂存/轮暂存/
  play&cap 暂存全部 PSRAM 懒分配。**boot 241k→285k、pre-wifi 186k→248k
  （+62KB）**。内部保留清单（DMA/驱动/取证强约束）：I2S/DPI DMA 描述
  符、esp-aes 对齐缓冲、hosted 内部、KF 事件环（14.6KB，硬锁取证需在
  PSRAM 失效场景下存活）
- 教训：`xTaskCreateStaticPinnedToCore` 不接受 core=-1（configASSERT
  →bootloop），负核号须走 `xTaskCreateStatic`（kf_task_psram 已内化）

**缺陷 A：esp-sr AEC3 FFT 无限循环（AEC 驻留挂起）**——"MR"+AEC 在
首遇真实参考数据后 wn_feed 热旋，TWDT 回溯钉 `dl_rfft_post_proc`（定
长循环变异=句柄数值失控，预编译库）。对照实验：PSRAM/内部分配同挂、
convo PSRAM 栈排除、'W' 无真实参考不挂。0.5dB→10.5dB 的排布结论不受
影响（探针短窗可用）；**驻留长跑挂起**。证据包齐（三组 TWDT 回溯），
向上游 esp-sr 提报候选。

**缺陷 B：picolibc stdout 锁死锁族（新定位，P0）**——soak 期 main 卡
`fflush→stdout 递归锁→xQueueTakeMutexRecursive` 永久等待（回溯完整）；
持锁任务异常后锁永不释放 → **所有任务的日志调用连环卡死**（含 feed/
detect——AFE 环空警告的另一半根源）。多任务直呼 printf/fflush 的架构
债，KF 统一日志通道（单写者+锁）是既定解法，下轮专攻。**此缺陷亦使
'W' hits=0/10 的"AEC 免疫"判读存疑**（管道堵塞同样致 0 命中）——
免疫门禁须在缺陷 B 修复后重测。

**当前板上固件**：M+静音窗（回退稳定态）+ 打断管线全量 + 内存新布局。

## 追记 #7 终章：缺陷 B 修复 + DashScope 证书轮换根因 + 全链复活

**缺陷 B（stdout 锁死锁）修复**：KF 统一日志通道上线——单写者
logger 任务独占 stdout（全系统唯一 printf/fflush 点），业务全量
printf→kf_log（行池 32×160B 内部 RAM + 超长堆回落 + 满丢计数），
`esp_log_set_vprintf` 把 ESP_LOGx 一并重定向。串口撕行消失，跨任务
日志争用根除。

**ASR 连锁失败的真根因：DashScope 证书轮换**（非固件回归）：
*.aliyuncs.com 于 2026-10-13 起换用 GlobalSign GCC R46 OV TLS CA 2025
→ Root R46 新链（openssl 取证），板内 IDF bundle 缺 R46 根 → verify
失败 → -0x3000 级联（内存/栈的连串实验皆为此症状所误导）。
**修复**：ASR TLS 显式锚定 R46 根（`cert_pem`，组件内嵌 gs_r46.pem）
——固定服务端 CA 亦为最优产品实践；bundle 回退默认 FULL（MiniMax 依
赖其查找）。绕行时的教训：bundle v2 只存 subject+公钥（不存整证），
DER 子串搜索检验法无效。

**复验（全绿）**：soak 连续 rc=0（asr_len 21~51、早切 0/14/2070/2512ms
正常）；'n' 注入链路 ✓；'l' 环回 38.4dB PASS（PSRAM 迁移无回归）；
KF 心跳/堆巡检在岗。**遗留**：AEC 驻留（缺陷 A）修后可撤静音窗启用
声学打断；真语音唤醒/打断待负责人目验。

## 追记 #8（2026-10-28）：真人测试首报定界——唤醒自动开会话 + wakenet 失聪修复

**负责人首测反馈**：短鸣 ✓ 但探头倾听未出现 + 全屏淡蓝一闪。
**coredump 取证定界**（flash 转储 → 手剥 ELF note → GDB 读现场）：
TWDT 触发（IDLE0 饿）→ TWDT ISR 内帧指针回溯器踩野指针自毁（esp_fp_
get_callers）→ panic 重启 → 面板重置=淡蓝一闪。**"未倾听"的产品缺陷：
FSM 未启动时声学唤醒事件被静默丢弃**。

**修复**：①`wake_handler`——声学唤醒时 FSM 未启动则自动开会话（产品
行为=说唤醒词即开始倾听；asr/tts/llm 配置提升文件作用域）；②gfx 渲染
钉核 1 + 音频任务提优先级 6（EAF 单核 ~95% 曾把 IDLE0/音频节拍挤到
TWDT 周期报警）；③麦克风增益 30→42；④'v' 调试键补上（此前插入锚点
未匹配静默丢失）；⑤wn_det/feed 响度仪表（DBGVOL/DBGIN）。

**wakenet"失聪"修复**：与 V-40 实证 9/10 版逐行 diff——唯一实质差异
= **`afe_config_check()` 调用**（原版未调用）。其"参数修正"在本板
破坏唤醒检测。删除后 'W' 首次命中（vol -41.5）+ AUTOSTART 全通。
后续自播报被自动会话的静音窗吞掉属 M 模式设计行为（真人语音不受限）。
