// Copyright (c) 2026 the ble-ranging authors
// SPDX-License-Identifier: MIT

#ifndef DATA_OUT_H_
#define DATA_OUT_H_

#include <zephyr/bluetooth/conn.h>

#include "ranging.h"

// Every machine readable line starts with '$' so a host script can pick them
// out from between the normal log lines.
//   $I,<uptime_ms>,<key>,<value>   facts about this build, the link and the radio
//   $R,<n>,<uptime_ms>,<counter>,<ifft_m>,<phase_slope_m>,<rtt_m>,<best_m>,
//      <tone_ok>,<good_channels>,<rtt_count>,<rssi_dbm>,<rtt_raw_m>,<best_med9_m>,<cal_m>
//   $Q,<uptime_ms>,<counter>,<steps>,<token>,<token>...   raw tones, see ranging_cs_initiator.c

void data_out_info(const char *key, const char *value);
void data_out_info_int(const char *key, int value);
void data_out_sample(const struct range_sample *s);
void data_out_iq(uint16_t counter, uint8_t steps, const char *tokens);

// Firmware version, board, chip and role of this build.
void data_out_build_info(void);

// Connection interval, latency, timeout, PHY, security as the link has them now.
void data_out_conn(struct bt_conn *conn);

// What the peer's radio says it can do for Channel Sounding.
void data_out_cs_caps(const struct bt_conn_le_cs_capabilities *c);

// The CS config both radios agreed on, including the channel list.
void data_out_cs_config(const struct bt_conn_le_cs_config *c);

// The timing the controllers settled on when procedures started.
void data_out_cs_procedure(const struct bt_conn_le_cs_procedure_enable_complete *p);

#endif
