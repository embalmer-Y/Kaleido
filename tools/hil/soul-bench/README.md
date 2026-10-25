# soul-bench：V-34×V-35 动画渲染 × XIP 交叉压测（M1.4 前置门）

Korvo-1（ESP32-S31）headless 压测工程。结果与判定见
`docs/gates/V-34V-35-动画与XIP交叉压测.md`（方案 v1.3/v1.4 §5 数据门）。

## 构建/烧录两个变体（任一 build 目录已存在则增量）

```bash
source ~/esp/esp-idf/export.sh
cd kaleido-fw/tools/hil/soul-bench

# XIP OFF（方案主路径）
idf.py --preview -B build_noxip6 -D SDKCONFIG=build_noxip6/sdkconfig \
  -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults" build

# XIP ON（esp-claw 现状默认）
idf.py --preview -B build_xip6 -D SDKCONFIG=build_xip6/sdkconfig \
  -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.overlay_xip" build

# 烧录（只刷 app 到 ota_0@0x20000——分区表按板上 edge_agent 布局，绝不动 NVS/bootloader）
esptool.py --chip esp32s31 -p /dev/ttyUSB0 -b 460800 --no-stub --after hard_reset \
  write_flash 0x20000 build_xip6/soul-bench.bin
```

固件上电自动跑全套基准（约 2 分钟），逐行打印 `SB,...`，结束打印 `SB,ALL,DONE`。
串口 115200 采集原始日志（样例见 docs/gates/logs/v34_*_v6c_serial.log）。

## 工程事实（改动前必读）

- **ThorVG（espressif/thorvg，meson 强制 pthread）必须在 pthread 上下文调用**——
  原生 FreeRTOS 任务里会 `assert pthread_self`（日志 v34_xip_on_v5）。基准线程用
  `pthread_create` + 64KB 内部栈。
- **lottie JSON 输入必须 NUL 结尾且可写**：EMBED_FILES 是 flash 映射无终止只读段，
  直接喂会在解析器内越界（崩溃或 rc=1）。bench_b.c 内已做"拷贝+NUL+copy=true"。
- **ALWAYSINTERNAL=0 是硬前提**：ThorVG 每帧 malloc/free churn 最高 1.14MB，
  小额分配优先内部会瞬间打穿内部堆（实测曾至 12B）。
- 任务 WDT 已关（bench 语境）；退出生产配置请勿照抄。
