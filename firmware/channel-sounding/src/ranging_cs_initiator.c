// Copyright (c) 2024 Nordic Semiconductor ASA
// Modifications copyright (c) 2026 the ble-ranging authors
// SPDX-License-Identifier: LicenseRef-Nordic-5-Clause

// Channel Sounding initiator, based on Nordic's ras_initiator sample and changed
// to report the result of every procedure.
//
// What one distance takes, in order:
//  1. Scan for the reflector by name and connect.
//  2. Encrypt the link, exchange the MTU and find the reflector's Ranging Service (RAS).
//  3. Agree a CS config with the reflector and turn on CS security.
//  4. Start CS procedures. Each procedure hops over up to 72 channels.
//  5. The local radio delivers its own step data (subevent_result_cb).
//  6. The reflector sends its step data over RAS (ranging_data_cb).
//  7. Pair the two sides step by step, run Nordic's estimator and report.
//
// Every result goes out on the UART as a $R line, followed by a $Q line with the
// raw tone of every channel, so other estimators can be run offline on the host.

#include <math.h>
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/cs.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>
#include <bluetooth/scan.h>
#include <bluetooth/gatt_dm.h>
#include <bluetooth/services/ras.h>
#include <bluetooth/cs_de.h>
#include <dk_buttons_and_leds.h>

#include "data_out.h"
#include "ranging.h"

LOG_MODULE_REGISTER(cs_initiator, LOG_LEVEL_INF);

#define LINK_LED DK_LED1

#define CS_CONFIG_ID 0
#define NUM_MODE_0_STEPS 3
#define PROCEDURE_COUNTER_NONE (-1)

// The estimator's channel index 0 is BLE channel 2, channels 0 and 1 are never used for CS.
#define CHANNEL_INDEX_OFFSET 2

// Phase methods need at least this many clean channels to be trusted.
#define TONE_OK_MIN_CHANNELS 15

// Connection interval 0x10 x 1.25 ms = 20 ms.
#define CONN_INTERVAL 0x10

#if defined(CONFIG_APP_STEP_MODE_2_SUB_1)
#define CS_CONFIG_MODE BT_CONN_LE_CS_MAIN_MODE_2_SUB_MODE_1
#define STEP_MODE_NAME "2+1"
#elif defined(CONFIG_APP_STEP_MODE_2)
#define CS_CONFIG_MODE BT_CONN_LE_CS_MAIN_MODE_2_NO_SUB_MODE
#define STEP_MODE_NAME "2"
#elif defined(CONFIG_APP_STEP_MODE_1)
#define CS_CONFIG_MODE BT_CONN_LE_CS_MAIN_MODE_1_NO_SUB_MODE
#define STEP_MODE_NAME "1"
#endif

// One antenna on each DK. Multiple antenna paths are not supported.
BUILD_ASSERT(CONFIG_BT_RAS_MAX_ANTENNA_PATHS == 1);

#define LOCAL_PROCEDURE_MEM                                                                        \
	((BT_RAS_MAX_STEPS_PER_PROCEDURE * sizeof(struct bt_le_cs_subevent_step)) +                \
	 (BT_RAS_MAX_STEPS_PER_PROCEDURE * BT_RAS_MAX_STEP_DATA_LEN))

static K_SEM_DEFINE(sem_connected, 0, 1);
static K_SEM_DEFINE(sem_security, 0, 1);
static K_SEM_DEFINE(sem_mtu_exchange_done, 0, 1);
static K_SEM_DEFINE(sem_discovery_done, 0, 1);
static K_SEM_DEFINE(sem_ras_features, 0, 1);
static K_SEM_DEFINE(sem_remote_capabilities_obtained, 0, 1);
static K_SEM_DEFINE(sem_config_created, 0, 1);
static K_SEM_DEFINE(sem_cs_security_enabled, 0, 1);

// Taken while the local steps of one procedure are being collected and given
// back once the reflector's half has arrived and been used. This stops a new
// procedure from overwriting steps that have not been matched yet.
static K_SEM_DEFINE(sem_local_steps, 1, 1);

static struct bt_conn *connection;
static ranging_cb_t sample_cb;

