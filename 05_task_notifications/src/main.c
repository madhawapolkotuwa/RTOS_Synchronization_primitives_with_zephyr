/*
 * 05_task_notifications: common part
 * Board: STM32F769I-DISCO, Zephyr RTOS
 *
 * This file owns the hardware: the blue USER button, the red event LED
 * (LD1) and the green heartbeat LED (LD2). It sets them up, debounces the
 * button and forwards presses to the demo, and then runs at the lowest
 * priority, blinking the heartbeat and calling demo_report().
 *
 * The signalling work is in the demo file that prj.conf selects:
 *   Part A  src/dma_driver.c    a driver thread waits for its completion ISR
 *   Part B  src/signal_bench.c  four mechanisms side by side
 *
 * Theory: resources/webpages/07_event_groups_and_notifications.html
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>

#include "demo.h"

#define HEARTBEAT_PERIOD_MS 500

static const struct gpio_dt_spec button        = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static const struct gpio_dt_spec event_led     = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec heartbeat_led = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);

static struct gpio_callback button_cb;

void event_led_toggle(void)
{
	gpio_pin_toggle_dt(&event_led);
}

void event_led_set(bool on)
{
	gpio_pin_set_dt(&event_led, on ? 1 : 0);
}

/* GPIO callback: on the STM32 this runs inside the EXTI interrupt. */
static void button_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);

	if (!(pins & BIT(button.pin))) {
		return;
	}

#if CONFIG_APP_DEBOUNCE_MS > 0
	/*
	 * Contact bounce produces several edges per press. Accept an edge
	 * only if the last accepted one is at least DEBOUNCE_MS old. Only
	 * this ISR uses last_accepted, so no lock is needed.
	 */
	static int64_t last_accepted = -CONFIG_APP_DEBOUNCE_MS;
	int64_t now = k_uptime_get();

	if (now - last_accepted < CONFIG_APP_DEBOUNCE_MS) {
		return;
	}
	last_accepted = now;
#endif

	demo_button_pressed();
}

int main(void)
{
	int ret;

	if (!gpio_is_ready_dt(&button) || !gpio_is_ready_dt(&event_led) ||
	    !gpio_is_ready_dt(&heartbeat_led)) {
		printk("ERROR: GPIO device not ready\n");
		return 0;
	}

	ret = gpio_pin_configure_dt(&event_led, GPIO_OUTPUT_INACTIVE);
	ret |= gpio_pin_configure_dt(&heartbeat_led, GPIO_OUTPUT_INACTIVE);
	ret |= gpio_pin_configure_dt(&button, GPIO_INPUT);
	if (ret < 0) {
		printk("ERROR: could not configure GPIO pins\n");
		return 0;
	}

	ret = gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);
	if (ret < 0) {
		printk("ERROR: could not configure button interrupt (%d)\n", ret);
		return 0;
	}

	gpio_init_callback(&button_cb, button_isr, BIT(button.pin));
	gpio_add_callback(button.port, &button_cb);

	printk("\n05_task_notifications on %s\n", CONFIG_BOARD);
	demo_start();

	/* Heartbeat and periodic report, at the lowest priority. */
	int64_t next_report = k_uptime_get() + CONFIG_APP_REPORT_PERIOD_MS;

	while (1) {
		gpio_pin_toggle_dt(&heartbeat_led);
		k_msleep(HEARTBEAT_PERIOD_MS);

		if (k_uptime_get() >= next_report) {
			next_report += CONFIG_APP_REPORT_PERIOD_MS;
			demo_report();
		}
	}

	return 0;
}
