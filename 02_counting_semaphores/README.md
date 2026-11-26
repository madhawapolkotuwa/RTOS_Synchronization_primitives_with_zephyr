# 02 · Counting semaphores: bursts without loss, and a buffer pool

Tutorial 01 ended with a binary semaphore losing events. This one fixes that with a counting semaphore, and then uses the same primitive for its second classic job: guarding a pool of identical buffers so that producers slow down, instead of failing, when the pool runs dry.

**Board:** STM32F769I-DISCO · **RTOS:** Zephyr (3.7.99 in this workspace) · **Time:** about 60 minutes

## Learning objectives

When you finish, you will be able to:

- Choose the initial count and the limit of a counting semaphore for the two uses: counting events (initial 0) and counting free resources (initial N).
- Size the limit from the worst-case burst, and tell a burst problem from an overload problem.
- Count lost events exactly in the ISR, and read the backlog with `k_sem_count_get()`.
- Discard a stale backlog with `k_sem_reset()`, and explain why its old count must be read under a lock.
- Build a buffer pool from a counting semaphore plus a lock-free "which buffer" map, and explain why the semaphore alone is not enough.
- Explain back-pressure, and why a producer in an ISR must use `K_NO_WAIT`.

## Theory to read first

- [Lesson 05 · Semaphores](../resources/webpages/05_semaphores.html): counting semaphores, saturation, and the pool pattern (worked example 2)
- [Lesson 08 · Message queues](../resources/webpages/08_message_queues.html): the worked example on sizing, for bursts versus average rate

## What you will build

One application with two demos. `prj.conf` selects which one is compiled.

**Part A: event counter** (`src/event_counter.c`)

```
  k_timer, every 10 ms               blue USER button
  (8 events, then silence             │ EXTI interrupt
   until the next 3 s period)         │
          │ timer interrupt           │
          ▼                           ▼
   burst_timer_expiry() ──┐    demo_button_pressed() ──┐
                          ▼                            ▼
       signal_event():  count raised / lost;  k_sem_give(&event_sem)
                                     │
                 counting semaphore event_sem (initial 0, limit 10)
                                     │
                                     ▼
   handler thread (prio 2):  k_sem_take(K_FOREVER) → toggle red LED,
                             print counters, "work" for 100 ms
```

**Part B: buffer pool** (`src/buffer_pool.c`)

```
   sender0 ─┐   (prio 5, each: take a buffer, fill it, start the transfer,
   sender1 ─┼──  sleep 200 ms; waits up to 1000 ms for a free buffer)
   sender2 ─┤
   button ISR ┘  (K_NO_WAIT: drops its message if no buffer is free)
            │ pool_alloc()                      ▲ pool_free()
            ▼                                   │
   free_bufs: counting semaphore (initial 4, limit 4) = HOW MANY are free
   busy_map:  4 atomic bits                           = WHICH ones are free
            │                                   │
            ▼                                   │
   pool[0..3] ──► simulated transfer, 300 ms ──► tx_done_isr() (k_timer)
```

| Board item | Devicetree alias | Part A | Part B |
|------------|------------------|--------|--------|
| Blue USER button B1 (PA0) | `sw0` | One event per press | A producer in interrupt context |
| Red LED LD1 (PJ13) | `led0` | Toggles per handled event | On while the pool is empty |
| Green LED LD2 (PJ5) | `led1` | Heartbeat | Heartbeat |
| ST-LINK virtual COM port (USART1) | `zephyr,console` | 115200 baud, 8N1 | 115200 baud, 8N1 |

As in tutorial 01, the board defines all four, so there is no devicetree overlay.

## Files

| File | Purpose |
|------|---------|
| [`CMakeLists.txt`](CMakeLists.txt) | Builds `main.c` plus the demo selected in `prj.conf` |
| [`Kconfig`](Kconfig) | The demo choice and the options of both parts |
| [`prj.conf`](prj.conf) | GPIO, priorities, assertions, option values |
| [`src/main.c`](src/main.c) | Common part: button, LEDs, heartbeat, periodic report |
| [`src/demo.h`](src/demo.h) | The three functions each demo provides |
| [`src/event_counter.c`](src/event_counter.c) | Part A |
| [`src/buffer_pool.c`](src/buffer_pool.c) | Part B |

## Build, flash, monitor

