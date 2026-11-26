# 03 · Mutexes: one bus, two threads, and the rules of ownership

Tutorials 01 and 02 used semaphores to *signal*: an event happened, or a buffer is free. This tutorial protects shared state, which is the other big job of synchronization. Two sensor threads share one (simulated) I2C bus. You will see transactions tear apart when nothing protects the bus, and see the same code deadlock itself when a semaphore is used as the lock. Finally a `k_mutex` gets it right. A short "ownership tour" at startup shows each mutex rule as a return code.

**Board:** STM32F769I-DISCO · **RTOS:** Zephyr (3.7.99 in this workspace) · **Time:** about 60 minutes

## Learning objectives

When you finish, you will be able to:

- Identify a critical section that spans several calls, and protect it with `k_mutex_lock()` / `k_mutex_unlock()`.
- Explain mutex ownership: only the owner may unlock (`-EPERM`), and an unlocked mutex cannot be unlocked (`-EINVAL`).
- Use timeouts on a lock, and tell `-EBUSY` (`K_NO_WAIT`) from `-EAGAIN` (timeout expired).
- Explain recursive locking, and why it lets a function that holds the lock call another function that takes it again.
- Explain why a binary semaphore is the wrong tool for mutual exclusion: no owner, no recursion, no priority inheritance.
- Keep a critical section small, and recognise a missing unlock from the symptoms it causes.

## Theory to read first

- [Lesson 06 · Mutexes](../resources/webpages/06_mutexes.html): critical sections, ownership, recursive locking, and mutex vs. binary semaphore
- [Lesson 10 · Mutual exclusion and deadlock](../resources/webpages/10_mutual_exclusion_and_deadlock.html): the self-deadlock in experiment 2

## What you will build

```
   accel thread (prio 4, every 10 ms)          temp thread (prio 6, every 2 ms)
   bus_lock()                  ◄── outer       bus_read_reg(TEMP)
     bus_read_reg(X) ─┐                          │
     bus_read_reg(Y) ─┼─ each locks again        │
     bus_read_reg(Z) ─┘   (nested)               │
   bus_unlock()                                  │
             │                                   │
             ▼                                   ▼
        bus_lock() / bus_unlock()   = k_mutex | binary k_sem | nothing   (prj.conf)
             │
             ▼
        bus_transaction(): 3 steps of 100 µs  (START+addr, register, read+STOP)
        simulated devices tag each value with the address and register it came from
```

The accelerometer must get X, Y and Z from the *same* moment, so it holds the lock across all three reads. Each read also locks, because `bus_read_reg()` is a public function that must be safe on its own. That nested lock is the trap a semaphore falls into.

| Board item | Devicetree alias | Use |
|------------|------------------|-----|
| Blue USER button B1 (PA0) | `sw0` | Prints a report at once |
| Red LED LD1 (PJ13) | `led0` | Toggles on every bad (torn) read |
| Green LED LD2 (PJ5) | `led1` | Heartbeat |
| ST-LINK virtual COM port (USART1) | `zephyr,console` | 115200 baud, 8N1 |

As before, the board defines all four, so there is no devicetree overlay.

## Files

| File | Purpose |
|------|---------|
| [`CMakeLists.txt`](CMakeLists.txt) | Builds the four source files |
| [`Kconfig`](Kconfig) | The lock choice and the tutorial options |
| [`prj.conf`](prj.conf) | GPIO, priorities, assertions, option values |
| [`src/app.h`](src/app.h) | Interfaces between the files, device and register addresses |
| [`src/main.c`](src/main.c) | Button, LEDs, heartbeat, report; runs the tour, starts the sensors |
| [`src/bus.c`](src/bus.c) | The simulated bus and the lock that protects it |
| [`src/sensors.c`](src/sensors.c) | The two sensor threads and their statistics |
| [`src/ownership_tour.c`](src/ownership_tour.c) | The startup tour of mutex rules |

## Build, flash, monitor

From the workspace root (`/workspace` in the dev container):

```bash
west build -b stm32f769i_disco -p auto apps/RTOS_Synchronization_primitives/03_mutexes \
           -d apps/RTOS_Synchronization_primitives/03_mutexes/build
```

If `west build` reports `unknown command "build"`, use CMake as in tutorial 01:

