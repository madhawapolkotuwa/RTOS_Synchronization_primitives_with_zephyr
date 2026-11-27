/*
 * 05_task_notifications, Part B: four receivers, one ISR, the same signals
 *
 * Each receiver thread has its own one-to-one signal, implemented with a
 * different mechanism. A k_timer ISR sends every signal to all four at
 * the same moment, so any difference in what they handle comes from the
 * mechanism alone:
 *
 *   k_sem          counting semaphore: remembers how many
 *   k_event        one bit: remembers that, not how many
 *   k_poll_signal  a flag and an int: remembers that, and the LAST value
 *   k_wakeup()     nothing: works only if the receiver is asleep right then
 *
 * Every round (CONFIG_APP_BENCH_PERIOD_MS) the ISR sends a burst of
 * CONFIG_APP_BENCH_BURST signals back to back, and one more signal
 * CONFIG_APP_BENCH_LATE_MS later, while the receivers are still busy
 * with the first. Each receiver spends CONFIG_APP_BENCH_HANDLE_MS on every
 * wake-up, in k_msleep(). The button sends one extra signal.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "demo.h"

enum receiver { RX_SEM, RX_EVENT, RX_POLL, RX_WAKEUP, NUM_RX };

static const char *const rx_label[NUM_RX] = {
	"k_sem     counts    ",
	"k_event   one bit   ",
	"k_poll    last value",
	"k_wakeup  no memory ",
};

#define STACK_SIZE   1024
#define RX_PRIORITY  3
#define EV_SIGNAL    BIT(0)

/* A wait so long that it never times out in this demo. */
#define WAKEUP_WAIT_MS 60000

/* One signal object per receiver: one-to-one. */
K_SEM_DEFINE(rx_sem, 0, K_SEM_MAX_LIMIT);  /* counting */
K_EVENT_DEFINE(rx_event);
static struct k_poll_signal rx_signal;     /* k_poll_signal_init() in demo_start() */

static atomic_t sent;                      /* signals sent to each receiver */
static atomic_t rounds;
static atomic_t handled[NUM_RX];
static atomic_t overwritten;               /* k_poll: values never seen */
static atomic_t cut_short;                 /* k_wakeup: work sleeps ended early */

static int64_t t0;

static uint32_t now_ms(void)
{
	return (uint32_t)(k_uptime_get() - t0);
}

/* Forward declaration: send_to_all() needs this thread's ID for k_wakeup(). */
static void wakeup_rx(void *p1, void *p2, void *p3);
K_THREAD_DEFINE(wakeup_tid, STACK_SIZE, wakeup_rx, NULL, NULL, NULL,
		RX_PRIORITY, 0, SYS_FOREVER_MS);

/* ------------------------------------------------------------------ */
/* The sender: interrupt context                                       */
/* ------------------------------------------------------------------ */

/* One signal to every receiver. The number n numbers the signals. */
static void send_to_all(void)
{
	int n = (int)atomic_inc(&sent) + 1;     /* atomic_inc returns the old value */

	k_sem_give(&rx_sem);                    /* count++ */
	k_event_post(&rx_event, EV_SIGNAL);     /* bit = 1 */
	k_poll_signal_raise(&rx_signal, n);     /* flag = 1, result = n */
	k_wakeup(wakeup_tid);                   /* end its sleep, if it is asleep */
}

static void late_isr(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	send_to_all();
}

K_TIMER_DEFINE(late_timer, late_isr, NULL);

static void round_isr(struct k_timer *timer)
{
	ARG_UNUSED(timer);

	atomic_inc(&rounds);
	for (int i = 0; i < CONFIG_APP_BENCH_BURST; i++) {
		send_to_all();                  /* no receiver runs in between */
	}
	if (CONFIG_APP_BENCH_LATE_MS > 0) {
		k_timer_start(&late_timer, K_MSEC(CONFIG_APP_BENCH_LATE_MS), K_NO_WAIT);
	}
	event_led_toggle();
}

K_TIMER_DEFINE(round_timer, round_isr, NULL);

void demo_button_pressed(void)
{
	send_to_all();
}

/* ------------------------------------------------------------------ */
/* The receivers                                                       */
/* ------------------------------------------------------------------ */

static void work(void)
{
	k_msleep(CONFIG_APP_BENCH_HANDLE_MS);
}

static void sem_rx(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		k_sem_take(&rx_sem, K_FOREVER);         /* count-- */
		atomic_inc(&handled[RX_SEM]);
		work();
	}
}

static void event_rx(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		uint32_t got = k_event_wait(&rx_event, EV_SIGNAL, false, K_FOREVER);

		k_event_clear(&rx_event, got);          /* clear first, then handle */
		atomic_inc(&handled[RX_EVENT]);
		work();
	}
}

