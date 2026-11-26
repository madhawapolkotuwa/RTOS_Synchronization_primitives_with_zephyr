/*
 * 04_event_groups, Part A: a wait-all start-up gate
 *
 * A worker may only transfer data when three independent conditions hold:
 *
 *   bit 0  EV_SENSOR_READY   posted once by the sensor init thread
 *   bit 1  EV_CONFIG_LOADED  posted once by the config init thread
 *   bit 2  EV_LINK_UP        posted and cleared by the link thread as the
 *                            link goes up and down (and by the button)
 *
 * The bits are STATE, not events: a bit stays set for as long as its
 * condition holds, and whoever owns the condition clears it. Nobody else
 * clears anything. The worker waits for all three with k_event_wait_all().
 * While they hold, the wait returns at once, so the worker keeps
 * transferring. When the link drops, the next wait blocks: the worker
 * pauses by itself, with no extra code.
 *
 * Kconfig switches turn on the classic mistakes (experiments A2 to A4).
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "demo.h"

#define EV_SENSOR_READY   BIT(0)
#define EV_CONFIG_LOADED  BIT(1)
#define EV_LINK_UP        BIT(2)
#define EV_ALL_READY      (EV_SENSOR_READY | EV_CONFIG_LOADED | EV_LINK_UP)

#define STACK_SIZE       1024
#define WORKER_PRIORITY  3
#define INIT_PRIORITY    5

/* Experiment A3. The correct value here is false. */
#define WAIT_RESET IS_ENABLED(CONFIG_APP_GATE_RESET)

/* The event group: 32 bits, all clear at start. */
K_EVENT_DEFINE(sys_events);

static int64_t t0;

static uint32_t now_ms(void)
{
	return (uint32_t)(k_uptime_get() - t0);
}

/* " sensor config link" for the bits that are set, " (none)" for none. */
static const char *bits_str(uint32_t bits, char *buf, size_t len)
{
	snprintk(buf, len, "%s%s%s%s",
		 (bits & EV_SENSOR_READY)  ? " sensor" : "",
		 (bits & EV_CONFIG_LOADED) ? " config" : "",
		 (bits & EV_LINK_UP)       ? " link"   : "",
		 (bits & EV_ALL_READY)     ? ""        : " (none)");
	return buf;
}

/*
 * Statistics. Each counter is written by one thread (or the button ISR,
 * atomically) and only read by main(). Aligned 32-bit loads and stores
 * are single instructions on the Cortex-M7.
 */
static uint32_t gate_openings;
static uint32_t transfers;
static uint32_t early_starts;     /* transfers started while a condition was missing */
static uint32_t wait_timeouts;
static atomic_t button_toggles;

/* ------------------------------------------------------------------ */
/* The condition owners                                                */
/* ------------------------------------------------------------------ */

struct init_task {
	const char *name;
	uint32_t bit;
	int delay_ms;
};

static const struct init_task init_tasks[] = {
	{ "sensor", EV_SENSOR_READY,  CONFIG_APP_SENSOR_INIT_MS },
	{ "config", EV_CONFIG_LOADED, CONFIG_APP_CONFIG_INIT_MS },
};

/* Simulates a slow initialisation, then announces it. Runs once. */
static void init_thread(void *p1, void *p2, void *p3)
{
	const struct init_task *task = &init_tasks[POINTER_TO_INT(p1)];

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	k_msleep(task->delay_ms);

	printk("[init]   t=%u ms  %s ready (posted once, never again)\n", now_ms(), task->name);
	k_event_post(&sys_events, task->bit);      /* OR the bit in; keep the others */
}

static void link_set(bool up)
{
	if (up) {
#if defined(CONFIG_APP_LINK_USE_SET)
		/* WRONG (experiment A4): replaces all 32 bits with EV_LINK_UP. */
		k_event_set(&sys_events, EV_LINK_UP);
#else
		k_event_post(&sys_events, EV_LINK_UP);
#endif
	} else {
		k_event_clear(&sys_events, EV_LINK_UP); /* clears only this bit */
	}
}

/* The link comes up, stays up for a while, drops, and comes back. */
static void link_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	k_msleep(CONFIG_APP_LINK_INIT_MS);

	while (1) {
		printk("[link]   t=%u ms  up\n", now_ms());
		link_set(true);

		if (CONFIG_APP_LINK_UP_TIME_MS == 0) {
			return;                         /* stays up for ever */
		}
		k_msleep(CONFIG_APP_LINK_UP_TIME_MS);

		link_set(false);
		printk("[link]   t=%u ms  down\n", now_ms());
		k_msleep(CONFIG_APP_LINK_DOWN_TIME_MS);
	}
}

/* The button "unplugs" or "replugs" the cable. ISR context: all ISR-safe. */
void demo_button_pressed(void)
{
	atomic_inc(&button_toggles);

	if (k_event_test(&sys_events, EV_LINK_UP)) {
		link_set(false);
	} else {
		link_set(true);
	}
}

/* ------------------------------------------------------------------ */
/* The worker                                                          */
/* ------------------------------------------------------------------ */