```bash
cd apps/RTOS_Synchronization_primitives/03_mutexes
cmake -GNinja -B build -DBOARD=stm32f769i_disco .
ninja -C build
```

**Choose the lock** in `prj.conf` by uncommenting exactly one of `CONFIG_APP_LOCK_MUTEX=y` (default), `CONFIG_APP_LOCK_SEMAPHORE=y` or `CONFIG_APP_LOCK_NONE=y`. For a one-off build, add for example `-DCONFIG_APP_LOCK_NONE=y` to the `cmake` line.

Expected memory use with the default configuration: about 31.7 KB flash and 8.6 KB RAM.

Flash and monitor as in tutorial 01: `west flash -d build`, or copy `build/zephyr/zephyr.bin` onto the `DIS_F769NI` drive. Then open the virtual COM port at 115200 baud and press RESET.

---

## Code walkthrough

### 1 · The critical section spans several steps

```c
static int bus_transaction(uint8_t dev, uint8_t reg, int16_t *value)
{
	if (bus.busy) { collided = true; }       /* step 1: START + address */
	bus.busy = true;
	bus.addr = dev;
	bus_step();                               /* k_busy_wait(100 µs) */

	bus.reg = reg;                            /* step 2: register address */
	bus_step();

	bus_step();                               /* step 3: read + STOP */
	struct reading r = sim_device_read(bus.addr, bus.reg);
	bus.busy = false;
	...
	if (collided || r.addr != dev || r.reg != reg) { return -EIO; }
}
```

The device answers for whatever `bus.addr` and `bus.reg` say *at step 3*. If another thread runs a transaction in between, the first thread gets the other thread's data. The simulated devices tag every value with its source, so the code can detect a torn read exactly. A real bus would just return plausible-looking garbage.

This models a real problem. Zephyr's I2C drivers lock the bus inside one `i2c_transfer()` call, but a *sequence* of calls, such as `i2c_write()` of a register address followed by `i2c_read()`, is not protected. The application has to lock that sequence itself.

`k_busy_wait()` keeps the CPU busy, as a polled driver would. A higher-priority thread that wakes up during a step preempts the transaction in the middle.

### 2 · One lock, three implementations

```c
int bus_lock(void)
{
#if defined(CONFIG_APP_LOCK_MUTEX)
	return k_mutex_lock(&bus_mutex, K_MSEC(CONFIG_APP_LOCK_TIMEOUT_MS));
#elif defined(CONFIG_APP_LOCK_SEMAPHORE)
	return k_sem_take(&bus_sem, K_MSEC(CONFIG_APP_LOCK_TIMEOUT_MS));
#else
	return 0;
#endif
}
```

Both calls return `0` on success and `-EAGAIN` when the timeout expires. In normal use the two locks look interchangeable. The difference shows up in the details: `k_mutex_unlock()` checks the caller and returns an error, while `k_sem_give()` accepts a give from anyone, and so it cannot report a bug.

### 3 · Lock exactly the transaction, with one exit point

```c
int bus_read_reg(uint8_t dev, uint8_t reg, int16_t *value)
{
	int ret = bus_lock();
	if (ret != 0) {
		return ret;             /* did not get the lock: nothing to unlock */
	}
	ret = bus_transaction(dev, reg, value);
	bus_unlock();               /* one exit point: no path can skip it */
	return ret;
}
```

The lock covers the three bus steps and nothing else. The caller processes the value *after* the unlock. Every path that took the lock reaches the single `bus_unlock()`.

### 4 · A nested lock for an atomic X/Y/Z sample

```c
static int accel_read_sample(int16_t xyz[3])
{
	int ret = bus_lock();                         /* outer: lock count 1 */
	...
	for (int i = 0; i < 3; i++) {
		ret = bus_read_reg(DEV_ACCEL, regs[i], &xyz[i]);  /* inner: count 2, then back to 1 */
	}
	bus_unlock();                                 /* count 0: the bus is free */
}
```

A `k_mutex` records its owner and a lock count. When the owner locks again, the count goes up and the call returns at once. Each unlock decrements the count, and the mutex is free only when the count reaches 0. A semaphore has no owner, so the inner take just waits for the count to become 1, and the only thread that could give it is the one that is waiting.

