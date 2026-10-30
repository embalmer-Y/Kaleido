# kaleido-fw

Kaleido 智能桌搭固件（**Kaleido-Claw** 架构）。架构设计：`../docs/design/kaleido-claw-架构.md`。
开发规范：`../constraints.md` §6。

## 状态

> **平台已切换（D-8，2026-10-25）**：主开发板为 **osptek P4C5**（ESP32-P4 +
> ESP32-C5，800×1280 JD9366 MIPI-DSI 屏），本仓主固件 target 已改
> `esp32p4`（见 `sdkconfig.defaults`）。早期 ESP32-S31-Korvo-1 路线的
> 装机记录与板级事实**仅作存档**（板已故，W-7）；平台切换实测见
> `../docs/gates/V-38-P4C5点板与平台切换实测.md`。

进行中：语音全链 HIL（`tools/hil/voice-loop-p4`，V-40/V-41：WakeNet 单麦
唤醒 + MiniMax TTS/LLM 已验）；显示舞台 HIL（`tools/hil/dsi-lvgl`，
V-36/V-39：JPEG 硬解 + lottie 舞台定稿）。门禁记录在 `docs/gates/`。

## 开发环境事实源（2026-10-24 装机记录，2026-10-26 修订）

- ESP-IDF：`~/esp/esp-idf`，**v6.1 @ `fff9895c` + 本地提交 c55d13c6**
 （XIP PSRAM+BSS/NOINIT 链接修复，esp-idf#18791 backport）
- 工具链：`~/.espressif`（riscv32-esp-elf esp-15.2.0_20251204）
- **esp32s31 为 preview target**（历史路线需 `--preview`）；**esp32p4 为正式
  target，无需 `--preview`**
- **网络**：git/pip/组件下载需代理 `http://127.0.0.1:7890`（github.com 直连被墙；components.espressif.com 可直连）
- **IDF v6 破坏性变更**：核心已无 `json` 组件——cJSON 改用组件仓库 `espressif/cjson`（组件内名 `espressif__cjson`）
- 构建命令：
  ```bash
  bash -c '. ~/esp/esp-idf/export.sh && cd /home/emb/project/Kaleido/kaleido-fw && idf.py build'
  # HIL 子工程（各自独立 build 目录）：
  #   cd tools/hil/voice-loop-p4 && idf.py build   # 需 sdkconfig.local（密钥）
  #   cd tools/hil/dsi-lvgl && idf.py build
  ```

## 钉版（升级走独立提交 + 回归）

| 依赖 | 版本/commit | 说明 |
|---|---|---|
| ESP-IDF | v6.1 @ `fff9895c` **+ backport `c566ace1f`**（本地提交 c55d13c6：XIP PSRAM+BSS/NOINIT 链接修复，esp-idf#18791；release/v6.1 未回合） | esp32p4 / esp32s31 |
| esp-claw | `9b23c15` + PR#157 补丁（3177144，W-3）**+ 3ebb75e（W-4：默认启用 TLS1.3）** | ../vendor/esp-claw，Apache-2.0 |
| espressif/cjson | ^1.7（lock: 1.7.19~2） | IDF v6 起替代核心 json 组件 |
| esp-sr | **2.6.0**（V-40 已上板验证：wn9_nihaoxiaozhi_tts 单麦唤醒 10/10） | model 分区 0xF20000/896KB |
| esp_codec_dev | **1.6.2**（各 HIL `idf_component.yml` 实钉；README 曾写 v2.0.x 为笔误） | ES8311 采放（voice-loop-p4） |
| lvgl / esp_lvgl_port | 9.4.0 / 2.7.x（dsi-lvgl HIL 实钉） | direct_mode+avoid_tearing 口径见 V-36 |

## 组件地图（2026-10-26 更新）

| 组件 | 职责 | 状态 |
|---|---|---|
| kaleido_app | KApp 框架：manifest/注册表/生命周期/权限位图 | **核心代码已落** |
| kaleido_speech | ASR/TTS/LLM provider 层（DashScope WS + MiniMax SSE） | **已落并在 HIL 验证**（V-33/V-41） |
| kaleido_emotion / kaleido_ignition | 情感模型 / 主动交互点火 | 核心代码已落（单测绿） |
| kaleido_memory / kaleido_persona | SD 长期记忆 / 角色人设包 | 核心代码已落（单测绿） |
| kaleido_bsp | 板级抽象（korvo1 存档 / P4C5 现行） | korvo1 已故（W-7），P4C5 引脚表见 V-38 |
| kaleido_audio | 音频管线 + 焦点管理器（全数字链路） | 占位（M2 接 TAS5805M/MA12040P） |
| kaleido_ui | KShell（LVGL9+PPA+JPEG）+ token 生成物 | 占位，tokens.json 已立单源 |
| kaleido_accessory | 扩展 HAL（顶部传感器位 + 底部执行器底座） | 占位（协议 v1 基线=报告16） |
| kaleido_claw | esp-claw 装配层（上游零侵入胶水） | 占位（M1） |

## 构建路线

```bash
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/ttyACM0 flash monitor   # 开发板：osptek P4C5（no-stub 烧写口径见 V-38）
```

单测（App 框架/焦点策略/协议解析，不依赖硬件）：
```bash
idf.py create-component-from ...  # M1 提供 CTest/Unity 接线
```
