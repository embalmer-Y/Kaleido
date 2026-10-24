# 04-PDM 麦克风调研

| 版本 | 日期 | 状态 |
|---|---|---|
| v1.0 | 2026-10-23 | 初稿（G1 调研） |

所有网页来源访问日期均为 **2026-10-23**。事实分级：【官方确认】/【供应商声称】/【社区经验】/【待验证】。

## 结论先行

1. **推荐双麦 PDM 方案**：2× 敏芯 **MSM261DDB031**（bottom port，SNR 68 dB，AOP 121 dB SPL，-26 dBFS，3.5×2.65×0.98 mm，立创商城渠道 ~$0.4 量级）；备选同为敏芯的 **MSM261DDB019**（65 dB）与国际渠道备选 **Syntiant/Knowles SPK0641HT4H-1**（top port，64.5 dB，DigiKey ~$0.88@5.7k）。推荐置信度：**中高（70%）**——敏芯官方家族页参数已确认，但单品完整 datasheet、±1 dB 公差档与库存深度仍待验证（见风险项）。
2. 双麦收益判断（置信度中等）：掌机喇叭自播噪声大，**AEC 回声抵消（支持 ≤2 麦）是唤醒体验的关键**，单麦（"MR"模式）即可跑通；双麦（"MM"）可加 BSS 盲源分离提升嘈杂环境唤醒/识别，代价是 PSRAM +~410 KB、单核 CPU +~13%（ESP32-S3 实测基准，S31 算力更强）。乐鑫对双麦间距要求 4–6.5 cm，**结构 envelope 必须在 G2 前确认能否容纳**；放不下则降级单麦 + AEC，唤醒功能不受影响。
3. 生命周期硬结论：**ST MP34DT05-M/-A 与 TDK ICS-41350 均已停产/EOL**（DigiKey 标注 no longer manufactured），按 R3 供货硬规则直接排除；WM8960 同理（见报告 05）。传统欧美 PDM 麦正在退市，**国产敏芯是当前在产、可量产、低成本的主通道**。
4. 接口：双麦共用 PDM_CLK，各走独立 DATA 线（ESP32-S31 PDM RX 支持最多 4 根 DATA 线【官方确认】），采样 16 kHz/16 bit（WakeNet 要求），PDM 时钟 1.024 MHz（fs×64 下采样模式），落在全部候选的标准模式时钟区间内。
5. 声学：麦孔 ≥1 mm、孔深:孔径 <2:1、密封硅胶套、远离 10W 喇叭并加橡胶垫隔振；bottom port 需 PCB 开孔 + 壳体声孔对准 + 密封圈设计，需求已列入"结构件提资"（§3.3）。

## 对比表：PDM 麦候选 × 关键维度

数据来源见来源清单编号；电气参数取自各官方 datasheet（【官方确认】），价格/库存为渠道页面快照（【供应商声称】，数量档以实际询价为准）。

