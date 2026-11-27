# 05 · Task notifications: one-to-one signals in Zephyr

Often exactly one thread ever waits for a signal. A driver waits for *its* DMA-complete interrupt, and a protocol task waits for *its* "frame received". FreeRTOS has a dedicated feature for this, the task notification: a value and a pending flag stored in the target task itself. Zephyr has no notification API. Instead you attach an ordinary kernel object to the one thread that waits, or you wake the thread directly. This tutorial builds the four usual ways, measures what they cost, and shows where they behave differently.

**Board:** STM32F769I-DISCO · **RTOS:** Zephyr (3.7.99 in this workspace) · **Time:** about 60 minutes

## Learning objectives

When you finish, you will be able to:

- Implement a one-to-one ISR-to-thread signal in four ways: a `k_sem` owned by one thread, a `k_event` owned by one thread, a `k_poll_signal` with `k_poll()`, and `k_wakeup()` of a sleeping thread.
- Measure the RAM and the wake-up latency of each, and read the flash cost from the build.
- Say what each one remembers: a count, a bit, the last value, or nothing.
- Explain why `k_wakeup()` loses a signal that arrives before the receiver sleeps, and why it can end the *wrong* sleep.
- Explain the stale signal: a completion that arrives after its timeout satisfies the next wait, unless you discard it before you start.

## Theory to read first

- [Lesson 07 · Event groups and task notifications](../resources/webpages/07_event_groups_and_notifications.html): task notifications, their update actions and limits, and worked example 2 (SPI DMA completion)
- [Lesson 04 · Interrupts and deferred processing](../resources/webpages/04_interrupts_and_deferred_processing.html): interrupt-to-task latency

## What you will build

One application with two demos. `prj.conf` selects which one is compiled.

**Part A: a driver and its completion ISR** (`src/dma_driver.c`)

```
  driver (prio 2), every 100 ms:
     discard a leftover signal          signal_reset()
     start the "DMA"                    k_timer one-shot, 5 ms
     wait for the completion, 20 ms     signal_wait()      ◄──┐
     check: ok? error? timeout? stale?                        │ one-to-one
                                                              │
  "DMA complete" ISR (k_timer expiry): stamp the cycle counter, signal_from_isr(status)
                                       status = -EIO for every 10th transfer

  signal_*() is one of:  k_sem · k_event · k_poll_signal · k_wakeup()   (prj.conf)
```

**Part B: four receivers, the same signals** (`src/signal_bench.c`)

```
  k_timer ISR, every 500 ms: burst of 2 signals ─┐
  10 ms later:               1 more signal ──────┤
  button ISR:                1 signal ───────────┤   each signal goes to all four:
                                                 ├──► k_sem_give      → rx_sem    (prio 3)
                                                 ├──► k_event_post    → rx_event  (prio 3)
                                                 ├──► k_poll_signal_raise(n) → rx_poll (prio 3)
                                                 └──► k_wakeup        → rx_wakeup (prio 3)
  each receiver: wait → count it → 50 ms of work (k_msleep) → wait again
```

| Board item | Devicetree alias | Part A | Part B |
|------------|------------------|--------|--------|
| Blue USER button B1 (PA0) | `sw0` | Makes the next transfer complete after its timeout | Sends one extra signal |
| Red LED LD1 (PJ13) | `led0` | On when the last transfer went wrong | Toggles every round |
| Green LED LD2 (PJ5) | `led1` | Heartbeat | Heartbeat |
| ST-LINK virtual COM port (USART1) | `zephyr,console` | 115200 baud, 8N1 | 115200 baud, 8N1 |

As before, there is no devicetree overlay, and the button is debounced in `main.c`.

## Files

| File | Purpose |
|------|---------|
| [`CMakeLists.txt`](CMakeLists.txt) | Builds `main.c` plus the demo selected in `prj.conf` |
| [`Kconfig`](Kconfig) | The demo choice, the mechanism choice, the options and experiment switches |
| [`prj.conf`](prj.conf) | GPIO, priorities, assertions, option values |
| [`src/main.c`](src/main.c) | Common part, the same as tutorial 04: button with debounce, LEDs, heartbeat, report |
| [`src/demo.h`](src/demo.h) | The three functions each demo provides |
| [`src/dma_driver.c`](src/dma_driver.c) | Part A |
| [`src/signal_bench.c`](src/signal_bench.c) | Part B |

