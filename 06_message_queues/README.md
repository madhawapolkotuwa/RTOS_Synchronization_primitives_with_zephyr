# 06 · Message queues: sizing a pipeline, passing ownership

The tutorials so far moved signals: "something happened", with at most one word of data. A message queue moves the data itself, in order, and wakes the receiver in the same operation. This tutorial builds the two standard pipelines from an interrupt to a processing thread. Part A copies small sensor samples through a `k_msgq`, and measures the fill level and the drops to check the queue depth from Lesson 08's worked example. Part B passes large packets by reference: buffers from a `k_mem_slab` pool, linked through a `k_fifo`. Then it breaks the ownership rule that makes that safe.

**Board:** STM32F769I-DISCO · **RTOS:** Zephyr (3.7.99 in this workspace) · **Time:** about 60 minutes

## Learning objectives

When you finish, you will be able to:

- Send from an ISR with `k_msgq_put(..., K_NO_WAIT)`, count every failure, and receive in a thread with `k_msgq_get()`.
- Size a queue from the arrival rate and the longest time the consumer cannot run, and validate the size with the measured maximum fill level and drop count.
- Tell a delay the queue can absorb from an overload no depth can absorb.
- Implement and compare two full-queue policies: drop the newest, and drop the oldest.
- Pass large data by reference with `k_mem_slab` and `k_fifo`, and state the ownership rule: after the send, only the receiver touches the buffer, and the receiver frees it.
- Recognise three reference-passing bugs from their symptoms: a leak, a write after send, and a missing reserved word.

## Theory to read first

- [Lesson 08 · Message queues](../resources/webpages/08_message_queues.html): copy versus reference, full-queue policies, and the worked example (sizing a 1 kHz sensor queue)
- [Lesson 05 · Semaphores](../resources/webpages/05_semaphores.html): a queue is a counting semaphore that carries data

## What you will build

One application with two demos. `prj.conf` selects which one is compiled.

**Part A: a sensor pipeline, by copy** (`src/sensor_pipeline.c`)

```
  sensor ISR (k_timer, 1 kHz):  sample {seq, stamp, accel[3], gyro[3]}  (20 bytes)
       k_msgq_put(&sample_q, &s, K_NO_WAIT)  ── full? count a drop ──┐
                                                                     ▼
                                   k_msgq sample_q: 16 slots × 20 bytes = 320 bytes
                                                                     │
  processing (prio 5):  k_msgq_get(K_FOREVER) → check seq and age → 0.3 ms of CPU
  other work (prio 2):  9 ms of CPU every 100 ms  ← the consumer cannot run meanwhile
```

**Part B: packets by reference** (`src/packet_pool.c`)

```
  receive ISR (k_timer, every 10 ms):
       k_mem_slab_alloc(&packet_pool, &p, K_NO_WAIT)  ── none free? count a drop
       fill *p (256 bytes)   →   k_fifo_put(&packet_fifo, p)      ← only the address moves
                                                                     │
  consumer (prio 5):  p = k_fifo_get(K_FOREVER) → 4 ms of work → check p → k_mem_slab_free(p)

  k_mem_slab packet_pool: 8 buffers × 256 bytes
```

| Board item | Devicetree alias | Part A | Part B |
|------------|------------------|--------|--------|
| Blue USER button B1 (PA0) | `sw0` | Makes the next block of other work 30 ms long | Makes the consumer spend 50 ms on the next packet |
| Red LED LD1 (PJ13) | `led0` | Lights at the first drop and stays on | Lights at the first drop or bad packet and stays on |
| Green LED LD2 (PJ5) | `led1` | Heartbeat | Heartbeat |
| ST-LINK virtual COM port (USART1) | `zephyr,console` | 115200 baud, 8N1 | 115200 baud, 8N1 |

The red LED is a *latch*: once a drop happened, it stays on until RESET. In testing, a drop count above zero means the sizing analysis was wrong, and you should not have to watch the console to notice.