| 维度 | 敏芯 MSM261DDB031（首推） | 敏芯 MSM261DDB019（备选） | Syntiant/Knowles SPK0641HT4H-1 | Knowles SPH0641LU4H-1 | ST MP34DT05-M/-A（排除） | TDK ICS-41350（排除） |
|---|---|---|---|---|---|---|
| SNR（dBA，典型） | 68 [11] | 65 [11] | 64.5（标准模式 2.4 MHz）[6] | 64.3（2.4 MHz）[5] | 64 [7] | 64（标准模式）[9] |
| 灵敏度（dBFS） | -26 [11] | -26 [11] | -26 ±1 [6] | -26 ±1 [5] | -26 ±3 [7] | -26 ±1（标准模式）[9] |
| AOP（dB SPL） | 121 [11] | 120 [11] | 120 [6] | 120 [5] | 122.5 [7] | 120（高性能模式 126）[9] |
| 电流（典型） | 待验证（家族页未列） | 待验证 | 630 µA@2.4 MHz / 230 µA@768 kHz / 待机 26 µA [6] | 620 µA@2.4 MHz / 235 µA@768 kHz / 待机 80 µA(max) [5] | 650 µA@2.4 MHz [7] | 430 µA@2.4 MHz / 185 µA 低功耗 / 待机 12 µA [9] |
| 时钟范围 | 待验证（同族 MSM261S 为 1.0–4.0 MHz 正常 / 150–800 kHz 低功耗 [12]） | 待验证 | 1.0–4.8 MHz；低功耗 350–800 kHz [6] | 标准 1.024–2.475 MHz；低功耗 351–815 kHz；超声 3.072–4.8 MHz [5] | 1.2–3.25 MHz [7] | 标准 1.0–3.3 MHz；低功耗 0.4–0.8 MHz；高性能 4.1–4.8 MHz [9] |
| 端口/封装 | Bottom / 3.5×2.65×0.98 mm [11] | Bottom / 3.5×2.65×0.98 mm [11] | Top / 4×3×1.0 mm LGA [6] | Bottom / 3.50×2.65×0.98 mm [5] | Top / 4×3 mm HCLGA [7] | Bottom / 3.5×2.65×0.98 mm [9] |
| 双麦共用 DATA 线 | 待验证（PDM 常规 L/R 选择） | 待验证 | 支持（SELECT 引脚复用）[6] | 支持（SELECT 引脚复用）[5] | 是（ST 板级惯例） | 支持（单时钟单数据线时分复用）[9] |
| 价格（渠道快照） | 待询价（同族在售 $0.29–0.48）[13] | $0.38@低量，482 库存 [13] | DigiKey ~$0.88@5.7k；Mouser $1.89@1 [15] | Mouser $3.22@1/$2.54@10；DigiKey ~€1.41@5.9k [14] | EOL 尾货 [8] | EOL 尾货 $1.8–1.95 [9] |
| 生命周期 | 在产（官方家族页在列）[11] | 在产 [11] | 在产（Mouser ~19.8k 库存，厂线 16 周）[15] | 在产（Syntiant 接盘 Knowles 消费线）[14] | **EOL/NRND，排除** [8] | **EOL（DigiKey 不再生产），排除** [9] |

注：任务书提及的"MSM261S 系列"经核实 **MSM261S4030H0R 是 I2S 输出而非 PDM**（datasheet V1.0 明确"Top-ported, I2S digital output"）[12]，PDM 对应系列为 **MSM261D**，本报告已纠正。另注意 TDK ICS-43432/ICS-43434 同为 I2S 输出而非 PDM [10]。

## 详细分析

### 1. 候选逐评

**1.1 敏芯 MEMSensing MSM261D 系列（首推通道）**
- 官方家族页 [11] 列出全系列 PDM 型号，其中 bottom port 3.5×2.65×0.98 mm 封装段（与 Knowles SPH0641LU4H-1 / TDK ICS-41350 同尺寸段，潜在替代关系）：MSM261DDB031（SNR 68 / AOP 121，"电视、手机"定位）、MSM261DDB019（65/120）、MSM261DDB034（66/121）；高 AOP 段 MSM371DDBB46（-37 dBFS，SNR 70，AOP 130）与 MSM371DDBA01（68/130）。【官方确认（家族页）】
- 供货：立创商城在售多款（MSM261DGT003 $0.4829/2700 片、MSM261DHT006 $0.2944/2688 片、MSM261DHP006 $0.3943/1108 片、MSM261DDB019 $0.3812/482 片）[13]；**MSM261DDB031 未在快照中确认立创库存**，为最大待验证项。社区常引用的 MSM261D4030H1CPM（C966942，$0.34-0.37）目前无货，且未出现在官方家族页，需向厂商核实定位 [13]。【供应商声称】
- 判断：SNR 68 dB 超过乐鑫"推荐 >64 dB"线 [1]，双麦 BOM ~$0.8，成本、性能、渠道三项均衡，是 1k 台量级的最优通道。

