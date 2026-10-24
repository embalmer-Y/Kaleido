# 09-IMU与振动马达调研

| 版本 | 日期 | 备注 |
|---|---|---|
| v1.0 | 2026-10-23 | G1 调研初稿（ZCode） |

## 结论先行

1. **IMU 主选 Bosch BMI270**（置信度高，官方 datasheet 实测下载核验 + 乐鑫生态双源）：ESP-Mosaico 同款，
   立创现货 ¥8.49（C2836813），有乐鑫官方 ESP-IDF 组件 `espressif/bmi270` 与 Bosch 官方 SensorAPI；
   内置手腕佩戴唤醒/甩动等手势引擎（硬件中断，MCU 休眠时可用），加速度计低功耗模式最低至 4 µA【官方确认】。
2. **IMU 备选 1：QST QMI8658A**（国产，¥6.13；QMI8658C ¥4.73）——成本敏感时的替代，Waveshare 有 ESP-IDF 组件；
   代价：无 BMI270 级手势硬件引擎，加速度计低功耗电流约 30–55 µA（高于 BMI270 的最低 4 µA 档）。
3. **IMU 备选 2：ST LSM6DSO**（¥7.71，9 KB FIFO、MLC 机器学习核）——供货与生态第二备胎。
   ICM-42670-P 性能最好但 DigiKey 缺货 45 周交期，供货风险高，不推荐主选。
4. **振动马达推荐（手感优先，建议采纳）：X 轴 LRA + 艾为 AW86224 驱动**，合计约 ¥9–11（AW86224 ¥3.28 + X 轴 LRA 约 ¥6–8），
   可输出"按键清脆点击"级触觉，配 1 MHz I2C、F0 自动追踪、内置波形 RAM【官方确认】。
5. **保底方案（成本优先）：ERM 扁平马达 + GPIO/低边 MOS 直驱**（约 ¥2.1，ESP-Mosaico 即此方案，GPIO8 + 低边 MOS）；
   启停慢（数十毫秒级）、手感糊，但零驱动 IC 成本、软件最简单。
6. 建议 v1 板卡**双方案兼容设计**（ERM 焊盘与 AW86224+LRA 焊盘同板预留），G2 成本校准后二选一投件；
   按 R2 优先级（音频>显示>交互），触觉预算上限建议 ≤ ¥11。
7. 驱动软件均有现成 ESP-IDF 路径：`espressif/bmi270`（基础）/Bosch SensorAPI（全功能）；
   AW86224 与 DRV2605L 均被 LilyGO 主程的 SensorLib 支持（Arduino + ESP-IDF 双框架）。

## 对比表

### 表 1：6 轴 IMU 候选对比

