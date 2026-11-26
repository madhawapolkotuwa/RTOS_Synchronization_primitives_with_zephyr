/*
 * 02_counting_semaphores, Part A: count bursty events without losing any
 *
 * A k_timer produces bursts of events from interrupt context: BURST_SIZE
 * events, BURST_GAP_MS apart, every BURST_PERIOD_MS. The blue button adds
 * one event per press (and one per contact bounce). Each event gives a
 * counting semaphore. A handler thread takes it once per event and then
 * spends PROCESSING_MS on that event.
 *
 * During a burst events arrive faster than the handler can process them.
 * The semaphore stores the backlog, up to its limit. Between bursts the
 * handler catches up. If the limit is at least the largest backlog, no
 * event is lost.
 *
 * The ISR counts lost events exactly: an event is lost when the count is
 * already at the limit at the moment of the give.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "demo.h"

#define HANDLER_STACK_SIZE 1024
#define HANDLER_PRIORITY   2

#define SEM_LIMIT     CONFIG_APP_EVENT_SEM_LIMIT
#define BURST_SIZE    CONFIG_APP_BURST_SIZE
#define BURST_GAP_MS  CONFIG_APP_BURST_GAP_MS
#define BURST_PERIOD  CONFIG_APP_BURST_PERIOD_MS

#if BURST_PERIOD > 0
BUILD_ASSERT(BURST_PERIOD % BURST_GAP_MS == 0,
	     "CONFIG_APP_BURST_PERIOD_MS must be a multiple of CONFIG_APP_BURST_GAP_MS");
BUILD_ASSERT(BURST_PERIOD / BURST_GAP_MS >= BURST_SIZE,
	     "a burst must fit inside one CONFIG_APP_BURST_PERIOD_MS");
#endif

/*
 * The counting semaphore.
 *   initial count 0:      no event has happened yet.
 *   limit SEM_LIMIT > 1:  up to SEM_LIMIT events can wait to be handled.
 */
K_SEM_DEFINE(event_sem, 0, SEM_LIMIT);

/*
 * Counters written in interrupt context. They are only read or written
 * with interrupts masked (irq_lock), so plain integers are enough: the
 * lock makes "read the count, give, update the counters" one indivisible
 * step, even if the button ISR and the timer ISR preempt each other.
 */
static uint32_t events_raised;
static uint32_t events_lost;        /* given while the count was at the limit */
static uint32_t events_discarded;   /* thrown away by k_sem_reset() */
static uint32_t peak_pending;       /* highest count ever reached: the backlog */

/* Only the handler thread touches this one. */
static uint32_t events_handled;

/* ------------------------------------------------------------------ */
/* Interrupt context                                                   */
/* ------------------------------------------------------------------ */

static void signal_event(void)
{
	unsigned int key = irq_lock();

	events_raised++;
	if (k_sem_count_get(&event_sem) == SEM_LIMIT) {
		events_lost++;          /* the give below will have no effect */
	}

	k_sem_give(&event_sem);     /* ISR-safe; saturates at SEM_LIMIT */

	uint32_t count = k_sem_count_get(&event_sem);

	if (count > peak_pending) {
		peak_pending = count;
	}

	irq_unlock(key);
}

void demo_button_pressed(void)
{
	signal_event();
}

#if BURST_PERIOD > 0
/*
 * Runs every BURST_GAP_MS. The first BURST_SIZE expiries of every
 * BURST_PERIOD produce an event; the rest are silent.
 */
static void burst_timer_expiry(struct k_timer *timer)
{
	static uint32_t tick;   /* only this ISR uses it */

	ARG_UNUSED(timer);

	if (tick < BURST_SIZE) {
		signal_event();
	}
	tick = (tick + 1) % (BURST_PERIOD / BURST_GAP_MS);
}

K_TIMER_DEFINE(burst_timer, burst_timer_expiry, NULL);
#endif

/* ------------------------------------------------------------------ */
/* Thread context                                                      */
/* ------------------------------------------------------------------ */

static void handle_one_event(void)
{
	uint32_t raised, lost, discarded, peak, pending;
	unsigned int key;

	events_handled++;
	event_led_toggle();

	/* A consistent snapshot: no ISR can run between these reads. */
	key = irq_lock();
	raised    = events_raised;
	lost      = events_lost;
	discarded = events_discarded;
	peak      = peak_pending;
	pending   = k_sem_count_get(&event_sem);
	irq_unlock(key);

	/* Every raised event is in exactly one of these four places. */
	bool balanced = (raised == events_handled + pending + lost + discarded);

	printk("[handler] raised=%u handled=%u pending=%u lost=%u discarded=%u peak=%u%s\n",
	       raised, events_handled, pending, lost, discarded, peak,
	       balanced ? "" : "  <-- counters do not add up");

#if CONFIG_APP_FLUSH_THRESHOLD > 0
	/*
	 * Drop policy: when the backlog gets this large, old events are
	 * stale. k_sem_reset() sets the count to 0 in one call. It does not
	 * return the old count, so read it first, with interrupts masked so
	 * that no give can slip in between the read and the reset.
	 */
	if (pending >= CONFIG_APP_FLUSH_THRESHOLD) {
		uint32_t dropped;

		key = irq_lock();
		dropped = k_sem_count_get(&event_sem);
		k_sem_reset(&event_sem);
		events_discarded += dropped;
		irq_unlock(key);

		printk("[handler] backlog reached %d: k_sem_reset() discarded %u events\n",
		       CONFIG_APP_FLUSH_THRESHOLD, dropped);
	}
#endif
}

static void handler_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		/* One take per event. Returns at once while events are pending. */
		k_sem_take(&event_sem, K_FOREVER);

		handle_one_event();

		/* Simulated work. Events raised meanwhile wait in the semaphore. */
		k_msleep(CONFIG_APP_PROCESSING_MS);
	}
}

K_THREAD_DEFINE(handler_tid, HANDLER_STACK_SIZE, handler_thread, NULL, NULL, NULL,
		HANDLER_PRIORITY, 0, 0);

/* ------------------------------------------------------------------ */
/* Called by main.c                                                    */
/* ------------------------------------------------------------------ */

void demo_start(void)
{
	k_thread_name_set(handler_tid, "handler");

	printk("Part A: counting events. Semaphore limit %d, processing %d ms per event\n",
	       SEM_LIMIT, CONFIG_APP_PROCESSING_MS);

#if BURST_PERIOD > 0
	printk("Bursts: %d events, %d ms apart, every %d ms\n",
	       BURST_SIZE, BURST_GAP_MS, BURST_PERIOD);
	k_timer_start(&burst_timer, K_MSEC(BURST_GAP_MS), K_MSEC(BURST_GAP_MS));
#else
	printk("Burst timer off. Press the blue USER button.\n");
#endif
}

void demo_report(void)
{
	/* Part A prints one line per handled event instead. */
}
