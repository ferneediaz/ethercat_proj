/*
 * Application glue between SOES and the actuator.
 *
 * SOES owns the AL state machine, the CoE mailbox and the PDO mapping; this
 * file only says what the process data means. cb_set_outputs() runs after
 * SOES has copied the master's outputs into Obj, and cb_get_inputs() runs
 * before it publishes Obj back, so neither needs to touch the ESC directly.
 */
#include "soes_app.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "ax58100.h"
#include "esc_irq.h"
#include "ecat_slv.h"
#include "esc.h"
#include "sync0.h"
#include "utypes.h"

LOG_MODULE_REGISTER(soes_app, LOG_LEVEL_INF);

_Objects Obj;

static soes_apply_fn apply_angle;
static soes_actual_fn read_actual;

/*
 * Last target actually handed to the actuator. 0xffff is a sentinel outside
 * the commandable range, so the first delivery of any angle — including 0 —
 * always gets through.
 */
#define NO_ANGLE_APPLIED 0xffffu
static uint16_t last_applied = NO_ANGLE_APPLIED;

/*
 * Outputs have arrived from the master. In SAFEOP SOES clears them rather
 * than delivering stale data, so this is only meaningful in OP — which is
 * precisely the guarantee SAFEOP exists to give.
 *
 * Driven on change only. Re-commanding an unchanged target every cycle looks
 * free but is not: a stepper's motion controller re-arms its timing source and
 * emits a STEP edge on each call, chopping the pulses of any move that spans
 * more than one cycle and losing steps that this board has no encoder to
 * notice. A servo's PWM is latched in hardware and needs no rewriting either.
 */
void cb_set_outputs(void)
{
	uint16_t target = Obj.Outputs.target_angle;

	if (target == last_applied) {
		return;
	}

	LOG_INF("target %u deg", target);
	last_applied = target;

	if (apply_angle != NULL) {
		apply_angle(target);
	}
}

/*
 * Build the input PDO. SOES calls this once per cycle.
 *
 * echo_angle now reports the angle actually accepted rather than
 * Obj.Outputs.target_angle — which is what the comment here always claimed
 * and the code never did. The two differ for one cycle after a new command
 * arrives, and publishing the raw output claimed the slave had acted before
 * it had.
 *
 * actual_angle is where the axis reports it really is. It is sampled every
 * cycle, not only when the target changes, because it moves while the axis
 * travels whether or not a new command arrived — that lag is the entire
 * point of carrying it.
 */
void cb_get_inputs(void)
{
	uint16_t echo = (last_applied == NO_ANGLE_APPLIED) ? 0 : last_applied;

	Obj.Inputs.echo_angle = echo;
	Obj.Inputs.actual_angle = read_actual ? read_actual(echo) : echo;
}