### 5 · Priorities

| Thread | Priority | Work |
|--------|----------|------|
| `other` (tour only) | 1 | One mutex or semaphore call, then exits |
| `accel` | 4 | Lock, 3 reads (≈ 900 µs), unlock, sleep 10 ms |
| `temp` | 6 | 1 read (≈ 300 µs), sleep 2 ms |
| `main` | 7 | Heartbeat, report |

`accel` is more urgent, so it can preempt `temp` in the middle of a transaction. That is what makes the unprotected version fail. With the mutex, when `accel` blocks on a mutex that `temp` holds, Zephyr temporarily raises `temp` to priority 4 (**priority inheritance**) so that it finishes its 300 µs quickly. Tutorial 08 is about that mechanism.

The sensor threads are defined with `K_THREAD_DEFINE(..., SYS_FOREVER_MS)`: they exist from boot but do not start until `sensors_start()` calls `k_thread_start()`, after the tour.

### 6 · The ownership tour

`ownership_tour()` runs once in `main`. To act as a second thread, it creates a short-lived thread `other` at priority 1 for each step, runs one call in it, and joins it. Each line prints the call, its return code, and what that means.

## Run it

```
03_mutexes on stm32f769i_disco

--- Ownership tour: k_mutex ---
  main   unlock (mutex is not locked)       -> -EINVAL  nothing to unlock
  main   lock                               -> 0        main is now the owner
  other  unlock                             -> -EPERM   rejected: other is not the owner
  other  lock, K_NO_WAIT                    -> -EBUSY   held by main
  other  lock, 20 ms timeout                -> -EAGAIN  gave up after 20 ms
  main   lock again (recursive)             -> 0        lock count is now 2
  main   unlock                             -> 0        lock count 1: still locked
  other  lock, K_NO_WAIT                    -> -EBUSY   still held by main
  main   unlock                             -> 0        lock count 0: free
  other  lock, K_NO_WAIT (then unlock)      -> 0        free, so other gets it
--- The same with a binary k_sem used as a lock ---
  main   take ("lock")                      -> 0        main holds it, but the kernel does not know
  other  give ("unlock")                    -> 0        accepted: a semaphore has no owner
  main   take again, K_NO_WAIT              -> 0        succeeds only because other gave
  main   take again (nested), 20 ms         -> -EAGAIN  not recursive: main waits for itself
--- End of tour ---

Sensors: temp (prio 6) reads every 2 ms, accel (prio 4) every 10 ms. Bus lock: k_mutex
[bus] t=2772 ms, lock: k_mutex
[bus]   temp  reads   ok=763 bad=0 lock timeouts=0 longest=1702 us
[bus]   accel samples ok=181 bad=0 lock timeouts=0 longest=1307 us
```

