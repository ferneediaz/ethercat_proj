/*
 * SOES-backed EtherCAT slave (branch: soes-port).
 *
 * Selected with CONFIG_ESC_USE_SOES=y, replacing the hand-written slave in
 * firmware/src/ecat_slave.c. Both sit on the same ax58100.c SPI layer and
 * present the same 2-byte-out / 6-byte-in process data, so the master does
 * not need to know which one is running.
 */
#ifndef SOES_APP_H
#define SOES_APP_H

#include <stdint.h>

/* Called with a new target angle whenever the master delivers outputs in OP.
 * Same contract as ecat_apply_fn, so the actuator code is shared. */
typedef void (*soes_apply_fn)(uint16_t target_angle);

/*
 * Asked once per cycle for where the axis actually is, so the input PDO can
 * carry position rather than an echo of the command. Takes the commanded
 * angle so an actuator with no position sense can fall back to it. May be
 * NULL, in which case the field reports the command.
 */
typedef uint16_t (*soes_actual_fn)(uint16_t commanded_angle);

/* Start SOES and run its poll loop. Never returns. */
void soes_app_run(soes_apply_fn apply, soes_actual_fn actual);

#endif /* SOES_APP_H */
