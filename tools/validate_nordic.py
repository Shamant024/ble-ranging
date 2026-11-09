#!/usr/bin/env python3
# Copyright (c) 2026 the ble-ranging authors
# SPDX-License-Identifier: MIT
#
# Runs the Python copy of Nordic's IFFT method on the saved raw tones and checks
# that it gives the same distance the board printed. A match on every procedure
# shows that the tone capture and the channel mapping are both correct.
#
#   python3 validate_nordic.py test2 test3

import sys

import numpy as np

from cs_estimators import delay_profile, nordic_ifft
from dataset import load

for test in sys.argv[1:] or ["test2", "test3"]:
    d = load(test)
    board = d["ifft_m"]
    match, total = 0, 0
    for i in range(len(board)):
        if not np.isfinite(board[i]) or not d["ok"][i].any():
            continue
        python_d = nordic_ifft(delay_profile(d["H"][i]))
        total += 1
        match += abs(python_d - board[i]) < 0.01
    print(f"{test}: Python result within 1 cm of the board on {match} of {total} procedures")
