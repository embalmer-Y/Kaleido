# Kaleido-Claw 软件架构设计

| 版本 | 日期 | 状态 | 作者 |
|---|---|---|---|
| v0.1 | 2026-10-24 | 初稿（评审后冻结为固件开发基线） | ZCode |

上游输入：负责人指示（2026-10-24）+ G1 调研（docs/research/01–16）+ Tessera 设计思想（/home/emb/project/Tessera，多 App 容器模型）+ vendor 钉版（docs/vendor-licenses.md）。
本文档遵守 constraints.md §6（固件规范）；与硬件的接口契约以 G1 汇总 §2 架构约定为准。

---

## 0. 定位与设计原则

Kaleido-Claw 是运行于 ESP32-S31 智能桌搭的**固件总架构**：以乐鑫 **ESP-Claw**（Apache-2.0，钉版 9b23c15）的 agent 框架为基座，融合 **Tessera** 的多 App 容器思想（简化鉴权），集成 **Meta Muse Gadget SDK**（Apache-2.0，钉版 c9354b7）使设备可接入 Muse 生态，UI 自研。

六条设计原则：

1. **P1 音频第一**（承 R2）：音频管线是系统一等公民，所有声音消费方经统一的音频焦点管理器仲裁。
2. **P2 App 即容器**（承 Tessera）：AI 生成的软件封装为标准 App（manifest+权限+生命周期），App 与板级硬件解耦，可安装/卸载/更新，**不做复杂鉴权**（无签名链，仅安装来源标记）。
3. **P3 上游零侵入**：对 esp-claw/muse 的扩展优先"包裹新组件"，不在 vendor 源文件原地修改（版权义务最小化，见 vendor-licenses R-1/义务 1）。
4. **P4 双 AI 后端可插拔**：Claw（本地 agent，chat coding/设备控制）与 Muse（云端伴侣，avatar/对话）并存，任一后端故障不影响系统。
5. **P5 UI 质量对标消费电子**：设计 token 单源生成、PPA/JPEG 硬件加速默认路径、动效优先（直击 esp-claw"UI 丑"短板）。
6. **P6 验证前移**：核心抽象（App 框架/音频焦点/配件协议）先在 Korvo-1 上跑通再上样机。

---

## 1. 总体分层

```text
┌────────────────────────────────────────────────────────────────┐
│  KShell（自研 UI 层：LVGL9 + PPA + JPEG 硬解）                     │
│  主屏 App 网格 · 状态栏 · 设置 · 动效引擎 · 设计 token 单源         │
├────────────────────────────────────────────────────────────────┤
│  KApp 框架（承 Tessera 思想，自研组件 kaleido_app）                 │
│  manifest 解析 · App 注册表 · 生命周期 · 权限位图(简化) ·          │
│  App 商店目录(NVS+FatFS) · 静态内置 App + Lua 动态 App            │
├───────────────────────────┬────────────────────────────────────┤
│  Claw 核心（fork 自 esp-claw，应用层装配）                         │
│  agent loop · LLM 接入 · MCP client/server · skill ·            │
│  结构化记忆 · IM 会话 · **cap_app_mgr（对接 KApp 框架）** ·        │
│  Lua 运行时(lua_modules 外设绑定，经 KApp 权限收窄)               │
├───────────────────────────┼────────────────────────────────────┤
│  Muse Link（包裹 muse-gadget-sdk esp32 端，kaleido_muse 组件）     │
│  chat session · avatar 渲染(视频流) · push-to-talk · 板级 hook    │
│  （可插拔：编译期开关，云端条款变化可整体移除，R-4）               │
├────────────────────────────────────────────────────────────────┤
│  Kaleido BSP（kaleido_bsp：板级抽象，Korvo-1 与样机双实现）        │
│  音频子系统(PDM→AFE→I2S→TAS5805M, 全数字) · 显示(CO5300 QSPI)     │
│  摄像头(DVP esp_video) · IMU/马达 · 电源/电池 · 存储(NAND/NVS)    │
├────────────────────────────────────────────────────────────────┤
│  扩展 HAL（kaleido_accessory：顶部+底部双 POGO 位）                │
│  配件识别(EEPROM ID) · 输入类/传感器类/执行器类协议 · 热插拔       │
├────────────────────────────────────────────────────────────────┤
│  ESP-IDF v6.1（锁定）· esp-sr v2.6.0 · FreeRTOS · esp_codec_dev  │
└────────────────────────────────────────────────────────────────┘
```

