#!/usr/bin/env python3
# Copyright (c) 2026 the ble-ranging authors
# SPDX-License-Identifier: MIT
#
# Compares the distance methods on tests 2 and 3. Every placement is scored on
# the same 10 s window. The fixed offset for "MUSIC + offset" is always learned on
# the other test, so no test is scored with an offset fitted on itself.
#
#   python3 evaluate.py

import numpy as np

from cs_estimators import delay_profile, fill_gaps, first_peak, music
from dataset import load

STACK = 10   # procedures combined into one MUSIC estimate, about 2 s


def methods(d):
    n = len(d["time_s"])
    out = {"Nordic IFFT (board)": d["ifft_m"], "Phase slope (board)": d["phase_slope_m"],
           "Round trip time (board)": d["rtt_m"]}
    low_rule = np.full(n, np.nan)
    mus = np.full(n, np.nan)
    filled = {}
    for i in np.where(d["in_window"])[0]:
        low_rule[i] = first_peak(delay_profile(d["H"][i]), 0.2)
        # This procedure and up to STACK - 1 before it, at the same placement.
        snaps = []
        for j in range(i - STACK + 1, i + 1):
            if j < 0 or d["placement"][j] != d["placement"][i]:
                continue
            if j not in filled:
                filled[j] = fill_gaps(d["H"][j], d["ok"][j])
            if filled[j] is not None:
                snaps.append(filled[j])
        if snaps:
            mus[i] = music(np.array(snaps))
    out["IFFT, 20% rule"] = low_rule
    out["MUSIC, 10 stacked"] = mus
    return out


def offset(d, est):
    w = d["in_window"]
    return np.nanmedian(est[w] - d["truth_m"][w])


def row(name, d, est):
    e = np.abs(est - d["truth_m"])[d["in_window"]]
    e = e[np.isfinite(e)]
    print(f"  {name:28} median {np.median(e):5.2f} m   within 1 m {np.mean(e < 1) * 100:4.0f}%"
          f"   within 0.5 m {np.mean(e < 0.5) * 100:4.0f}%")


data = {t: load(t) for t in ("test2", "test3")}
res = {t: methods(d) for t, d in data.items()}
for t, other in (("test2", "test3"), ("test3", "test2")):
    d = data[t]
    print(f"\n{t}: {d['in_window'].sum()} procedures in {len(d['placements'])} placements")
    for name, est in res[t].items():
        row(name, d, est)
    mus_other = res[other]["MUSIC, 10 stacked"]
    row(f"MUSIC + offset from {other}", d,
        res[t]["MUSIC, 10 stacked"] - offset(data[other], mus_other))
