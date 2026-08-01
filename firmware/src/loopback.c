/*
 * SPI loopback self-test — partitions a dead ESC link into "the ESP32 side
 * is broken" vs "the module side is broken".
 *
 * Reading 0xff from every ESC register means MISO is never driven low. That
 * has two very different causes and no way to tell them apart by looking at
 * the wires:
 *
 *   (a) the ESP32 is not actually clocking data out at all, or MISO/MOSI are
 *       not on the pins we think they are;
 *   (b) the ESP32 is fine and the break is in the cabling or the module.
 *
 * With MOSI tied to MISO, whatever we transmit must come straight back. If
 * it does, (a) is eliminated outright — the SPI peripheral, the pinctrl
 * mapping, and both wires up to the joint are all proven good.
 *
 * Join the two jumpers at the MODULE end (pull the MOSI and MISO leads off
 * P1 pins 7 and 6 and connect them to each other) so the test covers the
 * cables too, not just the ESP32 pins.
 */
#include "esc.h"

#include <string.h>

#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(loopback, LOG_LEVEL_INF);

/* Same node and mode as the real driver: if the loopback passes but the ESC
 * does not answer, the difference is not SPI configuration. */
static const struct spi_dt_spec lb_spi =
	SPI_DT_SPEC_GET(DT_NODELABEL(esc),
			SPI_OP_MODE_MASTER | SPI_TRANSFER_MSB |
				SPI_MODE_CPOL | SPI_MODE_CPHA | SPI_WORD_SET(8));

/* Values chosen so a stuck-high line (0xff), a stuck-low line (0x00) and a
 * bit-shift each produce an obviously wrong result rather than an
 * accidental match. */
static const uint8_t pattern[] = {0xa5, 0x5a, 0x00, 0xff, 0x01, 0x80, 0xc8};

void esc_spi_loopback_test(void)
{
	uint8_t rx[sizeof(pattern)];

	LOG_INF("=== SPI LOOPBACK SELF-TEST ===");
	LOG_INF("Expecting MOSI (GPIO 11) joined to MISO (GPIO 13).");

	if (!spi_is_ready_dt(&lb_spi)) {
		LOG_ERR("SPI bus not ready — this is a software fault, not wiring.");
		return;
	}

	memset(rx, 0, sizeof(rx));

	const struct spi_buf tx_buf = {.buf = (void *)pattern,
				       .len = sizeof(pattern)};
	const struct spi_buf rx_buf = {.buf = rx, .len = sizeof(rx)};
	const struct spi_buf_set tx_set = {.buffers = &tx_buf, .count = 1};
	const struct spi_buf_set rx_set = {.buffers = &rx_buf, .count = 1};

	int rc = spi_transceive_dt(&lb_spi, &tx_set, &rx_set);
	if (rc < 0) {
		LOG_ERR("spi_transceive failed: %d — software fault.", rc);
		return;
	}

	int bad = 0;
	for (size_t i = 0; i < sizeof(pattern); i++) {
		if (rx[i] != pattern[i]) {
			LOG_ERR("  sent 0x%02x  got 0x%02x  MISMATCH",
				pattern[i], rx[i]);
			bad++;
		} else {
			LOG_INF("  sent 0x%02x  got 0x%02x  ok", pattern[i],
				rx[i]);
		}
	}

	if (bad == 0) {
		LOG_INF("LOOPBACK PASS — the ESP32 SPI peripheral, the GPIO 11 "
			"and GPIO 13 pin mapping, and both jumper wires are "
			"all good.");
		LOG_INF("So the fault is on the module side: NSS (GPIO 10 -> "
			"P1 pin 9), SCK (GPIO 12 -> P1 pin 5), or the MOSI/"
			"MISO leads are on the wrong P1 pads.");
		return;
	}

	/* An all-0xff or all-0x00 result means nothing came back at all,
	 * which is a different fault from garbled data. */
	bool all_high = true, all_low = true;

	for (size_t i = 0; i < sizeof(rx); i++) {
		all_high &= (rx[i] == 0xff);
		all_low &= (rx[i] == 0x00);
	}

	LOG_ERR("LOOPBACK FAIL (%d/%zu bytes wrong).", bad, sizeof(pattern));
	if (all_high) {
		LOG_ERR("Every byte read 0xff: MISO is floating high. The "
			"MOSI-to-MISO joint is not actually made, or one of "
			"those two jumpers is broken/in the wrong hole.");
	} else if (all_low) {
		LOG_ERR("Every byte read 0x00: MISO is held low. Check it is "
			"not shorted to a ground pin.");
	} else {
		LOG_ERR("Data came back but altered — a real signal-integrity "
			"or pin-mapping fault. Report this output.");
	}
}
