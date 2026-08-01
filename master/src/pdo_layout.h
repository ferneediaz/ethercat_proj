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

/* Slave -> master (inputs, TxPDO from the slave's point of view)
 *
 * Six bytes, not two, because that is what the stock Beckhoff demo EEPROM
 * the module ships with declares for SyncManager 3. Matching the EEPROM
 * avoids rewriting it, and a bad EEPROM write stops the module enumerating
 * at all. Only echo_angle carries meaning; the padding exists so the length
 * the master programs into SM3 is satisfied. */
typedef struct
{
   uint16_t echo_angle;  /* slave echoes the angle it is applying */
   uint8_t reserved[4];  /* unused; present to match the EEPROM's SM3 size */
} servo_inputs_t;

#pragma pack(pop)

/* The whole point of this header is that both sides agree byte for byte.
 * Catch a mismatch at compile time rather than as a silent WKC failure. */
_Static_assert(sizeof(servo_outputs_t) == 2, "outputs must be 2 bytes (SM2)");
_Static_assert(sizeof(servo_inputs_t) == 6, "inputs must be 6 bytes (SM3)");

/* Identity from the ASIX ESI file — what the EEPROM would report if we
 * flash the ASIX ESI onto the module. */
#define SERVO_VENDOR_ID 0x00000B95u  /* ASIX */
#define SERVO_PRODUCT_CODE 0x00620300u

/* What the module actually reports as shipped: the stock Beckhoff
 * Slave Stack Code demo EEPROM. Observed 2026-07-31 with
 * rev 0x00020111, 2 bytes out / 6 bytes in, CoE mailbox, DC capable. */
#define SSC_DEFAULT_VENDOR_ID 0x00000009u /* Beckhoff */
#define SSC_DEFAULT_PRODUCT_CODE 0x26483052u

#endif /* PDO_LAYOUT_H */
