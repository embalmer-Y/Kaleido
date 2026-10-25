# kaleido-fw

Kaleido 智能桌搭固件（**Kaleido-Claw** 架构）。架构设计：`../docs/design/kaleido-claw-架构.md`。
开发规范：`../constraints.md` §6。

## 状态

**M0 骨架已完成并通过 esp32s31 构建**（2026-10-24：`kaleido-fw.bin` 190KB，app 分区余 82%）。
开发环境事实源见下节；板子（Korvo-1 V1.1）接入后进入 M1。

## 开发环境事实源（2026-10-24 装机记录）

- ESP-IDF：`~/esp/esp-idf`，**v6.1 @ `fff9895c82d744c7237be8847347bdd1b07c6643`**（浅克隆，全子模块）
- 工具链：`~/.espressif`（riscv32-esp-elf esp-15.2.0_20251204）；Python 3.14.4 官方 venv（`idf6.1_py3.14_env`）验证可用
- **esp32s31 为 preview target**：所有 idf.py 命令必须带 `--preview`（环境变量 `IDF_PREVIEW` 无效！）
- **网络**：git/pip/组件下载需代理 `http://127.0.0.1:7890`（github.com 直连被墙；components.espressif.com 可直连）
- **IDF v6 破坏性变更**：核心已无 `json` 组件——cJSON 改用组件仓库 `espressif/cjson`（组件内名 `espressif__cjson`，本仓 `components/kaleido_app/idf_component.yml` 已钉 `^1.7`）
- 构建命令：
  ```bash
  bash -c '. ~/esp/esp-idf/export.sh && cd /home/emb/project/Kaleido/kaleido-fw && idf.py --preview build'
  # 烧录（Korvo-1 接入后）：idf.py --preview -p /dev/ttyACM0 flash monitor
  ```

## 钉版（升级走独立提交 + 回归）

| 依赖 | 版本/commit | 说明 |
|---|---|---|
| ESP-IDF | v6.1 @ `fff9895c` **+ backport `c566ace1f`**（本地提交 c55d13c6：XIP PSRAM+BSS/NOINIT 链接修复，esp-idf#18791；release/v6.1 未回合） | esp32s31 preview target |
| esp-claw | `9b23c15` **+ PR#157 补丁**（本地提交 3177144：PSRAM 栈×flash cache 崩溃修复，W-3） | ../vendor/esp-claw，Apache-2.0 |
| espressif/cjson | ^1.7（lock: 1.7.19~2） | IDF v6 起替代核心 json 组件 |
| esp-sr | **2.6.0**（2026-10-24 esp32s31 编译验证通过：AFE 头文件+全组件 1300+ 目标构建 OK） | M1 接入 WakeNet/AFE（V-04 运行时验证待板卡串口权限） |
| esp_codec_dev | v2.0.x | M2 接入（TAS5805M 播放驱动，硬件验证 N） |

## 组件地图

| 组件 | 职责 | 状态 |
|---|---|---|
| kaleido_app | KApp 框架：manifest/注册表/生命周期/权限位图 | **核心代码已落** |
| kaleido_bsp | 板级抽象（korvo1 / kaleido_v1 双实现） | 头文件占位，引脚表待 G2 冻结 |
| kaleido_audio | 音频管线 + 焦点管理器（全数字链路） | 占位（依赖 G2 验证 V-01/04） |
| kaleido_ui | KShell（LVGL9+PPA+JPEG）+ token 生成物 | 占位，tokens.json 已立单源 |
| kaleido_accessory | 扩展 HAL（顶部传感器位 + 底部执行器底座） | 占位（协议 v1 基线=报告16） |
| kaleido_claw | esp-claw 装配层（上游零侵入胶水） | 占位（M1） |

## 构建路线

```bash
# M1：安装 ESP-IDF v6.1 后
idf.py set-target esp32s31
idf.py build
idf.py -p /dev/ttyACM0 flash monitor   # 目标板：ESP32-S31-Korvo-1 V1.1
```

单测（App 框架/焦点策略/协议解析，不依赖硬件）：
```bash
idf.py create-component-from ...  # M1 提供 CTest/Unity 接线
```