| 维度 | BMI270（主选） | QMI8658A（备选1） | LSM6DSO（备选2） | LSM6DSV16X | ICM-42670-P |
|---|---|---|---|---|---|
| 厂商 | Bosch | QST 矽睿（国产） | ST | ST | TDK InvenSense |
| 供电 | 1.71–3.6 V（VDD） | 1.71–3.6 V | 1.71–3.6 V | 1.71–3.6 V | 1.71–3.6 V |
| 6 轴功耗 | 685 µA（Normal）/970 µA（Performance）【官方确认】 | 751–1004 µA（6DOF 模式）【官方确认】 | 0.55 mA（combo 高性能）/0.265 mA（combo 低功耗）【官方确认】 | ≈0.65 mA（combo LP，官方数据手册）【官方确认】 | 0.55 mA（Low-Noise 6 轴）【官方确认】 |
| 加速度计低功耗 | 低至 4 µA【官方确认】 | 约 30–55 µA（LP 模式各 ODR）【官方确认】 | 远低于 combo（超低功耗档，datasheet 分表） | 低至 µA 级（搜索摘要 ~1.6–2.6 µA）【待验证】 | 支持 LP 模式（数值随 ODR，未抓取）【待验证】 |
| 挂起/掉电电流 | 3.5 µA（suspend）【官方确认】 | 6 µA（power-down）/8 µA（LP 时钟）【官方确认】 | datasheet 未抓取【待验证】 | —【待验证】 | —【待验证】 |
| 加速度计噪声（typ） | 0.38 mg RMS @12.5 Hz，1.06 mg @100 Hz（±8 g，normal 滤波）【官方确认】 | 低噪声（datasheet 参数表未逐项抓取）【待验证】 | datasheet 未逐项抓取【待验证】 | —【待验证】 | 业界领先（宣传语）【供应商声称】 |
| FIFO | 2 KB（2048 字节）【官方确认】 | 有（容量未抓取）【待验证】 | Smart FIFO 最高 9 KB【官方确认】 | —【待验证】 | 2.25 KB【官方确认】 |
| 片上智能引擎 | Any/No/Significant motion、Wrist wear wakeup、Flick/Pivot up/Push arm down、计步、活动识别【官方确认】 | 运动事件检测、时间戳；QMI8658C 内置 9 轴融合【供应商声称】 | MLC 机器学习核【官方确认】 | MLC + ISPU + SFLP 低功耗融合【官方确认】 | APEX 手势（wake-on-motion）【官方确认】 |
| 接口 | I2C Fm+（≤1 MHz）/SPI 10 MHz【官方确认】 | I2C/SPI/I3C【官方确认】 | I2C/SPI（datasheet） | I2C/SPI | I2C/SPI（datasheet） |
| 封装 | LGA-14 2.5×3 mm | LGA-14 2.5×3.0×0.86 mm | LGA-14 2.5×3 | LGA-14 2.5×3 | LGA-14 |
| 立创价格（参考单价，非 1k 阶梯） | ¥8.49，现货 | ¥6.13（QMI8658A）/¥4.73（QMI8658C），现货 | ¥7.71（LSM6DSDTR） | ¥14.1（LSM6DSV16XTR） | 立创在售但价格未抓取【待验证】；Mouser $3.42–3.83 |
| 西方渠道库存 | DigiKey 约 4.9 万只（ICM 替代推荐页快照） | 以国内渠道为主 | 常规在售 | 常规在售 | Mouser 仅 220 只；DigiKey 缺货、交期约 45 周【官方渠道】 |
| ESP-IDF 驱动 | `espressif/bmi270` v1.1.0（乐鑫官方）+ Bosch SensorAPI（全功能）+ `espp/bmi270` v1.3.7 | `waveshare/qmi8658` v2.0.2（ESP 组件库，下载 1.45 万次） | `espp/lsm6dso` v1.3.8（I2C/SPI/FIFO/中断/片上特性） | 无专用组件（ST 通用 C 库可移植）【待验证】 | 无 ESP 组件，TDK eMD 库可移植【待验证】 |
| 备注 | Mosaico 同款（I2C 0x69、INT→GPIO2）；初始化需上传 8 KB 配置文件（SensorAPI 已封装） | 国产高性价比，双地址（0x6A/0x6B 可选） | FIFO 最大，MLC 可跑姿态分类 | ISPU 价格偏高 | 性能强但供货风险大 |

### 表 2：振动马达与驱动方案对比

| 维度 | ERM 扁平马达 + MOS 直驱 | X 轴 LRA + AW86224 | LRA/ERM + DRV2605L |
|---|---|---|---|
| 手感 | 差：启停 30–100 ms 级，"嗡"感 | 好：启停可低至 ms 级（RichTap 称 X 轴低至 1 ms【供应商声称】），可播复杂波形 | 好：闭环过驱/制动 + 116 效果库（Immersion 授权） |
| 驱动复杂度 | 最低：GPIO + 低边 MOS（Mosaico 同方案） | 低：I2C（1 MHz）发寄存器/波形 RAM；F0 自动追踪 | 低：I2C 触发效果库；需按马达类型校准 |
| 功耗效率 | 低（直流电机持续旋转） | 高（谐振点驱动） | 高（闭环） |
| 1k 档成本估算 | 马达 ¥2.02（立得 1030，立创）+ MOS ≈ ¥0.1 ≈ **¥2.1** | IC ¥3.28 + LRA ¥6–8 ≈ **¥9–11** | IC ¥4.63 + LRA ≈ **¥7–9** |
| 立创现货 | 马达现货；MOS 任意通用件 | IC 现货（AW86224AFCR）；X 轴 LRA 立创未见现货，走 1688【待验证】 | IC 现货（DRV2605LDGSR，VSSOP-10） |
| 占位 | 马达 Ø10×3 mm（或 Ø8×3.4），+SOT-23 MOS | IC FCQFN 1.4×1.4 mm + 马达 ≈ 10×10×4 mm 级【待验证】 | IC VSSOP-10 3×3 mm + 马达 |
| ESP-IDF 软件 | 自写 PWM/GPIO，工作量极小 | SensorLib 含 AW86224 驱动（ESP-IDF 可用）【社区经验】 | SensorLib 含 DRV2605 驱动【社区经验】；Adafruit/Boardoza 参考设计多 |
| 风险 | 手感不达"旗舰"定位 | 艾为规格书为保密版（LILYGO 托管副本），需原厂支持 | TI 5.2 V 上限（锂电池满电 4.2 V OK）；封装偏大 |

