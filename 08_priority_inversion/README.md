# 08 · Priority inversion: H, M and L on the board

A fixed-priority scheduler promises that the most urgent ready thread runs. Share one resource between a high- and a low-priority thread, add a medium-priority thread that never touches it, and that promise breaks, by an amount no one can bound. This tutorial runs Lesson 11's worked scenario tick for tick on the STM32F769I-DISCO. First with a binary semaphore as the lock, which gives unbounded inversion. Then with a `k_mutex`, whose priority inheritance makes the inversion bounded. Then with an immediate priority ceiling done by hand. The program traces every preemption and priority change, and measures how long H waits.

**Board:** STM32F769I-DISCO · **RTOS:** Zephyr (3.7.99 in this workspace) · **Time:** about 45 minutes

## Learning objectives

When you finish, you will be able to:

- Reproduce unbounded priority inversion, and show that H's delay grows with the work of a task that never uses the lock.
- Show that `k_mutex` priority inheritance bounds the delay to L's critical section, and watch L's priority change in the trace.
- Explain why a semaphore cannot do this: it has no owner to boost.
- Implement an immediate priority ceiling with `k_thread_priority_set()`, and compare it with inheritance.
- Explain what `CONFIG_PRIORITY_CEILING` does in Zephyr: it caps inheritance. It is not a ceiling protocol.
- Explain why bounded does not mean small: inheritance does not shorten L's critical section.

## Theory to read first

