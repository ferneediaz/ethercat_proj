/*
 * SOES hardware layer for the AX58100 over PDI-SPI.
 *
 * SOES needs exactly four functions. Three of them are one-liners here
 * because the AX58100 is register-compatible with the ET1100 and we address
 * it directly: there is no indirect access protocol to implement, which is
 * why the LAN9252 HAL that ships with SOES is 449 lines and this one is not.
 *
 * All of the real work already happened in ax58100.c, and was proven by
 * milestone 4a before any of this existed.
 */
#include <zephyr/logging/log.h>

#include "ax58100.h"
#include "esc.h"

LOG_MODULE_REGISTER(soes_hw, LOG_LEVEL_INF);

/*
 * SOES reads and writes multi-byte quantities constantly — SyncManager
 * configuration, mailbox buffers, process data. Single-byte access is the
 * exception, not the rule.
 *
 * That matters historically: esc_read() originally terminated every read
 * after its first data byte, which single-byte register checks could not
 * detect. SOES would have failed everywhere, and confusingly. The fix is in
 * ax58100.c; this comment is here so nobody reintroduces it.
 */
/*
 * Refresh the AL event register into ESCvar.
 *
 * This is the HAL's job, not the stack's — nothing in SOES assigns
 * ESCvar.ALevent, and every working HAL reads register 0x0220 into it. Miss
 * it and SOES runs, initialises cleanly and parks in INIT forever, because
 * ESC_state() only reads AL Control when the control bit is set here. The
 * symptom is a slave that looks alive to the master and never transitions.
 */
static void esc_refresh_alevent(void)
{
	uint16_t ev = 0;

	if (esc_read(ESCREG_ALEVENT, &ev, sizeof(ev)) == 0) {
		ESCvar.ALevent = etohs(ev);
	}
}

void ESC_read(uint16_t address, void *buf, uint16_t len)
{
	int rc = esc_read(address, buf, len);

	if (rc < 0) {
		LOG_ERR("ESC_read(0x%04x, %u) failed: %d", address, len, rc);
	}

	/*
	 * SOES polls by reading the local time register at the top of every
	 * cycle, so piggy-backing the AL event refresh here gives it a fresh
	 * value exactly when it needs one, without the application having to
	 * remember to ask. This is the same trick the LAN9252 HAL uses.
	 */
	if (address == ESCREG_LOCALTIME) {
		esc_refresh_alevent();
	}
}

void ESC_write(uint16_t address, void *buf, uint16_t len)
{
	int rc = esc_write(address, buf, len);

	if (rc < 0) {
		LOG_ERR("ESC_write(0x%04x, %u) failed: %d", address, len, rc);
	}
}

void ESC_init(const esc_cfg_t *cfg)
{
	ARG_UNUSED(cfg);
	/* The SPI bus is brought up by esc_init() in main() before SOES
	 * starts, so that a failure there is reported against milestone 4a
	 * rather than surfacing as an unexplained stack error. */
}

/*
 * No reset line is wired to the ESC on this module — the header exposes
 * SCK/MISO/MOSI/NSS/IRQ/SYNC0 and nothing else. The chip is reset by power
 * cycling the board, which is acceptable because SOES only calls this on
 * fatal recovery paths that we would want to investigate by hand anyway.
 */
void ESC_reset(void)
{
	LOG_WRN("ESC_reset requested, but no reset line is wired on this module");
}
