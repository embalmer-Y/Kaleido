# 05-音频 codec 与架构调研

| 版本 | 日期 | 状态 |
|---|---|---|
| v1.0 | 2026-10-23 | 初稿（G1 调研） |

所有网页来源访问日期均为 **2026-10-23**。事实分级：【官方确认】/【供应商声称】/【社区经验】/【待验证】。

## 结论先行

1. **推荐架构 A（无 codec，全数字链路）为喇叭主链路**，置信度 **高（85%）**：PDM 数字麦 → ESP32-S31 I2S0 PDM RX（片内硬件 PDM→PCM 降采样）→ CPU/AFE → S31 I2S（标准 I2S，作主机）→ I2S 数字输入功放（TAS5805M/MA12040P，从机、无需 MCLK）。链路中不存在模拟音频信号，**喇叭通路根本不需要 codec**。S31 具备 2×I2S、PDM 硬件解调、硬件 ASRC、I2S MCLK 输出，全部为 datasheet/官方文档确认。
2. **耳机口建议：USB-C 数字音频（UAC gadget）优先，3.5mm/板载 codec 作为 G2 决策点**，置信度中（60%）。S31 带 USB 2.0 HS OTG【官方确认】，官方 `usb_device_uac` 组件支持 esp32s31 目标【官方确认】，USB-C 口兼作充电时二者可并存（OTG + 充电需 PD/CC 协商，硬件专题确认）。若产品定位确认需要 3.5mm 立体声耳机，则采用**架构 B-lite：加 ES8388（立体声 DAC，LCSC ~$0.56）只做耳机输出**，esp_codec_dev 原生支持；ES8311 为单声道 codec，做立体声耳机不合适。
3. 架构 C（codec DAC→模拟功放）与已定 I2S 数字功放直接矛盾，且模拟链路引入底噪/直通风险，仅作对比，**否决**。官方对标件 ESP-Mosaico 走的正是 C 类链路（ES8311→NS4150B 3W 模拟 D 类），本机以数字功放超越。
4. 时钟架构：**S31 为唯一时钟主机**。I2S 主机模式输出 BCLK/LRCK（及需要的 MCLK 256×fs）给功放（可选 codec）；PDM_CLK 1.024 MHz 给麦；全部时钟源自 S31 APLL（datasheet 标称 120 MHz）。功放为 I2S 从机、无 MCLK 依赖（TAS5805M datasheet 明示，本地件库 PDF 已核对）。**Wi-Fi/蓝牙流抖动不走音频时钟域调整，用硬件 ASRC（或 tune_rate 微调）吸收**，S31 硬件 ASRC 官方基准：CPU 0.79% 平均 vs 软件重采样 2.35%，内存 539 B vs 19.4 KB。
5. **给功放专题的接口约定**：I2S（Philips）从机、LRCK=48 kHz、slot 32 bit（数据 16/24 bit 可选，建议 24 bit）、BCLK=64×fs=3.072 MHz、无 MCLK；TAS5805M 支持 32–96 kHz 自动检测与热切换（切换前时钟停 ≥100 µs）。语音采集侧独立跑 16 kHz（WakeNet 硬性要求），播放侧统一 48 kHz，跨采样率由 S31 硬件 ASRC 完成。

## 对比表 1：三种架构 × 关键维度

| 维度 | A：无 codec（全数字） | B：codec 只做耳机/线路输出 | C：codec DAC→模拟功放 |
|---|---|---|---|
| 喇叭链路 | PDM→S31→I2S→数字功放（全数字，零模拟） | 同 A（喇叭仍走数字功放），codec 仅挂耳机/line out | codec DAC 模拟输出→模拟输入功放 |
| 信噪比瓶颈 | 麦 SNR（~68 dBA）与喇叭本底；无 codec 附加损耗 | 同 A + 耳机通路 codec DAC SNR（ES8388 96 dB） | codec DAC SNR + 模拟走线/耦合损耗 |
| BOM 增量 | $0 | ES8388 ~$0.56 + 耳机座 + 少量阻容 [8][9] | codec（$0.5–2.3）+ 模拟功放（需重选，与已定数字功放冲突） |
| 软件支持 | esp_codec_dev 提供 dummy_codec/PA-only 路径【官方确认】[7] | esp_codec_dev 原生支持 ES8311/ES8388【官方确认】[7] | 同 B，但功放选型需推翻重做 |
| 延迟 | 最短（数字直通） | 喇叭同 A；耳机多一级 DAC（同板时钟，无额外域） | 模拟链路最短，但整体架构被否 |
| 结论 | **采纳（喇叭主链路）** | **有条件采纳（仅耳机需求确认后）** | **否决**（与 I2S 功放选型矛盾） |

