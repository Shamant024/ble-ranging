# Tools

Python 3.10 or newer. Install the two packages with
`pip install -r requirements.txt`.

| Script | What it does |
|---|---|
| `capture.py` | Records the initiator's UART into CSV files: distance results, raw tones, full log and settings |
| `log_nordic_sample.py` | Records the distance lines printed by Nordic's unchanged `ras_initiator` sample |
| `cs_estimators.py` | Distance methods on the raw tones: a Python copy of Nordic's IFFT method, an IFFT rule with a lower threshold, and MUSIC |
| `dataset.py` | Loads one test folder of the dataset and marks the scoring window of each placement |
| `validate_nordic.py` | Re-runs Nordic's IFFT method on the saved tones and checks it against the distance the board printed |
| `evaluate.py` | Compares all methods on tests 2 and 3 |

## Recording a run

```sh
python3 capture.py --port /dev/ttyACM0 --out run1 --mark-file mark.txt
```

Each time the reflector is moved, write the tape distance and a short label
into the mark file, for example `echo "2.50 2.50m_1" > mark.txt`. All rows
after that carry the new distance and label until the next mark.

## Checking the data

`validate_nordic.py` combines the two boards' tones of each procedure, runs
the same peak search as Nordic's `cs_de` library and compares the result with
the `ifft_m` value the board printed. On the published data it matches within
1 cm on every procedure:

```
test2: Python result within 1 cm of the board on 3568 of 3568 procedures
test3: Python result within 1 cm of the board on 3335 of 3335 procedures
```

## Comparing methods

`evaluate.py` scores every method on the same window, 10 s starting 3 s after
each placement. MUSIC combines the current procedure with up to nine before it
at the same placement. The fixed offset is learned on one test and applied to
the other, so a test is never scored with an offset fitted on itself. It takes
about 10 s and prints the median error and the share of readings within 1 m
and 0.5 m for each method.
