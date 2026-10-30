#!/usr/bin/env python3
"""PC 侧语音合成：MiniMax t2a_v2 → WAV（板子同款音色 female-tianmei）。
用途：PC 扬声器播放替代人工语音配合（唤醒测试/ASR 测试）。"""
import json, base64, sys, urllib.request

KEY = open('/home/emb/key/minimax.txt').read().strip()

def group_id(jwt):
    payload = jwt.split('.')[1]
    payload += '=' * (-len(payload) % 4)
    dec = base64.urlsafe_b64decode(payload).decode()
    return json.loads(dec)['GroupID']

def synth(text, out, vol=80):
    url = f"https://api.minimaxi.com/v1/t2a_v2?GroupId={group_id(KEY)}"
    body = json.dumps({
        "model": "speech-2.5-hd-preview",
        "text": text,
        "stream": False,
        "voice_setting": {"voice_id": "female-tianmei", "vol": vol},
        "audio_setting": {"sample_rate": 24000, "bit_rate": 64000,
                          "format": "wav", "channel": 1},
    }).encode()
    req = urllib.request.Request(url, data=body, headers={
        "Authorization": f"Bearer {KEY}",
        "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=30) as r:
        resp = json.load(r)
    hexdata = resp["data"]["audio"]
    wav = bytes.fromhex(hexdata)
    open(out, 'wb').write(wav)
    print(f"wrote {out} {len(wav)}B status={resp['base_resp']['status_msg']}")

if __name__ == '__main__':
    synth(sys.argv[1], sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 80)
