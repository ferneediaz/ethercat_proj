/*
 * SOES configuration for this slave.
 *
 * Every value here must match what the module's EEPROM actually declares,
 * because the master programs the SyncManagers from the EEPROM and SOES
 * assumes it already knows those addresses. Taken from `slaveinfo eth0` run
 * against this hardware on 2026-08-02:
 *
 *   SM0 A:1000 L:128 F:00010026   mailbox out
 *   SM1 A:1080 L:128 F:00010022   mailbox in
 *   SM2 A:1100 L:  2 F:00010064   outputs, master -> slave
 *   SM3 A:1400 L:  6 F:00010020   inputs,  slave -> master
 *
 * The mailbox pair matches SOES's own demo defaults exactly — both derive
 * from the same Beckhoff Slave Stack Code conventions, since this module
 * ships with the stock SSC demo EEPROM. Only SM2/SM3 needed adjusting.
 */
#ifndef __ECAT_OPTIONS_H__
#define __ECAT_OPTIONS_H__

#include "cc.h"
#include "device_identity.h"

/* File and Ethernet over EtherCAT: neither is used, and both cost flash. */
#define USE_FOE 0
#define USE_EOE 0

#define MBXSIZE 128
#define MBXSIZEBOOT 128
#define MBXBUFFERS 3

#define MBX0_sma 0x1000
#define MBX0_sml MBXSIZE
#define MBX0_sme (MBX0_sma + MBX0_sml - 1)
#define MBX0_smc 0x26
#define MBX1_sma (MBX0_sma + MBX0_sml)
#define MBX1_sml MBXSIZE
#define MBX1_sme (MBX1_sma + MBX1_sml - 1)
#define MBX1_smc 0x22

#define MBX0_sma_b 0x1000
#define MBX0_sml_b MBXSIZEBOOT
#define MBX0_sme_b (MBX0_sma_b + MBX0_sml_b - 1)
#define MBX0_smc_b 0x26
#define MBX1_sma_b (MBX0_sma_b + MBX0_sml_b)
#define MBX1_sml_b MBXSIZEBOOT
#define MBX1_sme_b (MBX1_sma_b + MBX1_sml_b - 1)
#define MBX1_smc_b 0x22

/* Outputs. 0x64 rather than the demo's 0x24: the extra bit is the watchdog
 * trigger, which this EEPROM enables. Reported as F:00010064 by slaveinfo. */
#define SM2_sma 0x1100
#define SM2_smc 0x64
#define SM2_act 1

/* Inputs. The EEPROM places these at 0x1400, not the 0x1180 SOES's demo
 * uses. */
#define SM3_sma 0x1400
#define SM3_smc 0x20
#define SM3_act 1

/* Process data is 2 bytes out, and 4 or 6 in depending on whether the EEPROM
 * has been rewritten. Left generous so a future PDO change does not silently
 * truncate. */
#define MAX_RXPDO_SIZE 42
#define MAX_TXPDO_SIZE 42

/* Mapping counts come from the ESI, plus the padding entry on the stock
 * EEPROM. SOES sizes its mapping arrays from these, so they must be at least
 * what slave_objectlist.c presents. */
#define MAX_MAPPINGS_SM2 DEV_RXPDO_ENTRIES
#define MAX_MAPPINGS_SM3 DEV_TXPDO_ENTRIES

#endif /* __ECAT_OPTIONS_H__ */