void soes_app_run(soes_apply_fn apply, soes_actual_fn actual)
{
	static esc_cfg_t config = {
		.user_arg = NULL,
		.use_interrupt = 0,
		/*
		 * Counts poll iterations without process data before SOES
		 * drops out of OP. Zero does NOT disable it — the counter is
		 * already expired, so the very first OP request is refused
		 * with AL status code 0x001b (sync manager watchdog), which
		 * looks like a SyncManager configuration fault rather than a
		 * timing one. 150 is what SOES's own demos use.
		 */
		.watchdog_cnt = 150,
		.set_defaults_hook = NULL,
		.pre_state_change_hook = NULL,
		.post_state_change_hook = NULL,
		.application_hook = NULL,
		.safeoutput_override = NULL,
		.pre_object_download_hook = NULL,
		.post_object_download_hook = NULL,
		.pre_object_upload_hook = NULL,
		.post_object_upload_hook = NULL,
		.rxpdo_override = NULL,
		.txpdo_override = NULL,
		.esc_hw_interrupt_enable = NULL,
		.esc_hw_interrupt_disable = NULL,
		.esc_hw_eep_handler = NULL,
		.esc_check_dc_handler = NULL,
	};

	apply_angle = apply;
	read_actual = actual;
	last_applied = NO_ANGLE_APPLIED;

	LOG_INF("starting SOES");
	ecat_slv_init(&config);

	/*
	 * Arm SINT.
	 *
	 * Deliberately after ecat_slv_init: ESC_init resets the chip, which
	 * clears the AL event mask, so unmasking before this point would be
	 * silently undone.
	 *
	 * SM2 is included so the arrival of process data raises the line. The
	 * cyclic act still happens on SYNC0 — SINT says data is here, SYNC0
	 * says it is time — but knowing when the frame landed is what makes
	 * the difference between the two measurable.
	 */
	if (esc_irq_present() && esc_irq_init() == 0) {
		esc_irq_set_mask(ESC_ALEVENT_CONTROL | ESC_ALEVENT_SMCHANGE |
				 ESC_ALEVENT_SM0 | ESC_ALEVENT_SM1 |
				 ESC_ALEVENT_SM2);
	}

	/*
	 * Arm SYNC0 after ecat_slv_init(), not before. The ESC's SYNC0 output
	 * settles as DC is configured, and arming an edge interrupt across that
	 * would count the settling transition as a real tick.
	 */
	if (sync0_present() && sync0_init() != 0) {
		LOG_WRN("SYNC0 unavailable — the cycle will free-run on a local "
			"timer rather than the bus clock");
	}

	/*
	 * SOES prints only during init, so without this the stack runs
	 * completely silently and a slave stuck in INIT is indistinguishable
	 * from one that is not running at all.
	 */
	uint8_t last_status = 0xff;
	uint16_t last_error = 0xffff;
	/*
	 * The AL-status line below only prints on a state change, which is
	 * useless for watching SYNC0: the distributed clock starts a few
	 * hundred milliseconds AFTER the last transition, so the edge count in
	 * that line is always sampled too early and always reads zero. This
	 * heartbeat is the only honest way to see whether edges are arriving.
	 */
	int64_t next_beat = k_uptime_get() + 2000;
	uint32_t last_edges = 0;
	uint32_t last_irq = 0;
	bool probed = false;
	/* Starts true so the first transition logged is the interesting one:
	 * before OP there is no process data and the watchdog is not running,
	 * which is not a fault worth announcing. */
	bool wd_ok = true;
	struct k_sem *irq_signal = esc_irq_present() ? esc_irq_signal() : NULL;

	while (1) {
		/*
		 * Pace first, then work. Once the distributed clock is running
		 * this blocks until the SYNC0 edge and returns immediately
		 * after it, so the SPI read of SM2 happens at a bus-wide agreed
		 * instant rather than wherever the poll loop happened to land.
		 * Before DC is up it falls back to a 1 ms sleep, which is what
		 * this loop did before SYNC0 was wired in.
		 */
		/*
		 * Wait for whichever interrupt comes first.
		 *
		 * Two signals, two meanings, and the slave needs both:
		 *
		 *   SINT   the ESC raised AL Event — the master wrote SM2, or
		 *          asked for a state change, or put something in the
		 *          mailbox. Asynchronous: it arrives when the frame
		 *          arrives.
		 *   SYNC0  the distributed clock says it is time to act. Fixed
		 *          schedule, agreed bus-wide, independent of when the
		 *          frame happened to land.
		 *
		 * Acting on SINT would hand the actuator the master's
		 * scheduling jitter; waiting only for SYNC0 means never being
		 * told what changed without asking. So SINT drives servicing
		 * and SYNC0 drives the cyclic act, which is the same split
		 * SOES's own reference HAL uses.
		 *
		 * One thread rather than two on purpose. The reference splits
		 * the work across an ISR and a worker task, but that relies on
		 * CC_ATOMIC_* being real; in this port they fall back to plain
		 * assignment (see cc.h), so ESCvar would be racy. One thread
		 * waiting on both semaphores has no such problem and no mutex
		 * around SPI.
		 */
		struct k_poll_event evs[2];
		int nev = 0;

		if (sync0_present()) {
			k_poll_event_init(&evs[nev++],
					  K_POLL_TYPE_SEM_AVAILABLE,
					  K_POLL_MODE_NOTIFY_ONLY,
					  sync0_signal());
		}
		if (irq_signal != NULL) {
			k_poll_event_init(&evs[nev++],
					  K_POLL_TYPE_SEM_AVAILABLE,
					  K_POLL_MODE_NOTIFY_ONLY, irq_signal);
		}

		bool sync_edge = false;
		bool sint = false;

		if (nev > 0) {
			/*
			 * The timeout is a backstop, not a poll interval. It
			 * has to outlast a DC cycle comfortably or it would
			 * expire between every pair of edges and look like the
			 * clock had stopped; short enough that a bus which
			 * goes quiet is still noticed.
			 */
			(void)k_poll(evs, nev, K_MSEC(25));

			for (int i = 0; i < nev; i++) {
				if (evs[i].state !=
				    K_POLL_STATE_SEM_AVAILABLE) {
					continue;
				}
				if (evs[i].sem == irq_signal) {
					k_sem_take(evs[i].sem, K_NO_WAIT);
					sint = true;
				} else {
					k_sem_take(evs[i].sem, K_NO_WAIT);
					sync_edge = true;
				}
			}
		} else {
			k_sleep(K_MSEC(1));
		}

		/* Keeps the DC lock/unlock detection behaving as it did when
		 * this loop called sync0_pace(). */
		sync0_notify(sync_edge);
		(void)sint;

		ecat_slv();

		/*
		 * ecat_slv() has just read 0x0220, which is what releases a
		 * level-triggered SINT. Re-enable the interrupt now that the
		 * cause is cleared; doing it any earlier would re-enter the
		 * handler immediately.
		 */
		esc_irq_rearm();

		/*
		 * Watch the ESC's own SM watchdog alongside SOES's.
		 *
		 * SOES drops to SAFEOP with ALERR_WATCHDOG when its counter
		 * expires, but that counter counts POLL ITERATIONS, so what it
		 * means in seconds depends entirely on how fast this loop is
		 * running — which changes the moment SYNC0 starts pacing it.
		 * Register 0x0440 is time-based and programmed by the master,
		 * so it is the honest answer to "how long was the master gone".
		 *
		 * SOES already has a reader for it (ESC_WDstatus, esc.c), which
		 * nothing in this project called until now — no reason to add a
		 * second path to the same register.
		 *
		 * Only meaningful in OP. The watchdog does not run until process
		 * data is flowing, so outside OP it reads expired regardless —
		 * the same trap as reading DC register 0x0984 before the first
		 * SYNC0 pulse was due.
		 *
		 * Logged on the edge only. Polling it every cycle costs an SPI
		 * transaction inside the DC-paced window, and the transition is
		 * the only interesting part.
		 */
		if (ESCvar.ALstatus == ESCop) {
			uint8_t wd = ESC_WDstatus();
			bool ok = (wd & ESC_WD_STATUS_OK) != 0;

			if (ok != wd_ok) {
				wd_ok = ok;
				LOG_WRN("ESC SM watchdog %s (0x0440 = 0x%02x)",
					ok ? "fed again"
					   : "EXPIRED — master has stopped "
					     "sending process data",
					(unsigned)wd);
			}
		} else {
			/* Below OP there is no process data, so 0x0440 reads
			 * expired and means nothing. Reporting it here would
			 * announce "the master has stopped" on every boot,
			 * before a master has ever connected. Reset the edge
			 * detector so the first real transition still logs. */
			wd_ok = true;
		}

		if (ESCvar.ALstatus != last_status ||
		    ESCvar.ALerror != last_error) {
			LOG_INF("AL control=0x%02x status=0x%02x error=0x%04x "
				"SM2=%u SM3=%u dc=%s edges=%u",
				ESCvar.ALcontrol, ESCvar.ALstatus,
				ESCvar.ALerror, ESCvar.ESC_SM2_sml,
				ESCvar.ESC_SM3_sml,
				sync0_locked() ? "SYNC0" : "polled",
				sync0_edges());
			last_status = ESCvar.ALstatus;
			last_error = ESCvar.ALerror;
		}

		if (k_uptime_get() >= next_beat) {
			uint32_t e = sync0_edges();
			uint32_t irq = esc_irq_count();

			LOG_INF("sync: %s, %u edges (+%u in 2s, expect ~200), "
				"pin=%d, AL=0x%02x | SINT: %u total (+%u), "
				"line=%d, mask=0x%08x",
				sync0_locked() ? "DC-LOCKED" : "polled", e,
				e - last_edges, sync0_level(), ESCvar.ALstatus,
				irq, irq - last_irq, esc_irq_level(),
				esc_irq_mask());
			last_irq = irq;
			/* Once, when we are in OP and should be seeing edges but
			 * are not, find out what is actually on the pin. */
			if (!probed && e == 0 && ESCvar.ALstatus == 0x08) {
				probed = true;
				sync0_probe();
			}
			last_edges = e;
			next_beat = k_uptime_get() + 2000;
		}
	}
}
