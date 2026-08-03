/*
 * NEMA 17 stepper via a DRV8825 step/direction driver (phase 6).
 *
 * Built into every image, but only active when the nema17 overlay supplies
 * the devicetree nodes:
 *
 *   ./scripts/fw.sh build nema17
 *
 * Without the overlay stepper_present() is false and the other calls are
 * no-ops, exactly as servo.h behaves without the sg90 overlay. The two are
 * mutually exclusive in practice — one overlay or the other — but nothing
 * here breaks if both are absent.
 *
 * The interface deliberately mirrors servo.h. Both actuators are commanded
 * in degrees so the PDO layout, the master's CLI and angle.c are unchanged
 * by the swap; only the way degrees become motion differs.
 */
#ifndef STEPPER_H
#define STEPPER_H

#include <stdbool.h>
#include <stdint.h>

/* True when the nema17 overlay was applied and the controller is usable. */
bool stepper_present(void);

/*
 * Bind the motion controller, zero the position counter and energise the
 * driver. Returns 0 on success, negative errno otherwise, and 0 (nothing to
 * do) when no stepper is configured.
 *
 * Zeroing matters: a stepper has no absolute position sense, so wherever the
 * shaft happens to be at boot becomes 0 degrees by definition. Homing against
 * a limit switch is what a real axis would do; this project has no switch, so
 * the convention is documented rather than solved.
 */
int stepper_init(void);

/*
 * Move to an angle in degrees. Values outside 0..180 are clamped rather than
 * rejected — the same contract as servo_set_angle() and angle_clamp() on the
 * master side, so an out-of-range PDO value parks at an end stop instead of
 * dropping the update.
 *
 * Returns immediately: the controller pulses STEP in the background and the
 * call does not block until the move completes. A new target arriving mid-move
 * supersedes the old one, which is the behaviour a cyclic 10 ms PDO update
 * needs.
 */
int stepper_set_angle(uint16_t degrees);

#endif /* STEPPER_H */
