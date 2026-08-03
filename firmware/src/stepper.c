#include "stepper.h"

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/stepper/stepper_ctrl.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(stepper, LOG_LEVEL_INF);

#define AXIS_NODE DT_ALIAS(actuator_stepper)

#if DT_NODE_HAS_STATUS(AXIS_NODE, okay)

/*
 * Everything below comes from the `stepper-axis` node — see
 * dts/bindings/stepper-axis.yaml and overlays/nema17.overlay. Nothing about
 * the motor, the driver or the step rate is written into this file, so fitting
 * a different motor or moving the microstep DIP switches is a devicetree edit
 * rather than a code change.
 */
#define FULL_STEPS_PER_REV DT_PROP(AXIS_NODE, full_steps_per_revolution)
#define MICROSTEP_FACTOR   DT_PROP(AXIS_NODE, microstep_factor)
#define STEP_INTERVAL_NS   DT_PROP(AXIS_NODE, step_interval_ns)
#define STEPPER_ANGLE_MAX  DT_PROP(AXIS_NODE, max_angle)

/* Microsteps per revolution: the unit the controller actually counts in. */
#define STEPS_PER_REV ((uint32_t)FULL_STEPS_PER_REV * (uint32_t)MICROSTEP_FACTOR)

#define DEGREES_PER_REV 360u

BUILD_ASSERT(FULL_STEPS_PER_REV > 0, "full-steps-per-revolution must be positive");
BUILD_ASSERT(MICROSTEP_FACTOR > 0, "microstep-factor must be at least 1 (1 = full step)");
BUILD_ASSERT(STEP_INTERVAL_NS > 0, "step-interval-ns must be positive");
BUILD_ASSERT(STEPPER_ANGLE_MAX > 0 && STEPPER_ANGLE_MAX <= UINT16_MAX,
	     "max-angle must be a positive angle in degrees");
/* angle_to_steps() does the whole conversion in uint32_t. */
BUILD_ASSERT((uint64_t)STEPPER_ANGLE_MAX * STEPS_PER_REV < UINT32_MAX,
	     "max-angle x steps/rev overflows the degrees-to-steps conversion");

static const struct device *const ctrl = DEVICE_DT_GET(DT_PHANDLE(AXIS_NODE, controller));

#if DT_NODE_HAS_PROP(AXIS_NODE, enable_gpios)
/*
 * ENABLE is optional: a board that ties it to ground still works, it simply
 * cannot release the motor. Where it exists, the devicetree flags are what
 * make "logically active" mean "energised" (GPIO_ACTIVE_LOW for a DRV8825's
 * nENABLE), so nothing here has to know the polarity.
 */
static const struct gpio_dt_spec enable_pin = GPIO_DT_SPEC_GET(AXIS_NODE, enable_gpios);
#define HAS_ENABLE_PIN 1
#else
#define HAS_ENABLE_PIN 0
#endif

/*
 * Last position handed to the controller, and whether it means anything yet.
 *
 * Re-issuing a move mid-flight is not free: stepper_ctrl_move_to() restarts
 * the controller's timing source and drives one STEP edge synchronously, so a
 * caller that re-sent an unchanged target every bus cycle would truncate
 * whichever pulse happened to be in the air. A pulse cut below the driver's
 * minimum high time is a step the silicon ignores but the controller counts,
 * and with no encoder on this shaft that error is silent and permanent.
 *
 * Callers are expected to command only on change (see ecat_slave.c), but the
 * guard lives here as well so that no future caller has to know that.
 */
static int32_t commanded_steps;
static bool commanded_valid;

bool stepper_present(void)
{
	return true;
}

/* Degrees to microsteps, rounded to nearest rather than truncated. At full
 * step the quantisation is coarse enough (1.8 degrees) that always rounding
 * down would bias every commanded angle low by up to a whole step. */
static int32_t angle_to_steps(uint16_t degrees)
{
	return (int32_t)(((uint32_t)degrees * STEPS_PER_REV + DEGREES_PER_REV / 2u) /
			 DEGREES_PER_REV);
}

int stepper_init(void)
{
	int rc;

	commanded_valid = false;

	if (!device_is_ready(ctrl)) {
		LOG_ERR("stepper motion controller is not ready");
		return -ENODEV;
	}

#if HAS_ENABLE_PIN
	if (!gpio_is_ready_dt(&enable_pin)) {
		LOG_ERR("stepper ENABLE GPIO is not ready");
		return -ENODEV;
	}

	/* De-energised first. The pin floats until it is configured and a
	 * DRV8825's nENABLE has an internal pulldown, so the motor has been
	 * sitting energised since power-on with DIR and STEP in whatever state
	 * the controller driver left them. Leave that state before doing
	 * anything else. */
	rc = gpio_pin_configure_dt(&enable_pin, GPIO_OUTPUT_INACTIVE);
	if (rc < 0) {
		LOG_ERR("stepper ENABLE configure failed: %d", rc);
		return rc;
	}
#endif

	/* Must precede any move: with no interval set the controller rejects
	 * move_to() outright with -EINVAL rather than picking a default rate. */
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

#if HAS_ENABLE_PIN
	/*
	 * Energise, and stay energised.
	 *
	 * Releasing between moves would run cooler, but this axis has no
	 * encoder and no home switch: holding torque is the only thing that
	 * keeps the boot-time zero meaning anything a minute later. A released
	 * stepper that gets nudged comes back reporting an angle it is not at,
	 * and nothing downstream could tell. Heat is the cheaper problem, and
	 * the current limit is set well below the motor's rating to pay for it
	 * (see overlays/nema17.overlay).
	 */
	rc = gpio_pin_set_dt(&enable_pin, 1);
	if (rc < 0) {
		LOG_ERR("stepper ENABLE assert failed: %d", rc);
		return rc;
	}
#endif

	LOG_INF("Stepper ready: %u full steps/rev x%u microstepping = %u steps/rev, "
		"%u us between steps, 0..%u deg = 0..%d steps",
		(unsigned)FULL_STEPS_PER_REV, (unsigned)MICROSTEP_FACTOR,
		(unsigned)STEPS_PER_REV, (unsigned)(STEP_INTERVAL_NS / 1000u),
		(unsigned)STEPPER_ANGLE_MAX,
		angle_to_steps((uint16_t)STEPPER_ANGLE_MAX));
	return 0;
}

int stepper_set_angle(uint16_t degrees)
{
	int rc;

	if (degrees > (uint16_t)STEPPER_ANGLE_MAX) {
		degrees = (uint16_t)STEPPER_ANGLE_MAX;
	}

	const int32_t target = angle_to_steps(degrees);

	/* Already on its way there — see the note on commanded_steps. */
	if (commanded_valid && target == commanded_steps) {
		return 0;
	}

	rc = stepper_ctrl_move_to(ctrl, target);
	if (rc < 0) {
		LOG_ERR("stepper move_to failed for %u deg: %d", degrees, rc);
		return rc;
	}

	commanded_steps = target;
	commanded_valid = true;
	return 0;
}

#else /* no stepper axis in the devicetree */

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
