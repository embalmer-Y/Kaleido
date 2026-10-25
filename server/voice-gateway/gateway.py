#!/usr/bin/env python3
"""Kaleido 自托管语音网关（服务端附件，方案 v1.5 §6 / 19号 §3 组合 B/C）

设备侧协议 = kaleido_speech 的 WS 协议（run/continue/finish-task + 二进制
PCM 帧，V-33 实测语义）；后端当前为 DashScope 直连（复用设备侧 key 校验），
后续可替换 FunASR/CosyVoice 本地推理（同一北向接口，设备零改动）。

设备接入：ws://<host>:9100  （鉴权 Bearer <token>，见 GATEWAY_TOKEN）
范围：ASR 上行透传 + TTS 下行透传；不含复刻管理（走云控制台/二期）。
"""
import asyncio
import json
import os
import sys
import uuid

import websockets

# asyncio websockets 默认不做 v4/v6 回退：强制 IPv4（与同步探针行为一致）
import socket as _s
_ga = _s.getaddrinfo
_s.getaddrinfo = lambda *a, **k: [r for r in _ga(*a, **k)
                                   if r[0] == _s.AF_INET]

DS_WSS = "wss://dashscope.aliyuncs.com/api-ws/v1/inference"
TOKEN = os.environ.get("GATEWAY_TOKEN", "")
DS_KEY = os.environ.get("DS_API_KEY", "")

if not DS_KEY:
    k = "/home/emb/key/aliyun.txt"  # 本地开发便利；生产用环境变量
    DS_KEY = open(k).read().strip() if os.path.exists(k) else ""


async def ds_session(task: dict) -> tuple:
    """开上游 DashScope WS 并下发 run-task，返回 (ws, task_id)。"""
    ws = await websockets.connect(
        DS_WSS, additional_headers={"Authorization": f"Bearer {DS_KEY}"})
    tid = uuid.uuid4().hex
    task = dict(task)
    task["header"] = {"action": "run-task", "task_id": tid,
                      "streaming": "duplex"}
    await ws.send(json.dumps(task))
    return ws, tid


def fwd_hdr(action: str, tid: str) -> dict:
    return {"header": {"action": action, "task_id": tid,
                       "streaming": "duplex"},
            "payload": {"input": {}}}


async def pump_up(dev, upstream, tid):
    """设备→上游：文本 continue/finish + 二进制 PCM。"""
    async for msg in dev:
        if isinstance(msg, (bytes, bytearray)):
            await upstream.send(msg)
        else:
            m = json.loads(msg)
            act = m.get("header", {}).get("action")
            if act in ("continue-task", "finish-task"):
                hm = dict(m["header"])
                hm["task_id"] = tid
                await upstream.send(json.dumps(
                    {"header": hm, "payload": m.get("payload", {})}))


async def pump_down(dev, upstream, tid):
    """上游→设备：改写 task_id 后原样转发。"""
    async for msg in upstream:
        if isinstance(msg, (bytes, bytearray)):
            await dev.send(msg)
        else:
            m = json.loads(msg)
            m.setdefault("header", {})["task_id"] = tid
            await dev.send(json.dumps(m, ensure_ascii=False))


async def handle(dev):
    auth = dev.request.headers.get("Authorization", "")
    if TOKEN and auth != f"Bearer {TOKEN}":
        await dev.close(code=4401, reason="bad token")
        return
    first = json.loads(await dev.recv())
    pl = first.get("payload", {})
    task = pl.get("task")
    if task not in ("asr", "tts"):
        await dev.close(code=4400, reason="task must be asr|tts")
        return
    upstream, tid = await ds_session({
        "payload": {
            "task_group": "audio", "task": task,
            "function": "recognition" if task == "asr"
                        else "SpeechSynthesizer",
            "model": pl.get("model", ""),
            "parameters": pl.get("parameters", {}),
            "input": {},
        }})
    up_task = asyncio.create_task(pump_up(dev, upstream, tid))
    down_task = asyncio.create_task(pump_down(dev, upstream, tid))
    # S5：任一泵结束（含干净返回）即收尾，避免对端悬置
    done, pending = await asyncio.wait(
        [up_task, down_task], return_when=asyncio.FIRST_COMPLETED)
    for t in pending:
        t.cancel()
    await upstream.close()
    await dev.close()
    print(f"[gw] session {task} tid={tid[:8]} done: "
          f"{[type(d.exception()).__name__ if d.exception() else 'ok' for d in done]}",
          file=sys.stderr)


async def main():
    if not TOKEN and os.environ.get("KALEIDO_GW_INSECURE") != "1":
        print("[gw] refusing to start: GATEWAY_TOKEN empty "
              "(set it, or KALEIDO_GW_INSECURE=1 for LAN tests)",
              file=sys.stderr)
        return
    async with websockets.serve(handle, "0.0.0.0", 9100,
                                max_size=1 << 20):
        print("[gw] kaleido voice-gateway on :9100", file=sys.stderr)
        await asyncio.Future()


if __name__ == "__main__":
    asyncio.run(main())
