# RTOS Synchronization Primitives

This is the theory course for a tutorial series on real-time operating system concepts. It covers what an engineer needs before designing multi-threaded firmware: how tasks are scheduled, how they communicate and synchronise, what goes wrong (deadlock, priority inversion), and how to design so it doesn't.

The concepts are platform-neutral. Each lesson ends with a short **"What this becomes in [Zephyr](https://www.zephyrproject.org/)"** note naming the equivalent API on the **[STM32F769I-DISCO](https://www.st.com/en/evaluation-tools/32f769idiscovery.html)** board (STM32F769NI, single Arm Cortex-M7 at 216 MHz, double-precision FPU). The hands-on tutorials that apply each lesson come later, in the numbered sub-folders of this project.

**Start here:** open [`resources/webpages/index.html`](resources/webpages/index.html) in a web browser. The site works offline from the file system and needs no server, build step or external library. Or check it online as [GitHub page](https://madhawapolkotuwa.github.io/RTOS_Synchronization_primitives_with_zephyr/) 

---

## Course structure

Every lesson page has the same sections:

1. **Learning objectives**
2. **The concept** in plain language, with precise definitions where a standard one exists
3. **A diagram or interactive animation**
4. **A short worked example**
5. **Common mistakes**
6. **What this becomes in Zephyr**

The lessons build on each other. Work through them in order.

## Learning path

### Part I · Foundations

| # | Lesson | Covers |
|---|--------|--------|
| 01 | [Tasks and task states](resources/webpages/01_tasks_and_states.html) | Hard/firm/soft real-time, the task and its TCB, Running / Ready / Blocked / Suspended / Terminated, the idle task |
| 02 | [Scheduling](resources/webpages/02_scheduling.html) | Scheduling points, fixed-priority preemptive vs cooperative scheduling, time slicing, the tick and tickless operation. **Animation:** scheduler timeline |
| 03 | [Context switching](resources/webpages/03_context_switching.html) | Context, PendSV-based switching on Cortex-M, FPU lazy stacking, switch cost, why races happen |
| 04 | [Interrupts and deferred processing](resources/webpages/04_interrupts_and_deferred_processing.html) | Interrupt and response latency, ISR rules, kernel-aware vs zero-latency interrupts, handler tasks vs work queues |

### Part II · Communication and synchronization

| # | Lesson | Covers |
|---|--------|--------|
| 05 | [Semaphores](resources/webpages/05_semaphores.html) | Binary and counting semaphores, signalling, resource pools, lost events. **Animation:** semaphore/mutex handoff |
| 06 | [Mutexes](resources/webpages/06_mutexes.html) | Critical sections, ownership, recursive locking, priority inheritance, alternatives (interrupt masking, scheduler lock, atomics, gatekeeper). **Animation:** handoff demo in mutex mode |
| 07 | [Event groups and task notifications](resources/webpages/07_event_groups_and_notifications.html) | AND/OR waits, broadcast, state vs count; direct-to-task notifications and their limits |
| 08 | [Message queues](resources/webpages/08_message_queues.html) | Copy vs reference, ownership transfer, queue sizing, full-queue policies, gatekeeper pattern |
| 09 | [Choosing a primitive](resources/webpages/09_choosing_a_primitive.html) | Decision procedure and side-by-side comparison of all primitives |

### Part III · Hazards

| # | Lesson | Covers |
|---|--------|--------|
| 10 | [Mutual exclusion and deadlock](resources/webpages/10_mutual_exclusion_and_deadlock.html) | Race conditions, requirements for mutual exclusion, the four Coffman conditions and how to break each one, livelock and starvation. **Animation:** deadlock visualisation |
| 11 | [Priority inversion](resources/webpages/11_priority_inversion.html) | Bounded vs unbounded inversion, worked scenario, priority inheritance, original and immediate priority ceiling protocols. **Animation:** side-by-side inversion timeline |

### Part IV · Design practice

| # | Lesson | Covers |
|---|--------|--------|
| 12 | [Design guidance and common mistakes](resources/webpages/12_design_guidance.html) | Rate/deadline-monotonic priorities, response-time analysis, stack sizing, avoiding shared-state bugs, ISR-safe API usage, beginner-mistake checklist |

## Interactive animations

| Animation | Lesson | Script |
|-----------|--------|--------|
| Scheduler timeline: preemptive/cooperative, time slicing, ISR + deferred handler | 02 | `resources/webpages/js/scheduler_timeline.js` |
| Semaphore / mutex handoff: wait queues, lost events, ownership, recursion, inheritance | 05, 06 | `resources/webpages/js/handoff_demo.js` |
| Deadlock: resource-allocation graph, the four conditions, one fix per condition | 10 | `resources/webpages/js/deadlock_demo.js` |
| Priority inversion: plain mutex vs inheritance or immediate ceiling, side by side | 11 | `resources/webpages/js/inversion_demo.js` |

## Project layout

```
RTOS_Synchronization_primitives/
├── README.md                      this file
├── CLAUDE.md                      context for Claude Code sessions opened on this folder
├── _claude/                       session notes, transcripts and temp files (portable workspace)
├── resources/
│   ├── images/                    static SVG diagrams (render in any browser)
│   │   ├── task_states.svg
│   │   ├── context_switch.svg
│   │   ├── isr_deferred.svg
│   │   ├── semaphore_vs_mutex.svg
│   │   ├── event_group.svg
│   │   ├── message_queue.svg
│   │   ├── primitive_selection.svg
│   │   ├── deadlock_cycle.svg
│   │   ├── priority_inversion.svg
│   │   └── stack_layout.svg
│   └── webpages/
│       ├── index.html             course home
│       ├── 01_… – 12_….html       one page per lesson
│       ├── css/style.css          shared stylesheet (light and dark mode)
│       └── js/                    site navigation and the four animations
├── 01_binary_semaphores/          tutorial 01 (written: ISR → thread signalling)
├── 02_counting_semaphores/        tutorial 02 (written: event counting, buffer pool)
├── 03_mutexes/                    tutorial 03 (written: shared bus, ownership)
├── 04_event_groups/               tutorial 04 (written: wait-all gate, wait-any dispatcher)
│                                  tutorials 05–08 below: planned
├── 05_task_notifications/
├── 06_message_queues/
├── 07_deadlock/
└── 08_priority_inversion/
```

The folder name `resources` is spelled this way on purpose, to match the project specification. Keep it that way, because every page links to it.

## Tutorial series

The numbered folders hold the Zephyr tutorials for the STM32F769I-DISCO. Tutorials `01`–`04` are written. 01–03 build for the board with 0 warnings, and 02 and 03 were also run in QEMU; 04 has not been built yet. None has been run on the board yet. The other folders contain only a README with their planned scope.

| Folder | Topic | Theory | Status |
|--------|-------|--------|--------|
| `01_binary_semaphores` | ISR-to-thread signalling with `k_sem` | Lessons 04, 05 | written |
| `02_counting_semaphores` | Event counting and resource pools | Lesson 05 | written |
| `03_mutexes` | Protecting a shared peripheral with `k_mutex` | Lesson 06 | written |
| `04_event_groups` | Multi-condition waits with `k_event` | Lesson 07 | written |
| `05_task_notifications` | One-to-one signalling patterns in Zephyr | Lesson 07 | planned |
| `06_message_queues` | Producer/consumer with `k_msgq` | Lesson 08 | planned |
| `07_deadlock` | Reproducing and fixing a lock-order deadlock | Lesson 10 | planned |
| `08_priority_inversion` | Observing inversion and inheritance | Lesson 11 | planned |

## Glossary

| Term | Definition |
|------|------------|
| **Atomic operation** | An operation that completes as one indivisible step; no other context can observe or interleave with an intermediate state. |
| **Binary semaphore** | A semaphore whose count is limited to 0 or 1; used to signal that an event occurred. |
| **Blocked (pending)** | Task state: waiting for an event or timeout; uses no CPU time. |
| **Bounded waiting** | Requirement that, once a task has requested a critical section, others can enter only a bounded number of times before it does. |
| **Coffman conditions** | The four conditions that must all hold for deadlock: mutual exclusion, hold and wait, no preemption, circular wait. |
| **Context** | The processor state a task needs in order to continue: registers, program counter, stack pointer, status register, and FPU registers if used. |
| **Context switch** | Saving the running task's context and restoring another task's context. |
| **Cooperative scheduling** | The running task keeps the CPU until it blocks, yields or terminates. |
| **Counting semaphore** | A semaphore with a limit greater than one; counts events or free units of a resource. |
| **Critical section** | Code that accesses a shared resource and must not be executed by more than one context at a time. |
| **Deadlock** | A state in which each task of a set waits for an event that only another task of the set can cause. |
| **Deferred interrupt processing** | Splitting interrupt handling into a short ISR and a task (or work item) that does the remaining work. |
| **Event group** | A word of flag bits; tasks wait for any or all of selected bits; setting bits wakes every waiter whose condition is met. |
| **Gatekeeper task** | A task that is the only one allowed to access a resource; other tasks send it requests through a queue. |
| **Idle task** | Lowest-priority task, always ready; runs when no other task is ready. |
| **Interrupt latency** | Time from an interrupt request to the first instruction of its ISR. |
| **ISR** | Interrupt service routine: code invoked by hardware, running outside any task; it cannot block. |
| **Livelock** | Tasks keep running but make no progress, for example by retrying in lockstep. |
| **Message queue** | A FIFO of fixed-size messages with blocking send (when full) and receive (when empty). |
| **Mutex** | A lock with an owner: only the owner can unlock it; supports priority inheritance; thread-only. |
| **Mutual exclusion** | The guarantee that at most one context executes a given critical section at any time. |
| **Preemption** | Suspending the running task because a higher-priority task became ready. |
| **Preemptive scheduling (fixed-priority)** | At every scheduling point, the highest-priority ready task runs. |
| **Priority ceiling** | The highest priority of any task that may lock a given resource. |
| **Priority ceiling protocol** | Original: a task may lock only if its priority exceeds the ceilings of all resources locked by others. Immediate: locking raises the task to the resource's ceiling at once. Both bound blocking to one critical section and prevent deadlock on one CPU. |
| **Priority inheritance** | A mutex owner temporarily runs at the priority of the highest-priority task blocked on that mutex. |
| **Priority inversion** | A higher-priority task waits while a lower-priority task runs, because of a shared resource. Unbounded when unrelated medium-priority tasks extend the wait. |
| **Race condition** | A defect where the result depends on the timing or interleaving of concurrent accesses to shared state. |
| **Rate-monotonic assignment** | Assigning higher priority to tasks with shorter periods; optimal among fixed-priority schemes when deadline = period. |
| **Ready** | Task state: able to run, waiting for the CPU. |
| **Response time** | Time from a task's release (trigger) to the completion of its work. |
| **Running** | Task state: executing on the CPU. |
| **Scheduling point** | A moment when the scheduler may change the running task: block, ready, yield, tick, priority change. |
| **Semaphore** | A non-negative counter with atomic take (decrement or block) and give (wake a waiter or increment) operations; has no owner. |
| **Stack high-water mark** | The maximum stack depth a task has used, measured by painting the stack with a known pattern. |
| **Starvation** | A task is never granted the CPU or a resource although the system as a whole progresses. |
| **Suspended** | Task state: removed from scheduling by an explicit call until resumed. |
| **Task (thread)** | An independent flow of execution with its own stack, saved context, priority and state, managed by the kernel. |
| **Task control block (TCB)** | The kernel's record of a task: saved stack pointer, priority, state, wait information. |
| **Task notification** | A signal sent directly to one task through a value stored in its TCB, without a separate kernel object. |
| **Tick** | The periodic timer interrupt that drives kernel time; tickless kernels program the timer only for the next needed event. |
| **Time slicing** | Round-robin sharing of the CPU among ready tasks of equal priority, each for at most one quantum. |
| **Work queue** | A kernel thread that executes submitted work items one after another; used for deferred processing. |
| **Zero-latency interrupt** | An interrupt the kernel never masks; it must not call kernel functions. |

## Versions and conventions

- Zephyr API names were checked against the Zephyr tree in this workspace (version 3.7.99, at `/opt/toolchains/zephyr`).
- In the lessons, "higher priority" means more urgent. In Zephyr a **lower number is a higher priority**, and negative priorities are cooperative.
- The lessons say *task*; Zephyr says *thread*.
