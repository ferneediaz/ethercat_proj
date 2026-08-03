#include "servo.h"

#include <zephyr/devicetree.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(servo, LOG_LEVEL_INF);

#define SERVO_NODE DT_ALIAS(actuator_servo)

#if DT_NODE_HAS_STATUS(SERVO_NODE, okay)

#define SERVO_ANGLE_MAX 180u

static const struct pwm_dt_spec servo_pwm = PWM_DT_SPEC_GET(SERVO_NODE);

/* Last angle written. This is all a servo build can report as "actual" —
 * see the note on servo_actual_angle() in servo.h. */
static uint16_t last_commanded;
static const uint32_t min_pulse_ns = DT_PROP(SERVO_NODE, min_pulse);
static const uint32_t max_pulse_ns = DT_PROP(SERVO_NODE, max_pulse);

bool servo_present(void)
{
	return true;
}

int servo_init(void)
{
	if (!pwm_is_ready_dt(&servo_pwm)) {
		LOG_ERR("PWM channel for the servo is not ready");
		return -ENODEV;
	}

	LOG_INF("Servo ready: %u ms frame, %u..%u us pulse",
		(unsigned)(servo_pwm.period / 1000000u),
		(unsigned)(min_pulse_ns / 1000u), (unsigned)(max_pulse_ns / 1000u));
	return 0;
}

int servo_set_angle(uint16_t degrees)
{
	if (degrees > SERVO_ANGLE_MAX) {
		degrees = SERVO_ANGLE_MAX;
	}

	/*
	 * Same mapping as angle_to_pulse_ms() on the master side, in integer
	 * arithmetic. The span is 2000 us over 180 degrees, so the widest
	 * intermediate here is 2000000 * 180 — well inside uint32_t, and
	 * exact, which a float would not be.
	 */
	const uint32_t span_ns = max_pulse_ns - min_pulse_ns;
	const uint32_t pulse_ns =
		min_pulse_ns + (uint32_t)((uint64_t)span_ns * degrees / SERVO_ANGLE_MAX);

	int rc = pwm_set_pulse_dt(&servo_pwm, pulse_ns);

	if (rc < 0) {
		LOG_ERR("pwm_set_pulse failed for %u deg: %d", degrees, rc);
		return rc;
	}
	last_commanded = degrees;
	return rc;
}

int servo_actual_angle(uint16_t *degrees)
{
	if (degrees == NULL) {
		return -EINVAL;
	}
	*degrees = last_commanded;
	return 0;
}

#else /* no sg90 overlay */

bool servo_present(void)
{
	return false;
}

int servo_init(void)
{
	return 0;
}

int servo_set_angle(uint16_t degrees)
{
	ARG_UNUSED(degrees);
	return 0;
}

int servo_actual_angle(uint16_t *degrees)
{
	ARG_UNUSED(degrees);
	return -ENODEV;
}

#endif
