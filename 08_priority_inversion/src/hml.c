/*
 * 08_priority_inversion: Lesson 11's H/M/L scenario, on the board
 *
 *   L (prio 4), released at 0: works 1, locks, works 4 inside, unlocks, works 1
 *   H (prio 2), released at 2: works 1, locks, works 2 inside, unlocks, works 1
 *   M (prio 3), released at 3: works 6, never touches the lock
 *
 * Times are in units of CONFIG_APP_UNIT_MS (100 ms). "Works" is CPU time:
 * k_busy_wait() in 1 ms chunks. Between chunks each task notices when it
 * was preempted, and when its priority changed, and writes that into a
 * trace. The scenario repeats every CONFIG_APP_REPORT_PERIOD_MS; after
 * each run, main prints the trace and H's response and blocking times.
 *
 * prj.conf selects the lock:
 *   APP_LOCK_SEM      a binary k_sem: unbounded inversion
 *   APP_LOCK_MUTEX    a k_mutex: priority inheritance, bounded
 *   APP_LOCK_CEILING  a k_mutex, and L and H raise themselves to the
 *                     ceiling (H's priority) for as long as they hold it
 */

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <zephyr/timing/timing.h>

#include "demo.h"

#define STACK_SIZE   1024
#define H_PRIORITY   2
#define M_PRIORITY   3
#define L_PRIORITY   4
#define CEILING      H_PRIORITY      /* the highest priority of any user of the lock */

#define UNIT_MS      CONFIG_APP_UNIT_MS
#define CYCLE_MS     CONFIG_APP_REPORT_PERIOD_MS
#define FIRST_CYCLE  500             /* ms after start: never during a report */

/* ------------------------------------------------------------------ */
/* The lock                                                            */
/* ------------------------------------------------------------------ */

#if defined(CONFIG_APP_LOCK_SEM)
#define LOCK_NAME "k_sem (no owner, so no inheritance)"
K_SEM_DEFINE(lock_sem, 1, 1);
#else
#define LOCK_NAME (IS_ENABLED(CONFIG_APP_LOCK_CEILING) ? \
		   "k_mutex + immediate ceiling (priority " STRINGIFY(CEILING) " while held)" : \
		   "k_mutex (priority inheritance)")
K_MUTEX_DEFINE(lock_mutex);
#endif

/* ------------------------------------------------------------------ */
/* Optional trace pins (scope.overlay)                                 */
/* ------------------------------------------------------------------ */

#define ZEPHYR_USER DT_PATH(zephyr_user)

enum pin { PIN_H_WAITS, PIN_L_HOLDS, PIN_M_ACTIVE };

#if DT_NODE_HAS_PROP(ZEPHYR_USER, trace_gpios)
static const struct gpio_dt_spec trace_pin[] = {
	GPIO_DT_SPEC_GET_BY_IDX(ZEPHYR_USER, trace_gpios, 0),
	GPIO_DT_SPEC_GET_BY_IDX(ZEPHYR_USER, trace_gpios, 1),
	GPIO_DT_SPEC_GET_BY_IDX(ZEPHYR_USER, trace_gpios, 2),
};
#define HAVE_TRACE_PINS 1
#endif

static void pin_set(enum pin p, bool on)
{
#if defined(HAVE_TRACE_PINS)
	gpio_pin_set_dt(&trace_pin[p], on ? 1 : 0);
#else
	ARG_UNUSED(p);
	ARG_UNUSED(on);
#endif
	if (p == PIN_H_WAITS) {
		event_led_set(on);           /* red LED: H is waiting for the lock */
	}
}

/* ------------------------------------------------------------------ */
/* The trace                                                           */
/* ------------------------------------------------------------------ */

enum ev_kind { EV_RELEASED, EV_STARTS, EV_RESUMES, EV_PRIO, EV_WAITS, EV_LOCKS, EV_UNLOCKS,
	       EV_DONE };

struct event {
	uint32_t t_ms;                   /* from the start of the cycle */
	char who;
	int8_t prio;                     /* the task's priority at that moment */
	uint8_t kind;
	uint32_t arg;
};

