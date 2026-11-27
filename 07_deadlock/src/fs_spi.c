/*
 * 07_deadlock: the file system and the SPI bus (Lesson 10, worked example)
 *
 * An SPI bus is shared by an external flash, which holds a file system,
 * and an LCD. Two mutexes protect them:
 *
 *   fs_lock   level 1   the file system
 *   spi_lock  level 2   the SPI bus
 *
 *   logger  (prio 5): lock FS, open the file, lock SPI, write, unlock both
 *   display (prio 4): lock SPI, stream half a frame, a glyph is missing:
 *                     lock FS, read it, unlock FS, stream the rest, unlock SPI
 *
 * The logger takes FS then SPI, the display SPI then FS. If each gets its
 * first lock before the other asks for it, both wait for ever. With the
 * default timing, the scheduled writes never overlap a frame, so the
 * deadlock never happens by itself. The button asks the logger for an
 * extra write at a random moment, and about one press in seven hits the
 * window.
 *
 * Diagnosis: the report prints who holds and who waits for each lock,
 * the kernel shell lists the thread states, and the task watchdog reports
 * a task that stopped finishing its cycles. prj.conf selects one of three
 * fixes (Part B).
 */

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <zephyr/task_wdt/task_wdt.h>

#if defined(CONFIG_APP_WDT_REBOOT)
#include <zephyr/sys/reboot.h>
#endif

#include "demo.h"

#define STACK_SIZE           1024
#define GATEKEEPER_PRIORITY  3
#define DISPLAY_PRIORITY     4
#define LOGGER_PRIORITY      5

static int64_t t0;

static uint32_t now_ms(void)
{
	return (uint32_t)(k_uptime_get() - t0);
}

/* ------------------------------------------------------------------ */
/* Locks that remember who holds them and who waits for them           */
/* ------------------------------------------------------------------ */

struct worker;

struct app_lock {
	struct k_mutex mutex;
	const char *name;
	uint8_t level;               /* lock order: take lower levels first */
	struct worker *owner;        /* bookkeeping for the report */
	uint32_t max_wait_us;
};

struct worker {
	const char *name;
	uint32_t held_levels;        /* BIT(level) for each lock held */
	struct app_lock *waiting_for;
	int wdt_channel;
	uint32_t cycles;             /* frames or log records completed */
	struct k_sem spi_done;       /* fix B3: the gatekeeper's reply */
};

static struct app_lock fs_lock  = { .name = "fs_lock",  .level = 1 };
static struct app_lock spi_lock = { .name = "spi_lock", .level = 2 };

#if defined(CONFIG_APP_CHECK_LOCK_ORDER)
static struct app_lock *const lock_by_level[] = { NULL, &fs_lock, &spi_lock };
#endif

static struct worker display = { .name = "display" };
static struct worker logger  = { .name = "logger" };

static int lock(struct worker *w, struct app_lock *l, k_timeout_t timeout)
{
#if defined(CONFIG_APP_CHECK_LOCK_ORDER)
	/* No lock of the same or a higher level may be held already. */
	__ASSERT((w->held_levels >> l->level) == 0,
		 "lock order: %s takes %s while holding %s", w->name, l->name,
		 lock_by_level[31 - __builtin_clz(w->held_levels)]->name);
#endif
	int64_t start = k_uptime_ticks();

	w->waiting_for = l;
	int ret = k_mutex_lock(&l->mutex, timeout);

	w->waiting_for = NULL;

	uint32_t waited = k_ticks_to_us_floor32((uint32_t)(k_uptime_ticks() - start));

	l->max_wait_us = MAX(l->max_wait_us, waited);
	if (ret == 0) {
		l->owner = w;
		w->held_levels |= BIT(l->level);
	}
	return ret;
}

static void unlock(struct worker *w, struct app_lock *l)
{
	l->owner = NULL;
	w->held_levels &= ~BIT(l->level);
	k_mutex_unlock(&l->mutex);
}

/* ------------------------------------------------------------------ */
/* The SPI bus                                                         */
/* ------------------------------------------------------------------ */

