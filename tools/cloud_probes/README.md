# cloud_probes — 百炼语音链路探针（V-33 实测用）

复跑前提：key 明文放 `/home/emb/key/aliyun.txt`（不进仓库）；`pip install websocket-client`。

- `tts_probe.py [model voice fmt rate [out.bin]]`：WS 双向流式 TTS——task-started/首音频/总时长计时，
  服务端自动分句行为验证；默认跑 6 组合（pcm/mp3/wav × 16k/24k + 克隆）。
- `asr_probe.py [pcm_file [model]]`：把 TTS 产物按 100ms 分片喂实时 ASR，测首 partial/尾句定稿，
  打印定稿文本（sentence_end 聚合）。
- `clone_probe.py`：voice-enrollment 全流程（create→轮询 OK→list）；测试音色用后请 delete。

协议要点与结果：docs/gates/V-33-百炼语音链路实测.md（方案 v1.5 §3 的依据）。
已知坑：websocket-client 用 `recv_data()`（新版返回二元组）；TTS 逗号也断句（流式喂入勿死等
result-generated 才发下文——首 c1 后收到首个结果即补发剩余并 finish-task）。
