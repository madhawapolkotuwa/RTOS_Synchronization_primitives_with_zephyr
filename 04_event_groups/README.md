# 04 · Event groups: wait for all, react to any

Semaphores carry one kind of signal to one waiter. Many real decisions depend on several conditions: "transfer only when the sensor is ready **and** the configuration is loaded **and** the link is up", or "wake up when the button **or** the timer **or** new data needs attention". An event group holds up to 32 independent bits, and a thread can wait for all of a set or for any of a set. This tutorial builds one demo of each, then breaks them in the ways event groups are most often misused.

**Board:** STM32F769I-DISCO · **RTOS:** Zephyr (3.7.99 in this workspace) · **Time:** about 60 minutes

## Learning objectives

When you finish, you will be able to:

- Use `k_event_wait_all()` to wait until several independent conditions hold, and `k_event_wait()` to react to the first of several sources.
- Choose between `k_event_post()`, `k_event_set()` and `k_event_clear()`, and explain why `k_event_set()` erases other threads' bits.
- Tell **state bits**, which their owner sets and clears, from **event bits**, which the consumer clears.
- Explain what the `reset` argument does: it clears *before* the wait. It is not a clear-on-exit.
- Explain why an event group cannot count, and measure how many posts merge.
- Explain broadcast: one post wakes every waiter whose condition is met. Give each bit a single owner for clearing.

## Theory to read first

- [Lesson 07 · Event groups and task notifications](../resources/webpages/07_event_groups_and_notifications.html): set/clear/wait, AND/OR waits, broadcast, and worked example 1 (the start-up gate)
- [Lesson 05 · Semaphores](../resources/webpages/05_semaphores.html): for the contrast. A semaphore counts, and an event group does not.

## What you will build

One application with two demos. `prj.conf` selects which one is compiled.

**Part A: start-up gate** (`src/startup_gate.c`)

```
  sensor_init (prio 5) ── after 300 ms:  post(SENSOR_READY) ─┐   posted once
  config_init (prio 5) ── after 800 ms:  post(CONFIG_LOADED) ┤   posted once
  link        (prio 5) ── up at 1500 ms: post(LINK_UP),      │   owned by link:
                          down 3 s later: clear(LINK_UP) ... ┤   set AND cleared
  button ISR  ── toggles LINK_UP ("unplug / replug")         │
                                                             ▼
                        k_event sys_events   bit 0 sensor · bit 1 config · bit 2 link
                                                             │
   worker (prio 3):  k_event_wait_all(ALL three, reset=false, 1 s timeout)
                     → transfer for 200 ms → wait again (returns at once while all hold)
                     red LED on while the gate is open
```

**Part B: wait-any dispatcher** (`src/dispatcher.c`)

```
  button ISR ────── post(BUTTON) ─┐
  k_timer ISR, 1 s ─ post(TICK) ──┼──►  k_event app_events   bit 0 button · bit 1 tick · bit 2 data
  producer (prio 5), 300 ms ─ post(DATA) ┘          │
                                                    ▼
   dispatcher (prio 2):  got = k_event_wait(ANY of three, reset=false, K_FOREVER)
                         k_event_clear(got)      ← the only thread that clears
                         handle each bit in got: 50 ms each; button toggles the red LED
```

| Board item | Devicetree alias | Part A | Part B |
|------------|------------------|--------|--------|
| Blue USER button B1 (PA0) | `sw0` | Unplugs / replugs the link | A source in interrupt context |
| Red LED LD1 (PJ13) | `led0` | On while the gate is open | Toggles per handled button press |
| Green LED LD2 (PJ5) | `led1` | Heartbeat | Heartbeat |
| ST-LINK virtual COM port (USART1) | `zephyr,console` | 115200 baud, 8N1 | 115200 baud, 8N1 |

As in the earlier tutorials, the board defines all four, so there is no devicetree overlay. The button is now debounced in `main.c` (`CONFIG_APP_DEBOUNCE_MS`, 50 ms), as tutorial 02's experiment A5 recommended.

## Files