## Build, flash, monitor

From the workspace root (`/workspace` in the dev container):

```bash
west build -b stm32f769i_disco -p auto apps/RTOS_Synchronization_primitives/05_task_notifications \
           -d apps/RTOS_Synchronization_primitives/05_task_notifications/build
```

If `west build` reports `unknown command "build"`, use CMake as in tutorial 01:

```bash
cd apps/RTOS_Synchronization_primitives/05_task_notifications
cmake -GNinja -B build -DBOARD=stm32f769i_disco .
ninja -C build
```

**Choose the mechanism (Part A)** in `prj.conf`: uncomment exactly one of the four `CONFIG_APP_NOTIFY_*` lines. For a one-off build, pass it on the command line, for example `-DCONFIG_APP_NOTIFY_WAKEUP=y`. After changing the choice, rebuild from scratch (`-p always`, or delete `build/`), so that no old choice is left in the cache.

**Switch to Part B:** comment out `CONFIG_APP_DEMO_DRIVER=y` and uncomment `# CONFIG_APP_DEMO_BENCH=y`, or add `-DCONFIG_APP_DEMO_BENCH=y`.

You do not enable `CONFIG_EVENTS` or `CONFIG_POLL` yourself. The `Kconfig` file *selects* them when a choice needs them, so each Part A build contains only the kernel code its mechanism uses. That is what makes the flash comparison in experiment A1 fair.

Flash and monitor as before: `west flash -d build`, or copy `build/zephyr/zephyr.bin` onto the `DIS_F769NI` drive. Then open the virtual COM port at 115200 baud and press RESET.

> **About the expected output.** This tutorial has not been built or run yet. The outputs below were derived by hand from the code and the 10 kHz tick: each relative sleep or timeout lasts one tick (0.1 ms) longer than requested. Counts may differ by one near a report boundary. The object sizes are predictions for Zephyr 3.7 on a 32-bit core with assertions on, and the latencies cannot be predicted at all: they are shown as `…`. Correct this README once the board has run it.

---

# Part A · A driver and its completion ISR

## Code walkthrough

### 1 · One interface, four implementations

Every mechanism provides the same three functions, so the driver code does not change between them:

```c
static void signal_reset(void);                          /* discard a leftover signal  */
static void signal_from_isr(int status);                 /* the completion ISR signals */
static int  signal_wait(k_timeout_t timeout, int *status);  /* 0, or -EAGAIN on timeout */
```

Only the block for the mechanism selected in `prj.conf` is compiled. They are short enough to compare side by side.

**A semaphore that only the driver takes.**

```c
K_SEM_DEFINE(done_sem, 0, 1);                 /* binary */

/* ISR */   atomic_set(&dma_status, status);  k_sem_give(&done_sem);
/* wait */  if (k_sem_take(&done_sem, timeout) != 0) return -EAGAIN;
            *status = atomic_get(&dma_status);
/* reset */ k_sem_reset(&done_sem);
```

This is tutorial 01's pattern. A semaphore carries no data, so the status travels in a shared variable, written before the give.

**An event object that only the driver waits on.**

```c
/* ISR */   k_event_post(&done_event, status == 0 ? EV_DONE : EV_ERROR);
/* wait */  got = k_event_wait(&done_event, EV_DONE | EV_ERROR, false, timeout);
            if (got == 0) return -EAGAIN;
            k_event_clear(&done_event, got);
/* reset */ k_event_clear(&done_event, EV_DONE | EV_ERROR);
```

The bits carry a little information, here "done" or "error". This is FreeRTOS's "OR bits into the notification value".

**A poll signal.**

```c
/* ISR */   k_poll_signal_raise(&done_signal, status);     /* the int travels with it */
/* wait */  struct k_poll_event ev = K_POLL_EVENT_INITIALIZER(K_POLL_TYPE_SIGNAL,
                                        K_POLL_MODE_NOTIFY_ONLY, &done_signal);
            if (k_poll(&ev, 1, timeout) != 0) return -EAGAIN;
            key = irq_lock();
            k_poll_signal_check(&done_signal, &signaled, &result);
            k_poll_signal_reset(&done_signal);
            irq_unlock(key);
/* reset */ k_poll_signal_reset(&done_signal);
```

