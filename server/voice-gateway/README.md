# Kaleido 自托管语音网关（服务端附件）

方案 v1.5 §6 服务端附件 / 19号 §3 组合 C 的自托管覆盖位。设备固件
`kaleido_speech` 把 `url` 配成 `ws://<本机>:9100` 即切换到自托管路径，
协议零改动（V-33 语义）。

## 已验证（2026-10-25，PC 本机）

- TTS 经网关：首音频 323ms（直连 301ms，单跳代价可忽略）
- ASR 经网关：定稿文本精确回传
- 证据：本仓 docs/gates/V-33-百炼语音链路实测.md 附录；探针
  `kaleido-fw/tools/cloud_probes/`

## 运行

```bash
pip install websockets
DS_API_KEY=<你的key> GATEWAY_TOKEN=<设备侧令牌> python gateway.py
# 设备侧配置 speech url = ws://<host>:9100
```

生产建议放 NAS/Docker：

```bash
docker compose up -d   # 端口 9100；密钥走 .env
```

## 后端说明

当前上游 = DashScope 直连（复用云 key，适合"低配自托管"：省设备侧
TLS/多协议栈，语义与云一致）。二期替换 FunASR/CosyVoice 本地推理时只改
`ds_session()`，北向（设备）接口不变。
