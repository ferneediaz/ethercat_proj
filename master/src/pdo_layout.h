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
 * the module ships with declares for SyncManager 3 -- unless the EEPROM has
 * been rewritten with our own ESI, in which case it is four and the trailing
 * padding is gone. SERVO_SII_REWRITTEN selects between the two; see
 * firmware/soes/device_identity.h, which makes the same choice on the slave
 * side from the same fact about the hardware.
 *
 * actual_angle occupies two of the four bytes that used to be padding under
 * the stock image, which is why that image needs only two bytes of filler
 * rather than four.
 *
 * The two fields are NOT the same thing, and the difference is the point:
 *
 *   echo_angle   what the slave was told and accepted. Confirms the command
 *                arrived. Equals target as soon as a frame lands.
 *   actual_angle where the axis actually is, read from the motion
 *                controller's position counter. Lags echo while moving and
 *                converges on arrival.
 *
 * This is the Target Position / Position Actual Value pairing CiA 402 is
 * built around. It remains open loop: the counter reports steps emitted, not
 * steps the motor took, so a skipped step is invisible here. Closing that
 * needs an encoder on the shaft. */
typedef struct
{
   uint16_t echo_angle;   /* the angle the slave accepted */
   uint16_t actual_angle; /* where the axis reports it actually is */
#ifndef SERVO_SII_REWRITTEN
   uint8_t reserved[2];   /* unused; present to match the stock SM3 size */
#endif
} servo_inputs_t;

#pragma pack(pop)

/* Expected input size, and the identity that goes with it. Both follow from
 * which image is in the EEPROM, so they are chosen together rather than
 * separately -- picking one and forgetting the other is exactly the drift
 * this header exists to prevent. */
#ifdef SERVO_SII_REWRITTEN
#define SERVO_INPUT_BYTES 4
/* Ours, from esi/EthercatServoNode.xml. The vendor ID is still ASIX's: it is
 * their development hardware, and a vendor ID is assigned by the ETG rather
 * than chosen. The revision distinguishes this firmware from their kit. */
#define SERVO_VENDOR_ID 0x00000B95u
#define SERVO_PRODUCT_CODE 0x00620300u
#define SERVO_REVISION 0x00010000u
#else
#define SERVO_INPUT_BYTES 6
/* What the ASIX ESI would report if flashed. Kept so scan output against an
 * ASIX reference kit is still recognised rather than reported as unknown. */
#define SERVO_VENDOR_ID 0x00000B95u
#define SERVO_PRODUCT_CODE 0x00620300u
#define SERVO_REVISION 0x00000002u
#endif

/* The whole point of this header is that both sides agree byte for byte.
 * Catch a mismatch at compile time rather than as a silent WKC failure. */
_Static_assert(sizeof(servo_outputs_t) == 2, "outputs must be 2 bytes (SM2)");
_Static_assert(sizeof(servo_inputs_t) == SERVO_INPUT_BYTES,
               "inputs must match the SM3 size the EEPROM declares");

/* What the module actually reports as shipped: the stock Beckhoff
 * Slave Stack Code demo EEPROM. Observed 2026-07-31 with
 * rev 0x00020111, 2 bytes out / 6 bytes in, CoE mailbox, DC capable. */
#define SSC_DEFAULT_VENDOR_ID 0x00000009u /* Beckhoff */
#define SSC_DEFAULT_PRODUCT_CODE 0x26483052u

#endif /* PDO_LAYOUT_H */
