#include "esc_irq.h"

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "ax58100.h"

LOG_MODULE_REGISTER(esc_irq, LOG_LEVEL_INF);

#define ESC_NODE DT_NODELABEL(esc)

#if DT_NODE_HAS_PROP(ESC_NODE, irq_gpios)

static const struct gpio_dt_spec irq_pin =
	GPIO_DT_SPEC_GET(ESC_NODE, irq_gpios);
static struct gpio_callback irq_cb;

static K_SEM_DEFINE(irq_sem, 0, 1);
static atomic_t irq_count;
static uint32_t current_mask;
static bool armed;

bool esc_irq_present(void)
{
	return true;
}

/*
 * Level-triggered, so the handler must disable the interrupt before returning
 * or it re-enters immediately and forever: the pin stays asserted until the
 * event that raised it is cleared, and clearing it needs an SPI read that
 * cannot happen in interrupt context.
 *
 * Nothing else happens here. Reading 0x0220 means a SPI transaction, and
 * Zephyr's SPI API is not callable from an ISR.
 */
static void on_irq(const struct device *port, struct gpio_callback *cb,
		   gpio_port_pins_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	gpio_pin_interrupt_configure_dt(&irq_pin, GPIO_INT_DISABLE);
	atomic_inc(&irq_count);
	k_sem_give(&irq_sem);
}

struct k_sem *esc_irq_signal(void)
{
	return &irq_sem;
}

/*
 * Is anything actually driving this pin?
 *
 * Same technique sync0.c uses, and for the same reason: a pin that reads a
 * steady level tells you nothing about whether it is connected. Bias it high,
 * then low. A driven pin ignores a ~45k internal pull and reports the driver's
 * level both times; a floating pin follows whatever we pull it to.
 *
 * Worth doing here specifically because SINT is silent by default — the AL
 * Event Mask resets to zero — so "no interrupts" is the expected reading
 * whether the wire is present or not.
 */
static void irq_bias_test(void)
{
	int hi, lo;

	gpio_pin_configure_dt(&irq_pin, GPIO_INPUT | GPIO_PULL_UP);
	k_busy_wait(2000);
	hi = gpio_pin_get_dt(&irq_pin);

	gpio_pin_configure_dt(&irq_pin, GPIO_INPUT | GPIO_PULL_DOWN);
	k_busy_wait(2000);
	lo = gpio_pin_get_dt(&irq_pin);

	gpio_pin_configure_dt(&irq_pin, GPIO_INPUT);

	if (hi == 1 && lo == 0) {
		LOG_WRN("SINT pin follows our own pull-up/pull-down (hi=%d "
			"lo=%d) — nothing is driving it. Check the wire from "
			"module header pin 8 (IRQ) to GPIO %u.",
			hi, lo, irq_pin.pin);
	} else {
		LOG_INF("SINT pin is externally driven (pull-up read %d, "
			"pull-down read %d) — the wire is connected.",
			hi, lo);
	}
}

int esc_irq_init(void)
{
	int rc;

	if (!gpio_is_ready_dt(&irq_pin)) {
		LOG_ERR("SINT GPIO controller is not ready");
		return -ENODEV;
	}

	rc = gpio_pin_configure_dt(&irq_pin, GPIO_INPUT);
	if (rc < 0) {
		LOG_ERR("SINT pin configure failed: %d", rc);
		return rc;
	}

	gpio_init_callback(&irq_cb, on_irq, BIT(irq_pin.pin));
	rc = gpio_add_callback(irq_pin.port, &irq_cb);
	if (rc < 0) {
		LOG_ERR("SINT add_callback failed: %d", rc);
		return rc;
	}

	armed = true;
	irq_bias_test();

	rc = gpio_pin_interrupt_configure_dt(&irq_pin, GPIO_INT_LEVEL_ACTIVE);
	if (rc < 0) {
		LOG_ERR("SINT interrupt configure failed: %d", rc);
		armed = false;
		return rc;
	}

	LOG_INF("SINT armed on GPIO %u, level triggered. Silent until the AL "
		"event mask (0x0204) is written.",
		irq_pin.pin);
	return 0;
}

void esc_irq_set_mask(uint32_t mask)
{
	/* The ESC is little-endian on the wire and so is the ESP32, so the
	 * value goes out as-is. Stated rather than assumed because this is
	 * exactly the kind of thing that works until it is ported. */
	uint32_t le = mask;

	if (esc_write(ESC_REG_AL_EVENT_MASK, &le, sizeof(le)) < 0) {
		LOG_ERR("could not write AL event mask 0x%08x", mask);
		return;
	}
	current_mask = mask;
	LOG_INF("AL event mask 0x%08x written — SINT will now assert on: %s%s%s%s%s",
		mask,
		(mask & ESC_ALEVENT_CONTROL) ? "AL-control " : "",
		(mask & ESC_ALEVENT_SMCHANGE) ? "SM-change " : "",
		(mask & ESC_ALEVENT_SM0) ? "mbx-out " : "",
		(mask & ESC_ALEVENT_SM1) ? "mbx-in " : "",
		(mask & ESC_ALEVENT_SM2) ? "outputs " : "");
}

uint32_t esc_irq_mask(void)
{
	return current_mask;
}

uint32_t esc_irq_count(void)
{
	return (uint32_t)atomic_get(&irq_count);
}

int esc_irq_level(void)
{
	if (!armed) {
		return -1;
	}
	return gpio_pin_get_dt(&irq_pin);
}

void esc_irq_rearm(void)
{
	if (armed) {
		gpio_pin_interrupt_configure_dt(&irq_pin, GPIO_INT_LEVEL_ACTIVE);
	}
}

#else /* no irq-gpios in the devicetree */

struct k_sem *esc_irq_signal(void)
{
	return NULL;
}

bool esc_irq_present(void)
{
	return false;
}

int esc_irq_init(void)
{
	return -ENODEV;
}

void esc_irq_set_mask(uint32_t mask)
{
	ARG_UNUSED(mask);
}

uint32_t esc_irq_mask(void)
{
	return 0;
}

uint32_t esc_irq_count(void)
{
	return 0;
}

int esc_irq_level(void)
{
	return -1;
}

void esc_irq_rearm(void)
{
}

#endif
