// Copyright (c) 2026 the ble-ranging authors
// SPDX-License-Identifier: MIT

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include <dk_buttons_and_leds.h>

#include "data_out.h"
#include "ranging.h"

LOG_MODULE_REGISTER(app_main, LOG_LEVEL_INF);

// LED1 shows the link state and is driven by the role files. LED2 toggles once
// per distance result, so ranging can be seen working without a host attached.
#define SAMPLE_LED DK_LED2

static void on_sample(const struct range_sample *s)
{
	static bool led;

	data_out_sample(s);
	led = !led;
	dk_set_led(SAMPLE_LED, led);
}

int main(void)
{
	int err;

	data_out_build_info();
	dk_leds_init();

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("Bluetooth init failed (err %d)", err);
		return 0;
	}

	err = ranging_start(on_sample);
	if (err) {
		LOG_ERR("Ranging start failed (err %d), rebooting", err);
		// The board may be running from a power bank with nobody near it to press reset.
		k_sleep(K_SECONDS(2));
		sys_reboot(SYS_REBOOT_COLD);
	}

	return 0;
}