## 详细分析

### 1. IMU 候选逐项分析

**BMI270（主选）**。以下数字全部取自官方 datasheet BST-BMI270-DS000-08（已下载核验，2026-10-23）：

- 功耗（Table 6，p.26）：suspend 3.5 µA；加速度计低功耗模式"down to 4 µA"（随 ODR/滤波配置）；
  IMU Normal 685 µA；A+G Performance 970 µA；陀螺单独 Normal 600 µA。首行卖点"低功耗 685 µA（全 ODR 无混叠工作）"。
- 噪声：加速度计 RMS 噪声（±8 g、normal 滤波）0.38 mg@12.5 Hz → 1.06 mg@100 Hz（Table 8）；
  陀螺 Performance 模式噪声 typ < 7 mdps/√Hz。对掌机姿态交互绰绰有余。
- FIFO 2048 字节，所有功耗模式均可写入 FIFO（低功耗下可用水印中断批量读，摊薄平均电流）。
- 接口：I2C Fm+ 至 1 MHz；SPI 10 MHz；另有 I2C 主接口可外挂磁力计。
- 片上引擎：Significant motion / Any-motion / No-motion / Wrist wear wakeup / 计步 / 活动识别 /
  Push arm down / Pivot up / Wrist jiggle / Flick（Table 14 为手腕唤醒手势的正例场景）——即"抬手亮屏"
  "翻转静音"所需的原生硬件支持。
- 代价：POR 后需上传 8 KB 配置文件（datasheet §4.5；Bosch SensorAPI 已封装，约 6.6 ms @10 MHz SPI）。
- 生态：乐鑫组件库 `espressif/bmi270`（官方，v1.1.0，1.16 万次下载；注：该组件仅实现基础 I2C 读写，
  FIFO/中断/手势需用 Bosch BMI270-Sensor-API 或 espp 组件）；ESP-Mosaico 即采用 BMI270（I2C 地址 0x69、
  中断接 GPIO2），固件可直接对标移植。

**QMI8658A（备选 1）**。数字取自 Waveshare 托管 QMI8658A Datasheet Rev A（2022-06-20，已下载核验）：

- 功耗（Table 15–17，VDD=VDDIO=1.8 V）：加速度计 LP 模式约 30–55 µA（低 ODR 档）；加速度计 High-Resolution
  132–182 µA；陀螺单独 651–908 µA；6DOF 751–1004 µA；power-down 6 µA、LP 时钟 8 µA、上电默认 15 µA。
- 供电 1.71–3.6 V，I2C/SPI/I3C，LGA-14 2.5×3.0×0.86 mm；±16 g / ±2048 dps。
- 官网称 QMI8658C 内置 9 轴融合【供应商声称】；立创 QMI8658C ¥4.73 比 QMI8658A ¥6.13 更低。
- ESP 组件 `waveshare/qmi8658`（v2.0.2，1.45 万次下载）覆盖初始化/量程/ODR/数据读取。
- 定位：成本压力大时替换 BMI270，省约 ¥2–3.7；但手势需自写算法，且 LP 加速度计电流高一个量级，
  常开抬手检测的待机功耗劣化（见 §2 预算表）。

**LSM6DSO（备选 2）**：官方 datasheet（2018-08）确认 combo 高性能 0.55 mA / combo 低功耗 0.265 mA、
Smart FIFO 最高 9 KB、MLC 机器学习核；立创 LSM6DSDTR ¥7.71。ESP 生态靠 `espp/lsm6dso`（第三方）。
作为 BMI270 断供时的第二备胎，MLC 亦可承担翻转/姿态分类。

**LSM6DSV16X / ICM-42670-P（不主选）**：LSM6DSV16X（ISPU+MLC+SFLP）功能最强但 ¥14.1 贵 67%，
掌机用不到 ISPU 级算力；ICM-42670-P（6 轴 Low-Noise 0.55 mA、2.25 KB FIFO、1.71–3.6 V）性能优秀，
但 Mouser 仅 220 只库存、DigiKey 缺货且交期约 45 周（快照 2026-10-23），1 k 台量产供货风险不可接受。

### 2. 掌机 IMU 用途评估与功耗预算

