# Copyright (c) 2026 the ble-ranging authors
# SPDX-License-Identifier: MIT
#
# Distance estimators that work on the raw Channel Sounding tones.
#
# A procedure gives one complex tone per channel from each board. Multiplying
# the initiator's tone by the reflector's tone cancels the random phase of both
# radios and leaves the round trip phase. That product is called H here, one
# value per channel, 75 slots from 2404 MHz to 2478 MHz in 1 MHz steps. The
# slots 2425, 2426 and 2427 MHz are never used by Channel Sounding and stay 0.
#
# For one path of length d, H[n] = a * exp(-j * 2 * pi * n * 1 MHz * 2d / c).

import numpy as np

C = 299792458.0
NUM_CH = 75
DF = 1e6
FIRST_MHZ = 2404


def combined_tones(rows):
    # rows: tone rows of one procedure, as dicts with the tones.csv columns.
    # Only tones that both radios marked as good (quality 0) are kept.
    h = np.zeros(NUM_CH, complex)
    ok = np.zeros(NUM_CH, bool)
    for r in rows:
        if int(r["quality_local"]) != 0 or int(r["quality_remote"]) != 0:
            continue
        k = int(r["freq_mhz"]) - FIRST_MHZ
        loc = complex(int(r["i_local"]), int(r["q_local"]))
        rem = complex(int(r["i_remote"]), int(r["q_remote"]))
        h[k] = loc * rem
        ok[k] = True
    return h, ok


def delay_profile(h, nfft=2048):
    # Zero padded inverse FFT. Each path shows up as a peak at its distance.
    x = np.zeros(nfft, complex)
    x[:NUM_CH] = h
    return np.abs(np.fft.ifft(x))


def nordic_ifft(mag):
    # Same steps as find_ifft_peak_index() in Nordic's cs_de library: take the
    # strongest peak, move to an earlier peak if one is at least 40% of it,
    # apply the left null correction, then interpolate between bins.
    nfft = len(mag)
    peak_to_null = (nfft + NUM_CH - 1) // NUM_CH
    imax = int(np.argmax(mag))
    vmax = mag[imax]
    nw, nxt = nfft - 2, nfft - 1
    idx = imax
    rise = False
    while nw != imax:
        if mag[nxt] < mag[nw]:
            if 2.5 * mag[nw] > vmax and rise:
                idx = nw
                break
        else:
            rise = True
        nw, nxt = nxt, (nxt + 1) % nfft
    if idx < nfft - 2:
        left = idx
        while True:
            nl = nfft - 1 if left == 0 else left - 1
            if ((mag[left] * 2 > mag[idx] or mag[left] > 1.10 * mag[nl])
                    and mag[left] * 10 > mag[idx] and nl != idx):
                left = nl
            else:
                break
        gap = (nfft + idx - left) if left > idx else (idx - left)
        if gap > peak_to_null:
            if left > idx:
                idx = left + peak_to_null - nfft if left + peak_to_null - nfft > 0 else idx
            else:
                idx = left + peak_to_null
    p, e, l = mag[idx], mag[idx - 1], mag[(idx + 1) % nfft]
    shift = (l - e) / (4 * p - 2 * (e + l)) if p >= e and p >= l and 4 * p != 2 * (e + l) else 0.0
    d = (idx + shift) * C / (2 * nfft * DF)
    return np.nan if idx >= nfft - 2 or d < 0 else d


def first_peak(mag, threshold):
    # Earliest peak between -3 m and 40 m that is at least `threshold` of the
    # strongest one. Used to test a lower rule than Nordic's 40%.
    nfft = len(mag)
    b = np.arange(nfft)
    d = np.where(b > nfft / 2, b - nfft, b) * C / (2 * nfft * DF)
    order = np.argsort(d)
    keep = (d[order] >= -3) & (d[order] <= 40)
    bins = order[keep]
    m = mag[bins]
    imax = int(np.argmax(m))
    pick = imax
    for i in range(1, imax + 1):
        if m[i] >= m[i - 1] and m[i] >= m[i + 1] and m[i] >= threshold * m[imax]:
            pick = i
            break
    # Same interpolation between bins as in nordic_ifft().
    b = bins[pick]
    p, e, l = mag[b], mag[b - 1], mag[(b + 1) % nfft]
    shift = (l - e) / (4 * p - 2 * (e + l)) if p >= e and p >= l and 4 * p != 2 * (e + l) else 0.0
    b = b + shift
    return (b - nfft if b > nfft / 2 else b) * C / (2 * nfft * DF)


def fill_gaps(h, ok):
    # MUSIC needs a value on every channel, so missing ones are filled by
    # straight line interpolation from their good neighbours.
    good = np.where(ok)[0]
    if len(good) < 10:
        return None
    out = h.copy()
    bad = np.where(~ok)[0]
    out[bad] = np.interp(bad, good, h[good].real) + 1j * np.interp(bad, good, h[good].imag)
    return out / np.sqrt(np.mean(np.abs(out) ** 2))


def _mdl_order(ev, n_snap, max_paths):
    # Minimum description length rule: how many eigenvalues are signal, not noise.
    m = len(ev)
    ev = np.maximum(ev, 1e-12)
    best, order = np.inf, 1
    for k in range(min(max_paths, m - 1) + 1):
        tail = ev[k:]
        geo, ari = np.exp(np.mean(np.log(tail))), np.mean(tail)
        cost = -n_snap * (m - k) * np.log(geo / ari) + 0.5 * k * (2 * m - k) * np.log(n_snap)
        if cost < best:
            best, order = cost, k
    return max(order, 1)


def music(snapshots, sub_len=40, share=0.15, max_paths=6):
    # snapshots: one or more filled H vectors (rows). Consecutive procedures can
    # be stacked as extra rows, because the random radio phase is already gone.
    # Returns the shortest path whose strength is at least `share` of the strongest.
    X = np.atleast_2d(snapshots)
    sub = np.lib.stride_tricks.sliding_window_view(X, sub_len, axis=1).reshape(-1, sub_len)
    R = sub.T @ sub.conj() / len(sub)
    J = np.eye(sub_len)[::-1]
    R = 0.5 * (R + J @ R.conj() @ J)
    ev, V = np.linalg.eigh(R)
    ev, V = ev[::-1], V[:, ::-1]
    p = _mdl_order(ev, len(sub), max_paths)
    noise = V[:, p:]
    grid = np.arange(-5.0, 40.0, 0.02)
    steer = np.exp(-1j * 2 * np.pi * DF * np.outer(np.arange(sub_len), 2 * grid / C))
    spectrum = 1.0 / np.maximum(np.sum(np.abs(noise.conj().T @ steer) ** 2, 0), 1e-12)
    peaks = np.where((spectrum[1:-1] > spectrum[:-2]) & (spectrum[1:-1] >= spectrum[2:]))[0] + 1
    if len(peaks) == 0:
        return np.nan
    dist = grid[peaks[np.argsort(spectrum[peaks])[::-1][:p]]]
    # Strength of each path by least squares on all 75 channels.
    A = np.exp(-1j * 2 * np.pi * DF * np.outer(np.arange(NUM_CH), 2 * dist / C))
    amp = np.abs(np.linalg.lstsq(A, X.T, rcond=None)[0]).mean(1)
    return float(np.min(dist[amp >= share * amp.max()]))
