# V-36 JPEG 硬解背景与舞台定稿（2026-10-26）

## 结论

**JPEG 硬解链路验证通过并封存；舞台产品定稿为纯色背景 + 角色（无信息卡）。**

## 硬解验证（esp_driver_jpeg，IDF v6 内置裸驱动）

| 项 | 结果 |
|---|---|
| 800×1280 q82 JPEG → RGB565 | **11ms**（含 PSRAM 拷贝+引擎初始化；SW 约 300-500ms） |
| 字节序 | `JPEG_DEC_RGB_ELEMENT_ORDER_BGR`（小端）= LVGL RGB565 原生；资产内嵌红/绿/蓝色块自检 F800/07E0/001F 通过 |
| 全屏 lv_image 合成 | 背景常驻 fb，逐帧开销零增量（render/disp/cb 与纯色版持平） |
| 目视 | 背景正常、角色合成正常、无新瑕疵（用户验收） |

组件备注：注册表 `espressif/esp_jpeg`(1.3.2) 是 Tjpgd **软件**解码；硬解走 IDF 自带 `esp_driver_jpeg`（`jpeg_new_decoder_engine` → `jpeg_decoder_process` 一次性整图解码）。P4 输出 RGB565 时 YUV420/422 会有 16px 对齐 padding，800/1280 恰好对齐无影响。

## 产品决策（用户裁决）

- 信息卡（天气/设备/对话）**砍掉**：后续可能换小尺寸屏，信息块布局不通用
- 背景**回归纯色**：图像背景的解码/合成能力已证明，但当前产品形态不需要
- `bg_jpeg.c` 休眠封存（壁纸/相框/用户图库场景直接启用）
- 时钟不上假时间，等 SNTP 接入后再上

## 沉淀

1. LVGL 9.4 **无内置中文字体**（simsun 是 v8 的）：CJK 子集字体流水线（lv_font_conv）是未来对话文本上屏的前置项，已登记
2. `sdkconfig.defaults` 不会回灌已生成的 sdkconfig——新 Kconfig 项要同时写入两者
3. LVGL 9.x `lv_timer_t` 结构在私有头里，回调内不要解引用 timer 指针（跨任务传参用文件级静态）
4. 间歇性启动挂死（串口开口复位后 0.7s 内挂死→HW WDT 二次启动）持续存在，约每次开口复位有概率触发；跑稳后不再复发。B-2 类，待根因（疑似 Boya flash QIO@80MHz 启动时序）

## 状态

M1.6 L0 舞台最终形态：纯色底 + 325² lottie@30fps（pthread 三缓冲邮箱 + lv_canvas 合成 + direct_mode/avoid_tearing）。
下一站候选：WakeNet 单麦唤醒（M2 前置）/ M1.5 验收线裁决 / V-37 XIP 复测。
