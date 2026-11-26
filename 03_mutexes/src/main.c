/*
 * 03_mutexes: protecting a shared bus with a mutex
 * Board: STM32F769I-DISCO, Zephyr RTOS
 *
 * Two sensor threads of different priority read devices on one simulated
 * I2C bus (bus.c, sensors.c). prj.conf chooses what protects the bus: a
 * k_mutex, a binary semaphore, or nothing. The report shows how many
 * reads were torn by interleaving, and how many timed out on the lock.
 *
 * At startup, an ownership tour (ownership_tour.c) shows the rules of
 * mutex ownership as return codes.
 *
 * main() sets up the LEDs and the button, runs the tour, starts the
 * sensors, and then blinks the heartbeat and prints the report at the
 * lowest priority.
 *
 *   green LED LD2  heartbeat
 *   red LED LD1    toggles on every bad (torn) read
 *   USER button    prints a report immediately
 *
 * Theory: resources/webpages/06_mutexes.html
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#include "app.h"

#define HEARTBEAT_PERIOD_MS 500

static const struct gpio_dt_spec button        = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static const struct gpio_dt_spec error_led     = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec heartbeat_led = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);

static struct gpio_callback button_cb;
static atomic_t report_requested;

void error_led_toggle(void)
{
	gpio_pin_toggle_dt(&error_led);
}

/* GPIO callback: interrupt context. It may not use a mutex (experiment 5). */
static void button_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);

	if (pins & BIT(button.pin)) {
		atomic_set(&report_requested, 1);
	}
}

int main(void)
{
	int ret;

	if (!gpio_is_ready_dt(&button) || !gpio_is_ready_dt(&error_led) ||
	    !gpio_is_ready_dt(&heartbeat_led)) {
		printk("ERROR: GPIO device not ready\n");
		return 0;
	}

	ret = gpio_pin_configure_dt(&error_led, GPIO_OUTPUT_INACTIVE);
	ret |= gpio_pin_configure_dt(&heartbeat_led, GPIO_OUTPUT_INACTIVE);
	ret |= gpio_pin_configure_dt(&button, GPIO_INPUT);
	ret |= gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);
	if (ret < 0) {
		printk("ERROR: could not configure GPIO pins\n");
		return 0;
	}

	gpio_init_callback(&button_cb, button_isr, BIT(button.pin));
	gpio_add_callback(button.port, &button_cb);

	printk("\n03_mutexes on %s\n", CONFIG_BOARD);

	ownership_tour();
	sensors_start();

	int64_t next_report = k_uptime_get() + CONFIG_APP_REPORT_PERIOD_MS;

	while (1) {
		gpio_pin_toggle_dt(&heartbeat_led);
		k_msleep(HEARTBEAT_PERIOD_MS);

		if (atomic_set(&report_requested, 0) || k_uptime_get() >= next_report) {
			next_report = k_uptime_get() + CONFIG_APP_REPORT_PERIOD_MS;
			sensors_report();
		}
	}

	return 0;
}
