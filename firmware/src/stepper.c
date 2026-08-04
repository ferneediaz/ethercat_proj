#include "stepper.h"

#include "step_math.h"

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

/*
 * Runtime copies of the two devicetree properties that can be changed over
 * CoE. Initialised from the devicetree so the object dictionary's defaults and
 * the hardware description cannot disagree.
 */
static uint32_t step_interval_ns = STEP_INTERVAL_NS;
static uint16_t max_angle_deg = STEPPER_ANGLE_MAX;

/*
 * Bounds for the writable parameters.
 *
 * The lower step interval is not arbitrary: the controller uses half of it as
 * the STEP high time, and a DRV8825 ignores a pulse shorter than 1.9 us. 200 us
 * leaves two orders of magnitude of margin while still allowing 5000 steps/s,
 * which is already faster than this under-driven motor can follow. The upper
 * bound only stops a master from stalling the axis for a minute per step.
 */
#define STEP_INTERVAL_MIN_NS 200000u
#define STEP_INTERVAL_MAX_NS 50000000u
#define MAX_ANGLE_MIN_DEG 1u
#define MAX_ANGLE_MAX_DEG 360u

bool stepper_present(void)
{
	return true;
}

/* The arithmetic lives in step_math.h so it can be unit tested on the host,
 * away from the devicetree and the motor. */
static int32_t angle_to_steps(uint16_t degrees)
{
	return step_math_angle_to_steps(degrees, STEPS_PER_REV);
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
	rc = stepper_ctrl_set_microstep_interval(ctrl, step_interval_ns);
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

	if (degrees > max_angle_deg) {
		degrees = max_angle_deg;
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

static uint16_t steps_to_angle(int32_t steps)
{
	return step_math_steps_to_angle(steps, STEPS_PER_REV, max_angle_deg);
}

int stepper_actual_angle(uint16_t *degrees)
{
	int32_t steps = 0;
	int rc;

	if (degrees == NULL) {
		return -EINVAL;
	}

	rc = stepper_ctrl_get_actual_position(ctrl, &steps);
	if (rc < 0) {
		return rc;
	}

	*degrees = steps_to_angle(steps);
	return 0;
}

uint32_t stepper_step_interval_ns(void)
{
	return step_interval_ns;
}

uint16_t stepper_max_angle(void)
{
	return max_angle_deg;
}

uint16_t stepper_steps_per_rev(void)
{
	return (uint16_t)STEPS_PER_REV;
}

uint8_t stepper_microstep_factor(void)
{
	return (uint8_t)MICROSTEP_FACTOR;
}

int stepper_set_step_interval(uint32_t ns)
{
	int rc;

	if (ns < STEP_INTERVAL_MIN_NS || ns > STEP_INTERVAL_MAX_NS) {
		LOG_WRN("step interval %u ns rejected (allowed %u..%u)",
			ns, STEP_INTERVAL_MIN_NS, STEP_INTERVAL_MAX_NS);
		return -ERANGE;
	}

	rc = stepper_ctrl_set_microstep_interval(ctrl, ns);
	if (rc < 0) {
		LOG_ERR("set_microstep_interval(%u) failed: %d", ns, rc);
		return rc;
	}
	step_interval_ns = ns;
	LOG_INF("step interval now %u ns (%u steps/s)", ns, 1000000000u / ns);
	return 0;
}

int stepper_set_max_angle(uint16_t degrees)
{
	if (degrees < MAX_ANGLE_MIN_DEG || degrees > MAX_ANGLE_MAX_DEG) {
		LOG_WRN("max angle %u deg rejected (allowed %u..%u)", degrees,
			MAX_ANGLE_MIN_DEG, MAX_ANGLE_MAX_DEG);
		return -ERANGE;
	}
	max_angle_deg = degrees;
	LOG_INF("max angle now %u deg", degrees);
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

int stepper_actual_angle(uint16_t *degrees)
{
	ARG_UNUSED(degrees);
	return -ENODEV;
}

uint32_t stepper_step_interval_ns(void)
{
	return 0;
}

uint16_t stepper_max_angle(void)
{
	return 0;
}

uint16_t stepper_steps_per_rev(void)
{
	return 0;
}

uint8_t stepper_microstep_factor(void)
{
	return 0;
}

int stepper_set_step_interval(uint32_t ns)
{
	ARG_UNUSED(ns);
	return -ENODEV;
}

int stepper_set_max_angle(uint16_t degrees)
{
	ARG_UNUSED(degrees);
	return -ENODEV;
}

#endif