## 2. App 模型（Tessera 思想 → Kaleido 简化）

### 2.1 从 Tessera 吸收什么、简化什么

| Tessera 机制 | Kaleido-Claw 取舍 | 理由 |
|---|---|---|
| App = 可安装/可迁移单元，与板卡硬件解耦 | ✅ 保留（核心思想） | AI 生成软件的封装形态 |
| manifest + 权限位图（ts_perm_v1） | ✅ 保留，简化为 8 类粗粒度权限 | 掌上设备无需细粒度 |
| WAMR wasm 沙箱运行时 | ⏸ 二期再评估 | 一期用 esp-claw 原生 Lua（已验证可用，栈成熟）；wasm 带来的隔离性在"无鉴权"前提下收益降低 |
| TSAP v2 签名/验签（ed25519） | ❌ 移除 | 负责人明确"不需要太复杂鉴权"；安装包仅记来源标记 |
| slot 双槽 + 健康链 | ✅ 简化保留：安装槽+回滚计数器（crash 2 次自动禁用该 App） | 防坏 App 砖桌面 |
| 单活跃 App | ❌ 改为多 App 常驻+前台唯一 | 桌搭有前台/后台语义（音乐后台、avatar 前台） |
| AI 全链开发 Agent（PC 侧） | ✅ 由 Claw agent loop 原位承担（chat coding 即写 App） | esp-claw 基座能力 |

### 2.2 App 形态与 manifest

```jsonc
// manifest.json（示例：番茄钟 App）
{
  "id": "com.kaleido.pomodoro",      // 反域名，唯一
  "name": "番茄钟",
  "version": "1.0.0",                 // semver
  "api": 1,                           // 使用的 KApp API 版本
  "entry": "main.lua",                // Lua App；内置 App 为 C 注册
  "icon": "icon.png",                 // 128×128 ARGB，JPEG/PNG
  "permissions": ["ui", "audio_play", "net", "storage"],  // 见 2.3
  "category": "tool",
  "author": "Claw",                   // 安装来源：builtin / claw / usb / wifi
  "desc": "25 分钟专注计时"
}
```

### 2.3 权限位图（8 类，kperm）

`ui`（LVGL 画布/页面栈）· `audio_play`（播放队列）· `audio_rec`（录音，含唤醒接管）·
`net`（HTTP/WebSocket）· `storage`（App 沙箱目录）· `sensor`（IMU/摄像头读）·
`accessory`（扩展配件读写）· `system`（系统信息/设置写）。

- 权限在**安装时**随 manifest 声明并由用户在确认页一次授予（无复杂鉴权 ≠ 无确认）。
- Lua App 经 lua_modules 访问外设时，KApp 层按位图**裁剪可调用的模块表**（收窄 vendor-licenses R-6 的攻击面）。
- 越权调用返回错误码并记审计日志（NVS 环形缓冲，仅本机）。

### 2.4 生命周期

```text
INSTALLED → LOADED → RUNNING ⇄ PAUSED → STOPPED → (UNINSTALLED)
                │                            │
                └── 崩溃计数 ≥2 → DISABLED ──┘（自动回滚保护）
```

- 前台唯一：进入前台得 UI 画布；后台 App 保活但无 UI（音乐类可持 `audio_play`）。
- 内存压力时 appmgr 按 LRU 暂停后台 Lua VM（每 App 独立 lua_State，配额默认 64KB，可调）。

### 2.5 App 从哪来（AI 写软件的闭环）