NET_BUF_SIMPLE_DEFINE_STATIC(latest_local_steps, LOCAL_PROCEDURE_MEM);
NET_BUF_SIMPLE_DEFINE_STATIC(latest_peer_steps, BT_RAS_PROCEDURE_MEM);
static int32_t most_recent_local_ranging_counter = PROCEDURE_COUNTER_NONE;
static int32_t dropped_ranging_counter = PROCEDURE_COUNTER_NONE;
static uint32_t ras_feature_bits;

static struct bt_conn_le_cs_config cs_config;
static cs_de_report_t report;

// How many clean tones landed on each channel in this procedure.
static uint16_t n_iqs[CS_DE_NUM_CHANNELS];

static int32_t rssi_sum;
static uint8_t rssi_n;

// Raw tones for the host, one token per mode 2 step, in the order the radio
// hopped. They are kept before any quality filter or averaging, so other
// estimators can be tried offline on exactly what the radios measured.
#define IQ_MAX_STEPS 80
#define IQ_TOKEN_LEN 17
static char iq_tokens[IQ_MAX_STEPS * IQ_TOKEN_LEN + 1];
static size_t iq_len;
static uint8_t iq_steps;

// Token is 16 hex chars and a comma: channel (2), local PCT (6), remote PCT
// (6), local quality (1), remote quality (1). A PCT is the raw 24 bit field,
// I in the low 12 bits and Q in the high 12, both two's complement.
static void add_iq_token(uint8_t channel, struct bt_hci_le_cs_step_data_tone_info *local,
			 struct bt_hci_le_cs_step_data_tone_info *remote)
{
	if (iq_steps >= IQ_MAX_STEPS) {
		return;
	}
	iq_len += snprintk(iq_tokens + iq_len, sizeof(iq_tokens) - iq_len, ",%02x%06x%06x%x%x",
			   channel, sys_get_le24(local->phase_correction_term),
			   sys_get_le24(remote->phase_correction_term), local->quality_indicator,
			   remote->quality_indicator);
	iq_steps++;
}

static void running_mean(float *avg, float new_value, uint16_t n)
{
	float a = 1.0f / n;

	*avg = a * new_value + (1.0f - a) * (*avg);
}

// Phase correction terms (PCT) are the I/Q of one tone. The phase
// difference across channels is what the phase methods turn into distance.
static void extract_pcts(uint8_t channel_index, uint8_t antenna_permutation_index,
			 struct bt_hci_le_cs_step_data_tone_info *local_tone_info,
			 struct bt_hci_le_cs_step_data_tone_info *remote_tone_info)
{
	for (uint8_t tone_index = 0; tone_index < report.n_ap; tone_index++) {
		int ap = bt_le_cs_get_antenna_path(report.n_ap, antenna_permutation_index,
						   tone_index);
		if (ap < 0) {
			LOG_WRN("Invalid antenna path");
			return;
		}

		// A tone only counts if both radios rated it high quality.
		if (local_tone_info[tone_index].quality_indicator !=
			    BT_HCI_LE_CS_TONE_QUALITY_HIGH ||
		    remote_tone_info[tone_index].quality_indicator !=
			    BT_HCI_LE_CS_TONE_QUALITY_HIGH) {
			return;
		}

		struct bt_le_cs_iq_sample local_iq =
			bt_le_cs_parse_pct(local_tone_info[tone_index].phase_correction_term);
		struct bt_le_cs_iq_sample remote_iq =
			bt_le_cs_parse_pct(remote_tone_info[tone_index].phase_correction_term);
		cs_de_iq_tones_t *t = &report.iq_tones[ap];
		uint16_t n = ++n_iqs[channel_index];

		// Same channel can be visited more than once, average the visits.
		if (n == 1) {
			t->i_local[channel_index] = local_iq.i;
			t->q_local[channel_index] = local_iq.q;
			t->i_remote[channel_index] = remote_iq.i;
			t->q_remote[channel_index] = remote_iq.q;
		} else {
			running_mean(&t->i_local[channel_index], local_iq.i, n);
			running_mean(&t->q_local[channel_index], local_iq.q, n);
			running_mean(&t->i_remote[channel_index], remote_iq.i, n);
			running_mean(&t->q_remote[channel_index], remote_iq.q, n);
		}
	}
}

