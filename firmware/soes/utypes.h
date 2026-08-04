/*
 * Object dictionary storage for this slave.
 *
 * The process data is deliberately tiny and must stay byte-compatible with
 * master/src/pdo_layout.h and with what the stock EEPROM declares: 2 bytes
 * of outputs, 6 of inputs.
 */
#ifndef __UTYPES_H__
#define __UTYPES_H__

#include "cc.h"

typedef struct {
	/* Inputs: slave -> master (TxPDO 0x1A00, mapped from 0x6000) */
	struct {
		uint16_t echo_angle;
		uint16_t actual_angle;
	} Inputs;

	/* Outputs: master -> slave (RxPDO 0x1600, mapped from 0x7000) */
	struct {
		uint16_t target_angle;
	} Outputs;

	/*
	 * Axis parameters (0x8000). Not mapped into any PDO — these are
	 * configuration, reached over the CoE mailbox with SDO read/write
	 * while the bus runs, which is how real EtherCAT devices are set up.
	 *
	 * Seeded from the devicetree at startup so the dictionary and the
	 * hardware description cannot disagree.
	 */
	struct {
		uint32_t step_interval_ns;
		uint16_t max_angle;
		uint16_t steps_per_rev;
		uint8_t microstep_factor;
	} Axis;
} _Objects;

extern _Objects Obj;

#endif /* __UTYPES_H__ */
