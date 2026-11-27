/*
 * 05_task_notifications, Part A: a driver thread and its completion ISR
 *
 * Lesson 07, worked example 2: a driver starts a transfer and waits for
 * the "DMA complete" interrupt, with a timeout. Only the driver ever waits
 * for that interrupt, so the signal is one-to-one: a task notification in
 * FreeRTOS terms. Zephyr has no notification API, so this file implements
 * the signal in one of four ways, chosen in prj.conf:
 *
 *   APP_NOTIFY_SEM          a binary k_sem that only the driver takes
 *   APP_NOTIFY_EVENT        a k_event that only the driver waits on
 *   APP_NOTIFY_POLL_SIGNAL  a k_poll_signal, which also carries an int
 *   APP_NOTIFY_WAKEUP       k_wakeup() of the driver, which waits in k_sleep()
 *
 * The rest of the file does not change between them. The "DMA" is a
 * one-shot k_timer, whose expiry function runs in interrupt context, like
 * a real completion ISR. It stamps the cycle counter just before it
 * signals, and the driver stamps it again when it wakes: the difference
 * is the cost of the mechanism, as seen by the driver.
 *
 * Kconfig switches create the classic failures (experiments A2 to A4).
 */

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "demo.h"

#define STACK_SIZE       1024
#define DRIVER_PRIORITY  2

/* The button makes the next transfer complete this long after its timeout. */
#define LATE_EXTRA_MS    10

/* Forward declaration: the k_wakeup() mechanism needs the thread ID. */
static void driver_thread(void *p1, void *p2, void *p3);
K_THREAD_DEFINE(driver_tid, STACK_SIZE, driver_thread, NULL, NULL, NULL,
		DRIVER_PRIORITY, 0, SYS_FOREVER_MS);

static int64_t t0;

static uint32_t now_ms(void)
{
	return (uint32_t)(k_uptime_get() - t0);
}

/* ------------------------------------------------------------------ */
/* What the "DMA hardware" reports                                     */
/* ------------------------------------------------------------------ */

static atomic_t active_seq;          /* transfer in progress, set by the driver */
static atomic_t done_seq;            /* last transfer completed, set by the ISR */
static atomic_t in_wait;             /* 1 while the driver is inside its wait */
static atomic_t completion_was_early;  /* the ISR ran while the driver was not waiting */
static atomic_t next_is_late;        /* set by the button */
static volatile uint32_t isr_cycles; /* cycle counter just before the signal */

/* ------------------------------------------------------------------ */
/* The four mechanisms. Each provides the same three functions:        */
/*   signal_reset()     discard a leftover signal (before a transfer)  */
/*   signal_from_isr()  the completion ISR signals the driver          */
/*   signal_wait()      the driver waits: 0 with *status, or -EAGAIN   */
/* ------------------------------------------------------------------ */

#if defined(CONFIG_APP_NOTIFY_SEM)

#define MECH_NAME    "k_sem"
#define MECH_SIZE    sizeof(struct k_sem)
#define MECH_CARRIES "nothing (the status goes in a shared variable)"

K_SEM_DEFINE(done_sem, 0, 1);            /* binary: count 0, limit 1 */
static atomic_t dma_status;              /* 0 or -EIO: the semaphore cannot carry it */

static void signal_reset(void)
{
	k_sem_reset(&done_sem);
}

static void signal_from_isr(int status)
{
	atomic_set(&dma_status, status);     /* written before the give */
	k_sem_give(&done_sem);
}

static int signal_wait(k_timeout_t timeout, int *status)
{
	if (k_sem_take(&done_sem, timeout) != 0) {
		return -EAGAIN;
	}
	*status = (int)atomic_get(&dma_status);
	return 0;
}

#elif defined(CONFIG_APP_NOTIFY_EVENT)

#define MECH_NAME    "k_event"
#define MECH_SIZE    sizeof(struct k_event)
#define MECH_CARRIES "up to 32 bits (here DONE or ERROR)"

#define EV_DONE   BIT(0)
#define EV_ERROR  BIT(1)

K_EVENT_DEFINE(done_event);

static void signal_reset(void)
{
	k_event_clear(&done_event, EV_DONE | EV_ERROR);
}

static void signal_from_isr(int status)
{
	k_event_post(&done_event, status == 0 ? EV_DONE : EV_ERROR);
}

static int signal_wait(k_timeout_t timeout, int *status)
{
	uint32_t got = k_event_wait(&done_event, EV_DONE | EV_ERROR, false, timeout);

	if (got == 0) {
		return -EAGAIN;
	}
	k_event_clear(&done_event, got);     /* consume what we got (tutorial 04) */
	*status = (got & EV_ERROR) ? -EIO : 0;
	return 0;
}

#elif defined(CONFIG_APP_NOTIFY_POLL_SIGNAL)

#define MECH_NAME    "k_poll_signal"
#define MECH_SIZE    sizeof(struct k_poll_signal)
#define MECH_CARRIES "one int (here the status itself)"

static struct k_poll_signal done_signal; /* k_poll_signal_init() in demo_start() */

