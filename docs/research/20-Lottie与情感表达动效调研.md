# 20 Lottie 与情感表达动效调研

| 版本 | 日期 | 作者 | 触发 |
|---|---|---|---|
| v1.0 | 2026-10-25 | ZCode（调研子代理） | 负责人需求："类似 live2d 的程序…比如 lottie 动画"——AI 桌面伴侣在 460×460 AMOLED 上呈现"活的"角色（呼吸/眨眼/待机小动作/口型/情绪），由 LLM 情感标签驱动；质量优先但需给出内存/性能预算 |

**证据级别说明**：A = 一手源码/官方原始文件逐行验证；B = 官方文档/组件页/官方示例日志；C = 社区数据、厂商宣传或本报告外推估算。所有 URL 访问日期均为 **2026-10-25**。

**判定一句话（详见 §6）**：主推"**分层 PNG 骨架（LVGL/PPA 仿射）做常驻生命感 + EAF/JPEG 帧序列做全屏情绪动画 + Lottie(ThorVG) 做小面积（≤230×230）高保真矢量与高光时刻**"的混合管线；全屏 460×460 @30fps Lottie 在 S31 上判定不可行（单核不够 + QSPI 带宽临界），半屏 @24fps 可行但吃紧，须实测校准。

---

## §1 需求与硬件约束

### 1.1 需求拆解（动效层级）

| 层 | 动效 | 时长/频率 | 表现力要求 |
|---|---|---|---|
| L0 生命感 | 呼吸（胸腔缩放/上下浮动）、眨眼、待机小动作（歪头/看别处） | 常驻，呼吸 ~0.25Hz、眨眼随机 2-6s | 低——但**永不停**，是"活的"的关键 |
| L1 情绪 | 开心/委屈/思考/惊讶/生气…循环 | 情绪期间循环（秒级~分钟级） | 中高——需要美术质感 |
| L2 口型 | 说话嘴形 | 仅 TTS 播放期间 | 中——同步感 |
| L3 一次性动作 | 点头、欢呼、入睡转场 | 1-3s 单次 | 高——高光时刻 |
| 转场 | 情绪切换淡入淡出 | 200-300ms | 中 |

Live2D 的本质 = **分层贴图 + 网格变形 + 参数化骨骼**（呼吸=身体参数正弦驱动、眨眼= eyelid 参数阶跃、口型= viseme 参数组）。设备端要"类 Live2D"，不必复刻网格变形——**分层 + 仿射变换（位移/缩放/旋转）+ 参数化换帧**即可覆盖 L0/L2 的大部分观感（业界桌面宠物与 AI 硬件普遍如此，见 §3/§4）。

### 1.2 硬件约束（承接 03/23 号报告）

- **CPU**：ESP32-S31 双核 RISC-V @320MHz，无 FPU 密集优势场景；矢量光栅化是纯标量浮点/定点活。
- **PSRAM 16MB**：报告 23 已给峰值预算 13.5MB（保守），动画层配额约 0.9-1.8MB。
- **显示**：2.0" 460×460 AMOLED（CO5300），QSPI @50MHz。**带宽红线**：QSPI 4 线 50MHz 理论 25MB/s，有效约 12-18MB/s（指令/刷新开销）。全屏 RGB565 一帧 423KB：20fps=8.4MB/s（可行）、24fps=10.1MB/s（边缘）、30fps=12.7MB/s（超风险，需脏区局部刷新配合）。（C 级估算，假设 RGB565 输出）
- **硬件加速**：PPA 2D（旋转/缩放/alpha 混合 blit——**对分层 PNG 仿射路线是免费的**）、JPEG 硬解（**对帧序列路线是免费的**）；二者均不帮 ThorVG（CPU 光栅化）。
- **软件栈已定版**：LVGL 9.x + esp_lvgl_port 2.9.0。参考：xiaozhi-esp32 主线同样用 `lvgl ~9.5.0 + esp_lvgl_port ~2.9.0` 且已含 esp32s31 目标支持（A 级，其 main/idf_component.yml）。

---

## §2 LVGL Lottie 深度评估

### 2.1 官方支持情况（A 级源码验证）