// Mode 1 steps carry a time of arrival minus time of departure on each side.
// Their difference is the round trip in half nanoseconds.
static void extract_rtt_timings(struct bt_hci_le_cs_step_data_mode_1 *local,
				struct bt_hci_le_cs_step_data_mode_1 *peer)
{
	if (local->packet_quality_aa_check != BT_HCI_LE_CS_PACKET_QUALITY_AA_CHECK_SUCCESSFUL ||
	    local->packet_rssi == BT_HCI_LE_CS_PACKET_RSSI_NOT_AVAILABLE ||
	    local->tod_toa_reflector == BT_HCI_LE_CS_TIME_DIFFERENCE_NOT_AVAILABLE ||
	    peer->packet_quality_aa_check != BT_HCI_LE_CS_PACKET_QUALITY_AA_CHECK_SUCCESSFUL ||
	    peer->packet_rssi == BT_HCI_LE_CS_PACKET_RSSI_NOT_AVAILABLE ||
	    peer->tod_toa_reflector == BT_HCI_LE_CS_TIME_DIFFERENCE_NOT_AVAILABLE) {
		return;
	}

	report.rtt_accumulated_half_ns += local->toa_tod_initiator - peer->tod_toa_reflector;
	report.rtt_count++;

	// The RSSI field is a signed dBm value stored in a uint8_t.
	rssi_sum += (int8_t)local->packet_rssi;
	rssi_n++;
}

static bool process_ranging_header(struct ras_ranging_header *ranging_header, void *user_data)
{
	report.n_ap = MAX(1, __builtin_popcount(ranging_header->antenna_paths_mask & 0x0F));
	return true;
}

static bool process_step_data(struct bt_le_cs_subevent_step *local_step,
			      struct bt_le_cs_subevent_step *peer_step, void *user_data)
{
	uint8_t channel_index = local_step->channel - CHANNEL_INDEX_OFFSET;

	if (local_step->mode == BT_HCI_OP_LE_CS_MAIN_MODE_2) {
		struct bt_hci_le_cs_step_data_mode_2 *local = (void *)local_step->data;
		struct bt_hci_le_cs_step_data_mode_2 *peer = (void *)peer_step->data;

		// One antenna path, so tone 0 is the only tone in the step.
		add_iq_token(local_step->channel, &local->tone_info[0], &peer->tone_info[0]);
		extract_pcts(channel_index, local->antenna_permutation_index, local->tone_info,
			     peer->tone_info);
	} else if (local_step->mode == BT_HCI_OP_LE_CS_MAIN_MODE_1) {
		extract_rtt_timings((void *)local_step->data, (void *)peer_step->data);
	}

	return true;
}

static uint8_t count_good_channels(void)
{
	uint8_t count = 0;

	for (uint8_t i = 0; i < CS_DE_NUM_CHANNELS; i++) {
		if (BT_LE_CS_CHANNEL_BIT_GET(cs_config.channel_map, i + CHANNEL_INDEX_OFFSET) &&
		    n_iqs[i] >= 1) {
			count++;
		}
	}
	return count;
}

#define MEDIAN_N 9

// Linear calibration for one nRF54L15 and nRF54LM20 pair, fitted on 15 clear
// path points from 0.48 m to 12.19 m measured by tape: measured = 1.0526 x
// true + 1.1338. It only holds for that pair and those surroundings, so it has
// to be fitted again for any other setup.
#define CAL_SCALE  1.0526f
#define CAL_OFFSET 1.1338f

static float calibrate(float measured_m)
{
	return fmaxf((measured_m - CAL_OFFSET) / CAL_SCALE, 0.0f);
}