> **About the numbers.** The outputs in this README were measured in QEMU (`qemu_cortex_m3` with the board's 10 kHz tick and an emulated GPIO), not on the board, except where an experiment says "prediction". Counts and times on the board will differ somewhat. The pattern will not: which counters stay at 0, and which ones grow.

Read the tour as a table of rules:

- **`-EINVAL`**: an unlocked mutex cannot be unlocked.
- **`-EPERM`**: only the owner may unlock. This is the rule a semaphore does not have (compare the `give ("unlock") -> 0` line).
- **`-EBUSY` vs `-EAGAIN`**: `K_NO_WAIT` fails at once with `-EBUSY`. A timeout that expires returns `-EAGAIN`.
- **Recursion**: the owner may lock again, and the mutex is free only after the same number of unlocks.
- The last line is the semaphore failing the same nested take: it waits for itself, and only the timeout gets it out.

In the sensor report, the counts are approximate. `temp` completes a read about every 2.4 ms (300 µs of bus + 2 ms of sleep + one tick), and `accel` completes a sample about every 11 ms. The numbers to check are **`bad=0`** and **`lock timeouts=0`**. `longest` is a little over 1200 µs. That is a thread's own transaction plus, in the worst case, waiting for the other thread's (900 + 300 µs), plus interrupts and context switches.

## Experiments

Change a value in `prj.conf` (or the code, where stated), rebuild, flash, and write down your prediction first.

### 1 · No lock at all

Set `CONFIG_APP_LOCK_NONE=y` (and comment out the mutex line).

*Prediction:* whenever `accel` wakes up while `temp` is inside its 300 µs transaction, both transactions are ruined.

```
[bus] t=2736 ms, lock: none
[bus]   temp  reads   ok=657 bad=97 lock timeouts=0 longest=1789 us
[bus]   accel samples ok=85 bad=98 lock timeouts=0 longest=1230 us
[bus] t=4778 ms, lock: none
[bus]   temp  reads   ok=1300 bad=212 lock timeouts=0 longest=2197 us
[bus]   accel samples ok=154 bad=212 lock timeouts=0 longest=1303 us
```

`temp` is inside a transaction only about 300 / 2400 ≈ 12 % of the time, yet in QEMU more than half of the `accel` samples collided. The two periods do not drift randomly against each other, so a naive probability estimate can be far off. Your `bad` counts will differ. What stays the same is that the two `bad` counts are (almost) equal: each collision corrupts `accel`'s first read (the bus was busy when it started) *and* `temp`'s read (`accel` changed the address and register under it). The red LED flickers. Note that nothing crashes and no error is reported by the kernel. On real hardware you would simply get wrong numbers.

### 2 · A binary semaphore as the lock

Set `CONFIG_APP_LOCK_SEMAPHORE=y`.

*Prediction:* the ownership tour already told you. `accel`'s outer take succeeds, and each of its three inner takes waits 100 ms for itself and fails.

```
[bus] t=2769 ms, lock: binary k_sem
[bus]   temp  reads   ok=30 bad=0 lock timeouts=13 longest=100444 us
[bus]   accel samples ok=0 bad=0 lock timeouts=6 longest=301472 us
```

- Every `accel` sample fails, after holding the semaphore for 3 × 100 ms.
- `temp` is starved as a side effect. While `accel` holds the semaphore, each of `temp`'s attempts times out after 100 ms. It only gets the bus during `accel`'s 10 ms sleep.

Now set `CONFIG_APP_LOCK_TIMEOUT_MS=60000` (a prediction; this variant has not been run). Both threads stop: the report repeats the same numbers, while the heartbeat keeps blinking. This is a **self-deadlock**, and it looks like silence. With `K_FOREVER` it would never end.

You could "fix" this by removing the nested lock, but then `bus_read_reg()` would be unsafe to call on its own. The real fix is the right primitive. Restore `CONFIG_APP_LOCK_MUTEX=y` and the 100 ms timeout.

### 3 · A critical section that is too large

Set `CONFIG_APP_ACCEL_WORK_IN_LOCK_MS=5`. `accel` now "processes" its sample for 5 ms *before* releasing the bus.

*Prediction:* `temp`'s `longest` jumps from about 1.2 ms to about 0.9 + 5.1 + 0.3 ≈ 6.3 ms, and its read count drops, because the bus is now locked for about 6 ms in every 16 ms.

```
[bus] t=4815 ms, lock: k_mutex
[bus]   temp  reads   ok=1172 bad=0 lock timeouts=0 longest=6577 us
[bus]   accel samples ok=246 bad=0 lock timeouts=0 longest=7972 us
```

Compare with the default run: `temp` completes about 580 reads per report instead of 760, and `accel` about 123 samples instead of 181.

Then set it to `150` (a prediction; this variant has not been run). The lock is now held longer than `temp`'s 100 ms lock timeout, so `temp` reports `lock timeouts`. The code is still *correct*, and nothing is corrupted. It is just slow, and that is the usual cost of a lock that covers more than the shared resource. Set it back to `0`.

### 4 · A forgotten unlock

In `src/bus.c`, comment out the `bus_unlock();` line in `bus_read_reg()`.

*Prediction (not yet run):* `temp` starts first, so it becomes the owner and never gives the mutex back. Because the mutex is recursive, `temp` itself never notices: each of its locks just raises the lock count. Every `accel` sample waits 100 ms and fails.

```
[bus]   temp  reads   ok=830 bad=0 lock timeouts=0 longest=...
[bus]   accel samples ok=0 bad=0 lock timeouts=18 longest=100100 us
```

The thread with the bug looks healthy, and an innocent thread gets the blame. When a lock timeout appears, find out who owns the lock. In a debugger, the owner is in `bus_mutex.owner`, which is exactly what a semaphore cannot tell you. Restore the line.

### 5 · A mutex in an ISR

In `src/main.c`, add `bus_lock();` at the start of `button_isr()`, rebuild with the mutex, and press the button.

```
ASSERTION FAIL [!arch_is_in_isr()] @ WEST_TOPDIR/zephyr/kernel/mutex.c:111
	mutexes cannot be used inside ISRs
```

After the assertion the kernel halts with a fatal error, and the heartbeat stops.

A mutex has an owner, and an ISR is not a thread, so it cannot own anything or block. `CONFIG_ASSERT=y` catches this. Without it, the behaviour is undefined. ISRs protect data with `irq_lock()` or a spinlock, or hand the work to a thread (tutorial 01). Remove the line.

## Common mistakes

- **Locking each call instead of the whole sequence.** A lock inside every function is not enough when *several* calls must happen together (the X/Y/Z sample, write-register-then-read).
- **Using a binary semaphore as a lock.** It works until someone locks twice (experiment 2), and it never tells you when the wrong thread "unlocks" it.
- **An early `return` that skips the unlock.** Use one exit point, and check every error path (experiment 4).
- **Doing slow work while holding the lock**: processing, printing, or sleeping (experiment 3).
- **Ignoring the return value of `k_mutex_lock()` with a timeout.** On `-EAGAIN` you do *not* own the mutex, and you must not touch the resource or unlock.
- **Locking a mutex in an ISR** (experiment 5).
- **Letting a thread exit while it still holds a mutex.** Nothing unlocks it, and every other user waits forever.

## Check your understanding

1. `temp` calls `k_mutex_lock(&m, K_MSEC(100))` and gets `-EAGAIN`. Should it call `k_mutex_unlock()`?
   <details><summary>Answer</summary>No. It does not own the mutex. The unlock would return <code>-EPERM</code>, or worse, if it held the mutex from an earlier lock, it would release a lock it still needs.</details>
2. Why does experiment 1 corrupt both threads' reads, and not just one?
   <details><summary>Answer</summary><code>accel</code> starts its transaction while the bus is busy, which is a collision. It then changes the address and register that <code>temp</code>'s transaction is about to read from. So <code>accel</code>'s read starts wrong and <code>temp</code>'s read ends wrong.</details>
3. Why can `temp` never interrupt `accel`'s transaction, even without a lock?
   <details><summary>Answer</summary><code>temp</code> has a lower priority (6 vs 4). It can only run when <code>accel</code> is blocked or asleep, never in the middle of <code>accel</code>'s busy-wait. Only the more urgent thread can tear the less urgent one's transaction.</details>
4. In the semaphore version, why is it `accel` that hangs, and not `temp`?
   <details><summary>Answer</summary>Only <code>accel</code> nests the lock. <code>temp</code> takes once and gives once, which a semaphore handles fine. A semaphore fails exactly where ownership is needed: when the holder takes again.</details>
5. With the mutex, `accel` sometimes waits for `temp`. What stops a medium-priority thread at priority 5 from making that wait long?
   <details><summary>Answer</summary>Priority inheritance. While <code>accel</code> (4) waits on the mutex that <code>temp</code> holds, <code>temp</code> runs at priority 4, so a thread at priority 5 cannot preempt it. Tutorial 08 measures this.</details>

## Zephyr APIs used

`K_MUTEX_DEFINE`, `k_mutex_lock`, `k_mutex_unlock`, `K_SEM_DEFINE`, `k_sem_take`, `k_sem_give`, `K_NO_WAIT`, `K_FOREVER`, `K_MSEC`, `K_THREAD_DEFINE` (with `SYS_FOREVER_MS`), `k_thread_start`, `K_THREAD_STACK_DEFINE`, `k_thread_create`, `k_thread_join`, `k_thread_name_set`, `k_busy_wait`, `k_msleep`, `k_cycle_get_32`, `k_cyc_to_us_floor32`, `k_uptime_get`, `atomic_set`, `IS_ENABLED`, plus the GPIO calls from tutorial 01

## Next

**04 · Event groups:** wait until *several* conditions are true at once (wait-all), or react to the first of several sources (wait-any), with `k_event`.