#define MAX_EVENTS 48

static struct event trace[MAX_EVENTS];
static unsigned int n_events;
static atomic_t tasks_done;
static int64_t cycle_start;          /* ms since boot */
static uint32_t cycle_number;

/* Results of the last cycle, for the summary line. */
static uint32_t h_response_ms, h_blocked_us, h_work_ms;
static uint32_t worst_response_ms, worst_blocked_us, cycles_done;

static void log_event(char who, uint8_t kind, uint32_t arg, int64_t when)
{
	unsigned int key = irq_lock();

	if (n_events < MAX_EVENTS) {
		trace[n_events++] = (struct event){
			.t_ms = (uint32_t)(when - cycle_start),
			.who = who,
			.prio = (int8_t)k_thread_priority_get(k_current_get()),
			.kind = kind,
			.arg = arg,
		};
	}
	irq_unlock(key);
}

/* ------------------------------------------------------------------ */
/* The three tasks                                                     */
/* ------------------------------------------------------------------ */

struct task {
	char name;
	uint32_t release, before, inside, after;   /* units */
	bool uses_lock;
	int last_prio;
	int64_t last_run;                /* when the last 1 ms chunk ended */
	int saved_prio;                  /* ceiling: the priority to go back to */
};

static struct task task_l = { 'L', 0, CONFIG_APP_L_BEFORE, CONFIG_APP_L_INSIDE,
			      CONFIG_APP_L_AFTER, true };
static struct task task_h = { 'H', CONFIG_APP_H_RELEASE, CONFIG_APP_H_BEFORE,
			      CONFIG_APP_H_INSIDE, CONFIG_APP_H_AFTER, true };
static struct task task_m = { 'M', CONFIG_APP_M_RELEASE, CONFIG_APP_M_WORK, 0, 0, false };

static atomic_t extra_m_work;

/* Log a change of this task's priority (inheritance or ceiling). */
static void check_prio(struct task *t)
{
	int prio = k_thread_priority_get(k_current_get());

	if (prio != t->last_prio) {
		t->last_prio = prio;
		log_event(t->name, EV_PRIO, 0, k_uptime_get());
	}
}

/* CPU work, in 1 ms chunks. Notices preemption and priority changes. */
static void work(struct task *t, uint32_t units)
{
	for (uint32_t ms = 0; ms < units * UNIT_MS; ms++) {
		int64_t now = k_uptime_get();

		if (now - t->last_run > 2) {
			log_event(t->name, EV_RESUMES, (uint32_t)(now - t->last_run), now);
		}
		check_prio(t);
		k_busy_wait(1000);
		t->last_run = k_uptime_get();
	}
}

