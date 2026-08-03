/*
 * Minimal EtherCAT slave application (phase 4b).
 *
 * This is the piece that turns "the ESP32 can read ESC registers" into "the
 * ESP32 is an EtherCAT slave". It implements the two things the AX58100 will
 * not do for us, because device emulation is off (ESC Config = 0x0e):
 *
 *   1. the AL state machine — INIT / PREOP / SAFEOP / OP, driven by what the
 *      master requests in AL Control and answered in AL Status;
 *   2. cyclic process data exchange — reading the target angle out of the
 *      ESC's DPRAM through SyncManager 2 and writing the echo back through
 *      SyncManager 3.
 *
 * It deliberately does NOT implement a CoE mailbox. The process data layout
 * comes from the EEPROM the module already ships with, so nothing here
 * depends on rewriting it.
 */
#ifndef ECAT_SLAVE_H
#define ECAT_SLAVE_H

#include <stdint.h>

/*
 * Called with a new target angle each cycle while the slave is in OP.
 *
 * Kept as a callback rather than calling the servo directly so the actuator
 * stays swappable: the stepper in phase 6 registers a different function and
 * nothing in this file changes.
 */
typedef void (*ecat_apply_fn)(uint16_t target_angle);

/*
 * Asked once per cycle for where the axis actually is, in degrees, so the
 * input PDO can carry position rather than an echo of the command.
 *
 * Takes the commanded angle so the actuator can fall back to it when it has
 * no position sense — the alternative, reporting zero, would look to a master
 * like the axis had collapsed to an end stop.
 *
 * May be NULL, in which case the input PDO reports the commanded angle and
 * the field means exactly what the old echo meant.
 */
typedef uint16_t (*ecat_actual_fn)(uint16_t commanded_angle);

/* Prepare the slave and park it in INIT. esc_init() must have succeeded
 * first. Returns 0 on success, negative errno otherwise. */
int ecat_slave_init(ecat_apply_fn apply, ecat_actual_fn actual);

/*
 * Service the state machine and, when in SAFEOP or OP, exchange process
 * data. Call this repeatedly; it returns quickly and does not block.
 */
void ecat_slave_poll(void);

/* Current AL state (ESC_AL_INIT, ESC_AL_PREOP, ...), for logging. */
uint8_t ecat_slave_state(void);

/* Human-readable name for an AL state value. */
const char *ecat_state_name(uint8_t state);

#endif /* ECAT_SLAVE_H */