From the workspace root (`/workspace` in the dev container):

```bash
west build -b stm32f769i_disco -p auto apps/RTOS_Synchronization_primitives/02_counting_semaphores \
           -d apps/RTOS_Synchronization_primitives/02_counting_semaphores/build
```

If `west build` reports `unknown command "build"`, use CMake as in tutorial 01:

```bash
cd apps/RTOS_Synchronization_primitives/02_counting_semaphores
cmake -GNinja -B build -DBOARD=stm32f769i_disco .
ninja -C build
```

**Switch to Part B** in `prj.conf`: comment out `CONFIG_APP_DEMO_EVENTS=y` and uncomment `# CONFIG_APP_DEMO_POOL=y`. Then rebuild. For a one-off build without editing the file, add `-DCONFIG_APP_DEMO_POOL=y` to the `cmake` line (or after `--` on the `west build` line).

Expected memory use: about 28.6 KB flash and 6 KB RAM for Part A, and 29.6 KB flash and 8.9 KB RAM for Part B (three sender stacks).

Flash and monitor exactly as in tutorial 01: `west flash -d build`, or copy `build/zephyr/zephyr.bin` onto the `DIS_F769NI` drive. Then open the ST-LINK virtual COM port at 115200 baud and press RESET.

---

# Part A · Counting bursty events

## Code walkthrough

### 1 · The semaphore

```c
K_SEM_DEFINE(event_sem, 0, SEM_LIMIT);      /* SEM_LIMIT = CONFIG_APP_EVENT_SEM_LIMIT = 10 */
```

The only change from tutorial 01 is the limit. The initial count is still 0, because no event has happened yet. The limit is now the number of events that can wait to be handled.

### 2 · The ISR counts lost events exactly

```c
static void signal_event(void)
{
	unsigned int key = irq_lock();

	events_raised++;
	if (k_sem_count_get(&event_sem) == SEM_LIMIT) {
		events_lost++;          /* the give below will have no effect */
	}
	k_sem_give(&event_sem);

	uint32_t count = k_sem_count_get(&event_sem);
	if (count > peak_pending) {
		peak_pending = count;
	}

	irq_unlock(key);
}
```

Tutorial 01 *derived* the lost count (raised − handled − pending). Here the ISR sees each loss happen: a give while the count is already at the limit has no effect. The ISR also records the highest count ever reached, `peak`. That number is the one you need to size the limit.

Why `irq_lock()` inside an ISR? On the Cortex-M7, interrupts nest. The button's EXTI interrupt and the system timer can preempt each other. Masking makes "read the count, give, update the counters" one indivisible step, so plain `uint32_t` counters are enough. The masked section is a handful of instructions.

### 3 · A repeatable burst

```c
static void burst_timer_expiry(struct k_timer *timer)
{
	static uint32_t tick;

	if (tick < BURST_SIZE) {
		signal_event();
	}
	tick = (tick + 1) % (BURST_PERIOD / BURST_GAP_MS);
}
```

The timer fires every 10 ms. The first 8 expiries of each 3 s period raise an event, and the remaining 292 do nothing. That gives a burst of 8 events in 70 ms, every 3 s. Two `BUILD_ASSERT`s reject option values for which this does not work.

### 4 · The handler takes once per event

```c
while (1) {
	k_sem_take(&event_sem, K_FOREVER);
	handle_one_event();                     /* toggle LED, print counters */
	k_msleep(CONFIG_APP_PROCESSING_MS);     /* 100 ms of "work" */
}
```

When events are pending, `k_sem_take()` returns at once and decrements the count. The handler blocks only when the backlog is empty. Tutorial 01's timeout branch is gone (`K_FOREVER`), because this tutorial is about counting.

The handler prints a consistency check. Every raised event is either handled, pending, lost or discarded. If those four do not add up to `raised`, the line ends with `<-- counters do not add up`.

## Run it

