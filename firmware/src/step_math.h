/*
 * Degrees <-> microsteps, as pure arithmetic.
 *
 * Split out of stepper.c so it can be tested on the host. Everything else in
 * that file needs a devicetree, a Zephyr device and a real motor; this part
 * needs none of them, and it is the part where an off-by-one silently
 * mis-positions the axis rather than failing loudly.
 *
 * steps_per_rev is a parameter rather than a constant on purpose: it is
 * full-steps-per-revolution x microstep-factor, both devicetree properties,
 * and passing it in is what lets a test explore values this board is not
 * currently set to.
 */
#ifndef STEP_MATH_H
#define STEP_MATH_H

#include <stdint.h>

#define STEP_MATH_DEGREES_PER_REV 360u

/*
 * Rounds to nearest rather than truncating. At full step the quantisation is
 * 1.8 degrees, coarse enough that always rounding down would bias every
 * commanded angle low by up to a whole step — an error that accumulates in
 * the operator's mental model, not in the hardware, which is worse.
 */
static inline int32_t step_math_angle_to_steps(uint16_t degrees,
					       uint32_t steps_per_rev)
{
	return (int32_t)(((uint32_t)degrees * steps_per_rev +
			  STEP_MATH_DEGREES_PER_REV / 2u) /
			 STEP_MATH_DEGREES_PER_REV);
}

/*
 * The inverse, clamped to max_angle. Negative step counts report 0: the
 * controller can be commanded below its reference position, but this axis has
 * no meaning there, and a wrapped unsigned angle would be far more confusing
 * than a clamp.
 */
static inline uint16_t step_math_steps_to_angle(int32_t steps,
						uint32_t steps_per_rev,
						uint16_t max_angle)
{
	if (steps <= 0 || steps_per_rev == 0u) {
		return 0;
	}

	uint32_t deg = ((uint32_t)steps * STEP_MATH_DEGREES_PER_REV +
			steps_per_rev / 2u) /
		       steps_per_rev;

	return (deg > (uint32_t)max_angle) ? max_angle : (uint16_t)deg;
}

#endif /* STEP_MATH_H */
