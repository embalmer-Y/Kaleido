#!/usr/bin/env python3
"""Capture serial log with wall-clock timestamps (V-32 style: <<HH:MM:SS.mmm>>).

Usage: capture_serial.py PORT BAUD OUTFILE [STOP_STRING [TIMEOUT_S]]
Stops (exit 0) after seeing STOP_STRING plus 20s grace, or TIMEOUT_S (default 900).
"""
import sys, time

import serial  # pyserial, present in IDF python env


def main():
    port, baud, outfile = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    stop = sys.argv[4] if len(sys.argv) > 4 else "SB,ALL,DONE"
    timeout_s = float(sys.argv[5]) if len(sys.argv) > 5 else 900.0
    t_end = time.time() + timeout_s
    with open(outfile, "w", buffering=1) as f, serial.Serial(port, baud, timeout=1) as s:
        f.write("# capture start %s port=%s baud=%d stop=%r\n" % (time.strftime("%F %T"), port, baud, stop))
        stopped_at = None
        while time.time() < t_end:
            line = s.readline()
            if not line:
                if stopped_at is not None and time.time() - stopped_at > 20:
                    break
                continue
            ts = time.strftime("%H:%M:%S") + ".%03d" % (int(time.time() * 1000) % 1000)
            try:
                text = line.decode("utf-8", "replace").rstrip("\r\n")
            except Exception:
                text = repr(line)
            f.write("<<%s>> %s\n" % (ts, text))
            if stop and stop in text:
                if stopped_at is None:
                    stopped_at = time.time()
                if time.time() - stopped_at > 20:
                    break
        f.write("# capture end %s\n" % time.strftime("%F %T"))


if __name__ == "__main__":
    main()
