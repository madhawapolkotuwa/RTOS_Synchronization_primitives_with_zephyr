/*
 * 02_counting_semaphores, Part B: a pool of identical buffers with back-pressure
 *
 * POOL_SIZE transmit buffers are shared by three sender threads and the
 * button ISR. A buffer is busy from the moment a sender takes it until its
 * simulated transmission completes TX_TIME_MS later. A k_timer per buffer
 * stands in for the "DMA complete" interrupt, which returns the buffer.
 *
 * Two structures manage the pool, as in Lesson 05, worked example 2:
 *   free_bufs  counting semaphore, initial POOL_SIZE, limit POOL_SIZE.
 *              It counts the free buffers, and makes a sender wait when
 *              there are none. That wait is the back-pressure.
 *   busy_map   one atomic bit per buffer. It records WHICH buffers are
 *              free. The semaphore cannot tell you that.
 *
 * Senders are threads, so they may wait for a buffer. The button ISR may
 * not wait: it uses K_NO_WAIT and drops its message when the pool is empty.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#include "demo.h"

#define POOL_SIZE         CONFIG_APP_POOL_SIZE
#define NUM_SENDERS       3
#define SENDER_STACK_SIZE 1024
#define SENDER_PRIORITY   5     /* all equal: they share the pool fairly */
#define MSG_MAX           32

/* ------------------------------------------------------------------ */
/* The pool                                                            */
/* ------------------------------------------------------------------ */

struct tx_buffer {
	struct k_timer tx_done;     /* simulated "transmission complete" interrupt */
	char data[MSG_MAX];
};

static struct tx_buffer pool[POOL_SIZE];

/* Counts free buffers. Initial count = limit = POOL_SIZE: all free at start. */
K_SEM_DEFINE(free_bufs, POOL_SIZE, POOL_SIZE);

/* Bit i set = buffer i in use. ATOMIC_DEFINE starts all-zero: all free. */
static ATOMIC_DEFINE(busy_map, POOL_SIZE);

/* Statistics. Written from several threads and ISRs, hence atomic. */
static atomic_t free_low_water = ATOMIC_INIT(POOL_SIZE);
static atomic_t tx_completed;
static atomic_t isr_sent;
static atomic_t isr_dropped;
static atomic_t pool_errors;

static void record_low_water(atomic_val_t free_now)
{
	atomic_val_t old;

	do {
		old = atomic_get(&free_low_water);
		if (free_now >= old) {
			return;
		}
	} while (!atomic_cas(&free_low_water, old, free_now));
}

/* Red LED on while the pool is empty, i.e. while senders feel back-pressure. */
static void show_pool_state(void)
{
	/* Masked so that the LED always ends up matching the latest count. */
	unsigned int key = irq_lock();

	event_led_set(k_sem_count_get(&free_bufs) == 0);
	irq_unlock(key);
}

/*
 * Get a free buffer, waiting up to `timeout` for one.
 * From an ISR, `timeout` must be K_NO_WAIT.
 * Returns NULL if no buffer became free in time.
 */
static struct tx_buffer *pool_alloc(k_timeout_t timeout)
{
	/* Step 1: reserve one unit. Blocks while all buffers are busy. */
	if (k_sem_take(&free_bufs, timeout) != 0) {
		return NULL;
	}

	record_low_water(k_sem_count_get(&free_bufs));
	show_pool_state();

	/*
	 * Step 2: find which buffer. The semaphore guarantees that at most
	 * POOL_SIZE holders exist, so at least one bit is clear. Two callers
	 * may scan at once: atomic_test_and_set_bit() lets only one of them
	 * claim each bit.
	 */
	for (int i = 0; i < POOL_SIZE; i++) {
		if (!atomic_test_and_set_bit(busy_map, i)) {
			return &pool[i];
		}
	}

	/* Unreachable while the semaphore and the map agree. */
	atomic_inc(&pool_errors);
	printk("[pool] ERROR: the semaphore granted a buffer but none is free\n");
	return NULL;
}

/* Return a buffer to the pool. Safe from ISRs and threads. */
static void pool_free(struct tx_buffer *buf)
{
	int index = buf - pool;

	/*
	 * Order matters: mark the buffer free BEFORE giving the semaphore.
	 * The other way round, a woken sender could scan the map before the
	 * bit is cleared and find nothing.
	 */
	if (!atomic_test_and_clear_bit(busy_map, index)) {
		/* It was already free: a double free. Do not give twice. */
		atomic_inc(&pool_errors);
		return;
	}

	k_sem_give(&free_bufs);
	show_pool_state();
}

