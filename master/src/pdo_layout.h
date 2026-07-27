#ifndef PDO_LAYOUT_H
#define PDO_LAYOUT_H

#include <stdint.h>

/*
 * Process data layout shared between this master and the slave.
 *
 * IMPORTANT: these structs must match, byte for byte, the object
 * dictionary / process data mapping in the STM32 SOES application
 * (firmware/, Part B of the plan). If one side changes, change both.
 *
 * EtherCAT process data is little-endian on the wire; both the Pi
 * and the STM32 are little-endian, so plain packed structs work.
 */

#pragma pack(push, 1)

/* Master -> slave (outputs, RxPDO from the slave's point of view) */
typedef struct
{
   uint16_t target_angle; /* degrees, 0..180 */
} servo_outputs_t;

/* Slave -> master (inputs, TxPDO from the slave's point of view) */
typedef struct
{
   uint16_t echo_angle; /* slave echoes the angle it is applying */
} servo_inputs_t;

#pragma pack(pop)

/* Identity expected on the bus (from the ASIX ESI file) */
#define SERVO_VENDOR_ID 0x00000B95u  /* ASIX */
#define SERVO_PRODUCT_CODE 0x00620300u

#endif /* PDO_LAYOUT_H */
