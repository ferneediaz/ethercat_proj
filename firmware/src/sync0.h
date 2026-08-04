/*
 * SYNC0 — pacing the slave's cycle from the distributed clock.
 *
 * The AX58100's SYNC0 pin fires at an instant agreed bus-wide to well under
 * a microsecond. Acting on that edge instead of on "whenever the poll loop
 * next comes round" is what makes a slave DC-synchronous rather than
 * free-running, and it is what decouples actuation from the master's
 * scheduling jitter — which matters here, because the master runs on a stock
 * Raspberry Pi OS kernel with no real-time patches.
 *
 * SYNC0 only pulses once the master has called ec_dcsync0() on this slave,
 * which cannot happen before SAFEOP. Everything here therefore degrades to
 * plain polling until the edges actually start, and back again if they stop.
 */
#ifndef SYNC0_H
#define SYNC0_H

#include <stdbool.h>
#include <stdint.h>

/* True when the devicetree gives the ESC node a sync0-gpios property and
 * CONFIG_ESC_DC_SYNC is set. Everything below is a no-op otherwise. */
bool sync0_present(void);

/* Arm the edge interrupt. Safe to call when sync0_present() is false.
 * Returns 0 on success or a negative errno. */
int sync0_init(void);

/*
 * Pace one iteration of the slave's cycle, and return true if this iteration
 * was released by a DC edge rather than by the fallback timeout.
 *
 * Before the master activates DC this sleeps for SYNC0_POLL_MS, so the AL
 * state machine still runs often enough to get through INIT and PREOP. Once
 * edges have been seen it blocks on them instead, which is the whole point:
 * one cycle of work per DC tick, at the instant the DC tick says.
 */
bool sync0_pace(void);

/*
 * The semaphore the SYNC0 handler signals, and the bookkeeping that goes with
 * consuming it.
 *
 * sync0_pace() is the simple case: block until the edge. A loop that must also
 * wake on SINT cannot use it, because it has to wait on two things at once —
 * so it k_polls this semaphore directly and then calls sync0_notify() with
 * whether an edge was what woke it, which keeps the DC lock/unlock detection
 * working exactly as before.
 */
struct k_sem *sync0_signal(void);
void sync0_notify(bool edge);

/* True once edges have been seen and the loop is being driven by them. */
bool sync0_locked(void);

/* Total edges counted since boot. Useful as proof of life in a log line. */
uint32_t sync0_edges(void);

/*
 * Raw level on the SYNC0 pin, or -1 if unavailable.
 *
 * Distinguishes the two reasons an edge count can stay at zero: a pin that
 * reads a steady 1 is connected to a signal that has latched high (SYNC0
 * pulse length 0 means the ESC holds it until acknowledged), while a pin
 * that reads a steady 0 with a pull-up absent is more likely not connected
 * at all. Without this the two look identical from the log.
 */
int sync0_level(void);

/* Busy-poll the pin for 50 ms and report what is actually on it. Used when
 * the edge count stays at zero, to separate a dead wire from a dead ISR. */
void sync0_probe(void);

#endif /* SYNC0_H */
