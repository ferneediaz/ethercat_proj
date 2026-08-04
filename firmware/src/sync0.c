#include "sync0.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(sync0, LOG_LEVEL_INF);

#define ESC_NODE DT_NODELABEL(esc)

/*
 * Fallback poll interval, used before the distributed clock is running.
 * Matches the 1 ms the loop used before DC existed, so behaviour below
 * SAFEOP is unchanged.
 */
#define SYNC0_POLL_MS 1

/*
 * Timeout used once edges are being followed. It must be comfortably longer
 * than the DC cycle the master programs (10 ms) or it would expire between
 * every pair of edges and we would silently drop back to polling. It must
 * also be short enough that a stopped clock is noticed promptly.
 */
#define SYNC0_LOCKED_TIMEOUT_MS 25

/*
 * Edges required before switching from polling to edge-driven pacing.
 *
 * One edge is not enough: a single spurious transition — noise on a long
 * unshielded jumper, or the pin settling as the ESC configures DC — would
 * park the loop on a 25 ms timeout that nothing ever satisfies, and the
 * slave would look hung. Three consecutive cycles of a 100 Hz signal is
 * 30 ms of evidence, which costs nothing and cannot be a glitch.
 */
#define SYNC0_LOCK_EDGES 3

#if defined(CONFIG_ESC_DC_SYNC) && DT_NODE_HAS_PROP(ESC_NODE, sync0_gpios)

static const struct gpio_dt_spec sync0_pin =
	GPIO_DT_SPEC_GET(ESC_NODE, sync0_gpios);
static struct gpio_callback sync0_cb;
static K_SEM_DEFINE(sync0_sem, 0, 1);

static atomic_t edge_count;
static bool armed;
static bool locked;

bool sync0_present(void)
{
	return true;
}

/*
 * The semaphore is deliberately capped at one permit. If the application
 * overruns a DC cycle we want the next edge to release it immediately and
 * then carry on in step, not to accumulate a backlog of missed edges that
 * the loop then races through with no pacing at all.
 */
static void on_sync0_edge(const struct device *port, struct gpio_callback *cb,
			  gpio_port_pins_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	atomic_inc(&edge_count);
	k_sem_give(&sync0_sem);
}

/*
 * Is anything actually on the other end of this pin?
 *
 * Bias the input high, then low, and read it back each time. A pin driven by
 * the ESC's push-pull SYNC0 output ignores a ~45k internal pull and reports
 * the driver's level both times. A pin with nothing attached follows whatever
 * we pull it to.
 *
 * This is safe in a way that the obvious test is not: driving the pin as an
 * output to check the interrupt path would put the ESP32's driver in
 * contention with the ESC's, and the ESC is the part that is hard to replace.
 * The same technique already proved out the SPI lines in diag.c.
 */
static void sync0_bias_test(void)
{
	int hi, lo;

	gpio_pin_configure_dt(&sync0_pin, GPIO_INPUT | GPIO_PULL_UP);
	k_busy_wait(2000);
	hi = gpio_pin_get_dt(&sync0_pin);

	gpio_pin_configure_dt(&sync0_pin, GPIO_INPUT | GPIO_PULL_DOWN);
	k_busy_wait(2000);
	lo = gpio_pin_get_dt(&sync0_pin);

	/* Restore the plain input the interrupt was armed against. */
	gpio_pin_configure_dt(&sync0_pin, GPIO_INPUT);

	if (hi == 1 && lo == 0) {
		LOG_ERR("SYNC0 pin follows our own pull-up/pull-down (hi=%d "
			"lo=%d) — NOTHING is driving it. The wire from module "
			"pin 3 (SYNC0) to GPIO %u is missing or broken.",
			hi, lo, sync0_pin.pin);
	} else {
		LOG_INF("SYNC0 pin is externally driven (pull-up read %d, "
			"pull-down read %d) — the wire is connected.",
			hi, lo);
	}
}

int sync0_init(void)
{
	int rc;

	if (!gpio_is_ready_dt(&sync0_pin)) {
		LOG_ERR("SYNC0 GPIO controller is not ready");
		return -ENODEV;
	}

	rc = gpio_pin_configure_dt(&sync0_pin, GPIO_INPUT);
	if (rc < 0) {
		LOG_ERR("SYNC0 pin configure failed: %d", rc);
		return rc;
	}

	gpio_init_callback(&sync0_cb, on_sync0_edge, BIT(sync0_pin.pin));
	rc = gpio_add_callback(sync0_pin.port, &sync0_cb);
	if (rc < 0) {
		LOG_ERR("SYNC0 add_callback failed: %d", rc);
		return rc;
	}

	rc = gpio_pin_interrupt_configure_dt(&sync0_pin, GPIO_INT_EDGE_TO_ACTIVE);
	if (rc < 0) {
		LOG_ERR("SYNC0 interrupt configure failed: %d", rc);
		return rc;
	}

	armed = true;
	LOG_INF("SYNC0 armed on GPIO %u — polling at %u ms until the master "
		"activates the distributed clock",
		sync0_pin.pin, SYNC0_POLL_MS);
	sync0_bias_test();
	return 0;
}