## 对比表 2：codec 候选 × 关键维度（仅架构 B 场景）

电气参数为官方 datasheet 数值【官方确认】；价格为渠道快照【供应商声称】。

| 维度 | ES8311（乐鑫生态标配） | ES8388 | WM8960（排除） | NAU88C22 |
|---|---|---|---|---|
| 结构 | **单声道** codec（mono ADC+DAC+差分耳机驱动） | 立体声 codec（2ch ADC+2ch DAC） | 立体声 codec + 1W/声道 D 类 | 立体声 codec + 1W BTL |
| DAC SNR（A 计权） | 110 dB typ（min 105）[8] | 96 dB [9]（LCSC 规格页转抄 DS，档级【供应商声称】） | 98 dB [10] | 89 dB [11] |
| ADC SNR | 100 dB typ [8] | 95 dB [9] | 94 dB [10] | 89 dB [11] |
| 耳机驱动力 | 差分 mono；datasheet 标"14 mW playback and record"低功耗特性，**输出功率规格未在提取章节中出现** [8] | 立体声 HP 输出（集成耳机放大，mW 数待 DS 核实）[9] | 40 mW@16 Ω@3.3 V（THD -75 dB@20 mW）[10] | 40 mW@16 Ω [11] |
| I2S 主/从 | 主/从均可；从机模式要求 LRCK/SCLK 与系统时钟同源派生 [8] | 主/从 [9] | 主/从 [10] | 主/从；内部 PLL 可吃 8 MHz+ 任意系统时钟 [11] |
| 供电/工艺 | 1.8–3.3 V（abs max 3.6 V）；24 bit 8–96 kHz；QFN-20 [8] | 1.7–3.6 V；24 bit 96 kHz；QFN-28 4×4 [9] | 1.8–3.6 V；QFN-32 [10] | 24 bit 8–192 kHz [11] |
| esp_codec_dev 支持 | **是**（Y/Y）[7] | **是**（Y/Y）[7] | **否** [7] | **否** [7] |
| 价格/库存 | LCSC C962342 ~$0.28、~92k 库存 [12] | LCSC C365736 ~$0.56、~46k 库存 [9] | **EOL**：立创 ¥148.5 / DigiKey $10.83 尾货 [13] | Mouser $2.34@1（在产）；批量 ~$0.7@1k [14] |
| 生命周期 | 在产（ESP-Mosaico 在用） | 在产 | **EOL → 按 R3 排除** | 在产（Nuvoton 目录件） |
| 本项目适配判断 | 单声道 DAC ≠ 立体声耳机；若仅做 mono 线路出/调试口可用 | **耳机需求成立时的首选** | 排除 | SNR 最低且无 esp_codec_dev 支持，需自研驱动（R3 允许但无必要） |

## 详细分析

### 1. ESP32-S31 音频外设事实核实（架构判断的根基）

以下全部【官方确认】：

- **2×I2S**：芯片 datasheet（Pre-release v0.5）特性列表"2 I2S interfaces with hardware-level Bluetooth Audio"（§4.2.2.4）；模块 datasheet（WROOM-3）进一步给出 PDM/TDM 细则（§5.2.2.4）：主/从模式、PDM TX 支持 PCM→PDM 转换（≤2 ch）、PDM RX、TDM ≤16 ch、8/16/24/32 bit [1][2]。
- **PDM 输入能力**（ESP-IDF I2S 驱动，esp32s31 构建）[3]：
  - "ESP32-S31 contains two I2S peripheral(s)"；"All I2S ports can transmit and receive raw PDM data"；**PDM→PCM 硬件转换仅 I2S0 RX 方向**（PCM→PDM 仅 I2S0 TX）。
  - PDM RX 的 DATA 引脚为 `dins[4]`（配合 `I2S_PDM_RX_LINEx_SLOT` 槽位使能）→ **最多 4 根独立 DATA 线**，双麦各走一根线最简。
  - 降采样模式 `I2S_PDM_DSR_8S`（PDM_CLK=fs×64）与 `DSR_16S`（×128）；PDM 时钟典型区间 1.024–6.144 MHz。16 kHz 采集 → PDM_CLK=1.024 MHz，与报告 04 全部在产候选的标准模式时钟区间兼容。