- **lv_lottie widget 自 LVGL 9.2.0 引入**（B 级：https://components.espressif.com/components/lvgl/lvgl/versions/9.2.0 与 LVGL CHANGELOG https://lvgl.io/docs/open/9.4/CHANGELOG）。Kaleido 的 LVGL 9.x（建议锁定 9.5，与 xiaozhi 一致）原生可用。
- **依赖与配置**（A 级：https://raw.githubusercontent.com/lvgl/lvgl/release/v9.3/lv_conf_template.h 与 src/widgets/lottie/lv_lottie.c）：
  - `LV_USE_LOTTIE 1`（依赖 lv_canvas）；`LV_USE_THORVG_INTERNAL 1`（用 LVGL 内置精简 ThorVG）或 `LV_USE_THORVG_EXTERNAL`（外接库）。
  - **纠错**：不存在名为 `LV_LOTTIE_THORVG_VECTOR_ENGINE` 的宏——这是任务书里的误记。真正的"精简配置"在 LVGL 打包 ThorVG 时生成的 `src/libs/thorvg/config.h`（A 级原文）：
    ```c
    #define THORVG_SW_RASTER_SUPPORT 1          // 仅 CPU 软光栅
    #define THORVG_LOTTIE_LOADER_SUPPORT LV_USE_LOTTIE  // 只编 Lottie loader（无 SVG/PNG/TTF）
    #define THORVG_VERSION_STRING "0.15.3"      // LVGL 9.3 内置版
    #if LV_DRAW_SW_DRAW_UNIT_CNT > 1
    #define THORVG_THREAD_SUPPORT               // 可多线程渲染
    #endif
    ```
  - 即：LVGL 内置 ThorVG 只保留 SW 光栅 + Lottie loader，这正是嵌入式精简形态。
- **API**（A 级：src/widgets/lottie/lv_lottie.h）：`lv_lottie_create` / `lv_lottie_set_buffer(obj, w, h, buf)`（**buf = w×h×4 字节 ARGB8888 预乘，用户自备**）/ `set_src_data`（内存 JSON）/ `set_src_file` / `lv_lottie_get_anim`（拿 lv_anim 控制）。渲染进一个 canvas，再由 LVGL 合成到场景——alpha 由 LVGL 层合成解决（与 23 号报告的帧缓冲 RGB565 策略兼容）。
- **Espressif 官方组件** `esp_lv_lottie_player` v0.1.0（B 级：https://components.espressif.com/components/espressif/esp_lv_lottie_player）：LVGL v8/v9 通吃的 canvas 型播放器，依赖 `espressif/thorvg` 0.15（组件版 1.1.2，MIT 系许可），Apache-2.0，"支持所有目标"（含 S31）。还有独立组件 `espressif/thorvg`（B 级：https://components.espressif.com/components/espressif/thorvg）：menuconfig 可裁剪 loader（Lottie/SVG/PNG/JPEG/WebP/字体）、可选多线程。
- 若不想动 lv_conf，可用 espressif/thorvg 组件直接跑（23 号报告引用的即其 P4 示例）。

### 2.2 内存特性（A 级：LVGL maintainer kisvegabor 在 issue #5427 的结论）

https://github.com/lvgl/lvgl/issues/5427（评论经 GitHub API 原文核验，2026-10-25）：

> "它至少分配两大块内存：① 把**整个 Lottie JSON 文件搬进 RAM**；② 至少一块**内部渲染 canvas**，复杂动画可能更多。都是普通 malloc。"（2024-09-30）

- 同 issue 实测：ESP32(经典)/S3 上 28KB JSON 即内存吃紧；**ThorVG 需要大栈**（社区反馈 setup 任务 30KB、idle/ISR 栈放大 10 倍后 20×20 动画可跑）。esp_lv_lottie_player 组件页也建议 LVGL 任务栈 ≥32KB + 32KB worker 栈（B 级）。
- JSON 零拷贝访问已在上游解决（thorvg#2647 "support read-only access data without copy"，2024-08 起），espressif/thorvg 1.1.2 较新应已包含——**但需实测确认 mmap 场景不再整份拷贝**（C 级待验证项）。
- **GIF/PNG 序列别嵌进 Lottie**：esp_lv_lottie_player 文档举例 78 帧 500×417 位图序列 ≈ 62MB（B 级）。角色动画必须是**纯矢量** Lottie。
- ESP32-S3 + PSRAM 注意：9.2.x 有"lottie buffer 无法从 PSRAM 分配"的问题（B 级：https://github.com/lvgl/lvgl/issues/7402）——规避方式就是用 `lv_lottie_set_buffer` 传 `heap_caps_malloc(MALLOC_CAP_SPIRAM)` 的自备缓冲（API 本就支持，A 级签名可见）。