static void signal_reset(void)
{
	k_poll_signal_reset(&done_signal);
}

static void signal_from_isr(int status)
{
	k_poll_signal_raise(&done_signal, status);  /* the status travels with the signal */
}

static int signal_wait(k_timeout_t timeout, int *status)
{
	/* What to wait for. A fresh one each time: its state starts NOT_READY. */
	struct k_poll_event ev = K_POLL_EVENT_INITIALIZER(K_POLL_TYPE_SIGNAL,
							  K_POLL_MODE_NOTIFY_ONLY,
							  &done_signal);
	unsigned int signaled;
	int result;
	unsigned int key;

	if (k_poll(&ev, 1, timeout) != 0) {
		return -EAGAIN;                  /* -EAGAIN on timeout */
	}

	/*
	 * Read the result and reset the signal in one step. Between a
	 * separate check and reset, an ISR could raise again, and the reset
	 * would erase that raise.
	 */
	key = irq_lock();
	k_poll_signal_check(&done_signal, &signaled, &result);
	k_poll_signal_reset(&done_signal);
	irq_unlock(key);

	*status = result;
	return 0;
}

#elif defined(CONFIG_APP_NOTIFY_WAKEUP)

#define MECH_NAME    "k_wakeup"
#define MECH_SIZE    0                   /* uses the thread's own timeout */
#define MECH_CARRIES "nothing (the status goes in a shared variable)"

static atomic_t dma_status;              /* 0 or -EIO: a wakeup cannot carry it */

static void signal_reset(void)
{
	/* Nothing to reset: a wakeup has no memory. */
}

static void signal_from_isr(int status)
{
	atomic_set(&dma_status, status);
	k_wakeup(driver_tid);                /* ends a k_sleep(), if there is one */
}

static int signal_wait(k_timeout_t timeout, int *status)
{
	/*
	 * k_sleep() returns the time left: 0 if it slept to the end, more
	 * if a k_wakeup() ended it early. (A wakeup in the very last tick
	 * also returns 0 and looks like a timeout.)
	 */
	if (k_sleep(timeout) == 0) {
		return -EAGAIN;
	}
	*status = (int)atomic_get(&dma_status);
	return 0;
}

#endif

/* ------------------------------------------------------------------ */
/* The "DMA complete" interrupt                                        */
/* ------------------------------------------------------------------ */

static void dma_complete_isr(struct k_timer *timer)
{
	ARG_UNUSED(timer);

	uint32_t seq = (uint32_t)atomic_get(&active_seq);
	int status = (CONFIG_APP_ERROR_EVERY > 0 && seq % CONFIG_APP_ERROR_EVERY == 0) ? -EIO : 0;

	atomic_set(&completion_was_early, !atomic_get(&in_wait));
	atomic_set(&done_seq, seq);
	isr_cycles = k_cycle_get_32();
	signal_from_isr(status);
}

K_TIMER_DEFINE(dma_timer, dma_complete_isr, NULL);

/* ------------------------------------------------------------------ */
/* The driver                                                          */
/* ------------------------------------------------------------------ */

enum outcome { OUT_OK, OUT_TIMEOUT, OUT_LOST, OUT_STALE };

/* Written only by the driver, read by demo_report() under irq_lock(). */
static uint32_t transfers, ok, errors, timeouts, lost, stale, early, stray;
static uint32_t lat_samples, lat_min = UINT32_MAX, lat_max;
static uint64_t lat_sum;

static void record_latency(uint32_t cycles)
{
	lat_samples++;
	lat_sum += cycles;
	lat_min = MIN(lat_min, cycles);
	lat_max = MAX(lat_max, cycles);
}

