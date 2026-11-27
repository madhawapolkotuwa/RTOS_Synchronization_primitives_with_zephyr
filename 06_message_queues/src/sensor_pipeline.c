/*
 * 06_message_queues, Part A: a sensor pipeline, by copy
 *
 * Lesson 08's worked example. A sensor interrupt delivers one sample every
 * millisecond and puts it into a k_msgq without waiting. A processing
 * thread takes the samples out and spends 0.3 ms of CPU on each. On
 * average it keeps up easily (30 % load). But higher-priority work keeps
 * it off the CPU for 9 ms every 100 ms, and the queue must absorb what
 * arrives meanwhile.
 *
 * The ISR counts the samples it could not queue and records the highest
 * fill level. The processing thread checks the sequence numbers and the
 * age of each sample. Together they validate the queue depth: Lesson 08
 * says it must be at least 9 messages.
 *
 * Kconfig switches change the depth, the timing and the full-queue policy
 * (experiments A1 to A4).
 */

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "demo.h"

#define STACK_SIZE           1024
#define PROCESSING_PRIORITY  5
#define OTHER_WORK_PRIORITY  2

/* One sample: 3 accelerometer and 3 gyroscope axes, plus bookkeeping. */
struct sample {
	uint32_t seq;            /* 1, 2, 3, ...: a gap means a lost sample */
	uint32_t stamp;          /* cycle counter when the ISR read the sensor */
	int16_t accel[3];
	int16_t gyro[3];
};

/* depth x sizeof(struct sample) bytes, reserved at build time. Aligned to 4. */
K_MSGQ_DEFINE(sample_q, sizeof(struct sample), CONFIG_APP_QUEUE_DEPTH, 4);

#if defined(CONFIG_APP_ISR_PUT_WAIT)
#define PUT_TIMEOUT K_MSEC(1)            /* WRONG (experiment A4): an ISR cannot wait */
#else
#define PUT_TIMEOUT K_NO_WAIT
#endif

static int64_t t0;

static uint32_t now_ms(void)
{
	return (uint32_t)(k_uptime_get() - t0);
}

/* ------------------------------------------------------------------ */
/* The producer: the sensor interrupt                                  */
/* ------------------------------------------------------------------ */

/* Written only by the ISR, read by demo_report(). */
static atomic_t produced;
static atomic_t dropped;
static atomic_t max_fill;

static void sample_isr(struct k_timer *timer)
{
	static uint32_t seq;
	struct sample s;
	int ret;

	ARG_UNUSED(timer);

	/* "Read the sensor": made-up values, a real ISR would read registers. */
	s.seq = ++seq;
	s.stamp = k_cycle_get_32();
	for (int i = 0; i < 3; i++) {
		s.accel[i] = (int16_t)(seq * (i + 1));
		s.gyro[i] = (int16_t)(seq * (i + 4));
	}
	atomic_inc(&produced);

	/* Copied into the queue: s may go out of scope right after. */
	ret = k_msgq_put(&sample_q, &s, PUT_TIMEOUT);

#if defined(CONFIG_APP_POLICY_DROP_OLDEST)
	if (ret != 0) {
		struct sample oldest;

		/* Make room: discard the head, then queue the new sample. */
		(void)k_msgq_get(&sample_q, &oldest, K_NO_WAIT);
		atomic_inc(&dropped);
		ret = k_msgq_put(&sample_q, &s, K_NO_WAIT);
	}
#endif
	if (ret != 0) {
		atomic_inc(&dropped);            /* -ENOMSG: the queue was full */
	}
	if (atomic_get(&dropped) > 0) {
		event_led_set(true);             /* red LED: latched at the first drop */
	}

	uint32_t fill = k_msgq_num_used_get(&sample_q);

	if (fill > (uint32_t)atomic_get(&max_fill)) {
		atomic_set(&max_fill, fill);
	}
}

K_TIMER_DEFINE(sample_timer, sample_isr, NULL);

/* ------------------------------------------------------------------ */
/* The consumer: the processing thread                                 */
/* ------------------------------------------------------------------ */

/* Written only by the processing thread, read by demo_report() under irq_lock(). */
static uint32_t received, gaps, missing, gap_first, gap_last, max_age_us;