**1.2 Syntiant/Knowles SPK0641HT4H-1（国际渠道备选，top port）**
- 官方 datasheet [6]：SNR 64.5 dB(A)、-26 dBFS ±1、AOP 120 dB SPL、LFRO 35 Hz、平坦 20 Hz–20 kHz；1.6–3.6 V；630 µA@2.4 MHz；支持双麦数据线复用；RF 屏蔽；待机 26 µA。文档同时给出声学结构建议：密封胶圈腔体直径 ≥1.0 mm、壳体孔径 1.0–1.5 mm。【官方确认】
- 供货：Mouser ~19,800 片现货（$1.89@1，厂线交期 16 周）、DigiKey ~$0.88@5,700 [15]。品牌已由 Knowles 消费线并入 Syntiant，渠道仍在正常供货。【供应商声称】
- 判断：作为国际渠道第二源与工程验证对照板用麦合适；单价高于敏芯 2 倍以上，不作 1k 主通道。

**1.3 Knowles SPH0641LU4H-1（bottom port 老牌，价格劣势）**
- 官方 datasheet（Rev A, 2014-05-16）[5]：SNR 64.3 dB(A)、AOP 120 dB SPL、235 µA@768 kHz 低功耗档、多模式 + 80 kHz 超声档（3.072–4.8 MHz）。生态最广（各类开发板常用）。【官方确认】
- 价格 $1.4–3.2，是敏芯 4–8 倍 [14]；文档为 2014 年版，选它没有获得性能溢价。判断：仅作 bottom port 脚位兼容参考（与敏芯 DDB 系列同封装段）。

**1.4 ST MP34DT05-M（排除）**
- 电气参数不差（AOP 122.5 dB SPL 为候选中最高 [7]），但 DigiKey 标注 "no longer manufactured"、Mouser 标注 "Scheduled for obsolescence" [8]。触发 R3"禁用 EOL/NRND"红线，排除。替代料为 IMP34DT05（工业级）或 MP34DT06J（±1 dB 公差）[8]，如需 ST 渠道可在 ADR 中列为三源。
- 附注：任务书中的"MP34DT05-M"为 ST/Arduino 评估板丝印型号，ST 目录文档对应 **MP34DT05-A（top port）**；两者为同一硅片的板级标注差异 [8]。【社区经验 + 官方目录交叉】

**1.5 TDK ICS-41350（排除）**
- 规格优秀（多模式、低功耗 185 µA、高性能模式 AOP 126 dB SPL、超声 40 kHz）[9]，但 DigiKey 标注停产，仅剩 Mouser ~9.5k / DigiKey ~7.4k 尾货 [9]。排除，理由同上。若未来做超声交互（如近场手势）可关注其官方替代型号（TDK 产品页查询 [9]）。

### 2. 单麦 vs 双麦（ESP-SR/WakeNet 视角）

**2.1 乐鑫对麦的硬指标 [1]【官方确认】**（Espressif Microphone Design Guidelines）：
- 全向 MEMS；数字麦灵敏度 ≥ -26 dBFS；**SNR ≥62 dB，推荐 >64 dB**；频响 50 Hz–16 kHz 内 ±3 dB；PSRR >55 dB。
- 单麦灵敏度公差 ±2 dB 内；**阵列（双麦）要求公差 ±1 dB**，且同型号、灵敏度差 ≤3 dB、相位差 ≤10°。
- 结论：首推敏芯 68 dB SNR 满足"推荐线"；但 ±1 dB 公差档需向敏芯确认（Knowles SPK0641/SPH0641 datasheet 明确 ±1 dB [5][6]）。