1. **内置**：C 静态注册（时钟/设置/相机/Muse avatar 等 6–8 个出厂 App）；
2. **Claw 生成**：用户语音/文字提需求 → Claw agent loop（chat coding）生成 Lua+manifest →
   装配为 `.kapp` 目录 → 用户确认安装页 → 入商店列表。**这是"AI 写的软件封装成 App"的主链路**；
3. **USB/WiFi 侧载**：FatFS `/apps` 目录放 `.kapp`，设置页扫描安装（来源标记 usb/wifi，UI 徽标提示）。

## 3. 双 AI 后端与音频仲裁（P1/P4 的落点）

### 3.1 角色分工

| | Claw（本地 agent 基座） | Muse（云端伴侣） |
|---|---|---|
| 职责 | 设备控制、写 App、MCP 工具、离线命令兜底 | avatar 对话、视频流、云端智能 |
| 语音入口 | WakeNet 唤醒词 A（默认主入口） | push-to-talk 按住说话（复用唤醒词 B 可选） |
| 播报 | TTS（自选云 TTS，走播放队列） | MP3 流（muse minimp3 解码） |
| UI | KShell 页面/Toast | avatar 画布（App 形态呈现） |

### 3.2 音频焦点管理器（kaleido_audio 焦点策略）

单一所有者仲裁 I2S1 播放管线与 I2S0/PDM 采集路由：

- 焦点类型：`VOICE_CALL`（Muse 对话/push-to-talk，采集+播放同时持有，最高）＞ `TTS_ANNOUNCE`（Claw 播报，可打断 MUSIC）＞ `MUSIC`（App 音乐）＞ `AMBIENT`（氛围音，最先让路）。
- 唤醒事件（WakeNet）→ 意图路由：Muse 会话中 → 送 Muse；否则 → Claw。策略表用户可配。
- 回声问题：喇叭大音量播放时自麦采集回采（AEC 由 esp-sr AFE 承担，G2 在 Korvo-1 实测词误唤醒率，V-04 扩展项）。

## 4. Muse Link 设计（可插拔约束）

- muse SDK 以**组件**方式引入（`vendor/muse-gadget-sdk/esp32/components/*` → kaleido-fw COMPONENT 指向），不整仓集成；仅取 `muse`、`muse_vstream`、`minimp3`、`camera`（按需）。
- 板级适配：实现 `components/muse/boards/` 要求的 board hook（muse 已有 Mosaico 适配可参考——同 SoC 家族，改动集中在屏与音频路径）。
- **R-7 前置**：视频流依赖 tinyh264（疑似 LGPL）——集成 `muse_vstream` 前必须完成许可证核实（vendor-licenses §5），未闭环则编译期关闭视频、avatar 降级静态帧。
- 编译开关 `KALEIDO_ENABLE_MUSE`（默认开）：整体移除不影响 Claw/KApp/KShell。

## 5. KShell（UI 层）

- LVGL9 + esp_lvgl_port；渲染默认路径 PPA blit/scale + JPEG 硬解（承 constraints §6.7，禁止 CPU 软解作为默认）。
- **设计 token 单源**：`design/tokens.json`（色板/字阶/圆角/间距/动效曲线）→ 构建期生成 `kaleido_ui_tokens.h` + LVGL 主题（constraints §8.2 的落地）。
- 页面结构：状态栏（网络/电量/音量/配件）+ 主屏 App 网格（4×4，AMOLED 圆形/方形自适应）+ 前台 App 容器 + 控制中心（下拉）+ 设置。
- 动效：60fps 目标下先保 30fps 稳态（G1 §2 显示带宽 50MHz QSPI 上限约束），惯性滚动/淡入淡出为主，避免全屏重绘动效。

## 6. 扩展 HAL（kaleido_accessory）

承 G1 报告 10/16 协议，扩展为三类配件 + 双物理位：

| 类 | 顶部 PO 位（传感器/摄像头） | 底部 DOCK 位（执行器底座） |
|---|---|---|
| input | 迷你按键/摇杆（16B 报表，500Hz） | 底座按钮 |
| sensor | 温湿度/环境/摄像头配件 | 云台位姿回读 |
| **actuator** | — | 舵机/云台（目标角度寄存器，配件侧 MCU 执行 PWM；**底座自带 USB-C 供电口**，舵机大电流不取自主机 5V——电源约束已登记 ECR 候选） |

