# 07 · Deadlock: reproduce it, diagnose it, design it out

Two tasks, two mutexes, opposite order. That is the whole recipe for the most common deadlock in firmware, and it can pass every bench test because the window is a few milliseconds wide. This tutorial builds Lesson 10's worked example: a logger and a display share a file system and an SPI bus. It reproduces the deadlock on purpose, diagnoses it three ways (the program's own lock report, the kernel shell, and a task watchdog), and then fixes it three ways, each one breaking a different Coffman condition.

**Board:** STM32F769I-DISCO · **RTOS:** Zephyr (3.7.99 in this workspace) · **Time:** about 60 minutes

## Learning objectives

When you finish, you will be able to:

- Reproduce a two-mutex lock-order deadlock, and explain from the timeline why it happens only in a narrow window.
- Name the four Coffman conditions in the running code.
- Diagnose a hang with a wait-for graph, the kernel shell's thread list, and a task watchdog, and say what each can and cannot tell you.
- Fix the deadlock by a global lock order, by a timeout with release and back-off, and by a gatekeeper thread, and compare their costs.
- Check a lock order with an assertion, which finds the bug without waiting for the deadlock to happen.

## Theory to read first

- [Lesson 10 · Mutual exclusion and deadlock](../resources/webpages/10_mutual_exclusion_and_deadlock.html): the Coffman conditions, one fix per condition, and the worked example this tutorial builds
- [Lesson 06 · Mutexes](../resources/webpages/06_mutexes.html) and tutorial 03, for `k_mutex` itself

## What you will build

```
  display (prio 4), every 50 ms:              logger (prio 5), every 100 ms from 25 ms,
                                                               and once per button press:
     lock spi_lock                               lock fs_lock
     stream half a frame        5 ms             open the file         2 ms
     lock fs_lock   ◄── glyph missing            lock spi_lock  ◄── the flash driver
     read the glyph             1 ms             write the record      2 ms
     unlock fs_lock                              unlock spi_lock
     stream the other half      5 ms             unlock fs_lock
     unlock spi_lock

                 display: SPI → FS          logger: FS → SPI          ← opposite order
```

`prj.conf` selects one of four versions of the display's frame: no fix (Part A), or one of three fixes (Part B). The logger is the same in all of them, except with the gatekeeper.

| Board item | Devicetree alias | Use |
|------------|------------------|-----|
| Blue USER button B1 (PA0) | `sw0` | "Log this event now": an extra log write at a random moment |
| Red LED LD1 (PJ13) | `led0` | Not used |
| Green LED LD2 (PJ5) | `led1` | Heartbeat: it keeps blinking during a deadlock, because main is not involved |
| ST-LINK virtual COM port (USART1) | `zephyr,console` | Console and shell, 115200 baud, 8N1 |

## Files

| File | Purpose |
|------|---------|
| [`CMakeLists.txt`](CMakeLists.txt) | Builds `main.c` and `fs_spi.c` |
| [`Kconfig`](Kconfig) | The fix choice, the timing, the experiment switches |
| [`prj.conf`](prj.conf) | GPIO, priorities, the kernel shell, the task watchdog, assertions, option values |
| [`src/main.c`](src/main.c) | Common part, the same as tutorials 04 to 06 |
| [`src/demo.h`](src/demo.h) | The three functions the demo provides |
| [`src/fs_spi.c`](src/fs_spi.c) | The two tasks, the locks with bookkeeping, the three fixes, the watchdog |

## Build, flash, monitor

From the workspace root (`/workspace` in the dev container):

```bash
west build -b stm32f769i_disco -p auto apps/RTOS_Synchronization_primitives/07_deadlock \
           -d apps/RTOS_Synchronization_primitives/07_deadlock/build
```

If `west build` reports `unknown command "build"`, use CMake as in tutorial 01:

```bash
cd apps/RTOS_Synchronization_primitives/07_deadlock
cmake -GNinja -B build -DBOARD=stm32f769i_disco .
ninja -C build
```

**Choose the fix** in `prj.conf`: uncomment exactly one `CONFIG_APP_FIX_*` line, or pass it on the command line, for example `-DCONFIG_APP_FIX_TIMEOUT=y`. After changing the choice, rebuild from scratch (`-p always`, or delete `build/`).

