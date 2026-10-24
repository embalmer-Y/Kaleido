# vendor 上游仓库复原说明

主仓不收录 `vendor/`（两个上游克隆各有 .git）。按本文可完整复原已验证的钉版状态。

## 复原命令（需代理时前置 `https_proxy=http://127.0.0.1:7890`）

```bash
mkdir -p vendor && cd vendor

# 1) esp-claw：钉版 9b23c15 + 两个本地补丁提交
git clone https://github.com/espressif/esp-claw.git
cd esp-claw && git checkout 9b23c15bc28703369b4b7fad3e810f8cb535fb59
# 补丁 A（W-3）：应用 PR#157（PSRAM 栈×flash cache 崩溃修复）
#   curl -L https://github.com/espressif/esp-claw/pull/157.diff | git apply
#   git commit -am "apply PR#157（对应本地提交 3177144）"
# 补丁 B（W-4）：application/edge_agent/sdkconfig.defaults 追加
#   CONFIG_MBEDTLS_SSL_PROTO_TLS1_3=y
#   （对应本地提交 3ebb75e）
cd ..

# 2) muse-gadget-sdk：钉版 c9354b7（未打补丁）
git clone https://github.com/facebookincubator/muse-gadget-sdk.git
cd muse-gadget-sdk && git checkout c9354b7a765cfb7dd3c4f1945a9ca838d871630c
```

## 运行时还会被改动的文件（每次 reconfigure 后需重打，均已在 docs/gates/M1 记录）

- `vendor/esp-claw/application/edge_agent/managed_components/espressif__esp_boards/esp32_s31_korvo_1/board_peripherals.yaml`：注释 adc_oneshot 块（W-1，上游 issue #183 已报）
- 同目录 `board_devices.yaml`：注释 adc_button_group 块

## 许可证义务

两仓库均 Apache-2.0：发布时随附各自 LICENSE（详见 docs/vendor-licenses.md）。
