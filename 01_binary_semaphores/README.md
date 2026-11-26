# 01 · Binary semaphores: signalling a thread from an interrupt

In this tutorial you build the most common pattern in RTOS firmware: a short ISR that signals a thread, which then does the real work. You will see a binary semaphore wake the thread, a timeout fire when nothing happens, and events get **lost** when they arrive faster than the thread can handle them.

**Board:** STM32F769I-DISCO · **RTOS:** Zephyr (3.7.99 in this workspace) · **Time:** about 45 minutes

## Learning objectives

When you finish, you will be able to:

- Define a binary semaphore with the right initial count and limit for signalling.
- Give a semaphore from interrupt context and take it, with a timeout, in a thread.
- Explain the split between the ISR (top half) and the handler thread (bottom half).
- Predict and measure how many events a binary semaphore loses under a burst.
- Recognise the illegal case: a blocking take inside an ISR.

## Theory to read first

- [Lesson 04 · Interrupts and deferred processing](../resources/webpages/04_interrupts_and_deferred_processing.html): ISR rules, top half / bottom half
- [Lesson 05 · Semaphores](../resources/webpages/05_semaphores.html): definition, initial count, saturation and lost events

## What you will build

```
  blue USER button (PA0)            k_timer (optional, every N ms)
          │ EXTI interrupt                   │ timer interrupt
          ▼                                  ▼
   button_isr() ──────┐            timer_expiry() ──┐
                      ▼                             ▼
               signal_event():  atomic_inc(raised); k_sem_give(&event_sem)
                                        │
                     binary semaphore event_sem (initial 0, limit 1)
                                        │
                                        ▼
   handler thread (prio 2):  k_sem_take(&event_sem, 5 s) → toggle red LED,
                             print counters, "work" for 1000 ms
   main thread    (prio 7):  blinks the green LED every 500 ms (heartbeat)
```

| Board item | Devicetree alias | Role in this tutorial |
|------------|------------------|-----------------------|
| Blue USER button B1 (PA0) | `sw0` | Event source (interrupt on press) |
| Red LED LD1 (PJ13) | `led0` | Toggles once per handled event |
| Green LED LD2 (PJ5) | `led1` | Heartbeat from the lowest-priority thread |
| ST-LINK virtual COM port (USART1) | `zephyr,console` | Output at 115200 baud, 8N1 |

All four are defined by the board itself, so this project needs no devicetree overlay.

## Files

| File | Purpose |
|------|---------|
| [`CMakeLists.txt`](CMakeLists.txt) | Standard Zephyr application build file |
| [`Kconfig`](Kconfig) | Three tutorial options, so experiments need no code edits |
| [`prj.conf`](prj.conf) | GPIO, thread priorities, assertions, tutorial option values |
| [`src/main.c`](src/main.c) | The application |

## Build, flash, monitor

From the workspace root (`/workspace` in the dev container):

```bash
west build -b stm32f769i_disco -p auto apps/RTOS_Synchronization_primitives/01_binary_semaphores \
           -d apps/RTOS_Synchronization_primitives/01_binary_semaphores/build
```

If `west build` reports `unknown command "build"`, the west workspace is not initialised on this machine. CMake does the same build:

```bash
cd apps/RTOS_Synchronization_primitives/01_binary_semaphores
cmake -GNinja -B build -DBOARD=stm32f769i_disco .
ninja -C build
```

Expected memory use: about 27.5 KB flash and 6 KB RAM.

**Flash.** Use either method:

- `west flash -d build` (needs the ST-LINK USB device visible inside the container), or
- drag-and-drop: copy `build/zephyr/zephyr.bin` onto the `DIS_F769NI` USB drive that the ST-LINK shows on the host PC. The board reprograms itself and resets.

**Monitor.** Open the ST-LINK virtual COM port at 115200 baud, 8N1, with any serial terminal (PuTTY, Tera Term, `minicom`, or the VS Code Serial Monitor), then press the black RESET button.

## Code walkthrough

### 1 · The semaphore

```c
K_SEM_DEFINE(event_sem, 0, 1);
```

- **Initial count 0**: nothing has happened yet, so the handler's first `k_sem_take()` blocks.
- **Limit 1**: binary. A `k_sem_give()` when the count is already 1 does nothing.

### 2 · The ISR does almost nothing

```c
static void signal_event(void)
{
	atomic_inc(&events_raised);
	k_sem_give(&event_sem);
}
```