// Median over the last MEDIAN_N finite values. Bad procedures (NAN) are left
// out, so one failed procedure does not drag the median.
static float median_of_last(float value)
{
	static float last[MEDIAN_N];
	static uint8_t count;
	static uint8_t next;
	float sorted[MEDIAN_N];

	if (isfinite(value)) {
		last[next] = value;
		next = (next + 1) % MEDIAN_N;
		count = MIN(count + 1, MEDIAN_N);
	}
	if (count == 0) {
		return NAN;
	}

	// Insertion sort, at most 9 values.
	for (uint8_t i = 0; i < count; i++) {
		float v = last[i];
		int8_t j = i - 1;

		while (j >= 0 && sorted[j] > v) {
			sorted[j + 1] = sorted[j];
			j--;
		}
		sorted[j + 1] = v;
	}
	return count % 2 ? sorted[count / 2]
			 : (sorted[count / 2 - 1] + sorted[count / 2]) / 2.0f;
}

static void release_local_steps(void)
{
	net_buf_simple_reset(&latest_local_steps);
	if (!(ras_feature_bits & RAS_FEAT_REALTIME_RD)) {
		net_buf_simple_reset(&latest_peer_steps);
	}
	k_sem_give(&sem_local_steps);
}

// Steps 6 and 7. The reflector's half of a procedure has arrived.
static void ranging_data_cb(struct bt_conn *conn, uint16_t ranging_counter, int err)
{
	if (err) {
		LOG_ERR("Ranging data error, counter %u (err %d)", ranging_counter, err);
		return;
	}

	if (ranging_counter != most_recent_local_ranging_counter) {
		LOG_INF("Dropped: peer counter %u does not match ours %d", ranging_counter,
			most_recent_local_ranging_counter);
		net_buf_simple_reset(&latest_local_steps);
		k_sem_give(&sem_local_steps);
		return;
	}

	if (latest_local_steps.len == 0) {
		LOG_WRN("All subevents of procedure %u were aborted", ranging_counter);
		release_local_steps();
		return;
	}

	memset(&report, 0, sizeof(report));
	memset(n_iqs, 0, sizeof(n_iqs));
	rssi_sum = 0;
	rssi_n = 0;
	iq_len = 0;
	iq_steps = 0;
	iq_tokens[0] = '\0';
	report.role = cs_config.role;

	bt_ras_rreq_rd_subevent_data_parse(&latest_peer_steps, &latest_local_steps,
					   cs_config.role, process_ranging_header, NULL,
					   process_step_data, NULL);

	uint8_t good_channels = count_good_channels();

	report.distance_estimates[0].ifft = NAN;
	report.distance_estimates[0].phase_slope = NAN;
	report.distance_estimates[0].rtt = NAN;
	report.distance_estimates[0].best = NAN;
	report.tone_quality[0] = good_channels >= TONE_OK_MIN_CHANNELS ? CS_DE_TONE_QUALITY_OK
								      : CS_DE_TONE_QUALITY_BAD;

	release_local_steps();

	// The estimator fills distance_estimates. If it rejects the whole
	// procedure, the NANs stay and the failure is visible on the host.
	cs_de_calc(&report);

	struct range_sample s = {
		.counter = ranging_counter,
		.ifft_m = report.distance_estimates[0].ifft,
		.phase_slope_m = report.distance_estimates[0].phase_slope,
		.rtt_m = report.distance_estimates[0].rtt,
		// Same sum Nordic uses, without the clamp at 0. Half ns units,
		// halved again for one way, light covers 0.2998 m per ns.
		.rtt_raw_m = report.rtt_count
				     ? report.rtt_accumulated_half_ns * 0.5f / report.rtt_count / 2.0f *
					       0.299792458f
				     : NAN,
		.best_m = report.distance_estimates[0].best,
		.best_med9_m = median_of_last(report.distance_estimates[0].best),
		.tone_ok = report.tone_quality[0] == CS_DE_TONE_QUALITY_OK,
		.good_channels = good_channels,
		.rtt_count = report.rtt_count,
		.rssi_dbm = rssi_n ? (int8_t)(rssi_sum / rssi_n) : RANGE_RSSI_NONE,
	};

	s.cal_m = calibrate(s.best_med9_m);

	if (sample_cb) {
		sample_cb(&s);
	}
	data_out_iq(ranging_counter, iq_steps, iq_tokens);
}

