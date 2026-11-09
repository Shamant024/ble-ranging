// Copyright (c) 2026 the ble-ranging authors
// SPDX-License-Identifier: MIT

#ifndef RANGING_H_
#define RANGING_H_

#include <stdbool.h>
#include <stdint.h>

// RSSI value used when no step carried one.
#define RANGE_RSSI_NONE 127

// One result per ranging procedure. Every radio backend fills this same struct,
// so a different backend (for example a BLE 5.4 one) can be added without
// changing main.c.
struct range_sample {
	uint16_t counter;      // procedure counter from the radio, 12 bits, wraps at 4096
	float ifft_m;          // distance from the IFFT method, NAN if it had no data
	float phase_slope_m;   // distance from the phase slope method, NAN if no data
	float rtt_m;           // distance from round trip time, NAN if no data
	float rtt_raw_m;       // same before Nordic clamps it at 0, can be negative
	float best_m;          // the library's pick of the three
	float best_med9_m;     // median of best_m over the last 9 procedures, as in Nordic's sample
	float cal_m;           // best_med9_m after the linear calibration in ranging_cs_initiator.c
	bool tone_ok;          // enough clean channels for the phase methods to be trusted
	uint8_t good_channels; // channels with at least one clean tone, out of 72
	uint8_t rtt_count;     // round trip steps that passed the quality checks
	int8_t rssi_dbm;       // mean RSSI over the round trip steps, or RANGE_RSSI_NONE
};

typedef void (*ranging_cb_t)(const struct range_sample *sample);

// Brings up the radio side for this build's role and returns once ranging
// is running. cb is called from the Bluetooth thread for every procedure.
// The reflector never produces samples, so it ignores cb.
int ranging_start(ranging_cb_t cb);

#endif