/* Timer expiry = "transmission complete" interrupt. */
static void tx_done_isr(struct k_timer *timer)
{
	struct tx_buffer *buf = CONTAINER_OF(timer, struct tx_buffer, tx_done);

	atomic_inc(&tx_completed);
	pool_free(buf);
}

/* Hand a filled buffer to the (simulated) transmitter. */
static void transmit(struct tx_buffer *buf)
{
	k_timer_start(&buf->tx_done, K_MSEC(CONFIG_APP_TX_TIME_MS), K_NO_WAIT);
}

/* ------------------------------------------------------------------ */
/* Producers                                                           */
/* ------------------------------------------------------------------ */

struct sender_stats {
	uint32_t sent;
	uint32_t failed;        /* no buffer within CONFIG_APP_ALLOC_TIMEOUT_MS */
	uint32_t max_wait_ms;
};

/*
 * Each sender writes only its own entry; main() reads them for the report.
 * 32-bit loads and stores are single instructions on the Cortex-M7, so
 * main() never sees a half-written value. The three values of one entry
 * may be from slightly different moments, which is fine for a report.
 */
static struct sender_stats stats[NUM_SENDERS];

K_THREAD_STACK_ARRAY_DEFINE(sender_stacks, NUM_SENDERS, SENDER_STACK_SIZE);
static struct k_thread sender_threads[NUM_SENDERS];

static void sender_thread(void *p1, void *p2, void *p3)
{
	int id = (int)(uintptr_t)p1;
	struct sender_stats *st = &stats[id];
	uint32_t seq = 0;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		int64_t start = k_uptime_get();
		struct tx_buffer *buf = pool_alloc(K_MSEC(CONFIG_APP_ALLOC_TIMEOUT_MS));
		uint32_t waited = (uint32_t)(k_uptime_get() - start);

		if (waited > st->max_wait_ms) {
			st->max_wait_ms = waited;
		}

		if (buf == NULL) {
			st->failed++;           /* the message is dropped */
		} else {
			snprintk(buf->data, sizeof(buf->data), "sender%d #%u", id, seq);
			transmit(buf);
			st->sent++;
		}
		seq++;

		k_msleep(CONFIG_APP_SENDER_PERIOD_MS);
	}
}

/* The button ISR is a fourth producer. It may not block. */
void demo_button_pressed(void)
{
	struct tx_buffer *buf = pool_alloc(K_NO_WAIT);

	if (buf == NULL) {
		atomic_inc(&isr_dropped);
		return;
	}

	snprintk(buf->data, sizeof(buf->data), "button");
	transmit(buf);
	atomic_inc(&isr_sent);
}

/* ------------------------------------------------------------------ */
/* Called by main.c                                                    */
/* ------------------------------------------------------------------ */

void demo_start(void)
{
	printk("Part B: buffer pool. %d buffers, %d senders, send every %d ms, "
	       "transmit takes %d ms\n",
	       POOL_SIZE, NUM_SENDERS, CONFIG_APP_SENDER_PERIOD_MS, CONFIG_APP_TX_TIME_MS);

	for (int i = 0; i < POOL_SIZE; i++) {
		k_timer_init(&pool[i].tx_done, tx_done_isr, NULL);
	}

	for (int i = 0; i < NUM_SENDERS; i++) {
		char name[] = "sender0";

		name[6] = (char)('0' + i);
		k_thread_create(&sender_threads[i], sender_stacks[i],
				K_THREAD_STACK_SIZEOF(sender_stacks[i]), sender_thread,
				(void *)(uintptr_t)i, NULL, NULL, SENDER_PRIORITY, 0, K_NO_WAIT);
		k_thread_name_set(&sender_threads[i], name);
	}
}

void demo_report(void)
{
	static uint32_t last_completed;
	uint32_t completed = (uint32_t)atomic_get(&tx_completed);

	printk("[pool] t=%u ms  transmitted=%u (+%u)  free now=%u lowest=%d  "
	       "button sent=%d dropped=%d  errors=%d\n",
	       (uint32_t)k_uptime_get(), completed, completed - last_completed,
	       k_sem_count_get(&free_bufs), (int)atomic_get(&free_low_water),
	       (int)atomic_get(&isr_sent), (int)atomic_get(&isr_dropped),
	       (int)atomic_get(&pool_errors));
	last_completed = completed;

	for (int i = 0; i < NUM_SENDERS; i++) {
		printk("[pool]   sender%d sent=%u failed=%u longest wait=%u ms\n",
		       i, stats[i].sent, stats[i].failed, stats[i].max_wait_ms);
	}
}
