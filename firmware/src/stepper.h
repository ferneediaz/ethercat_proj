/*
 * NEMA 17 stepper via a DRV8825 step/direction driver (phase 6).
 *
 * Built into every image, but only active when the nema17 overlay supplies
 * the devicetree nodes:
 *
 *   ./scripts/fw.sh build nema17
 *
 * Without the overlay stepper_present() is false and the other calls are
 * no-ops, exactly as servo.h behaves without the sg90 overlay. Exactly one
 * actuator overlay is applied per build — they contend for GPIO 1, and
 * main.c has a BUILD_ASSERT that rejects an image containing both.
 *
 * The mechanics live entirely in the devicetree (`stepper-axis` node, see
 * dts/bindings/stepper-axis.yaml): steps per revolution, microstepping, step
 * rate and the angle limit are all properties, so a different motor or a
 * different DIP switch setting never touches this code.
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
 * MUST succeed before stepper_set_angle() is worth calling: the step interval
 * is programmed here, and without it the controller rejects every move with
 * -EINVAL. Callers are expected to check the return and stop wiring the
 * actuator up rather than command an axis that will only log errors.
 *
 * Zeroing matters: a stepper has no absolute position sense, so wherever the
 * shaft happens to be at boot becomes 0 degrees by definition. Homing against
 * a limit switch is what a real axis would do; this project has no switch, so
 * the convention is documented rather than solved. The motor is then left
 * energised to defend that zero — see the comment in stepper_init().
 */
int stepper_init(void);

/*
 * Move to an angle in degrees. Values above the axis's max-angle property are
 * clamped rather than rejected — the same contract as servo_set_angle() and
 * angle_clamp() on the master side, so an out-of-range PDO value parks at an
 * end stop instead of dropping the update.
 *
 * Returns immediately: the controller pulses STEP in the background and the
 * call does not block until the move completes. A genuinely new target
 * arriving mid-move supersedes the old one, which is the behaviour a cyclic
 * PDO update needs; a repeat of the target already in flight is dropped,
 * because re-arming the controller mid-pulse can cost a step that nothing
 * downstream would notice was lost.
 */
int stepper_set_angle(uint16_t degrees);

#endif /* STEPPER_H */