- **ASRC（硬件异步采样率转换器）**：芯片 datasheet §4.2.1.3 有专节；模块 datasheet §5.2.1.3 描述为"accelerator-type audio processing module，用于不同采样率间的高质量转换"，双通道可独立或联合使用，支持 mono/stereo 各方向组合，**转换率覆盖 8/16/32/44.1/48 kHz** [1][2]。
- **MCLK 输出**：I2S 驱动支持 MCLK 输出（256×fs 常用，24 bit 建议 384×fs），同一控制器 MCLK 只能挂一个通道 [3]。时钟源含 APLL_CLK（datasheet §4.1.4.3 标称 120 MHz）[1]。
- **USB**：USB 2.0 **High-Speed OTG**（480 Mbit/s，主机/设备）+ USB Serial/JTAG 双外设 [1][2] → USB 数字音频硬件可行。
- **esp_asrc 组件 v1.0.1** [4]：目标芯片表中 **仅 ESP32-S31 与 ESP32-H4 标注"Hardware + software"**（其余含 S3/P4 均为纯软件），硬件模式内存 ~0.5–0.6 KB、CPU ~0.8–3.5%；支持 ≤192 kHz、4000 Hz 或 11025 Hz 整数倍采样率对。
- **ESP-ADF 蓝牙音频方案页** [5]：芯片对比表明确 S31 独有"Hardware ASRC"（ESP32/S3 为 No）；硬件 ASRC vs 软件重采样 CPU 0.79% 平均/1.86% 峰值 vs 2.35%/14.17%，内存 539/672 B vs 19.4/87 KB；I2S"2 controllers, hardware Bluetooth audio sync"。

结论：S31 是一颗"为音频链路定制过"的 SoC——PDM 直采 + 双 I2S + 硬件 ASRC + 蓝牙音频同步齐备，A 架构成立的全部硬件条件都由官方文档背书。

### 2. 三种架构逐一评估

**架构 A（无 codec，全数字）**
- 拾音：双 PDM 麦（见报告 04）→ I2S0 PDM RX（硬件降采样为 16 kHz PCM）→ ESP-SR AFE（AEC 用 I2S 播放缓冲作回声参考）。
- 播放：S31 I2S（建议 I2S1，规避 I2S0 被 PDM/蓝牙路径占用）作主机 → 数字功放 I2S 从机。功放侧 TAS5805M datasheet（本地件库已核）[15]：无需 MCLK、fs 32–96 kHz 自动侦测、支持在线切换采样率（切换前 LRCLK/SCLK 停 ≥100 µs）、最高 32 bit MSB-first；MA12040P datasheet（本地件库）[16]：I2S 数字输入 44.1–192 kHz、输入口为从机、格式经 I2C 配置。两款候选功放都不需要 MCLK，**A 架构下 MCLK 可以完全不用引出**。
- 软件路径：esp_codec_dev 官方提供 `dummy_codec`/PA-only 用例（"A dummy codec for PA-only speaker designs"）[7]，即乐鑫官方支持"无 codec + 纯数字功放"形态。
- 风险：无模拟输出（调试用示波器看波形需借助 I2S 分析仪或 USB 音频）；耳机需求只能走 USB-C。

