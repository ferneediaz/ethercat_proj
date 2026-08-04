/*
 * The AX58100's SINT interrupt line.
 *
 * SINT (chip pin 1, module header pin 8 silkscreened IRQ, ESP32 GPIO 16) is
 * the ESC telling the host "an AL event needs servicing" — a SyncManager was
 * written, the master requested a state change, the mailbox has traffic.
 *
 * It answers a different question from SYNC0, and the distinction is the whole
 * design:
 *
 *   SINT   something CHANGED. Asynchronous, arrives when the frame arrives.
 *   SYNC0  it is TIME to act. Fixed schedule, agreed across the whole bus.
 *
 * Waking the actuator on SINT would mean acting whenever the frame happened to
 * land, inheriting the master's scheduling jitter. Waking on SYNC0 alone means
 * never being told what changed without asking. Real DC-synchronous slaves use
 * both — SOES's own reference HAL does exactly this — and so does this port.
 *
 * Which events assert the pin is controlled by the AL Event Mask (0x0204),
 * which resets to zero: SINT is silent until something unmasks it. That is why
 * this pin looked dead for most of the project.
 */
#ifndef ESC_IRQ_H
#define ESC_IRQ_H

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>

/*
 * The semaphore the SINT handler signals, for k_poll alongside SYNC0.
 *
 * Exposed rather than wrapped in a blocking wait because the slave loop has
 * to wait on BOTH this and SYNC0 at once — they mean different things and
 * either may come first. NULL when no pin is configured.
 */
struct k_sem *esc_irq_signal(void);

/* True when the devicetree gives us an irq-gpios pin to work with. */
bool esc_irq_present(void);

/*
 * Claim the pin and arm the interrupt. Does NOT unmask anything in the ESC —
 * that is esc_irq_set_mask()'s job, and SOES drives it through the
 * esc_hw_interrupt_enable hook when the slave reaches OP.
 *
 * Returns 0 on success, negative errno otherwise.
 */
int esc_irq_init(void);

/*
 * Write the ESC's AL Event Mask (0x0204). Bits set here are the events allowed
 * to assert SINT; everything else still shows up in AL Event Request (0x0220)
 * when polled, it just does not raise the line.
 */
void esc_irq_set_mask(uint32_t mask);

/* Current mask, as last written. */
uint32_t esc_irq_mask(void);

/* Total SINT assertions seen since boot. The honest way to tell a wire that
 * works from one that does not — the same role sync0_edges() plays. */
uint32_t esc_irq_count(void);

/* Raw level of the pin: 1 = asserted (logically active), 0 = idle, -1 when
 * no pin is configured. */
int esc_irq_level(void);

/*
 * Re-arm after servicing.
 *
 * SINT is LEVEL triggered on this module (EEPROM byte 0x0A bit 4 clear), so it
 * stays asserted until the AL event that raised it is cleared by reading
 * 0x0220. A level interrupt left enabled would therefore re-fire continuously
 * and starve everything else, so the ISR disables it and the servicing thread
 * calls this once the read has happened.
 */
void esc_irq_rearm(void);

#endif /* ESC_IRQ_H */