### 2.3 文件体积（B/C 级）

- 纯矢量 Lottie JSON 典型 **几 KB~100KB 内**（优化目标 <100KB：https://lottiewizard.com 、https://tinylottie.com ；LottieFiles 论坛 https://forum.lottiefiles.com/t/lottie-file-size-reduction/5403 ）。一个角色的一个情绪动画 20-60KB 属常态 → **8-12 个情绪动画 ≈ 0.3-0.8MB，放 assets 分区/NAND 完全无压力**（与 23 号报告 assets 1MB 预算吻合）。
- dotLottie（.lottie 压缩包，省 70-80%）**ThorVG/lv_lottie 不支持**，只能喂原始 JSON（B 级：API 只收 data/file）。存 NAND 时可自解压到 PSRAM 或 mmap。

### 2.4 性能实测锚点与 S31 外推

| 场景 | 数据 | 来源/级别 |
|---|---|---|
| ThorVG Lottie 示例 @ ESP32-P4 400MHz（esp32_p4_function_ev_board，MIPI DSI，48 帧动画，**分辨率未注明**——该板 DSI 屏为 1024×600） | **CPU 86%，FPS 19/20** | https://components.espressif.com/components/espressif/thorvg/versions/0.13.6~1/examples/thorvg-example （B；23 号报告同源） |
| ThorVG 自称 v1.0 架构重写后平均快 1.8×（对标 Skia 快 2.9×） | 宣传口径 | https://www.thorvg.org/post/thorvg-v1-0-a-new-generation-released （C） |
| Canva iOS 用 ThorVG 渲染 Lottie：快 80%、峰值内存 -70% | 案例宣传 | https://www.thorvg.org/showcase （C） |

**S31 外推（C 级估算，务必实测校准）**：CPU 占用 ≈ 像素吞吐 × 每像素成本。以 P4 示例为 1024×600@20fps=12.3Mpx/s 占 86%（400MHz、核数口径不明）归一：
- **460×460@24fps（5.1Mpx/s，0.41×）**：≈ 86%×0.41×(400/320 主频比) ≈ **单核 40-60%**（若 P4 数据是单核口径；若双核合计则翻倍到 80-110%——**口径不明是最大不确定度**，23 号报告取 60-100% 即保守口径）。
- **230×230（1/4 屏）@24fps（1.27Mpx/s，0.10×）**：≈ **单核 10-20%**，宽裕。
- **325×325（约半屏面积）@24fps**：≈ **单核 25-40%**，可行。
- **460×460@30fps**：≥75-130% 单核 + QSPI 12.7MB/s 临界 → **判定不可行**（与 23 号结论一致）。
- 多核选项：`LV_DRAW_SW_DRAW_UNIT_CNT>1` 时 ThorVG 开线程，可双核分摊光栅化——但会吃掉音频核预算，仅 T2 高光态可开（C 级策略）。

### 2.5 小结

lv_lottie 在 Kaleido 的栈上是**现成、官方、MIT** 的能力，弱点全部集中在"全屏高帧率"场景：CPU（软光栅）、RAM（ARGB8888 画布 + JSON + 引擎工作内存）、栈（32KB+）。**用它，但只用在正确的地方**（小面积/中面积/限时高光）。

---

## §3 替代路线矩阵