**架构 B（+codec 仅为耳机/线路输出）**
- 若产品调研确认需要 3.5mm 耳机或模拟线路输出：加 **ES8388**（立体声 DAC，96 dB SNR，esp_codec_dev 支持，LCSC ~$0.56/46k 库存 [9]），挂到 S31 第二路 I2S TX（S31 2×I2S、每控制器 TX/RX 可配），从机模式，时钟由 S31 MCLK（256×fs=12.288 MHz@48 kHz）+ BCLK/LRCK 提供；喇叭链路不变。
- 为什么不是 ES8311：ES8311 是 **mono** codec（datasheet 首行"Low Power Mono Audio CODEC"）[8]，立体声耳机需两颗或放弃；但其软件生态最成熟（ESP-Mosaico 在用 [17][18]），若仅需 mono 调试/线路输出可选。
- R3 合规：ES8388/ES8311 在 esp_codec_dev 中有开源驱动，无闭源依赖风险；esp_codec_dev 组件的具体许可证文本在 ADR 中逐字核对后记录【待验证】。

**架构 C（codec DAC→模拟功放）**
- 即 ESP-Mosaico 形态（ES8311 DAC → NS4150B 3W 模拟 D 类 [17][18]）。要上 10W 双声道必须换模拟输入大功率功放，等于推翻功放专题选型；且 codec DAC（ES8311 差分 ±0.9×AVDD/3.3 Vrms 满幅 [8]）到功放的模拟走线引入耦合底噪，PCB 分区/接地复杂度上升。**否决**。

### 3. 耳机口必要性（产品判断，置信度中）

- 掌机使用场景（居家/通勤、儿童/极客玩家）中私密聆听需求真实存在；但本机喇叭为第一体验（10W 双声道），耳机是次级路径。
- **USB-C 音频路线**：S31 有 USB 2.0 HS OTG [1][2]；官方 `usb_device_uac` v1.3.2 组件目标列表含 esp32s31，支持最多 8 声道扬声器/4 声道麦、音量/静音控制、同步反馈端点；已知限制：不支持运行时动态切换采样率、"Windows 与 Linux 主机不能同时兼容"（配置二选一）[6]。USB-C 数字耳机的代价：占用 USB 口（与充电分时或走 PD 多角色，硬件专题评估）、系统带宽与固件栈复杂度。
- **3.5mm 路线**：加 ES8388（BOM ~$0.6–1 含座子与阻容）+ 模拟走线，但体验最稳（无协议栈依赖）。
- 建议：G1→G2 期间做一轮目标用户轻量调研（问卷/社区投票，开源项目可直接在 README 发起）；**原理图 v1 预留 ES8388 的 I2S+I2C+模拟输出 footprint（可选贴装，DNP 默认不贴）**，把产品决策成本压到贴装阶段。此为典型"硬件免费期权"，代价仅 PCB 面积。
- 听力安全（constraints.md 第 11.3 条）：若上 3.5mm，需在 codec 输出前加不可软件越权的增益上限设计（如硬件衰减电阻网络），方案在 ADR 中定。

### 4. 时钟架构与 ASRC 位置

**4.1 时钟树（写入 `docs/audio-clock.md` 的骨架）**

```
S31 APLL (120 MHz, datasheet §4.1.4.3)
 ├─ I2S0（PDM RX 主机）→ PDM_CLK 1.024 MHz → 2× PDM 麦（16 kHz PCM 回读）
 ├─ I2S1（STD TX 主机）→ BCLK 3.072 MHz + LRCK 48 kHz → 数字功放（I2S 从机，无 MCLK）
 │    （MCLK 12.288 MHz @256×fs 仅在架构 B 贴装 ES8388 时引出）
 └─ （蓝牙音频路径可走 I2S0 的 BT 数据通道，datasheet"hardware-level Bluetooth Audio"）
```

- 全链路单一时钟源（S31），无独立晶振、无 codec 内部 PLL 独立时基 → 不存在模拟域+数字域双时钟漂移问题；这优于"独立音频晶振喂 codec、Wi-Fi SoC 自己跑"的传统双时钟域方案。
- ES8388 若贴装：从机模式，datasheet 要求 LRCK/SCLK 与系统时钟同源派生（ES8311 datasheet 表述，ES8388 同族设计惯例【待验证】），由 S31 MCLK+BCLK 满足。

**4.2 ASRC 在链路中的位置（Wi-Fi 音频抖动的吸收点）**