static void processing_thread(void *p1, void *p2, void *p3)
{
	struct sample s;
	uint32_t expected = 1;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		k_msgq_get(&sample_q, &s, K_FOREVER);   /* copied out: s is ours */

		uint32_t age = k_cyc_to_us_floor32(k_cycle_get_32() - s.stamp);

		received++;
		max_age_us = MAX(max_age_us, age);
		if (s.seq != expected) {
			gaps++;
			missing += s.seq - expected;
			gap_first = expected;
			gap_last = s.seq - 1;
		}
		expected = s.seq + 1;

		k_busy_wait(CONFIG_APP_PROCESS_US);     /* the "sensor fusion": CPU time */
	}
}

K_THREAD_DEFINE(processing_tid, STACK_SIZE, processing_thread, NULL, NULL, NULL,
		PROCESSING_PRIORITY, 0, SYS_FOREVER_MS);

/* ------------------------------------------------------------------ */
/* Higher-priority work: the reason the consumer is sometimes late     */
/* ------------------------------------------------------------------ */

static atomic_t button_block;

static void other_work_thread(void *p1, void *p2, void *p3)
{
	/* Half a period in, so that the blocks never coincide with a report. */
	int64_t next = k_uptime_get() + CONFIG_APP_BLOCK_PERIOD_MS / 2;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		k_sleep(K_TIMEOUT_ABS_MS(next));
		next += CONFIG_APP_BLOCK_PERIOD_MS;

		int ms = atomic_clear(&button_block) ? CONFIG_APP_BUTTON_BLOCK_MS
						     : CONFIG_APP_BLOCK_MS;

		if (ms > 0) {
			k_busy_wait((uint32_t)ms * 1000U);  /* "radio burst + control update" */
		}
	}
}

K_THREAD_DEFINE(other_work_tid, STACK_SIZE, other_work_thread, NULL, NULL, NULL,
		OTHER_WORK_PRIORITY, 0, SYS_FOREVER_MS);

/* ------------------------------------------------------------------ */
/* Called by main.c                                                    */
/* ------------------------------------------------------------------ */

/* The next block of higher-priority work is longer. ISR context. */
void demo_button_pressed(void)
{
	atomic_set(&button_block, 1);
}

void demo_start(void)
{
	k_thread_name_set(processing_tid, "processing");
	k_thread_name_set(other_work_tid, "other_work");

	printk("Part A: sensor ISR -> k_msgq -> processing thread, %u-byte samples by copy\n",
	       (unsigned int)sizeof(struct sample));
	printk("  a sample every %d us; queue depth %d (%u bytes); processing %d us per sample\n",
	       CONFIG_APP_SAMPLE_PERIOD_US, CONFIG_APP_QUEUE_DEPTH,
	       (unsigned int)(CONFIG_APP_QUEUE_DEPTH * sizeof(struct sample)),
	       CONFIG_APP_PROCESS_US);
	printk("  other work at priority %d: %d ms every %d ms; full queue: drop the %s\n",
	       OTHER_WORK_PRIORITY, CONFIG_APP_BLOCK_MS, CONFIG_APP_BLOCK_PERIOD_MS,
	       IS_ENABLED(CONFIG_APP_POLICY_DROP_OLDEST) ? "oldest" : "newest");

	t0 = k_uptime_get();
	k_thread_start(processing_tid);
	k_thread_start(other_work_tid);
	k_timer_start(&sample_timer, K_USEC(CONFIG_APP_SAMPLE_PERIOD_US),
		      K_USEC(CONFIG_APP_SAMPLE_PERIOD_US));
}

void demo_report(void)
{
	uint32_t p, d, mf, now_fill, r, g, m, gf, gl, age;
	unsigned int key;

	/* A consistent snapshot: no ISR or thread can run between these reads. */
	key = irq_lock();
	p = (uint32_t)atomic_get(&produced);
	d = (uint32_t)atomic_get(&dropped);
	mf = (uint32_t)atomic_get(&max_fill);
	now_fill = k_msgq_num_used_get(&sample_q);
	r = received; g = gaps; m = missing; gf = gap_first; gl = gap_last; age = max_age_us;
	irq_unlock(key);

	printk("[report] t=%u ms  produced=%u received=%u dropped=%u  queue now=%u max=%u/%d  "
	       "max age=%u us\n", now_ms(), p, r, d, now_fill, mf, CONFIG_APP_QUEUE_DEPTH, age);

	if (g == 0) {
		printk("[report]   no gaps in the sequence numbers\n");
	} else if (gf == gl) {
		printk("[report]   %u gaps, %u samples missing; last gap: seq %u\n", g, m, gf);
	} else {
		printk("[report]   %u gaps, %u samples missing; last gap: seq %u..%u\n",
		       g, m, gf, gl);
	}
}
