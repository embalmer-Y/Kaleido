# Kaleido C 编码规范（Linux 内核风格 · ESP-IDF 适配版）

| 版本 | 日期 | 依据 |
|---|---|---|
| v1.0 | 2026-10-25 | constraints.md §6.3（v0.3，负责人指示）；基准 Documentation/process/coding-style.rst（Linux 内核） |

**事实源声明**：本文是 Kaleido 全部 C 代码的编码规范事实源；与内核文档冲突处以本文为准（适配项均已标注"豁免"）。
工具链：仓库根 `.clang-format`（内核预设）由 CI 强制；`clang-format -i` 全量格式化为提交前置。

---

## 1. 排版（clang-format 自动执行）

- **Tab 缩进，宽度 8**（`UseTab: ForContinuationAndIndentation`）；空格仅用于对齐续行。
- **80 列硬上限**（`ColumnLimit: 80`）。
- 花括号（`BreakBeforeBraces: Linux`）：
  - **函数**：左花括号另起一行；
  - `if/else/for/while/switch`：左花括号同行；`} else {` 同行衔接；
  - 单语句不加括号（允许多行单语句裸写）。
- 指针声明星号贴近变量名：`struct foo *bar;`
- `case` 与 `switch` 同列（`IndentCaseLabels: false`）。

## 2. 注释

- **只用 `/* */`**，禁止 `//`（checkpatch 规则）。
- 多行注释：首行与尾行留空内容：
  ```c
  /*
   * 这是多行注释的正文。
   */
  ```
- 注释写**为什么**，不写"这行在干什么"；文件头保留版权/SPDX 与一句话用途。

## 3. 命名

- 全局函数/变量/类型：`snake_case`；**模块前缀**：`kd_audio_*`、`kapp_*`（新模块用 `kd_`）。
- 宏与枚举常量：`UPPER_CASE`。
- 局部变量：短而清晰（`i`/`ret` 允许）；杜绝混合大小写与匈牙利记法。
- **新代码禁新增 typedef**（内核规则）：用 `struct name`、`enum name` 直接声明；
  **豁免清单**（既有公共 API 兼容保留，逐步消除）：
  - `kaleido_app.h` 的 `kapp_manifest_t`/`kapp_app_t`/`kperm_bit_t` 等 M0 遗留 typedef；
  - ESP-IDF/组件头文件自带的 typedef（`esp_err_t`、`i2s_chan_handle_t` 等外部类型不受本规范管辖）。

## 4. 函数与控制流

- 函数短小（一屏内为宜，硬上限 ~60 行）；单一职责；静态函数先于使用处定义，避免无谓原型。
- **单出口原则**：错误清理用 `goto out;` 模式（内核惯例），禁止多 return 交错深嵌套：
  ```c
  int kd_foo(struct dev *d)
  {
      int ret;

      ret = step_one(d);
      if (ret)
          goto out;
      ret = step_two(d);
  out:
      return ret;
  }
  ```
- 优先 `enum` 定义相关整数常量集合；标志位用位域宏。
- 禁止在头文件中定义函数与变量（`static inline` 例外）。

## 5. 头文件

- 头文件守卫用 `#ifndef _KD_XXX_H` 形式（内核惯例）；**豁免**：既有文件的 `#pragma once` 保留，新文件一律用守卫。
- include 顺序：本文件对应头 → 本组件头 → 同仓组件头 → ESP-IDF 头 → 第三方/标准库（组间空行）。

## 6. ESP-IDF 适配豁免（明示记录）

| 项 | 内核风格 | Kaleido 决定 |
|---|---|---|
| 返回值 | 负 errno（`-EINVAL`） | **沿用 `esp_err_t`**（IDF 生态接口一致性；模块内部可用布尔/errno 风格） |
| 日志 | `pr_info`/`dev_dbg` | **`ESP_LOGx`**（IDF 日志系统） |
| 错误指针 | `ERR_PTR` | `NULL` + `esp_err_t` 出参（IDF 惯例） |
| 并发原语 | lockdep/rwlock | FreeRTOS `SemaphoreHandle_t`/`portMUX` |
| 容器 | `list_head` 循环链表 | 优先数组/固定池（无动态分配原则不变） |

## 7. 提交纪律（与 constraints §6.4 联动）

- 提交前 `clang-format -i` 全量格式化；格式化单独提交，不与逻辑变更混排。
- 一次提交一件事；Commit message 首行 ≤50 字符、祈使语气、正文说清 why。
- 禁止提交时夹带 reformat 与重构之外的功能改动（反之亦然）。

## 8. 存量迁移

- M0 已有代码（kaleido_app 等）已于 2026-10-25 完成一次性内核风格迁移（纯格式化提交，构建回归通过）；
- HIL 测试工程（tools/hil/）同规范执行；
- 后续新组件（kaleido_audio/kaleido_ui/…）自第一行起遵守。
