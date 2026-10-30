
## Crash 取证包（KF，V-45R）

1. **事件环回放**：panic/WDT 复位后，下次开机自动打印
   `VL,CRASH,last=<原因>` + 死前最近 24 条事件（`VL,CRASH,EV,...`）。
   关键 flow 均已接 `kf_ev()`（emote/wakenet/convo/aio）。
2. **coredump 落盘**：panic 转储写 flash `coredump` 分区(0x910000,64KB)。
   离线解码：
   ```bash
   esptool --chip esp32p4 -p /dev/ttyACM0 read_flash 0x910000 0x10000 /tmp/core.bin
   python -m esp_coredump info_corefile -c /tmp/core.bin build/convo_stage.elf
   ```
3. **调试模式**：串口 'v' 切换。开=全局 DEBUG + KF 流上屏 + FB 采样；
   关=发布口径（WARN 级安静串口）。'u' = 运行时统计 + KF 健康转储
   （heap/各任务心跳年龄/事件环尾 16 条）。
4. **FPS 仪表**：`DSI,EMOTE,FPS,n` 每 1.5s 一条（n=近 1.5s 渲染帧数，
   配置上限 24；V-45R 修复后实测稳定 20-25，修复前最差 4-5）。