bool sync0_pace(void)
{
	if (!armed) {
		k_sleep(K_MSEC(SYNC0_POLL_MS));
		return false;
	}

	k_timeout_t wait = locked ? K_MSEC(SYNC0_LOCKED_TIMEOUT_MS)
				  : K_MSEC(SYNC0_POLL_MS);
	bool edge = (k_sem_take(&sync0_sem, wait) == 0);

	sync0_notify(edge);
	return edge;
}

struct k_sem *sync0_signal(void)
{
	return &sync0_sem;
}

void sync0_notify(bool edge)
{
	static int64_t last_edge_ms;

	if (!armed) {
		return;
	}

	/*
	 * Unlock on elapsed TIME, not on a single wake without an edge.
	 *
	 * When this was only ever called from sync0_pace(), "no edge" meant
	 * "the wait timed out" and dropping the lock was right. Now the loop
	 * also wakes on SINT, and SINT and SYNC0 almost never land in the same
	 * wakeup — so treating any edgeless wake as a stopped clock made the
	 * lock flap off constantly and report a DC-synchronised bus as polled.
	 */
	if (edge) {
		last_edge_ms = k_uptime_get();
	} else if (locked &&
		   (k_uptime_get() - last_edge_ms) < SYNC0_LOCKED_TIMEOUT_MS) {
		return;
	}

	if (edge) {
		/*
		 * Lock on edge count rather than on consecutive edges: while
		 * polling at 1 ms against a 10 ms clock, most iterations are
		 * timeouts by construction, so "consecutive" would never be
		 * reached. Edges only happen when DC is genuinely running.
		 */
		if (!locked && atomic_get(&edge_count) >= SYNC0_LOCK_EDGES) {
			locked = true;
			LOG_INF("SYNC0 running — cycle is now DC-synchronised "
				"(%u edges seen)",
				(uint32_t)atomic_get(&edge_count));
		}
	} else if (locked) {
		locked = false;
		LOG_WRN("SYNC0 stopped after %u edges — back to %u ms polling. "
			"Expected when the master exits or leaves OP.",
			(uint32_t)atomic_get(&edge_count), SYNC0_POLL_MS);
	}
}

bool sync0_locked(void)
{
	return locked;
}

uint32_t sync0_edges(void)
{
	return (uint32_t)atomic_get(&edge_count);
}

int sync0_level(void)
{
	if (!armed) {
		return -1;
	}
	return gpio_pin_get_dt(&sync0_pin);
}

/*
 * Busy-poll the pin fast enough to catch a 100 us pulse in a 10 ms cycle.
 *
 * The edge interrupt reporting zero has two completely different causes and
 * the log cannot tell them apart: either no signal reaches the pin, or the
 * signal is there and the interrupt path is not working. Sampling in a tight
 * loop for 50 ms covers five DC cycles and answers it directly — at roughly a
 * sample per microsecond a 100 us pulse cannot be missed.
 */
void sync0_probe(void)
{
	uint32_t highs = 0, trans = 0, samples = 0;
	int prev, v;
	uint32_t start, limit;

	if (!armed) {
		return;
	}

	prev = gpio_pin_get_dt(&sync0_pin);
	start = k_cycle_get_32();
	limit = k_ms_to_cyc_ceil32(50);

	while ((k_cycle_get_32() - start) < limit) {
		v = gpio_pin_get_dt(&sync0_pin);
		samples++;
		if (v > 0) {
			highs++;
		}
		if (v != prev) {
			trans++;
			prev = v;
		}
	}

	LOG_INF("SYNC0 probe: %u samples over 50 ms (5 DC cycles), %u high, "
		"%u transitions", samples, highs, trans);
	if (trans == 0 && highs == 0) {
		LOG_ERR("SYNC0 probe: the pin never went high. No SYNC0 signal "
			"is reaching GPIO %u — check that its wire goes to "
			"module pin 3 and not to ground.", sync0_pin.pin);
	} else if (trans > 0) {
		LOG_WRN("SYNC0 probe: the signal IS present but the edge "
			"interrupt did not fire — that is a firmware fault, "
			"not a wiring one.");
	}
}

#else /* no sync0-gpios in the devicetree, or CONFIG_ESC_DC_SYNC=n */

bool sync0_present(void)
{
	return false;
}

int sync0_init(void)
{
	return 0;
}

bool sync0_pace(void)
{
	k_sleep(K_MSEC(SYNC0_POLL_MS));
	return false;
}

bool sync0_locked(void)
{
	return false;
}

uint32_t sync0_edges(void)
{
	return 0;
}

int sync0_level(void)
{
	return -1;
}

#endif
