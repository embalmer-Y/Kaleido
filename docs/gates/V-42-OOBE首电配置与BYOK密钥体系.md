# V-42 — OOBE 首电配置（Web 配置页）+ BYOK 密钥体系

日期：2026-10-26。结论先行：**设备侧通过**。密钥经浏览器 Web 页配置
（负责人指示：长密钥界面输入成本过高），NVS 持久 + 取值链回退 +
SoftAP 热点态 bring-up 全部上板实证；手机真机走查留负责人验收。

## 判据与实测（全部上板，2026-10-26）

| 判据（计划版） | 实测 |
|---|---|
| 配置页可访问、密钥可粘贴提交 | ✅ STA 局域网 curl：页面 1272B（中文/移动端/textarea）、POST /save 表单解析 ds+mm 双字段全中（`save: fields=0x6`） |
| 密钥持久化 | ✅ POST 真实 key → 板自重启 → `nvs_mask=0x6` + MiniMax URL 从 NVS 里的 JWT 解析成功（`VL,LLM,MINIMAX,ok`） |
| 状态可观测 | ✅ GET /status → `{"configured":6,"ip":"192.168.2.58"}` |
| captive 助推 | ✅ /generate_204 → 302 → /（Android/iOS/Windows 三探测 URL 注册） |
| 出厂复位 | ✅ POST /erase → 重启 → `nvs_mask=0x0`，回退链（NVS→CONFIG）继续工作 |
| SoftAP 热点态（首电形态） | ✅ 运行时探针 'o'：`esp_wifi_set_mode(AP)+set_config+start` 经 esp_hosted→C5 RPC 全过，横幅 `ssid=Kaleido-OOBE,url=http://192.168.4.1/`，无崩溃 |
| 语音链无回归 | ✅ 't' 内置轮：ASR 全文/LLM 1240ms/first_audio 3514ms，泵与 TTS 无异常 |
| 端到端计分 | **11/11 PASS**（页面/状态/重定向/保存/持久/解析/复位/回退链） |

## 落地形态

- **kaleido_secret 组件**（新）：NVS namespace `ksec`（wifi_ssid/pass、
  ds_key、mm_key），`ksec_get/set/erase_all/state` + 全链统一的
  `ksec_get_fallback()`（NVS → CONFIG → 空）。出厂镜像 CONFIG 为空。
- **provision.c**（voice-loop-p4）：同一套 HTTP 服务两种承载——首电
  SoftAP（开放网络 Kaleido-OOBE，192.168.4.1）与已入网密钥轮换
  （'k' 起服务，局域网浏览器可达）。留空字段=保持原值；保存/抹除后
  3s 自重启。
- **首电分流**（voice_loop_main）：无 WiFi 凭据 → SoftAP+Web；有则
  正常连网；密钥缺口时 Web 常驻待补。

## 工程教训（新沉淀）

1. **NVS 存在性探测必须 `buf=NULL` 取长度**：用短探测缓冲
   `nvs_get_str`，值长于缓冲返回 `INVALID_LENGTH` 被误判"不存在"——
   本次密钥（90B/690B）对 16B 探测缓冲全灭，写入其实一直成功，
   状态读全错，一度伪装成"持久化失败"。
2. esp_http_server v6 常量是 `HTTPD_RESP_USE_STRLEN`（旧名 USE_PLEN
   不存在）；v6 把 default netif 创建器拆去了 `esp_wifi_default.h`；
   `esp_netif_create_default_wifi_*` 返回 netif 指针，**不能**包进
   `ESP_ERROR_CHECK`。
3. 测试侧：测 302 必须禁用 urllib 的自动重定向跟随，否则看到的是
   落地页 200（本次第一版误报 captive 失败）。

## 留守（登记）

- **即测即显**（保存后连网+云端探活结果回显页面）：V-42 phase 2，
  属体验增强，不挡 V-43。
- **captive DNS 未起**：自动弹窗不保证，横幅明示手输 `192.168.4.1`；
  补 lwip 迷你 DNS 随 phase 2。
- **开放 AP + 明文 HTTP**：V-1 安全口径（见 provision.c 头注）；
  产品化上 WPA2-PIN/HTTPS，登记于 M2 风险表。
- **手机真机走查**：✅ **负责人验收通过（2026-10-26）**——出厂镜像
  （CONFIG 凭据全空）上电进热点态 → 手机连 Kaleido-OOBE → 浏览器配置
  全部密钥 → 保存重启 → `nvs_mask=0x7` 持久、MiniMax URL 从 NVS 的
  JWT 解析成功、't' 内置轮全链通（asr 430ms / llm 954ms / 首音 3530ms，
  全程零 CONFIG 后备）。设备自此运行在纯 BYOK 产品路径上。
