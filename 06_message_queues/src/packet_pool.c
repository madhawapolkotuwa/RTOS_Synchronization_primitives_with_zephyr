/*
 * 06_message_queues, Part B: packets by reference
 *
 * A packet of 256 bytes is too big to copy twice through a message queue.
 * Instead, the receive interrupt takes a free buffer from a fixed-block
 * pool (k_mem_slab), fills it, and passes only its address to the
 * consumer through a k_fifo. The rule that makes this safe is ownership
 * transfer:
 *
 *   ISR:       alloc from the pool -> fill -> k_fifo_put   (and never touch it again)
 *   consumer:  k_fifo_get -> process -> k_mem_slab_free   (it owns it until the free)
 *
 * The consumer checks every packet: a checksum over the payload, and a
 * copy of the sequence number at the end of the packet against the one
 * at the start. Kconfig switches break the rule in three classic ways
 * (experiments B2 to B4).
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "demo.h"

#define STACK_SIZE         1024
#define CONSUMER_PRIORITY  5
#define PAYLOAD_SIZE       240
#define TOUCH_DELAY_MS     2

struct packet {
#if !defined(CONFIG_APP_FIFO_NO_RESERVED)
	void *fifo_reserved;     /* first word: k_fifo links its items through it */
#endif
	uint32_t seq;            /* first data word: overwritten if there is no reserved word */
	uint32_t len;
	uint8_t payload[PAYLOAD_SIZE];
	uint32_t checksum;       /* over payload[] */
	uint32_t seq_copy;       /* the same as seq, at the far end of the packet */
};

/* CONFIG_APP_POOL_BLOCKS buffers of sizeof(struct packet) bytes, aligned to 4. */
K_MEM_SLAB_DEFINE(packet_pool, sizeof(struct packet), CONFIG_APP_POOL_BLOCKS, 4);
K_FIFO_DEFINE(packet_fifo);

static int64_t t0;

static uint32_t now_ms(void)
{
	return (uint32_t)(k_uptime_get() - t0);
}

static uint32_t payload_checksum(const struct packet *p)
{
	uint32_t sum = 0;

	for (int i = 0; i < PAYLOAD_SIZE; i++) {
		sum += p->payload[i];
	}
	return sum;
}

/* ------------------------------------------------------------------ */
/* The producer: the receive interrupt                                 */
/* ------------------------------------------------------------------ */

/* Written only by the ISR, read by demo_report(). */
static atomic_t sent, dropped, max_in_use;

#if defined(CONFIG_APP_TOUCH_AFTER_SEND)
static struct packet *last_sent;

/* WRONG (experiment B3): "the DMA was still writing" into a packet it gave away. */
static void touch_isr(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	last_sent->payload[0] ^= 0xFF;
}

K_TIMER_DEFINE(touch_timer, touch_isr, NULL);
#endif

static void packet_isr(struct k_timer *timer)
{
	static uint32_t seq;
	struct packet *p;

	ARG_UNUSED(timer);

	seq++;                               /* a dropped packet leaves a gap */

	/* An ISR must not wait for a free buffer. No buffer: drop and count. */
	if (k_mem_slab_alloc(&packet_pool, (void **)&p, K_NO_WAIT) != 0) {
		atomic_inc(&dropped);
		event_led_set(true);             /* red LED: latched at the first problem */
		return;
	}

	uint32_t in_use = k_mem_slab_num_used_get(&packet_pool);

	if (in_use > (uint32_t)atomic_get(&max_in_use)) {
		atomic_set(&max_in_use, in_use);
	}

	/* "Receive" the packet: a real driver's DMA would fill the buffer. */
	p->seq = seq;
	p->len = PAYLOAD_SIZE;
	for (int i = 0; i < PAYLOAD_SIZE; i++) {
		p->payload[i] = (uint8_t)(seq + i);
	}
	p->checksum = payload_checksum(p);
	p->seq_copy = seq;

	/* Hand it over. From here on the consumer owns it. Only the address is copied. */
	k_fifo_put(&packet_fifo, p);
	atomic_inc(&sent);

#if defined(CONFIG_APP_TOUCH_AFTER_SEND)
	last_sent = p;
	k_timer_start(&touch_timer, K_MSEC(TOUCH_DELAY_MS), K_NO_WAIT);
#endif
}