**2.2 AFE 配置与资源代价 [2][3]【官方确认】**（ESP32-S3 上的官方基准，S31 需样机复测）：
- 输入格式字符串：单麦+回声参考 "MR"；双麦 "MM"；双麦+参考 "MMNR"。AEC 最多 2 麦；BSS 盲源分离为双通道算法；NS 为单通道。
- 资源：单麦（MR, SR, LOW_COST）内部 RAM 60.1 KB + PSRAM 739.7 KB，feed 8.8%/fetch 9.8% CPU；双麦（MMNR, SR, LOW_COST）RAM 79.1 KB + PSRAM 1153.7 KB，23.7%/22.9% CPU。**双麦较单麦：PSRAM +约 410 KB、fetch CPU +约 13 个百分点**。S31 封装内 16/32 MB PSRAM 与双核 320 MHz 承受无压力，但需与 UI/解码共享预算时要在 G2 排布。
- WakeNet 输入要求：16 kHz、单声道、signed 16-bit [4]【官方确认】。故 PDM 采集按 16 kHz 配置（PDM_CLK=1.024 MHz，DSR_8S，fs×64 [16]）。

**2.3 掌机形态判断（置信度中等，含产品判断）**
- 本机 10W 双声道自播场景下，**喇叭→麦的声学/结构回声远大于环境噪声**：AEC（需回声参考通道，"R"）是"边播报边打断唤醒"的前提，比双麦 BSS 更关键；单麦 + AEC 即可获得完整语音链路。
- 双麦的增益点：嘈杂环境（户外/公共交通）语音 BSS 分离、DOA（可作为交互花样）、以及 ESP-SR 的 2 麦 AEC 精度提升；**未找到乐鑫官方公布的"双麦对唤醒率提升 X%"量化数据**（【待验证】，验证手段见风险项 R1）。
- 结论：**双麦为默认设计**（差价 <$1，S31 硬件原生支持），但必须满足 4–6.5 cm 间距与同型号 ±1 dB 配对 [1]；结构放不下则单麦 + AEC 降级，不阻塞唤醒功能。

### 3. 声学设计注意（提资结构件专题）

以下除标注外均出自乐鑫《Microphone Design Guidelines》[1]【官方确认】与 Knowles SPK0641 机械章节 [6]。

**3.1 密封与开孔**
- 声孔孔径 >1 mm；拾声孔深:径 <2:1；密封腔容积尽量小、导管尽量短；腔体谐振频率须 >9 kHz。
- 麦与壳体之间必须加硅胶套/泡棉做密封与减振；防尘网必备。
- 密封性验收（写进结构验收标准）：封堵声孔后 100 Hz–8 kHz 衰减 ≥25 dB 合格、≥30 dB 良好（90 dB 白噪声 @0.5 m 测）。
- Knowles 建议：密封胶圈腔体直径 ≥1.0 mm，壳体开孔 1.0–1.5 mm [6]。

**3.2 bottom port 焊盘与开孔设计**
- Bottom port 声孔在焊盘面：需 PCB 于麦 footprint 中央开声孔（或对应焊盘开槽），壳体声孔与 PCB 孔严格对准，公差链（贴片偏移 ±0.1~0.15 mm + 壳体公差）需在结构 DFM 中核算；麦下方 PCB 区域禁止走高速信号线。
- 好处：声孔朝向手背后侧，远离屏幕正面气流/风噪与用户手指遮挡，适合掌机握持姿态；劣势：桌面平放时可能被遮挡（乐鑫提示 bottom 朝向需保留与桌面间隙 [1]）。

**3.3 与喇叭的隔离（结构耦合提资）**
- 10W 功放工作时壳体振动会经结构传导至 MEMS（结构噪声），这是 PDM 麦底噪的第二来源：要求**麦与喇叭腔体物理分区、中间加橡胶垫/硅胶套缓冲** [1]；麦声孔不得朝向喇叭出声面；PCB 上功放大电流回路与麦分区布局（constraints.md §5.4 已有对应规则）。
- AEC 回声参考信号：从"DA 后级、PA 前级"取参考，Class-D 输出需加 >22 kHz 截止的低通滤波，最大音量下参考峰值保持 -3~-5 dB [1]（本机 PA 为 I2S 数字输入，参考信号直接取 I2S 数字播放缓冲即可实现等效"DA 后级"，滤波要求转化为固件侧约束，写入 kaleido_audio 组件文档）。