## Files

| File | Purpose |
|------|---------|
| [`CMakeLists.txt`](CMakeLists.txt) | Builds `main.c` plus the demo selected in `prj.conf` |
| [`Kconfig`](Kconfig) | The demo choice, the options, the full-queue policy and the experiment switches |
| [`prj.conf`](prj.conf) | GPIO, priorities, assertions, option values |
| [`src/main.c`](src/main.c) | Common part, the same as tutorials 04 and 05 |
| [`src/demo.h`](src/demo.h) | The three functions each demo provides |
| [`src/sensor_pipeline.c`](src/sensor_pipeline.c) | Part A |
| [`src/packet_pool.c`](src/packet_pool.c) | Part B |

## Build, flash, monitor

From the workspace root (`/workspace` in the dev container):

```bash
west build -b stm32f769i_disco -p auto apps/RTOS_Synchronization_primitives/06_message_queues \
           -d apps/RTOS_Synchronization_primitives/06_message_queues/build
```

If `west build` reports `unknown command "build"`, use CMake as in tutorial 01:

```bash
cd apps/RTOS_Synchronization_primitives/06_message_queues
cmake -GNinja -B build -DBOARD=stm32f769i_disco .
ninja -C build
```

**Switch to Part B** in `prj.conf`: comment out `CONFIG_APP_DEMO_SENSOR=y` and uncomment `# CONFIG_APP_DEMO_PACKETS=y`, or add `-DCONFIG_APP_DEMO_PACKETS=y` to the `cmake` line. The experiment options work the same way, for example `-DCONFIG_APP_QUEUE_DEPTH=8`. When you change a choice (the demo or the full-queue policy), rebuild from scratch (`-p always`, or delete `build/`).

`k_msgq`, `k_fifo` and `k_mem_slab` are always part of the kernel, so there is nothing to enable in `prj.conf`.

Flash and monitor as before: `west flash -d build`, or copy `build/zephyr/zephyr.bin` onto the `DIS_F769NI` drive. Then open the virtual COM port at 115200 baud and press RESET.

> **About the expected output.** This tutorial has not been built or run yet. The outputs below were derived by hand from the code and the 10 kHz tick: each relative sleep or timer lasts one tick (0.1 ms) longer than requested. Times and ages may differ by a fraction of a millisecond, and counts by one or two near a report boundary. The values marked "about" in A2 are estimates. Correct this README once the board has run it.

---

# Part A · A sensor pipeline, by copy

## Code walkthrough

### 1 · The queue is reserved at build time

```c
struct sample {
	uint32_t seq;            /* 1, 2, 3, ...: a gap means a lost sample */
	uint32_t stamp;          /* cycle counter when the ISR read the sensor */
	int16_t accel[3];
	int16_t gyro[3];
};                           /* 20 bytes */

K_MSGQ_DEFINE(sample_q, sizeof(struct sample), CONFIG_APP_QUEUE_DEPTH, 4);
```

`K_MSGQ_DEFINE(name, message size, depth, alignment)` reserves `depth × size` bytes, 320 here, in RAM at build time. Nothing is allocated at run time, so a full queue is a normal, expected condition, not an out-of-memory error. The 12 bytes of sensor data from Lesson 08 come with two bookkeeping words, so that the receiver can detect gaps and measure how old each sample is.

### 2 · The ISR never waits, and counts every failure

```c
s.seq = ++seq;
s.stamp = k_cycle_get_32();
...
ret = k_msgq_put(&sample_q, &s, K_NO_WAIT);   /* copied into the queue */
if (ret != 0) {
	atomic_inc(&dropped);                     /* -ENOMSG: the queue was full */
}
fill = k_msgq_num_used_get(&sample_q);        /* record the high-water mark */
```

