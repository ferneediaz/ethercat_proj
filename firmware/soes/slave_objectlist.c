/*
 * Object dictionary.
 *
 * Normally generated from an ESI file by SOES's tooling. Hand-written here
 * because the dictionary is small and, more importantly, because it must be
 * pinned to what the module's existing EEPROM already declares rather than
 * to an ESI we would otherwise have to flash.
 *
 * The two mappings below are what make the PDO sizes come out right:
 *
 *   RxPDO 0x1600 -> 0x7000:01, 16 bits            = 2 bytes  (matches SM2)
 *   TxPDO 0x1A00 -> 0x6000:01, 16 bits
 *                 + 32 bits of padding            = 6 bytes  (matches SM3)
 *
 * The padding entry is not decoration. The EEPROM declares SM3 as 6 bytes,
 * and if the mapping produced only 2 the master would compute a different
 * input size than the SyncManager it programmed, and refuse SAFEOP.
 */
#include <stddef.h>

#include "esc_coe.h"
#include "utypes.h"

#ifndef HW_REV
#define HW_REV "1.0"
#endif

#ifndef SW_REV
#define SW_REV "1.0"
#endif

static const char acName1000[] = "Device Type";
static const char acName1008[] = "Device Name";
static const char acName1009[] = "Hardware Version";
static const char acName100A[] = "Software Version";
static const char acName1018[] = "Identity Object";
static const char acName1018_00[] = "Max SubIndex";
static const char acName1018_01[] = "Vendor ID";
static const char acName1018_02[] = "Product Code";
static const char acName1018_03[] = "Revision Number";
static const char acName1018_04[] = "Serial Number";
static const char acName1600[] = "Servo RxPDO";
static const char acName1600_00[] = "Max SubIndex";
static const char acName1600_01[] = "Target Angle";
static const char acName1A00[] = "Servo TxPDO";
static const char acName1A00_00[] = "Max SubIndex";
static const char acName1A00_01[] = "Echo Angle";
static const char acName1A00_02[] = "Padding";
static const char acName1C00[] = "Sync Manager Communication Type";
static const char acName1C00_00[] = "Max SubIndex";
static const char acName1C00_01[] = "Communications Type SM0";
static const char acName1C00_02[] = "Communications Type SM1";
static const char acName1C00_03[] = "Communications Type SM2";
static const char acName1C00_04[] = "Communications Type SM3";
static const char acName1C12[] = "Sync Manager 2 PDO Assignment";
static const char acName1C12_00[] = "Max SubIndex";
static const char acName1C12_01[] = "PDO Mapping";
static const char acName1C13[] = "Sync Manager 3 PDO Assignment";
static const char acName1C13_00[] = "Max SubIndex";
static const char acName1C13_01[] = "PDO Mapping";
static const char acName6000[] = "Servo Inputs";
static const char acName6000_00[] = "Max SubIndex";
static const char acName6000_01[] = "Echo Angle";
static const char acName7000[] = "Servo Outputs";
static const char acName7000_00[] = "Max SubIndex";
static const char acName7000_01[] = "Target Angle";

const _objd SDO1000[] = {
	{0x0, DTYPE_UNSIGNED32, 32, ATYPE_RO, acName1000, 0x00001389, NULL},
};
const _objd SDO1008[] = {
	{0x0, DTYPE_VISIBLE_STRING, 152, ATYPE_RO, acName1008, 0,
	 "ethercat_servo_node"},
};
const _objd SDO1009[] = {
	{0x0, DTYPE_VISIBLE_STRING, 0, ATYPE_RO, acName1009, 0, HW_REV},
};
const _objd SDO100A[] = {
	{0x0, DTYPE_VISIBLE_STRING, 0, ATYPE_RO, acName100A, 0, SW_REV},
};

/*
 * Identity deliberately mirrors the stock EEPROM (vendor 0x00000009,
 * product 0x26483052, revision 0x00020111). A master that compares the CoE
 * identity against the SII identity must see the same numbers, and we are
 * not rewriting the SII.
 */
const _objd SDO1018[] = {
	{0x00, DTYPE_UNSIGNED8, 8, ATYPE_RO, acName1018_00, 4, NULL},
	{0x01, DTYPE_UNSIGNED32, 32, ATYPE_RO, acName1018_01, 0x00000009, NULL},
	{0x02, DTYPE_UNSIGNED32, 32, ATYPE_RO, acName1018_02, 0x26483052, NULL},
	{0x03, DTYPE_UNSIGNED32, 32, ATYPE_RO, acName1018_03, 0x00020111, NULL},
	{0x04, DTYPE_UNSIGNED32, 32, ATYPE_RO, acName1018_04, 0x00000000, NULL},
};