The only one of the four that carries a value: a full `int`, set by the sender. The signal stays raised until someone resets it. Check and reset are done with interrupts locked, because a raise between them would be erased by the reset. `k_poll()` can also wait for several objects at once, but here it waits for one.

**A direct wakeup.**

```c
/* ISR */   atomic_set(&dma_status, status);  k_wakeup(driver_tid);
/* wait */  if (k_sleep(timeout) == 0) return -EAGAIN;   /* returns the time left */
            *status = atomic_get(&dma_status);
/* reset */ /* nothing: a wakeup has no memory */
```

No object at all. The ISR names the thread, and `k_wakeup()` ends its `k_sleep()` early. This is the closest to a FreeRTOS notification in cost. It is not the same in behaviour, because a notification has a pending flag and `k_wakeup()` has none. Experiments A2 and A3 show what that means.

### 2 · The "DMA" and the measurement

```c
static void dma_complete_isr(struct k_timer *timer)
{
	...
	atomic_set(&completion_was_early, !atomic_get(&in_wait));
	atomic_set(&done_seq, seq);              /* which transfer completed */
	isr_cycles = k_cycle_get_32();           /* stamp, then signal */
	signal_from_isr(status);
}
```

A one-shot `k_timer` plays the DMA controller. Its expiry function runs in interrupt context, so it obeys the same rules as a real completion ISR. It stamps the cycle counter just before it signals, and the driver stamps it again right after its wait returns. The difference covers everything the mechanism costs the receiver: the signalling call, the scheduler, the context switch and the return from the wait. The report prints the minimum, average and maximum in nanoseconds and in CPU cycles (216 MHz).

Two kinds of completion are left out of the latency statistics:

- An **early** completion arrives while the driver is not yet waiting. Its wait returns at once and there is no wake-up to measure. The report counts these as `early`.
- Any completion that is not `OK` (below).

### 3 · Four outcomes

```c
if (ret != 0)      → done_seq == seq ? LOST : TIMEOUT
else if (done_seq != seq) → STALE
else               → OK (status 0 or -EIO)
```

- **OK:** the completion of *this* transfer woke the driver. Every 10th one carries an error status (`errors`), which shows that the status reaches the driver.
- **TIMEOUT:** no completion arrived in time.
- **LOST:** the transfer did complete (`done_seq == seq`), but the wait still timed out. The signal was lost on the way.
- **STALE:** the wait returned at once, but for an **older** transfer's completion. The driver now believes a transfer finished that has not.

A failure is printed once, when it first happens. The counters in the report keep counting, and the red LED stays on while the transfers go wrong.

### 4 · Discard a leftover before you start

```c
if (IS_ENABLED(CONFIG_APP_RESET_BEFORE_START)) {
	signal_reset();
}
start the transfer, then wait
```

A completion can arrive *after* the driver gave up on it. With a semaphore, an event or a poll signal, that late signal is stored, and it would satisfy the next wait. Discarding it **before** the next transfer starts is safe: no completion for the new transfer can exist yet. Discarding it *after* the start could erase the new transfer's own completion. That is the same trap as the `reset` argument of `k_event_wait()` in tutorial 04, experiment A3. Experiment A4 turns the reset off.

### 5 · The pause between transfers

```c
next += CONFIG_APP_TRANSFER_PERIOD_MS;
while (k_sleep(K_TIMEOUT_ABS_MS(next)) > 0) {
	stray++;                                 /* a k_wakeup() ended the pause early */
}
```

The driver sleeps until an absolute time, so transfers start every 100 ms exactly. `k_wakeup()` ends *any* sleep, including this one. If it does, the loop goes back to sleep until the same deadline, and counts a `stray` wakeup. With the other three mechanisms nobody calls `k_wakeup()`, and the loop body never runs.

## Run it (Part A)

With the default `k_sem`:

```
05_task_notifications on stm32f769i_disco
Part A: a driver thread and its completion ISR. Signal: k_sem
  object size: 16 bytes   carries: nothing (the status goes in a shared variable)
  a transfer every 100 ms, DMA 5 ms, timeout 20 ms, setup 0 us, error every 10, reset before start: yes
  cycle counter: 216000000 Hz
[driver] t=2000 ms  k_sem: transfers=20 ok=18 errors=2 timeouts=0 lost=0 stale=0 early=0 stray=0
[driver]   ISR-to-thread wake-up, 20 samples: min=… avg=… max=… ns (cycles …/…/…)
[driver] t=4000 ms  k_sem: transfers=40 ok=36 errors=4 timeouts=0 lost=0 stale=0 early=0 stray=0
[driver]   ISR-to-thread wake-up, 40 samples: min=… avg=… max=… ns (cycles …/…/…)
```

- Transfers start at 0, 100, … 1900 ms, so 20 have finished by the first report. Transfers 10 and 20 carry the error status.
- Every completion arrived while the driver was waiting (`early=0`), so every one is a latency sample.
- The red LED stays off.
- *What to expect for the latency:* a few microseconds, that is, several hundred to a few thousand cycles. Most of it is the part that all mechanisms share: leaving the interrupt, running the scheduler and switching context. `max` is higher than `min` when the wake-up has to wait for something else, such as another interrupt.

## Experiments (Part A)

Change a value in `prj.conf`, rebuild, flash, and write down your prediction before you look.

### A1 · What each mechanism costs

Build and run Part A four times, once with each `CONFIG_APP_NOTIFY_*` line, and fill in this table. Take **FLASH** and **RAM** from the memory summary at the end of the build, the size from the start-up banner, and the latency from the second report.

| Mechanism | Object (banner) | FLASH | RAM | Latency min / avg / max (ns) |
|-----------|-----------------|-------|-----|------------------------------|
| `k_sem` | 16 B (predicted) | | | |
| `k_event` | 16 B (predicted) | | | |
| `k_poll_signal` | 16 B, plus a 20 B `k_poll_event` on the stack (predicted) | | | |
| `k_wakeup` | 0 B | | | |

The counter line is the same for all four: `ok=18 errors=2` per 2 s. Each mechanism delivers every completion here, and each one delivers the status, through the object or through the shared variable.

*What to look for:*

- **RAM** barely differs. Each object is a few words, and the driver thread's stack (1 KB) dwarfs all of them.
- **FLASH** differs more. `k_poll_signal` pulls in the whole `k_poll` framework (`CONFIG_POLL`), and `k_event` pulls in the event code (`CONFIG_EVENTS`). The semaphore and wakeup code is in every Zephyr image anyway. Also, with `CONFIG_POLL=y` *every* `k_sem` in the system grows by a list head (8 bytes), so that it can be polled. Part B's banner shows `k_sem 24`.
- **Latency:** compare the differences with the common part from the default run. If they are small, choose by **behaviour**, which Part B and the next experiments show, not by speed.

Go back to `CONFIG_APP_NOTIFY_SEM=y`.

### A2 · The completion arrives before the wait

Set `CONFIG_APP_DMA_TIME_MS=1` and `CONFIG_APP_SETUP_US=3000`. The driver now busy-waits for 3 ms after starting the transfer, and the "DMA" completes after about 1.1 ms, while the driver is still busy.

*Prediction with `k_sem`* (and the same with `k_event` and `k_poll_signal`):

```
[driver] t=2000 ms  k_sem: transfers=20 ok=18 errors=2 timeouts=0 lost=0 stale=0 early=20 stray=0
[driver]   ISR-to-thread wake-up: no samples
```

The give was **stored**, so the wait returns at once. Nothing is lost, and there is simply no wake-up to measure.

Now also set `CONFIG_APP_NOTIFY_WAKEUP=y` (comment out the semaphore line) and rebuild from scratch:

```
[driver] t=23 ms  transfer 1: completed, but the signal was lost (timeout)
[driver] t=2000 ms  k_wakeup: transfers=20 ok=0 errors=0 timeouts=0 lost=20 stale=0 early=0 stray=0
[driver]   ISR-to-thread wake-up: no samples
```

`k_wakeup()` on a thread that is **running** does nothing, and nothing is stored. The driver then sleeps for its full 20 ms timeout, every time. The transfers did complete (`lost`, not `timeouts`), but the driver cannot know that. A FreeRTOS notification would have set its pending flag, and the wait would have returned at once. `k_wakeup()` is only safe when the receiver is *certain* to be asleep when the signal comes. Real drivers rarely are, because the completion can always be faster than you think.