- 问题定义：云端 LLM 音频流经 Wi-Fi 到达的速率天然抖动（网络缓冲不均匀），而 I2S 播放时钟恒定 48 kHz。两类解法：a) 缓冲水位反馈微调 I2S 时钟（S31 提供 `i2s_channel_tune_rate()` 运行时微调 [3]）；b) **硬件 ASRC 在播放前把"网络时钟域"重采样到"播放时钟域"**。
- 官方背书：ESP-ADF 音频方案页 [5] 与 esp-gmf/esp_asrc 组件 [4] 均把 S31 硬件 ASRC 定位为音频管线（解码→效果→AEC→播放）中的采样率适配单元，BT/Wi-Fi 多源场景收益最大（CPU 0.79% vs 2.35%，内存 539 B vs 19.4 KB）。
- 本机管线建议：`麦 16k ─┐`；`云流/BT/本地解码(任意 fs) → 硬件ASRC → 48k 播放缓冲 → I2S1 → 功放`；`48k 播放缓冲 → 降采样参考 → AFE AEC(16k)`。AEC 参考与麦同为 16 kHz，需一次 48k→16k 重采样（ASRC 或软件，成本低）。
- 备选简化：若首版不做云端流式播放（整段缓存后播），ASRC 可延后启用，仅靠 tune_rate 即可；但 LE Audio/经典蓝牙 A2DP 路径建议保留 ASRC（BT 时钟域与 I2S 时钟域必然异步）。

**4.3 给功放专题的接口约定（冻结建议，G2 走 ECR 可改）**

| 项 | 约定值 | 依据 |
|---|---|---|
| 接口 | I2S（Philips），功放为从机 | TAS5805M [15]、MA12040P [16] 均从机输入 |
| 采样率 | 48 kHz（语音提示/音乐/游戏音效统一；32/44.1/88.2/96 kHz 功放均兼容，S31 侧 ASRC 统一到 48k） | TAS5805M 32–96 kHz 自动检测 [15]；MA12040P 44.1–192 kHz [16] |
| 位宽/slot | slot 32 bit，有效数据 24 bit（16 bit 兼容） | TAS5805M 接受最高 32 bit [15]；24 bit 保留 DSP 动态余量 |
| BCLK | 64×fs = 3.072 MHz | 32 bit×2 ch 标准 I2S |
| MCLK | 不连接（若同板贴 ES8388 则 256×fs=12.288 MHz 仅到 codec） | TAS5805M"no MCLK required" [15] |
| 控制总线 | I2C（功放配置/静音/故障）+ 至少 1 个 GPIO（PD/SD/FAULT） | 两款功放均有 I2C+控制脚 [15][16] |
| 采样率热切换 | 允许，切换前 LRCLK/BCLK 停 ≥100 µs | TAS5805M"Sample Rate on the Fly" [15] |
| 开机噗啪 | 功放侧静音时序由固件控制 PD/SD 脚先行 | 功放专题细化 |

### 5. 推荐架构与置信度

- **主链路 A（无 codec）：85% 置信度**。扣分项：S31 datasheet 为 Pre-release v0.5，PDM/ASRC 行为需首板实测；esp_codec_dev dummy-PA 路径在 S31 上的成熟度需一个 bring-up 工程验证。
- **耳机口：USB-C UAC 优先 + ES8388 footprint 预留：60% 置信度**（本质是产品决策，硬件侧两条路都已铺好）。
- 架构 C 否决：95% 置信度（与功放选型硬冲突）。

## 风险与待验证项