K_TIMER_DEFINE(packet_timer, packet_isr, NULL);

/* ------------------------------------------------------------------ */
/* The consumer                                                        */
/* ------------------------------------------------------------------ */

/* Written only by the consumer, read by demo_report() under irq_lock(). */
static uint32_t received, freed, bad_checksum, bad_header, last_bad_header;
static atomic_t stall_next;

static void consumer_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		struct packet *p = k_fifo_get(&packet_fifo, K_FOREVER);

		received++;

		/* "Process" it: waits for something else, such as a flash write. */
		k_msleep(atomic_clear(&stall_next) ? CONFIG_APP_STALL_MS : CONFIG_APP_HANDLE_MS);

		/* The processing has read the whole packet: was it intact? */
		if (payload_checksum(p) != p->checksum) {
			bad_checksum++;
			event_led_set(true);
		}
		if (p->seq != p->seq_copy) {
			bad_header++;
			last_bad_header = p->seq;
			event_led_set(true);
		}

#if !defined(CONFIG_APP_FORGET_FREE)
		k_mem_slab_free(&packet_pool, p);   /* give it back; p is no longer ours */
		freed++;
#endif
	}
}

K_THREAD_DEFINE(consumer_tid, STACK_SIZE, consumer_thread, NULL, NULL, NULL,
		CONSUMER_PRIORITY, 0, SYS_FOREVER_MS);

/* ------------------------------------------------------------------ */
/* Called by main.c                                                    */
/* ------------------------------------------------------------------ */

/* The consumer is slow on the next packet. ISR context. */
void demo_button_pressed(void)
{
	atomic_set(&stall_next, 1);
}

void demo_start(void)
{
	k_thread_name_set(consumer_tid, "consumer");

	printk("Part B: receive ISR -> k_fifo -> consumer, %u-byte packets by reference\n",
	       (unsigned int)sizeof(struct packet));
	printk("  a packet every %d ms; pool of %d buffers (%u bytes); %d ms per packet%s\n",
	       CONFIG_APP_PACKET_PERIOD_MS, CONFIG_APP_POOL_BLOCKS,
	       (unsigned int)(CONFIG_APP_POOL_BLOCKS * sizeof(struct packet)),
	       CONFIG_APP_HANDLE_MS,
	       IS_ENABLED(CONFIG_APP_FIFO_NO_RESERVED) ? "; NO reserved first word" : "");

	t0 = k_uptime_get();
	k_thread_start(consumer_tid);

	/* First packet half a period in, so that a report never cuts one. */
	k_timer_start(&packet_timer, K_MSEC(CONFIG_APP_PACKET_PERIOD_MS / 2),
		      K_MSEC(CONFIG_APP_PACKET_PERIOD_MS));
}

void demo_report(void)
{
	uint32_t s, d, mx, used, r, f, bc, bh, lbh;
	unsigned int key;

	/* A consistent snapshot: no ISR or thread can run between these reads. */
	key = irq_lock();
	s = (uint32_t)atomic_get(&sent);
	d = (uint32_t)atomic_get(&dropped);
	mx = (uint32_t)atomic_get(&max_in_use);
	used = k_mem_slab_num_used_get(&packet_pool);
	r = received; f = freed; bc = bad_checksum; bh = bad_header; lbh = last_bad_header;
	irq_unlock(key);

	printk("[report] t=%u ms  sent=%u dropped=%u received=%u freed=%u  "
	       "buffers in use now=%u max=%u/%d\n",
	       now_ms(), s, d, r, f, used, mx, CONFIG_APP_POOL_BLOCKS);
	printk("[report]   bad checksum=%u  header overwritten=%u (last value 0x%08x)\n",
	       bc, bh, lbh);
}
