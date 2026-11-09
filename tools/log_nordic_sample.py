#!/usr/bin/env python3
# Copyright (c) 2026 the ble-ranging authors
# SPDX-License-Identifier: MIT
#
# Records the distance lines printed by Nordic's unchanged ras_initiator sample.
# That sample prints a median of recent results, not every procedure.
#
#   python3 log_nordic_sample.py --port /dev/ttyACM0 --out nordic --mark-file mark.txt

import argparse
import os
import re
import time

import serial

LINE = re.compile(r"ifft: ([-\d.na]+), phase_slope: ([-\d.na]+), rtt: ([-\d.na]+)")

ap = argparse.ArgumentParser()
ap.add_argument("--port", required=True)
ap.add_argument("--out", required=True, help="output file stem")
ap.add_argument("--mark-file", help="file holding '<distance_m> <label>'")
args = ap.parse_args()

distance, label, mark_time, t0 = "", "", None, None
with serial.Serial(args.port, 115200, timeout=1) as ser, \
        open(args.out + ".csv", "w") as out, open(args.out + ".log", "w") as log:
    out.write("time_s,ifft_m,phase_slope_m,rtt_m,distance_m,label\n")
    try:
        while True:
            if args.mark_file and os.path.exists(args.mark_file):
                m = os.path.getmtime(args.mark_file)
                if m != mark_time:
                    mark_time = m
                    words = open(args.mark_file).read().split()
                    if len(words) == 2:
                        distance, label = words
            line = ser.readline().decode(errors="replace").rstrip()
            if not line:
                continue
            now = time.time()
            t0 = now if t0 is None else t0
            log.write(f"{now - t0:.3f} {line}\n")
            hit = LINE.search(line)
            if hit:
                out.write(f"{now - t0:.3f},{hit[1]},{hit[2]},{hit[3]},{distance},{label}\n")
                out.flush()
    except KeyboardInterrupt:
        pass
