/*
 * Which device this firmware claims to be, and how big its process data is.
 *
 * There are two answers, and which one is correct depends on a fact this
 * firmware cannot discover for itself: what is actually in the ESC's EEPROM.
 *
 *   CONFIG_ESC_SII_REWRITTEN=n   the module still carries the stock Beckhoff
 *                                SSC demo image it shipped with. Identity is
 *                                Beckhoff's, and SM3 is six bytes, so the
 *                                TxPDO needs a padding entry to reach that
 *                                length.
 *
 *   CONFIG_ESC_SII_REWRITTEN=y   scripts/write-eeprom.sh has been run and the
 *                                EEPROM now holds the image built from
 *                                esi/EthercatServoNode.xml. Identity is ours,
 *                                SM3 is four bytes, and the padding is gone.
 *
 * The master programs the SyncManagers from the EEPROM and then checks the
 * CoE object dictionary against them. Pick the wrong one here and the sizes
 * disagree, so the master refuses SAFEOP -- the bus comes up to PREOP and
 * stops. That failure is loud and harmless, but it is confusing if you do not
 * know to look here, which is why both sets live in one file.
 *
 * ORDER MATTERS: write the EEPROM first, then rebuild with this on. Doing it
 * the other way round leaves a slave whose dictionary is smaller than the
 * SyncManager the master programmed for it.
 */
#ifndef __DEVICE_IDENTITY_H__
#define __DEVICE_IDENTITY_H__

#include <stdint.h>

#include "esi_generated.h"

#ifdef CONFIG_ESC_SII_REWRITTEN

#define DEV_VENDOR_ID        ESI_VENDOR_ID
#define DEV_PRODUCT_CODE     ESI_PRODUCT_CODE
#define DEV_REVISION         ESI_REVISION
#define DEV_NAME             ESI_DEVICE_NAME

#define DEV_RXPDO_BYTES      ESI_RXPDO_BYTES
#define DEV_TXPDO_BYTES      ESI_TXPDO_BYTES
#define DEV_RXPDO_ENTRIES    ESI_RXPDO_ENTRY_COUNT
#define DEV_TXPDO_ENTRIES    ESI_TXPDO_ENTRY_COUNT

#else /* stock EEPROM */

/*
 * Beckhoff's, deliberately. A master that compares the CoE identity against
 * the SII identity must see the same numbers on both, and under this build
 * the SII is still theirs. Claiming our own identity here would produce a
 * mismatch, not a nicer-looking slave.
 */
#define DEV_VENDOR_ID        0x00000009u
#define DEV_PRODUCT_CODE     0x26483052u
#define DEV_REVISION         0x00020111u
#define DEV_NAME             "ethercat_servo_node"

#define DEV_RXPDO_BYTES      2
#define DEV_TXPDO_BYTES      6
#define DEV_RXPDO_ENTRIES    ESI_RXPDO_ENTRY_COUNT
/* The ESI's entries plus one gap entry to reach the stock SM3 length. */
#define DEV_TXPDO_ENTRIES    (ESI_TXPDO_ENTRY_COUNT + 1)

/*
 * A mapping entry with no object behind it: index 0, subindex 0, and a bit
 * length that pads the TxPDO out to what the EEPROM declares. Sized from the
 * two numbers above rather than written as a literal, so that adding a real
 * field to the TxPDO shrinks the gap automatically instead of silently
 * overflowing SM3.
 */
#define DEV_TXPDO_PAD_BITS   ((DEV_TXPDO_BYTES - ESI_TXPDO_BYTES) * 8)
#define DEV_TXPDO_PAD_ENTRY  ((uint32_t)DEV_TXPDO_PAD_BITS)

#endif /* CONFIG_ESC_SII_REWRITTEN */

#endif /* __DEVICE_IDENTITY_H__ */
