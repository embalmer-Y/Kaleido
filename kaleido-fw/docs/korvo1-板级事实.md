# ESP32-S31-Korvo-1 V1.1 板级事实（固件适配用）

> **存档（2026-10-25 D-8 平台切换）**：主开发板已换 osptek P4C5（ESP32-P4），
> Korvo-1 因 flash 物理故障返修（W-7）不再是现行目标板。本文硬件事实仍有效，
> 但"BSP 以此为准 / M1 计划"等表述仅存档；现行板级事实见
> `../docs/gates/V-38-P4C5点板与平台切换实测.md`。

> 来源：esp-claw 板级定义（vendor/esp-claw/application/edge_agent/boards/espressif/esp32_s31_korvo1/，钉版 9b23c15）+ G1 报告 15 附节。
> 提取日期：2026-10-24。BSP 实现（kaleido_bsp korvo1）以此为准，与官方 schematic 冲突时以后者为准并回改本文件。

## 音频
- I2S0 std 收发共用配置：48kHz、16bit、立体声、mclk_multiple 256、MCLK 未用(-1)
  - BCLK = GPIO3，WS = GPIO4（data in/out 引脚见 board_peripherals.yaml 全文，实现时核对）
- codec：ES8389（I2C 控制）——**模拟麦×2 + NS4150B×2 功放**（非 PDM、非数字功放，与 kaleido_v1 目标架构不同，仅作验证/对照板）
- PA_CTRL（NS4150 使能）= GPIO7，高有效（default_level 0 = 关）

## 其他外设
- WS2812 状态灯 = GPIO37（RMT，GRB，1 颗）
- SD 电源使能 = GPIO39（低有效）
- ADC 按键组 = GPIO42（VOLUP 380mV / VOLDOWN 820mV / MODE 1340mV / SET 1870mV）——esp-claw 中默认注释未启用
- DVP 摄像头连接器（OV3660 级）、4.3" 800×480 RGB LCD 子板、USB-A host、NAND 预留（详见 G1 报告 15）

## esp-claw 现成支持（M1 快速通道）
- 板级包 `esp32_s31_korvo1`（board_info/devices/peripherals.yaml + sdkconfig.defaults.board + setup_device.c）
- **M1 计划**：直接构建 esp-claw edge_agent @korvo1 烧录验证板 → 真机验证 Claw agent 全链（IM 聊天/Lua/写代码），再迁回 kaleido-fw 装配
