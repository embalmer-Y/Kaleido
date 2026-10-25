import json, time, uuid, sys
import websocket
from websocket import WebSocketTimeoutException

KEY = open('/home/emb/key/aliyun.txt').read().strip()
URL = "wss://dashscope.aliyuncs.com/api-ws/v1/inference"
TEXT = "今天天气真不错，我们去公园走走吧。对了，你上次说的那个项目，后来进展怎么样了？"

def frame(action, tid, text=None):
    p = {"header":{"action":action,"task_id":tid,"streaming":"duplex"},"payload":{"input":{}}}
    if text is not None: p["payload"]["input"]["text"] = text
    return json.dumps(p)

def run(model, voice, fmt, srate, out=None, text=TEXT):
    tag = f"{model}/{voice}/{fmt}@{srate}"
    tid = uuid.uuid4().hex
    t, audio, err, first_audio = {}, bytearray(), None, None
    mid = text.find("，", 6) + 1
    c1, c2 = text[:mid], text[mid:]
    ws = websocket.create_connection(URL, header=[f"Authorization: Bearer {KEY}"], timeout=30)
    t['conn'] = time.perf_counter()
    ws.send(json.dumps(
        {"header":{"action":"run-task","task_id":tid,"streaming":"duplex"},
         "payload":{"task_group":"audio","task":"tts","function":"SpeechSynthesizer","model":model,
                    "parameters":{"text_type":"PlainText","voice":voice,"format":fmt,
                                  "sample_rate":srate,"volume":50,"rate":1,"pitch":1},"input":{}}}))
    sent_all = False; sents = 0
    while True:
        ws.settimeout(20 if sent_all else 1.5)
        try:
            op, data = ws.recv_data()
        except WebSocketTimeoutException:
            if not sent_all:
                ws.send(frame("continue-task", tid, c2))
                ws.send(frame("finish-task", tid))
                t['c2'] = time.perf_counter(); sent_all = True; continue
            err = "client-recv-timeout"; break
        now = time.perf_counter()
        if op == websocket.ABNF.OPCODE_BINARY:
            if first_audio is None: t['fa'] = now; first_audio = now
            audio += data
            continue
        h = json.loads(data)['header']; ev = h.get('event')
        if ev == 'task-started':
            t['started'] = now
            ws.send(frame("continue-task", tid, c1)); t['c1'] = time.perf_counter()
        elif ev == 'result-generated':
            if json.loads(data)['payload'].get('output',{}).get('type') == 'sentence-end': sents += 1
            if not sent_all:   # 首个结果即补发剩余文本并收尾（服务端对逗号也会断句）
                ws.send(frame("continue-task", tid, c2))
                ws.send(frame("finish-task", tid))
                t['c2'] = time.perf_counter(); sent_all = True
        elif ev == 'task-finished':
            t['fin'] = now; break
        elif ev == 'task-failed':
            err = f"{h.get('error_code')}: {h.get('error_message')}"; break
    ws.close()
    if err or 'fin' not in t:
        print(f"TTS,{tag},FAIL,{err or 'no-finish'},bytes={len(audio)}"); return False
    est = f",audio_est_s={len(audio)/2/srate:.2f}" if fmt=='pcm' else ""
    fn = out or f"/tmp/v33/tts_{fmt}_{srate}_{voice}.bin"
    open(fn,'wb').write(bytes(audio))
    d = lambda a,b: (t[b]-t[a])*1e3
    print(f"TTS,{tag},OK,bytes={len(audio)},task_started={d('conn','started'):.0f}ms,"
          f"FIRST_AUDIO_after_c1={d('c1','fa'):.0f}ms,total={d('c1','fin'):.0f}ms,"
          f"sentences={sents}{est},file={fn}")
    return True

if __name__ == '__main__':
    if len(sys.argv) > 3:
        run(sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]),
            out=sys.argv[5] if len(sys.argv) > 5 else None)
    else:
        for c in [("cosyvoice-v3-flash","longanyang","pcm",16000),
                  ("cosyvoice-v3-flash","longanyang","pcm",24000),
                  ("cosyvoice-v3-flash","longanyang","mp3",22050),
                  ("cosyvoice-v3-flash","longanyang","wav",16000),
                  ("cosyvoice-v2","longxiaochun","pcm",16000),
                  ("qwen-tts-realtime","loongstella","pcm",24000)]:
            try: run(*c)
            except Exception as e: print(f"TTS,{c},EXC,{type(e).__name__}:{e}")