/* 0x70000110 = index 0x7000, subindex 0x01, 0x10 (16) bits. */
const _objd SDO1600[] = {
	{0x00, DTYPE_UNSIGNED8, 8, ATYPE_RO, acName1600_00, 1, NULL},
	{0x01, DTYPE_UNSIGNED32, 32, ATYPE_RO, acName1600_01, 0x70000110, NULL},
};

/* 0x00000020 is the standard "gap" entry: no object, 0x20 (32) bits of
 * padding, taking this PDO from 2 bytes to the 6 the EEPROM declares. */
const _objd SDO1A00[] = {
	{0x00, DTYPE_UNSIGNED8, 8, ATYPE_RO, acName1A00_00, 2, NULL},
	{0x01, DTYPE_UNSIGNED32, 32, ATYPE_RO, acName1A00_01, 0x60000110, NULL},
	{0x02, DTYPE_UNSIGNED32, 32, ATYPE_RO, acName1A00_02, 0x00000020, NULL},
};

const _objd SDO1C00[] = {
	{0x00, DTYPE_UNSIGNED8, 8, ATYPE_RO, acName1C00_00, 4, NULL},
	{0x01, DTYPE_UNSIGNED8, 8, ATYPE_RO, acName1C00_01, 1, NULL},
	{0x02, DTYPE_UNSIGNED8, 8, ATYPE_RO, acName1C00_02, 2, NULL},
	{0x03, DTYPE_UNSIGNED8, 8, ATYPE_RO, acName1C00_03, 3, NULL},
	{0x04, DTYPE_UNSIGNED8, 8, ATYPE_RO, acName1C00_04, 4, NULL},
};
const _objd SDO1C12[] = {
	{0x00, DTYPE_UNSIGNED8, 8, ATYPE_RO, acName1C12_00, 1, NULL},
	{0x01, DTYPE_UNSIGNED16, 16, ATYPE_RO, acName1C12_01, 0x1600, NULL},
};
const _objd SDO1C13[] = {
	{0x00, DTYPE_UNSIGNED8, 8, ATYPE_RO, acName1C13_00, 1, NULL},
	{0x01, DTYPE_UNSIGNED16, 16, ATYPE_RO, acName1C13_01, 0x1A00, NULL},
};

const _objd SDO6000[] = {
	{0x00, DTYPE_UNSIGNED8, 8, ATYPE_RO, acName6000_00, 1, NULL},
	{0x01, DTYPE_UNSIGNED16, 16, ATYPE_RO, acName6000_01, 0,
	 &Obj.Inputs.echo_angle},
};
const _objd SDO7000[] = {
	{0x00, DTYPE_UNSIGNED8, 8, ATYPE_RO, acName7000_00, 1, NULL},
	{0x01, DTYPE_UNSIGNED16, 16, ATYPE_RW, acName7000_01, 0,
	 &Obj.Outputs.target_angle},
};

const _objectlist SDOobjects[] = {
	{0x1000, OTYPE_VAR, 0, 0, acName1000, SDO1000},
	{0x1008, OTYPE_VAR, 0, 0, acName1008, SDO1008},
	{0x1009, OTYPE_VAR, 0, 0, acName1009, SDO1009},
	{0x100A, OTYPE_VAR, 0, 0, acName100A, SDO100A},
	{0x1018, OTYPE_RECORD, 4, 0, acName1018, SDO1018},
	{0x1600, OTYPE_RECORD, 1, 0, acName1600, SDO1600},
	{0x1A00, OTYPE_RECORD, 2, 0, acName1A00, SDO1A00},
	{0x1C00, OTYPE_ARRAY, 4, 0, acName1C00, SDO1C00},
	{0x1C12, OTYPE_ARRAY, 1, 0, acName1C12, SDO1C12},
	{0x1C13, OTYPE_ARRAY, 1, 0, acName1C13, SDO1C13},
	{0x6000, OTYPE_RECORD, 1, 0, acName6000, SDO6000},
	{0x7000, OTYPE_RECORD, 1, 0, acName7000, SDO7000},
	{0xffff, 0xff, 0xff, 0xff, NULL, NULL},
};
