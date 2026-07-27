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

/* Release the interface. Safe to call at any time after ecat_open. */
void ecat_close(void);

#endif /* ECAT_H */
