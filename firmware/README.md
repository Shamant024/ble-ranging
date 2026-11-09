# Firmware

## channel-sounding

One Zephyr application that runs on both development kits. The role is
chosen at build time with a configuration file:

| File | Role | What the board does |
|---|---|---|
| `role_initiator.conf` | Initiator | Scans for the reflector, connects, runs Channel Sounding procedures and computes the distance with Nordic's `cs_de` library |
| `role_reflector.conf` | Reflector | Advertises, accepts the connection, answers the sounding and sends its step data back over the Ranging Service (RAS) |

The Channel Sounding settings are the same as in Nordic's `ras_initiator`
sample: mode 2 (phase) with mode 1 (round trip time) sub steps, 72 channels
from 2404 MHz to 2478 MHz, channel selection 3b, 20 ms connection interval and
one procedure every 200 ms. One change is the 2048 point IFFT
(`CONFIG_BT_CS_DE_2048_NFFT`), which gives 0.073 m per bin instead of 0.29 m.

### Build and flash

From an nRF Connect SDK v3.4.1 workspace. Zephyr does not build from a path
that contains spaces.

```sh
# initiator on the nRF54L15 DK
west build -b nrf54l15dk/nrf54l15/cpuapp -d build/initiator firmware/channel-sounding \
    -- -DEXTRA_CONF_FILE=role_initiator.conf
west flash -d build/initiator --dev-id <J-Link serial>

# reflector on the nRF54LM20 DK
west build -b nrf54lm20dk/nrf54lm20b/cpuapp -d build/reflector firmware/channel-sounding \
    -- -DEXTRA_CONF_FILE=role_reflector.conf
west flash -d build/reflector --dev-id <J-Link serial>
```

Flash the reflector first. The initiator connects only to a device that
advertises the name set in `CONFIG_APP_PEER_NAME` (default `cs-reflector`).

On both boards LED1 shows the link. On the initiator LED2 toggles once per
distance result. With `CONFIG_APP_POWER_BANK_LOAD` (on by default for the
reflector) LED2 to LED4 stay lit and the CPU never sleeps, so a USB power bank
does not switch off because the load is too small.

### UART output

The initiator prints at 115200 baud. Machine readable lines start with `$`:

| Line | Content |
|---|---|
| `$I,<uptime_ms>,<key>,<value>` | One setting of the build, the link or the Channel Sounding configuration |
| `$R,<n>,<uptime_ms>,<counter>,<ifft_m>,<phase_slope_m>,<rtt_m>,<best_m>,<tone_ok>,<good_channels>,<rtt_count>,<rssi_dbm>,<rtt_raw_m>,<best_med9_m>,<cal_m>` | One distance result per procedure |
| `$Q,<uptime_ms>,<counter>,<steps>,<token>,...` | Raw tones of every mode 2 step of that procedure |

Each `$Q` token is 16 hex characters: channel (2), the initiator's 24 bit phase
correction term (6), the reflector's (6), then the quality flag of each side
(1 each). In a phase correction term, I is the low 12 bits and Q the high 12
bits, both signed. `tools/capture.py` decodes these lines into CSV files.

### Source files

| File | Purpose |
|---|---|
| `src/main.c` | Starts Bluetooth and ranging, forwards each result to the UART |
| `src/ranging.h` | The result structure every radio backend fills |
| `src/ranging_cs_initiator.c` | Initiator: connection, Channel Sounding setup, step pairing, estimator call, raw tone output |
| `src/ranging_cs_reflector.c` | Reflector: advertising and Channel Sounding answers |
| `src/data_out.c` | Formats the `$I`, `$R` and `$Q` lines |
| `boards/*.overlay` | Antenna switch pins for each development kit, from Nordic's samples |

## nordic-samples

`ras_initiator` and `ras_reflector` copied without changes from
`nrf/samples/bluetooth/channel_sounding` in nRF Connect SDK v3.4.1. They were
used to check that the distance error seen with the application above also
appears with Nordic's own code (test 4 in the dataset). They use a 512 point
IFFT and print a median of recent results rather than every procedure. Build
them the same way, without the `EXTRA_CONF_FILE` option.