- `k_msgq_put()` **copies** the 20 bytes into a free slot. `s` is a local variable of the ISR, and that is fine, because the queue keeps no pointer to it. Part B is different.
- An ISR cannot block, so it uses `K_NO_WAIT`. If the queue is full, the put fails at once with `-ENOMSG`. That is the **drop-the-newest** policy, and the ISR counts every drop.
- After each put, the ISR reads the fill level and keeps the maximum. Drops show that the queue was too small. The maximum fill shows by how much it was big enough.

### 3 · The consumer checks what it gets

```c
k_msgq_get(&sample_q, &s, K_FOREVER);         /* copied out: s is ours */

age = k_cyc_to_us_floor32(k_cycle_get_32() - s.stamp);
if (s.seq != expected) {
	gaps++;
	missing += s.seq - expected;
}
expected = s.seq + 1;

k_busy_wait(CONFIG_APP_PROCESS_US);           /* 0.3 ms of "sensor fusion" */
```

The consumer measures the same thing from the other side. `missing` must equal the ISR's `dropped`. The **age** is the latency the queue adds: how long the oldest sample waited before anyone looked at it. The processing is `k_busy_wait()`, because sensor fusion uses the CPU. It does not wait for anything.

### 4 · The reason for the queue: a consumer that is sometimes late

```c
k_sleep(K_TIMEOUT_ABS_MS(next));              /* every 100 ms, at 50, 150, 250, ... */
k_busy_wait(9 * 1000);                        /* 9 ms: radio burst + control update */
```

A thread at priority 2 takes the CPU for 9 ms every 100 ms. That is the "worst-case consumer delay" from Lesson 08. During those 9 ms, the sensor ISR keeps running, because interrupts preempt every thread. The consumer at priority 5 does not run. Nine samples arrive and wait in the queue.

### 5 · The sizing rule

> depth ≥ arrival rate × longest time the consumer cannot run, provided the consumer's average service rate exceeds the arrival rate.

Here: 1 sample/ms × 9 ms = **9 messages**. Lesson 08 adds margin and chooses 16. The service rate is 1 sample per 0.3 ms, about 3.3 times the arrival rate, so the backlog clears within about 4 ms after each block.

## Run it (Part A)

```
06_message_queues on stm32f769i_disco
Part A: sensor ISR -> k_msgq -> processing thread, 20-byte samples by copy
  a sample every 1000 us; queue depth 16 (320 bytes); processing 300 us per sample
  other work at priority 2: 9 ms every 100 ms; full queue: drop the newest
[report] t=2000 ms  produced=2000 received=2000 dropped=0  queue now=0 max=9/16  max age=8900 us
[report]   no gaps in the sequence numbers
[report] t=4000 ms  produced=4000 received=4000 dropped=0  queue now=0 max=9/16  max age=8900 us
[report]   no gaps in the sequence numbers
```

- **`max=9/16`** is the analysis, measured: 9 samples arrived during a 9 ms block. The 7 free slots are the margin.
- **`max age=8900 us`**: the first sample of a block arrives 0.1 ms after the block starts and waits until it ends. The queue adds up to 9 ms of latency, and that is the price of absorbing the delay.
- **`dropped=0`**, no gaps, and the red LED stays off.
- The blocks are at 50, 150, … ms, so a report never falls inside one. Otherwise main (priority 7) would wait for the block to end, and the counts would no longer be round numbers.

## Experiments (Part A)

Change a value in `prj.conf`, rebuild, flash, and write down your prediction before you look.

### A1 · Too shallow

Set `CONFIG_APP_QUEUE_DEPTH=8`, one less than the analysis requires.

*Prediction:*

```
[report] t=2000 ms  produced=2000 received=1980 dropped=20  queue now=0 max=8/8  max age=8900 us
[report]   20 gaps, 20 samples missing; last gap: seq 1958
```

In each block, samples 1 to 8 fill the queue, and the 9th is dropped. That is one drop per block and 20 per report. The ISR's `dropped` and the consumer's `missing` agree, so every lost sample is accounted for. The red LED lights at 58 ms, at the first drop (the 9th sample of the first block).