| 路线 | 表现力上限 | 工作量 | 内存（460×460 全屏口径） | CPU | 许可 | 判定 |
|---|---|---|---|---|---|---|
| **A. Lottie/ThorVG**（§2） | 高（矢量、平滑、设计师生态）；无网格变形，"类 Live2D"可达 70% 观感 | 低（集成）+ 高（矢量美术） | 画布 827KB ARGB8888 + JSON KB 级 + 引擎 ~0.2-1MB + 栈 32KB+ | 全屏 24fps 单核 40-60%（C）；1/4 屏 <20% | LVGL MIT / ThorVG MIT / esp 组件 Apache-2.0 | **主路线之一（限面积）** |
| **B. EAF/AAF 帧序列（Espressif 官方）** | 高（任意美术，含位图质感、发光、复杂形变——本质是视频感的帧动画）；不可程序化参数（呼吸/口型要靠序列本身或叠加） | 低集成 + 中美术（画帧/转 GIF） | 播放缓冲 RGB565 一帧 423KB（+解码行缓冲）；资产在 Flash/NAND 流式读 | **低**：RLE/Huffman 走 CPU 也轻；JPEG 帧可吃 **S31 JPEG 硬解 + PPA** | esp_lv_eaf_player Apache-2.0；esp_emote_gfx/表情组件 Apache-2.0 | **主路线之一（全屏情绪）** |
| **C. PNG 序列（自管 sprite）** | 同 B（EAF 就是它的官方格式化封装） | 中（自己写播放器/资源表） | 帧缓存 423KB×缓存深度 | 解码取决于格式（PNG 软解慢，JPEG 硬解快） | 自研 MIT | 不如 B（B 官方已做好） |
| **D. GIF（LVGL 内置 LV_USE_GIF）** | 低-中（256 色、无 alpha、帧率低） | 极低 | gifdec 全帧解码进 RAM（460×460 一帧 423KB×帧数，不可流式）→ **GIF 全屏基本不可用** | 中 | LVGL 内置（随 LVGL MIT） | 仅小图标（<100×100）可用 |
| **E. APNG** | 中（真彩色+alpha） | 中 | LVGL **无内置** APNG 解码；社区 lv_lib_png 系支持（B/C 级） | PNG 软解（inflate）不轻 | 社区库 MIT 系 | 不推荐（有 EAF） |
| **F. rlottie**（Samsung） | 中高（Lottie 子集） | 中-高 | C++/STL/异常在 ESP32 上体积与堆开销大 | 未知（无 ESP32 公开数据） | MIT（COPYING 原文 A 级），仓库 2026-09 仍有 push（B 级 API）但主力维护已停 | **排除**（ThorVG 就是它的上位替代，LVGL 官方已选边） |
| **G. Live2D 原生（Cubism SDK）** | 最高（网格变形/物理/参数化——真 Live2D） | — | — | — | **专有**：开发免费；个人/小微企业发布豁免；**AI/聊天机器人属"可扩展应用"需 Live2D 审查+特别协议**；VTuber 追踪类年销 >2000 万日元需付费；发布前 1 个月申请（B 级：https://www.live2d.com/en/sdk/license ）。且 **Cubism Core 二进制无 ESP32/嵌入式目标**（B 级：SDK 发行包平台列表） | **排除**（无嵌入式运行时 + 许可审查不可控） |
| **H. nanoem** | — | — | — | — | **纠错：nanoem 是 MMD(MikuMikuDance) 兼容运行时（MPL-2.0/MIT 双许可），不是 Live2D 运行时**（A 级 README 原文：https://github.com/hkrn/nanoem ）。MMD 模型生态与 Live2D 无关，PMX 渲染需要 GPU/OpenGL 栈，ESP32 不适用 | **排除**（任务书认知纠偏） |
| **I. 自研分层骨架（PNG 分层 + LVGL 仿射/PPA）** | 中-高（呼吸=缩放/位移、眨眼=换 eyelid 层、口型=viseme 换帧、摇摆=旋转）——**这正是消费级桌面伴侣的主流实现形态**；网格变形做不了 | 中（一次性，之后纯内容生产） | 图层缓存：一套 460×460 角色切 5-8 层 RGB565+A8 ≈ 1-1.5MB PSRAM 常驻 | **极低**：变换走 PPA 硬件 blit 或 LVGL sw（局部脏区）；换帧=换指针 | 自研 MIT | **主路线基座（L0/L2 常驻）** |
| J. 视频流 | — | — | — | — | — | 已排除（任务书） |