This build includes the **shell**, so it is larger than the earlier ones. The console and the shell share the virtual COM port. After the start-up banner, a `uart:~$` prompt appears, and the reports are printed between your typing. You can type even while reports scroll.

Flash and monitor as before: `west flash -d build`, or copy `build/zephyr/zephyr.bin` onto the `DIS_F769NI` drive. Then open the virtual COM port at 115200 baud, in a terminal that sends each key as you type it (PuTTY, Tera Term, `minicom`, `screen`), and press RESET.

> **The shell runs on the board, not on your PC.** `uart:~$` is the prompt printed by the firmware over the virtual COM port. You type its commands into the same serial terminal that shows the reports, not into PowerShell or the build container. Any terminal that can *send* works: PuTTY or Tera Term (Serial, COMx, 115200), or VS Code's *Serial Monitor* extension. In Serial Monitor, set **Line ending to CR** (or CRLF). With the default *None*, your command reaches the board, but the shell never receives Enter and nothing happens. To test, send an empty line: a new `uart:~$` prompt should appear. Then send `help`.

> **About the expected output.** This tutorial has not been built or run yet. The outputs below were derived by hand from the code and the 10 kHz tick: each relative sleep or timeout lasts one tick (0.1 ms) longer than requested. Times may differ by a millisecond, and wait times by a tick. The shell output is abbreviated and its exact format depends on the Zephyr version. Correct this README once the board has run it.

---

# Part A · The deadlock

## Code walkthrough

### 1 · Locks that keep a record

```c
struct app_lock {
	struct k_mutex mutex;
	const char *name;
	uint8_t level;               /* lock order: take lower levels first */
	struct worker *owner;        /* bookkeeping for the report */
	uint32_t max_wait_us;
};

static int lock(struct worker *w, struct app_lock *l, k_timeout_t timeout)
{
	w->waiting_for = l;                          /* edge "w waits for l"  */
	int ret = k_mutex_lock(&l->mutex, timeout);
	w->waiting_for = NULL;
	if (ret == 0) {
		l->owner = w;                            /* edge "l held by w"    */
		...
```

The kernel does not detect deadlocks, and it has no public call that says who owns a mutex or what a thread waits for. So every lock in this program goes through `lock()` and `unlock()`, which record the two kinds of edge in Lesson 10's resource-allocation graph: *held by* and *waits for*. The report prints the graph, and calls a cycle a deadlock. The wrapper also measures how long each lock took, and in Part B it checks the lock order.

### 2 · The two tasks

```c
/* display, every 50 ms */                    /* logger, every 100 ms */
lock(w, &spi_lock, K_FOREVER);                lock(w, &fs_lock, K_FOREVER);
spi_transaction(w, 5);                        k_msleep(2);              /* open */
lock(w, &fs_lock, K_FOREVER);   /* SPI→FS */  lock(w, &spi_lock, K_FOREVER);   /* FS→SPI */
k_msleep(1);                                  spi_transaction(w, 2);    /* write */
unlock(w, &fs_lock);                          unlock(w, &spi_lock);
spi_transaction(w, 5);                        unlock(w, &fs_lock);
unlock(w, &spi_lock);
```

All four Coffman conditions hold:

1. **Mutual exclusion:** each mutex has one owner at a time.
2. **Hold and wait:** each task holds its first lock while it waits for its second.
3. **No preemption:** the kernel never takes a mutex away from its owner.
4. **Circular wait:** possible, because the two tasks take the locks in opposite order.

### 3 · The window

The deadlock needs the logger to take `fs_lock` while the display is in the first half of its frame (holding SPI, not yet asking for FS), or the display to take `spi_lock` while the logger is opening its file (holding FS, not yet asking for SPI). Each frame has a window of about **7 ms** in which a log write that starts will deadlock: 5 ms before the glyph, plus 2 ms before the frame.

With the default timing, the frames start at 0, 50, 100, … ms and the scheduled log writes at 25, 125, 225, … ms. Every write starts halfway between two frames and is finished 4 ms later, long before the next frame. **The scheduled writes never deadlock, however long you run it.** The bug is still there, waiting for a write at the wrong moment.

### 4 · The button: a write at the wrong moment

```c
if (k_sem_take(&log_now, K_TIMEOUT_ABS_MS(next)) == 0) {
	button_writes++;                         /* an extra, unscheduled write */
}
```