static void take_lock(struct task *t)
{
	timing_t start, end;
	uint32_t blocked_us = 0;

#if defined(CONFIG_APP_LOCK_CEILING)
	/* Immediate ceiling: raise to the ceiling BEFORE locking, every time. */
	t->saved_prio = k_thread_priority_get(k_current_get());
	k_thread_priority_set(k_current_get(), CEILING);
	check_prio(t);
#endif

#if defined(CONFIG_APP_LOCK_SEM)
	if (k_sem_take(&lock_sem, K_NO_WAIT) != 0) {
#else
	if (k_mutex_lock(&lock_mutex, K_NO_WAIT) != 0) {
#endif
		/* Taken by someone else: we will block. */
		log_event(t->name, EV_WAITS, 0, k_uptime_get());
		if (t == &task_h) {
			pin_set(PIN_H_WAITS, true);
		}
		start = timing_counter_get();
#if defined(CONFIG_APP_LOCK_SEM)
		k_sem_take(&lock_sem, K_FOREVER);
#else
		k_mutex_lock(&lock_mutex, K_FOREVER);
#endif
		end = timing_counter_get();
		blocked_us = (uint32_t)(timing_cycles_to_ns(timing_cycles_get(&start, &end)) / 1000U);
		if (t == &task_h) {
			pin_set(PIN_H_WAITS, false);
		}
	}

	t->last_run = k_uptime_get();    /* blocking is not preemption */
	if (t == &task_h) {
		h_blocked_us = blocked_us;
	}
	if (t == &task_l) {
		pin_set(PIN_L_HOLDS, true);
	}
	log_event(t->name, EV_LOCKS, blocked_us / 1000U, k_uptime_get());
}

static void give_lock(struct task *t)
{
	/*
	 * Log first: the unlock can hand the lock to H, which then runs at
	 * once, and this task may not run again for a long time.
	 */
	log_event(t->name, EV_UNLOCKS, 0, k_uptime_get());
	if (t == &task_l) {
		pin_set(PIN_L_HOLDS, false);
	}
#if defined(CONFIG_APP_LOCK_SEM)
	k_sem_give(&lock_sem);
#else
	k_mutex_unlock(&lock_mutex);             /* drops an inherited priority */
#endif
#if defined(CONFIG_APP_LOCK_CEILING)
	k_thread_priority_set(k_current_get(), t->saved_prio);   /* leave the ceiling */
#endif
	/* The next work() notices the new priority, and how long we were away. */
}

static void run_once(struct task *t, int64_t release)
{
	int64_t now = k_uptime_get();
	uint32_t before = t->before;

	if (t == &task_m) {
		before += (uint32_t)atomic_clear(&extra_m_work) * CONFIG_APP_BUTTON_EXTRA;
		pin_set(PIN_M_ACTIVE, true);
	}

	t->last_prio = k_thread_priority_get(k_current_get());
	log_event(t->name, EV_RELEASED, 0, release);
	if (now - release > 2) {
		log_event(t->name, EV_STARTS, (uint32_t)(now - release), now);
	}
	t->last_run = now;

	work(t, before);
	if (t->uses_lock) {
		take_lock(t);
		work(t, t->inside);
		give_lock(t);
		work(t, t->after);
	}

	now = k_uptime_get();
	log_event(t->name, EV_DONE, (uint32_t)(now - release), now);

	if (t == &task_h) {
		h_response_ms = (uint32_t)(now - release);
		h_work_ms = (t->before + t->inside + t->after) * UNIT_MS;
	}
	if (t == &task_m) {
		pin_set(PIN_M_ACTIVE, false);
	}
	atomic_inc(&tasks_done);
}

static int64_t t0;

/* Cycle n starts at t0 + FIRST_CYCLE + n x CYCLE_MS. */
static int64_t cycle_start_for(uint32_t n)
{
	return t0 + FIRST_CYCLE + (int64_t)n * CYCLE_MS;
}

static void task_thread(void *p1, void *p2, void *p3)
{
	struct task *t = p1;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (uint32_t n = 0;; n++) {
		int64_t release = cycle_start_for(n) + t->release * UNIT_MS;

		k_sleep(K_TIMEOUT_ABS_MS(release));
		run_once(t, release);
	}
}

K_THREAD_DEFINE(l_tid, STACK_SIZE, task_thread, &task_l, NULL, NULL,
		L_PRIORITY, 0, SYS_FOREVER_MS);
K_THREAD_DEFINE(m_tid, STACK_SIZE, task_thread, &task_m, NULL, NULL,
		M_PRIORITY, 0, SYS_FOREVER_MS);
K_THREAD_DEFINE(h_tid, STACK_SIZE, task_thread, &task_h, NULL, NULL,
		H_PRIORITY, 0, SYS_FOREVER_MS);

/* ------------------------------------------------------------------ */
/* Called by main.c                                                    */
/* ------------------------------------------------------------------ */

/* M gets more work in the next cycle. ISR context. */
void demo_button_pressed(void)
{
	atomic_set(&extra_m_work, 1);
}

void demo_start(void)
{
#if defined(HAVE_TRACE_PINS)
	for (int i = 0; i < ARRAY_SIZE(trace_pin); i++) {
		gpio_pin_configure_dt(&trace_pin[i], GPIO_OUTPUT_INACTIVE);
	}
#endif
	timing_init();
	timing_start();

	k_thread_name_set(l_tid, "L");
	k_thread_name_set(m_tid, "M");
	k_thread_name_set(h_tid, "H");

	printk("Lock: %s\n", LOCK_NAME);
	printk("Unit %d ms. L (prio %d) at 0: %d, lock %d, %d.  H (prio %d) at %d: %d, lock %d, %d.  "
	       "M (prio %d) at %d: %d\n",
	       UNIT_MS, L_PRIORITY, CONFIG_APP_L_BEFORE, CONFIG_APP_L_INSIDE, CONFIG_APP_L_AFTER,
	       H_PRIORITY, CONFIG_APP_H_RELEASE, CONFIG_APP_H_BEFORE, CONFIG_APP_H_INSIDE,
	       CONFIG_APP_H_AFTER, M_PRIORITY, CONFIG_APP_M_RELEASE, CONFIG_APP_M_WORK);
	printk("Inheritance cap CONFIG_PRIORITY_CEILING = %d\n", CONFIG_PRIORITY_CEILING);

	t0 = k_uptime_get();
	cycle_start = cycle_start_for(0);
	k_thread_start(l_tid);
	k_thread_start(m_tid);
	k_thread_start(h_tid);
}

static const char *what(const struct event *e, char *buf, size_t len)
{
	switch (e->kind) {
	case EV_RELEASED:
		return "released";
	case EV_STARTS:
		snprintk(buf, len, "starts running (was ready, not running, for %u ms)", e->arg);
		return buf;
	case EV_RESUMES:
		snprintk(buf, len, "resumes (was preempted for %u ms)", e->arg);
		return buf;
	case EV_PRIO:
		return "now runs at this priority";
	case EV_WAITS:
		return "waits for the lock";
	case EV_LOCKS:
		if (e->arg == 0) {
			return "locks";
		}
		snprintk(buf, len, "locks, after waiting %u ms", e->arg);
		return buf;
	case EV_UNLOCKS:
		return "unlocks";
	case EV_DONE:
		snprintk(buf, len, "done (response time %u ms)", e->arg);
		return buf;
	}
	return "?";
}

void demo_report(void)
{
	char buf[64];

	if (atomic_get(&tasks_done) < 3) {
		printk("[cycle %u] still running\n", cycle_number + 1);
		return;
	}

	/* Sort by time (insertion sort, stable): events are logged when a task
	 * runs, which can be after the moment they describe.
	 */
	for (unsigned int i = 1; i < n_events; i++) {
		struct event e = trace[i];
		unsigned int j = i;

		while (j > 0 && trace[j - 1].t_ms > e.t_ms) {
			trace[j] = trace[j - 1];
			j--;
		}
		trace[j] = e;
	}

	cycles_done++;
	worst_response_ms = MAX(worst_response_ms, h_response_ms);
	worst_blocked_us = MAX(worst_blocked_us, h_blocked_us);

	printk("[cycle %u] %s\n", cycle_number + 1, LOCK_NAME);
	for (unsigned int i = 0; i < n_events; i++) {
		const struct event *e = &trace[i];

		printk("  %5u ms  %c  prio %d  %s\n", e->t_ms, e->who, e->prio,
		       what(e, buf, sizeof(buf)));
	}
	printk("[cycle %u] H: response %u ms for %u ms of its own work, blocked on the lock "
	       "%u.%03u ms   (worst so far: %u ms, %u.%03u ms)\n",
	       cycle_number + 1, h_response_ms, h_work_ms,
	       h_blocked_us / 1000U, h_blocked_us % 1000U,
	       worst_response_ms, worst_blocked_us / 1000U, worst_blocked_us % 1000U);

	/* Get ready for the next cycle, which starts after this report. */
	unsigned int key = irq_lock();

	n_events = 0;
	h_blocked_us = 0;
	atomic_set(&tasks_done, 0);
	cycle_number++;
	cycle_start = cycle_start_for(cycle_number);
	irq_unlock(key);
}