Restore `CONFIG_APP_DMA_TIME_MS=5` and `CONFIG_APP_SETUP_US=0`. Keep `k_wakeup` for A3.

### A3 · A late completion

With the default timing, press the button. The next transfer's "DMA" takes 30 ms, 10 ms longer than the driver waits. The driver prints nothing when it starts that transfer, because a `printk()` takes milliseconds at 115200 baud and would delay the start.

*Prediction with `k_wakeup`* (a press at about 5.2 s):

```
[driver] t=5320 ms  transfer 54: no completion within 20 ms (timeout), the button made this DMA late
[driver] t=5330 ms  pause cut short by a stray k_wakeup()
[driver] t=5400 ms  transfer 55: normal again
```

The late completion's `k_wakeup()` ends the **pause** between transfers. The wakeup cannot say which sleep it was meant for. The `while` loop from §5 puts the driver back to sleep, so the next transfer still starts on time. Without that loop, it would start 70 ms early. Every `k_sleep()` in a thread that receives wakeups needs this kind of guard.

*Prediction with `k_sem`* (and the same with `k_event` and `k_poll_signal`): the timeout line, then `normal again` at 5400 ms, and no stray line. The late give is stored, and `signal_reset()` discards it before transfer 55 starts.

Go back to `CONFIG_APP_NOTIFY_SEM=y`.

### A4 · The stale signal

Uncomment `CONFIG_APP_RESET_BEFORE_START=n`, so the driver no longer discards leftovers. Press the button.

*Prediction:*

```
[driver] t=5320 ms  transfer 54: no completion within 20 ms (timeout), the button made this DMA late
[driver] t=5400 ms  transfer 55: woke at once, for transfer 54's completion (STALE)
[driver] t=6000 ms  k_sem: transfers=60 ok=48 errors=5 timeouts=1 lost=0 stale=6 early=0 stray=0
```

- Transfer 54's completion arrives at 5330 ms. Nobody is waiting, so the semaphore stores it.
- Transfer 55's wait takes that stored give at once. The driver believes transfer 55 is done 5 ms before it really is.
- Transfer 55's own completion arrives at 5405 ms and is stored for transfer 56, and so on. **One late interrupt has shifted every later completion by one transfer, for good.** `stale` grows by 10 per second, the red LED stays on, and only RESET recovers. The status each transfer gets also belongs to the previous one.

`k_event` and `k_poll_signal` behave the same, because they store too. `k_wakeup` does not: it stores nothing, so nothing can go stale. Its weakness is the opposite one, from A2.

Comment the line out again.

---

# Part B · Four receivers, the same signals