Now try `CONFIG_APP_QUEUE_DEPTH=9`: `dropped=0`, but `max=9/9`. It works with **no margin**. One more microsecond of blocking, or one sample's jitter, and it drops. That is why Lesson 08 chose 16.

Press the button (back at depth 16): the next block lasts 30 ms. 30 samples arrive, 16 fit, and **14 are dropped**. Lesson 08's analysis was only as good as its worst-case delay. A longer delay that nobody planned for shows up here as drops, not as a crash.

Set the depth back to 16.

### A2 · An overloaded consumer

Set `CONFIG_APP_PROCESS_US=1200`. Each sample now takes 1.2 ms, longer than the 1 ms between samples.

*Prediction:* the red LED lights after about 100 ms, and then **the reports and the heartbeat stop**. The consumer never empties the queue, so it never blocks. It runs `k_busy_wait()` all the time at priority 5, and main, at priority 7, never runs again. An overloaded thread starves every thread below it, as in tutorial 04's B4.

To see the numbers, also set `CONFIG_MAIN_THREAD_PRIORITY=4`, so that main can preempt the consumer:

```
[report] t=2000 ms  produced=2000 received=1500 dropped=484  queue now=16 max=16/16  max age=28000 us
[report]   … gaps, … samples missing; last gap: seq …
```

(The values are estimates. In 2 s the consumer gets about 1820 ms of CPU, 2000 minus 20 blocks of 9 ms, minus the time main spends printing. 1820 / 1.2 ≈ 1500 samples. The queue holds 16, and the rest are dropped.)

Now set `CONFIG_APP_QUEUE_DEPTH=64`. The drop rate stays the same, about 500 per 2 s once the queue is full, and `max age` grows to about 86 ms (64 × 1.2 ms + 9 ms). **When the consumer is too slow on average, no depth is enough.** A deeper queue only delays the first drop, and makes every sample older. Lesson 08: "too deep hides an overloaded consumer and adds latency".

Restore `CONFIG_APP_PROCESS_US=300`, `CONFIG_APP_QUEUE_DEPTH=16` and `CONFIG_MAIN_THREAD_PRIORITY=7`.

### A3 · Drop the oldest instead

Set `CONFIG_APP_QUEUE_DEPTH=8` again, and switch the policy: comment out `CONFIG_APP_POLICY_DROP_NEWEST=y` and uncomment `CONFIG_APP_POLICY_DROP_OLDEST=y`. Rebuild from scratch.

```c
ret = k_msgq_put(&sample_q, &s, K_NO_WAIT);
if (ret != 0) {
	k_msgq_get(&sample_q, &oldest, K_NO_WAIT);    /* discard the head */
	atomic_inc(&dropped);
	ret = k_msgq_put(&sample_q, &s, K_NO_WAIT);   /* now there is room */
}
```

*Prediction:*

```
[report] t=2000 ms  produced=2000 received=1980 dropped=20  queue now=0 max=8/8  max age=7900 us
[report]   20 gaps, 20 samples missing; last gap: seq 1950
```

The same number of drops, but different samples. With drop-the-newest, the last sample of the block was lost (seq 1958). Now the **first** one is (seq 1950), and the consumer sees the 8 most recent samples. The oldest sample it ever sees is one millisecond younger (`max age=7900 us`). Choose this policy when the latest data matters most, such as a control loop. Choose drop-the-newest when the history matters more than the latest value, such as a log.

The get and the put run back to back in the ISR, and no thread can run between them. Doing the same from a thread would need a lock, or a consumer could take the slot first. Restore `CONFIG_APP_POLICY_DROP_NEWEST=y` and depth 16.

### A4 · An ISR that waits

Uncomment `CONFIG_APP_ISR_PUT_WAIT=y`. The ISR now calls `k_msgq_put(&sample_q, &s, K_MSEC(1))`.

