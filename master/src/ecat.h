#ifndef ECAT_H
#define ECAT_H

#include <stdbool.h>
#include <stdint.h>

#include "pdo_layout.h"

/* Cycle time of the process-data loop */
#define ECAT_CYCLE_US 10000 /* 10 ms */

/* Open the network interface and enumerate slaves.
 * Returns the number of slaves found (0 is not an error), or -1 on
 * failure to open the interface. */
int ecat_open(const char *ifname);

/* Print a one-line summary per slave and check slave 1 against the
 * expected ASIX identity. Returns true when slave 1 matches. */
bool ecat_print_slaves(void);

/* Read and decode the ESC configuration registers of slave 1 —
 * chip identity, PDI type, SPI mode, device emulation, AL state.
 * Works in INIT, so it needs no host MCU attached. */
void ecat_dump_regs(void);

/* Map process data and bring all slaves to OPERATIONAL.
 * Returns true on success; prints diagnostics on failure. */
bool ecat_to_op(void);

/* Pointers into the mapped IO image for slave 1. Valid after
 * ecat_to_op() succeeds. May return NULL if sizes don't match the
 * expected layout. */
servo_outputs_t *ecat_outputs(void);
servo_inputs_t *ecat_inputs(void);

/* One process-data cycle: send outputs, receive inputs.
 * Returns the working counter (>= expected means healthy). */
int ecat_cycle(void);

/* Expected working counter for the configured bus. */
int ecat_expected_wkc(void);

/*
 * Distributed clocks.
 *
 * ecat_to_op() activates the slave's SYNC0 output when the slave reports DC
 * support and DC has not been switched off with SERVO_DC=0 in the
 * environment. That env var exists so the two modes can be compared back to
 * back on the same hardware — the jitter figure printed by the cyclic loop
 * is the whole point of doing this at all.
 */

/* True when SYNC0 was activated on slave 1 and the loop should phase-lock. */
bool ecat_dc_active(void);

/* Phase error between the master's cycle and the slave's DC clock, in
 * nanoseconds, as measured on the last cycle. Meaningless unless
 * ecat_dc_active(). Converges towards zero over the first second or so. */
int64_t ecat_dc_delta(void);

/*
 * Nanosecond correction to add to this cycle's sleep so the master's loop
 * stays phase-locked to the slave's DC clock. Returns 0 when DC is inactive.
 *
 * Without this the master's cycle runs off CLOCK_MONOTONIC while SYNC0 runs
 * off the bus clock, and the two drift relative to each other. The frame then
 * lands at a different point in the DC cycle every time — which is worse than
 * not using DC at all, because the slave acts on a fixed schedule against
 * data that arrives on a wandering one.
 */
int64_t ecat_dc_correction(void);

/*
 * Re-read the ESC's DC status while the bus is running and print it.
 *
 * Must be called seconds into OP, not immediately after activation: register
 * 0x0984 bit 0 means "the first SYNC0 pulse has not happened yet", which is
 * true and meaningless in the ~100 ms between arming the clock and its start
 * time. Read it early and it always says pending, whether or not the clock
 * ever runs.
 */
void ecat_dc_report(void);

/* Release the interface. Safe to call at any time after ecat_open. */
void ecat_close(void);

#endif /* ECAT_H */