| File | Purpose |
|------|---------|
| [`CMakeLists.txt`](CMakeLists.txt) | Builds `main.c` plus the demo selected in `prj.conf` |
| [`Kconfig`](Kconfig) | The demo choice, the options and the experiment switches |
| [`prj.conf`](prj.conf) | `CONFIG_EVENTS`, GPIO, priorities, assertions, option values |
| [`src/main.c`](src/main.c) | Common part: button with debounce, LEDs, heartbeat, periodic report |
| [`src/demo.h`](src/demo.h) | The three functions each demo provides |
| [`src/startup_gate.c`](src/startup_gate.c) | Part A |
| [`src/dispatcher.c`](src/dispatcher.c) | Part B |

## Build, flash, monitor

From the workspace root (`/workspace` in the dev container):

```bash
west build -b stm32f769i_disco -p auto apps/RTOS_Synchronization_primitives/04_event_groups \
           -d apps/RTOS_Synchronization_primitives/04_event_groups/build
```

If `west build` reports `unknown command "build"`, use CMake as in tutorial 01:

```bash
cd apps/RTOS_Synchronization_primitives/04_event_groups
cmake -GNinja -B build -DBOARD=stm32f769i_disco .
ninja -C build
```

**Switch to Part B** in `prj.conf`: comment out `CONFIG_APP_DEMO_GATE=y` and uncomment `# CONFIG_APP_DEMO_DISPATCH=y`. For a one-off build, add `-DCONFIG_APP_DEMO_DISPATCH=y` to the `cmake` line. The experiment switches work the same way, for example `-DCONFIG_APP_GATE_WAIT_ANY=y`.

`k_event` is optional in Zephyr. Without `CONFIG_EVENTS=y` in `prj.conf`, the build fails at `K_EVENT_DEFINE`.

Flash and monitor as before: `west flash -d build`, or copy `build/zephyr/zephyr.bin` onto the `DIS_F769NI` drive. Then open the virtual COM port at 115200 baud and press RESET.

> **About the expected output.** This tutorial has not been built or run yet. The outputs below were derived by hand from the code and the 10 kHz tick: each relative sleep or timeout lasts one tick (0.1 ms) longer than requested. Times may differ by a millisecond or two, and counts by one near a report boundary. Correct this README once the board has run it.

---

# Part A · A wait-all start-up gate

## Code walkthrough

### 1 · Three bits, three owners

```c
#define EV_SENSOR_READY   BIT(0)
#define EV_CONFIG_LOADED  BIT(1)
#define EV_LINK_UP        BIT(2)
#define EV_ALL_READY      (EV_SENSOR_READY | EV_CONFIG_LOADED | EV_LINK_UP)

K_EVENT_DEFINE(sys_events);      /* 32 bits, all clear at start */
```

Each bit is a **condition**, and each has exactly one owner. The sensor and config init threads post their bit once and exit. The link thread posts `EV_LINK_UP` when the link comes up and clears it when the link drops. The worker only *reads* the bits. It never clears them, because they are not its to clear.

### 2 · Post, not set

```c
k_event_post(&sys_events, task->bit);        /* events |= bit   */
k_event_clear(&sys_events, EV_LINK_UP);      /* events &= ~bit  */
```

`k_event_post()` ORs bits into the word and leaves the others alone. `k_event_set()` **replaces the whole word**, which is the trap in experiment A4. To change a few bits to given values, use `k_event_set_masked()`. All four calls are allowed in ISRs, and the button's "unplug" uses them from interrupt context.

### 3 · The gate

```c
uint32_t got = k_event_wait_all(&sys_events, EV_ALL_READY, false,
                                K_MSEC(CONFIG_APP_GATE_TIMEOUT_MS));
if (got == 0) {
	/* timeout: print which bits are still missing, wait again */
}
/* all three hold: transfer for 200 ms, then loop */
```

- `k_event_wait_all()` blocks until **every** bit in the mask is set, and returns the matching bits, or 0 on timeout.
- The bits stay set after the wait. While all three hold, the next wait returns at once, so the worker keeps transferring.
- When the link drops, the next wait blocks. The worker pauses by itself, with no "is the link still up?" logic of its own. That is Lesson 07's worked example 1.
- The third argument, `reset`, is `false`. Experiment A3 shows what `true` does.
- A timeout is not an error. The worker uses it to print what it is still waiting for, with `k_event_test()`, which reads bits without waiting.

### 4 · A condition can change during the work