*Prediction*, at the very first sample:

```
ASSERTION FAIL [!arch_is_in_isr() || K_TIMEOUT_EQ(timeout, K_NO_WAIT)] @ WEST_TOPDIR/zephyr/kernel/msgq.c:…
```

The kernel halts with a fatal error, and the heartbeat stops. The assertion fires even though the queue is empty and the put would not have had to wait. A timeout in an ISR is a bug whether or not it ever blocks. Without `CONFIG_ASSERT=y`, the behaviour is undefined. Comment the line out again.

---

# Part B · Packets by reference

Switch to Part B (see [Build](#build-flash-monitor)) before reading on.

## Code walkthrough

### 1 · A pool of fixed-size buffers, and a FIFO of pointers

```c
struct packet {
	void *fifo_reserved;     /* first word: k_fifo links its items through it */
	uint32_t seq;
	uint32_t len;
	uint8_t payload[240];
	uint32_t checksum;       /* over payload[] */
	uint32_t seq_copy;       /* the same as seq, at the far end of the packet */
};                           /* 256 bytes */

K_MEM_SLAB_DEFINE(packet_pool, sizeof(struct packet), CONFIG_APP_POOL_BLOCKS, 4);
K_FIFO_DEFINE(packet_fifo);
```

- A `k_mem_slab` is a pool of equal-sized blocks, reserved at build time: 8 × 256 bytes here. Allocating and freeing one takes constant time, never fragments, and is allowed in an ISR with `K_NO_WAIT`.
- A `k_fifo` does not copy anything. It links the items you give it into a list, and it uses **the first word of each item** for the link. That word belongs to the kernel while the item is in the FIFO. Experiment B4 shows what happens if your data is there.
- Through a `k_msgq`, every 256-byte packet would be copied twice, into the queue and out again, and the queue would need its own 256 bytes per slot. Here, only a 4-byte address moves.

### 2 · The producer: allocate, fill, hand over, forget

```c
if (k_mem_slab_alloc(&packet_pool, (void **)&p, K_NO_WAIT) != 0) {
	atomic_inc(&dropped);                /* no free buffer: drop and count */
	return;
}
... fill p->seq, p->payload, p->checksum, p->seq_copy ...
k_fifo_put(&packet_fifo, p);             /* from here on, the consumer owns *p */
```

The pool plays the role of the queue depth from Part A. When every buffer is in use, the ISR cannot wait for one, so it drops the packet. After `k_fifo_put()`, the ISR must never touch `*p` again: **ownership has moved**.

### 3 · The consumer: take, process, check, free

```c
struct packet *p = k_fifo_get(&packet_fifo, K_FOREVER);
k_msleep(CONFIG_APP_HANDLE_MS);                  /* 4 ms: e.g. waiting for a flash write */
if (payload_checksum(p) != p->checksum) bad_checksum++;
if (p->seq != p->seq_copy)              bad_header++;
k_mem_slab_free(&packet_pool, p);                /* give it back; p is no longer ours */
```

The consumer owns the packet until it frees it, and **it must free it**, or the pool slowly empties (B2). The two checks detect the other two bugs. A payload that changed after it was sent fails the checksum (B3). A header word that the kernel overwrote no longer matches its copy at the end of the packet (B4).

## Run it (Part B)

```
06_message_queues on stm32f769i_disco
Part B: receive ISR -> k_fifo -> consumer, 256-byte packets by reference
  a packet every 10 ms; pool of 8 buffers (2048 bytes); 4 ms per packet
[report] t=2000 ms  sent=200 dropped=0 received=200 freed=200  buffers in use now=0 max=1/8
[report]   bad checksum=0  header overwritten=0 (last value 0x00000000)
```

Each packet is processed and freed 4 ms after it arrives, long before the next one, so at most one buffer is ever in use. Now press the button. The consumer spends 50 ms on the next packet, and 5 more packets arrive meanwhile:

```
[report] t=6000 ms  sent=600 dropped=0 received=600 freed=600  buffers in use now=0 max=6/8
```

Each waiting packet holds a buffer. The pool is the queue depth in disguise, and the same sizing rule applies: 1 packet per 10 ms × 50 ms = 5 waiting, plus the one being processed. With `CONFIG_APP_POOL_BLOCKS=4`, the same press drops 2 packets.

## Experiments (Part B)

### B1 · The pool as the depth

Set `CONFIG_APP_POOL_BLOCKS=4` and press the button.

*Prediction:* `max=4/4` and `dropped=2`. The red LED lights. The packets that arrived while all four buffers were in use were lost, just like the samples in A1. Set it back to 8.

### B2 · A leak: the consumer forgets to free

Uncomment `CONFIG_APP_FORGET_FREE=y`.

*Prediction:*

```
[report] t=2000 ms  sent=8 dropped=192 received=8 freed=0  buffers in use now=8 max=8/8
[report]   bad checksum=0  header overwritten=0 (last value 0x00000000)
```

The first 8 packets work, and then every allocation fails, for ever. The red LED lights at 85 ms. A leak in a pool does not crash anything. It looks like an overload, except that `buffers in use` stays at the maximum even when nothing is being processed. In a real system it can take hours to show, if only one rare path forgets to free. Comment the line out again.

### B3 · Writing after sending

Uncomment `CONFIG_APP_TOUCH_AFTER_SEND=y`. The ISR now changes one byte of each packet 2 ms after sending it, like a DMA channel that was not stopped before its buffer was handed over.

*Prediction:*

```
[report] t=2000 ms  sent=200 dropped=0 received=200 freed=200  buffers in use now=0 max=1/8
[report]   bad checksum=200  header overwritten=0 (last value 0x00000000)
```

The consumer checks each packet after its 4 ms of work, and by then the byte has changed. Every packet is corrupt.

Now also set `CONFIG_APP_HANDLE_MS=1`: `bad checksum=0`. The consumer is done, and has freed the buffer, before the late write. The write now lands in a *free* buffer, and the next packet overwrites it again. **The bug is still there, but the test passes.** A faster consumer hid it, and a slower one, or a new feature, will bring it back. The only reliable fix is the rule: after the send, the sender does not touch the buffer. Restore `CONFIG_APP_HANDLE_MS=4` and comment the line out.

### B4 · No reserved first word

Uncomment `CONFIG_APP_FIFO_NO_RESERVED=y`. `seq` is now the first word of the packet.

*Prediction:* at first, nothing happens:

```
[report] t=2000 ms  sent=200 dropped=0 received=200 freed=200  buffers in use now=0 max=1/8
[report]   bad checksum=0  header overwritten=0 (last value 0x00000000)
```

The consumer is always waiting when a packet arrives, and the kernel then hands the packet directly to the waiting thread, without linking it into the list. Now press the button:

```
[report] t=6000 ms  sent=600 dropped=0 received=600 freed=600  buffers in use now=0 max=6/8
[report]   bad checksum=0  header overwritten=7 (last value 0x00000000)
```

During the stall, packets **queue up**, and the kernel writes its list links into their first word. Each `seq` becomes the address of the next packet (a RAM address, `0x2…`), or 0 for the last one in the list. That is 7 packets: the 5 that arrived during the stall, plus 2 that arrived while the backlog was still draining. A bug that only appears under load, in a field you thought was yours. Comment the line out again.

---

## Common mistakes

- **Waiting in an ISR** (A4). An ISR producer must use `K_NO_WAIT`, for `k_msgq_put()` and for `k_mem_slab_alloc()`.
- **Ignoring the return value of a no-wait put or alloc.** Count every failure. A drop counter that stays at 0 in testing is the evidence that the queue is big enough.
- **Sizing by guesswork** (A1). Compute the depth from the arrival rate and the longest consumer delay, then measure the maximum fill level to confirm it.
- **Making a queue deeper to cure an overload** (A2). If the consumer is slower on average, it only adds latency.
- **Forgetting to free a buffer** (B2) on any path, including error paths.
- **Touching a buffer after sending its pointer** (B3). Ownership has moved. The sender and the receiver now race on it, and the race may only show under different timing.
- **Data in the first word of a `k_fifo` item** (B4). Reserve it with a `void *` first member.
- **Sending a pointer to a local variable.** The copy in Part A is safe. A pointer to a local in Part B's FIFO would point into a stack frame that is reused as soon as the function returns.
- **Large messages by copy.** Every byte is copied twice, and the queue reserves a full slot per message. Pass references for anything bigger than a few dozen bytes.

## Check your understanding

1. A sensor delivers a sample every 0.5 ms, and the consumer can be blocked for up to 6 ms. What is the minimum depth, and what else must be true?
   <details><summary>Answer</summary>2 samples/ms × 6 ms = 12 messages, plus margin, for example 16. The consumer must also process a sample in less than 0.5 ms on average, including its share of the CPU. Otherwise no depth is enough.</details>
2. In A1, why are the ISR's `dropped` and the consumer's `missing` always equal? When could they differ?
   <details><summary>Answer</summary>Every sample has a sequence number, and every sample the ISR could not queue leaves exactly one gap. They differ briefly when drops happened at the end of the queue and the consumer has not reached the next sample yet. Or when a lost sample is never detected, for example if the last samples before a halt are dropped.</details>
3. A3 drops as many samples as A1. Why choose one policy over the other?
   <details><summary>Answer</summary>By which data is worth more. Drop-the-oldest keeps the most recent samples and gives lower latency, which suits control loops and displays. Drop-the-newest keeps an unbroken history up to the overflow, which suits logs and protocols that reassemble data in order.</details>
4. Why does B4 work perfectly until the button is pressed?
   <details><summary>Answer</summary>When a thread is already waiting in <code>k_fifo_get()</code>, the kernel gives the item directly to that thread, without linking it into the list, so the first word is not touched. Only items that wait in the FIFO get a link written into them, and that needs a backlog.</details>
5. You have 64-byte messages at 100 Hz. Would you use a `k_msgq` or a `k_fifo` with a `k_mem_slab`?
   <details><summary>Answer</summary>Either works. A <code>k_msgq</code> copies 128 bytes per message, 12.8 kB/s, which is negligible, and its copy semantics rule out every ownership bug in Part B. Choose references when messages are large, frequent, or must not be copied (DMA buffers).</details>

## Zephyr APIs used

`K_MSGQ_DEFINE`, `k_msgq_put`, `k_msgq_get`, `k_msgq_num_used_get`, `K_MEM_SLAB_DEFINE`, `k_mem_slab_alloc`, `k_mem_slab_free`, `k_mem_slab_num_used_get`, `K_FIFO_DEFINE`, `k_fifo_put`, `k_fifo_get`, `K_TIMER_DEFINE`, `k_timer_start`, `K_USEC`, `K_TIMEOUT_ABS_MS`, `k_sleep`, `k_msleep`, `k_busy_wait`, `k_cycle_get_32`, `k_cyc_to_us_floor32`, `K_THREAD_DEFINE` (with `SYS_FOREVER_MS`), `k_thread_start`, `k_thread_name_set`, `irq_lock`, `irq_unlock`, `atomic_inc`, `atomic_get`, `atomic_set`, `atomic_clear`, plus the GPIO calls from tutorial 01

## Next

**07 · Deadlock:** two threads, two mutexes, opposite order. Reproduce a lock-order deadlock on purpose, diagnose it with the kernel shell, and fix it with lock ordering, timeouts with back-off, or a gatekeeper thread that owns the resource and is fed by a queue like the ones built here.
