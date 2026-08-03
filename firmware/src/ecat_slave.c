#include "ecat_slave.h"

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "ax58100.h"

LOG_MODULE_REGISTER(ecat, LOG_LEVEL_INF);

/*
 * Process data sizes. These are dictated by the EEPROM the module ships
 * with — the stock Beckhoff Slave Stack Code demo — which declares 2 bytes
 * of outputs and 6 of inputs. We match it rather than rewriting the EEPROM,
 * because a bad EEPROM write stops the module enumerating at all and there
 * is nothing here that needs a different layout.
 *
 * Only the first two input bytes carry meaning; the rest are padding that
 * exists solely so the length the master programmed into SM3 is satisfied.
 * These must agree with master/src/pdo_layout.h.
 */
#define PDO_OUT_LEN 2u
#define PDO_IN_LEN 6u

struct sm_cfg {
	uint16_t addr;
	uint16_t len;
};

static struct sm_cfg sm_out;
static struct sm_cfg sm_in;
static uint8_t cur_state = ESC_AL_INIT;
static ecat_apply_fn apply_angle;
static uint16_t last_angle;

/*
 * False until an output value has actually been applied in the current spell
 * in OP. Cleared on every exit from OP so that re-entering re-applies even if
 * the master resends the angle we were already holding — after a bus drop the
 * shaft may not be where last_angle says any more.
 *
 * Without this, gating on `target != last_angle` alone would swallow the very
 * first command of a session whenever it happened to be 0.
 */
static bool angle_applied;

const char *ecat_state_name(uint8_t state)
{
	switch (state & ESC_AL_STATE_MASK) {
	case ESC_AL_INIT: return "INIT";
	case ESC_AL_PREOP: return "PREOP";
	case ESC_AL_BOOT: return "BOOT";
	case ESC_AL_SAFEOP: return "SAFEOP";
	case ESC_AL_OP: return "OP";
	default: return "?";
	}
}

uint8_t ecat_slave_state(void)
{
	return cur_state;
}

/* AL Status is 16-bit and little-endian on the wire; the ESP32 is
 * little-endian too, so the struct layout needs no swapping. */
static void report_state(uint8_t state)
{
	uint16_t v = state;

	esc_write(ESC_REG_AL_STATUS, &v, sizeof(v));
	cur_state = state;

	/* Any state but OP means the outputs are not ours to apply. Forget that
	 * we ever applied one, so the first command after coming back into OP
	 * is delivered even if it repeats the angle we left off holding. */
	if (state != ESC_AL_OP) {
		angle_applied = false;
	}
}

/*
 * Refuse a transition the way a real slave must: leave AL Status at the
 * state we are actually in, set the error bit so the master knows the
 * request was rejected rather than still pending, and put the reason in AL
 * Status Code. Without the code the master only learns that something went
 * wrong, not what.
 */
static void refuse_transition(uint16_t reason)
{
	uint16_t code = reason;
	uint16_t status = (uint16_t)(cur_state | ESC_AL_ERROR_ACK);

	esc_write(ESC_REG_AL_STATUS_CODE, &code, sizeof(code));
	esc_write(ESC_REG_AL_STATUS, &status, sizeof(status));
	LOG_ERR("refused transition, AL Status Code 0x%04x", reason);
}

static int read_sm(uint8_t n, struct sm_cfg *out)
{
	uint8_t buf[4];
	int rc = esc_read(ESC_SM_REG(n, ESC_SM_PHYS_ADDR), buf, sizeof(buf));

	if (rc < 0) {
		return rc;
	}
	out->addr = (uint16_t)(buf[0] | (buf[1] << 8));
	out->len = (uint16_t)(buf[2] | (buf[3] << 8));
	return 0;
}

/*
 * PREOP -> SAFEOP is where the process data actually gets located.
 *
 * The master has just programmed SM2 and SM3 from the EEPROM's description
 * of this slave. Reading them back is what tells us the DPRAM addresses;
 * they are assigned by the master and hardcoding them would break the
 * moment anything about the configuration changed.
 */