EAF/AAF 关键事实（B 级）：
- **EAF**（Emote Animation Format）：`esp_lv_eaf_player`（https://components.espressif.com/components/espressif/esp_lv_eaf_player ，v0.3.0，Apache-2.0）以 LVGL widget 形式播放（`lv_eaf_create`），RLE/Huffman/JPEG 压缩可选、RGB565 优化、逐帧延时可控（示例 50ms=20fps）、官方在线转换工具 https://esp32-gif.espressif.com/ 。
- **AAF**：`espressif2022/image_player`（https://components.espressif.com/components/espressif2022/image_player ）的 JPEG 帧容器（附 GIF→AAF 脚本），直推 esp_lcd panel，xiaozhi esp-hi 机器人用它在 240×280 屏跑 15fps 表情（A 级源码）。
- **esp_emote_expression**（https://components.espressif.com/components/espressif2022/esp_emote_expression ，v1.0.2，Apache-2.0，30 万+下载）：整套"表情状态机"参考实现——idle/speak/listen/system 事件、情绪循环、**插入式对话动画（anim dialog）**、UI 对象（label/二维码/自定义 anim）、资产 .bin 打包 + index.json + mmap。**Kaleido 的动画子系统可直接对标其架构**（A 级：xiaozhi 集成代码 + B 级组件文档）。

---

## §4 情感→动画驱动链路设计

### 4.1 业界机制考证

**xiaozhi-esp32（A 级源码，clone @ main 分支 2026-10-25）**：
- 协议：WebSocket 下行 JSON `{"type":"llm","emotion":"happy"}` → `Display::SetEmotion(name)`（application.cc L679-689）。**云端负责从 LLM 提取情感词，设备只收一个字符串标签**。
- 标准情感词表（esp-vocat emote.json，23 个 tag，A 级）：happy/laughing/funny/loving/embarrassed/confident/delicious/sad/crying/sleepy/silly/angry/surprised/shocked/thinking/winking/relaxed/confused/neutral + 系统态 idle/low_battery/battery_connected 等。**注意映射是多对一**（funny/loving/winking…全指向 Happy.eaf）——设备端只备 8-10 个基础动画，云端口子开 20+ 情感词。
- 播放：EAF 文件 + `{"emote":"happy","src":"Happy.eaf","loop":true,"fps":20}` 映射表；说话/倾听/开机有独立状态动画。

**Open-LLM-VTuber（A 级源码，clone @ main 2026-10-25）**：
- Prompt 机制（prompts/utils/live2d_expression_prompt.txt 原文）：把当前模型 emotionMap 的键列表注入系统提示词，要求 LLM **在回复文本里内联 `[表情名]` 标签**（"只能用列出的关键词、必须带方括号"）。
- 解析（agent/transformers.py）：按句切分流式输出，逐句 `extract_emotion()` 扫描 `[key]` → `Actions(expressions=[模型表情索引])`，随 TTS 音频段一起发前端；live2d_model.py 维护 `emotionMap: {关键词: 表情索引}`（来自 model_dict.json，随 Live2D 模型走）。
- 前端：pixi/Cubism WebSDK 播表情 motion，**口型由音频音量驱动**（WebSDK 侧 speaking 状态）。
- 价值：证明了**句级内联标签**在流式对话里的可行性（表情与语音天然对齐，无需额外 function calling 往返）。

**其他**：xiaozhi-assets-generator（https://github.com/78/xiaozhi-assets-generator ）提供网页端自定义表情/字体/唤醒词资产生成——资产管线的社区范本（B 级）。"Muse avatar"未检索到同名知名开源项目（C 级，疑为记忆偏差；近似物即 Open-LLM-VTuber / Vtuber_Kit / ChatdollKit）。

### 4.2 Kaleido 推荐链路（设计稿）