The worker checks the bits again *after* each transfer. If the link dropped during a transfer, that transfer ran partly without a link. An event group tells you that the conditions held *when the wait returned*, not that they still hold. Real code must still handle errors from the link itself.

## Run it (Part A)

```
04_event_groups on stm32f769i_disco
Part A: start-up gate. The worker waits for ALL of: sensor config link
[init]   t=300 ms  sensor ready (posted once, never again)
[init]   t=800 ms  config ready (posted once, never again)
[worker] t=1000 ms  still waiting, missing: link
[link]   t=1500 ms  up
[worker] t=1500 ms  gate open, the wait returned: sensor config link
[gate]   t=2000 ms  bits now: sensor config link  openings=1 transfers=2 early=0 timeouts=1 button=0
[gate]   t=4000 ms  bits now: sensor config link  openings=1 transfers=12 early=0 timeouts=1 button=0
[link]   t=4500 ms  down
[worker] t=4501 ms  gate closed, missing: link
[worker] t=5501 ms  still waiting, missing: link
[gate]   t=6001 ms  bits now: sensor config  openings=1 transfers=15 early=0 timeouts=2 button=0
[link]   t=6500 ms  up
[worker] t=6500 ms  gate open, the wait returned: sensor config link
[gate]   t=8001 ms  bits now: sensor config link  openings=2 transfers=22 early=0 timeouts=2 button=0
```

- **t = 300 and 800 ms.** Two of the three bits are set. The worker stays blocked, because a wait-all is not satisfied by part of its mask.
- **t = 1000 ms.** The 1 s timeout expires. The worker reports the missing bit and waits again.
- **t = 1500 ms.** The last bit arrives, and the worker wakes in the same tick. It has priority 3, so it runs before the link thread (priority 5) has even returned from `k_event_post()`. That is why `[worker]` prints right after `[link]`.
- **t = 1500…4500 ms.** 15 transfers, back to back. The red LED is on.
- **t = 4500 ms.** The link drops in the middle of transfer 15, which finishes at 4501 ms. That is §4 in action.
- **t = 6500 ms.** The link comes back and the gate reopens. The sensor and config bits were posted once, 6 s earlier, and are still there. An event group stores state.

## Experiments (Part A)

Change a value in `prj.conf`, rebuild, flash, and write down your prediction before you look.

### A1 · The order does not matter

Set `CONFIG_APP_SENSOR_INIT_MS=2500`, so the sensor is now the last to be ready.

*Prediction:* the timeouts report `missing: sensor link` at 1000 ms and `missing: sensor` at 2000 ms, and the gate opens at 2500 ms. A wait-all opens at the last condition, whichever it is. With semaphores you would need one take per condition, in a fixed order, and a condition that arrived "out of order" would have to be stored somewhere.

Set it back to 300.

### A2 · Wait-any by mistake

Uncomment `CONFIG_APP_GATE_WAIT_ANY=y`.

```
[init]   t=300 ms  sensor ready (posted once, never again)
[worker] t=300 ms  BUG: transfer started, missing: config link
[init]   t=800 ms  config ready (posted once, never again)
[worker] t=900 ms  BUG: transfer started, missing: link
[link]   t=1500 ms  up
[worker] t=1500 ms  gate open, the wait returned: sensor config link
[gate]   t=2000 ms  bits now: sensor config link  openings=1 transfers=8 early=6 timeouts=0 button=0
```

`k_event_wait()` returns as soon as **any** bit in the mask is set, so the worker starts transferring at 300 ms with neither configuration nor link. The `BUG` line is printed only when the set of missing bits changes, but `early` counts every bad transfer: 6 before the link comes up, and about 10 per report while the link is down. The second `BUG` line appears at 900 ms, not 800 ms, because the worker notices the change only at the start of its next transfer.

The two calls differ by four letters, and the compiler cannot help you. Comment the line out again.

### A3 · `reset = true` is not "clear on exit"

Uncomment `CONFIG_APP_GATE_RESET=y`. The worker now calls `k_event_wait_all(..., true, ...)`.

```
[init]   t=300 ms  sensor ready (posted once, never again)
[init]   t=800 ms  config ready (posted once, never again)
[worker] t=1000 ms  still waiting, missing: link
[link]   t=1500 ms  up
[worker] t=2000 ms  still waiting, missing: sensor config
[worker] t=3000 ms  still waiting, missing: sensor config link
[worker] t=4000 ms  still waiting, missing: sensor config link
...
```