- [Lesson 11 · Priority inversion](../resources/webpages/11_priority_inversion.html): the worked scenario (this tutorial's timeline), Mars Pathfinder, inheritance and ceiling protocols. Its interactive timeline shows the same three runs.
- Tutorial 03, experiment 2, where a semaphore used as a lock already failed in another way

## What you will build

```
  units of 100 ms:   0    1    2    3    4    5    6    7    8    9 ...
  L (prio 4)         L ── lock ─────── 4 units inside ───── unlock ── 1 unit
  H (prio 2)                   H ── lock ── 2 units inside ── unlock ── 1 unit
  M (prio 3)                        M ────── 6 units, never touches the lock ──────

  repeated every 3 s (first at 0.5 s); main prints the trace after each run
```

Each task's "work" is CPU time (`k_busy_wait()` in 1 ms chunks), as in Lesson 11, where every task computes. Between chunks, each task notices when it has been preempted, and when its priority changed, and logs it. After the run, main sorts the log by time and prints it, followed by H's response time and how long H was blocked on the lock, measured with the timing counter (the Cortex-M7 DWT cycle counter).

| Board item | Devicetree alias | Use |
|------------|------------------|-----|
| Blue USER button B1 (PA0) | `sw0` | Gives M 5 more units of work in the next cycle |
| Red LED LD1 (PJ13) | `led0` | On while H waits for the lock |
| Green LED LD2 (PJ5) | `led1` | Heartbeat. It pauses while the scenario runs, because main (priority 7) cannot run while the three tasks keep the CPU busy |
| ST-LINK virtual COM port (USART1) | `zephyr,console` | 115200 baud, 8N1 |
| Optional: Arduino D2, D3, D4 | `zephyr,user` (`scope.overlay`) | Logic-analyser pins: H waits, L holds the lock, M active |

The red LED is the inversion you can see: about 0.9 s with the semaphore, 0.3 s with the mutex, and never with the ceiling.

## Files

| File | Purpose |
|------|---------|
| [`CMakeLists.txt`](CMakeLists.txt) | Builds `main.c` and `hml.c` |
| [`Kconfig`](Kconfig) | The lock choice and the scenario, in units |
| [`prj.conf`](prj.conf) | GPIO, priorities, timing functions, assertions, option values |
| [`scope.overlay`](scope.overlay) | Optional: three trace pins on the Arduino header |
| [`src/main.c`](src/main.c) | Common part, the same as tutorials 04 to 07 |
| [`src/demo.h`](src/demo.h) | The three functions the demo provides |
| [`src/hml.c`](src/hml.c) | The three tasks, the lock, the trace and the report |

## Build, flash, monitor

From the workspace root (`/workspace` in the dev container):

```bash
west build -b stm32f769i_disco -p auto apps/RTOS_Synchronization_primitives/08_priority_inversion \
           -d apps/RTOS_Synchronization_primitives/08_priority_inversion/build
```

If `west build` reports `unknown command "build"`, use CMake as in tutorial 01:

```bash
cd apps/RTOS_Synchronization_primitives/08_priority_inversion
cmake -GNinja -B build -DBOARD=stm32f769i_disco .
ninja -C build
```

**Choose the lock** in `prj.conf`: uncomment exactly one `CONFIG_APP_LOCK_*` line, or pass it on the command line, for example `-DCONFIG_APP_LOCK_MUTEX=y`. After changing the choice, rebuild from scratch (`-p always`, or delete `build/`).

**Trace pins (optional):** add `-DEXTRA_DTC_OVERLAY_FILE=scope.overlay` and connect a logic analyser to D2, D3, D4 and GND. Without the overlay, the code leaves those pins alone.

Flash and monitor as before: `west flash -d build`, or copy `build/zephyr/zephyr.bin` onto the `DIS_F769NI` drive. Then open the virtual COM port at 115200 baud and press RESET.

> **About the expected output.** This tutorial has not been built or run yet. The traces below were derived by hand from the code and Lesson 11's scenario, with exact 100 ms units. On the board, each unit of busy work takes slightly longer than 100 ms: each 1 ms chunk has some bookkeeping overhead, and timer interrupts steal a little time. So expect the times to drift by a few milliseconds over a cycle, and H's blocked time to differ from the round numbers by a few milliseconds too. The order of the events and the differences between the three locks are what matter. Correct this README once the board has run it.

---

# Part A · Unbounded and bounded

## Code walkthrough

### 1 · Three tasks, one body

```c
static struct task task_l = { 'L', 0, 1, 4, 1, true };   /* release, before, inside, after */
static struct task task_h = { 'H', 2, 1, 2, 1, true };
static struct task task_m = { 'M', 3, 6, 0, 0, false };  /* never uses the lock */

static void run_once(struct task *t, int64_t release)
{
	work(t, t->before);
	if (t->uses_lock) {
		take_lock(t);
		work(t, t->inside);
		give_lock(t);
		work(t, t->after);
	}
}
```

All three tasks run the same code, at priorities 2 (H), 3 (M) and 4 (L). Each sleeps until its release time in the current cycle (`K_TIMEOUT_ABS_MS`), then runs once. The numbers are Lesson 11's worked scenario, and every one of them is a Kconfig option.

### 2 · Work that notices preemption and priority changes

```c
for (uint32_t ms = 0; ms < units * UNIT_MS; ms++) {
	int64_t now = k_uptime_get();

	if (now - t->last_run > 2) {
		log_event(t->name, EV_RESUMES, now - t->last_run, now);   /* we were preempted */
	}
	check_prio(t);                                            /* inherited? restored? */
	k_busy_wait(1000);
	t->last_run = k_uptime_get();
}
```

A task cannot see itself being preempted, but it can see the gap afterwards. If more than 2 ms passed since its last chunk, something else ran. `check_prio()` reads `k_thread_priority_get(k_current_get())`, which returns the **current** priority, including any inherited boost, and logs every change. This is how the trace shows inheritance at work, from inside the task that receives it.

### 3 · The lock

```c
#if defined(CONFIG_APP_LOCK_SEM)
	if (k_sem_take(&lock_sem, K_NO_WAIT) != 0) {       /* binary semaphore, count 1 */
#else
	if (k_mutex_lock(&lock_mutex, K_NO_WAIT) != 0) {
#endif
		log_event(t->name, EV_WAITS, ...);            /* we are about to block */
		start = timing_counter_get();
		... take or lock with K_FOREVER ...
		end = timing_counter_get();
	}
```

The try-first, then-block pattern tells the trace *whether* the task had to wait. The timing counter measures *how long*. A binary semaphore with a count of 1 works as a lock, and that is exactly the mistake Lesson 11 warns about: it has no **owner**, so the kernel does not know whom to boost.

## Run it (Part A, the semaphore)

```
08_priority_inversion on stm32f769i_disco
Lock: k_sem (no owner, so no inheritance)
Unit 100 ms. L (prio 4) at 0: 1, lock 4, 1.  H (prio 2) at 2: 1, lock 2, 1.  M (prio 3) at 3: 6
Inheritance cap CONFIG_PRIORITY_CEILING = -127
[cycle 1] k_sem (no owner, so no inheritance)
      0 ms  L  prio 4  released
    100 ms  L  prio 4  locks
    200 ms  H  prio 2  released
    300 ms  H  prio 2  waits for the lock
    300 ms  M  prio 3  released
    900 ms  M  prio 3  done (response time 600 ms)
    900 ms  L  prio 4  resumes (was preempted for 700 ms)
   1200 ms  L  prio 4  unlocks
   1200 ms  H  prio 2  locks, after waiting 900 ms
   1400 ms  H  prio 2  unlocks
   1500 ms  H  prio 2  done (response time 1300 ms)
   1500 ms  L  prio 4  resumes (was preempted for 300 ms)
   1600 ms  L  prio 4  done (response time 1600 ms)
[cycle 1] H: response 1300 ms for 400 ms of its own work, blocked on the lock 900.000 ms   (worst so far: 1300 ms, 900.000 ms)
```

This is the left-hand column of Lesson 11's table, line for line:

- **200 ms:** H is released and preempts L, which holds the lock.
- **300 ms:** H needs the lock and blocks. M is released at the same moment. M (3) is more urgent than L (4), so M runs, and **L, the one task that could free H, does not.**
- **300 to 900 ms:** M runs its 6 units. H, the most urgent task in the system, waits for M, a task that never touches the lock.
- **1200 ms:** L finally finishes its critical section, and H runs.

H needed 400 ms of CPU and took 1300 ms. 900 ms of that was blocking, and 600 ms of the blocking was M's work. The red LED stayed on for those 900 ms.

## Experiments (Part A)

### A1 · The mutex

Comment out `CONFIG_APP_LOCK_SEM=y`, uncomment `CONFIG_APP_LOCK_MUTEX=y`, and rebuild from scratch.

*Prediction:*

```
[cycle 1] k_mutex (priority inheritance)
      0 ms  L  prio 4  released
    100 ms  L  prio 4  locks
    200 ms  H  prio 2  released
    300 ms  H  prio 2  waits for the lock
    300 ms  L  prio 2  resumes (was preempted for 100 ms)
    300 ms  L  prio 2  now runs at this priority
    300 ms  M  prio 3  released
    600 ms  L  prio 2  unlocks
    600 ms  H  prio 2  locks, after waiting 300 ms
    800 ms  H  prio 2  unlocks
    900 ms  H  prio 2  done (response time 700 ms)
    900 ms  M  prio 3  starts running (was ready, not running, for 600 ms)
   1500 ms  M  prio 3  done (response time 1200 ms)
   1500 ms  L  prio 4  resumes (was preempted for 900 ms)
   1500 ms  L  prio 4  now runs at this priority
   1600 ms  L  prio 4  done (response time 1600 ms)
[cycle 1] H: response 700 ms for 400 ms of its own work, blocked on the lock 300.000 ms   (worst so far: 700 ms, 300.000 ms)
```

The right-hand column of Lesson 11's table:

- **300 ms:** when H blocks in `k_mutex_lock()`, the kernel raises the owner, L, to H's priority 2. L logs it from the inside: `L prio 2 now runs at this priority`. M is released, but L at 2 beats M at 3, so M waits, ready but not running.
- **600 ms:** L unlocks and drops back to 4 at once. H takes the mutex.
- H's blocking is **300 ms, exactly what remained of L's critical section**. M can no longer extend it.

Nothing changed in H's or L's code. Only the type of the lock changed. The total amount of work is the same, and L still finishes at 1600 ms. The difference is *who waits*: M, which can afford it, instead of H. Every `k_mutex` in Zephyr does this, and there is nothing to enable.

### A2 · Make M work longer

Press the button once, with each lock. M gets 5 more units in the next cycle.

*Prediction:*

- **Semaphore:** H's response grows to **1800 ms** and its blocking to **1400 ms**. Every unit of M's work is added to H's delay. With more medium tasks, or a longer one, there is no limit. That is what "unbounded" means.
- **Mutex:** H stays at **700 ms** and **300 ms**. Only M finishes later, at 2000 ms.

The same happens with `CONFIG_APP_M_WORK=12` for every cycle. That is the Mars Pathfinder failure: a medium-priority communication task kept the low-priority holder of a mutex off the CPU, until the watchdog reset the lander. The mutex had inheritance, but it was turned off.

### A3 · Bounded is not the same as small

Keep the mutex and set `CONFIG_APP_L_INSIDE=8`.

*Prediction:* H blocks at 300 ms, L inherits priority 2 and works the remaining 7 units of its critical section. H waits **700 ms** and responds after 1100 ms. Inheritance removed M from H's delay, but it cannot shorten L's critical section. Every bit of that section that H has to wait out is still H's blocking time. The design rule from Lesson 11 still applies: keep critical sections short, and do not share a lock across distant priority levels if you can pass data through a queue instead. Set it back to 4.

---

# Part B · Ceilings

## B1 · `CONFIG_PRIORITY_CEILING` is a cap on inheritance

Keep `CONFIG_APP_LOCK_MUTEX=y` and uncomment `CONFIG_PRIORITY_CEILING=4` in `prj.conf`. This is a kernel option. It limits how high inheritance may raise a thread: never above priority 4 here.

*Prediction:* the banner shows `CONFIG_PRIORITY_CEILING = 4`, and the trace is the semaphore's. L is never raised, because 4 is its own priority, and there is no `now runs at this priority` line. H responds after **1300 ms**. The cap has switched inheritance off.

Now try `CONFIG_PRIORITY_CEILING=3`, the priority of M. L is raised to 3, but probably not enough. M became ready at 300 ms, just before H blocked and L was raised. At equal priority Zephyr does not preempt, and the scheduler puts a thread whose priority changed behind the threads already waiting at that priority. So M runs first, and H still waits about 900 ms. The trace should show `L prio 3`, but only when L resumes at 900 ms. *(This prediction depends on the scheduler's queue order. Check it.)*

The name is confusing. This option has nothing to do with the priority *ceiling protocol*. It is a safety cap for systems that do not want a mutex to raise a thread into, for example, the cooperative priorities. Comment the line out again.

## B2 · An immediate ceiling, by hand

Zephyr has no ceiling mutex, but the immediate ceiling protocol is easy to emulate. Before locking, raise the thread to the **ceiling** of the resource, the highest priority of any thread that uses it (H's 2). Restore it after unlocking:

```c
t->saved_prio = k_thread_priority_get(k_current_get());
k_thread_priority_set(k_current_get(), CEILING);      /* before the lock, every time */
k_mutex_lock(&lock_mutex, ...);
...
k_mutex_unlock(&lock_mutex);
k_thread_priority_set(k_current_get(), t->saved_prio);
```

Select `CONFIG_APP_LOCK_CEILING=y` and rebuild from scratch.

*Prediction:*

```
[cycle 1] k_mutex + immediate ceiling (priority 2 while held)
      0 ms  L  prio 4  released
    100 ms  L  prio 2  now runs at this priority
    100 ms  L  prio 2  locks
    200 ms  H  prio 2  released
    300 ms  M  prio 3  released
    500 ms  L  prio 2  unlocks
    500 ms  H  prio 2  starts running (was ready, not running, for 300 ms)
    600 ms  H  prio 2  locks
    800 ms  H  prio 2  unlocks
    900 ms  H  prio 2  done (response time 700 ms)
    900 ms  M  prio 3  starts running (was ready, not running, for 600 ms)
   1500 ms  M  prio 3  done (response time 1200 ms)
   1500 ms  L  prio 4  resumes (was preempted for 1000 ms)
   1500 ms  L  prio 4  now runs at this priority
   1600 ms  L  prio 4  done (response time 1600 ms)
[cycle 1] H: response 700 ms for 400 ms of its own work, blocked on the lock 0.000 ms   (worst so far: 700 ms, 0.000 ms)
```

- **100 ms:** L is raised to 2 *as it locks*, before anyone else wants the lock.
- **200 ms:** H is released, but at equal priority it does not preempt L. H waits *ready*, not blocked: `starts running (was ready, not running, for 300 ms)`.
- H **never blocks on the lock** (`blocked 0.000 ms`), and the red LED never lights. Its response is the same 700 ms as with inheritance. Lesson 11 says so too: the same worst case, reached differently.

*Costs, compared with inheritance:*

- The priority is raised on **every** lock, even when no one else wants the resource. Inheritance changes priorities only under contention.
- The ceiling must be known at design time. Set it lower than a real user's priority, and that user can preempt the holder: the protection is lost.
- A ceiling held by a thread that then **blocks** is still broken. Never block while holding the resource.
- For very short sections, `k_sched_lock()` / `k_sched_unlock()` acts as a ceiling above every thread, at the cost of delaying everything, even threads that do not use the resource.

Restore `CONFIG_APP_LOCK_SEM=y` when you are done.

---

## Common mistakes

- **Using a binary semaphore as a lock** (Part A). It has no owner, so nothing can be boosted, and the inversion is unbounded.
- **Thinking inheritance makes long critical sections harmless** (A3). It removes the medium tasks from the delay. The critical section itself remains.
- **Testing only under light load.** Unbounded inversion needs a medium task to run at exactly the wrong time (A2). On the bench M may be idle, and in the field it is not.
- **Blocking while holding a mutex.** Inheritance raises a thread's priority, which helps only if the thread is ready to run. A holder that sleeps or waits on a queue makes the boost useless.
- **Mistaking `CONFIG_PRIORITY_CEILING` for a ceiling protocol** (B1). It is a cap on inheritance, and a low value disables it.
- **A ceiling lower than a real user's priority** (B2). That user preempts the holder, and the protection is gone.
- **Forgetting non-lock inversions.** If H waits for a queue or a semaphore that L fills or gives, M can delay H just the same, and no protocol helps. Give the producer a priority that matches the urgency of its consumer.
- **Unlocking nested mutexes out of order.** Zephyr's inheritance assumes that mutexes are released in reverse order of locking.

## Check your understanding

1. In Part A, H waited 900 ms. Which part of that delay is inherent to sharing the lock, and which part is the defect?
   <details><summary>Answer</summary>The inherent part is the rest of L's critical section, 300 ms. The defect is the 600 ms in which M ran while L, holding the lock, could not. That part depends on M's work, so it has no bound.</details>
2. Why can a `k_sem` not use priority inheritance?
   <details><summary>Answer</summary>A semaphore has no owner. Any thread may give it, so when H blocks the kernel does not know which thread to boost. A mutex records its owner when it is locked, and only the owner may unlock it.</details>
3. In A1, M is released at 300 ms but runs only at 900 ms. Is M now suffering from inversion?
   <details><summary>Answer</summary>No. M waits for L (boosted to H's priority) and then for H. Both do work on behalf of H, the most urgent task, and that is the scheduler keeping its promise. M's response gets worse, from 600 ms to 1200 ms, which is the price of protecting H.</details>
4. With the immediate ceiling, H is never blocked on the lock. Does that mean H is never delayed by L?
   <details><summary>Answer</summary>No. H waits 300 ms *ready* instead of *blocked*, because L runs at the ceiling. The worst-case delay is the same as with inheritance, one critical section of L. The protocol changes when the waiting happens and that it happens at most once, not how long it lasts.</details>
5. You set `CONFIG_PRIORITY_CEILING=2` (H's priority). Does anything change in A1?
   <details><summary>Answer</summary>No. The cap only limits how high a boost may go, and the boost here is to 2 anyway. A cap of 2 would only matter if an even more urgent thread (priority 0 or 1) waited for the mutex.</details>

## Zephyr APIs used

`K_SEM_DEFINE`, `k_sem_take`, `k_sem_give`, `K_MUTEX_DEFINE`, `k_mutex_lock`, `k_mutex_unlock`, `k_thread_priority_get`, `k_thread_priority_set`, `k_current_get`, `CONFIG_PRIORITY_CEILING`, `CONFIG_TIMING_FUNCTIONS`: `timing_init`, `timing_start`, `timing_counter_get`, `timing_cycles_get`, `timing_cycles_to_ns`; `k_busy_wait`, `k_sleep`, `K_TIMEOUT_ABS_MS`, `k_uptime_get`, `K_THREAD_DEFINE` (with `SYS_FOREVER_MS`), `k_thread_start`, `k_thread_name_set`, `GPIO_DT_SPEC_GET_BY_IDX` on `zephyr,user`, `irq_lock`, `irq_unlock`, `atomic_set`, `atomic_clear`, `atomic_inc`, plus the GPIO calls from tutorial 01

## Next

This is the last tutorial of the series. Lesson 12, [Design guidance and common mistakes](../resources/webpages/12_design_guidance.html), collects the rules that tutorials 01 to 08 demonstrated one at a time.
