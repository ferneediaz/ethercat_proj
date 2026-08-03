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
} _Objects;

extern _Objects Obj;

#endif /* __UTYPES_H__ */
