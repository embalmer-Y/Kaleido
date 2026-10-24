# vendor 上游项目许可证与版权风险评估

| 版本 | 日期 | 作者 |
|---|---|---|
| v1.0 | 2026-10-24 | ZCode |

评估对象：`vendor/esp-claw`、`vendor/muse-gadget-sdk`（2026-10-24 经代理克隆，钉版见下）。

## 1. 钉版记录（升级须走独立提交 + 回归）

| 项目 | 仓库 | 钉版 commit | 日期 | 许可证 |
|---|---|---|---|---|
| ESP-Claw | github.com/espressif/esp-claw | `9b23c15bc28703369b4b7fad3e810f8cb535fb59` | 2026-10-22 | Apache-2.0 |
| Muse Gadget SDK | github.com/facebookincubator/muse-gadget-sdk | `c9354b7a765cfb7dd3c4f1945a9ca838d871630c` | 2026-10-23 | Apache-2.0 |

## 2. 结论：**可以引入，风险低**

两者均为 **Apache License 2.0**（许可型许可证），允许商用、修改、再分发、闭源集成，
仅要求：① 保留原 LICENSE 与版权/许可证声明；② 修改过的文件**必须显著声明已修改**；
③ 不使用上游商标名义背书；④（若上游提供 NOTICE）随分发保留 NOTICE。
**不存在 GPL 式传染义务**：Kaleido-Claw 固件可整体采用 Apache-2.0（与 constraints R3 开源义务一致，且 Apache-2.0 与我们硬件侧 CERN-OHL-S、文档侧 CC BY-SA 互不冲突——三者分仓发布）。

## 3. 义务落实（开发纪律）

1. **fork 改造处标记**：对 esp-claw / muse 源码的任何修改，文件头追加
   `// Modified by Kaleido project, 2026-xx-xx: <原因>`；优先以"新增组件包裹"而非"原地改上游文件"的方式扩展（上游文件零改动 = 义务最小）。
2. **发布时随附**：两个项目的 LICENSE 全文 + 来源与钉版 commit（本文件即登记载体，随仓库发布）。
3. **vendor 目录管理**：`vendor/` 当前为完整克隆（含 .git）；对外发布时改用
   `git submodule`（钉 commit）或导出去 .git 的源码快照 + 保留 LICENSE，二选一在 G6 前定稿。

## 4. 残余风险与注意项（不阻塞，登记在案）

| # | 风险点 | 等级 | 处置 |
|---|---|---|---|
| R-1 | **商标**："ESP-Claw"为乐鑫项目名。我们命名 Kaleido-Claw 属致敬式衍生命名，对外发布时 README 明确"based on Espressif ESP-Claw (Apache-2.0)"，不以乐鑫名义宣传 | 低 | 已写入发布清单义务 |
| R-2 | **muse 仓内含 `esp32/dev_signing_key.pem`**（上游开发签名私钥）：仅用于其 OTA 开发签名。我们**不得在量产/发布中使用该 key** 签任何东西；自有 key 独立生成 | 低 | 写入 kaleido-fw 安全注意事项 |
| R-3 | Apache-2.0 §4 要求：若上游标注 NOTICE，修改版不得移除——发布前复查两仓库根目录（当前均无 NOTICE 文件，2026-10-24 核实） | 低 | G6 发布 checklist 项 |
| R-4 | muse 云端服务（Muse 平台本身）条款与配额不受 SDK 许可证管辖——SDK 开源≠服务免费/永久；接入层做协议适配隔离，云端条款变化不波及固件架构 | 中 | 架构文档 §Muse Link 设计为可插拔后端 |
| R-5 | esp-claw 处于快速演进期（钉版距今 2 天仍有提交）：升级频率高，钉版纪律必须严格执行，防上游 breaking change | 中 | 每次升级独立提交 + 回归测试 |
| R-6 | 上游代码质量/安全：lua_modules 暴露外设给动态脚本，攻击面较大——我们引入时按 Tessera 权限位图思想收窄（架构文档 §App 权限） | 中 | 架构设计已吸收 |

## 5. 第三方传递依赖

- esp-claw 依赖：ESP-IDF 组件、Lua 5.4（Lua MIT）、（其内嵌组件各自的 permissive 许可证——首次构建时以 `idf.py` 拉取清单为准，G6 前出第三方清单）
- muse esp32 依赖：libvpx（BSD）、minimp3（CC0）、tinyh264（LGPL-2.1？**待构建时核实**——LGPL 在固件静态链接场景需注意提供目标文件或源码；当前仅 vendor 未集成，集成前必须核实并登记）

> ⚠ R-7（新增）：`tinyh264` 疑似 LGPL——若确认，静态链接需履行 LGPL §6（提供可重链接目标文件）或改用源码集成并开源该部分。集成 Muse 视频流功能前必须闭环此项。