```
02_counting_semaphores on stm32f769i_disco
Part A: counting events. Semaphore limit 10, processing 100 ms per event
Bursts: 8 events, 10 ms apart, every 3000 ms
[handler] raised=1 handled=1 pending=0 lost=0 discarded=0 peak=0
[handler] raised=8 handled=2 pending=6 lost=0 discarded=0 peak=7
[handler] raised=8 handled=3 pending=5 lost=0 discarded=0 peak=7
[handler] raised=8 handled=4 pending=4 lost=0 discarded=0 peak=7
[handler] raised=8 handled=5 pending=3 lost=0 discarded=0 peak=7
[handler] raised=8 handled=6 pending=2 lost=0 discarded=0 peak=7
[handler] raised=8 handled=7 pending=1 lost=0 discarded=0 peak=7
[handler] raised=8 handled=8 pending=0 lost=0 discarded=0 peak=7
[handler] raised=9 handled=9 pending=0 lost=0 discarded=0 peak=7
...
```

Read it line by line:

- **t = 10 ms.** Event 1 finds the handler waiting. `k_sem_give()` hands the event directly to the waiting thread, so the count never leaves 0. That is why `peak=0` on the first line (Lesson 05, "direct handoff").
- **t = 20…80 ms.** Events 2 to 8 arrive while the handler sleeps. The count climbs to 7.
- **t ≈ 116 ms.** The handler wakes, takes one (6 left) and prints. It then works through the backlog, one event about every 106 ms: 100 ms of "work" plus about 6 ms to print the line. `printk()` sends one character at a time, and at 115200 baud a 66-character line takes about 6 ms.
- **t ≈ 750 ms.** The backlog is empty. The handler blocks until the next burst, at t = 3010 ms (`raised=9`).

The backlog peaked at 7, below the limit of 10, so nothing was lost. The red LED toggles 8 times per burst.

The sizing rule follows from this: **the limit must be at least the largest backlog**, which is the number of events that arrive while the handler is busy. Here that is the burst size minus the one event handled at once: 8 − 1 = 7.

## Experiments (Part A)

Change a value in `prj.conf`, rebuild, flash, and write down your prediction before you look at the output.

### A1 · Back to binary

Set `CONFIG_APP_EVENT_SEM_LIMIT=1`.

*Prediction:* event 1 is handed off, event 2 is stored, and events 3 to 8 are lost: 6 per burst.

```
[handler] raised=1 handled=1 pending=0 lost=0 discarded=0 peak=0
[handler] raised=8 handled=2 pending=0 lost=6 discarded=0 peak=1
[handler] raised=9 handled=3 pending=0 lost=6 discarded=0 peak=1
[handler] raised=16 handled=4 pending=0 lost=12 discarded=0 peak=1
```

This is tutorial 01's behaviour. Only 2 of every 8 events are handled.

### A2 · Find the smallest limit that loses nothing

Predict it from the sizing rule, then test your answer and the value just below it.

| Limit | Lost per burst | `peak` |
|-------|----------------|--------|
| 1 | 6 | 1 |
| 4 | 3 | 4 |
| 6 | 1 | 6 |
| 7 | **0** | 7 |
| 10 (default) | 0 | 7 |

When `peak` equals the limit, the semaphore was full at some point, and that is the moment events start to be lost. In a real product, log `peak` during testing under the worst load you can create, then add a margin.

### A3 · A burst problem or an overload problem?

Reset the limit to 10 and set `CONFIG_APP_BURST_PERIOD_MS=500`. A burst now arrives every 500 ms, while the handler needs 800 ms to work through one.

*Prediction:* 16 events per second arrive and about 10 per second can be handled. The backlog grows to the limit and stays there, and `lost` grows by about 3 per burst. Typical lines once the backlog is full:

```
[handler] raised=24 handled=11 pending=9 lost=4 discarded=0 peak=10
[handler] raised=24 handled=12 pending=8 lost=4 discarded=0 peak=10
...
[handler] raised=40 handled=20 pending=9 lost=11 discarded=0 peak=10
```

Try a limit of 100. Events are still lost, only later. A limit absorbs **bursts**. Against a sustained **overload**, where the average arrival rate is higher than the average service rate, no limit is enough. The fixes are a faster handler, fewer events, or a deliberate drop policy, which is the next experiment.

### A4 · A drop policy with `k_sem_reset()`

Set the period back to 3000 and set `CONFIG_APP_FLUSH_THRESHOLD=5`. When the handler finds 5 or more events pending, it throws the whole backlog away:

```
[handler] raised=1 handled=1 pending=0 lost=0 discarded=0 peak=0
[handler] raised=8 handled=2 pending=6 lost=0 discarded=0 peak=7
[handler] backlog reached 5: k_sem_reset() discarded 6 events
[handler] raised=9 handled=3 pending=0 lost=0 discarded=6 peak=7
```

