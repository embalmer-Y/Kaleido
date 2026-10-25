import json, time, base64, wave, io, sys
import urllib.request

KEY = open('/home/emb/key/aliyun.txt').read().strip()
LEGACY = "https://dashscope.aliyuncs.com/api/v1/services/audio/tts/customization"

# TTS 产物 → wav 容器
pcm = open('/tmp/v33/tts_pcm_16000_longanyang.bin','rb').read()
buf = io.BytesIO()
with wave.open(buf, 'wb') as w:
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(16000)
    w.writeframes(pcm)
b64 = base64.b64encode(buf.getvalue()).decode()
audio_uri = f"data:audio/wav;base64,{b64}"

def call(payload, url=LEGACY):
    req = urllib.request.Request(url, data=json.dumps(payload).encode(),
        headers={"Authorization": f"Bearer {KEY}", "Content-Type": "application/json"})
    t0 = time.perf_counter()
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            body = json.loads(r.read())
            return r.status, body, (time.perf_counter()-t0)*1e3
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b'{}'), (time.perf_counter()-t0)*1e3

# 1) create_voice（旧域名）
st, body, ms = call({"model":"voice-enrollment","input":{
    "action":"create_voice","target_model":"cosyvoice-v3-flash","prefix":"kaleido",
    "audio":audio_uri,"language_hints":["zh"]}})
print(f"CREATE,legacy,http={st},{ms:.0f}ms,{json.dumps(body,ensure_ascii=False)[:300]}")
vid = body.get("output",{}).get("voice_id")
if not vid:
    sys.exit(0)
# 2) 轮询 query_voice
for i in range(30):
    time.sleep(4)
    st, body, ms = call({"model":"voice-enrollment","input":{"action":"query_voice","voice_id":vid}})
    status = body.get("output",{}).get("status")
    print(f"QUERY,{i},http={st},{ms:.0f}ms,status={status}")
    if status in ("OK","UNDEPLOYED"): break
# 3) list_voice 核对
st, body, ms = call({"model":"voice-enrollment","input":{"action":"list_voice","page_size":20}})
print(f"LIST,http={st},{ms:.0f}ms,count={len(body.get('output',{}).get('voice_list',[]))}")
open('/tmp/v33/clone_voice_id.txt','w').write(vid or '')