#if defined(CONFIG_APP_FIX_GATEKEEPER)
/*
 * Fix B3: one thread owns the bus. Everyone else sends it requests and
 * waits for the reply. Nobody locks the bus, so it cannot be part of a
 * cycle of locks.
 */
struct spi_request {
	uint32_t duration_ms;
	struct k_sem *done;
};

K_MSGQ_DEFINE(spi_requests, sizeof(struct spi_request), 4, 4);

static uint32_t gk_served, gk_max_queued;

static void gatekeeper_thread(void *p1, void *p2, void *p3)
{
	struct spi_request req;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		k_msgq_get(&spi_requests, &req, K_FOREVER);
		k_msleep(req.duration_ms);              /* the SPI transaction */
		gk_served++;
		k_sem_give(req.done);
	}
}

K_THREAD_DEFINE(gatekeeper_tid, STACK_SIZE, gatekeeper_thread, NULL, NULL, NULL,
		GATEKEEPER_PRIORITY, 0, SYS_FOREVER_MS);

static void spi_transaction(struct worker *w, int ms)
{
	struct spi_request req = { .duration_ms = ms, .done = &w->spi_done };

	k_msgq_put(&spi_requests, &req, K_FOREVER);
	gk_max_queued = MAX(gk_max_queued, k_msgq_num_used_get(&spi_requests));
	k_sem_take(&w->spi_done, K_FOREVER);
}
#else
/* The caller holds spi_lock. */
static void spi_transaction(struct worker *w, int ms)
{
	ARG_UNUSED(w);
	k_msleep(ms);
}
#endif

/* ------------------------------------------------------------------ */
/* The display task: one frame, four versions                          */
/* ------------------------------------------------------------------ */

static uint32_t backoffs;

static void display_frame(struct worker *w)
{
#if defined(CONFIG_APP_FIX_NONE)
	lock(w, &spi_lock, K_FOREVER);
	spi_transaction(w, CONFIG_APP_HALF_FRAME_MS);  /* first half of the frame */
	lock(w, &fs_lock, K_FOREVER);                  /* a glyph is missing: SPI -> FS */
	k_msleep(CONFIG_APP_GLYPH_MS);                 /* read it */
	unlock(w, &fs_lock);
	spi_transaction(w, CONFIG_APP_HALF_FRAME_MS);  /* second half */
	unlock(w, &spi_lock);

#elif defined(CONFIG_APP_FIX_LOCK_ORDER)
	/* Fix B1: load the glyph first, in the global order FS -> SPI. */
	lock(w, &fs_lock, K_FOREVER);
	lock(w, &spi_lock, K_FOREVER);                 /* the flash is on the SPI bus */
	k_msleep(CONFIG_APP_GLYPH_MS);
	unlock(w, &spi_lock);
	unlock(w, &fs_lock);
	/* Then the frame, holding SPI only. */
	lock(w, &spi_lock, K_FOREVER);
	spi_transaction(w, CONFIG_APP_HALF_FRAME_MS);
	spi_transaction(w, CONFIG_APP_HALF_FRAME_MS);
	unlock(w, &spi_lock);

#elif defined(CONFIG_APP_FIX_TIMEOUT)
	/* Fix B2: wait for FS only so long; then give SPI back and retry. */
	while (1) {
		lock(w, &spi_lock, K_FOREVER);
		spi_transaction(w, CONFIG_APP_HALF_FRAME_MS);
		if (lock(w, &fs_lock, K_MSEC(CONFIG_APP_LOCK_TIMEOUT_MS)) == 0) {
			break;                         /* got both: finish the frame */
		}
		unlock(w, &spi_lock);                  /* release what we hold ... */
		backoffs++;
		k_msleep(CONFIG_APP_BACKOFF_MS);       /* ... let the other one finish ... */
	}                                              /* ... and redo the half frame */
	k_msleep(CONFIG_APP_GLYPH_MS);
	unlock(w, &fs_lock);
	spi_transaction(w, CONFIG_APP_HALF_FRAME_MS);
	unlock(w, &spi_lock);

#elif defined(CONFIG_APP_FIX_GATEKEEPER)
	/* Fix B3: no SPI lock at all. Each transaction is a request. */
	spi_transaction(w, CONFIG_APP_HALF_FRAME_MS);
	lock(w, &fs_lock, K_FOREVER);
	spi_transaction(w, CONFIG_APP_GLYPH_MS);       /* the glyph, read from the flash */
	unlock(w, &fs_lock);
	spi_transaction(w, CONFIG_APP_HALF_FRAME_MS);
#endif
}

