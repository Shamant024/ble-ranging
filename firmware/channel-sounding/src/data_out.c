// Copyright (c) 2026 the ble-ranging authors
// SPDX-License-Identifier: MIT

#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/cs.h>
#include <app_version.h>

#include "data_out.h"

// Counts every $R line sent. A gap in this count on the host means a line was
// lost on the UART, which is different from a procedure lost on the radio.
static uint32_t line_n;

void data_out_info(const char *key, const char *value)
{
	printk("$I,%u,%s,%s\n", k_uptime_get_32(), key, value);
}

void data_out_info_int(const char *key, int value)
{
	printk("$I,%u,%s,%d\n", k_uptime_get_32(), key, value);
}

void data_out_build_info(void)
{
	data_out_info("fw", APP_VERSION_STRING);
	data_out_info("board", CONFIG_BOARD_TARGET);
	data_out_info("soc", CONFIG_SOC);
	data_out_info("role", IS_ENABLED(CONFIG_APP_ROLE_INITIATOR) ? "initiator" : "reflector");
	data_out_info("peer_name", CONFIG_APP_PEER_NAME);
}

static const char *phy_name(uint8_t phy)
{
	switch (phy) {
	case BT_GAP_LE_PHY_1M:
		return "1M";
	case BT_GAP_LE_PHY_2M:
		return "2M";
	case BT_GAP_LE_PHY_CODED:
		return "Coded";
	default:
		return "?";
	}
}

void data_out_conn(struct bt_conn *conn)
{
	struct bt_conn_info info;
	char addr[BT_ADDR_LE_STR_LEN];

	if (bt_conn_get_info(conn, &info)) {
		return;
	}

	bt_addr_le_to_str(info.le.local, addr, sizeof(addr));
	data_out_info("local_addr", addr);
	bt_addr_le_to_str(info.le.dst, addr, sizeof(addr));
	data_out_info("peer_addr", addr);
	data_out_info_int("conn_interval_us", info.le.interval_us);
	data_out_info_int("conn_latency", info.le.latency);
	data_out_info_int("conn_timeout_ms", info.le.timeout * 10);
	data_out_info_int("security_level", info.security.level);
	data_out_info_int("key_size", info.security.enc_key_size);
	data_out_info("phy_tx", phy_name(info.le.phy->tx_phy));
	data_out_info("phy_rx", phy_name(info.le.phy->rx_phy));
}

// The link can change PHY or timing after it is up, report every change.
static void phy_updated(struct bt_conn *conn, struct bt_conn_le_phy_info *param)
{
	data_out_info("phy_tx", phy_name(param->tx_phy));
	data_out_info("phy_rx", phy_name(param->rx_phy));
}

static void param_updated(struct bt_conn *conn, uint16_t interval, uint16_t latency,
			  uint16_t timeout)
{
	data_out_info_int("conn_interval_us", interval * 1250);
	data_out_info_int("conn_latency", latency);
	data_out_info_int("conn_timeout_ms", timeout * 10);
}

BT_CONN_CB_DEFINE(data_out_conn_cb) = {
	.le_phy_updated = phy_updated,
	.le_param_updated = param_updated,
};

void data_out_cs_caps(const struct bt_conn_le_cs_capabilities *c)
{
	data_out_info_int("peer_num_antennas", c->num_antennas_supported);
	data_out_info_int("peer_max_antenna_paths", c->max_antenna_paths_supported);
	data_out_info_int("peer_num_configs", c->num_config_supported);
	data_out_info_int("peer_initiator_ok", c->initiator_supported);
	data_out_info_int("peer_reflector_ok", c->reflector_supported);
	data_out_info_int("peer_mode3_ok", c->mode_3_supported);
	data_out_info_int("peer_sync_2m_ok", c->cs_sync_2m_phy_supported);
	data_out_info_int("peer_chsel_3c_ok", c->chsel_alg_3c_supported);
	data_out_info("peer_rtt_aa_precision",
		      c->rtt_aa_only_precision == BT_CONN_LE_CS_RTT_AA_ONLY_10NS    ? "10 ns"
		      : c->rtt_aa_only_precision == BT_CONN_LE_CS_RTT_AA_ONLY_150NS ? "150 ns"
										    : "none");
	data_out_info_int("peer_t_ip1_mask", c->t_ip1_times_supported);
}