The logger sleeps until its next scheduled write, or until the button gives `log_now`. A press comes at a random moment, and lands in the window with a probability of about 7 ms / 50 ms ≈ **1 in 7**. On the bench, a deadlock like this one may need an unusual event at an unlucky moment, and that is why it passes the tests and appears in the field.

### 5 · The task watchdog

```c
task_wdt_init(NULL);                          /* software only, no hardware watchdog */
display.wdt_channel = task_wdt_add(1000, wdt_expired, &display);
...
display_frame(&display);
task_wdt_feed(display.wdt_channel);           /* after each completed frame */
```

Each task has its own watchdog channel and feeds it after every **completed** cycle. If a task stops finishing its cycles for 1 s, for any reason, `wdt_expired()` runs and reports it. On a product, the callback would reset the system, and a hardware watchdog (the STM32's IWDG) would stand behind the task watchdog in case the whole CPU hangs. Experiment A4 turns the reset on.

## Run it (Part A)

```
07_deadlock on stm32f769i_disco
Fix: none (display SPI -> FS, logger FS -> SPI)
Display: a frame every 50 ms. Logger: a record every 100 ms from 25 ms, and one per button press
uart:~$
[report] t=2000 ms  frames=40 writes=20 (button 0)  backoffs=0  max wait: fs_lock 0 us, spi_lock 0 us
[report]   fs_lock  free
[report]   spi_lock held by display
[report] t=4000 ms  frames=80 writes=40 (button 0)  backoffs=0  max wait: fs_lock 0 us, spi_lock 0 us
[report]   fs_lock  free
[report]   spi_lock held by display
```

40 frames and 20 log records every 2 s, and no task ever waited for a lock. `spi_lock held by display` is a snapshot: at the moment of the report, the frame that started at 2000 ms is being streamed.

## Experiments (Part A)

### A1 · Reproduce it

Press the button, wait for the next report, and repeat. Most presses add one to `button` and change nothing else, although `max wait` may grow: a write that lands later in a frame waits for the display, without a deadlock. After a few presses (on average 7), the counts freeze:

```
[report] t=8000 ms  frames=128 writes=65 (button 4)  backoffs=0  max wait: fs_lock 1100 us, spi_lock 5000 us
[report]   fs_lock  held by logger, waited for by display
[report]   spi_lock held by display, waited for by logger
[report]   DEADLOCK: display waits for fs_lock (held by logger), logger waits for spi_lock (held by display)
[wdt]    t=8130 ms  logger has not finished a cycle for 1000 ms
```

(Your numbers depend on when you pressed. These are an example.)

- `frames` and `writes` stop for good. Both tasks are blocked, and only RESET recovers.
- The report draws the cycle: display → `fs_lock` → logger → `spi_lock` → display.
- `max wait` does not grow any more. The two waits that matter never end, so they are never measured. A timing statistic cannot show a deadlock.
- The heartbeat keeps blinking and the reports keep coming, because main uses neither lock. Only the two tasks are dead. A system can look alive while part of it has stopped.
- The task watchdog reports the task whose channel was fed least recently, about 1 s after its last completed cycle. Whether a second `[wdt]` line follows for the other task depends on how your Zephyr version re-arms the watchdog timer after a callback.

### A2 · Diagnose it with the kernel shell

Your own lock bookkeeping is the best diagnosis, but a real hang often happens in code without it. With the deadlock from A1 in place, type at the `uart:~$` prompt:

```
uart:~$ kernel thread list
```

(Type it in the serial terminal and end it with Enter. In VS Code's Serial Monitor that needs *Line ending: CR*, see [Build, flash, monitor](#build-flash-monitor). If the shell says the command is unknown, your tree uses the older name: `kernel threads`. Press Tab after `kernel ` to list the subcommands.) Abbreviated, you should see:

```
Threads:
 0x2000….  logger
	options: 0x0, priority: 4 timeout: 0
	state: pending, entry: 0x….
	...
 0x2000….  display
	options: 0x0, priority: 4 timeout: 0
	state: pending, entry: 0x….
	...
*0x2000….  shell_uart
	...
 0x2000….  main
	options: 0x1, priority: 7 timeout: …
	state: suspended, entry: ...  (or sleeping)
```

What the shell tells you:

- **Both tasks are `pending`**: blocked on a kernel object, with no timeout (`timeout: 0`). A sleeping thread would have a timeout counting down. A thread that is pending for ever, and stays that way from one listing to the next, is the first sign of a deadlock.
- **The logger's priority is 4, not 5.** The display (4) waits for `fs_lock`, which the logger holds, so priority inheritance (tutorial 03, Lesson 11) has raised the logger to 4. An unexpected priority is a hint about who holds what.

What it does not tell you: **which object** each thread waits for. You still have to find the cycle, from the code, from your own bookkeeping, or with a debugger. Read `thread->base.pended_on` and each mutex's `owner` field in GDB. Those are kernel internals, fine for debugging but not for the application to use.

### A3 · Make it deterministic

Set `CONFIG_APP_LOGGER_OFFSET_MS=2`. Every scheduled write now starts 2 ms after a frame, in the middle of the window.

*Prediction:*

```
[report] t=2000 ms  frames=0 writes=0 (button 0)  backoffs=0  max wait: fs_lock 0 us, spi_lock 0 us
[report]   fs_lock  held by logger, waited for by display
[report]   spi_lock held by display, waited for by logger
[report]   DEADLOCK: display waits for fs_lock (held by logger), logger waits for spi_lock (held by display)
[wdt]    t=1000 ms  display has not finished a cycle for 1000 ms
```

(The `[wdt]` line appears before the first report.)

At 0 ms the display takes SPI. At 2 ms the logger takes FS, which is free. At 4.1 ms the logger asks for SPI and blocks. At 5.1 ms the display asks for FS and blocks. Neither task ever completes a cycle. The watchdog fires at 1000 ms for the display, whose channel was added first. Keep this setting for A4 and for all of Part B. It is the test that proves a fix.

### A4 · Recovery is not a fix

Keep `CONFIG_APP_LOGGER_OFFSET_MS=2` and uncomment `CONFIG_APP_WDT_REBOOT=y`.

*Prediction:*

```
07_deadlock on stm32f769i_disco
...
[wdt]    t=1000 ms  display has not finished a cycle for 1000 ms
[wdt]    rebooting

07_deadlock on stm32f769i_disco
...
[wdt]    t=1000 ms  display has not finished a cycle for 1000 ms
[wdt]    rebooting
...
```

The watchdog turns a silent hang into a reset, about once a second here. In the field, with the rare button case, that might be a reset once a week, which is far better than a dead product. But it is a safety net. The deadlock is still in the design, and every reset loses the work in progress. Lesson 10: use detection and recovery as a safety net, not as the design. Comment the line out again.

### A5 · Find it without waiting for it

Set `CONFIG_APP_LOGGER_OFFSET_MS=25` again (no collisions), and uncomment `CONFIG_APP_CHECK_LOCK_ORDER=y`. Every lock now checks that the thread holds no lock of the same or a higher level: `fs_lock` is level 1, and `spi_lock` is level 2.

```c
__ASSERT((w->held_levels >> l->level) == 0,
	 "lock order: %s takes %s while holding %s", ...);
```

*Prediction*, at the very first frame:

```
ASSERTION FAIL [(w->held_levels >> l->level) == 0] @ WEST_TOPDIR/apps/RTOS_Synchronization_primitives/07_deadlock/src/fs_spi.c:88
	lock order: display takes fs_lock while holding spi_lock
```

The program has never deadlocked, and with this timing it never would by itself, yet the assertion finds the bug 5 ms after start. It checks the **order**, which is wrong on every frame, instead of waiting for the unlucky **timing**. This is how Lesson 10's advice "prove the absence by design" is enforced in a debug build. Fix B1 keeps the check on. Comment the line out again.

---

# Part B · Three fixes

Keep `CONFIG_APP_LOGGER_OFFSET_MS=2` from A3: every log write collides with a frame, so a fix that works here works at every timing. Select one fix at a time, and rebuild from scratch.

## B1 · One lock order for everyone (breaks circular wait)

`CONFIG_APP_FIX_LOCK_ORDER=y`. The order is FS (level 1) before SPI (level 2), everywhere. The display now loads the glyph *before* the frame, then streams the frame holding SPI only:

```c
lock(w, &fs_lock, K_FOREVER);                  /* FS ...                    */
lock(w, &spi_lock, K_FOREVER);                 /* ... then SPI: the flash   */
k_msleep(CONFIG_APP_GLYPH_MS);                 /* read the glyph            */
unlock(w, &spi_lock);
unlock(w, &fs_lock);
lock(w, &spi_lock, K_FOREVER);                 /* the frame: SPI only       */
spi_transaction(w, 5);
spi_transaction(w, 5);
unlock(w, &spi_lock);
```

*Prediction:*

```
Fix: B1, lock order FS -> SPI everywhere, lock order checked
...
[report] t=2000 ms  frames=40 writes=20 (button 0)  backoffs=0  max wait: fs_lock 0 us, spi_lock 7200 us
[report]   fs_lock  free
[report]   spi_lock held by display
```

No deadlock, and the lock-order assertion stays silent. The logger still collides with every frame. It takes FS at 2 ms (the display released it at 1.1 ms), asks for SPI at 4.1 ms, and waits until the frame ends at 11.3 ms: that is the **7.2 ms** wait. Waiting is not deadlock. The display needs nothing the logger holds, so the chain ends.

*Cost:* almost nothing at run time. It takes discipline, and the glyph is now read on every frame, even when it is in a cache, unless the code checks first. Lesson 10 calls this the cheapest fix and the one to prefer.

## B2 · Timeout, release, back off (breaks no preemption)

`CONFIG_APP_FIX_TIMEOUT=y`. The display waits for FS for at most 10 ms. On timeout, it gives up SPI, the lock it holds, waits 3 ms, and redoes the frame:

```c
while (1) {
	lock(w, &spi_lock, K_FOREVER);
	spi_transaction(w, 5);
	if (lock(w, &fs_lock, K_MSEC(10)) == 0) {
		break;                       /* got both: finish the frame */
	}
	unlock(w, &spi_lock);            /* release what we hold ...           */
	backoffs++;
	k_msleep(3);                     /* ... let the other one finish ...   */
}                                    /* ... and redo the half frame        */
```

*Prediction:*

```
Fix: B2, timeout, release and back off
...
[report] t=2000 ms  frames=40 writes=20 (button 0)  backoffs=20  max wait: fs_lock 10100 us, spi_lock 11100 us
```

The deadlock still forms, once per log write, but it lasts only 10 ms:

- at 5.1 ms the display asks for FS and waits;
- at 15.2 ms it times out and releases SPI. The logger takes SPI and finishes its write at 17.3 ms;
- the display retries at 18.3 ms and finishes the frame at 29.6 ms, well before the next one at 50 ms.

*Cost:* `backoffs=20`. On every collision the first half frame is streamed twice, and the frame is 18 ms late. The display must be able to *undo* its partial work, here by redrawing. If both tasks backed off with the same delay, they could retry in lockstep and collide for ever: **livelock**. That is why the back-off should be random or depend on the priority, and why only one side backs off here.

## B3 · An SPI gatekeeper (breaks mutual exclusion on the bus)

`CONFIG_APP_FIX_GATEKEEPER=y`. A thread at priority 3 owns the SPI bus. The display and the logger send it requests through a `k_msgq` (tutorial 06) and wait for the reply on their own semaphore (tutorial 05). Nobody locks the bus:

```c
static void spi_transaction(struct worker *w, int ms)
{
	struct spi_request req = { .duration_ms = ms, .done = &w->spi_done };

	k_msgq_put(&spi_requests, &req, K_FOREVER);
	k_sem_take(&w->spi_done, K_FOREVER);
}
```

*Prediction:*

```
Fix: B3, SPI gatekeeper thread
...
[report] t=2000 ms  frames=40 writes=20 (button 0)  backoffs=0  max wait: fs_lock 2100 us, spi_lock 0 us
[report]   gatekeeper: 140 transactions, at most 1 request(s) queued
[report]   fs_lock  free
[report]   spi_lock not used: the gatekeeper owns the bus
```

- The logger still **holds FS while it waits** for the gatekeeper. That is hold and wait. But the gatekeeper never waits for FS, so no cycle can close.
- At 5.1 ms the display wants FS for the glyph and waits **2.1 ms**, until the logger's write is done.
- 140 transactions = 40 frames × 3 + 20 writes. Once, per collision, a request had to queue behind another.

*Cost:* an extra thread with its stack, a queue, two context switches per transaction, and each transaction is now separate. Another task's transaction can come between the two halves of a frame, which an LCD must tolerate. In return, no task can deadlock on the bus, however the code grows. Lesson 10: the most robust fix as a system grows.

Restore `CONFIG_APP_LOGGER_OFFSET_MS=25` and `CONFIG_APP_FIX_NONE=y` when you are done.

---

## Common mistakes

- **Taking locks in different orders in different places** (Part A). Define one order, document it, and check it with an assertion (A5).
- **Calling code that takes locks while holding one.** The glyph read and the flash driver hide their locks inside. Every call made while holding a lock is part of the lock order.
- **Waiting for ever on every lock.** An infinite wait turns a design error into a silent hang. A bounded wait that reports failure makes it visible in testing, and enables B2.
- **Relying on testing.** With the default timing, A1 needs an unusual event at an unlucky moment. The order check (A5) does not.
- **Fixing it with a delay.** Shifting the logger by a few milliseconds, as the default offset of 25 does, only moves the window. A1 still hits it.
- **Treating the watchdog as the fix** (A4). It recovers from a hang; it does not prevent one.
- **Backing off in lockstep** (B2). Two tasks that time out and retry with the same delay can livelock.
- **Forgetting non-mutex waits.** A wait for a gatekeeper's reply, a queue, or a semaphore is also a "waits for" edge. B3 is safe only because the gatekeeper itself never waits for FS.

## Check your understanding

1. In Part A, which Coffman condition does each fix remove?
   <details><summary>Answer</summary>B1 removes circular wait: with one order, a cycle cannot form. B2 removes "no preemption": the display gives up the lock it holds when it cannot get the next one. B3 removes mutual exclusion on the bus as a lock: no task holds the SPI bus, the gatekeeper owns it. Hold and wait is still present in B1 (FS while getting SPI) and B3 (FS while waiting for the gatekeeper), and that is fine.</details>
2. With the default timing, the program ran for an hour without a deadlock. Is it correct?
   <details><summary>Answer</summary>No. The scheduled writes never land in the 7 ms window, but any write at another moment can (A1). Testing showed the timing was lucky, not that the design is safe. The order assertion (A5) shows the design is wrong, at the first frame.</details>
3. The kernel shell shows two threads `pending` with `timeout: 0`. Is that a deadlock?
   <details><summary>Answer</summary>Not necessarily. Both could be waiting for something that will come, such as a semaphore given by an interrupt. It is a deadlock if they stay pending and each waits for something only the other can provide. The shell does not show the objects. You need the code, your own bookkeeping, or a debugger.</details>
4. In B1, the logger waits 7.2 ms for SPI on every write. Why is that not a problem, when a wait is half of a deadlock?
   <details><summary>Answer</summary>The display, which holds SPI, needs nothing the logger holds. It finishes its frame and releases SPI. A deadlock needs a cycle of waits; a chain that ends is just contention.</details>
5. Why does the watchdog channel get fed after each completed cycle, not at the start of each cycle?
   <details><summary>Answer</summary>Feeding at the start proves only that the task woke up. Feeding at the end proves that it got through its locks and its work. A task that wakes and then blocks for ever inside the cycle must stop feeding.</details>

## Zephyr APIs used

`k_mutex_init`, `k_mutex_lock` (with `K_FOREVER` and with a timeout), `k_mutex_unlock`, `K_MSGQ_DEFINE`, `k_msgq_put`, `k_msgq_get`, `k_msgq_num_used_get`, `k_sem_init`, `K_SEM_DEFINE`, `k_sem_give`, `k_sem_take`, `K_TIMEOUT_ABS_MS`, `k_sleep`, `k_msleep`, `k_uptime_ticks`, `k_ticks_to_us_floor32`, `__ASSERT`, `CONFIG_TASK_WDT`: `task_wdt_init`, `task_wdt_add`, `task_wdt_feed`; `sys_reboot`, `CONFIG_SHELL`, `CONFIG_KERNEL_SHELL`, `CONFIG_THREAD_MONITOR`, `K_THREAD_DEFINE` (with `SYS_FOREVER_MS`), `k_thread_start`, `k_thread_name_set`, `irq_lock`, `irq_unlock`, plus the GPIO calls from tutorial 01

## Next

**08 · Priority inversion:** in A2 the logger's priority went up from 5 to 4 while the display waited for it. That was priority inheritance at work. The next tutorial removes it, and shows the unbounded inversion it prevents.
