#!/bin/bash
# 自动唤醒测试：标准音频 wake_auto.wav 播 N 次（28s 间隔让每轮排干），
# 配合串口抓取统计 WN,WAKE。板子须已 READY；PC Master 建议 55%。
# 用法：wake_test.sh [次数]   （另开串口抓取：见 README 归因一行）
N=${1:-5}
for i in $(seq 1 "$N"); do
  echo "[PLAY auto r$i] $(date +%H:%M:%S)"
  aplay -q -D plughw:0,0 "$(dirname "$0")/sample/wake_auto.wav"
  sleep 28
done