| 用途 | 是否值得做 | 配置 | 采样率 | IMU 电流预算（BMI270） | 说明 |
|---|---|---|---|---|---|
| 抬手亮屏 | 值得（掌机拿起的瞬间响应是第一体验） | accel LP + Any-motion/Wrist-gesture 硬件中断，MCU 深睡 | 25–50 Hz 常开 | 平均 ≈ 5–20 µA（LP 模式 + 中断唤醒批量读 FIFO）【官方确认区间，实测定】 | Mosaico/手表标配交互 |
| 翻转静音 | 值得（与抬手共用数据流，边际成本≈0） | accel LP，MCU 或 MLC 判姿态翻转 | 与抬手共用 25–50 Hz | ≈ 0（复用） | 判定阈值需实测调参 |
| 横竖屏/甩动切歌等姿态交互 | 值得（差异化卖点） | Flick/Pivot up 引擎或固件算法 | 50–100 Hz 按需 | 常开 ≈ 20–100 µA 级；建议事件驱动按需开 | BMI270 手势引擎原生支持 |
| 游戏体感（陀螺仪瞄准/体感操作） | 值得（掌机差异化） | 6 轴 Normal/Performance | 200 Hz–1.6 kHz | 685–970 µA 仅游戏时开启 | 一次性活动，对续航影响可忽略 |
| 计步/活动统计 | 可选（低优先级） | 片上计步器 | 25 Hz 常开 | 计入 LP 常开预算 | 非核心卖点 |

结论：以"抬手亮屏 + 翻转静音 + 手势"常开、体感按需的负载模型，IMU 常开预算按 **≤30 µA 平均**
写入 `docs/power-budget.md`（BMI270 路线）；QMI8658A 路线该数字约 30–55 µA，需在休眠预算中权衡。

### 3. 振动马达：ERM vs LRA 与驱动

**马达本体**：

- ERM（偏心转子）：立创在售 LEADER 立得 LCM1030A3097F（Ø10×3 mm、3 V、80 mA max、13500 rpm、
  1.35 G min）¥2.02；LCM0834A1195F（Ø8×3.4 mm、0.85 G）为同系小尺寸。启停由转子惯性决定，
  典型全振幅建立需数十毫秒，且停止有惯性余振——只能做"提醒级"振动。
- LRA（线性谐振）：共振驱动，启停可达毫秒级（RichTap 宣称 X 轴低至 1 ms【供应商声称】），可播放
  点击/纹理等丰富波形，是"旗舰手感"的必要条件。缺点：只在高 Q 共振点（典型 150–235 Hz）有效，
  偏离 2–3 Hz 振幅即骤降（TI 应用笔记 zhca603），必须用带 F0 追踪的驱动 IC。
  立创暂未搜到 X 轴 LRA 现货；1688 手机马达批发 9595 方形 X 轴约 ¥7.70/个、手表长条 X 轴约 ¥6/个
  （均为小批量参考价）【供应商声称】；工业品牌渠道：Vybronics（扁平 LRA 列表）、Nidec、AAC RichTap。

**驱动 IC**：

- **AW86224A/B（艾为，推荐）**：官方 datasheet（LILYGO 托管副本，已下载核验）：单芯片 H 桥 LRA 驱动，
  基于 BEMF 的 F0 检测与追踪、3 KB SRAM 用户波形（12k/24k/48k 采样率，可上采样至 48k）、实时+存储播放、
  自动刹车（autobrake）、LRA 电阻诊断、供电 3–5.5 V、1 MHz I2C。立创 AW86224AFCR ¥3.28（FCQFN-9L
  1.4×1.4 mm）。LILYGO 已在其产品中使用（datasheet 由其 wiki 托管即旁证），SensorLib 有驱动。
- **DRV2605L（TI，替代）**：官方 datasheet（已下载核验）：2–5.2 V 供电、Immersion 授权效果库
  （6 个 ERM 库 + 1 个 LRA 库）、自动过驱/制动、LRA 自动共振追踪、差分输出、PWM 输入、I2C 控制接口。
  立创 DRV2605LDGSR ¥4.63（VSSOP-10）。生态最成熟（Adafruit/Boardoza 开源硬件参考），
  但比 AW86224 贵约 ¥1.3、封装大，且无波形 RAM（效果库固定）。
- **ERM + 低边 MOS 直驱（Mosaico 方案）**：ESP-Mosaico 官方文档确认其振动马达由 GPIO8 经低边 MOS 驱动
  （高有效）——零 IC 成本，PWM 调强度，软件 10 行代码；代价是手感"只能提醒、不能点击"。