This suits events where only the latest one matters, for example "the sensor has a new sample", when the handler is too far behind for old samples to be useful. The code:

```c
key = irq_lock();
dropped = k_sem_count_get(&event_sem);
k_sem_reset(&event_sem);
events_discarded += dropped;
irq_unlock(key);
```

`k_sem_reset()` sets the count to 0 but does not return the old count. Without the lock, an ISR could give between the read and the reset, and that event would vanish without being counted anywhere. The consistency check would then fail. The lock is legal here: with interrupts masked, `k_sem_reset()` cannot switch threads, and nothing is waiting on the semaphore anyway, because the handler is its only taker. Also keep in mind that `k_sem_reset()` makes any thread currently blocked in `k_sem_take()` on that semaphore return `-EAGAIN`.

Set the threshold back to 0 afterwards.

### A5 · Every press counts, and so does every bounce

Set `CONFIG_APP_BURST_PERIOD_MS=0` (timer off) and press the button five times quickly. All five presses are handled, one per 100 ms, and the red LED toggles five times.

You may also see `raised` go up by 2 or 3 for a single press. That is contact bounce. Tutorial 01's binary semaphore hid bounce by merging the extra edges. A counting semaphore counts every edge faithfully, so the handler now runs once per bounce. A counting semaphore is only as good as its event source, so debouncing belongs before the give: in hardware (an RC filter), or in the ISR (ignore edges within a few milliseconds of the last accepted one).

---

# Part B · A pool of identical buffers