Switch to Part B (see [Build](#build-flash-monitor)) before reading on.

## Code walkthrough

### 1 · One sender, four mechanisms

```c
static void send_to_all(void)
{
	int n = atomic_inc(&sent) + 1;          /* number the signals 1, 2, 3, ... */

	k_sem_give(&rx_sem);                    /* count++ */
	k_event_post(&rx_event, EV_SIGNAL);     /* bit = 1 */
	k_poll_signal_raise(&rx_signal, n);     /* flag = 1, result = n */
	k_wakeup(wakeup_tid);                   /* end its sleep, if it is asleep */
}
```

Each receiver has its own object, so each signal is still one-to-one. The timer ISR calls `send_to_all()` `CONFIG_APP_BENCH_BURST` times in a row. No receiver can run in between, because they are threads and the sender is an interrupt. A second one-shot timer sends one more signal 10 ms later, while every receiver is in its 50 ms of work. The semaphore here is a **counting** one (`K_SEM_MAX_LIMIT`), so that it can show what counting buys.

### 2 · Four receivers

```c
sem:     k_sem_take(&rx_sem, K_FOREVER);                       handled++; work();
event:   got = k_event_wait(&rx_event, EV_SIGNAL, false, K_FOREVER);
         k_event_clear(&rx_event, got);                        handled++; work();
poll:    k_poll(&ev, 1, K_FOREVER);  check + reset → n;        handled++; work();
         if n skipped numbers: overwritten += the gap
wakeup:  if (k_msleep(60000) > 0) { handled++;
             if (k_msleep(50) > 0) cut_short++; }
```

All four have the same priority and do the same work. The work is `k_msleep(50)`, which stands for "waits for something else", such as a bus. For the wakeup receiver, that is a second sleep that a `k_wakeup()` can end.

## Run it (Part B)

```
05_task_notifications on stm32f769i_disco
Part B: one ISR signals four receivers, one mechanism each
  every 500 ms: a burst of 2, then 1 more after 10 ms (0 = none); 50 ms of work per wake-up
  bytes per object: k_sem 24, k_event 16, k_poll_signal 16 (+ k_poll_event 20 on the waiter's stack), k_wakeup 0
[report] t=2000 ms  rounds=4  signals sent to each receiver=12
[report]   k_sem     counts      handled=12   not handled=0
[report]   k_event   one bit     handled=8    not handled=4
[report]   k_poll    last value  handled=8    not handled=4     values overwritten=4
[report]   k_wakeup  no memory   handled=4    not handled=8     work sleeps cut short=4
```

The rounds start at 250, 750, 1250 and 1750 ms, half a period in, so that a report never splits a round. Each round sends 3 signals to each receiver. Per round:

| Receiver | Burst of 2 | Late one, during the work | Handled |
|----------|-----------|---------------------------|---------|
| `k_sem` | count = 2: two wake-ups | count + 1: a third | **3** |
| `k_event` | the bit is set twice, which is the same as once: one wake-up | the bit is set again: a second wake-up after the work | **2** |
| `k_poll` | raised twice: one wake-up, which sees `n` = 2. Value 1 was overwritten. | raised again: a second wake-up, which sees 3 | **2** |
| `k_wakeup` | the first ends the wait. The second finds the thread already awake and does nothing. | ends the *work* sleep, 40 ms early. The signal is used up, and nobody handles it. | **1** |

That is Lesson 07's table of update actions, made visible. The semaphore **increments**, the event **ORs bits**, and the poll signal **overwrites** a value. The wakeup has no value at all. Press the button between rounds: every receiver is waiting, so all four handle it.

## Experiments (Part B)

### B1 · Signals that never overlap

Set `CONFIG_APP_BENCH_BURST=1` and `CONFIG_APP_BENCH_LATE_MS=100`. The second signal now arrives 100 ms after the first, when all four receivers have finished their 50 ms of work.

*Prediction:*

```
[report] t=2000 ms  rounds=4  signals sent to each receiver=8
[report]   k_sem     counts      handled=8    not handled=0
[report]   k_event   one bit     handled=8    not handled=0
[report]   k_poll    last value  handled=8    not handled=0     values overwritten=0
[report]   k_wakeup  no memory   handled=8    not handled=0     work sleeps cut short=0
```

When every signal finds its receiver waiting, all four are equivalent, and you could choose by cost alone. The question is whether you can *guarantee* that. Usually you cannot, because a faster interrupt or a slower receiver changes the timing.

### B2 · A burst

Set `CONFIG_APP_BENCH_BURST=3` and `CONFIG_APP_BENCH_LATE_MS=0`.

*Prediction:* 12 sent. `k_sem` handles 12. `k_event` handles 4. `k_poll` handles 4, with 8 values overwritten. `k_wakeup` handles 4, with no sleeps cut short. Only the semaphore keeps the number of signals. The poll receiver sees only the last of each burst (`n` = 3, 6, 9, 12), which is right when the value is "the latest reading" and wrong when every value matters. When every value matters, use a message queue (tutorial 06).

### B3 · A signal while the receiver is busy

Set `CONFIG_APP_BENCH_BURST=1` and `CONFIG_APP_BENCH_LATE_MS=10`.

*Prediction:* 8 sent. `k_sem`, `k_event` and `k_poll` each handle 8, with nothing overwritten, because a single signal while busy is stored. `k_wakeup` handles only 4, and `work sleeps cut short=4`. The wakeup that should have meant "there is more" instead cut the previous work short. Restore the defaults afterwards: `BURST=2`, `LATE_MS=10`.

---

## Common mistakes

- **Using `k_wakeup()` as if it had a pending flag** (A2). A wakeup sent to a thread that is not asleep is lost. Use it only when the receiver is certain to be asleep, or use one of the three objects.
- **Sleeping for other reasons in a thread that receives wakeups** (A3, B3). Every `k_sleep()` in that thread can end early. Check the return value and sleep again.
- **Not discarding a leftover signal before a new transfer** (A4). One late completion then shifts every later one. Call `k_sem_reset()`, `k_event_clear()` or `k_poll_signal_reset()` *before* the start, never after it.
- **Checking and resetting a poll signal in two steps with interrupts enabled.** A raise in between is erased.
- **Forgetting to reset a poll signal at all.** It stays raised, and every later `k_poll()` returns at once.
- **Expecting a bit or a poll signal to count** (B2). Use a counting semaphore for "how many", and a queue for "which values".
- **Using a one-to-one signal where two threads wait.** That is a different design: give each its own signal, or use an event group (tutorial 04) to broadcast.
- **Choosing by speed before behaviour.** Measure (A1). The differences are usually smaller than the common part of every wake-up.

## Check your understanding

1. The driver uses `k_wakeup()`, and the DMA usually takes 5 ms. A firmware update makes the setup after the start 2 ms slower. What can go wrong, and how would you see it?
   <details><summary>Answer</summary>Nothing, as long as the setup stays shorter than the DMA time. Once it is longer, the completion arrives before the <code>k_sleep()</code>, the wakeup is lost, and every transfer takes the full timeout instead of 5 ms (A2). You would see it as <code>lost</code> counts, or as a throughput drop. With a semaphore, the give would have been stored.</details>
2. Why is `signal_reset()` called before the transfer starts, and not right before the wait?
   <details><summary>Answer</summary>Between the start and the wait, the new transfer's own completion may already have arrived (A2). A reset there would erase it, and the driver would time out. Before the start, no completion for the new transfer can exist, so anything stored is a leftover.</details>
3. In Part B's default run, the poll receiver handles 2 signals per round, and so does the event receiver. What does the poll receiver know that the event receiver does not?
   <details><summary>Answer</summary>The value of the last signal (<code>n</code>), and from the gaps in <code>n</code>, how many signals it missed. The event receiver knows only that at least one signal came.</details>
4. Which of the four mechanisms can produce a stale signal, and which one can lose one?
   <details><summary>Answer</summary><code>k_sem</code>, <code>k_event</code> and <code>k_poll_signal</code> store a signal until it is consumed, so a late one can go stale (A4). <code>k_wakeup</code> stores nothing, so it cannot go stale, but it loses any signal that comes while the thread is awake (A2, B3).</details>
5. FreeRTOS's `xTaskNotifyGive()` / `ulTaskNotifyTake()` pair behaves like which of the four?
   <details><summary>Answer</summary>Like the counting <code>k_sem</code>. The notification value is incremented by each give and decremented (or zeroed) by each take, and a give before the take is remembered. It is cheaper because the value lives in the task itself, but its behaviour matches the semaphore, not <code>k_wakeup()</code>.</details>

## Zephyr APIs used

`K_SEM_DEFINE`, `k_sem_give`, `k_sem_take`, `k_sem_reset`, `K_SEM_MAX_LIMIT`, `CONFIG_EVENTS`, `K_EVENT_DEFINE`, `k_event_post`, `k_event_wait`, `k_event_clear`, `CONFIG_POLL`, `k_poll_signal_init`, `k_poll_signal_raise`, `k_poll_signal_check`, `k_poll_signal_reset`, `K_POLL_EVENT_INITIALIZER`, `K_POLL_TYPE_SIGNAL`, `K_POLL_MODE_NOTIFY_ONLY`, `k_poll`, `k_wakeup`, `k_sleep`, `K_TIMEOUT_ABS_MS`, `k_cycle_get_32`, `k_cyc_to_ns_floor64`, `sys_clock_hw_cycles_per_sec`, `k_busy_wait`, `K_TIMER_DEFINE`, `k_timer_start`, `K_THREAD_DEFINE` (with `SYS_FOREVER_MS`), `k_thread_start`, `k_thread_name_set`, `irq_lock`, `irq_unlock`, `atomic_set`, `atomic_get`, `atomic_inc`, `atomic_add`, `atomic_clear`, plus the GPIO calls from tutorial 01

## Next

**06 · Message queues:** every signal so far said "something happened", and carried at most one word. A message queue carries the data itself, and keeps every item in order.
