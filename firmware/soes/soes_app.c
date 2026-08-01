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

#include "ecat_slv.h"
#include "esc.h"
#include "utypes.h"

LOG_MODULE_REGISTER(soes_app, LOG_LEVEL_INF);

_Objects Obj;

static soes_apply_fn apply_angle;
static uint16_t last_logged = 0xffff;

/*
 * Outputs have arrived from the master. In SAFEOP SOES clears them rather
 * than delivering stale data, so this is only meaningful in OP — which is
 * precisely the guarantee SAFEOP exists to give.
 */
void cb_set_outputs(void)
{
	uint16_t target = Obj.Outputs.target_angle;

	if (target != last_logged) {
		LOG_INF("target %u deg", target);
		last_logged = target;
	}
	if (apply_angle != NULL) {
		apply_angle(target);
	}
}

/*
 * Publish what we are actually applying, not what was asked for. If the two
 * ever diverge the master sees it immediately, which is the cheapest
 * end-to-end check that the chain is live rather than merely connected.
 */
void cb_get_inputs(void)
{
	Obj.Inputs.echo_angle = Obj.Outputs.target_angle;
}

void soes_app_run(soes_apply_fn apply)
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

	LOG_INF("starting SOES");
	ecat_slv_init(&config);

	/*
	 * SOES prints only during init, so without this the stack runs
	 * completely silently and a slave stuck in INIT is indistinguishable
	 * from one that is not running at all.
	 */
	uint8_t last_status = 0xff;
	uint16_t last_error = 0xffff;

	while (1) {
		ecat_slv();

		if (ESCvar.ALstatus != last_status ||
		    ESCvar.ALerror != last_error) {
			LOG_INF("AL control=0x%02x status=0x%02x error=0x%04x "
				"SM2=%u SM3=%u",
				ESCvar.ALcontrol, ESCvar.ALstatus,
				ESCvar.ALerror, ESCvar.ESC_SM2_sml,
				ESCvar.ESC_SM3_sml);
			last_status = ESCvar.ALstatus;
			last_error = ESCvar.ALerror;
		}
		k_sleep(K_MSEC(1));
	}
}