Switch to Part B (see [Build](#build-flash-monitor)) before reading on, so that you can watch it run.

## Code walkthrough

### 1 · Two structures: how many, and which

```c
K_SEM_DEFINE(free_bufs, POOL_SIZE, POOL_SIZE);   /* initial = limit = 4 */
static ATOMIC_DEFINE(busy_map, POOL_SIZE);        /* bit i set = buffer i in use */
```

- The **semaphore** counts free buffers. The initial count is 4, because all buffers are free at start, which is the opposite of Part A. A take reserves one buffer and blocks when none is left.
- The **bitmap** records *which* buffers are free. A semaphore is only a number, so it cannot tell a sender which buffer to use. `ATOMIC_DEFINE` creates an array of `atomic_t` that starts all-zero, so every buffer starts free.

### 2 · Allocate: reserve, then claim

```c
static struct tx_buffer *pool_alloc(k_timeout_t timeout)
{
	if (k_sem_take(&free_bufs, timeout) != 0) {
		return NULL;                                   /* no buffer in time */
	}
	for (int i = 0; i < POOL_SIZE; i++) {
		if (!atomic_test_and_set_bit(busy_map, i)) {
			return &pool[i];
		}
	}
	/* unreachable while the semaphore and the map agree */
}
```

A successful take guarantees that at most 4 holders exist at once, so at least one bit is still clear. Two senders may scan at the same time, for instance when one preempts the other. `atomic_test_and_set_bit()` sets the bit and returns its old value in one indivisible step, so each bit can be claimed only once. The free list needs no lock, which is the "lock-free" option from Lesson 05's worked example 2. `pool_alloc()` works from threads and, with `K_NO_WAIT`, from ISRs.

### 3 · Free: mark, then give, and catch double frees

```c
static void pool_free(struct tx_buffer *buf)
{
	int index = buf - pool;

	if (!atomic_test_and_clear_bit(busy_map, index)) {
		atomic_inc(&pool_errors);       /* it was already free: double free */
		return;
	}
	k_sem_give(&free_bufs);
}
```

The order matters. The bit is cleared *before* the give. Done the other way round, a sender woken by the give could scan the map before the bit is cleared and find nothing.

`atomic_test_and_clear_bit()` also reports whether the buffer was in use. If it was not, the call is a double free. Giving the semaphore anyway would push the count above the number of really free buffers, and a later allocation would then find no buffer. So the function refuses to give.

### 4 · The simulated transmitter

```c
static void transmit(struct tx_buffer *buf)
{
	k_timer_start(&buf->tx_done, K_MSEC(CONFIG_APP_TX_TIME_MS), K_NO_WAIT);
}

static void tx_done_isr(struct k_timer *timer)
{
	struct tx_buffer *buf = CONTAINER_OF(timer, struct tx_buffer, tx_done);
	pool_free(buf);
}
```

Each buffer embeds its own `k_timer`. When the timer expires, which happens in interrupt context like a real "DMA transfer complete" interrupt, `CONTAINER_OF` recovers the buffer from the timer pointer, and the buffer goes back to the pool.

### 5 · Producers, and what back-pressure means

Each sender loops: allocate, waiting up to 1000 ms; fill the buffer; transmit; sleep 200 ms. Three senders want 15 messages per second. Four buffers that are each busy for 300 ms can carry only 4 / 0.3 s ≈ 13.3 messages per second.

So the pool is the bottleneck, and the senders feel it as waiting inside `k_sem_take()`: they are slowed down to the rate the transmitter can sustain. That is **back-pressure**. Nothing is lost, nothing piles up without bound, and no code had to measure any rates.

The button ISR is a fourth producer. An ISR cannot wait, so it calls `pool_alloc(K_NO_WAIT)` and drops its message when the pool is empty. A producer in an ISR has only two choices: drop, or hand the work to a thread that can wait.

The red LED is on while the free count is 0. The `irq_lock()` in `show_pool_state()` makes the LED match the latest count even when a thread and the transfer-complete ISR update it at nearly the same moment.

## Run it

```
02_counting_semaphores on stm32f769i_disco
Part B: buffer pool. 4 buffers, 3 senders, send every 200 ms, transmit takes 300 ms
[pool] t=2040 ms  transmitted=24 (+24)  free now=0 lowest=0  button sent=0 dropped=0  errors=0
[pool]   sender0 sent=10 failed=0 longest wait=60 ms
[pool]   sender1 sent=9 failed=0 longest wait=90 ms
[pool]   sender2 sent=9 failed=0 longest wait=90 ms
[pool] t=4080 ms  transmitted=51 (+27)  free now=0 lowest=0  button sent=0 dropped=0  errors=0
[pool]   sender0 sent=19 failed=0 longest wait=60 ms
[pool]   sender1 sent=18 failed=0 longest wait=90 ms
[pool]   sender2 sent=18 failed=0 longest wait=90 ms
```

> **About the numbers in Part B.** The outputs in Part B were measured in QEMU (`qemu_cortex_m3` at the board's 10 kHz tick), not on the board. In QEMU, times come out in steps of about 10 ms. On the board, expect the counts to agree to within one or two, and the times to be cleaner: a 1000 ms timeout shows as 1000 ms, not 1010.

- **transmitted +27 per 2 s** is 13.5 per second: the pool's capacity, not the 15 per second the senders asked for.
- **lowest=0**: the pool has run empty. **longest wait** shows the price. A sender comes back 200 ms after its last send and finds all four buffers busy. The buffer it is waiting for was started at about the same moment as its own last send, so it frees 300 ms after that send. The wait is therefore at most TX time − sender period = 300 − 200 = **100 ms**. QEMU shows 60 to 90 ms. A tick-accurate model of the board predicts 100 ms.
- **failed=0**: no wait came close to the 1000 ms timeout.
- The red LED flickers, on whenever all four buffers are in flight.
- `t=` advances by a little more than 2000 ms per report. `main()` checks the time only between its 500 ms heartbeat sleeps, and printing takes time.

Now press the button a few times. Some presses find a free buffer (`button sent` goes up), and the rest find the pool empty (`button dropped`). The ISR could not wait, so it lost the message.

## Experiments (Part B)

### B1 · A fast transmitter: no back-pressure

Set `CONFIG_APP_TX_TIME_MS=100`. Capacity is now 40 messages per second against a demand of 15.

```
[pool] t=2040 ms  transmitted=30 (+30)  free now=4 lowest=1  button sent=0 dropped=0  errors=0
[pool]   sender0 sent=10 failed=0 longest wait=0 ms
[pool] t=4080 ms  transmitted=57 (+27)  free now=1 lowest=1  button sent=0 dropped=0  errors=0
[pool]   sender0 sent=20 failed=0 longest wait=0 ms
```

No sender ever waits, and the pool never runs empty. At most three buffers are in flight at once, so `lowest=1`. `free now` is 4 or 1, depending on whether the report falls between transfers or during one. Throughput is now set by the senders: 3 × 5 = 15 per second, or 30 per report. A report that falls just before a batch of three completions shows +27.

### B2 · A slow transmitter: heavy back-pressure

Set `CONFIG_APP_TX_TIME_MS=1000`. Capacity is now 4 per second.

```
[pool] t=4080 ms  transmitted=15 (+8)  free now=0 lowest=0  button sent=0 dropped=0  errors=0
[pool]   sender0 sent=7 failed=0 longest wait=790 ms
```

The senders still lose nothing, but each one now spends most of its time blocked, and they slow down to exactly the transmitter's rate. Expect a longest wait of about 800 ms on the board. Now also set `CONFIG_APP_ALLOC_TIMEOUT_MS=500`. The waits no longer fit the timeout: each sender gives up after the 500 ms timeout (QEMU shows 510), and `failed` grows by about 2 per sender per report. Throughput stays at 8 per report, because the transmitter is still the bottleneck.

### B3 · Drop instead of wait

Set `CONFIG_APP_TX_TIME_MS=300` and `CONFIG_APP_ALLOC_TIMEOUT_MS=0` (the defaults are 300 and 1000), so the senders now behave like the ISR.

```
[pool] t=4080 ms  transmitted=39 (+20)  free now=3 lowest=0  button sent=0 dropped=0  errors=0
[pool]   sender0 sent=20 failed=0 longest wait=0 ms
[pool]   sender1 sent=10 failed=10 longest wait=0 ms
[pool]   sender2 sent=10 failed=10 longest wait=0 ms
```

Two things get worse:

1. **Messages are lost**: a third of them.
2. **Throughput falls** from about 27 to about 20 per 2 s, even though the senders want just as many messages as before. A sender that fails sleeps its full 200 ms before trying again, so buffers freed in the meantime sit idle (`free now=3`). Waiting uses every buffer the moment it is freed.

The losses are not shared fairly either. All three senders wake in the same tick, and `sender0` always runs first, so it always finds a buffer. That is the [Lesson 05](../resources/webpages/05_semaphores.html) point about who wakes first, in a form you can measure.

Set the timeout back to 1000 afterwards.

### B4 · The wrong initial count

In `src/buffer_pool.c`, change `K_SEM_DEFINE(free_bufs, POOL_SIZE, POOL_SIZE)` to an initial count of 0: `K_SEM_DEFINE(free_bufs, 0, POOL_SIZE)`.

```
[pool] t=2040 ms  transmitted=0 (+0)  free now=0 lowest=4  button sent=0 dropped=0  errors=0
[pool]   sender0 sent=0 failed=1 longest wait=1010 ms
[pool]   sender1 sent=0 failed=1 longest wait=1010 ms
[pool]   sender2 sent=0 failed=1 longest wait=1010 ms
```

All four buffers are free, but the semaphore says none are, and nothing will ever give it. Every sender times out, forever. With `K_FOREVER` instead of a timeout, they would hang silently. `lowest=4` is only the starting value of the statistic, because no take ever succeeded. This is Lesson 05's "0 instead of N for a pool" mistake. Restore the initial count afterwards.

### B5 · A double free

In `tx_done_isr()`, call `pool_free(buf);` twice, as a buggy driver might when it handles the same completion twice.

```
[pool] t=2040 ms  transmitted=24 (+24)  free now=0 lowest=0  button sent=0 dropped=0  errors=24
[pool]   sender0 sent=10 failed=0 longest wait=60 ms
```

`errors` grows by exactly one per transmitted message, and everything else is the same as the normal run: the double-free check refused every second give.

Now also replace the whole `if` block in `pool_free()` with a plain `atomic_clear_bit(busy_map, index);`, which removes the check:

```
[pool] ERROR: the semaphore granted a buffer but none is free
[pool] ERROR: the semaphore granted a buffer but none is free
[pool] ERROR: the semaphore granted a buffer but none is free
[pool] ERROR: the semaphore granted a buffer but none is free
[pool] t=2040 ms  transmitted=22 (+22)  free now=2 lowest=0  button sent=0 dropped=0  errors=4
[pool]   sender0 sent=10 failed=0 longest wait=0 ms
[pool]   sender1 sent=10 failed=0 longest wait=90 ms
[pool]   sender2 sent=6 failed=4 longest wait=90 ms
```

The exact numbers depend on timing, so look for the pattern. The second give succeeds whenever the count is below 4, so the count now claims buffers that the bitmap does not have. A sender whose take succeeds scans the map, finds nothing, and prints the `ERROR` line. `errors` and `failed` grow, and those failures cost no waiting time. The limit caps the damage, because the count can never exceed 4, but it does not prevent it.

The check is not a complete defence either. If a sender had already reallocated the buffer between the two frees, the second free would release a buffer that someone is still using. Ownership bugs must be fixed at their source. Restore the code afterwards.

## Common mistakes

- **Initial count 0 for a pool, or N for an event counter.** A pool starts full, and an event counter starts empty (B4, and tutorial 01's experiment 4).
- **Sizing the limit from the average.** Size it from the largest backlog under the worst burst, measured with a `peak` statistic (A2).
- **Treating overload as a sizing problem.** If events arrive faster on average than they are handled, every limit fills up (A3).
- **Using the semaphore alone as the pool.** It tells you *how many* buffers are free, not *which*. You also need a free list or bitmap that is safe under concurrency.
- **Giving before marking the buffer free**, which lets a woken thread find no buffer, or **giving more than you took**, which makes the count drift above the real number of free buffers (B5).
- **Reading the count and then calling `k_sem_reset()` without a lock**, which loses events uncounted (A4).
- **Blocking in an ISR to get a buffer.** Only `K_NO_WAIT` is legal there.
- **Counting raw button edges.** Debounce before you give (A5).

## Check your understanding

1. In Part A's default run, why does the first line show `peak=0` although an event was raised?
   <details><summary>Answer</summary>The handler was blocked in <code>k_sem_take()</code>. The give handed the event directly to it, so the count stayed at 0. A counting semaphore's count holds only events that nobody was waiting for.</details>
2. Bursts of 20 events arrive 2 ms apart, and the handler needs 5 ms per event. What is the smallest limit that loses nothing?
   <details><summary>Answer</summary>The burst lasts 38 ms. The first event is handed off at once, and during the burst the handler finishes about 38 / 5 ≈ 7 more events, so the backlog peaks at about 20 − 1 − 7 = 12. Choose 12 plus a margin, and confirm it by measuring <code>peak</code>. The average must also fit: 20 × 5 ms = 100 ms of work must fit between two bursts.</details>
3. Why is the free bitmap safe without a mutex, even though three threads and an ISR use it?
   <details><summary>Answer</summary>Every access is one atomic read-modify-write (<code>atomic_test_and_set_bit</code>, <code>atomic_test_and_clear_bit</code>) on a single bit, and the semaphore guarantees that a free bit exists before anyone scans. No sequence of two operations needs to be indivisible.</details>
4. In B3, why did throughput drop when senders stopped waiting?
   <details><summary>Answer</summary>A failed sender sleeps a full period before it retries, so buffers freed in the meantime stay idle. A waiting sender takes a buffer the moment it is freed, which keeps the transmitter busy.</details>
5. Part B's senders all have priority 5. What would change if `sender0` had priority 4, which is more urgent?
   <details><summary>Answer</summary><code>k_sem_give()</code> wakes the highest-priority waiter first, so whenever <code>sender0</code> is waiting it gets the next free buffer. Its own demand is only 5 per second, so the others still get the remaining capacity. Their waits grow, and with drop-on-timeout they would fail more often.</details>

## Zephyr APIs used

`K_SEM_DEFINE`, `k_sem_give`, `k_sem_take`, `k_sem_count_get`, `k_sem_reset`, `K_FOREVER`, `K_NO_WAIT`, `K_MSEC`, `K_TIMER_DEFINE`, `k_timer_init`, `k_timer_start`, `K_THREAD_DEFINE`, `K_THREAD_STACK_ARRAY_DEFINE`, `k_thread_create`, `k_thread_name_set`, `k_msleep`, `k_uptime_get`, `irq_lock`, `irq_unlock`, `ATOMIC_DEFINE`, `atomic_test_and_set_bit`, `atomic_test_and_clear_bit`, `atomic_clear_bit`, `atomic_inc`, `atomic_get`, `atomic_cas`, `CONTAINER_OF`, `BUILD_ASSERT`, plus the GPIO calls from tutorial 01

## Next

**03 · Mutexes:** protect shared data with an owned lock, and see why the semaphores used here are the wrong tool for that.