static void display_thread(void *p1, void *p2, void *p3)
{
	int64_t next = t0;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		k_sleep(K_TIMEOUT_ABS_MS(next));
		next += CONFIG_APP_DISPLAY_PERIOD_MS;

		display_frame(&display);
		display.cycles++;
		task_wdt_feed(display.wdt_channel);    /* "I finished a cycle" */
	}
}

K_THREAD_DEFINE(display_tid, STACK_SIZE, display_thread, NULL, NULL, NULL,
		DISPLAY_PRIORITY, 0, SYS_FOREVER_MS);

/* ------------------------------------------------------------------ */
/* The logger task: the same in every version but B3                   */
/* ------------------------------------------------------------------ */

K_SEM_DEFINE(log_now, 0, 1);                           /* given by the button */
static uint32_t button_writes;

static void log_write(struct worker *w)
{
	lock(w, &fs_lock, K_FOREVER);
	k_msleep(CONFIG_APP_OPEN_MS);                  /* open the file: FS only */
#if defined(CONFIG_APP_FIX_GATEKEEPER)
	spi_transaction(w, CONFIG_APP_WRITE_MS);
#else
	lock(w, &spi_lock, K_FOREVER);                 /* the flash driver: FS -> SPI */
	spi_transaction(w, CONFIG_APP_WRITE_MS);
	unlock(w, &spi_lock);
#endif
	unlock(w, &fs_lock);
}

static void logger_thread(void *p1, void *p2, void *p3)
{
	int64_t next = t0 + CONFIG_APP_LOGGER_OFFSET_MS;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		/* Sleep until the next scheduled write, or until the button. */
		if (k_sem_take(&log_now, K_TIMEOUT_ABS_MS(next)) == 0) {
			button_writes++;                   /* an extra, unscheduled write */
		} else {
			next += CONFIG_APP_LOGGER_PERIOD_MS;
		}

		log_write(&logger);
		logger.cycles++;
		task_wdt_feed(logger.wdt_channel);
	}
}

K_THREAD_DEFINE(logger_tid, STACK_SIZE, logger_thread, NULL, NULL, NULL,
		LOGGER_PRIORITY, 0, SYS_FOREVER_MS);

/* ------------------------------------------------------------------ */
/* Detection and recovery: the task watchdog                           */
/* ------------------------------------------------------------------ */

/* Called from the task watchdog's timer: interrupt context. */
static void wdt_expired(int channel_id, void *user_data)
{
	struct worker *w = user_data;

	ARG_UNUSED(channel_id);

	printk("[wdt]    t=%u ms  %s has not finished a cycle for %d ms\n",
	       now_ms(), w->name, CONFIG_APP_WDT_TIMEOUT_MS);
#if defined(CONFIG_APP_WDT_REBOOT)
	printk("[wdt]    rebooting\n");
	sys_reboot(SYS_REBOOT_COLD);
#endif
}

/* ------------------------------------------------------------------ */
/* Called by main.c                                                    */
/* ------------------------------------------------------------------ */

/* "Log this event now": an unscheduled write at a random moment. ISR context. */
void demo_button_pressed(void)
{
	k_sem_give(&log_now);
}

