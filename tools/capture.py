#!/usr/bin/env python3
# Copyright (c) 2026 the ble-ranging authors
# SPDX-License-Identifier: MIT
#
# Records the initiator's UART output into CSV files.
#
#   python3 capture.py --port /dev/ttyACM0 --out run1 --mark-file mark.txt
#
# Writes four files:
#   <out>.csv      one row per procedure ($R lines)
#   <out>-iq.csv   one row per channel per procedure, the raw tones ($Q lines)
#   <out>.log      every line the board printed, with host time
#   <out>.json     board and Channel Sounding settings ($I lines) and the marks
#
# To label one long run over many distances, write "<distance_m> <label>" into
# the mark file each time the reflector is moved. Every row after that carries
# the new distance and label. Stop with Ctrl-C.

import argparse
import json
import os
import time

import serial

FIELDS = ["n", "uptime_ms", "counter", "ifft_m", "phase_slope_m", "rtt_m", "best_m", "tone_ok",
          "good_channels", "rtt_count", "rssi_dbm", "rtt_raw_m", "best_med9_m", "cal_m"]
IQ_FIELDS = ["counter", "step", "channel", "freq_mhz", "i_local", "q_local", "i_remote",
             "q_remote", "quality_local", "quality_remote"]


def pct_to_iq(field):
    # A 24 bit PCT holds I in the low 12 bits and Q in the high 12, both signed.
    i, q = field & 0xFFF, field >> 12
    return (i - 4096 if i & 0x800 else i), (q - 4096 if q & 0x800 else q)


def parse_iq(line):
    # $Q,<uptime_ms>,<counter>,<steps>,<token>,...
    # Token: channel (2 hex), local PCT (6), remote PCT (6), local and remote quality (1 each).
    parts = line.split(",")
    if len(parts) < 4 or len(parts) != int(parts[3]) + 4:
        return None
    rows = []
    for step, tok in enumerate(parts[4:]):
        if len(tok) != 16:
            return None
        ch = int(tok[0:2], 16)
        il, ql = pct_to_iq(int(tok[2:8], 16))
        ir, qr = pct_to_iq(int(tok[8:14], 16))
        rows.append([parts[2], step, ch, 2402 + ch, il, ql, ir, qr, int(tok[14], 16), int(tok[15], 16)])
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True, help="serial port of the initiator")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--out", required=True, help="output file stem")
    ap.add_argument("--mark-file", help="file holding '<distance_m> <label>'")
    args = ap.parse_args()

    meta = {"board": {}, "marks": []}
    distance, label, mark_time = "", "", None
    t0 = None

    with serial.Serial(args.port, args.baud, timeout=1) as ser, \
            open(args.out + ".csv", "w") as out, open(args.out + "-iq.csv", "w") as iq, \
            open(args.out + ".log", "w") as log:
        out.write(",".join(["time_s"] + FIELDS + ["distance_m", "label"]) + "\n")
        iq.write(",".join(["time_s"] + IQ_FIELDS + ["distance_m", "label"]) + "\n")
        print(f"Recording {args.port} to {args.out}.csv, Ctrl-C to stop")
        try:
            while True:
                raw = ser.readline()
                if not raw:
                    continue
                now = time.time()
                t0 = now if t0 is None else t0
                t = f"{now - t0:.3f}"

                if args.mark_file and os.path.exists(args.mark_file):
                    m = os.path.getmtime(args.mark_file)
                    if m != mark_time:
                        mark_time = m
                        words = open(args.mark_file).read().split()
                        if words:
                            distance = words[0]
                            label = words[1] if len(words) > 1 else f"{float(distance):.2f}m"
                            meta["marks"].append({"time_s": float(t), "distance_m": float(distance),
                                                  "label": label})
                            print(f"--- now at {distance} m ({label})")

                line = raw.decode(errors="replace").strip()
                log.write(f"{t} {line}\n")
                if line.startswith("$I,"):
                    parts = line.split(",", 3)
                    if len(parts) == 4:
                        meta["board"][parts[2]] = parts[3]
                elif line.startswith("$Q,"):
                    for r in parse_iq(line) or []:
                        iq.write(",".join([t] + [str(x) for x in r] + [distance, label]) + "\n")
                elif line.startswith("$R,"):
                    parts = line.split(",")[1:]
                    if len(parts) == len(FIELDS):
                        out.write(",".join([t] + parts + [distance, label]) + "\n")
                        print(f"#{parts[2]:>4}  ifft {parts[3]}  phase slope {parts[4]}  rtt {parts[5]}")
        except KeyboardInterrupt:
            pass

    with open(args.out + ".json", "w") as f:
        json.dump(meta, f, indent=2)


if __name__ == "__main__":
    main()