static void driver_thread(void *p1, void *p2, void *p3)
{
	enum outcome last = OUT_OK;
	int64_t next = k_uptime_get();
	uint32_t seq = 0;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		seq++;

		if (IS_ENABLED(CONFIG_APP_RESET_BEFORE_START)) {
			signal_reset();          /* a late completion of an old transfer */
		}

		/* No printk() here: it takes milliseconds and would delay the start. */
		bool late = atomic_clear(&next_is_late);
		int dma_ms = late ? CONFIG_APP_DMA_TIMEOUT_MS + LATE_EXTRA_MS
				  : CONFIG_APP_DMA_TIME_MS;

		/* Start the "transfer". */
		atomic_set(&active_seq, seq);
		k_timer_start(&dma_timer, K_MSEC(dma_ms), K_NO_WAIT);

		if (CONFIG_APP_SETUP_US > 0) {
			k_busy_wait(CONFIG_APP_SETUP_US);   /* experiment A2 */
		}

		/* Wait for the completion. */
		int status = 0;

		atomic_set(&in_wait, 1);
		int ret = signal_wait(K_MSEC(CONFIG_APP_DMA_TIMEOUT_MS), &status);
		uint32_t woke_cycles = k_cycle_get_32();

		atomic_set(&in_wait, 0);

		/* What happened? */
		enum outcome out;

		transfers++;
		if (ret != 0) {
			if ((uint32_t)atomic_get(&done_seq) == seq) {
				out = OUT_LOST;          /* it completed, but we never woke */
				lost++;
			} else {
				out = OUT_TIMEOUT;
				timeouts++;
			}
		} else if ((uint32_t)atomic_get(&done_seq) != seq) {
			out = OUT_STALE;                 /* woken by an older transfer's signal */
			stale++;
		} else {
			out = OUT_OK;
			if (status == 0) {
				ok++;
			} else {
				errors++;
			}
			if (atomic_get(&completion_was_early)) {
				early++;                 /* no wake-up to measure */
			} else {
				record_latency(woke_cycles - isr_cycles);
			}
		}

		/* Print only changes, so a failure that repeats prints once. */
		if (out != last) {
			switch (out) {
			case OUT_TIMEOUT:
				printk("[driver] t=%u ms  transfer %u: no completion within %d ms "
				       "(timeout)%s\n", now_ms(), seq, CONFIG_APP_DMA_TIMEOUT_MS,
				       late ? ", the button made this DMA late" : "");
				break;
			case OUT_LOST:
				printk("[driver] t=%u ms  transfer %u: completed, but the signal "
				       "was lost (timeout)\n", now_ms(), seq);
				break;
			case OUT_STALE:
				printk("[driver] t=%u ms  transfer %u: woke at once, for transfer "
				       "%u's completion (STALE)\n", now_ms(), seq,
				       (uint32_t)atomic_get(&done_seq));
				break;
			case OUT_OK:
				printk("[driver] t=%u ms  transfer %u: normal again\n", now_ms(), seq);
				break;
			}
			last = out;
		}
		event_led_set(out != OUT_OK);        /* red LED: the last transfer went wrong */

		/*
		 * Pause until the next transfer is due. With k_wakeup(), a
		 * completion that arrives now ends this sleep early: the wakeup
		 * cannot tell one k_sleep() from another. Sleep again until the
		 * absolute deadline. For the other mechanisms k_sleep() never
		 * returns early, and the loop runs once.
		 */
		next += CONFIG_APP_TRANSFER_PERIOD_MS;
		while (k_sleep(K_TIMEOUT_ABS_MS(next)) > 0) {
			stray++;
			printk("[driver] t=%u ms  pause cut short by a stray k_wakeup()\n",
			       now_ms());
		}
	}
}

/* ------------------------------------------------------------------ */
/* Called by main.c                                                    */
/* ------------------------------------------------------------------ */

/* The button makes the next transfer complete after its timeout. ISR context. */
void demo_button_pressed(void)
{
	atomic_set(&next_is_late, 1);
}

void demo_start(void)
{
#if defined(CONFIG_APP_NOTIFY_POLL_SIGNAL)
	k_poll_signal_init(&done_signal);
#endif
	k_thread_name_set(driver_tid, "driver");

	printk("Part A: a driver thread and its completion ISR. Signal: %s\n", MECH_NAME);
	printk("  object size: %u bytes   carries: %s\n", (unsigned int)MECH_SIZE, MECH_CARRIES);
	printk("  a transfer every %d ms, DMA %d ms, timeout %d ms, setup %d us, "
	       "error every %d, reset before start: %s\n",
	       CONFIG_APP_TRANSFER_PERIOD_MS, CONFIG_APP_DMA_TIME_MS, CONFIG_APP_DMA_TIMEOUT_MS,
	       CONFIG_APP_SETUP_US, CONFIG_APP_ERROR_EVERY,
	       IS_ENABLED(CONFIG_APP_RESET_BEFORE_START) ? "yes" : "NO");
	printk("  cycle counter: %u Hz\n", (unsigned int)sys_clock_hw_cycles_per_sec());

	t0 = k_uptime_get();
	k_thread_start(driver_tid);
}

void demo_report(void)
{
	uint32_t s_tr, s_ok, s_err, s_to, s_lost, s_stale, s_early, s_stray;
	uint32_t n, mn, mx;
	uint64_t sum;
	unsigned int key;

	/* A consistent snapshot: the driver cannot run between these reads. */
	key = irq_lock();
	s_tr = transfers; s_ok = ok; s_err = errors; s_to = timeouts;
	s_lost = lost; s_stale = stale; s_early = early; s_stray = stray;
	n = lat_samples; mn = lat_min; mx = lat_max; sum = lat_sum;
	irq_unlock(key);

	printk("[driver] t=%u ms  %s: transfers=%u ok=%u errors=%u timeouts=%u lost=%u "
	       "stale=%u early=%u stray=%u\n",
	       now_ms(), MECH_NAME, s_tr, s_ok, s_err, s_to, s_lost, s_stale, s_early, s_stray);

	if (n == 0) {
		printk("[driver]   ISR-to-thread wake-up: no samples\n");
		return;
	}
	printk("[driver]   ISR-to-thread wake-up, %u samples: min=%u avg=%u max=%u ns "
	       "(cycles %u/%u/%u)\n", n,
	       (uint32_t)k_cyc_to_ns_floor64(mn), (uint32_t)k_cyc_to_ns_floor64(sum / n),
	       (uint32_t)k_cyc_to_ns_floor64(mx), mn, (uint32_t)(sum / n), mx);
}
