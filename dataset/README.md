# Dataset

Bluetooth 6.0 Channel Sounding measurements between an nRF54L15 DK
(initiator) and an nRF54LM20 DK (reflector), recorded indoors in an office
room. The initiator stayed in one place, and the reflector was moved to a
series of distances along a line of sight, measured with a tape.

All times are in seconds from the start of each recording. Distances are in
metres. Licence: CC BY 4.0.

## Tests

| Test | Firmware | Raw tones | Procedures | Placements | Distances (m) |
|---|---|---|---|---|---|
| test1 | `channel-sounding`, before the raw tone output existed | no | 12,192 | 15 labelled | 0.48 to 12.19 |
| test2 | `channel-sounding` | yes | 3,590 | 13 | 0.60, 1.30, 2.30, 3.50, 6.10, 12.19 |
| test3 | `channel-sounding` | yes | 3,336 | 14 | 0.60, 1.30, 2.50, 3.50, 8.23, 12.19 |
| test4 | Nordic `ras_initiator` / `ras_reflector`, unchanged | no | 1,131 lines | 4 | 1.30, 2.50, 3.50 |

**test1** holds four sessions. Session `a` is the first run over the full
range, including cases with a person next to a board or standing between the
boards. Session `b` used a 512 point IFFT and session `c` the same spots with a
2048 point IFFT. Session `d` applied the linear calibration in the `cal_m`
column. Only session `a` has time windows in `placements.csv`. The other sessions
visited these distances in order: `b` 0.60, 1.30, 2.50 and 3.40 m; `c` about
0.65, 1.30, 2.50 and 3.60 m; `d` 0.60, 1.30, 2.50, 3.60, 8.23 and 12.19 m.

**test2** is the first run with raw tones. Each distance was placed two or
three times. During this run people moved near the link while placing the
reflector, so the first seconds after a placement can include movement.

**test3** repeats test2 with a stricter routine: the reflector was placed, the
person walked away from the line, and only then was the placement marked.
Placement `8.23m_3` has a person standing between the boards.

**test4** is Nordic's unchanged sample at three distances. It prints a median
of recent results at about 10 lines per second, not every procedure. The link
dropped twice, as noted in `placements.csv`.

## Files

### ranging.csv (tests 1 to 3)

One row per Channel Sounding procedure, as printed by the initiator.

| Column | Meaning |
|---|---|
| `session` | test1 only, the recording session |
| `ifft_points` | test1 only, IFFT size used by the board |
| `time_s` | host time since the start of the recording |
| `line_n` | count of result lines sent by the board; a gap means a line was lost on the UART |
| `uptime_ms` | board uptime |
| `counter` | procedure counter from the radio, wraps at 4096 |
| `ifft_m` | distance from Nordic's IFFT method |
| `phase_slope_m` | distance from the slope of phase against frequency |
| `rtt_m` | distance from the round trip time of mode 1 steps, clamped at 0 by the library |
| `best_m` | the library's choice among the three, equal to `ifft_m` here |
| `tone_ok` | 1 if at least 15 channels had good tones |
| `good_channels` | channels with at least one good tone, out of 72 |
| `rtt_count` | mode 1 steps that passed the quality checks |
| `rssi_dbm` | mean RSSI of the mode 1 steps, 127 if none |
| `rtt_raw_m` | round trip time distance before the clamp at 0 |
| `best_med9_m` | median of `best_m` over the last 9 procedures |
| `cal_m` | `best_med9_m` after the firmware's linear calibration |
| `distance_m` | tape distance of the current placement, empty before the first one |
| `placement` | name of the current placement |

Missing values are empty. `nan` from the board is also stored as empty.

### tones.csv (tests 2 and 3)

One row per mode 2 step, which is one channel visit within a procedure. The
values are the raw phase correction terms of both radios, before any filter
or averaging.

| Column | Meaning |
|---|---|
| `time_s`, `counter` | as in `ranging.csv`, used to join the two files |
| `step` | order of the step within the procedure |
| `channel` | Channel Sounding channel number |
| `freq_mhz` | 2402 + channel |
| `i_local`, `q_local` | I and Q measured by the initiator, signed 12 bit |
| `i_remote`, `q_remote` | I and Q measured by the reflector, signed 12 bit |
| `quality_local`, `quality_remote` | tone quality flag of each side, 0 is good |
| `distance_m`, `placement` | as in `ranging.csv` |

The round trip tone of a channel is
`(i_local + j q_local) * (i_remote + j q_remote)`. Multiplying the two cancels
the random starting phase of each radio.

### placements.csv

| Column | Meaning |
|---|---|
| `placement` | name, distance and repeat number, for example `2.50m_2` |
| `distance_m` | tape distance |
| `start_s`, `end_s` | from the mark to the next mark (tests 2 to 4), or the analysed window (test1) |
| `condition` | `clear`, or what was between or next to the boards |
| `note` | anything unusual about the placement |

In tests 2 to 4 the span from `start_s` to `end_s` also contains the walk to
the next spot. The comparison in `tools/evaluate.py` uses 10 s starting 3 s
after `start_s`.

### setup.json (tests 2 and 3)

Link and Channel Sounding settings as read back from the controllers.

## Known issues

- One room only, and the two boards are different chips.
- In test3, placement `0.60m_2` reads about 5 m with a signal about 15 dB
  weaker than the other 0.60 m placements. The cause was not recorded.
- In test2, the 12.19 m placements include part of the walk back.