| # | 风险/缺口 | 等级 | 验证手段 | 责任落点 |
|---|---|---|---|---|
| R1 | S31 datasheet 为 **Pre-release v0.5**，PDM RX 硬件解调、ASRC 通道数、APLL 精度（jitter 对音频时钟的相位噪声影响）未最终定稿 | 高 | 订阅乐鑫文档更新；首板用示波器/音频分析仪测 I2S 时钟抖动与 PDM 波形；esp_asrc 组件跑 16k↔48k 转换正确性测试 | G3 原理图评审前复核 datasheet 新版；G5 实测 |
| R2 | I2S0 被 PDM RX 占用后，I2S0 TX（蓝牙音频 BT 数据路径要求 I2S0）与功放 I2S1 TX 的共存/仲裁策略未验证 | 中 | ESP-IDF v6 最小工程：I2S0 PDM RX + I2S1 STD TX 同时跑；蓝牙 A2DP/LE Audio demo 时再验 BT 路径 | G1 软件生态专题 + G5 |
| R3 | esp-sr（WakeNet9）在 S31 的模型支持未在文档正文显式确认（见报告 04 R4） | 中 | esp-skainet 在 S31 编译运行验证 | 软件生态专题 |
| R4 | ES8388 完整官方 datasheet 未取得（SNR 96/95 dB 现为 LCSC 规格页转抄）；耳机驱动力（mW@Ω）未知 | 中 | 向 Everest（everest-semi.com）或代理商索要 DS；或改用 Waveshare WM8960 板级数据作立体声参考对照 | G2 前（仅当耳机路线选 ES8388） |
| R5 | usb_device_uac 在 S31 的实配（OTG+充电并存、手机/PC 兼容性、"Windows/Linux 二选一"限制影响）未实测 | 中 | S31 官方模组 + USB-C 扩展板跑 usb_device_uac example，Windows/Linux/macOS/Android 四端点测 | G2 前（若耳机走 USB） |
| R6 | Wi-Fi 云流场景下 ASRC + tune_rate 双机制的水位控制策略（谁做主）无官方参考实现细节 | 中 | esp-gmf 音频管线示例（esp_asrc 包）先行评估；首版固件做缓冲水位日志调参 | 固件专题 G5 |
| R7 | 功放 10W 大电流对 I2S/PDM 信号完整性与麦的结构/电气串扰（constraints.md §5.4 已有分区规则） | 中 | 首板按分区布局 + HIL 音频回路底噪测试（CI 四项必测之一） | 硬件+G5 |
| R8 | 3.5mm 耳机口的产品必要性无用户数据 | 低 | G1→G2 期间社区问卷；DNP footprint 方案使决策可延后到贴装 | 项目负责人/产品 |

## 来源清单

