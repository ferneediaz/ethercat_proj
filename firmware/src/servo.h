/*
 * SG90 hobby servo on LEDC PWM (phase 5).
 *
 * Built into every image, but only active when the sg90 overlay supplies
 * the devicetree node:
 *
 *   ./scripts/fw.sh build sg90
 *
 * Without the overlay servo_present() is false and the other calls are
 * no-ops, so the milestone 4a image still builds and runs unchanged.
 */
#ifndef SERVO_H
#define SERVO_H

#include <stdbool.h>
#include <stdint.h>

/* True when the sg90 overlay was applied and the PWM channel is usable. */
bool servo_present(void);

/* Bind the PWM channel. Returns 0 on success, negative errno otherwise,
 * and 0 (nothing to do) when no servo is configured. */
int servo_init(void);

/* Drive the servo to an angle in degrees. Values outside 0..180 are
 * clamped rather than rejected — the same contract as angle_clamp() on
 * the master side, so an out-of-range PDO value parks at an end stop
 * instead of dropping the update. */
int servo_set_angle(uint16_t degrees);

/*
 * Where the servo is, in degrees. Returns 0 and fills *degrees, or negative
 * errno.
 *
 * Reports the last COMMANDED angle, because an SG90 has no position output —
 * there is a potentiometer inside it, but the three wires are power, ground
 * and pulse-width in, so nothing comes back. The value is therefore a
 * statement of intent, not a measurement, and it will not lag while the horn
 * is travelling the way the stepper's does.
 *
 * Kept so both actuators satisfy the same interface. The asymmetry is real
 * and worth knowing rather than papering over: on a servo build, actual and
 * echo agreeing proves nothing about the shaft.
 */
int servo_actual_angle(uint16_t *degrees);

#endif /* SERVO_H */
