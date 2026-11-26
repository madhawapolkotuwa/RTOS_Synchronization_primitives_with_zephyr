/*
 * 04_event_groups, Part B: a wait-any dispatcher
 *
 * Three sources post one bit each into the same event group:
 *
 *   bit 0  button  the blue USER button ISR (debounced in main.c)
 *   bit 1  tick    a k_timer, also in interrupt context
 *   bit 2  data    a producer thread
 *
 * One dispatcher thread waits for ANY of them with k_event_wait(). It
 * clears exactly the bits it was given, and then handles them. The
 * dispatcher is the only thread that clears: it owns the bits.
 *
 * Every post and every handling is counted per source. The difference
 * shows what an event group cannot do: count. A bit that is posted
 * again before the dispatcher has cleared it merges with the first post.
 *
 * Kconfig switches turn on the classic mistakes (experiments B2 to B4).
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "demo.h"

enum source { SRC_BUTTON, SRC_TICK, SRC_DATA, NUM_SOURCES };

static const char *const source_name[NUM_SOURCES] = { "button", "tick", "data" };

#define EV_ALL_SOURCES (BIT(SRC_BUTTON) | BIT(SRC_TICK) | BIT(SRC_DATA))

#define STACK_SIZE          1024
#define DISPATCHER_PRIORITY 2
#define PRODUCER_PRIORITY   5

/* The event group: 32 bits, all clear at start. */
K_EVENT_DEFINE(app_events);

static int64_t t0;

static uint32_t now_ms(void)
{
	return (uint32_t)(k_uptime_get() - t0);
}

/* Statistics, written from ISRs and threads: atomic. */
static atomic_t posted[NUM_SOURCES];
static atomic_t handled[NUM_SOURCES];
static atomic_t wakeups;
static atomic_t multi_bit_wakeups;     /* the wait returned more than one bit */
static atomic_t logger_wakeups;

/* ------------------------------------------------------------------ */
/* Producers                                                           */
/* ------------------------------------------------------------------ */

/* Safe from ISRs and threads. */
static void post_source(enum source src)
{
	/* Count first, so that a report never sees handled > posted. */
	atomic_inc(&posted[src]);
	k_event_post(&app_events, BIT(src));    /* OR in; wakes every waiter whose condition is met */
}

void demo_button_pressed(void)
{
	post_source(SRC_BUTTON);
}

#if CONFIG_APP_TICK_PERIOD_MS > 0
static void tick_expiry(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	post_source(SRC_TICK);
}

K_TIMER_DEFINE(tick_timer, tick_expiry, NULL);
#endif

static void producer_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	if (CONFIG_APP_DATA_PERIOD_MS == 0) {
		return;
	}

	while (1) {
		k_msleep(CONFIG_APP_DATA_PERIOD_MS);
		post_source(SRC_DATA);                  /* "a new block of data is ready" */
	}
}

/* ------------------------------------------------------------------ */
/* The dispatcher: the only thread that clears bits                    */
/* ------------------------------------------------------------------ */

static void handle(enum source src)
{
	atomic_inc(&handled[src]);

	if (src == SRC_BUTTON) {
		event_led_toggle();
		printk("[dispatch] t=%u ms  button press handled\n", now_ms());
	}

	k_msleep(CONFIG_APP_HANDLE_MS);                 /* simulated work */
}

static void dispatcher_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		/*
		 * Wait for ANY source. reset = false: never wipe bits before
		 * waiting. The return value says which bits woke us.
		 */
		uint32_t got = k_event_wait(&app_events, EV_ALL_SOURCES, false, K_FOREVER);

		atomic_inc(&wakeups);
		if (__builtin_popcount(got) > 1) {
			atomic_inc(&multi_bit_wakeups);
		}

#if !defined(CONFIG_APP_CLEAR_AFTER_HANDLING)
		/*
		 * Correct: claim exactly the bits we are about to handle, BEFORE
		 * handling them. A post that arrives during the handling sets
		 * the bit again, and the next wait returns at once for it.
		 */
		k_event_clear(&app_events, got);
#endif

		for (int src = 0; src < NUM_SOURCES; src++) {
			if (got & BIT(src)) {
				handle((enum source)src);
			}
		}