1. Espressif，ESP32-S31 Series Datasheet（**Pre-release v0.5**）：特性列表（2×I2S with hardware-level Bluetooth Audio、USB 2.0 HS OTG、RISC-V 双核 320 MHz、512 KB HP SRAM、Wi-Fi 6、BT 5.4、PLL 表），https://documentation.espressif.com/esp32-s31_datasheet_en.html （2026-10-23）【官方确认】
2. Espressif，ESP32-S31-WROOM-3/3U Datasheet：§5.2.1.3 ASRC（双通道、8/16/32/44.1/48 kHz）、§5.2.2.4 I2S（PDM TX/RX、TDM ≤16 ch、主/从）、§5.2.2.6 USB OTG HS（480 Mbit/s 主机/设备、16 host 通道），https://documentation.espressif.com/esp32-s31-wroom-3_wroom-3u_datasheet_en.html （2026-10-23）【官方确认】
3. Espressif，ESP-IDF I2S 驱动（esp32s31 构建）：PDM RX `dins[4]`、DSR_8S/16S、PDM 时钟 1.024–6.144 MHz、MCLK 256/384×fs、`i2s_channel_tune_rate`、I2S0 PDM↔PCM 专属转换，https://docs.espressif.com/projects/esp-idf/en/latest/esp32s31/api-reference/peripherals/i2s.html （2026-10-23）【官方确认】
4. Espressif，esp_asrc 组件 v1.0.1（S31/H4 硬件+软件混合；性能表；采样率规则），https://components.espressif.com/components/espressif/esp_asrc/versions/1.0.1/readme （2026-10-23）【官方确认】
5. Espressif，ESP-ADF 蓝牙音频方案中心（S31 硬件 ASRC 对比表、CPU/内存数据、I2S 蓝牙同步），https://docs.espressif.com/projects/esp-adf/en/latest/solution-center/bluetooth-audio.html （2026-10-23）【官方确认】
6. Espressif，usb_device_uac 组件 v1.3.2（支持 esp32s31；8 声道扬声/4 声道麦；限制条款），https://components.espressif.com/components/espressif/usb_device_uac （2026-10-23）【官方确认】
7. Espressif，esp_codec_dev 组件 v1.6.2（支持芯片表：ES8311/ES8388/ES8374/ES8389/ZL38063/CJC8910/ES7210/ES7243(E)/ES8156/TAS6805M/AW88298 + dummy_codec/PA-only 路径；WM8960、NAU88C22 不在列），https://components.espressif.com/components/espressif/esp_codec_dev （2026-10-23）【官方确认】
8. Everest Semiconductor，ES8311 Datasheet Rev 10.0（2021-01）："Low Power Mono Audio CODEC"、ADC SNR 95/100/102 dB、DAC SNR 105/110/115 dB、THD+N、14 mW 低功耗特性、主/从机时钟要求，https://files.waveshare.com/wiki/common/ES8311.DS.pdf （2026-10-23 下载并全文提取；乐鑫另托管 Rev 7：https://dl.espressif.com/dl/schematics/Audio_ES8311.pdf ）【官方确认】
9. ES8388：LCSC 产品页 C365736（2ch ADC+2ch DAC、ADC SNR 95 dB/DAC 96 dB、THD+N -85/-83 dB、24 bit/96 kHz、QFN-28 4×4、~$0.5591、~46k 库存），https://www.lcsc.com/product-detail/C365736.html （2026-10-23）【供应商声称（规格页转抄，DS 原文待取）】
10. Cirrus Logic，WM8960 Datasheet v4.2（DAC SNR 98 dB、ADC SNR 94 dB、HP 40 mW@16 Ω@3.3 V、1W/声道 D 类），https://www.mouser.com/datasheet/2/76/WM8960_v4.2-532436.pdf （2026-10-23 下载并全文提取）【官方确认】
11. Nuvoton，NAU88C22 Datasheet Rev 0.6（2016-06）（DAC 89 dB、ADC 89 dB、BTL 1W@8 Ω、HP 40 mW@16 Ω、8 MHz+ PLL、主/从），https://www.nuvoton.com/resource-files/NAU88C22DataSheet0.6.pdf （2026-10-23 下载并全文提取）【官方确认】
12. ES8311 价格库存：LCSC C962342（~$0.28、~92k 库存），https://www.lcsc.com/zh-CN/product-detail/C962342.html （搜索快照，2026-10-23）【供应商声称】
13. WM8960 EOL 价格：立创 ¥148.5/片、DigiKey $10.83/片（尾货，渠道快照，2026-10-23）【供应商声称】
14. NAU88C22 价格：Mouser https://www.mouser.com/ProductDetail/Nuvoton/NAU88C22YG （$2.34@1）；X-On ~$0.70@980+（https://www.xonelec.com/mpn/nuvoton/nau88c22yg ）（2026-10-23）【供应商声称】
15. TI，TAS5805M Datasheet（本地件库 /home/emb/project/Kaleido/tas5805m.pdf）：I2S 从机、无需 MCLK、fs 32–96 kHz 自动检测、在线换采样率（时钟停 ≥100 µs）、≤32 bit、TDM 支持（2026-10-23 提取核对）【官方确认】
16. Infineon，MA12040P Datasheet（本地件库 /home/emb/project/Kaleido/ma12040p.pdf）：I2S 数字输入 44.1–192 kHz、输入口从机、格式经 I2C 配置（2026-10-23 提取核对）【官方确认】
17. ESP-Mosaico User Guide（乐鑫官方开发板文档存在性），https://documentation.espressif.com/projects/esp-dev-kits/en/latest/esp32s31/esp-mosaico/user_guide.html （2026-10-23；页面为动态渲染，正文未能抓取）【官方确认（存在性）/待验证（细节）】
18. LinuxGizmos，ESP-Mosaico 报道（ES8311 + NS4150B 3W D 类、板载麦），https://linuxgizmos.com/esp-mosaico-places-dual-core-risc-v-esp32-s31-behind-a-2-16-inch-square-amoled （2026-10-23）【社区经验（媒体拆解级）】
