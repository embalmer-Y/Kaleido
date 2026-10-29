# C5 恢复包与今晚执行单（2026-10-29；等负责人切换物理 USB 开关）

## 根因（上游 issue 证据链闭环）

**esp-idf#18886**：IDF 6.0.2/6.1-rc1 的 ESP32-C5 app 因 RISC-V ZCMP
回归在进 app_main 前即崩（Interrupt WDT；修复 commit 104f8ff5 仅在
master，未进 release/v6.1）。我方 C5 镜像=v6.1 构建 → 崩溃 →
SDIO 从机永不初始化 → 总线无应答。P4 host 不受影响（C5 特定
代码生成路径）。次级佐证：5.5.4 已带 SOC_CPU_ZCMP_WORKAROUND
（该区域 6.x 才出回归）；C5 bootloader 文档确认 5.x bootloader
本可启动 6.x app（排除 bootloader 版本门）。

另有过程发现：esp-hosted-mcu#128（P4 经 RPC OTA 更新 C5 的已知
抖动）与 3.x host 无条件拒连 2.x slave（芯片枚举改号 0x17→0x0d，
迁移文档"back-compatible"与实现矛盾）——均记入战报
docs/hosted3-升级与C5恢复-2026-10-29.md。

## 本目录产物（esp-hosted-mcu v3.0.10 cp，IDF v5.5.4 构建=官方 CI 同版）

| 文件 | 烧写偏移 | 说明 |
|---|---|---|
| bootloader.bin | 0x2000 | |
| partition-table.bin | 0x8000 | ota_0/ota_1 双槽 4MB 布局 |
| ota_data_initial.bin | 0xd000 | |
| eh_cp_wifi_sta.bin | 0x10000 | 3.0.10 slave app（1.15MB） |

flash 参数：dio / 80m / 4MB（见 flash_args）。

## 执行单（开关切到 C5 后，/dev/ttyACM* 会出现第二个设备）

```bash
source ~/esp/esp-idf/export.sh   # 任一版本皆可，esptool 通用
# 0) 识别 C5 端口与 flash 大小（新出现的 ttyACM 即 C5）
python -m esptool --port /dev/ttyACM1 flash_id
# 1) 先整片备份（另一 OTA 槽可能仍存出厂 2.12 镜像）
python -m esptool --port /dev/ttyACM1 --baud 460800 \
    read_flash 0 0x400000 /tmp/c5_backup_before_recovery.bin
#    （若 flash_id 报 8MB 改 0x800000；备份留档）
# 2) 全套烧写（--no-stub 不需要——esptool stub 对 C5 正常，但
#    沿用无 stub 口径更稳）
python -m esptool --chip esp32c5 --port /dev/ttyACM1 -b 460800 \
    --before default-reset --after hard-reset --no-stub \
    write-flash --flash-mode dio --flash-freq 80m --flash-size 4MB \
    0x2000 bootloader.bin 0x8000 partition-table.bin \
    0xd000 ota_data_initial.bin 0x10000 eh_cp_wifi_sta.bin
# 3) 开关切回 P4 → P4 复位 → 看串口：
#    eh_init_evt: fw versions: host=3.0.10 coprocessor=3.0.10 (match)
#    → wifi → READY → 全测试链（soak/复位采样/128B 行实验）
```

注意：
- 若 C5 实际 flash 为 2MB（flash_id 判断）：分区表 ota_1 超界，
  需改用 partitions_eh_cp_ota_2m.csv 重构建（仓库 example 目录有）。
- 备份文件勿删——若新镜像仍有问题，可整片回写并回到 2.12 状态
  （连 P4 也要回退 2.12.9+补丁，镜像在 c5_backup 内）。
- 烧完 C5 开关切回时**不要断 P4 电**（NVS/记忆不受影响，C5 新表
  与 P4 的 storage 无关）。