**3.4 双麦布局**
- 间距 4–6.5 cm，两麦连线平行于水平方向，阵列中心靠近整机中心 [1]。掌机横向宽度预计 >12 cm，建议两麦置于顶部左右对称位（各距中心 2.5–3.25 cm）或顶部同侧两组；避免分别放左右远端（间距过大超出算法假设）。G2 envelope 冻结前完成摆位图。

## 风险与待验证项

| # | 风险/缺口 | 等级 | 验证手段 | 责任落点 |
|---|---|---|---|---|
| R1 | 双麦对唤醒率/误唤醒的量化收益无官方数据 | 中 | esp-skainet 官方测试框架 [4] 在目标硬件上跑 A/B（单麦+NS vs 双麦+BSS），安静/嘈杂/自播三场景各 ≥1000 次唤醒统计 | G5 HIL（wake 率为四项必测之一） |
| R2 | MSM261DDB031 单品完整 datasheet（电流、时钟范围、公差档、L/R 复用）未取得；立创 1k 阶梯价与深度库存未确认 | 高 | 向敏芯（memsensing.com / 代理商德诺 [17]）索要 DS-xxx 规格书与 ±1 dB 配对说明；立创商城询价 1k 档；同步确认 DDB019/DGT003 作备选的参数 | G2 选型冻结前 |
| R3 | 3.5×2.65×0.98 bottom port 封装与 SPH0641LU4H-1/ICS-41350 的焊盘兼容性未逐 pin 比对 | 中 | 取三家 land pattern 图纸比对；若敏芯不兼容，则以敏芯为准重画，Knowles 仅作样品对照 | 原理图/封装库阶段 |
| R4 | ESP32-S31 的 esp-sr 支持：文档站已有 esp32s31 版本构建，但 WakeNet9 模型芯片列表正文未显式列 S31（仍写 ESP32/S3/P4）[4] | 中 | 用 esp-skainet/esp-sr 最新 release 在 S31 上编译+跑 wn9 模型验证；无则提 issue 追踪乐鑫 | G1 收尾前（软件生态专题联办） |
| R5 | PDM 时钟 1.024 MHz 下敏芯麦性能（ datasheet 典型测试多在 2.4 MHz/3.072 MHz）与 S31 PDM RX 实际波形质量（抖动、占空比）未实测 | 中 | 首板 bring-up 时示波器测 PDM_CLK 占空比（40–60% 区间 [5][6]）；录音频谱验证底噪与频响 | G5 bring-up |
| R6 | 双麦 4–6.5 cm 间距与整机结构冲突 | 中 | G2 envelope 约束图阶段输出麦位摆位图，与显示/按键/喇叭腔协商；冲突则执行单麦降级预案 | 结构件专题 + G2 |
| R7 | bottom port 声孔与壳体公差链导致密封不良（泄漏 <25 dB） | 中 | 按 [1] 泄漏测试法（橡皮泥封孔法）做首个 SLA 壳体验收 | G5 |

## 来源清单