#if defined(CONFIG_APP_CLEAR_AFTER_HANDLING)
		/*
		 * WRONG (experiment B2): a post of the same source during the
		 * handling found the bit still set, merged with it, and is
		 * now erased without ever being handled.
		 */
		k_event_clear(&app_events, got);
#endif
	}
}

K_THREAD_DEFINE(dispatcher_tid, STACK_SIZE, dispatcher_thread, NULL, NULL, NULL,
		DISPATCHER_PRIORITY, 0, SYS_FOREVER_MS);
K_THREAD_DEFINE(producer_tid, STACK_SIZE, producer_thread, NULL, NULL, NULL,
		PRODUCER_PRIORITY, 0, SYS_FOREVER_MS);

/* ------------------------------------------------------------------ */
/* A second listener (experiments B3 and B4)                           */
/* ------------------------------------------------------------------ */

#if defined(CONFIG_APP_BUTTON_LOGGER)
/*
 * Also waits for the button bit, but never clears it: the dispatcher owns
 * the clearing. With the default priority (4, less urgent than the
 * dispatcher) the dispatcher has already cleared the bit by the time the
 * logger waits again. With priority 1 (more urgent), the bit is still set
 * when the logger waits again, so the wait returns at once, again and again.
 */
static void logger_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		uint32_t got = k_event_wait(&app_events, BIT(SRC_BUTTON), false, K_FOREVER);

		atomic_inc(&logger_wakeups);
		printk("[logger]   t=%u ms  saw the button too (bits 0x%x)\n", now_ms(), got);
	}
}

K_THREAD_DEFINE(logger_tid, STACK_SIZE, logger_thread, NULL, NULL, NULL,
		CONFIG_APP_LOGGER_PRIORITY, 0, SYS_FOREVER_MS);
#endif

/* ------------------------------------------------------------------ */
/* Called by main.c                                                    */
/* ------------------------------------------------------------------ */

void demo_start(void)
{
	k_thread_name_set(dispatcher_tid, "dispatcher");
	k_thread_name_set(producer_tid, "producer");

	printk("Part B: dispatcher. Waits for ANY of: button tick data. "
	       "Handling takes %d ms per event%s\n",
	       CONFIG_APP_HANDLE_MS,
	       IS_ENABLED(CONFIG_APP_CLEAR_AFTER_HANDLING) ? " (clears AFTER handling)" : "");
	printk("Sources: tick every %d ms, data every %d ms (0 = off), button\n",
	       CONFIG_APP_TICK_PERIOD_MS, CONFIG_APP_DATA_PERIOD_MS);

	t0 = k_uptime_get();

	k_thread_start(dispatcher_tid);
	k_thread_start(producer_tid);

#if defined(CONFIG_APP_BUTTON_LOGGER)
	k_thread_name_set(logger_tid, "logger");
	printk("Logger thread at priority %d also waits for the button bit\n",
	       CONFIG_APP_LOGGER_PRIORITY);
	k_thread_start(logger_tid);
#endif

#if CONFIG_APP_TICK_PERIOD_MS > 0
	k_timer_start(&tick_timer, K_MSEC(CONFIG_APP_TICK_PERIOD_MS),
		      K_MSEC(CONFIG_APP_TICK_PERIOD_MS));
#endif
}

void demo_report(void)
{
	uint32_t p[NUM_SOURCES], h[NUM_SOURCES];
	unsigned int key;

	/* A consistent snapshot: no ISR or thread can run between these reads. */
	key = irq_lock();
	for (int i = 0; i < NUM_SOURCES; i++) {
		p[i] = (uint32_t)atomic_get(&posted[i]);
		h[i] = (uint32_t)atomic_get(&handled[i]);
	}
	irq_unlock(key);

	printk("[report]   t=%u ms  wakeups=%d (with more than one bit: %d)",
	       now_ms(), (int)atomic_get(&wakeups), (int)atomic_get(&multi_bit_wakeups));
	if (IS_ENABLED(CONFIG_APP_BUTTON_LOGGER)) {
		printk("  logger wakeups=%d", (int)atomic_get(&logger_wakeups));
	}
	printk("\n");

	for (int i = 0; i < NUM_SOURCES; i++) {
		printk("[report]     %-6s posted=%u handled=%u not handled=%u\n",
		       source_name[i], p[i], h[i], p[i] - h[i]);
	}
}
