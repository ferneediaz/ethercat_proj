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

#include "ecat_slave.h"
#include "ax58100.h"
#if defined(CONFIG_ESC_USE_SOES)
#include "soes_app.h"
#endif
#include "servo.h"

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
	{"AL Status", ESC_REG_AL_STATUS, ESC_AL_INIT, true},
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

/*
 * Step to the three angles that matter first, holding each long enough to
 * see and measure, then sweep continuously.
 *
 * The discrete steps come first deliberately: a servo that buzzes but does
 * not hold position, or that slams to one end and stays there, is a power
 * or pulse-width problem, and that is far easier to spot against a held
 * angle than in the middle of a continuous sweep.
 */
static void servo_demo(void)
{
	static const uint16_t steps[] = {90, 0, 90, 180, 90};

	for (size_t i = 0; i < ARRAY_SIZE(steps); i++) {
		LOG_INF("servo -> %u deg", steps[i]);
		servo_set_angle(steps[i]);
		k_sleep(K_MSEC(1200));
	}

	LOG_INF("sweeping 0..180 continuously; ESC heartbeat every ~5 s");

	uint16_t angle = 0;
	int step = 2;
	int ticks = 0;

	while (1) {
		servo_set_angle(angle);

		if (angle == 180) {
			step = -2;
		} else if (angle == 0) {
			step = 2;
		}
		angle = (uint16_t)(angle + step);

		/* Keep proving the SPI link while the servo runs: a servo
		 * browning out the rail shows up here as the ESC going
		 * unreadable, which is the failure this wiring invites. */
		if (++ticks >= 250) {
			ticks = 0;
			LOG_INF("heartbeat: Type=0x%02x angle=%u",
				esc_read8(ESC_REG_TYPE), angle);
		}
		k_sleep(K_MSEC(20));
	}
}

/* Adapter: the actuator API returns an error code, the slave callback does
 * not. A failed PWM write is already logged by servo.c, and there is nothing
 * useful the state machine could do about it mid-cycle — dropping to SAFEOP
 * because one frame's pulse did not take would be worse than holding. */
static void apply_servo_angle(uint16_t degrees)
{
	(void)servo_set_angle(degrees);
}

int main(void)
{
	LOG_INF("EtherCAT servo node — ESP32-S3 host, milestone 4a");

	if (IS_ENABLED(CONFIG_ESC_DIAG)) {
		/*
		 * Runs BEFORE esc_init() and never returns. Order matters: the
		 * diagnostics read the SPI pins as plain GPIOs, and once the SPI
		 * controller has claimed them it keeps driving MOSI and SCLK.
		 * On the ESP32 a later gpio_pin_configure() does not detach the
		 * peripheral's output routing, so initialising SPI first makes
		 * GPIO 11 and 12 report as externally driven when nothing is
		 * attached to them at all.
		 */
		esc_diag_run();
		return 0;
	}

	if (esc_init() != 0) {
		return 0;
	}

	/* The ESC needs a moment after power-up to load its EEPROM before
	 * PDI access is meaningful. */
	k_sleep(K_MSEC(100));

	/*
	 * Discard one read before checking anything.
	 *
	 * The first PDI-SPI access after reset returns 0xff on this hardware
	 * even when every later read is correct — the ESC is still settling
	 * and has not driven MISO yet. Without this, register 0x0000 fails
	 * while 0x0140/0x0141/0x0150 all pass, which looks like a wiring
	 * fault on one line rather than a timing artifact on the first
	 * transaction.
	 */
	(void)esc_read8(ESC_REG_TYPE);

	int failures = run_checks();

	if (failures == 0) {
		LOG_INF("MILESTONE 4a PASS — SPI link to the AX58100 works.");
		LOG_INF("Next: integrate SOES (see docs/bringup-checklist.md).");
	} else {
		LOG_ERR("%d check(s) failed.", failures);
		hint_on_total_failure();
	}

	if (IS_ENABLED(CONFIG_SERVO_LOCAL_SWEEP)) {
		/*
		 * MILESTONE 5a — the actuator moves under its own steam, with
		 * the bus ignored entirely. Diagnostic only: it separates a
		 * broken actuator from a broken slave stack.
		 */
		if (servo_present() && servo_init() == 0) {
			servo_demo();
		}
		LOG_ERR("CONFIG_SERVO_LOCAL_SWEEP set but no actuator present.");
		return 0;
	}

	if (IS_ENABLED(CONFIG_ESC_USE_SOES)) {
		/* Hands the AL state machine, the CoE mailbox and the PDO
		 * mapping to SOES. Never returns. */
#if defined(CONFIG_ESC_USE_SOES)
		soes_app_run(servo_present() ? apply_servo_angle : NULL);
#endif
		return 0;
	}

	/*
	 * MILESTONE 4b — run as an EtherCAT slave.
	 *
	 * The angle now arrives as process data instead of a local counter.
	 * Polling is fast and unconditional: the master decides the cycle
	 * time, and the working counter it checks only increments when we
	 * have actually read the outputs and written the inputs.
	 */
	if (servo_present()) {
		servo_init();
	}
	if (ecat_slave_init(servo_present() ? apply_servo_angle : NULL) != 0) {
		return 0;
	}

	uint8_t last_logged = 0xff;

	while (1) {
		ecat_slave_poll();

		uint8_t st = ecat_slave_state();

		if (st != last_logged) {
			LOG_INF("AL state: %s", ecat_state_name(st));
			last_logged = st;
		}
		k_sleep(K_MSEC(1));
	}
	return 0;
}