On the STM32, the GPIO callback `button_isr()` runs inside the EXTI interrupt, and a `k_timer` expiry function also runs in interrupt context. Both call `signal_event()`, which only counts the event and gives the semaphore. It does no printing, no LED work and no waiting: that is Lesson 04's rule "be short, never block". `k_sem_give()` is ISR-safe.

`events_raised` is an `atomic_t` because it is written in an ISR and read in a thread. A plain `events_raised++` is a load, an add and a store, and an interrupt between them would lose an increment (Lesson 03).

### 3 · The handler thread blocks with a timeout

```c
int ret = k_sem_take(&event_sem, K_MSEC(CONFIG_APP_TAKE_TIMEOUT_MS));
if (ret == -EAGAIN) { ...no event for 5 s... ; continue; }
```

While the handler waits, its state is *pending*, and it uses no CPU. The green heartbeat LED keeps blinking even though the heartbeat thread has the lowest priority. The return value is always checked: after `-EAGAIN` the thread did **not** get an event.

The handler then toggles the red LED, prints the counters and calls `k_msleep(CONFIG_APP_PROCESSING_MS)`. That sleep simulates slow work, such as writing a record to flash.

### 4 · Counting lost events

```c
key = irq_lock();
raised  = atomic_get(&events_raised);
pending = k_sem_count_get(&event_sem);
irq_unlock(key);

lost = raised - events_handled - pending;
```

Every event is in exactly one of three places: handled, pending in the semaphore (0 or 1), or lost. The two reads are wrapped in `irq_lock()`/`irq_unlock()` so that an ISR cannot run between them and make the snapshot inconsistent. This is the "share data with an ISR safely" rule from Lesson 04, applied to just a few instructions.

### 5 · Priorities

| Thread | Priority | Why |
|--------|----------|-----|
| `handler` | 2 (preemptible) | Deferred interrupt work should run soon after the ISR |
| `main` (heartbeat) | 7, set by `CONFIG_MAIN_THREAD_PRIORITY` | Background work; least urgent |

In Zephyr, a lower number means a higher priority.

## Run it

After reset:

```
01_binary_semaphores on stm32f769i_disco
Press the blue USER button. Processing time per event: 1000 ms
```

**One press**, then wait:

```
[handler] raised=1 handled=1 pending=0 lost=0
```

The red LED toggles. If you press nothing for 5 s:

```
[handler] no event for 5000 ms (k_sem_take returned -EAGAIN)
```

**Five fast presses within one second:**

```
[handler] raised=1 handled=1 pending=0 lost=0
[handler] raised=5 handled=2 pending=0 lost=3
```

The first press is handled at once, and the handler then "works" for 1 s. The second press finds the count at 0 and raises it to 1 (it will be handled next). Presses three to five find the count already at 1 and are lost. The red LED has toggled only twice for five presses. Your exact numbers depend on how fast you press.

> If `raised` goes up by more than 1 for a single press, that is contact bounce: the button made and broke contact several times, and each edge fired the interrupt. The binary semaphore happens to absorb bounce, because extra edges within the handler's processing window are merged. Lesson 05 explains why that behaviour becomes a defect as soon as every event matters.

## Experiments

Change a value in `prj.conf`, rebuild, flash, and compare with the prediction before you look at the output.

### 1 · A repeatable burst

Set `CONFIG_APP_TIMER_PERIOD_MS=200`. The timer gives every 200 ms; the handler needs 1000 ms per event.

*Prediction:* per second, 5 events are raised and 1 is handled, so the other ~4 are lost.

```
Timer event source on: one event every 200 ms
[handler] raised=1 handled=1 pending=0 lost=0
[handler] raised=6 handled=2 pending=0 lost=4
[handler] raised=11 handled=3 pending=0 lost=8
```

The exact values can shift by one, depending on whether the timer tick or the handler's wake-up is processed first in the same tick.

### 2 · Make the consumer faster than the producer

Keep the timer at 200 ms and set `CONFIG_APP_PROCESSING_MS=150`. `lost` should stay at 0. A binary semaphore is a correct signal as long as the handler is always ready again before the next event.

### 3 · A deeper semaphore does not save an overloaded consumer

Set the timer back to 200 ms and processing back to 1000 ms. In `src/main.c`, change the limit to 10: `K_SEM_DEFINE(event_sem, 0, 10);`

