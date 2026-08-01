#include "esc.h"

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(esc, LOG_LEVEL_INF);

/*
 * PDI-SPI frame format, from AX58100 datasheet v105 Figure 4-17
 * (2 byte addressing, read with Wait State byte):
 *
 *   byte 0 : A12..A5
 *   byte 1 : A4..A0 (bits 7..3) | command (bits 2..0)
 *   byte 2 : wait state byte    (reads only)
 *   byte 3+: data
 *
 * The command encodings below are the standard EtherCAT ESC values; the
 * AX58100 is register- and protocol-compatible with the Beckhoff ET1100
 * (see AX58100_Migration_from_Beckhoff_ET1100_AppNote_v100.pdf in
 * knowledge base/). They are not printed as a table in the AX58100
 * datasheet itself — the read of ESC_REG_TYPE returning 0xc8 is what
 * confirms this encoding is right on real hardware.
 */
#define ESC_CMD_NOP 0x00u
#define ESC_CMD_READ 0x02u
#define ESC_CMD_READ_WS 0x03u /* read, one wait state byte before data */
#define ESC_CMD_WRITE 0x04u

#define ESC_ADDR_HDR_LEN 2
#define ESC_WAIT_STATE_LEN 1

/* Largest single transfer we ever do. Process data for this application
 * is a handful of bytes; the header dominates. */
#define ESC_MAX_XFER 64

static const struct spi_dt_spec esc_spi =
	SPI_DT_SPEC_GET(DT_NODELABEL(esc),
			SPI_OP_MODE_MASTER | SPI_TRANSFER_MSB |
				SPI_MODE_CPOL | SPI_MODE_CPHA | SPI_WORD_SET(8));

static void esc_fill_header(uint8_t *hdr, uint16_t adr, uint8_t cmd)
{
	hdr[0] = (uint8_t)(adr >> 5);
	hdr[1] = (uint8_t)(((adr & 0x1fu) << 3) | (cmd & 0x07u));
}

int esc_init(void)
{
	if (!spi_is_ready_dt(&esc_spi)) {
		LOG_ERR("SPI bus for the AX58100 is not ready");
		return -ENODEV;
	}
	LOG_INF("ESC SPI ready: mode 3, %u Hz, CS active low",
		esc_spi.config.frequency);
	return 0;
}

int esc_read(uint16_t adr, void *buf, size_t len)
{
	if (len == 0 || len > ESC_MAX_XFER) {
		return -EINVAL;
	}

	const size_t total = ESC_ADDR_HDR_LEN + ESC_WAIT_STATE_LEN + len;
	uint8_t tx[ESC_ADDR_HDR_LEN + ESC_WAIT_STATE_LEN + ESC_MAX_XFER];
	uint8_t rx[ESC_ADDR_HDR_LEN + ESC_WAIT_STATE_LEN + ESC_MAX_XFER];

	esc_fill_header(tx, adr, ESC_CMD_READ_WS);
	/* Wait state byte and the data phase: MOSI content is ignored by
	 * the ESC, but the last byte clocked out must be the read
	 * termination byte (0xff) so the ESC ends the access cleanly. */
	memset(&tx[ESC_ADDR_HDR_LEN], 0xff, total - ESC_ADDR_HDR_LEN);

	const struct spi_buf tx_buf = {.buf = tx, .len = total};
	const struct spi_buf rx_buf = {.buf = rx, .len = total};
	const struct spi_buf_set tx_set = {.buffers = &tx_buf, .count = 1};
	const struct spi_buf_set rx_set = {.buffers = &rx_buf, .count = 1};

	int rc = spi_transceive_dt(&esc_spi, &tx_set, &rx_set);
	if (rc < 0) {
		LOG_ERR("SPI read at 0x%04x failed: %d", adr, rc);
		return rc;
	}

	memcpy(buf, &rx[ESC_ADDR_HDR_LEN + ESC_WAIT_STATE_LEN], len);
	return 0;
}

int esc_write(uint16_t adr, const void *buf, size_t len)
{
	if (len == 0 || len > ESC_MAX_XFER) {
		return -EINVAL;
	}

	/* Writes have no wait state byte (datasheet Figure 4-19). */
	const size_t total = ESC_ADDR_HDR_LEN + len;
	uint8_t tx[ESC_ADDR_HDR_LEN + ESC_MAX_XFER];

	esc_fill_header(tx, adr, ESC_CMD_WRITE);
	memcpy(&tx[ESC_ADDR_HDR_LEN], buf, len);

	const struct spi_buf tx_buf = {.buf = tx, .len = total};
	const struct spi_buf_set tx_set = {.buffers = &tx_buf, .count = 1};

	int rc = spi_write_dt(&esc_spi, &tx_set);
	if (rc < 0) {
		LOG_ERR("SPI write at 0x%04x failed: %d", adr, rc);
	}
	return rc;
}

int esc_read8(uint16_t adr)
{
	uint8_t v = 0;
	int rc = esc_read(adr, &v, sizeof(v));

	return (rc < 0) ? rc : (int)v;
}
