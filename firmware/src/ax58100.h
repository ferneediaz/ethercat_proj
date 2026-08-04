/*
 * AX58100 EtherCAT Slave Controller register access over PDI-SPI.
 *
 * This is the layer SOES will eventually sit on top of. It is kept
 * deliberately free of any SOES dependency so it can be proven on its
 * own first (see MILESTONE 4a in docs/bringup-checklist.md).
 */
#ifndef AX58100_H
#define AX58100_H

#include <stddef.h>
#include <stdint.h>

/* ESC registers we care about during bring-up. Values in the comments
 * were read over EtherCAT from the Pi with `servo_master eth0 regs`,
 * so they are known-good expected answers for the SPI path. */
#define ESC_REG_TYPE 0x0000u       /* expect 0xc8  */
#define ESC_REG_REVISION 0x0001u   /* expect 0x00  */
#define ESC_REG_BUILD 0x0002u      /* 16-bit       */
#define ESC_REG_PDI_CONTROL 0x0140u/* expect 0x05 (SPI slave) */
#define ESC_REG_ESC_CONFIG 0x0141u /* expect 0x0e (device emulation off) */
#define ESC_REG_PDI_CONFIG 0x0150u /* expect 0x03 (mode 3, CS active low) */

/*
 * SyncManager watchdog status, register 0x0440, ETG.1000.4.
 *
 * Bit 0 is 1 while the watchdog is being fed and 0 once it has expired. That
 * is the authoritative "is the master still there" signal on this hardware,
 * and it is time-based — unlike SOES's own watchdog, which counts poll
 * iterations and so means different things depending on how fast the loop
 * happens to be running.
 *
 * The register address itself is not defined here: SOES already has it as
 * ESCREG_WDSTATUS with a reader (ESC_WDstatus), and the master programs the
 * divider and timeout. Only the bit meaning is ours to name.
 *
 * The EEPROM already enables the watchdog trigger bit on SM2 (F:00010064 in
 * slaveinfo), so nothing has to be written to make this work.
 */
#define ESC_WD_STATUS_OK 0x01u /* bit 0: set while the watchdog is fed */

/*
 * AL Event, ETG.1000.4. Two registers with the same bit layout:
 *
 *   0x0220 AL Event Request — what has happened and needs servicing.
 *   0x0204 AL Event Mask    — which of those are allowed to assert SINT.
 *
 * The mask resets to ZERO, so SINT stays silent until something writes it.
 * That is why the IRQ wire looked dead for most of this project: the pin was
 * connected and the chip was working, and nothing had ever asked it to speak.
 *
 * Reading 0x0220 is what clears the event and releases the pin, which matters
 * because SINT is level triggered on this module (EEPROM byte 0x0A bit 4).
 *
 * Values match SOES's ESCREG_ALEVENT_* exactly — both come from the spec. They
 * are restated here so this layer builds without SOES, since the hand-written
 * slave uses the same hardware.
 */
#define ESC_REG_AL_EVENT_MASK 0x0204u
#define ESC_REG_AL_EVENT 0x0220u

#define ESC_ALEVENT_CONTROL 0x00000001u  /* master wrote AL Control */
#define ESC_ALEVENT_DC_LATCH 0x00000002u
#define ESC_ALEVENT_DC_SYNC0 0x00000004u
#define ESC_ALEVENT_DC_SYNC1 0x00000008u
#define ESC_ALEVENT_SMCHANGE 0x00000010u /* SM activation register changed */
#define ESC_ALEVENT_EEP 0x00000020u
#define ESC_ALEVENT_WD 0x00000040u
#define ESC_ALEVENT_SM0 0x00000100u      /* mailbox out written */
#define ESC_ALEVENT_SM1 0x00000200u      /* mailbox in read */
#define ESC_ALEVENT_SM2 0x00000400u      /* process outputs written */
#define ESC_ALEVENT_SM3 0x00000800u      /* process inputs read */
/* AL state machine. Addresses per AX58100 datasheet v105 register map
 * (and ETG.1000): AL Control is what the master requests, AL Status is
 * what the slave reports back, AL Status Code carries the reason for a
 * refusal. These three were previously shifted by one slot here, which
 * made a correct "AL Status = 0x01 (INIT)" read look like an AL Control
 * value and hid the master's actual request completely. */
#define ESC_REG_AL_CONTROL 0x0120u     /* master writes the requested state */
#define ESC_REG_AL_STATUS 0x0130u      /* slave writes the achieved state */
#define ESC_REG_AL_STATUS_CODE 0x0134u /* slave writes the refusal reason */

/* AL states, low nibble of AL Control / AL Status. */
#define ESC_AL_INIT 0x01u
#define ESC_AL_PREOP 0x02u
#define ESC_AL_BOOT 0x03u
#define ESC_AL_SAFEOP 0x04u
#define ESC_AL_OP 0x08u
#define ESC_AL_ERROR_ACK 0x10u /* set by the master to clear an error */
#define ESC_AL_STATE_MASK 0x0fu

/*
 * SyncManagers. Eight bytes of configuration each, starting at 0x0800:
 *
 *   +0  physical start address in the ESC's DPRAM (16 bit)
 *   +2  length in bytes (16 bit)
 *   +4  control (buffer type, direction)
 *   +5  status
 *   +6  activate (bit 0 enables the SyncManager)
 *   +7  PDI control
 *
 * The master programs these from the EEPROM's description of the slave on
 * the PREOP->SAFEOP transition. The slave reads them back to learn where in
 * DPRAM its process data actually lives — those addresses are not fixed and
 * must never be hardcoded.
 *
 * SM0/SM1 are the mailbox pair; SM2 carries outputs (master -> slave) and
 * SM3 inputs (slave -> master).
 */
#define ESC_SM_BASE 0x0800u
#define ESC_SM_REG(n, off) (ESC_SM_BASE + (uint16_t)((n) * 8u) + (off))
#define ESC_SM_PHYS_ADDR 0u
#define ESC_SM_LENGTH 2u
#define ESC_SM_CONTROL 4u
#define ESC_SM_STATUS 5u
#define ESC_SM_ACTIVATE 6u

#define ESC_SM_OUTPUTS 2u
#define ESC_SM_INPUTS 3u

/* AL Status Code values we can return, from ETG.1000.6 Table 12. */
#define ESC_ALSC_NO_ERROR 0x0000u
#define ESC_ALSC_INVALID_OUTPUT_CFG 0x001du
#define ESC_ALSC_INVALID_INPUT_CFG 0x001eu

/* Bind to the devicetree node and configure the SPI bus.
 * Returns 0 on success, negative errno otherwise. */
int esc_init(void);

/* Read len bytes starting at the ESC register address adr.
 * len may be 1..N; the ESC auto-increments its address. */
int esc_read(uint16_t adr, void *buf, size_t len);

/* Write len bytes starting at the ESC register address adr. */
int esc_write(uint16_t adr, const void *buf, size_t len);

/* Convenience single-byte read. Returns negative errno on failure,
 * otherwise the byte value in 0..255. */
int esc_read8(uint16_t adr);

/* Diagnostic build only: test the NSS wire by watching the ESC react, then
 * leave a live monitor of every SPI line running. NSS and SCK are ESC
 * inputs, so this is the only way to tell a carrying wire from a dead one.
 * See diag.c. */
void esc_diag_run(void);

#endif /* AX58100_H */
