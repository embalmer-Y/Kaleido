# p4-bringup — osptek P4C5 底板冒烟（B-1 门禁）

平台切换 S31→P4C5 的第一块板级 HIL。2026-10-25 全绿。

## 实测事实

| 项 | 值 | 备注 |
|---|---|---|
| 芯片 | ESP32-P4 v3.2 双核 | ROM eco7 2026-01-09 |
| flash | 16MB QIO@80MHz | JEDEC 厂商 0x68；出厂固件可正常运行（与上块 S31 的 0x46 假读不同） |
| PSRAM | 32MB HEX@200MHz 片内叠封 | `largest_psram=32256KB` |
| 空闲堆 | 内部 578KB / PSRAM 32765KB | 应用初始化后 |
| 烧写 | **`--no-stub` @230400 唯一稳定** | stub 在 921600/460800/230400 均无响应（CH343P 通路）；sdkconfig 已固化 NO_STUB |
| 复位 | CH343 RTS 直拉 EN 无效 | 用 `esptool --after hard-reset` 复位 |

## 分区表（沿用出厂布局）

```
nvs 0x11000 24K / phy 0x17000 / factory 0x20000 8MB / storage(littlefs) 0x820000 7MB
```

出厂固件已备份：`backup/factory_app_backup.bin`（1MB app）+
`backup/partition_table_backup.bin`。恢复出厂 = 把两者按 `idf.py flash` 的
偏移写回。

## 板卡硬件（Gitee：osptek/esp32-p4c5-module-dev-board）

- ESP32-C5HF4 负责 WiFi6/BT5（P4 无射频，SDIO + esp_hosted）
- ES8311 codec + NS4150 功放 + 单硅麦 LMA3729
- 10.1" 800×1280 JD9366 MIPI-DSI（套件屏）
- 拨动开关：上=P4 / 下=C5（切 CH343 到对应芯片，给 C5 烧固件时往下拨）
- 引脚表：待原理图提取（见 p4 引脚表 HIL 后续文档）

## 烧写

```
idf.py -p /dev/ttyACM0 -b 230400 flash
```