```
LLM（云端）
 ├─ 主通道：对话流按句输出，每句尾部内联 [情绪] 标签（Open-LLM-VTuber 式）
 ├─ 兜底：整轮结束发 {"type":"llm","emotion":"<23词表>"}（xiaozhi 式，兼容生态）
 └─ 可选：function calling 输出 {emotion, intensity:1-3, action:"nod|shake|none", duration_s}
        ↓  WS 下行（JSON，字节级开销 <100B/句）
设备端 动画子系统（单 LVGL 任务，对标 esp_emote_expression 架构）
 ├─ 情感词归一：23 词表 → 8-10 基础情绪 × intensity（多对一映射表，热更新自 NAND JSON）
 ├─ 四层合成状态机：
 │   L0 idle 层（常驻）：分层 PNG 骨架——呼吸(缩放±1.5% 正弦)、随机眨眼(eyelid 帧序列 120ms)、
 │       待机小动作(每 15-40s 随机播歪头/环视/掏耳朵，一次性动画后回落)
 │   L1 情绪层：情绪循环动画（EAF 全屏 或 分层姿态组），优先级覆盖 idle 姿态；
 │       转场 = LVGL opacity 补间 250ms 淡入淡出（lv_obj_set_style_opa + lv_anim）
 │   L2 口型层：TTS 播放期间激活——3-5 个 viseme 层按音频能量包络切换（10-12Hz 换帧，
 │       音频回调算 RMS，零额外任务）；静音段回落闭嘴帧
 │   L3 动作层：一次性动作（点头/欢呼/入睡），播完自动回落 L1
 ├─ 仲裁：speaking(L2) > 情绪(L1) > idle(L0)；系统态(低电/连接中)直接抢占全层
 └─ T2 高光仲裁（联动 23 号报告 §3.2）：全屏 Lottie/EAF 高光时降级后台 Lua、LLM 只收流不刷表情
```

关键取舍：**情感标签由云端 LLM 产出**（设备不做 NLU 情感分析——省 CPU 与不确定性）；**口型由本地音频能量驱动**（不依赖文本对齐，天然同步）；情绪切换频率限 0.5-1Hz 防抖。

---

## §5 资产管线与许可

### 5.1 Lottie 管线

- 制作：After Effects + Bodymovin 插件导出 JSON（B 级常识链：https://lottiefiles.com/blog ）。**纯矢量**（形状/路径/变换），禁用位图序列、效果插件、表达式，关键帧精简、小数位压缩 → 单动画 20-100KB。美术成本：一个熟练 AE 动画师做一个角色情绪循环约 1-3 天（C 级行业估计）——**全表情 Lottie 化的美术预算是隐藏大头**。
- 免费资产：LottieFiles 免费区适用 **Lottie Simple License**——免费商用、可修改、无署名义务（B 级：https://lottiefiles.com/page/license ）；但 Marketplace 付费资产走卖家独立条款，不可默认再分发。免费区资产多为 UI 动效（勾选/火箭/礼物），**完整角色情感动画仍需定制**。
- dotLottie 不适用于 ThorVG（§2.3）；交付物定为原始 JSON + 命名规范 `emo_<name>_<loop|once>.json`。

### 5.2 帧序列（EAF/AAF）管线

- 制作：AE/Spine/任何工具出帧或 GIF → 官方在线工具 https://esp32-gif.espressif.com/ （EAF）或 gfx-gen-tool.pages.dev（esp_emote 资产包）→ .eaf/.bin + emote 映射表 JSON 入 NAND。位图美术门槛低于矢量 Lottie（AI 生成底图 + 逐帧修型也可行）。

### 5.3 分层 PNG 管线

- 制作：角色立绘（AI 生成底图：SD/Midjourney + 人工修）→ PSD/Krita 分层（身体/左眼睁开/左眼闭合/右眼×2/嘴 viseme×5/前发/饰品，8-12 层）→ 导出 RGB565+A8 或 PNG（构建期转 LVGL bin）→ PSRAM 图层缓存。**AI 生成图的服务条款与训练数据版权风险需法务过一眼**（C 级提示）；对外开源时资产必须可再分发（CC0 优先，CC-BY 需署名机制）。
- 分层资产是三条管线里**可复用性最高**的（同一套层可程序化组合出无数微动作）。

### 5.4 许可红线汇总

