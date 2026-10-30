#!/usr/bin/env python3
# 入库版（缺陷 H 结案 §4）：复现器=宿主满载+本脚本；分类 pre_app/in_app/ok
"""缺陷H定位采样：每轮 RTS 复位后观察25s，按崩溃位置分类并提取MEPC。
位置分类：
  pre_app  = Guru 出现时还没有任何 VL, 横幅（app_main 尚未执行）
  in_app   = 有 VL, 横幅但未到 VL,READY（初始化中途死）
  ok       = 到达 VL,READY
  silent   = 无输出
"""
import sys, time, re, serial

CYCLES = 5
WATCH_S = 25
LOG = "/tmp/diagH_last.log"

def classify(txt):
    mepcs = re.findall(r"MEPC[: ]+(0x[0-9a-fA-F]+)", txt)
    gurus = txt.count("Guru Meditation")
    reached_vl = "VL," in txt
    ready = "VL,READY" in txt or "VL,DIAG" in txt and "beat" in txt
    # 诊断构建没有 VL,READY，用 VL,DIAG,beat 判活
    if gurus == 0 and ready:
        pos = "ok"
    elif gurus == 0 and not reached_vl:
        pos = "silent" if len(txt.strip()) == 0 else "ok_noguru"
    elif not reached_vl:
        pos = "pre_app"
    else:
        pos = "in_app"
    return pos, gurus, mepcs

s = serial.Serial("/dev/ttyACM0", 115200, timeout=0.5)
full = []
summary = []
for cyc in range(CYCLES):
    s.setDTR(False); s.setRTS(True); time.sleep(0.1); s.setRTS(False)
    buf = ""
    t_end = time.time() + WATCH_S
    while time.time() < t_end:
        d = s.read(4096)
        if d:
            t = d.decode("utf-8", "replace")
            buf += t
    pos, gurus, mepcs = classify(buf)
    # 每轮最后一个 VL, 阶段标记（死在哪个横幅后）
    stages = re.findall(r"(VL,[A-Z_]+)", buf)
    last_stage = stages[-1] if stages else "-"
    summary.append((cyc, pos, gurus, last_stage, mepcs))
    full.append("==== cycle %d %s gurus=%d last=%s mepcs=%s ====\n%s" %
                (cyc, pos, gurus, last_stage, ",".join(mepcs) or "-", buf))
    print("# cycle %d: %-8s gurus=%d last_stage=%s mepcs=%s" %
          (cyc, pos, gurus, last_stage, ",".join(mepcs) or "-"), flush=True)

cnt = {}
for _, pos, g, _, _ in summary:
    cnt[pos] = cnt.get(pos, 0) + 1
allmepc = [m for _, _, _, _, ms in summary for m in ms]
print("# SUMMARY", cnt, "total_gurus=%d" % sum(g for _, _, g, _, _ in summary),
      "mepcs=%s" % ",".join(allmepc), flush=True)
with open(LOG, "w") as f:
    f.write("\n".join(full))
s.close()
