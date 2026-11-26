/* Semaphore / mutex handoff demo.
 *
 * Three tasks (High, Medium, Low) operate on one kernel object. The learner
 * presses take/give (or lock/unlock) buttons, or plays a scripted scenario.
 *
 * Behaviour modelled (matches common RTOS kernels, including Zephyr):
 *  - Semaphore: a counter with a limit and no owner. take decrements or
 *    blocks at 0. give wakes the highest-priority waiter (FIFO among equal
 *    priorities) and hands it the unit directly; with no waiter it increments
 *    up to the limit, and a give at the limit is lost (count saturates).
 *    Any task, or an ISR, may give.
 *  - Mutex: has an owner. Only the owner may unlock. The owner may lock
 *    again (recursive lock count). A blocked locker raises the owner's
 *    priority to its own (priority inheritance) until the owner unlocks.
 *    ISRs may not use a mutex.
 */
(function () {
  "use strict";
  var D = window.RTOSDemo;

  var MODES = {
    binary:   { label: "Binary semaphore (initial 0, limit 1)", kind: "sem", init: 0, limit: 1 },
    counting: { label: "Counting semaphore (initial 3, limit 3)", kind: "sem", init: 3, limit: 3 },
    mutex:    { label: "Mutex (with priority inheritance)", kind: "mutex" }
  };

  var SCENARIOS = {
    binary: [
      ["note", "Binary semaphore used for signalling: it starts at 0, so the first take blocks."],
      ["take", "L", "Low waits for an event: k_sem_take() on an empty semaphore blocks."],
      ["isr", null, "An ISR gives the semaphore. The waiting task gets it directly and becomes ready."],
      ["isr", null, "Another interrupt: nobody waits, so the count goes 0 → 1."],
      ["isr", null, "A third interrupt before anyone took it: the count is already at its limit 1, so this event is lost."],
      ["take", "M", "Medium takes: count 1 → 0, it does not block. One take consumed two interrupts' worth of signals."],
      ["note", "Lesson: a binary semaphore records 'at least one event happened', not how many. Use a counting semaphore or a queue to count events."]
    ],
    counting: [
      ["note", "Counting semaphore guarding a pool of 3 identical buffers."],
      ["take", "L", "Low takes a buffer: 3 → 2."],
      ["take", "M", "Medium takes a buffer: 2 → 1."],
      ["take", "H", "High takes the last buffer: 1 → 0."],
      ["take", "L", "Low asks for a second buffer: none left, it blocks."],
      ["take", "M", "Medium asks for a second buffer: it blocks too. Medium has higher priority, so it is ahead of Low in the wait queue."],
      ["give", "H", "High returns its buffer. The kernel hands it to the highest-priority waiter: Medium."],
      ["give", "M", "Medium returns one buffer: handed to Low, the only waiter left."],
      ["note", "There is no ownership: the semaphore counts units, it does not know which task holds which one."]
    ],
    mutex: [
      ["note", "Mutex protecting a shared resource. Watch ownership and priority inheritance."],
      ["take", "L", "Low locks the mutex and becomes its owner."],
      ["take", "L", "Low locks again (nested call). The mutex is recursive: lock count 2, no self-deadlock."],
      ["take", "H", "High tries to lock: it blocks. Low inherits High's priority so Medium cannot preempt it."],
      ["take", "M", "Medium tries to lock and blocks behind High (wait queue ordered by priority)."],
      ["give", "M", "Medium tries to unlock a mutex it does not own: rejected (Zephyr returns -EPERM)."],
      ["give", "L", "Low unlocks once: lock count 2 → 1. It still owns the mutex and keeps the boosted priority."],
      ["give", "L", "Low unlocks again: count 0. Ownership passes to High, the highest-priority waiter. Low drops back to its own priority."],
      ["isr", null, "An ISR tries to unlock: not allowed. Mutex operations are thread-only."],
      ["give", "H", "High unlocks: ownership passes to Medium."],
      ["give", "M", "Medium unlocks: the mutex is free."]
    ]
  };

  var PRIO = { H: 3, M: 2, L: 1 };
  var NAMES = { H: "High", M: "Medium", L: "Low" };

  function init(root) {
    var modeSel = root.querySelector("[data-opt=mode]");
    var stage = root.querySelector(".hd-stage");
    var status = root.querySelector(".status");
    var log = D.logger(root.querySelector(".log"));
    var bScenario = root.querySelector("[data-act=scenario]");
    var bReset = root.querySelector("[data-act=reset]");
    var bIsr = root.querySelector("[data-act=isr]");
    var st, timer = null;

    if (root.dataset.default && MODES[root.dataset.default]) modeSel.value = root.dataset.default;

    function fresh() {
      var m = MODES[modeSel.value];
      st = {
        mode: modeSel.value, m: m,
        count: m.kind === "sem" ? m.init : 0,
        owner: null, lockCount: 0,
        waiters: [],
        tasks: {}
      };
      ["H", "M", "L"].forEach(function (k) {
        st.tasks[k] = { id: k, base: PRIO[k], eff: PRIO[k], held: 0, waiting: false };
      });
    }

    function enqueueWaiter(k) {
      // priority order, FIFO among equal priority
      var p = st.tasks[k].eff;
      var i = 0;
      while (i < st.waiters.length && st.tasks[st.waiters[i]].eff >= p) i++;
      st.waiters.splice(i, 0, k);
      st.tasks[k].waiting = true;
    }

    function applyInheritance() {
      if (!st.owner) return;
      var o = st.tasks[st.owner];
      var top = o.base;
      st.waiters.forEach(function (w) { top = Math.max(top, st.tasks[w].eff); });
      if (top !== o.eff) {
        log.add("  " + NAMES[o.id] + " runs at priority " + prioName(top) + (top > o.base ? " (inherited)" : " (restored)"));
      }
      o.eff = top;
    }

    function prioName(p) { return p === 3 ? "High" : p === 2 ? "Medium" : "Low"; }

    function take(k) {
      var t = st.tasks[k];
      if (t.waiting) return msg(NAMES[k] + " is blocked and cannot run code.");
      if (st.m.kind === "sem") {
        if (st.count > 0) {
          st.count--; t.held++;
          return msg(NAMES[k] + ": take → success, count now " + st.count + ".");
        }
        enqueueWaiter(k);
        return msg(NAMES[k] + ": take → count is 0, " + NAMES[k] + " blocks (state: waiting).");
      }
      if (st.owner === null) {
        st.owner = k; st.lockCount = 1; t.held = 1;
        return msg(NAMES[k] + ": lock → mutex was free, " + NAMES[k] + " is now the owner.");
      }
      if (st.owner === k) {
        st.lockCount++; t.held = st.lockCount;
        return msg(NAMES[k] + ": lock → already owner, lock count " + st.lockCount + ".");
      }
      enqueueWaiter(k);
      msg(NAMES[k] + ": lock → owned by " + NAMES[st.owner] + ", " + NAMES[k] + " blocks.");
      applyInheritance();
    }

    function give(k, fromIsr) {
      var who = fromIsr ? "ISR" : NAMES[k];
      if (!fromIsr && st.tasks[k].waiting) return msg(NAMES[k] + " is blocked and cannot run code.");
      if (st.m.kind === "sem") {
        if (!fromIsr && st.tasks[k].held > 0) st.tasks[k].held--;
        if (st.waiters.length) {
          var w = st.waiters.shift();
          st.tasks[w].waiting = false; st.tasks[w].held++;
          return msg(who + ": give → handed directly to waiting " + NAMES[w] + " (highest-priority waiter). Count stays " + st.count + ".");
        }
        if (st.count < st.m.limit) {
          st.count++;
          return msg(who + ": give → no waiter, count now " + st.count + ".");
        }
        return msg(who + ": give → count already at limit " + st.m.limit + ", the give has no effect (event lost).");
      }
      if (fromIsr) return msg("ISR: unlock → rejected. Mutexes cannot be used from interrupt context.", true);
      if (st.owner !== k) return msg(NAMES[k] + ": unlock → rejected, " + NAMES[k] + " is not the owner (-EPERM).", true);
      st.lockCount--; st.tasks[k].held = st.lockCount;
      if (st.lockCount > 0) return msg(NAMES[k] + ": unlock → lock count " + st.lockCount + ", still owner.");
      var o = st.tasks[k];
      if (o.eff !== o.base) log.add("  " + NAMES[k] + " drops back to its own priority " + prioName(o.base));
      o.eff = o.base;
      st.owner = null;
      if (st.waiters.length) {
        var nw = st.waiters.shift();
        st.tasks[nw].waiting = false;
        st.owner = nw; st.lockCount = 1; st.tasks[nw].held = 1;
        msg(NAMES[k] + ": unlock → ownership handed to " + NAMES[nw] + ".");
        applyInheritance();
        return;
      }
      msg(NAMES[k] + ": unlock → mutex is free.");
    }

    function timeout(k) {
      var i = st.waiters.indexOf(k);
      if (i < 0) return;
      st.waiters.splice(i, 1);
      st.tasks[k].waiting = false;
      msg(NAMES[k] + ": wait timed out → returns an error (Zephyr: -EAGAIN), no unit/ownership obtained.");
      if (st.m.kind === "mutex") applyInheritance();
    }

    function msg(text, bad) {
      log.add(text);
      status.textContent = text;
      status.style.color = bad ? "var(--bad)" : "";
    }

    function render() {
      var isMutex = st.m.kind === "mutex";
      var html = '<div class="hd-tasks">';
      ["H", "M", "L"].forEach(function (k) {
        var t = st.tasks[k];
        var cls = t.waiting ? "waiting" : (t.held > 0 ? "holding" : "");
        var state = t.waiting ? "waiting on the " + (isMutex ? "mutex" : "semaphore") :
          (isMutex ? (st.owner === k ? "owner, lock count " + st.lockCount : "running") :
            (t.held > 0 ? "has taken " + t.held : "running"));
        var prio = "priority " + prioName(t.eff) + (t.eff !== t.base ? " ↑ (inherited)" : "");
        html += '<div class="hd-task ' + cls + '" style="border-left:6px solid ' + colorOf(k) + '">' +
          '<span class="hd-name">' + NAMES[k] + ' task</span>' +
          '<span class="hd-btns">' +
          '<button data-k="' + k + '" data-a="take"' + (t.waiting ? " disabled" : "") + ">" + (isMutex ? "lock" : "take") + "</button>" +
          '<button data-k="' + k + '" data-a="give"' + (t.waiting ? " disabled" : "") + ">" + (isMutex ? "unlock" : "give") + "</button>" +
          (t.waiting ? '<button data-k="' + k + '" data-a="timeout">time out</button>' : "") +
          "</span>" +
          '<span class="hd-state">' + state + " · " + prio + "</span></div>";
      });
      html += "</div>";

      html += '<div class="hd-object"><div class="hd-kind">' + st.m.label + "</div>";
      if (isMutex) {
        html += '<div class="hd-tokens">' + (st.owner ? '<div class="hd-token" style="background:' + colorOf(st.owner) + '">' + st.owner + "</div>" : '<div class="hd-token empty"></div>') + "</div>";
        html += '<div class="hd-meta">owner: ' + (st.owner ? NAMES[st.owner] : "none") + " · lock count: " + st.lockCount + "</div>";
      } else {
        var tok = "";
        for (var i = 0; i < st.m.limit; i++) tok += i < st.count ? '<div class="hd-token">1</div>' : '<div class="hd-token empty"></div>';
        html += '<div class="hd-tokens">' + tok + "</div>";
        html += '<div class="hd-meta">count: ' + st.count + " / limit " + st.m.limit + " · no owner</div>";
      }
      html += '<div class="hd-queue">wait queue: ' + (st.waiters.length ? st.waiters.map(function (w) { return NAMES[w]; }).join(" → ") : "empty") + "</div></div>";

      html += '<div class="hd-meta"><p><b>Rules in this mode</b></p>' + rulesFor(st.m) + "</div>";
      stage.innerHTML = html;
      bIsr.textContent = isMutex ? "ISR: unlock" : "ISR: give";
    }

    function rulesFor(m) {
      if (m.kind === "mutex") {
        return "<ul><li>Only the owner can unlock.</li><li>Owner may lock again (recursive).</li><li>Blocked lockers lend their priority to the owner.</li><li>Not usable from an ISR.</li></ul>";
      }
      return "<ul><li>take: count−1, or block at 0.</li><li>give: wake highest-priority waiter, else count+1 up to the limit.</li><li>No owner: any task or ISR may give.</li></ul>";
    }

    function colorOf(k) {
      return k === "H" ? D.cssVar("--t-high", "#d1495b") : k === "M" ? D.cssVar("--t-med", "#edae49") : D.cssVar("--t-low", "#00798c");
    }

    function reset() {
      stopScenario();
      fresh();
      log.clear();
      log.add("Mode: " + st.m.label + ".");
      status.textContent = "";
      render();
    }

    function runAction(a) {
      if (a[0] === "note") msg(a[1]);
      else {
        log.add("— " + a[2]);
        if (a[0] === "take") take(a[1]);
        else if (a[0] === "give") give(a[1], false);
        else if (a[0] === "isr") give(null, true);
        status.textContent = a[2];
      }
      render();
    }

    function playScenario() {
      if (timer) { stopScenario(); return; }
      reset();
      var steps = SCENARIOS[st.mode].slice();
      bScenario.textContent = "Stop scenario";
      runAction(steps.shift());
      timer = setInterval(function () {
        if (!steps.length) { stopScenario(); return; }
        runAction(steps.shift());
      }, 2600);
    }
    function stopScenario() {
      if (timer) clearInterval(timer);
      timer = null;
      bScenario.textContent = "Play scenario";
    }

    stage.addEventListener("click", function (e) {
      var b = e.target.closest("button[data-a]");
      if (!b) return;
      stopScenario();
      var k = b.dataset.k;
      if (b.dataset.a === "take") take(k);
      else if (b.dataset.a === "give") give(k, false);
      else if (b.dataset.a === "timeout") timeout(k);
      render();
    });
    bIsr.addEventListener("click", function () { stopScenario(); give(null, true); render(); });
    bScenario.addEventListener("click", playScenario);
    bReset.addEventListener("click", reset);
    modeSel.addEventListener("change", reset);
    reset();
  }

  document.querySelectorAll("[data-demo=handoff]").forEach(init);
})();
