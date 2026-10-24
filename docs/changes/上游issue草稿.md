# 上游 issue 草拟稿（2026-10-24，**已全部发布 ✅**）

> 发布账号：embalmer-Y（负责人 GitHub 账号，经其提供的 classic token 由 ZCode 代发）。
> **发布回执**：
> 1. esp-claw **issue #183**（W-1 首发）：https://github.com/espressif/esp-claw/issues/183
> 2. esp-claw **PR#157 评论**（W-3 复现数据+修复证实）：https://github.com/espressif/esp-claw/pull/157#issuecomment-6385151348
> 3. esp-idf **#18791 评论**（backport 请求）：https://github.com/espressif/esp-idf/issues/18791#issuecomment-6385151524
>
> ⚠ 安全提醒：代发所用 token 为全权限 classic token，**负责人应在确认发布完成后立即吊销**（GitHub → Settings → Developer settings → Tokens (classic) → Delete）。
> 以下为发布原文存档。

---

## 草稿一：esp-claw 新 issue（W-1，首发）

**Title**: `[bug] Board resource generator rejects registry board defs using `adc` peripherals (e.g. esp32_s31_korvo_1)`

**Body**:

### Checklist
- [x] Searched for existing issues (none found for adc / korvo / board generator)
- [x] Reproduced with current master (9b23c15, 2026-10-22)

### Environment
- ESP-Claw: master @ `9b23c15`, IDF v6.1 (esp32s31, preview)
- Board: `esp32_s31_korvo_1` (from managed component `espressif/esp_boards`)
- Host: Linux

### Description
Building `application/edge_agent` with `idf.py bmgr -b esp32_s31_korvo_1` fails at configure time:

```
CMake Error at components/common/app_claw/CMakeLists.txt:26 (message):
  Board resource generation failed: Hardware Resource generation failed:
  adc_oneshot: unsupported standard peripheral type 'adc'
```

Root cause: `components/common/app_claw/tools/generate_board_resources.py` handles a fixed set of peripheral kinds (`i2c/spi/uart/ledc/rmt/sdm/dsi/ldo/gpio...`) but not `adc`. The registry board definition `managed_components/espressif__esp_boards/esp32_s31_korvo_1/board_peripherals.yaml` defines:

```yaml
  - name: adc_oneshot
    type: adc
    role: oneshot
```

(referenced by the `adc_button_group` device in `board_devices.yaml`).

Notably, the in-repo copy at `application/edge_agent/boards/espressif/esp32_s31_korvo1/board_devices.yaml` has the ADC button group **commented out**, so the two board sources are inconsistent — the repo copy works around the generator limitation while the registry version does not. Also note `idf.py bmgr -c ./boards` (from AGENTS.md) is no longer a valid option in bmgr 0.6.2, so pointing bmgr at the in-repo copy isn't available as a workaround.

### Suggested fixes (either side)
1. `generate_board_resources.py`: add `adc` to the supported peripheral kinds, or skip-with-warning unknown standard types instead of failing; or
2. `espressif/esp_boards`: comment out the ADC entries for this board until supported (matching the in-repo copy).

Workaround we currently use: comment out `adc_oneshot` in `board_peripherals.yaml` and `adc_button_group` in `board_devices.yaml` of the managed component (loses the GPIO42 ADC buttons).

---

## 草稿二：esp-claw #157 评论（W-3 复现数据）

**Post to**: https://github.com/espressif/esp-claw/pull/157

**Body**:

+1 — we hit what looks like exactly the first bug on **ESP32-S31** (Korvo-1 V1.1, IDF v6.1, esp-claw @ 9b23c15), before applying this PR:

1. Configure any LLM (openai_compatible), reboot, send one WebIM message.
2. `claw_mem_extrac` stops feeding the task watchdog; `IDLE0` starves; no agent reply is ever produced (message stays queued, run_id allocated).

```
E (155927) task_wdt: Task watchdog got triggered...
E (155927) task_wdt:  - IDLE0 (CPU 0)
E (155927) task_wdt: CPU 0: claw_mem_extrac
Core  0 register dump:
MEPC    : 0x502bfba2  RA : 0x5027817a  SP : 0x505f5100 ...
```

PC/SP are in PSRAM regions, consistent with a PSRAM-backed task stack being hit around flash-cache-disabled file I/O, as described in this PR.

We've applied this PR locally (on top of 9b23c15) and are re-testing; will report back. Thanks for the solid root-cause write-up — any plan to merge soon?

---

## 草稿三：esp-idf #18791 评论（请求 backport）

**Post to**: https://github.com/espressif/esp-idf/issues/18791

**Body**:

This exact failure still reproduces on the **v6.1 release line** — the fix `c566ace1f` appears to be on master only, not backported to `release/v6.1` (verified via branch compare; our checkout is the `v6.1` tag `fff9895c`).

Repro: ESP32-S31 + IDF v6.1 + an app with `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y` (+ WiFi, so `.rodata_wlog_*` is non-empty) fails at link with:

```
ld: sections.ld:1357 cannot move location counter backwards (from 50420d50 to 5041c700)
```

(esp-claw edge_agent @ esp32_s31_korvo_1 board is a public reproducer.)

Would it be possible to backport `c566ace1f` (and `22cc8d41c` if applicable) to `release/v6.1`? We've applied the patch locally on v6.1 and it links cleanly. Thanks!

---
