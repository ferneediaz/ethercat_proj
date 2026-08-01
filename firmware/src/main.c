/*
 * MILESTONE 4a — prove the SPI link to the AX58100.
 *
 * Every value checked here was already read over EtherCAT from the Pi
 * with `servo_master eth0 regs`, so this is not a blind bring-up: there
 * are known-good expected answers for each register. If SPI is wired and
 * configured correctly these must agree.
 *
 * Do not add SOES until this passes.
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "esc.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

struct reg_check {
	const char *name;
	uint16_t adr;
	uint8_t expect;
	bool has_expect;
};

static const struct reg_check checks[] = {
	{"Type", ESC_REG_TYPE, 0xc8, true},
	{"Revision", ESC_REG_REVISION, 0x00, false},
	{"PDI Control", ESC_REG_PDI_CONTROL, 0x05, true},
	{"ESC Config", ESC_REG_ESC_CONFIG, 0x0e, true},
	{"PDI Config", ESC_REG_PDI_CONFIG, 0x03, true},
	{"AL Status", ESC_REG_AL_STATUS, 0x00, false},
};

static int run_checks(void)
{
	int failures = 0;

	for (size_t i = 0; i < ARRAY_SIZE(checks); i++) {
		int v = esc_read8(checks[i].adr);

		if (v < 0) {
			LOG_ERR("0x%04x %-12s READ FAILED (%d)",
				checks[i].adr, checks[i].name, v);
			failures++;
			continue;
		}

		if (!checks[i].has_expect) {
			LOG_INF("0x%04x %-12s 0x%02x", checks[i].adr,
				checks[i].name, v);
		} else if (v == checks[i].expect) {
			LOG_INF("0x%04x %-12s 0x%02x  PASS", checks[i].adr,
				checks[i].name, v);
		} else {
			LOG_ERR("0x%04x %-12s 0x%02x  FAIL (expected 0x%02x)",
				checks[i].adr, checks[i].name, v,
				checks[i].expect);
			failures++;
		}
	}
	return failures;
}

/* All-zero or all-ones across every register is the classic signature of
 * a dead SPI link rather than a misconfigured ESC, and it has a small set
 * of likely causes worth printing rather than making the user guess. */
static void hint_on_total_failure(void)
{
	int type = esc_read8(ESC_REG_TYPE);

	if (type != 0x00 && type != 0xff) {
		return;
	}
	LOG_ERR("Register 0x0000 reads 0x%02x — that is a dead SPI link, not "
		"a configuration problem.", type);
	LOG_ERR("Most likely, in order: (1) no common ground between the "
		"ESP32 (USB powered) and the module (Pi powered);");
	LOG_ERR("(2) MISO and MOSI swapped — they are adjacent on the "
		"module header; (3) module unpowered — check the Pi is on.");
}

int main(void)
{
	LOG_INF("EtherCAT servo node — ESP32-S3 host, milestone 4a");

	if (esc_init() != 0) {
		return 0;
	}

	/* The ESC needs a moment after power-up to load its EEPROM before
	 * PDI access is meaningful. */
	k_sleep(K_MSEC(100));

	int failures = run_checks();

	if (failures == 0) {
		LOG_INF("MILESTONE 4a PASS — SPI link to the AX58100 works.");
		LOG_INF("Next: integrate SOES (see docs/bringup-checklist.md).");
	} else {
		LOG_ERR("%d check(s) failed.", failures);
		hint_on_total_failure();
	}

	/* Keep re-reading so the link can be watched live while wiring is
	 * poked at — a loose jumper shows up immediately. */
	while (1) {
		k_sleep(K_SECONDS(5));
		int type = esc_read8(ESC_REG_TYPE);

		LOG_INF("heartbeat: Type=0x%02x AL Status=0x%02x", type,
			esc_read8(ESC_REG_AL_STATUS));
	}
	return 0;
}