// Writes the used CS channels as frequency ranges, like "2404-2424,2428-2478".
// CS channel k sits at 2402 + k MHz.
static void channel_ranges(const uint8_t map[10], char *out, size_t len, int *count)
{
	int start = -1;
	size_t used = 0;

	*count = 0;
	out[0] = '\0';
	for (int k = 0; k <= 79; k++) {
		bool on = k < 79 && BT_LE_CS_CHANNEL_BIT_GET(map, k);

		if (on) {
			(*count)++;
			if (start < 0) {
				start = k;
			}
		} else if (start >= 0) {
			used += snprintf(out + used, len - used, "%s%d-%d", used ? ";" : "",
					 2402 + start, 2402 + k - 1);
			start = -1;
			if (used >= len) {
				return;
			}
		}
	}
}

void data_out_cs_config(const struct bt_conn_le_cs_config *c)
{
	static const char *const rtt_types[] = {"AA only", "32 bit sounding", "96 bit sounding",
						"32 bit random", "64 bit random", "96 bit random",
						"128 bit random"};
	static const char *const sync_phys[] = {"?", "1M", "2M", "2M 2BT"};
	char buf[64];
	int n_channels;

	data_out_info_int("cs_config_id", c->id);
	data_out_info("cs_role", c->role == BT_CONN_LE_CS_ROLE_INITIATOR ? "initiator" : "reflector");
	data_out_info_int("cs_main_mode", BT_CONN_LE_CS_MODE_MAIN_MODE_PART(c->mode));
	data_out_info_int("cs_sub_mode", BT_CONN_LE_CS_MODE_SUB_MODE_PART(c->mode));
	data_out_info_int("cs_min_main_steps", c->min_main_mode_steps);
	data_out_info_int("cs_max_main_steps", c->max_main_mode_steps);
	data_out_info_int("cs_main_mode_repetition", c->main_mode_repetition);
	data_out_info_int("cs_mode0_steps", c->mode_0_steps);
	data_out_info("cs_rtt_type", c->rtt_type < ARRAY_SIZE(rtt_types) ? rtt_types[c->rtt_type] : "?");
	data_out_info("cs_sync_phy", c->cs_sync_phy < ARRAY_SIZE(sync_phys) ? sync_phys[c->cs_sync_phy] : "?");
	data_out_info_int("cs_channel_map_repetition", c->channel_map_repetition);
	data_out_info("cs_channel_selection", c->channel_selection_type == 0 ? "3b" : "3c");
	data_out_info_int("cs_t_ip1_us", c->t_ip1_time_us);
	data_out_info_int("cs_t_ip2_us", c->t_ip2_time_us);
	data_out_info_int("cs_t_fcs_us", c->t_fcs_time_us);
	data_out_info_int("cs_t_pm_us", c->t_pm_time_us);

	channel_ranges(c->channel_map, buf, sizeof(buf), &n_channels);
	data_out_info_int("cs_num_channels", n_channels);
	data_out_info("cs_channels_mhz", buf);
}

void data_out_cs_procedure(const struct bt_conn_le_cs_procedure_enable_complete *p)
{
	data_out_info_int("tx_power_dbm", p->selected_tx_power);
	data_out_info_int("antenna_config", p->tone_antenna_config_selection);
	data_out_info_int("subevent_len_us", p->subevent_len);
	data_out_info_int("subevents_per_event", p->subevents_per_event);
	data_out_info_int("subevent_interval", p->subevent_interval);
	data_out_info_int("event_interval", p->event_interval);
	data_out_info_int("procedure_interval", p->procedure_interval);
	data_out_info_int("procedure_count", p->procedure_count);
	data_out_info_int("max_procedure_len", p->max_procedure_len);
}

void data_out_sample(const struct range_sample *s)
{
	// Build the whole line first and print it once, so a log line from
	// another thread is less likely to land in the middle of it.
	char line[160];

	snprintk(line, sizeof(line), "$R,%u,%u,%u,%.3f,%.3f,%.3f,%.3f,%u,%u,%u,%d,%.3f,%.3f,%.3f\n",
		 line_n++, k_uptime_get_32(), s->counter, (double)s->ifft_m,
		 (double)s->phase_slope_m, (double)s->rtt_m, (double)s->best_m, s->tone_ok,
		 s->good_channels, s->rtt_count, s->rssi_dbm, (double)s->rtt_raw_m,
		 (double)s->best_med9_m, (double)s->cal_m);
	printk("%s", line);
}

// tokens already starts with a comma, or is empty if no mode 2 step came in.
void data_out_iq(uint16_t counter, uint8_t steps, const char *tokens)
{
	printk("$Q,%u,%u,%u%s\n", k_uptime_get_32(), counter, steps, tokens);
}