**选型建议**：Kaleido 对标"顶配版 Mosaico"，触觉是游戏/交互体验的显性卖点 → **手感优先，主选
AW86224 + X 轴 LRA（约 ¥9–11）**；原理图按"ERM 直驱 + AW86224/LRA"双方案兼容布局，G2 成本校准若
超支则以 ERM 直驱保底（省约 ¥8）。DRV2605L 记录为 AW86224 的功能替代料（供货硬规则）。

## 风险与待验证项

| # | 风险/待验证 | 等级 | 验证手段 |
|---|---|---|---|
| 1 | BMI270 立创 ¥8.49 为单件参考价，1k 阶梯价未抓取 | 中 | 立创批量询价 + 贸泽/DigiKey 1k 档比价（页面动态渲染，需人工截图归档） |
| 2 | QMI8658C 与 A 的差异（内置融合、性能）仅官网一句话 | 低 | 向矽睿索取 QMI8658C 规格书比对；若引入 C 版需回归验证 I2C 时序 |
| 3 | AW86224 正式规格书为保密件，现用 LILYGO 托管 Rev 版本 | 中 | 向艾为原厂申请最新 datasheet + 评估板（AW86224EVB）；确认 SRAM 波形格式与工具链 |
| 4 | X 轴 LRA 立创无现货，1688 货源一致性/F0 公差未知 | 高 | 1688 索样 3 家（各 20 只），用 AW86224 F0 检测读回 F0 分布；记录 F0 温漂 |
| 5 | LRA/磁铁与扬声器的磁耦合（失真/吸力） | 高 | 样机实测：高斯计测绘 20–50 mm 场强 + 播放 1 kHz 正弦对比 THD（有无磁铁两工况） |
| 6 | BMI270 抬手亮屏真实平均电流（表 6 为"down to"理想值） | 中 | 评估板（小米手环同款生态）实测：accel LP 25/50 Hz + FIFO 水印中断，纳安表记录 |
| 7 | ICM-42670-P/LSM6DSV16X 立创 1k 价格 | 低 | 立创询价（仅备胎，不阻塞） |
| 8 | `espressif/bmi270` 组件不含 FIFO/中断/手势 | 低 | G2 前跑通 Bosch BMI270-Sensor-API 于 ESP-IDF v6（SPI 或 I2C），产出 kaleido 组件封装 |
| 9 | SensorLib（AW86224/DRV2605 驱动）在 IDF v6 的可编译性 | 低 | IDF v6 下编译 SensorLib demo 验证；不行则按 datasheet 寄存器表自写（≤300 行） |

## 来源清单（访问日期均为 2026-10-23）

1. Bosch Sensortec，BMI270 Datasheet **BST-BMI270-DS000-08**（150 页，已下载核验；功耗 Table 6 p.26、噪声 Table 8、FIFO §4.7、特性列表、§4.5 初始化）
   https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bmi270-ds000.pdf
2. QST 矽睿，QMI8658A Datasheet Rev A（2022-06-20，Waveshare 托管，已下载核验；Table 15–17 电流、睡眠模式章节）
   https://files.waveshare.com/upload/5/5f/QMI8658A_Datasheet_Rev_A.pdf
3. QST 官网 QMI8658 产品页（封装/接口/量程、QMI8658C 内置 9 轴融合） https://www.qstcorp.com/imu_prod/QMI8658
4. TDK InvenSense，ICM-42670-P Datasheet **DS-000451 v1.0**（0.55 mA 6 轴 Low-Noise、2.25 KB FIFO、1.71–3.6 V）
   https://cdn-shop.adafruit.com/product-files/5787/DS-000451-ICM-42670-P-v1.0.pdf
5. TDK InvenSense ICM-42670-P 产品页 https://www.invensense.tdk.com/en-us/products/6-axis/icm-42670-p
6. ST，LSM6DSO Datasheet（162 页；combo 高性能 0.55 mA、combo 低功耗 0.265 mA、Smart FIFO 9 KB、MLC）
   https://www.st.com/resource/en/datasheet/lsm6dso.pdf
