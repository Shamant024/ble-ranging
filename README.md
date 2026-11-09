# ble-ranging

Firmware, tools and measured data for Bluetooth distance measurement on Nordic
development kits.

The current work uses Bluetooth 6.0 Channel Sounding between an nRF54L15 DK
and an nRF54LM20 DK. The firmware prints every distance result together with
the raw phase measurement (I/Q) of every channel, so the data can be studied
offline with methods other than the one built into the vendor library. A BLE
5.4 part on the nRF5340 will be added to the same repository later.

## Contents

| Folder | What it holds |
|---|---|
| `firmware/channel-sounding` | Zephyr application for both boards. One source tree, the role (initiator or reflector) is chosen at build time |
| `firmware/nordic-samples` | Nordic's `ras_initiator` and `ras_reflector` samples from nRF Connect SDK v3.4.1, unchanged, used as a reference run |
| `tools` | Python scripts to record the UART output, re-run Nordic's distance method on the raw tones, and compare methods |
| `dataset` | Four test sets recorded with this setup, as CSV files, with a description of every column |

## Hardware and software

| Item | Value |
|---|---|
| Initiator | nRF54L15 DK (PCA10156) |
| Reflector | nRF54LM20 DK (PCA10184), nRF54LM20B |
| SDK | nRF Connect SDK v3.4.1, Zephyr 4.4.2 |
| Python | 3.10 or newer, `numpy`, `pyserial` |

## Quick start

Build and flash the two boards (see `firmware/README.md`), then record a run:

```sh
pip install -r tools/requirements.txt
python3 tools/capture.py --port /dev/ttyACM0 --out run1 --mark-file mark.txt
```

Check the published data and reproduce the comparison:

```sh
cd tools
python3 validate_nordic.py
python3 evaluate.py
```

## Licences

Files based on Nordic Semiconductor samples keep Nordic's licence
(`LicenseRef-Nordic-5-Clause`, see `LICENSES/`). That licence allows use only
with Nordic Semiconductor integrated circuits. The other source files are under
the MIT licence. The dataset is under CC BY 4.0. Each source file names its
licence in its first lines.
