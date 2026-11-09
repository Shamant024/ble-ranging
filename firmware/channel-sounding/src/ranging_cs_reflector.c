// Copyright (c) 2024 Nordic Semiconductor ASA
// Modifications copyright (c) 2026 the ble-ranging authors
// SPDX-License-Identifier: LicenseRef-Nordic-5-Clause

// Channel Sounding reflector, based on Nordic's ras_reflector sample and reduced
// to the parts needed here.
//
// The reflector advertises, waits for the initiator to connect, then
// answers every sounding step. After each procedure the Ranging Service
// (RAS) sends its own step data back to the initiator over GATT, which is
// what lets the initiator work out the distance.

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/cs.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>
#include <bluetooth/services/ras.h>
#include <dk_buttons_and_leds.h>

#include "data_out.h"
#include "ranging.h"

LOG_MODULE_REGISTER(cs_reflector, LOG_LEVEL_INF);

#define LINK_LED DK_LED1

static K_SEM_DEFINE(sem_connected, 0, 1);
static K_SEM_DEFINE(sem_config, 0, 1);

static struct bt_conn *connection;

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL, BT_UUID_16_ENCODE(BT_UUID_RANGING_SERVICE_VAL)),
	// The initiator filters on this exact name, both sides read it from the same Kconfig.
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_APP_PEER_NAME, sizeof(CONFIG_APP_PEER_NAME) - 1),
};

static void connected_cb(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("Connected to %s (err 0x%02X)", addr, err);

	if (err) {
		// Advertising has stopped by now. Reboot so it starts again without anyone pressing reset.
		sys_reboot(SYS_REBOOT_COLD);
	}

	connection = bt_conn_ref(conn);
	dk_set_led_on(LINK_LED);
	data_out_info("event", "connected");
	k_sem_give(&sem_connected);
}

static void disconnected_cb(struct bt_conn *conn, uint8_t reason)
{
	LOG_INF("Disconnected (reason 0x%02X)", reason);
	data_out_info_int("disconnect_reason", reason);

	bt_conn_unref(conn);
	connection = NULL;
	dk_set_led_off(LINK_LED);

	// A reboot returns the CS state to a known start, which is simpler than
	// tearing it down by hand. The initiator does the same.
	sys_reboot(SYS_REBOOT_COLD);
}

static void config_create_cb(struct bt_conn *conn, uint8_t status,
			     struct bt_conn_le_cs_config *config)
{
	if (status != BT_HCI_ERR_SUCCESS) {
		LOG_WRN("CS config creation failed (HCI status 0x%02x)", status);
		return;
	}

	LOG_INF("CS config %u created by the initiator, mode %u", config->id, config->mode);
	data_out_cs_config(config);
	k_sem_give(&sem_config);
}

static void security_changed_cb(struct bt_conn *conn, bt_security_t level,
				enum bt_security_err err)
{
	if (!err) {
		data_out_conn(conn);
	}
}

static void security_enable_cb(struct bt_conn *conn, uint8_t status)
{
	if (status == BT_HCI_ERR_SUCCESS) {
		LOG_INF("CS security enabled");
	} else {
		LOG_WRN("CS security enable failed (HCI status 0x%02x)", status);
	}
}

static void procedure_enable_cb(struct bt_conn *conn, uint8_t status,
				struct bt_conn_le_cs_procedure_enable_complete *params)
{
	if (status != BT_HCI_ERR_SUCCESS) {
		LOG_WRN("CS procedure enable failed (HCI status 0x%02x)", status);
		return;
	}

	LOG_INF("CS procedures %s", params->state ? "running" : "stopped");
	if (params->state) {
		data_out_cs_procedure(params);
	}
	data_out_info("event", params->state ? "ranging" : "stopped");
}

#if defined(CONFIG_APP_POWER_BANK_LOAD)
// Runs at the lowest priority, so Bluetooth and logging always run first. Its
// only job is to stop the CPU from sleeping, which keeps the current draw up.
static void load_thread(void)
{
	while (1) {
		k_yield();
	}
}

K_THREAD_DEFINE(load_tid, 512, load_thread, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
#endif

BT_CONN_CB_DEFINE(conn_cb) = {
	.connected = connected_cb,
	.disconnected = disconnected_cb,
	.security_changed = security_changed_cb,
	.le_cs_config_complete = config_create_cb,
	.le_cs_security_enable_complete = security_enable_cb,
	.le_cs_procedure_enable_complete = procedure_enable_cb,
};

int ranging_start(ranging_cb_t cb)
{
	int err;

	ARG_UNUSED(cb);

	if (IS_ENABLED(CONFIG_APP_POWER_BANK_LOAD)) {
		// LED1 stays the link LED, the other three are spare load.
		dk_set_led_on(DK_LED2);
		dk_set_led_on(DK_LED3);
		dk_set_led_on(DK_LED4);
	}

	err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, ad, ARRAY_SIZE(ad), NULL, 0);
	if (err) {
		LOG_ERR("Advertising failed to start (err %d)", err);
		return err;
	}

	LOG_INF("Advertising as \"%s\"", CONFIG_APP_PEER_NAME);
	data_out_info("event", "advertising");

	k_sem_take(&sem_connected, K_FOREVER);

	const struct bt_le_cs_set_default_settings_param default_settings = {
		.enable_initiator_role = false,
		.enable_reflector_role = true,
		.cs_sync_antenna_selection = BT_LE_CS_ANTENNA_SELECTION_OPT_REPETITIVE,
		.max_tx_power = BT_HCI_OP_LE_CS_MAX_MAX_TX_POWER,
	};

	err = bt_le_cs_set_default_settings(connection, &default_settings);
	if (err) {
		LOG_ERR("Failed to set default CS settings (err %d)", err);
		return err;
	}

	// The initiator creates the config. The reflector waits for it, then sets wide
	// limits so that the timing chosen by the initiator always fits.
	k_sem_take(&sem_config, K_FOREVER);

	const struct bt_le_cs_set_procedure_parameters_param procedure_params = {
		.config_id = 0,
		.max_procedure_len = 1000,
		.min_procedure_interval = 1,
		.max_procedure_interval = 100,
		.max_procedure_count = 0,
		.min_subevent_len = 10000,
		.max_subevent_len = 75000,
		.tone_antenna_config_selection = BT_LE_CS_TONE_ANTENNA_CONFIGURATION_A1_B1,
		.phy = BT_LE_CS_PROCEDURE_PHY_2M,
		.tx_power_delta = 0x80,
		.preferred_peer_antenna = BT_LE_CS_PROCEDURE_PREFERRED_PEER_ANTENNA_1,
		.snr_control_initiator = BT_LE_CS_SNR_CONTROL_NOT_USED,
		.snr_control_reflector = BT_LE_CS_SNR_CONTROL_NOT_USED,
	};

	err = bt_le_cs_set_procedure_parameters(connection, &procedure_params);
	if (err) {
		LOG_ERR("Failed to set procedure parameters (err %d)", err);
		return err;
	}

	return 0;
}