7. ST，LSM6DSV16X Datasheet（198 页；ISPU/MLC/SFLP） https://www.st.com/resource/en/datasheet/lsm6dsv16x.pdf
8. Future Electronics LSM6DSV16X 专题页（combo LP ≈0.65 mA 交叉源） https://www.futureelectronics.com/resources/featured-products/stmicroelectronics-lsm6dsv16x-inertial-measurement-unit-imu
9. 立创商城 BMI270（C2836813，LGA-14 2.5×3，参考价 ¥8.49，编带） https://item.szlcsc.com/3005172.html
10. 立创商城 QMI8658A（参考价 ¥6.13） https://item.szlcsc.com/3544058.html ；QMI8658C（参考价 ¥4.73） https://item.szlcsc.com/3026402.html
11. 立创商城 LSM6DSV16XTR（参考价 ¥14.1） https://item.szlcsc.com/5993815.html ；LSM6DSVTR（¥31.28） https://item.szlcsc.com/5993808.html
12. Mouser ICM-42670-P（库存 220、$3.42–3.83） https://www.mouser.com/en/ProductDetail/TDK-InvenSense/ICM-42670-P ；DigiKey（缺货、~45 周交期、BMI270 替代推荐库存 49,135） https://www.digikey.com/en/products/detail/tdk-invensense/ICM-42670-P/14319524
13. Mouser BMI270 产品页 https://www.mouser.com/en/ProductDetail/Bosch-Sensortec/BMI270
14. ESP 组件库 `espressif/bmi270` v1.1.0 https://components.espressif.com/components/espressif/bmi270
15. ESP 组件库 `espp/lsm6dso` v1.3.8 https://components.espressif.com/components/espp/lsm6dso ；`espp/bmi270` v1.3.7 https://components.espressif.com/components/espp/bmi270/versions/1.3.7/readme
16. ESP 组件库 `waveshare/qmi8658` v2.0.2 https://components.espressif.com/components/waveshare/qmi8658
17. Bosch BMI270-Sensor-API（配置文件与参考驱动） https://github.com/BoschSensortec/BMI270-Sensor-API
18. Espressif，ESP-Mosaico V1.2 User Guide（BMI270 I2C 0x69/INT GPIO2；振动马达 GPIO8 + 低边 MOS；扩展槽/EEPROM）
    https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s31/esp-mosaico/user_guide.html
19. TI，DRV2605L Datasheet（已下载核验；2–5.2 V、6 ERM 库+1 LRA 库、自动过驱/制动、共振追踪、I2C）
    https://www.ti.com/lit/ds/symlink/drv2605l.pdf
20. 立创商城 DRV2605LDGSR（参考价 ¥4.63，VSSOP-10） https://item.szlcsc.com/545760.html
21. 艾为 AW86224A/B Datasheet（LILYGO wiki 托管，已下载核验；3 KB SRAM、F0 追踪、3–5.5 V、1 MHz I2C）
    https://wiki.lilygo.cc/datasheet/AW86224.pdf ；艾为产品页 https://www.awinic.com/en/productDetail/AW86224AFCR
22. 立创商城 AW86224AFCR（参考价 ¥3.28，FCQFN-9L 1.4×1.4 mm） https://item.szlcsc.com/5811420.html
23. 立创商城 ERM 扁平马达 LCM1030A3097F（¥2.02，Ø10×3、3 V、80 mA、1.35 G） https://item.szlcsc.com/3017171.html ；LCM0834A1195F（Ø8×3.4、0.85 G） https://item.szlcsc.com/2880061.html
24. 1688 手机马达批发列表（9595 方形 X 轴线性马达约 ¥7.70/个等） https://s.1688.com/kq/-CAD6BBFAC2EDB4EF.html
25. SensorLib（lewisxhe/LilyGO，含 DRV2605+AW86224 驱动，Arduino/ESP-IDF 双框架） https://github.com/lewisxhe/SensorLib
26. Vybronics 扁平 LRA 产品列表（工业品牌渠道参考） https://vybronics.hk/products/coin-vibration-motors/lra
27. RichTap（AAC）线性马达与 X 轴启停 1 ms 宣称 https://www.richtap-haptics.com/product/component
28. TI 应用笔记《触觉能耗》(zhca603)：LRA 共振漂移 ±2–3 Hz 即失效 https://www.ti.com.cn/cn/lit/pdf/zhca603
29. Adafruit DRV2605L Breakout 学习页（开源参考设计/库） https://learn.adafruit.com/adafruit-drv2605-haptic-controller-breakout?view=all
30. Boardoza DRV2605L 开源硬件 https://github.com/Boardoza/Boardoza_DRV2605L_Haptic_Driver_Breakout_Board

> 事实分级说明：标注【官方确认】的参数均直接核验自官方 datasheet/文档；立创价格为页面动态数据，
> 标注为"参考单价"，1k 阶梯价列入待验证项 #1。