static void poll_rx(void *p1, void *p2, void *p3)
{
	int last_seen = 0;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		struct k_poll_event ev = K_POLL_EVENT_INITIALIZER(K_POLL_TYPE_SIGNAL,
								  K_POLL_MODE_NOTIFY_ONLY,
								  &rx_signal);
		unsigned int signaled;
		int n;
		unsigned int key;

		k_poll(&ev, 1, K_FOREVER);

		key = irq_lock();                       /* read and reset in one step */
		k_poll_signal_check(&rx_signal, &signaled, &n);
		k_poll_signal_reset(&rx_signal);
		irq_unlock(key);

		atomic_inc(&handled[RX_POLL]);
		if (n > last_seen + 1) {
			atomic_add(&overwritten, n - last_seen - 1);
		}
		last_seen = n;
		work();
	}
}

static void wakeup_rx(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		/* Waiting = sleeping. A k_wakeup() ends the sleep early. */
		if (k_msleep(WAKEUP_WAIT_MS) == 0) {
			continue;                       /* slept to the end: no signal */
		}
		atomic_inc(&handled[RX_WAKEUP]);

		/* The work is a sleep too, and a k_wakeup() ends it just the same. */
		if (k_msleep(CONFIG_APP_BENCH_HANDLE_MS) > 0) {
			atomic_inc(&cut_short);
		}
	}
}

K_THREAD_DEFINE(sem_tid, STACK_SIZE, sem_rx, NULL, NULL, NULL,
		RX_PRIORITY, 0, SYS_FOREVER_MS);
K_THREAD_DEFINE(event_tid, STACK_SIZE, event_rx, NULL, NULL, NULL,
		RX_PRIORITY, 0, SYS_FOREVER_MS);
K_THREAD_DEFINE(poll_tid, STACK_SIZE, poll_rx, NULL, NULL, NULL,
		RX_PRIORITY, 0, SYS_FOREVER_MS);

/* ------------------------------------------------------------------ */
/* Called by main.c                                                    */
/* ------------------------------------------------------------------ */

void demo_start(void)
{
	k_poll_signal_init(&rx_signal);

	k_thread_name_set(sem_tid, "rx_sem");
	k_thread_name_set(event_tid, "rx_event");
	k_thread_name_set(poll_tid, "rx_poll");
	k_thread_name_set(wakeup_tid, "rx_wakeup");

	printk("Part B: one ISR signals four receivers, one mechanism each\n");
	printk("  every %d ms: a burst of %d, then 1 more after %d ms (0 = none); "
	       "%d ms of work per wake-up\n",
	       CONFIG_APP_BENCH_PERIOD_MS, CONFIG_APP_BENCH_BURST, CONFIG_APP_BENCH_LATE_MS,
	       CONFIG_APP_BENCH_HANDLE_MS);
	printk("  bytes per object: k_sem %u, k_event %u, k_poll_signal %u "
	       "(+ k_poll_event %u on the waiter's stack), k_wakeup 0\n",
	       (unsigned int)sizeof(struct k_sem), (unsigned int)sizeof(struct k_event),
	       (unsigned int)sizeof(struct k_poll_signal),
	       (unsigned int)sizeof(struct k_poll_event));

	t0 = k_uptime_get();
	k_thread_start(sem_tid);
	k_thread_start(event_tid);
	k_thread_start(poll_tid);
	k_thread_start(wakeup_tid);

	/* First round half a period in, so that a report never cuts a round. */
	k_timer_start(&round_timer, K_MSEC(CONFIG_APP_BENCH_PERIOD_MS / 2),
		      K_MSEC(CONFIG_APP_BENCH_PERIOD_MS));
}

void demo_report(void)
{
	uint32_t s, r, h[NUM_RX], ow, cs;
	unsigned int key;

	/* A consistent snapshot: no ISR or thread can run between these reads. */
	key = irq_lock();
	s = (uint32_t)atomic_get(&sent);
	r = (uint32_t)atomic_get(&rounds);
	for (int i = 0; i < NUM_RX; i++) {
		h[i] = (uint32_t)atomic_get(&handled[i]);
	}
	ow = (uint32_t)atomic_get(&overwritten);
	cs = (uint32_t)atomic_get(&cut_short);
	irq_unlock(key);

	printk("[report] t=%u ms  rounds=%u  signals sent to each receiver=%u\n",
	       now_ms(), r, s);
	for (int i = 0; i < NUM_RX; i++) {
		printk("[report]   %s  handled=%-4u not handled=%-4u", rx_label[i], h[i], s - h[i]);
		if (i == RX_POLL) {
			printk("  values overwritten=%u", ow);
		} else if (i == RX_WAKEUP) {
			printk("  work sleeps cut short=%u", cs);
		}
		printk("\n");
	}
}
