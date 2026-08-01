/*
 * Link diagnostics for the AX58100 SPI connection.
 *
 * Built only when CONFIG_ESC_DIAG=y, in place of the milestone 4a register
 * check. It exists because the normal check reports a single symptom — every
 * register reads 0xff or 0x00 — that has several very different causes, and
 * nothing in a passive reading distinguishes them.
 *
 * The hard part is NSS and SCK. Both are ESC inputs, so pulling them up on
 * the ESP32 side reads high whether or not the wire carries; a dead wire and
 * a good one look identical. The way out is to use the ESC itself as the
 * instrument: it visibly reacts to being selected, so driving NSS and
 * watching for that reaction tests the wire end to end.
 */
#include "esc.h"

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(diag, LOG_LEVEL_INF);

/* SPI signal pins, matching app.overlay. */
#define PIN_NSS 10
#define PIN_MOSI 11
#define PIN_SCK 12
#define PIN_MISO 13
#define PIN_IRQ 16

/*
 * Stage 1: test the NSS wire using the ESC's own reaction.
 *
 * NSS and SCK are ESC inputs, so a pull-up on the ESP32 side cannot tell
 * whether their wires conduct — that is the gap that kept this bring-up
 * stuck. But when NSS was grounded by hand at the module, the ESC responded
 * observably: IRQ (GPIO 16) went low and MISO (GPIO 13) changed state.
 *
 * That reaction is a probe. Driving NSS low from GPIO 10 and watching those
 * two lines tests the wire end to end with no manual intervention: if the
 * ESC reacts, the wire carries; if nothing moves, it does not.
 */
static void nss_wire_test(void)
{
	const struct device *gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));

	LOG_INF("--- stage 1: NSS wire test via ESC reaction ---");

	gpio_pin_configure(gpio0, PIN_MISO, GPIO_INPUT | GPIO_PULL_UP);
	gpio_pin_configure(gpio0, PIN_IRQ, GPIO_INPUT | GPIO_PULL_UP);
	gpio_pin_configure(gpio0, PIN_NSS, GPIO_OUTPUT_HIGH);
	k_sleep(K_MSEC(20));

	int miso_hi = gpio_pin_get(gpio0, PIN_MISO);
	int irq_hi = gpio_pin_get(gpio0, PIN_IRQ);

	gpio_pin_set(gpio0, PIN_NSS, 0); /* assert chip select */
	k_sleep(K_MSEC(20));

	int miso_lo = gpio_pin_get(gpio0, PIN_MISO);
	int irq_lo = gpio_pin_get(gpio0, PIN_IRQ);

	gpio_pin_set(gpio0, PIN_NSS, 1);

	LOG_INF("  NSS high: MISO=%d IRQ=%d", miso_hi, irq_hi);
	LOG_INF("  NSS low : MISO=%d IRQ=%d", miso_lo, irq_lo);

	if (miso_hi != miso_lo || irq_hi != irq_lo) {
		LOG_INF("STAGE 1 PASS — the ESC reacted to NSS, so the GPIO 10 "
			"wire carries. Remaining suspect is SCK (GPIO 12).");
	} else {
		LOG_ERR("STAGE 1 FAIL — nothing moved when NSS was asserted.");
		LOG_ERR("The ESC did react when NSS was grounded by hand at the "
			"module, so the chip works: the GPIO 10 -> NSS wire is "
			"not carrying.");
	}
}

/*
 * Stage 2: live pin map.
 *
 * When the link test fails there are two possibilities left and no way to
 * tell them apart from the Mac: the jumper is not conducting, or the pad
 * being poked is not the GPIO we believe it is (unsoldered headers, a
 * miscounted pad, a board pinout that differs from the published diagram).
 *
 * Every SPI-related pin is held high by an internal pull-up and printed
 * continuously. Touching a pad to GND pulls exactly one line low, which
 * names that physical pad with certainty. That is a direct measurement of
 * the board in front of the user, not an assumption from a datasheet.
 */
static const struct {
	uint8_t pin;
	const char *role;
} monitored[] = {
	{PIN_NSS, "NSS  -> P1.9"}, {PIN_MOSI, "MOSI -> P1.7"}, {PIN_SCK, "SCK -> P1.5"},
	{PIN_MISO, "MISO -> P1.6"}, {PIN_IRQ, "IRQ -> P1.8"}, {21, "SYNC0-> P1.3"},
};

/*
 * Bias sweep: tell "nothing attached" apart from "held low".
 *
 * The monitor holds every pin high with an internal pull-up, so a 0 is read
 * as "something is grounding this". That inference fails if the pull-up
 * itself did not take effect, and the two cases look identical from one
 * sample. Reading each pin under a pull-up and then a pull-down separates
 * them, because only a floating pin follows the bias:
 *
 *   up=1 down=0  -> floating, nothing attached
 *   up=0 down=0  -> genuinely held low by something external
 *   up=1 down=1  -> held high by something external
 */
static void bias_sweep(void)
{
	const struct device *gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));

	LOG_INF("--- bias sweep: floating vs driven ---");

	for (size_t i = 0; i < ARRAY_SIZE(monitored); i++) {
		uint8_t pin = monitored[i].pin;

		gpio_pin_configure(gpio0, pin, GPIO_INPUT | GPIO_PULL_UP);
		k_sleep(K_MSEC(5));
		int up = gpio_pin_get(gpio0, pin);

		gpio_pin_configure(gpio0, pin, GPIO_INPUT | GPIO_PULL_DOWN);
		k_sleep(K_MSEC(5));
		int down = gpio_pin_get(gpio0, pin);

		const char *verdict;

		if (up == 1 && down == 0) {
			verdict = "floating (nothing attached)";
		} else if (up == 0 && down == 0) {
			verdict = "HELD LOW externally";
		} else if (up == 1 && down == 1) {
			verdict = "HELD HIGH externally";
		} else {
			verdict = "inverted - unexpected";
		}

		LOG_INF("  GPIO %-2u %-14s up=%d down=%d  %s", pin,
			monitored[i].role, up, down, verdict);
	}
}

static void pin_monitor(void)
{
	const struct device *gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));

	LOG_INF("--- stage 2: live pin monitor ---");
	LOG_INF("All pins pulled UP internally. Touch a pad to GND and the");
	LOG_INF("matching line below flips to 0, which identifies that pad.");

	for (size_t i = 0; i < ARRAY_SIZE(monitored); i++) {
		if (gpio_pin_configure(gpio0, monitored[i].pin,
				       GPIO_INPUT | GPIO_PULL_UP) < 0) {
			LOG_ERR("could not configure GPIO %u", monitored[i].pin);
		}
	}

	while (1) {
		char line[96];
		size_t off = 0;

		for (size_t i = 0; i < ARRAY_SIZE(monitored); i++) {
			int v = gpio_pin_get(gpio0, monitored[i].pin);

			off += snprintk(line + off, sizeof(line) - off,
					"%u=%d ", monitored[i].pin, v);
		}
		LOG_INF("pins: %s", line);
		k_sleep(K_MSEC(500));
	}
}

void esc_diag_run(void)
{
	LOG_INF("=== ESC LINK DIAGNOSTICS ===");

	/* Stage 1 is the only test that can see NSS and SCK, because they are
	 * ESC inputs: a pull-up on the ESP32 side reads high whether or not
	 * the wire carries. It works by watching the ESC's own reaction. */
	bias_sweep();
	nss_wire_test();

	/* Runs last and never returns — leaves a live view of every line so
	 * wiring can be poked at without reflashing. */
	pin_monitor();
}
