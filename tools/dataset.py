# Copyright (c) 2026 the ble-ranging authors
# SPDX-License-Identifier: MIT
#
# Reads one test folder of the dataset (ranging.csv, tones.csv, placements.csv).

import csv
import os
from collections import defaultdict

import numpy as np

from cs_estimators import combined_tones

HERE = os.path.dirname(os.path.abspath(__file__))
DATASET = os.path.join(HERE, "..", "dataset")

# Every placement is scored on the same window: 10 s that start 3 s after the
# reflector was put down, so the person placing it has moved away.
SETTLE_S = 3.0
WINDOW_S = 10.0


def _f(v):
    return float(v) if v not in ("", "nan") else np.nan


def load(test):
    folder = os.path.join(DATASET, test)
    rows = list(csv.DictReader(open(os.path.join(folder, "ranging.csv"))))
    place = list(csv.DictReader(open(os.path.join(folder, "placements.csv"))))
    t = np.array([_f(r["time_s"]) for r in rows])
    d = {k: np.array([_f(r[k]) for r in rows]) for k in ("ifft_m", "phase_slope_m", "rtt_m")}
    d["time_s"] = t
    d["counter"] = [r["counter"] for r in rows]

    # Which placement each procedure belongs to, and whether it is in the window.
    d["placement"] = np.full(len(t), -1)
    d["in_window"] = np.zeros(len(t), bool)
    d["truth_m"] = np.full(len(t), np.nan)
    for j, p in enumerate(place):
        start, end = float(p["start_s"]), float(p["end_s"])
        inside = (t >= start) & (t < end)
        d["placement"][inside] = j
        d["truth_m"][inside] = float(p["distance_m"])
        d["in_window"] |= inside & (t >= start + SETTLE_S) & (t < start + SETTLE_S + WINDOW_S)
    d["placements"] = place

    # Combined tones per procedure, matched to ranging rows by procedure counter.
    tones_path = os.path.join(folder, "tones.csv")
    if os.path.exists(tones_path):
        by_counter = defaultdict(list)
        for r in csv.DictReader(open(tones_path)):
            by_counter[r["counter"]].append(r)
        H = np.zeros((len(rows), 75), complex)
        ok = np.zeros((len(rows), 75), bool)
        for i, c in enumerate(d["counter"]):
            if c in by_counter:
                H[i], ok[i] = combined_tones(by_counter[c])
        d["H"], d["ok"] = H, ok
    return d