void demo_start(void)
{
	k_mutex_init(&fs_lock.mutex);
	k_mutex_init(&spi_lock.mutex);
	k_sem_init(&display.spi_done, 0, 1);
	k_sem_init(&logger.spi_done, 0, 1);

	k_thread_name_set(display_tid, "display");
	k_thread_name_set(logger_tid, "logger");

	printk("Fix: %s%s\n",
	       IS_ENABLED(CONFIG_APP_FIX_LOCK_ORDER) ? "B1, lock order FS -> SPI everywhere" :
	       IS_ENABLED(CONFIG_APP_FIX_TIMEOUT)    ? "B2, timeout, release and back off" :
	       IS_ENABLED(CONFIG_APP_FIX_GATEKEEPER) ? "B3, SPI gatekeeper thread" :
						       "none (display SPI -> FS, logger FS -> SPI)",
	       IS_ENABLED(CONFIG_APP_CHECK_LOCK_ORDER) ? ", lock order checked" : "");
	printk("Display: a frame every %d ms. Logger: a record every %d ms from %d ms, "
	       "and one per button press\n",
	       CONFIG_APP_DISPLAY_PERIOD_MS, CONFIG_APP_LOGGER_PERIOD_MS,
	       CONFIG_APP_LOGGER_OFFSET_MS);

	/* Software task watchdog only: no hardware watchdog behind it here. */
	task_wdt_init(NULL);
	display.wdt_channel = task_wdt_add(CONFIG_APP_WDT_TIMEOUT_MS, wdt_expired, &display);
	logger.wdt_channel = task_wdt_add(CONFIG_APP_WDT_TIMEOUT_MS, wdt_expired, &logger);

	t0 = k_uptime_get();
#if defined(CONFIG_APP_FIX_GATEKEEPER)
	k_thread_name_set(gatekeeper_tid, "spi_gatekeeper");
	k_thread_start(gatekeeper_tid);
#endif
	k_thread_start(display_tid);
	k_thread_start(logger_tid);
}

static void print_lock(const struct app_lock *l, const struct worker *waiter)
{
#if defined(CONFIG_APP_FIX_GATEKEEPER)
	if (l == &spi_lock) {
		printk("[report]   %-8s not used: the gatekeeper owns the bus\n", l->name);
		return;
	}
#endif
	printk("[report]   %-8s %s%s%s%s\n", l->name,
	       l->owner ? "held by " : "free", l->owner ? l->owner->name : "",
	       waiter ? ", waited for by " : "", waiter ? waiter->name : "");
}

void demo_report(void)
{
	struct worker *workers[] = { &display, &logger };
	struct app_lock *locks[] = { &fs_lock, &spi_lock };
	struct worker *waiter[2] = { NULL, NULL }, *waits_owner[2];
	struct app_lock *waits[2];
	uint32_t frames, writes, bw, bo, fs_wait, spi_wait;
	unsigned int key;

	/* A consistent snapshot: no thread can run between these reads. */
	key = irq_lock();
	frames = display.cycles;
	writes = logger.cycles;
	bw = button_writes;
	bo = backoffs;
	fs_wait = fs_lock.max_wait_us;
	spi_wait = spi_lock.max_wait_us;
	for (int i = 0; i < 2; i++) {
		waits[i] = workers[i]->waiting_for;
		waits_owner[i] = waits[i] ? waits[i]->owner : NULL;
		if (waits[i]) {
			waiter[waits[i] == &fs_lock ? 0 : 1] = workers[i];
		}
	}
	irq_unlock(key);

	printk("[report] t=%u ms  frames=%u writes=%u (button %u)  backoffs=%u  "
	       "max wait: fs_lock %u us, spi_lock %u us\n",
	       now_ms(), frames, writes, bw, bo, fs_wait, spi_wait);
#if defined(CONFIG_APP_FIX_GATEKEEPER)
	printk("[report]   gatekeeper: %u transactions, at most %u request(s) queued\n",
	       gk_served, gk_max_queued);
#endif
	for (int i = 0; i < 2; i++) {
		print_lock(locks[i], waiter[i]);
	}

	/* A cycle in the wait-for graph: each one waits for a lock the other holds. */
	if (waits_owner[0] == &logger && waits_owner[1] == &display) {
		printk("[report]   DEADLOCK: display waits for %s (held by logger), "
		       "logger waits for %s (held by display)\n",
		       waits[0]->name, waits[1]->name);
	}
}
