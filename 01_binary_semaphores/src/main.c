/*
 * 01_binary_semaphores: ISR-to-thread signalling with a binary semaphore
 * Board: STM32F769I-DISCO, Zephyr RTOS
 *
 * The blue user button raises an interrupt. Its ISR does almost nothing:
 * it counts the event and gives a binary semaphore. A handler thread,
 * blocked on that semaphore, wakes up, toggles LED1 (red) and then spends
 * CONFIG_APP_PROCESSING_MS on the event.
 *
 * Events that arrive while the handler is busy are stored in the
 * semaphore, but a binary semaphore holds at most one. Further gives are
 * lost. The handler prints counters so you can see how many.
 *
 * Optional: CONFIG_APP_TIMER_PERIOD_MS > 0 adds a k_timer that also gives
 * the semaphore from interrupt context, which makes event loss repeatable.
 *
 * Theory: resources/webpages/04_interrupts_and_deferred_processing.html
 *         resources/webpages/05_semaphores.html
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

/* ------------------------------------------------------------------ */
/* Settings                                                            */
/* ------------------------------------------------------------------ */

#define HANDLER_STACK_SIZE   1024
#define HANDLER_PRIORITY     2      /* preemptible, more urgent than main (7) */
#define HEARTBEAT_PERIOD_MS  500

/* ------------------------------------------------------------------ */
/* Kernel objects and hardware                                         */
/* ------------------------------------------------------------------ */

/*
 * The binary semaphore.
 *   initial count 0: no event has happened yet, so the first take blocks.
 *   limit 1:         binary. A give at count 1 has no effect.
 */
K_SEM_DEFINE(event_sem, 0, 1);

static const struct gpio_dt_spec button        = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static const struct gpio_dt_spec event_led     = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec heartbeat_led = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);

static struct gpio_callback button_cb;

/*
 * Counters.
 *   events_raised is written in interrupt context and read by a thread,
 *   so it is an atomic_t (a plain "x++" is a read-modify-write that an
 *   interrupt could split, see Lesson 03).
 *   events_handled is only touched by the handler thread: a plain integer
 *   is enough.
 */
static atomic_t events_raised = ATOMIC_INIT(0);
static uint32_t events_handled;

/* ------------------------------------------------------------------ */
/* Interrupt context: the "top half"                                   */
/* ------------------------------------------------------------------ */

/* Called from every event source. Must stay short and must never block. */
static void signal_event(void)
{
	atomic_inc(&events_raised);
	k_sem_give(&event_sem);         /* ISR-safe: wakes the handler, or count 0 -> 1 */
}

/* GPIO callback: on the STM32 this runs inside the EXTI interrupt. */
static void button_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);

	if (pins & BIT(button.pin)) {
		signal_event();
	}
}

#if CONFIG_APP_TIMER_PERIOD_MS > 0
/* k_timer expiry functions also run in interrupt context. */
static void timer_expiry(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	signal_event();
}

K_TIMER_DEFINE(event_timer, timer_expiry, NULL);
#endif

/* ------------------------------------------------------------------ */
/* Thread context: the "bottom half"                                   */
/* ------------------------------------------------------------------ */

static void print_counters(void)
{
	uint32_t raised;
	uint32_t pending;
	unsigned int key;

	/*
	 * Take a consistent snapshot of "raised" and the semaphore count.
	 * An ISR could otherwise run between the two reads. irq_lock()
	 * masks interrupts for these few instructions only.
	 */
	key = irq_lock();
	raised  = (uint32_t)atomic_get(&events_raised);
	pending = k_sem_count_get(&event_sem);
	irq_unlock(key);

	/* Every raised event was either handled, is still pending, or was lost. */
	uint32_t lost = raised - events_handled - pending;

	printk("[handler] raised=%u handled=%u pending=%u lost=%u\n",
	       raised, events_handled, pending, lost);
}

static void handler_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		/* Blocked here (state: pending), using no CPU, until an ISR gives. */
		int ret = k_sem_take(&event_sem, K_MSEC(CONFIG_APP_TAKE_TIMEOUT_MS));

		if (ret == -EAGAIN) {
			printk("[handler] no event for %d ms (k_sem_take returned -EAGAIN)\n",
			       CONFIG_APP_TAKE_TIMEOUT_MS);
			continue;
		}

		events_handled++;
		gpio_pin_toggle_dt(&event_led);
		print_counters();

		/*
		 * Simulated slow work, e.g. writing a record to flash. The thread
		 * sleeps rather than spins, so lower-priority threads keep running.
		 * Events raised meanwhile collect in the semaphore: one is kept,
		 * the rest are lost.
		 */
		k_msleep(CONFIG_APP_PROCESSING_MS);
	}
}

/* Created at boot. It blocks on the semaphore until main() has set up the button. */
K_THREAD_DEFINE(handler_tid, HANDLER_STACK_SIZE, handler_thread, NULL, NULL, NULL,
		HANDLER_PRIORITY, 0, 0);

/* ------------------------------------------------------------------ */
/* main: hardware setup, then a heartbeat at the lowest priority       */
/* ------------------------------------------------------------------ */

int main(void)
{
	int ret;

	k_thread_name_set(handler_tid, "handler");

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

	printk("\n01_binary_semaphores on %s\n", CONFIG_BOARD);
	printk("Press the blue USER button. Processing time per event: %d ms\n",
	       CONFIG_APP_PROCESSING_MS);

#if CONFIG_APP_TIMER_PERIOD_MS > 0
	k_timer_start(&event_timer, K_MSEC(CONFIG_APP_TIMER_PERIOD_MS),
		      K_MSEC(CONFIG_APP_TIMER_PERIOD_MS));
	printk("Timer event source on: one event every %d ms\n", CONFIG_APP_TIMER_PERIOD_MS);
#endif

	/* Heartbeat: proves the CPU is free while the handler is blocked or sleeping. */
	while (1) {
		gpio_pin_toggle_dt(&heartbeat_led);
		k_msleep(HEARTBEAT_PERIOD_MS);
	}

	return 0;
}