(The `[gate]` report lines are left out above. They show `openings=0` and mostly `bits now: (none)`.)

*Explanation:* `reset = true` clears the tracked events **before** the thread starts waiting, every time it waits.

- At 1000 ms the worker times out and waits again, and that wait erases the sensor and config bits. Those are posted only once, so they never come back.
- At 2000 ms the next wait erases the link bit too.
- The gate never opens. When the link thread posts again at 6500 ms, that bit survives only until the next timeout.

`reset` is for one narrow case: "I only care about events that happen *from now on*". It is never right for state bits that other threads own. To consume an event after a wait, call `k_event_clear()` with the bits you got, as Part B does.

Comment it out again.

### A4 · `k_event_set()` instead of `k_event_post()`

Uncomment `CONFIG_APP_LINK_USE_SET=y`. The link thread now brings the link up with `k_event_set(&sys_events, EV_LINK_UP)`.

```
[init]   t=300 ms  sensor ready (posted once, never again)
[init]   t=800 ms  config ready (posted once, never again)
[worker] t=1000 ms  still waiting, missing: link
[link]   t=1500 ms  up
[worker] t=2000 ms  still waiting, missing: sensor config
[gate]   t=2000 ms  bits now: link  openings=0 transfers=0 early=0 timeouts=2 button=0
```

`k_event_set()` writes the whole 32-bit word, so `events = LINK_UP` and the other two bits are gone. One owner wiped two other owners' state. Use `k_event_post()` to set bits, `k_event_clear()` to clear them, and `k_event_set_masked()` when you need "these bits to these values, leave the rest alone".

Comment it out again.

### A5 · Unplug the cable

With the default configuration, wait for the gate to open (red LED on). Then press the button.

*Prediction:* the button ISR clears `EV_LINK_UP`. The current transfer finishes, the worker prints `gate closed, missing: link`, and the red LED goes off. Press again: the bit is posted, the waiting worker wakes at once, and the gate opens. The ISR changed a condition with two ISR-safe calls, and the worker needed no extra code to react. The link thread still runs its own cycle, so its next "up" or "down" overrides your last press.

---

# Part B · A wait-any dispatcher

