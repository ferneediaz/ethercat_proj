#include "stepper.h"

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/stepper/stepper_ctrl.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(stepper, LOG_LEVEL_INF);

#define STEPPER_NODE DT_ALIAS(stepper_ctrl)

#if DT_NODE_HAS_STATUS(STEPPER_NODE, okay)

#define STEPPER_ANGLE_MAX 180u

/*
 * Full-step resolution of the 17HS4023: 1.8 degrees per step, so 200 steps
 * per revolution. This assumes the expansion board's MS1/MS2/MS3 DIP switches
 * are all OFF, which is how the board is configured for this build — there is
 * no way to read them back, so the two have to be kept in agreement by hand.
 *
 * Changing the DIP switches without changing this number makes every commanded
 * angle wrong by the microstep factor, and it fails silently: the motor moves
 * smoothly, just not as far as it was told.
 */
#define STEPS_PER_REV 200

/*
 * Time between step pulses, in nanoseconds. 2.5 ms gives 400 steps/s, which
 * is a full 180-degree sweep in a quarter of a second.
 *
 * Deliberately conservative. The datasheet allows 1400 PPS from standstill,
 * but this board is currently running under-driven: Vref measured 0.23 V,
 * which on a DRV8825 (I = Vref x 2) is ~0.46 A against the motor's 1.0 A
 * rating. An under-driven stepper skips steps when accelerated hard, and
 * skipped steps are silent — there is no encoder to notice — so the commanded
 * angle and the real shaft angle would drift apart with no error anywhere.
 * Slow enough not to skip is worth more than fast.
 *
 * If Vref is later set to the rated 0.50 V, this interval can come down —
 * 1 ms (1000 steps/s) was the next value tried. Change both together, and
 * confirm on hardware that the motor still reaches its commanded angle.
 */
#define STEP_INTERVAL_NS 2500000

static const struct device *const ctrl = DEVICE_DT_GET(STEPPER_NODE);

/*
 * ENABLE is active low on the DRV8825 and is held as a plain GPIO rather than
 * through the stepper driver API — see the comment in nema17.overlay. The
 * expansion board exposes it on the EN header.
 *
 * It is asserted once in stepper_init() and never released. That is a
 * deliberate choice, not an oversight: a stepper with de-energised coils can
 * be back-driven by any load, and since there is no encoder the position
 * counter would then be quietly wrong. Holding costs continuous current
 * (~0.46 A/phase at the Vref this board is set to) and the motor runs warm.
 *
 * Releasing when idle needs a definition of idle that this application does
 * not have — a master holding a constant target is not the same as a master
 * that has gone away. Doing it properly means dropping ENABLE on the OP->SAFEOP
 * transition, which belongs in the slave stack rather than here.
 */
#define ENABLE_NODE DT_ALIAS(stepper_enable)
static const struct gpio_dt_spec enable_pin =
	GPIO_DT_SPEC_GET(ENABLE_NODE, gpios);

bool stepper_present(void)
{
	return true;
}

/* Degrees to full steps, rounded to nearest rather than truncated. At 1.8
 * degrees per step the quantisation is coarse enough that always rounding
 * down would bias every commanded angle low by up to a full step. */
static int32_t angle_to_steps(uint16_t degrees)
{
	return (int32_t)(((uint32_t)degrees * STEPS_PER_REV + 180u) / 360u);
}

int stepper_init(void)
{
	int rc;

	if (!device_is_ready(ctrl)) {
		LOG_ERR("stepper motion controller is not ready");
		return -ENODEV;
	}

	if (!gpio_is_ready_dt(&enable_pin)) {
		LOG_ERR("stepper ENABLE GPIO is not ready");
		return -ENODEV;
	}

	/* Inactive first, asserted at the end of this function: there is no
	 * reason to energise the coils while the controller is still being
	 * configured, and a driver that is enabled before its reference
	 * position is set would hold at whatever angle the shaft was left in
	 * by the previous run. (It cannot move early — move_by() returns
	 * -EINVAL until the step interval is set, so there is no default rate
	 * to run away at.) */
	rc = gpio_pin_configure_dt(&enable_pin, GPIO_OUTPUT_INACTIVE);
	if (rc < 0) {
		LOG_ERR("stepper ENABLE configure failed: %d", rc);
		return rc;
	}

	rc = stepper_ctrl_set_microstep_interval(ctrl, STEP_INTERVAL_NS);
	if (rc < 0) {
		LOG_ERR("stepper set_microstep_interval failed: %d", rc);
		return rc;
	}

	/* Wherever the shaft is now is 0 degrees. See the note in stepper.h. */
	rc = stepper_ctrl_set_reference_position(ctrl, 0);
	if (rc < 0) {
		LOG_ERR("stepper set_reference_position failed: %d", rc);
		return rc;
	}

	rc = gpio_pin_set_dt(&enable_pin, 1);
	if (rc < 0) {
		LOG_ERR("stepper ENABLE assert failed: %d", rc);
		return rc;
	}

	LOG_INF("Stepper ready: %d steps/rev, %u us between steps, "
		"0..%u deg = 0..%d steps",
		STEPS_PER_REV, (unsigned)(STEP_INTERVAL_NS / 1000u),
		STEPPER_ANGLE_MAX, angle_to_steps(STEPPER_ANGLE_MAX));
	return 0;
}

int stepper_set_angle(uint16_t degrees)
{
	static int32_t commanded = INT32_MIN;
	int32_t steps;
	int rc;

	if (degrees > STEPPER_ANGLE_MAX) {
		degrees = STEPPER_ANGLE_MAX;
	}
	steps = angle_to_steps(degrees);

	/*
	 * Re-issuing the same target must not reach the driver.
	 *
	 * The slave stacks call the apply callback every cycle while in OP,
	 * not only when the target changes, and Zephyr's step-dir controller
	 * toggles the STEP pin SYNCHRONOUSLY inside move_by() — start_stepping()
	 * arms the timer and then calls stepper_handle_timing_signal() directly.
	 * So an unguarded call injects an extra edge every 10 ms on top of the
	 * timer's own 1.25 ms half-period.
	 *
	 * A move of 180 degrees takes 250 ms, so that is ~25 out-of-band
	 * toggles per sweep leg. Any that land just after the timer raised STEP
	 * cut the pulse below the DRV8825's 1.9 us minimum: the driver counts a
	 * step the motor never took. With no encoder, commanded and real shaft
	 * angle then diverge permanently and silently.
	 *
	 * move_to() is absolute, so skipping a repeat is not merely safe — the
	 * controller is already travelling to that exact position.
	 */
	if (steps == commanded) {
		return 0;
	}

	rc = stepper_ctrl_move_to(ctrl, steps);
	if (rc < 0) {
		LOG_ERR("stepper move_to failed for %u deg: %d", degrees, rc);
		return rc;
	}
	commanded = steps;
	return 0;
}

#else /* no nema17 overlay */

bool stepper_present(void)
{
	return false;
}

int stepper_init(void)
{
	return 0;
}

int stepper_set_angle(uint16_t degrees)
{
	ARG_UNUSED(degrees);
	return 0;
}

#endif
