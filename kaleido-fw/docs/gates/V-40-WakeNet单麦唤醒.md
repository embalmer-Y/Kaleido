# V-40 WakeNet 单麦唤醒（2026-10-26）

## 结论

**单麦唤醒全链路打通并双路验收通过**（TTS 回环自验 10/10 + 真人全命中），
资源占用与 Lottie 舞台共存无压力。

## 配置

| 项 | 值 |
|---|---|
| 组件 | esp-sr 2.6.0（2026-10-09 版，支持 P4；注册表 `espressif/esp_jpeg` 是软解、不相关） |
| 模型 | wn9_nihaoxiaozhi_tts「你好小智」**TTS 鲁棒变体**——TTS 合成语音同样可唤醒，正是回环自验证所需 |
| 模型存放 | `model` 分区（spiffs，0xF20000 起 896KB，littlefs 之后尾部分配；esp-sr CMake 自动打包 srmodels.bin 291KB 并随 flash 写入） |
| AFE | v1n2，输入格式 "M"（单通道，直接吃 aio_capture 的左声道提取），AFE_TYPE_SR + HIGH_PERF，`MEMORY_ALLOC_MORE_PSRAM` |
| 任务 | wn_feed(4K, CPU0) 持续喂 512 样本块；wn_det(8K, CPU1) fetch 阻塞等结果 |

## 资源

- 内部 RAM：**-6KB**；PSRAM：**-393KB**
- CPU：**wn_det ≤7%（IDLE1 剩 93%）**——与 30fps Lottie 舞台双核共存可行
- 唤醒即双短鸣（880Hz×300ms×2）确认

## 验收

1. **TTS 回环自验**：喇叭每 7s 说一次"你好小智"→ 麦克风 → WakeNet。**10/10 命中**，
   feed 起算延迟 1.9-3.1s（含云端 TTS 首包 ~0.8s + 语音本身 ~1s，声学检测在词尾即触发）；
   音量 -20~-33 dBFS
2. **真人验收**：正常音量说"你好小智"两三次，**全部命中**，无误触发

## 踩坑

1. dashscope TTS：finish-task 后同一 websocket 会话再喂新文本报
   `Missing required parameter 'payload.task_group'`——**每轮独立开关会话**绕开（M2 长对话要复用会话需在 ks_tts 层做续喂状态机）
2. esp-sr 模型必须在分区表里有 `model` 分区，否则 `esp_srmodel_init` 报
   "Can not find model in partition table"（sdkconfig 选了模型也不够）
3. `esp_afe_handle_from_config` 声明在 `esp_afe_sr_models.h`（非 iface 头）

## 后续登记

- **自定义唤醒词**（"你好墨墨"等品牌词）：需乐鑫定制模型服务；过渡期用"你好小智"
- **AEC 回声免疫**：当前无 TX 参考，自己说话中若 TTS 恰含唤醒词可能误触发；参考通道在同一条 I2S 上可取，接入 AFE input_format "MR" 即可（M2）
- **唤醒→对话状态机**（wake → beep → ASR round → 回监听）= M2 主体
- serial 工具口径：开口必复位板子 + 偶发启动挂死轮盘（V-39 已沉淀），readline 必须带 timeout
