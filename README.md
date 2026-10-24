# kaleido-fw

Kaleido 智能桌搭固件（**Kaleido-Claw** 架构）。架构设计：`../docs/design/kaleido-claw-架构.md`。
开发规范：`../constraints.md` §6。

## 状态

**M0 骨架阶段**（2026-10-24）：目录结构 + KApp 框架核心代码。
构建需 ESP-IDF v6.1（尚未安装到本机；M1 安装并出首个 CI 构建）。

## 钉版（升级走独立提交 + 回归）

| 依赖 | 版本/commit | 说明 |
|---|---|---|
| ESP-IDF | v6.1（commit 待首个构建锁定） | esp32s31 target（preview） |
| esp-claw | 9b23c15bc28703369b4b7fad3e810f8cb535fb59 | vendor/esp-claw，Apache-2.0 |
| muse-gadget-sdk | c9354b7a765cfb7dd3c4f1945a9ca838d871630c | vendor/muse-gadget-sdk，Apache-2.0；集成前闭环 tinyh264 LGPL 疑点（docs/vendor-licenses.md R-7） |
| esp-sr | v2.6.0 | WakeNet/AFE，S31 支持（preliminary） |
| esp_codec_dev | v2.0.x | TAS5805M 播放驱动（硬件验证 N，M2 实测） |

## 组件地图

| 组件 | 职责 | 状态 |
|---|---|---|
| kaleido_app | KApp 框架：manifest/注册表/生命周期/权限位图 | **核心代码已落** |
| kaleido_bsp | 板级抽象（korvo1 / kaleido_v1 双实现） | 头文件占位，引脚表待 G2 冻结 |
| kaleido_audio | 音频管线 + 焦点管理器（全数字链路） | 占位（依赖 G2 验证 V-01/04） |
| kaleido_ui | KShell（LVGL9+PPA+JPEG）+ token 生成物 | 占位，tokens.json 已立单源 |
| kaleido_muse | Muse Link 包裹层（KALEIDO_ENABLE_MUSE 开关） | 占位 |
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
