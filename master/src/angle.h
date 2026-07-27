#ifndef ANGLE_H
#define ANGLE_H

#include <stdint.h>
#include <stdbool.h>

#define ANGLE_MIN 0
#define ANGLE_MAX 180

/* Parse a decimal string as an angle. Returns true and stores the value
 * in *out when the string is a valid integer in [0, 180]. */
bool angle_parse(const char *s, uint16_t *out);

/* Clamp any integer to the valid [0, 180] range. */
uint16_t angle_clamp(int value);

/* Triangle-wave sweep: position (degrees) at time t within a full
 * 0 -> 180 -> 0 sweep of the given period. period_s must be > 0. */
uint16_t angle_sweep_position(double t_seconds, double period_s);

/* Servo pulse width in milliseconds for an angle (0.5ms .. 2.5ms).
 * Used by the STM32 firmware later; kept here so it is unit-tested. */
double angle_to_pulse_ms(uint16_t angle);

#endif /* ANGLE_H */
