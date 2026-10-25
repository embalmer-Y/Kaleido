import json, time, uuid, sys, threading
import websocket
from websocket import WebSocketTimeoutException

KEY = open('/home/emb/key/aliyun.txt').read().strip()
URL = "wss://dashscope.aliyuncs.com/api-ws/v1/inference"
PCM = open(sys.argv[1] if len(sys.argv) > 1 else '/tmp/v33/tts_pcm_16000_longanyang.bin','rb').read()
MODEL = sys.argv[2] if len(sys.argv) > 2 else 'paraformer-realtime-v2'

tid = uuid.uuid4().hex
t = {}
texts, first_partial, err = [], None, None
last_chunk_sent = [None]

ws = websocket.create_connection(URL, header=[f"Authorization: Bearer {KEY}"], timeout=30)
t['conn'] = time.perf_counter()
ws.send(json.dumps({"header":{"action":"run-task","task_id":tid,"streaming":"duplex"},
  "payload":{"task_group":"audio","task":"asr","function":"recognition","model":MODEL,
             "parameters":{"format":"pcm","sample_rate":16000},"input":{}}}))

def feeder():
    CHUNK = 3200
    for i in range(0, len(PCM), CHUNK):
        time.sleep(0.1)
        try:
            ws.send_binary(PCM[i:i+CHUNK])
        except Exception:
            return
        last_chunk_sent[0] = time.perf_counter()
    time.sleep(0.2)
    try:
        ws.send(json.dumps({"header":{"action":"finish-task","task_id":tid,"streaming":"duplex"},
                            "payload":{"input":{}}}))
    except Exception:
        pass

th = threading.Thread(target=feeder, daemon=True)
th.start()
while True:
    ws.settimeout(25)
    try:
        op, data = ws.recv_data()
    except WebSocketTimeoutException:
        err = "client-recv-timeout"; break
    now = time.perf_counter()
    if op == websocket.ABNF.OPCODE_BINARY:
        continue
    h = json.loads(data)['header']
    ev = h.get('event')
    if ev == 'task-started':
        t['started'] = now
    elif ev == 'result-generated':
        if 'first_res' not in t:
            t['first_res'] = now
        out = json.loads(data)['payload'].get('output', {})
        txt = out.get('sentence', {}).get('text', '')
        if txt:
            if first_partial is None:
                first_partial = (now, txt)
            texts.append((out.get("sentence",{}).get("sentence_end",False), txt))
    elif ev == 'task-finished':
        t['fin'] = now; break
    elif ev == 'task-failed':
        err = f"{h.get('error_code')}: {h.get('error_message')}"; break
ws.close()
if err or 'fin' not in t:
    print(f"ASR,{MODEL},FAIL,{err or 'no-finish'}")
    sys.exit(0)
full = ''.join(x[1] for x in texts if x[0])
d = lambda a, b: (t[b] - t[a]) * 1e3
fp = f"{(first_partial[0]-t['started'])*1e3:.0f}ms({first_partial[1][:12]})" if first_partial else "-1"
print(f"ASR,{MODEL},OK,pcm={len(PCM)}B/{len(PCM)/32000:.1f}s,"
      f"task_started={d('conn','started'):.0f}ms,first_partial_after_start={fp},"
      f"final_after_lastchunk={(t['fin']-last_chunk_sent[0])*1e3:.0f}ms,events={len(texts)}")
print(f"ASR_TEXT_FINAL,{full}")