1. Espressif，Microphone Design Guidelines（ESP-SR 文档，esp32s3 构建），https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/audio_front_end/Espressif_Microphone_Design_Guidelines.html （2026-10-23）【官方确认】
2. Espressif，AFE Introduction（input_format/AEC≤2 麦/BSS/MISO），https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/audio_front_end/README.html （2026-10-23）【官方确认】
3. Espressif，ESP-SR Benchmark（AFE 资源占用表，ESP32-S3），https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/benchmark/README.html （2026-10-23）【官方确认】
4. Espressif，WakeNet 文档（16 kHz/mono/s16；wn9 支持芯片列表；esp32s31 文档构建存在），https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/wake_word_engine/README.html 与 https://docs.espressif.com/projects/esp-sr/en/latest/esp32s31/wake_word_engine/README.html （2026-10-23）【官方确认 + 待验证（S31 模型支持）】
5. Knowles，SPH0641LU4H-1 Datasheet Rev A（2014-05-16），https://www.mouser.com/datasheet/2/218/-746191.pdf （2026-10-23 下载并全文提取）【官方确认】
6. Knowles/Syntiant，SPK0641HT4H-1 Datasheet，https://www.mouser.com/datasheet/2/218/spk0641ht4h_1_ramis_datasheet-3312963.pdf （2026-10-23 下载并全文提取）【官方确认】
7. ST，MP34DT05-A Datasheet（DS13121），https://www.st.com/resource/en/datasheet/mp34dt05-a.pdf （2026-10-23 下载并全文提取）【官方确认】
8. MP34DT05 生命周期：DigiKey 产品页 https://www.digikey.com/en/products/detail/stmicroelectronics/MP34DT05TR-A/7385251 ；Mouser 产品页 https://www.mouser.com/ProductDetail/STMicroelectronics/MP34DT05TR-A （2026-10-23）【供应商声称（渠道标注）】
9. TDK InvenSense，ICS-41350 Datasheet DS-000047 Rev 1.1，https://www.mouser.com/pdfdocs/DS-000047-ICS-41350-v11-1112263.pdf ；产品页 https://product.tdk.com/en/search/sw_piezo/mic/mems-mic/info?part_no=ICS-41350 ；DigiKey https://www.digikey.com/en/products/detail/tdk-invensense/ICS-41350/6025659 （2026-10-23）【官方确认 + 渠道标注】
10. TDK InvenSense，ICS-43432 Datasheet Rev 1.0（I2S 输出，用于纠正任务书假设），https://www.mouser.com/datasheet/2/400/ICS-43432_DS-1113358.pdf （2026-10-23 下载并全文提取）【官方确认】
11. 敏芯 MEMSensing，硅麦克风产品家族页（PDM 系列参数表），https://www.memsensing.com/product/176.html （2026-10-23）【官方确认（家族页级）】
12. 敏芯 MEMSensing，MSM261S4030H0R Datasheet V1.0（2017-04，I2S 输出），https://files.waveshare.com/upload/0/01/MSM261S4030H0R.pdf （2026-10-23 下载并全文提取）【官方确认】
13. 立创商城/LCSC 渠道快照（价格与库存，搜索结果页转述）：MSM261D4030H1CPM（C966942）https://www.lcsc.com/product-detail/MEMS-Microphones_MEMS-MSM261D4030H1CPM_C966942.html 及同族 DGT003/DHT006/DHP006/DDB019 条目（2026-10-23）【供应商声称（数量档未定）】
14. SPH0641LU4H-1 价格库存：Mouser 产品页（$3.22@1/$2.54@10，~3.2k 库存）、DigiKey 比利时站（~€1.41@5.9k）（搜索快照，2026-10-23）【供应商声称】
15. SPK0641HT4H-1 价格库存：Mouser https://www.mouser.com/en/ProductDetail/Syntiant/SPK0641HT4H-1 ；DigiKey https://www.digikey.com/en/products/detail/syntiant/SPK0641HT4H-1/8573345 （2026-10-23）【供应商声称】
16. Espressif，ESP-IDF I2S 驱动（esp32s31 构建）：PDM RX dins[4]/LINE 槽位、DSR_8S（CLK=fs×64）/DSR_16S、PDM 时钟 1.024–6.144 MHz、MCLK 256/384×fs、tune_rate，https://docs.espressif.com/projects/esp-idf/en/latest/esp32s31/api-reference/peripherals/i2s.html （2026-10-23）【官方确认】
17. 敏芯代理商德诺，硅麦克风选型页，https://www.denovocn.com/silicon-microphone （2026-10-23，页面产品表动态加载未完整呈现）【供应商声称/待验证】