- I2C 0x20–0x27 段分配 + AT24C02 ID（0x50）；协议寄存器映射 v1 以报告 16 §5 草案为基线，actuator 类扩展寄存器（目标角/速度/到位状态/堵转告警）G2 定稿。
- 热插拔：IRQ+轮询兜底；识别新配件 → Toast + 自动装载对应内置驱动 App。

## 7. 仓库与目录（kaleido-fw，承 constraints §6.2）

```text
kaleido-fw/
  components/
    kaleido_app/        # KApp 框架（manifest/注册表/生命周期/权限）
    kaleido_audio/      # 音频管线 + 焦点管理器
    kaleido_ui/         # KShell + token 生成物
    kaleido_bsp/        # 板级抽象（board.h 双实现：korvo1 / kaleido_v1）
    kaleido_muse/       # Muse Link 包裹层（KALEIDO_ENABLE_MUSE）
    kaleido_accessory/  # 扩展 HAL
    kaleido_claw/       # esp-claw 装配层（应用侧 fork 胶水，上游零侵入）
  main/                 # 系统装配：启动顺序、组件接线
  design/tokens.json    # UI token 单源
  tools/hil/            # 硬件在环测试（constraints §6.5）
  test/                 # Unity 单测（App 框架/焦点策略/协议解析先行）
  vendor/ → ../vendor   # 上游钉版（符号链接或 COMPONENT 路径引用）
```

## 8. 启动顺序（main 装配）

NVS → 板级early init（电源/时钟）→ 显示 KShell 启动画面 → 音频管线起栈（WakeNet 常驻）
→ KApp 框架（内置 App 注册→商店扫描）→ 配件枚举 → Claw 起（agent loop 后台）→ Muse Link 起（可配延迟，等网络）→ 就绪动效。

## 9. 钉版与里程碑

**钉版表**（初始，constraints §6.1）：IDF v6.1（release 锁定待首个 CI 构建确认 commit）、esp-sr v2.6.0、
esp-claw `9b23c15`、muse-sdk `c9354b7`、LVGL 9.x（随 esp_lvgl_port 2.9.0）、esp_codec_dev v2.0.x。

| 里程碑 | 内容 | 验收 |
|---|---|---|
| M0 骨架 | 仓库结构 + KApp 框架核心代码 + 本文档冻结 | 代码评审过（本文档 §2 全字段实现） |
| M1 Korvo 验证 | IDF v6.1 构建 + KApp 跑 3 内置 App + esp-sr 唤醒 + Claw 最小 agent（Lua 一条链）+ muse 编译开关过 | V-02/04/22 相关项闭环 |
| M2 样机 | BSP 换 kaleido_v1（QSPI 屏/TAS5805M/PDM 双麦/双 POGO）+ Muse avatar + 首批 Claw 生成 App | constraints §6.5 四项 HIL |

## 10. 风险登记

| # | 风险 | 缓解 |
|---|---|---|
| A-1 | esp-claw/muse 对 S31 target 未验证（G1 已知 preview 态） | M1 在 Korvo-1 先行；BSP 隔离芯片差异 |
| A-2 | Lua+LVGL 内存压力（16MB PSRAM 下多 App 常驻） | M1 内存审计（heap 高水位+PSRAM 账本，承 Tessera MP 思想）；配额可调 |
| A-3 | tinyh264 LGPL 疑点 | R-7 门禁：未核实不开视频流 |
| A-4 | 双后端音频竞争 | §3.2 焦点管理器 + HIL 用例（对话中放音乐等 6 场景） |
| A-5 | esp-claw 上游演进 breaking | 钉版+升级独立提交回归（vendor-licenses R-5） |
| A-6 | 唤醒词双后端路由体验 | 策略表用户可配；V-04 扩展实测误唤醒率 |