static bool enter_safeop(void)
{
	if (read_sm(ESC_SM_OUTPUTS, &sm_out) < 0 ||
	    read_sm(ESC_SM_INPUTS, &sm_in) < 0) {
		LOG_ERR("could not read SyncManager configuration");
		return false;
	}

	LOG_INF("SM2 outputs @0x%04x len %u | SM3 inputs @0x%04x len %u",
		sm_out.addr, sm_out.len, sm_in.addr, sm_in.len);

	if (sm_out.len != PDO_OUT_LEN) {
		LOG_ERR("SM2 length %u, expected %u", sm_out.len, PDO_OUT_LEN);
		refuse_transition(ESC_ALSC_INVALID_OUTPUT_CFG);
		return false;
	}
	if (sm_in.len != PDO_IN_LEN) {
		LOG_ERR("SM3 length %u, expected %u", sm_in.len, PDO_IN_LEN);
		refuse_transition(ESC_ALSC_INVALID_INPUT_CFG);
		return false;
	}
	return true;
}

/*
 * One cycle of process data.
 *
 * Inputs are produced in both SAFEOP and OP — that is what SAFEOP means:
 * the master sees valid data while the outputs stay ignored. Outputs are
 * only applied in OP, which is the whole safety point of the distinction,
 * so an operator can watch a slave's feedback before it is allowed to move.
 *
 * The read of SM3 is also what increments the working counter the master
 * checks, so the exchange has to run every cycle even when nothing has
 * changed.
 *
 * The actuator, though, is driven only on change. Re-commanding an unchanged
 * target every cycle is not the harmless no-op it looks like: a stepper's
 * motion controller re-arms its timing source and emits a STEP edge on every
 * call, so a move spanning several cycles gets its pulses chopped and loses
 * steps that nothing on this board can detect. A servo's PWM is latched in
 * hardware and does not need rewriting either. Applying on change is both
 * correct and cheaper.
 */
static void exchange_process_data(void)
{
	uint8_t out[PDO_OUT_LEN];
	uint8_t in[PDO_IN_LEN];

	if (esc_read(sm_out.addr, out, sizeof(out)) == 0) {
		uint16_t target = (uint16_t)(out[0] | (out[1] << 8));

		if (cur_state == ESC_AL_OP && (!angle_applied || target != last_angle)) {
			LOG_INF("target %u deg", target);
			last_angle = target;
			angle_applied = true;

			if (apply_angle != NULL) {
				apply_angle(target);
			}
		}
	}

	/* Echo back what we are applying. The master compares this with what
	 * it sent, which is the cheapest possible end-to-end check that the
	 * whole chain is live rather than merely connected. */
	memset(in, 0, sizeof(in));
	in[0] = (uint8_t)(last_angle & 0xff);
	in[1] = (uint8_t)(last_angle >> 8);
	esc_write(sm_in.addr, in, sizeof(in));
}

int ecat_slave_init(ecat_apply_fn apply)
{
	apply_angle = apply;
	last_angle = 0;
	angle_applied = false;
	report_state(ESC_AL_INIT);
	LOG_INF("EtherCAT slave ready, state INIT");
	return 0;
}

void ecat_slave_poll(void)
{
	int ctrl = esc_read8(ESC_REG_AL_CONTROL);

	if (ctrl < 0) {
		return;
	}

	uint8_t req = (uint8_t)ctrl & ESC_AL_STATE_MASK;

	if (req != cur_state) {
		LOG_INF("%s -> %s", ecat_state_name(cur_state),
			ecat_state_name(req));

		switch (req) {
		case ESC_AL_INIT:
		case ESC_AL_PREOP:
			/* Nothing to tear down: the actuator holds its last
			 * position rather than snapping to zero, which is
			 * what an operator expects when a bus drops. */
			report_state(req);
			break;

		case ESC_AL_SAFEOP:
			if (enter_safeop()) {
				report_state(ESC_AL_SAFEOP);
			}
			break;

		case ESC_AL_OP:
			/* Only reachable from SAFEOP, where the SyncManager
			 * configuration was already validated. */
			if (cur_state == ESC_AL_SAFEOP) {
				report_state(ESC_AL_OP);
			} else {
				refuse_transition(ESC_ALSC_INVALID_OUTPUT_CFG);
			}
			break;

		default:
			LOG_WRN("unsupported state request 0x%02x", req);
			break;
		}
	}

	if (cur_state == ESC_AL_SAFEOP || cur_state == ESC_AL_OP) {
		exchange_process_data();
	}
}
