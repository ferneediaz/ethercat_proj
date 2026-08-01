/*
 * AX58100 EtherCAT Slave Controller register access over PDI-SPI.
 *
 * This is the layer SOES will eventually sit on top of. It is kept
 * deliberately free of any SOES dependency so it can be proven on its
 * own first (see MILESTONE 4a in docs/bringup-checklist.md).
 */
#ifndef ESC_H
#define ESC_H

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
#define ESC_REG_AL_CONTROL 0x0130u
#define ESC_REG_AL_STATUS 0x0134u

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

#endif /* ESC_H */