| 项 | 许可 | 结论 |
|---|---|---|
| LVGL / ThorVG / lv_lottie | MIT | 可商用、可再分发（保留版权头） |
| esp_lv_lottie_player / espressif thorvg / esp_lv_eaf_player / esp_emote_* / image_player | Apache-2.0 | 可商用 |
| rlottie | MIT（部分子目录另有条款） | 已排除，无风险 |
| Live2D Cubism SDK | 专有；AI/聊天机器人类"可扩展应用"需审查+特别协议；小微企业豁免但发布需提前 1 个月申请；年销 >JPY 2000 万（VTuber 口径）需付费 | **不采用**（且无嵌入式 Core） |
| Live2D 官方示例角色（Shizuku/Haru 等） | 逐角色单独条款（如 Shizuku 不得改名改设定） | **不可**用于 Kaleido 角色 |
| LottieFiles 免费资产 | Simple License（免费商用免署名） | 可用于 UI 点缀；角色定制 |
| nanoem | MPL-2.0/MIT（但它是 MMD 运行时） | 不相关 |

---

## §6 推荐与预算（含 S31 档位判定）

### 6.1 主路线：三层混合管线

> **基座 I（常驻生命感）：自研分层 PNG 骨架 + LVGL 补间/PPA 仿射**——呼吸/眨眼/待机小动作/口型全在这层，CPU 几乎免费，是"活的"的底座。
> **主力 II（情绪表达）：EAF 帧序列（JPEG 压缩 + S31 JPEG 硬解/PPA）全屏 20-24fps**——全屏质感与美术自由度的主力，闪存/NAND 流式，PSRAM 占用小。
> **点睛 III（高保真矢量）：Lottie(ThorVG) 限 ≤230×230 常态 + 限时半屏/全屏高光**——矢量小动画（心情气泡、粒子、图标配光）与开机/庆祝等 T2 时刻。

降级路线（任一环节失败时的兜底）：**全 EAF + 分层 PNG**（无 Lottie 依赖，xiaozhi esp-vocat 已验证该形态成品可用）。

### 6.2 内存/性能预算表（460×460，RGB565 主帧缓冲，承接 23 号报告）