Switch to Part B (see [Build](#build-flash-monitor)) before reading on.

## Code walkthrough

### 1 · One bit per source

```c
enum source { SRC_BUTTON, SRC_TICK, SRC_DATA, NUM_SOURCES };
#define EV_ALL_SOURCES (BIT(SRC_BUTTON) | BIT(SRC_TICK) | BIT(SRC_DATA))

static void post_source(enum source src)
{
	atomic_inc(&posted[src]);                 /* count first */
	k_event_post(&app_events, BIT(src));
}
```

These bits are **events**, not state: "something happened, please look". So the consumer clears them, unlike in Part A. The same function is called from the button ISR, the timer ISR and the producer thread. Counting before posting guarantees that a report never shows more handled than posted.

### 2 · Wait for any, clear what you got, then handle

```c
uint32_t got = k_event_wait(&app_events, EV_ALL_SOURCES, false, K_FOREVER);

k_event_clear(&app_events, got);              /* claim exactly these bits */

for (int src = 0; src < NUM_SOURCES; src++) {
	if (got & BIT(src)) {
		handle(src);                          /* 50 ms each */
	}
}
```

- `k_event_wait()` returns the bits that matched. If several sources fired before the dispatcher ran, they arrive together, and the report counts those as `with more than one bit`.
- The dispatcher clears **only the bits it got**. A bit of another source that arrives a moment later survives for the next wait.
- It clears **before** handling. A post that arrives during the handling sets the bit again, and the next wait returns at once for it. Experiment B2 reverses the order.

### 3 · What the report tells you

```
[report]   t=2000 ms  wakeups=8 (with more than one bit: 0)
[report]     button posted=0 handled=0 not handled=0
[report]     tick   posted=2 handled=2 not handled=0
[report]     data   posted=6 handled=6 not handled=0
```

`not handled` = posted − handled. For each source it can be 1 while a post is still waiting to be handled. Anything above that means posts **merged**: the bit was already set when the post came, so the second post changed nothing. An event group stores *whether* something happened, not *how many times*.

## Run it (Part B)

```
04_event_groups on stm32f769i_disco
Part B: dispatcher. Waits for ANY of: button tick data. Handling takes 50 ms per event
Sources: tick every 1000 ms, data every 300 ms (0 = off), button
[report]   t=2000 ms  wakeups=8 (with more than one bit: 0)
[report]     button posted=0 handled=0 not handled=0
[report]     tick   posted=2 handled=2 not handled=0
[report]     data   posted=6 handled=6 not handled=0
[report]   t=4000 ms  wakeups=17 (with more than one bit: 0)
[report]     button posted=0 handled=0 not handled=0
[report]     tick   posted=4 handled=4 not handled=0
[report]     data   posted=13 handled=13 not handled=0
```

Press the button a few times:

```
[dispatch] t=5212 ms  button press handled
```

and the red LED toggles. Handling takes 50 ms, much less than the gap between two posts of the same source, so nothing merges. At t ≈ 3000 ms the tick and a data post arrive about 1 ms apart. The tick wakes the dispatcher first, and the data bit, posted during the tick's handling, is picked up by the next wait. That is still two separate wakeups, each with one bit.

## Experiments (Part B)

### B1 · An event group does not count

Set `CONFIG_APP_TICK_PERIOD_MS=0` (only data now, for clear numbers) and `CONFIG_APP_HANDLE_MS=400`. Data is posted every 300 ms, but handling one takes 400 ms.

*Prediction:* the dispatcher is busy all the time and handles one data event every 400 ms, 5 per report. About 6.7 are posted per report, so `not handled` grows by about 1.7 per report:

```
[report]   t=2000 ms  wakeups=5 (with more than one bit: 0)
[report]     data   posted=6 handled=5 not handled=1
[report]   t=4000 ms  wakeups=10 (with more than one bit: 0)
[report]     data   posted=13 handled=10 not handled=3
[report]   t=6001 ms  wakeups=15 (with more than one bit: 0)
[report]     data   posted=19 handled=15 not handled=4
```

The dispatcher still runs at its full capacity. What it loses is the *number* of events: during each 400 ms handling, one or two posts set a bit that is already set. Compare tutorial 02 Part A. A counting semaphore would have stored the backlog, up to its limit. Use an event group when "it happened at least once since I last looked" is enough, such as "new data is ready, read whatever is there". Use a semaphore or a queue when every occurrence matters.

### B2 · Clear after handling

Keep B1's settings and uncomment `CONFIG_APP_CLEAR_AFTER_HANDLING=y`.

*Prediction:* the post that arrives during the 400 ms handling finds the bit still set, so it merges. The late `k_event_clear()` then erases it. The dispatcher waits for the *next* post, so it handles one event every 600 ms instead of every 400:

```
[report]   t=2000 ms  wakeups=3 (with more than one bit: 0)
[report]     data   posted=6 handled=3 not handled=3
[report]   t=4000 ms  wakeups=7 (with more than one bit: 0)
[report]     data   posted=13 handled=7 not handled=6
```

Half of the posts are now lost, and the dispatcher is idle a third of the time even though data is waiting. Clear first, then handle. Restore the defaults afterwards (`CONFIG_APP_TICK_PERIOD_MS=1000`, `CONFIG_APP_HANDLE_MS=50`, clear-after commented out).

### B3 · Broadcast: two threads, one post

Uncomment `CONFIG_APP_BUTTON_LOGGER=y`. A logger thread at priority 4 also waits for the button bit, and never clears it.

*Prediction:* each press prints two lines:

```
[dispatch] t=3105 ms  button press handled
[logger]   t=3110 ms  saw the button too (bits 0x1)
```

One `k_event_post()` made **both** waiters ready, because both conditions were met. A `k_sem_give()` would have woken only one thread. The logger sees the press even though the dispatcher clears the bit before the logger runs: its wait was satisfied at the moment of the post, and the post handed it the bits it needed.

The report's `logger wakeups` should equal `button posted`.

### B4 · A waiter that runs before the owner clears

Keep the logger and set `CONFIG_APP_LOGGER_PRIORITY=1`, which makes it more urgent than the dispatcher (2).

*Prediction:* everything works until the first press. Then:

```
[logger]   t=4021 ms  saw the button too (bits 0x1)
[logger]   t=4026 ms  saw the button too (bits 0x1)
[logger]   t=4032 ms  saw the button too (bits 0x1)
...
```

The logger runs first, prints, and waits again. The dispatcher has not run, so the bit is still set, and the wait returns at once. The logger now spins forever at priority 1. The dispatcher never runs, the report stops, and the heartbeat LED stops blinking, because `main` is starved too. Only RESET recovers.

The design rule from Lesson 07: **give each bit one owner for clearing**. Every other waiter must either be less urgent than the owner, or keep its own record of what it has already seen. Set the priority back to 4 and comment the logger out.

---

## Common mistakes

- **`k_event_wait()` where you meant `k_event_wait_all()`**, or the other way round (A2). Name the variables after the condition, such as `all_ready`, so a review catches it.
- **`k_event_set()` to set one bit.** It replaces all 32 (A4). Use `k_event_post()`.
- **Thinking `reset = true` means "clear the bits I waited for".** It clears the tracked events *before* the wait (A3). Consume bits with `k_event_clear(&ev, got)`.
- **Clearing state bits you do not own.** In Part A only the owner of a condition clears its bit. The worker only reads.
- **Clearing event bits after handling them** (B2), or **clearing all bits** instead of the ones you got. Either one erases posts that arrived meanwhile.
- **Expecting an event group to count** (B1). Repeated posts of a set bit merge. Use a semaphore or a queue when the number matters.
- **Several waiters, no clear owner** (B4). A waiter that does not clear and runs first will see the same bit again and again.
- **Forgetting `CONFIG_EVENTS=y`.** The build fails at `K_EVENT_DEFINE`.
- **Blocking on an event group in an ISR.** Posting, setting and clearing are fine in an ISR, but a wait there must use `K_NO_WAIT`.

## Check your understanding

1. In Part A, why does the worker not clear any bits after its wait?
   <details><summary>Answer</summary>They are conditions owned by other threads. The sensor and config bits are posted only once, so clearing them would close the gate for ever, which is what experiment A3 does indirectly. The link bit is cleared by its owner when the link really drops.</details>
2. A wait-all for bits 0–2 times out. How do you find out which bits were missing?
   <details><summary>Answer</summary>The wait returns 0 on timeout, so it does not tell you. Read the current bits with <code>k_event_test(&amp;ev, mask)</code> and compute <code>mask &amp; ~current</code>, as the worker does. The bits may change between the timeout and the test, so the result is a snapshot.</details>
3. Three interrupts post the same bit before the dispatcher runs. How many times is it handled, and how would you make it three?
   <details><summary>Answer</summary>Once: the three posts set an already-set bit. To handle each occurrence, count them with a counting semaphore (tutorial 02), or pass them through a message queue (tutorial 06).</details>
4. In B2, why does the dispatcher handle *fewer* events, even though it is idle part of the time?
   <details><summary>Answer</summary>The post that arrives during handling merges with the bit that is still set, and the late clear erases it. The dispatcher then waits for the next post, about 200 ms later, while data was in fact ready all along.</details>
5. What is the difference between B3 and B4? Only one number changed.
   <details><summary>Answer</summary>The order in which the two woken threads run. At priority 4, the dispatcher runs first and clears the bit, so the logger's next wait blocks. At priority 1, the logger runs first, waits again while the bit is still set, and returns at once, for ever. The clearing owner must run before any other waiter waits again.</details>

## Zephyr APIs used

`CONFIG_EVENTS`, `K_EVENT_DEFINE`, `k_event_post`, `k_event_set`, `k_event_clear`, `k_event_wait`, `k_event_wait_all`, `k_event_test`, `K_TIMER_DEFINE`, `k_timer_start`, `K_THREAD_DEFINE` (with `SYS_FOREVER_MS`), `k_thread_start`, `k_thread_name_set`, `k_msleep`, `k_uptime_get`, `K_MSEC`, `K_FOREVER`, `irq_lock`, `irq_unlock`, `atomic_inc`, `atomic_get`, `IS_ENABLED`, `INT_TO_POINTER`, `POINTER_TO_INT`, `BIT`, plus the GPIO calls from tutorial 01

## Next

**05 · Task notifications:** Zephyr has no dedicated task-notification API. The next tutorial builds the usual equivalents for one-to-one signalling, and compares their cost with the event group used here.