*Watch:* the semaphore fills at about 4 events per second (5 in, 1 out) and reaches its limit after about 2.5 s. From then on `pending` stays at 9 in the printout (it is printed just after a take) and `lost` starts growing again. When the average arrival rate is higher than the service rate, no depth is enough (Lesson 08). A larger limit only buys time for bursts. Counting semaphores are the subject of tutorial 02. Set the limit back to 1 afterwards.

### 4 · The wrong initial count

Change the initial count to 1: `K_SEM_DEFINE(event_sem, 1, 1);`. Without pressing anything, right after reset:

```
[handler] raised=0 handled=1 pending=0 lost=4294967295

01_binary_semaphores on stm32f769i_disco
```

The line appears even before the banner. The handler thread (priority 2) is created at boot and runs before `main()` (priority 7) gets to print: its first take succeeds at once.

The handler "handled" an event that never happened, and `lost` wrapped around below zero. In a real driver this is the read of a stale ADC result or an empty receive buffer. For signalling, the initial count must be 0. Set it back afterwards.

### 5 · A shorter timeout

Set `CONFIG_APP_TAKE_TIMEOUT_MS=1000` and do not press the button. A timeout message appears every second while the heartbeat keeps blinking. Timeouts are how a driver detects a device that has stopped responding, instead of hanging forever with `K_FOREVER`.

### 6 · The illegal call

Add this as the first line of `button_isr()`:

```c
k_sem_take(&event_sem, K_MSEC(10));   /* WRONG: blocking call in an ISR */
```

Rebuild, flash, press the button. Because `CONFIG_ASSERT=y`, the kernel stops with an assertion from `kernel/sem.c` that checks `arch_is_in_isr() == false || timeout == K_NO_WAIT`, followed by a fatal error. An ISR has no thread context to suspend; only `K_NO_WAIT` is legal there. Without `CONFIG_ASSERT` the behaviour is undefined. Remove the line afterwards.

## Common mistakes

- **Initial count 1 for a signal.** The first take falls through before anything happened (experiment 4).
- **Printing or doing work in the ISR.** `printk()` over a 115200-baud UART takes about 87 µs per character, so a 50-character line keeps the CPU in interrupt context for over 4 ms.
- **Ignoring the return value of `k_sem_take()`.** After `-EAGAIN` there is no event to process.
- **Expecting a binary semaphore to count.** It records "at least one event since the last take" (experiments 1 and 3).
- **A plain `int` counter shared with an ISR.** Use `atomic_t` or mask interrupts around the access.
- **Blocking in an ISR.** Only `k_sem_take(..., K_NO_WAIT)` is allowed (experiment 6).

## Check your understanding

1. Why does the handler, at priority 2, not stop the heartbeat thread at priority 7 from running?
   <details><summary>Answer</summary>Most of the time it is blocked in <code>k_sem_take()</code> or sleeping in <code>k_msleep()</code>. Blocked threads use no CPU, so the scheduler runs the highest-priority <em>ready</em> thread, which is the heartbeat.</details>
2. With the timer at 200 ms and processing at 1000 ms, what is the largest value `pending` can show in the printout, and why?
   <details><summary>Answer</summary>0. The snapshot is taken microseconds after the take consumed the one stored event, so a new event can almost never arrive in between; new events get stored during the following sleep. In any case a binary semaphore never holds more than 1.</details>
3. Why is the counter snapshot taken with interrupts masked, and why is that acceptable here?
   <details><summary>Answer</summary>So that no ISR runs between reading <code>raised</code> and reading the count, which would make <code>lost</code> wrong by one. It is acceptable because the masked section is a few instructions long, so it adds negligible interrupt latency.</details>
4. Which primitive would you choose if every button press had to be processed?
   <details><summary>Answer</summary>A counting semaphore, sized from the worst-case burst (tutorial 02). If the event carries data, such as a timestamp, use a message queue (tutorial 06).</details>

## Zephyr APIs used

`K_SEM_DEFINE`, `k_sem_give`, `k_sem_take`, `k_sem_count_get`, `K_MSEC`, `K_THREAD_DEFINE`, `k_thread_name_set`, `k_msleep`, `K_TIMER_DEFINE`, `k_timer_start`, `atomic_inc`, `atomic_get`, `irq_lock`, `irq_unlock`, `GPIO_DT_SPEC_GET`, `gpio_pin_configure_dt`, `gpio_pin_interrupt_configure_dt`, `gpio_init_callback`, `gpio_add_callback`, `gpio_pin_toggle_dt`

## Next

**02 · Counting semaphores:** count every event without loss, and manage a pool of buffers.