static void worker_thread(void *p1, void *p2, void *p3)
{
	char buf[32];
	bool open = false;
	uint32_t last_reported_bad = 0;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		/*
		 * Block until ALL three bits are set. Returns the matching
		 * bits, or 0 on timeout. The bits are NOT cleared: they are
		 * conditions owned by other threads.
		 */
#if defined(CONFIG_APP_GATE_WAIT_ANY)
		/* WRONG (experiment A2): returns as soon as ANY bit is set. */
		uint32_t got = k_event_wait(&sys_events, EV_ALL_READY, WAIT_RESET,
					    K_MSEC(CONFIG_APP_GATE_TIMEOUT_MS));
#else
		uint32_t got = k_event_wait_all(&sys_events, EV_ALL_READY, WAIT_RESET,
						K_MSEC(CONFIG_APP_GATE_TIMEOUT_MS));
#endif

		if (got == 0) {
			wait_timeouts++;
			uint32_t missing = EV_ALL_READY & ~k_event_test(&sys_events, EV_ALL_READY);

			printk("[worker] t=%u ms  still waiting, missing:%s\n", now_ms(),
			       bits_str(missing, buf, sizeof(buf)));
			continue;
		}

		/* What holds right now? With wait-all this is always all three. */
		uint32_t now = k_event_test(&sys_events, EV_ALL_READY);

		if (now == EV_ALL_READY) {
			if (!open) {
				open = true;
				last_reported_bad = 0;
				gate_openings++;
				event_led_set(true);
				printk("[worker] t=%u ms  gate open, the wait returned:%s\n",
				       now_ms(), bits_str(got, buf, sizeof(buf)));
			}
		} else {
			/* Only reachable with the wrong wait (experiment A2). */
			early_starts++;
			if (now != last_reported_bad) {
				last_reported_bad = now;
				printk("[worker] t=%u ms  BUG: transfer started, missing:%s\n",
				       now_ms(), bits_str(EV_ALL_READY & ~now, buf, sizeof(buf)));
			}
		}

		/* The work. Conditions can change while it runs. */
		k_msleep(CONFIG_APP_TRANSFER_MS);
		transfers++;

		now = k_event_test(&sys_events, EV_ALL_READY);
		if (open && now != EV_ALL_READY) {
			open = false;
			event_led_set(false);
			printk("[worker] t=%u ms  gate closed, missing:%s\n", now_ms(),
			       bits_str(EV_ALL_READY & ~now, buf, sizeof(buf)));
		}
	}
}

/* Created at boot but not started: demo_start() starts them, so that all
 * times are measured from the same moment t0.
 */
K_THREAD_DEFINE(worker_tid, STACK_SIZE, worker_thread, NULL, NULL, NULL,
		WORKER_PRIORITY, 0, SYS_FOREVER_MS);
K_THREAD_DEFINE(sensor_tid, STACK_SIZE, init_thread, INT_TO_POINTER(0), NULL, NULL,
		INIT_PRIORITY, 0, SYS_FOREVER_MS);
K_THREAD_DEFINE(config_tid, STACK_SIZE, init_thread, INT_TO_POINTER(1), NULL, NULL,
		INIT_PRIORITY, 0, SYS_FOREVER_MS);
K_THREAD_DEFINE(link_tid, STACK_SIZE, link_thread, NULL, NULL, NULL,
		INIT_PRIORITY, 0, SYS_FOREVER_MS);

/* ------------------------------------------------------------------ */
/* Called by main.c                                                    */
/* ------------------------------------------------------------------ */

void demo_start(void)
{
	char buf[32];

	k_thread_name_set(worker_tid, "worker");
	k_thread_name_set(sensor_tid, "sensor_init");
	k_thread_name_set(config_tid, "config_init");
	k_thread_name_set(link_tid, "link");

	printk("Part A: start-up gate. The worker waits for %s of:%s%s%s\n",
	       IS_ENABLED(CONFIG_APP_GATE_WAIT_ANY) ? "ANY" : "ALL",
	       bits_str(EV_ALL_READY, buf, sizeof(buf)),
	       IS_ENABLED(CONFIG_APP_GATE_RESET) ? "  (reset = true)" : "",
	       IS_ENABLED(CONFIG_APP_LINK_USE_SET) ? "  (link uses k_event_set)" : "");

	t0 = k_uptime_get();

	/* The worker first (priority 3 runs at once and blocks in its wait). */
	k_thread_start(worker_tid);
	k_thread_start(sensor_tid);
	k_thread_start(config_tid);
	k_thread_start(link_tid);
}

void demo_report(void)
{
	char buf[32];

	printk("[gate]   t=%u ms  bits now:%s  openings=%u transfers=%u early=%u "
	       "timeouts=%u button=%d\n",
	       now_ms(), bits_str(k_event_test(&sys_events, EV_ALL_READY), buf, sizeof(buf)),
	       gate_openings, transfers, early_starts, wait_timeouts,
	       (int)atomic_get(&button_toggles));
}