// Step 5. The local radio's steps, which can arrive in several pieces.
static void subevent_result_cb(struct bt_conn *conn, struct bt_conn_le_cs_subevent_result *result)
{
	if (dropped_ranging_counter == result->header.procedure_counter) {
		return;
	}

	if (most_recent_local_ranging_counter !=
	    bt_ras_rreq_get_ranging_counter(result->header.procedure_counter)) {
		// New procedure. If the last one is still waiting for the
		// reflector, there is no buffer for this one, so it is skipped.
		if (k_sem_take(&sem_local_steps, K_NO_WAIT) < 0) {
			dropped_ranging_counter = result->header.procedure_counter;
			LOG_INF("Dropped local steps, still waiting for the reflector");
			return;
		}
		most_recent_local_ranging_counter =
			bt_ras_rreq_get_ranging_counter(result->header.procedure_counter);
	}

	if (result->header.subevent_done_status != BT_CONN_LE_CS_SUBEVENT_ABORTED &&
	    result->step_data_buf) {
		uint16_t len = result->step_data_buf->len;

		if (len > net_buf_simple_tailroom(&latest_local_steps)) {
			LOG_ERR("No room for step data (%u > %u)", latest_local_steps.len + len,
				latest_local_steps.size);
			net_buf_simple_reset(&latest_local_steps);
			dropped_ranging_counter = result->header.procedure_counter;
			return;
		}
		net_buf_simple_add_mem(&latest_local_steps,
				       net_buf_simple_pull_mem(result->step_data_buf, len), len);
	}

	dropped_ranging_counter = PROCEDURE_COUNTER_NONE;

	if (result->header.procedure_done_status == BT_CONN_LE_CS_PROCEDURE_COMPLETE) {
		most_recent_local_ranging_counter =
			bt_ras_rreq_get_ranging_counter(result->header.procedure_counter);
	} else if (result->header.procedure_done_status == BT_CONN_LE_CS_PROCEDURE_ABORTED) {
		LOG_WRN("Procedure %u aborted", result->header.procedure_counter);
		net_buf_simple_reset(&latest_local_steps);
		k_sem_give(&sem_local_steps);
	}
}

// Only used when the reflector does not stream in real time. The reflector
// signals that data is ready and the initiator fetches it.
static void ranging_data_ready_cb(struct bt_conn *conn, uint16_t ranging_counter)
{
	if (ranging_counter != most_recent_local_ranging_counter) {
		return;
	}

	int err = bt_ras_rreq_cp_get_ranging_data(connection, &latest_peer_steps, ranging_counter,
						  ranging_data_cb);
	if (err) {
		LOG_ERR("Get ranging data failed (err %d)", err);
		net_buf_simple_reset(&latest_local_steps);
		net_buf_simple_reset(&latest_peer_steps);
		k_sem_give(&sem_local_steps);
	}
}

static void ranging_data_overwritten_cb(struct bt_conn *conn, uint16_t ranging_counter)
{
	LOG_INF("Ranging data %u overwritten before it was read", ranging_counter);
}

static void mtu_exchange_cb(struct bt_conn *conn, uint8_t err,
			    struct bt_gatt_exchange_params *params)
{
	if (err) {
		LOG_ERR("MTU exchange failed (err %d)", err);
		return;
	}
	LOG_INF("MTU %u", bt_gatt_get_mtu(conn));
	data_out_info_int("att_mtu", bt_gatt_get_mtu(conn));
	k_sem_give(&sem_mtu_exchange_done);
}

static void discovery_completed_cb(struct bt_gatt_dm *dm, void *context)
{
	int err = bt_ras_rreq_alloc_and_assign_handles(dm, bt_gatt_dm_conn_get(dm));

	if (err) {
		LOG_ERR("RAS client setup failed (err %d)", err);
	}
	bt_gatt_dm_data_release(dm);
	k_sem_give(&sem_discovery_done);
}