| 项 | PSRAM | Flash/NAND | CPU（单核口径，C 级估） | 备注 |
|---|---|---|---|---|
| 主帧缓冲 RGB565 双缓冲 | 846KB | — | — | 23 号报告既有 |
| **I. 分层骨架** | 图层缓存 1-1.5MB（8-12 层常驻） | 每层 30-150KB | **<10%**（PPA blit + LVGL 补间，局部脏区） | 口型换帧=换指针 |
| **II. EAF 全屏 20fps** | 解码/播放缓冲 423KB×1-2 | 每动画 0.5-2MB（JPEG 帧 ~15-30KB/帧） | **15-40%**（RLE/Huffman 软解；JPEG 走硬解更低，待实测） | QSPI 8.4MB/s，安全 |
| II'. EAF 全屏 24fps | 同上 | 同上 | 20-50% | QSPI 10.1MB/s，边缘（需脏区/直推 panel 绕过主帧缓冲，AAF 直推模式可行） |
| **III. Lottie 230×230@24fps** | 画布 212KB + JSON ≤100KB + 引擎 0.2-1MB | 每动画 20-100KB | **10-20%** | 宽裕，可常驻 |
| III'. Lottie 325×325@24fps | 画布 423KB + 同上 | 同上 | **25-40%** | 可行（T1 态） |
| III''. Lottie 全屏 460×460@24fps | 画布 827KB + 同上 | 同上 | **40-60%（乐观）~ >100%（保守口径）** | 仅 T2 高光、限时、并降级并发 |
| III'''. Lottie 全屏 @30fps | 同上 | — | **>100% 单核 + QSPI 12.7MB/s 临界** | **判定：不可行** |
| ThorVG 栈与任务 | 内部 RAM 压力：任务栈 32KB+ | ThorVG 代码 ~200-400KB（C，23 号报告） | — | 内部 RAM 紧张者是栈，非堆 |

### 6.3 档位判定（"质量优先"下 S31 能跑什么）

| 档位 | 内容 | 判定 |
|---|---|---|
| T0 日常待机 | 分层骨架呼吸/眨眼/小动作 @24-30fps | **稳**（<10% CPU） |
| T1 对话 | EAF 全屏情绪 20fps + 骨架口型叠加；或 325×325 Lottie @24fps | **可行**（单核 25-50%，QSPI 20fps 安全/24fps 边缘） |
| T2 高光 | 全屏 Lottie @20-24fps 限时（开机/庆祝/开箱），或全屏 EAF 24fps | **条件可行**：允许降级后台并发 + 动画焦点仲裁；实测不过则退全屏 EAF |
| 禁区 | 全屏 Lottie @30fps 常驻 | **不可行**（CPU 单核超载 + QSPI 带宽临界，双重红线） |

---

## §7 风险与第 2 轮建议

| # | 风险/缺口 | 等级 | 建议（V 系列验证项，并入 23 号报告 §6） |
|---|---|---|---|
| 1 | ThorVG 在 S31 零实测；P4 基准 86% 的核数/分辨率口径不明 | 高 | 把 espressif thorvg lottie example 改靶 esp32s31 + 460×460 QSPI 屏：测 230²/325²/460² × 20/24fps 的单核 CPU 与 heap 峰值（perfmon + heap_caps_print_heap_info） |
| 2 | ThorVG 引擎工作内存随动画复杂度波动（多 canvas） | 高 | 用真实角色 Lottie（非官方示例）压测 PSRAM 峰值；同时验证 espressif/thorvg 1.1.2 是否已走 JSON 零拷贝（thorvg#2647），NAND mmap 场景是否整份拷贝 |
| 3 | QSPI @24fps 全屏刷新带宽 10.1MB/s 逼近有效带宽 | 中 | 实测 CO5300 有效带宽；备选：EAF/AAF 直推 panel（绕过 LVGL 主帧缓冲）与脏区刷新 |
| 4 | 口型与 TTS 时序（能量包络回调延迟） | 中 | 在音频输出回调测 RMS→viseme 切换延迟；不达标则退化为"说话=开合循环"两帧口型（xiaozhi 同款简化） |
| 5 | Lottie 角色美术成本被低估（矢量逐情绪定制） | 中 | 先打样 1 个情绪（开心循环）+ 1 个高光（开机），评估单动画人日后再定 Lottie 覆盖比例；EAF 承接其余 |
| 6 | 情感协议未定版 | 低 | 直接采纳 §4.2：句级内联 [tag]（兼容 Open-LLM-VTuber 生态）+ 整轮 emotion JSON（兼容 xiaozhi 词表），设备端 23 词表多对一映射 |
| 7 | esp_emote_expression 与自研状态机二选一 | 低 | 第 2 轮读 esp_emote_gfx/expression 源码评估直接集成 vs 借架构自研（其 UI 对象/事件模型与 Kaleido KShell 的耦合度） |

### 附：本报告引用源码核验清单（均 2026-10-25）

- LVGL release/v9.3：`src/widgets/lottie/lv_lottie.{c,h}`、`src/libs/thorvg/config.h`（ThorVG 0.15.3）、`lv_conf_template.h` —— https://raw.githubusercontent.com/lvgl/lvgl/release/v9.3/（A）
- LVGL issue #5427 评论（GitHub API 原文）—— https://github.com/lvgl/lvgl/issues/5427 （A）
- xiaozhi-esp32 @ main（tarball 下载）：`main/application.cc`（emotion 协议）、`main/display/display.h`、`main/boards/espressif/esp-vocat/assets/360_360/emote.json`、`main/boards/espressif/esp-hi/emoji_display.cc`（AAF/anim_player）、`main/idf_component.yml`（esp32s31 目标、组件版本）—— https://github.com/78/xiaozhi-esp32 （A）
- Open-LLM-VTuber @ main：`prompts/utils/live2d_expression_prompt.txt`、`src/open_llm_vtuber/live2d_model.py`、`src/open_llm_vtuber/agent/transformers.py`、`LICENSE-Live2D.md` —— https://github.com/Open-LLM-VTuber/Open-LLM-VTuber （A）
- nanoem README —— https://github.com/hkrn/nanoem （A）
- rlottie COPYING + 仓库元数据 —— https://github.com/Samsung/rlottie （A/B）
- 组件页：esp_lv_lottie_player / espressif thorvg 1.1.2 / thorvg-example（P4 86%）/ esp_lv_eaf_player / esp_emote_expression / image_player —— components.espressif.com（B）
- 许可页：Live2D SDK Release License / LottieFiles Simple License（B）
