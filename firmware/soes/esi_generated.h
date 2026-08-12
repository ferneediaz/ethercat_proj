/*
 * GENERATED FILE - DO NOT EDIT.
 *
 * Produced by scripts/esi_tool.py from esi/EthercatServoNode.xml.
 * Regenerate with:
 *
 *     ./scripts/esi_tool.py header -o firmware/soes/esi_generated.h
 *
 * These are the numbers the EEPROM image was built from. They describe the
 * device as the ESI declares it, which is only what the hardware reports
 * once that image has actually been written -- see device_identity.h for
 * the switch between this and the stock EEPROM the module ships with.
 */
#ifndef __ESI_GENERATED_H__
#define __ESI_GENERATED_H__

#define ESI_VENDOR_ID       0x00000b95u
#define ESI_PRODUCT_CODE    0x00620300u
#define ESI_REVISION        0x00010000u
#define ESI_DEVICE_NAME     "ethercat_servo_node"

/* SyncManager 2: master -> slave. */
#define ESI_SM2_ADDR        0x1100u
#define ESI_SM2_CONTROL     0x64u
#define ESI_RXPDO_BYTES     2

/* SyncManager 3: slave -> master. */
#define ESI_SM3_ADDR        0x1400u
#define ESI_SM3_CONTROL     0x20u
#define ESI_TXPDO_BYTES     4

/* Mapping entries, encoded as index<<16 | subindex<<8 | bitlength, in the
 * order the ESI lists them. The object dictionary must present exactly
 * these, and exactly this many. */
#define ESI_RXPDO_ENTRY_COUNT 1
#define ESI_TXPDO_ENTRY_COUNT 2
#define ESI_RXPDO_ENTRY_1   0x70000110u  /* Target Angle */
#define ESI_TXPDO_ENTRY_1   0x60000110u  /* Echo Angle */
#define ESI_TXPDO_ENTRY_2   0x60000210u  /* Actual Angle */

#endif /* __ESI_GENERATED_H__ */