static void discovery_service_not_found_cb(struct bt_conn *conn, void *context)
{
	LOG_ERR("Peer has no Ranging Service, disconnecting");
	bt_conn_disconnect(connection, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
}

static void discovery_error_found_cb(struct bt_conn *conn, int err, void *context)
{
	LOG_ERR("Service discovery failed (err %d)", err);
	bt_conn_disconnect(connection, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
}

static struct bt_gatt_dm_cb discovery_cb = {
	.completed = discovery_completed_cb,
	.service_not_found = discovery_service_not_found_cb,
	.error_found = discovery_error_found_cb,
};

static void ras_features_read_cb(struct bt_conn *conn, uint32_t feature_bits, int err)
{
	if (err) {
		LOG_WRN("Could not read RAS features (err %d)", err);
	} else {
		LOG_INF("RAS features 0x%x", feature_bits);
		ras_feature_bits = feature_bits;
		data_out_info_int("ras_features", feature_bits);
		data_out_info("ras_mode", feature_bits & RAS_FEAT_REALTIME_RD ? "real time" : "on demand");
	}
	k_sem_give(&sem_ras_features);
}

static void connected_cb(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("Connected to %s (err 0x%02X)", addr, err);

	if (err) {
		// Scanning stopped to connect. Reboot so it scans again.
		sys_reboot(SYS_REBOOT_COLD);
	}

	connection = bt_conn_ref(conn);
	dk_set_led_on(LINK_LED);
	data_out_info("event", "connected");
	data_out_info("peer", addr);
	k_sem_give(&sem_connected);
}

static void disconnected_cb(struct bt_conn *conn, uint8_t reason)
{
	LOG_INF("Disconnected (reason 0x%02X)", reason);
	data_out_info_int("disconnect_reason", reason);

	bt_conn_unref(conn);
	connection = NULL;
	dk_set_led_off(LINK_LED);

	// Reboot and start clean. The host sees the new $I lines.
	sys_reboot(SYS_REBOOT_COLD);
}

static void security_changed_cb(struct bt_conn *conn, bt_security_t level,
				enum bt_security_err err)
{
	if (err) {
		LOG_ERR("Security failed, level %u err %d", level, err);
		return;
	}
	LOG_INF("Security level %u", level);
	data_out_conn(conn);
	k_sem_give(&sem_security);
}

// Keep the requested connection interval, the procedure timing depends on it.
static bool le_param_req_cb(struct bt_conn *conn, struct bt_le_conn_param *param)
{
	return false;
}

static void remote_capabilities_cb(struct bt_conn *conn, uint8_t status,
				   struct bt_conn_le_cs_capabilities *params)
{
	if (status != BT_HCI_ERR_SUCCESS) {
		LOG_WRN("CS capability exchange failed (HCI status 0x%02x)", status);
		return;
	}
	LOG_INF("CS capabilities exchanged");
	data_out_cs_caps(params);
	k_sem_give(&sem_remote_capabilities_obtained);
}

static void config_create_cb(struct bt_conn *conn, uint8_t status,
			     struct bt_conn_le_cs_config *config)
{
	if (status != BT_HCI_ERR_SUCCESS) {
		LOG_WRN("CS config creation failed (HCI status 0x%02x)", status);
		return;
	}

	cs_config = *config;
	LOG_INF("CS config %u: mode %u, steps %u to %u, mode 0 steps %u, channel map repeat %u",
		config->id, config->mode, config->min_main_mode_steps,
		config->max_main_mode_steps, config->mode_0_steps,
		config->channel_map_repetition);
	data_out_cs_config(config);
	k_sem_give(&sem_config_created);
}

static void security_enable_cb(struct bt_conn *conn, uint8_t status)
{
	if (status != BT_HCI_ERR_SUCCESS) {
		LOG_WRN("CS security enable failed (HCI status 0x%02x)", status);
		return;
	}
	LOG_INF("CS security enabled");
	k_sem_give(&sem_cs_security_enabled);
}

static void procedure_enable_cb(struct bt_conn *conn, uint8_t status,
				struct bt_conn_le_cs_procedure_enable_complete *params)
{
	if (status != BT_HCI_ERR_SUCCESS) {
		LOG_WRN("CS procedure enable failed (HCI status 0x%02x)", status);
		return;
	}

	if (!params->state) {
		data_out_info("event", "stopped");
		return;
	}

	// What the controllers actually agreed, which can differ from what was requested.
	data_out_info("step_mode", STEP_MODE_NAME);
	data_out_cs_procedure(params);
	data_out_info_int("procedure_period_ms",
			  params->procedure_interval * CONN_INTERVAL * 5 / 4);
	data_out_info("event", "ranging");
}

BT_CONN_CB_DEFINE(conn_cb) = {
	.connected = connected_cb,
	.disconnected = disconnected_cb,
	.le_param_req = le_param_req_cb,
	.security_changed = security_changed_cb,
	.le_cs_read_remote_capabilities_complete = remote_capabilities_cb,
	.le_cs_config_complete = config_create_cb,
	.le_cs_security_enable_complete = security_enable_cb,
	.le_cs_procedure_enable_complete = procedure_enable_cb,
	.le_cs_subevent_data_available = subevent_result_cb,
};

static void scan_connecting_error(struct bt_scan_device_info *device_info)
{
	LOG_INF("Connecting failed, scanning again");
	bt_scan_start(BT_SCAN_TYPE_SCAN_PASSIVE);
}

BT_SCAN_CB_INIT(scan_cb, NULL, NULL, scan_connecting_error, NULL);

static const struct bt_le_conn_param conn_param =
	BT_LE_CONN_PARAM_INIT(CONN_INTERVAL, CONN_INTERVAL, 0, BT_GAP_MS_TO_CONN_TIMEOUT(4000));

static int scan_and_connect(void)
{
	struct bt_scan_init_param scan_init = {
		.conn_param = &conn_param,
		.connect_if_match = true,
	};
	int err;

	bt_scan_init(&scan_init);
	bt_scan_cb_register(&scan_cb);

	err = bt_scan_filter_add(BT_SCAN_FILTER_TYPE_NAME, CONFIG_APP_PEER_NAME);
	if (!err) {
		err = bt_scan_filter_enable(BT_SCAN_NAME_FILTER, false);
	}
	if (!err) {
		err = bt_scan_start(BT_SCAN_TYPE_SCAN_PASSIVE);
	}
	if (err) {
		LOG_ERR("Scan setup failed (err %d)", err);
		return err;
	}

	LOG_INF("Scanning for \"%s\"", CONFIG_APP_PEER_NAME);
	data_out_info("event", "scanning");
	k_sem_take(&sem_connected, K_FOREVER);
	return 0;
}

static int ras_subscribe(void)
{
	int err;

	err = bt_ras_rreq_read_features(connection, ras_features_read_cb);
	if (err) {
		return err;
	}
	k_sem_take(&sem_ras_features, K_FOREVER);

	// Real time: the reflector pushes its steps as it goes.
	// On demand: it signals "ready" and the initiator requests them.
	if (ras_feature_bits & RAS_FEAT_REALTIME_RD) {
		return bt_ras_rreq_realtime_rd_subscribe(connection, &latest_peer_steps,
							 ranging_data_cb);
	}

	err = bt_ras_rreq_rd_overwritten_subscribe(connection, ranging_data_overwritten_cb);
	if (!err) {
		err = bt_ras_rreq_rd_ready_subscribe(connection, ranging_data_ready_cb);
	}
	if (!err) {
		err = bt_ras_rreq_on_demand_rd_subscribe(connection);
	}
	if (!err) {
		err = bt_ras_rreq_cp_subscribe(connection);
	}
	return err;
}

static int cs_setup_and_start(void)
{
	int err;

	const struct bt_le_cs_set_default_settings_param default_settings = {
		.enable_initiator_role = true,
		.enable_reflector_role = false,
		.cs_sync_antenna_selection = BT_LE_CS_ANTENNA_SELECTION_OPT_REPETITIVE,
		.max_tx_power = BT_HCI_OP_LE_CS_MAX_MAX_TX_POWER,
	};

	err = bt_le_cs_set_default_settings(connection, &default_settings);
	if (err) {
		LOG_ERR("Default CS settings failed (err %d)", err);
		return err;
	}

	err = ras_subscribe();
	if (err) {
		LOG_ERR("RAS subscribe failed (err %d)", err);
		return err;
	}

	err = bt_le_cs_read_remote_supported_capabilities(connection);
	if (err) {
		LOG_ERR("CS capability exchange failed (err %d)", err);
		return err;
	}
	k_sem_take(&sem_remote_capabilities_obtained, K_FOREVER);

	struct bt_le_cs_create_config_params config_params = {
		.id = CS_CONFIG_ID,
		.mode = CS_CONFIG_MODE,
		.min_main_mode_steps = 2,
		.max_main_mode_steps = 5,
		.main_mode_repetition = 0,
		.mode_0_steps = NUM_MODE_0_STEPS,
		.role = BT_CONN_LE_CS_ROLE_INITIATOR,
		.rtt_type = BT_CONN_LE_CS_RTT_TYPE_AA_ONLY,
		.cs_sync_phy = BT_CONN_LE_CS_SYNC_1M_PHY,
		.channel_map_repetition = 1,
		.channel_selection_type = BT_CONN_LE_CS_CHSEL_TYPE_3B,
		.ch3c_shape = BT_CONN_LE_CS_CH3C_SHAPE_HAT,
		.ch3c_jump = 2,
	};

	// Turns on every channel CS is allowed to use (72 of them).
	bt_le_cs_set_valid_chmap_bits(config_params.channel_map);

	err = bt_le_cs_create_config(connection, &config_params,
				     BT_LE_CS_CREATE_CONFIG_CONTEXT_LOCAL_AND_REMOTE);
	if (err) {
		LOG_ERR("CS config failed (err %d)", err);
		return err;
	}
	k_sem_take(&sem_config_created, K_FOREVER);

	err = bt_le_cs_security_enable(connection);
	if (err) {
		LOG_ERR("CS security failed (err %d)", err);
		return err;
	}
	k_sem_take(&sem_cs_security_enabled, K_FOREVER);

	// Procedure interval is counted in connection events. The procedure
	// itself must end before the next one starts, so it gets one event
	// less. Procedure length is in 0.625 ms units, twice the 1.25 ms
	// connection interval unit.
	uint16_t interval = CONFIG_APP_PROCEDURE_INTERVAL;
	uint16_t max_len = CONN_INTERVAL * 2 * (interval - 1);

	const struct bt_le_cs_set_procedure_parameters_param procedure_params = {
		.config_id = CS_CONFIG_ID,
		.max_procedure_len = max_len,
		.min_procedure_interval = interval,
		.max_procedure_interval = interval,
		.max_procedure_count = 0,
		.min_subevent_len = 16000,
		.max_subevent_len = 16000,
		.tone_antenna_config_selection = BT_LE_CS_TONE_ANTENNA_CONFIGURATION_A1_B1,
		.phy = BT_LE_CS_PROCEDURE_PHY_2M,
		.tx_power_delta = 0x80,
		.preferred_peer_antenna = BT_LE_CS_PROCEDURE_PREFERRED_PEER_ANTENNA_1,
		.snr_control_initiator = BT_LE_CS_SNR_CONTROL_NOT_USED,
		.snr_control_reflector = BT_LE_CS_SNR_CONTROL_NOT_USED,
	};

	err = bt_le_cs_set_procedure_parameters(connection, &procedure_params);
	if (err) {
		LOG_ERR("Procedure parameters failed (err %d)", err);
		return err;
	}

	struct bt_le_cs_procedure_enable_param enable = {
		.config_id = CS_CONFIG_ID,
		.enable = BT_CONN_LE_CS_PROCEDURES_ENABLED,
	};

	return bt_le_cs_procedure_enable(connection, &enable);
}

int ranging_start(ranging_cb_t cb)
{
	int err;

	sample_cb = cb;

	err = scan_and_connect();
	if (err) {
		return err;
	}

	err = bt_conn_set_security(connection, BT_SECURITY_L2);
	if (err) {
		LOG_ERR("Encrypting the link failed (err %d)", err);
		return err;
	}
	k_sem_take(&sem_security, K_FOREVER);

	static struct bt_gatt_exchange_params mtu_params = {.func = mtu_exchange_cb};

	bt_gatt_exchange_mtu(connection, &mtu_params);
	k_sem_take(&sem_mtu_exchange_done, K_FOREVER);

	err = bt_gatt_dm_start(connection, BT_UUID_RANGING_SERVICE, &discovery_cb, NULL);
	if (err) {
		LOG_ERR("Service discovery start failed (err %d)", err);
		return err;
	}
	k_sem_take(&sem_discovery_done, K_FOREVER);

	return cs_setup_and_start();
}
