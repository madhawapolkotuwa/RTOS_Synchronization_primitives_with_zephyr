/* Scheduler timeline animation.
 *
 * A tick-based simulation of one CPU running a fixed task set. The trace is
 * computed in full, then revealed tick by tick so the learner can watch the
 * scheduler decisions.
 *
 * Model
 *  - Priorities: larger number = more urgent (the page explains that Zephyr
 *    uses the opposite numeric convention).
 *  - Preemptive: at every tick the highest-priority ready task runs. A running
 *    task keeps the CPU against equal-priority tasks unless time slicing is on.
 *  - Cooperative: the running task keeps the CPU until its job completes
 *    (it blocks). Interrupts still preempt it, but tasks made ready by an
 *    ISR wait until the running task gives up the CPU.
 *  - Time slicing (preemptive only): a task that has run for Q consecutive
 *    ticks while an equal-priority task is ready moves to the back of the
 *    ready queue for its priority.
 *  - ISR: occupies one tick and makes the deferred handler task ready.
 */
(function () {
  "use strict";
  var D = window.RTOSDemo;
  var N = 40;
  var ISR_TICKS = [7, 26];

  function buildTasks(opts) {
    var t = [];
    if (opts.isr) {
      t.push({ id: "DH", name: "Deferred handler", prio: 4, exec: 2, releases: [], color: "#8e44ad" });
    }
    t.push({ id: "HI", name: "High (period 10)", prio: 3, exec: 2, releases: periodic(3, 10), color: D.cssVar("--t-high", "#d1495b") });
    t.push({ id: "MED", name: "Medium (period 15)", prio: 2, exec: 4, releases: periodic(1, 15), color: D.cssVar("--t-med", "#edae49") });
    t.push({ id: "LOA", name: opts.twin ? "Low A" : "Low (background)", prio: 1, exec: 9, releases: [0], color: D.cssVar("--t-low", "#00798c") });
    if (opts.twin) {
      t.push({ id: "LOB", name: "Low B (same prio)", prio: 1, exec: 7, releases: [0], color: "#5b8e3e" });
    }
    t.forEach(function (x) { x.remaining = 0; x.slice = 0; x.misses = 0; });
    return t;
  }

  function periodic(offset, period) {
    var r = [];
    for (var k = offset; k < N; k += period) r.push(k);
    return r;
  }

  function simulate(opts) {
    var tasks = buildTasks(opts);
    var byId = {};
    tasks.forEach(function (x) { byId[x.id] = x; });
    var queue = [];          // ready queue, in arrival order
    var current = null;      // task that owns the CPU (null = idle)
    var trace = [];
    var switches = 0;
    var lastThread = "IDLE"; // last thread context that ran (ISR excluded)
    var preemptive = opts.mode === "preemptive";
    var Q = preemptive ? opts.slice : 0;

    function enqueue(task) {
      if (queue.indexOf(task) < 0) queue.push(task);
    }

    for (var t = 0; t < N; t++) {
      var ev = [];
      // 1. periodic releases
      tasks.forEach(function (x) {
        if (x.releases.indexOf(t) >= 0) {
          if (x.remaining > 0) {
            x.misses++;
            ev.push(x.name + " released again before finishing: deadline miss");
          }
          x.remaining += x.exec;
          enqueue(x);
          ev.push(x.name + " released (ready)");
        }
      });

      // 2. interrupt
      if (opts.isr && ISR_TICKS.indexOf(t) >= 0) {
        var dh = byId.DH;
        dh.remaining += dh.exec;
        enqueue(dh);
        ev.push("IRQ: ISR runs, acknowledges the device, signals the deferred handler");
        trace.push(snapshot(t, "ISR", ev, false));
        continue;
      }

      // 3. scheduling decision
      var chosen = null;
      var ready = queue.filter(function (x) { return x.remaining > 0; });
      if (ready.length) {
        if (!preemptive && current && current.remaining > 0) {
          chosen = current;
        } else {
          var top = Math.max.apply(null, ready.map(function (x) { return x.prio; }));
          var peers = ready.filter(function (x) { return x.prio === top; });
          if (current && current.remaining > 0 && current.prio === top) {
            if (Q > 0 && current.slice >= Q && peers.length > 1) {
              queue.splice(queue.indexOf(current), 1);
              queue.push(current);
              ev.push("time slice of " + current.name + " expired: moved to back of its priority queue");
              peers = queue.filter(function (x) { return x.remaining > 0 && x.prio === top; });
              chosen = peers[0];
            } else {
              chosen = current;
            }
          } else {
            chosen = peers[0];
          }
        }
      }

      var id = chosen ? chosen.id : "IDLE";
      var switched = id !== lastThread;
      if (switched) {
        switches++;
        var from = lastThread === "IDLE" ? "idle" : byId[lastThread].name;
        var to = chosen ? chosen.name : "idle";
        var why = "";
        if (lastThread !== "IDLE" && byId[lastThread].remaining > 0) {
          why = preemptive && byId[lastThread].prio < (chosen ? chosen.prio : 0) ? " (preemption)" : " (time slice)";
        } else if (lastThread !== "IDLE") {
          why = " (previous task blocked)";
        }
        ev.push("context switch #" + switches + ": " + from + " → " + to + why);
        if (chosen) chosen.slice = 0;
      }
      current = chosen;
      lastThread = id;

      if (chosen) {
        chosen.remaining--;
        chosen.slice++;
        if (chosen.remaining === 0) {
          ev.push(chosen.name + " finished its job and blocks until its next release");
          queue.splice(queue.indexOf(chosen), 1);
        }
      }
      trace.push(snapshot(t, id, ev, switched));
    }

    function snapshot(t, running, events, switched) {
      var st = {};
      tasks.forEach(function (x) {
        if (x.id === running) st[x.id] = "run";
        else st[x.id] = x.remaining > 0 ? "ready" : "wait";
      });
      // a task that just consumed its last tick still ran in this tick
      if (running !== "ISR" && running !== "IDLE") st[running] = "run";
      return { t: t, running: running, st: st, events: events, switched: switched };
    }

    return { tasks: tasks, trace: trace, switches: switches };
  }

  function draw(ctx, sim, upto, opts) {
    var W = ctx.width, H = ctx.height;
    var rows = (opts.isr ? [{ id: "ISR", name: "ISR", color: D.cssVar("--t-isr", "#30343f") }] : [])
      .concat(sim.tasks).concat([{ id: "IDLE", name: "Idle", color: D.cssVar("--t-idle", "#c9ced6") }]);
    var left = 140, top = 10, rowH = 26, gap = 6;
    var cw = (W - left - 10) / N;
    var text = D.cssVar("--text", "#222"), muted = D.cssVar("--muted", "#666");
    ctx.clearRect(0, 0, W, H);
    ctx.font = "12px system-ui, sans-serif";
    ctx.textBaseline = "middle";

    rows.forEach(function (r, i) {
      var y = top + i * (rowH + gap);
      ctx.fillStyle = text;
      ctx.fillText(r.name, 6, y + rowH / 2);
      ctx.strokeStyle = D.cssVar("--border", "#ddd");
      ctx.beginPath(); ctx.moveTo(left, y + rowH + gap / 2); ctx.lineTo(W - 10, y + rowH + gap / 2); ctx.stroke();

      for (var t = 0; t < upto; t++) {
        var s = sim.trace[t];
        var x = left + t * cw;
        var on = false, isReady = false;
        if (r.id === "ISR") on = s.running === "ISR";
        else if (r.id === "IDLE") on = s.running === "IDLE";
        else { on = s.st[r.id] === "run"; isReady = s.st[r.id] === "ready"; }
        if (on) {
          ctx.fillStyle = r.color;
          ctx.fillRect(x + 0.5, y, cw - 1, rowH);
        } else if (isReady) {
          ctx.globalAlpha = 0.28;
          ctx.fillStyle = r.color;
          ctx.fillRect(x + 0.5, y + rowH * 0.3, cw - 1, rowH * 0.4);
          ctx.globalAlpha = 1;
        }
      }
      // release arrows
      if (r.releases) {
        r.releases.forEach(function (rt) {
          if (rt >= upto) return;
          var x = left + rt * cw;
          ctx.fillStyle = text;
          ctx.beginPath();
          ctx.moveTo(x, y - 2); ctx.lineTo(x - 4, y + 6); ctx.lineTo(x + 4, y + 6); ctx.closePath();
          ctx.fill();
        });
      }
    });

    // axis
    var ay = top + rows.length * (rowH + gap) + 4;
    ctx.fillStyle = muted;
    ctx.textAlign = "center";
    for (var t = 0; t <= N; t += 5) {
      var x = left + t * cw;
      ctx.fillRect(x, ay - 3, 1, 6);
      ctx.fillText(String(t), x, ay + 12);
    }
    ctx.textAlign = "left";
    ctx.fillText("tick", 6, ay + 12);
    // context-switch marks
    ctx.fillStyle = D.cssVar("--bad", "#c4314b");
    for (var k = 0; k < upto; k++) {
      if (sim.trace[k].switched) ctx.fillRect(left + k * cw - 1, ay - 10, 2, 8);
    }
    // playhead
    if (upto < N) {
      ctx.fillStyle = D.cssVar("--accent", "#1f6feb");
      ctx.fillRect(left + upto * cw - 1, top - 4, 2, ay - top);
    }
  }

  function init(root) {
    var canvas = root.querySelector("canvas");
    var modeSel = root.querySelector("[data-opt=mode]");
    var sliceSel = root.querySelector("[data-opt=slice]");
    var isrChk = root.querySelector("[data-opt=isr]");
    var twinChk = root.querySelector("[data-opt=twin]");
    var bPlay = root.querySelector("[data-act=play]");
    var bStep = root.querySelector("[data-act=step]");
    var bAll = root.querySelector("[data-act=all]");
    var bReset = root.querySelector("[data-act=reset]");
    var status = root.querySelector(".status");
    var log = D.logger(root.querySelector(".log"));
    var opts, sim, upto, timer = null, ctx;

    function readOpts() {
      return {
        mode: modeSel.value,
        slice: parseInt(sliceSel.value, 10),
        isr: isrChk.checked,
        twin: twinChk.checked
      };
    }

    function rebuild() {
      stop();
      opts = readOpts();
      sliceSel.disabled = opts.mode !== "preemptive";
      sim = simulate(opts);
      upto = 0;
      var rows = sim.tasks.length + (opts.isr ? 1 : 0) + 1;
      ctx = D.setupCanvas(canvas, 10 + rows * 32 + 30);
      log.clear();
      log.add("Mode: " + opts.mode + (opts.mode === "preemptive" && opts.slice ? ", time slice = " + opts.slice + " ticks" : "") + ". Press Play or Step.");
      status.textContent = "";
      draw(ctx, sim, upto, opts);
    }

    function step() {
      if (upto >= N) { stop(); return; }
      var s = sim.trace[upto];
      s.events.forEach(function (e) { log.add("t=" + s.t + "  " + e); });
      upto++;
      draw(ctx, sim, upto, opts);
      var runName = s.running === "ISR" ? "ISR" : s.running === "IDLE" ? "idle thread" :
        sim.tasks.filter(function (x) { return x.id === s.running; })[0].name;
      status.textContent = "t=" + s.t + ": CPU runs " + runName + ".";
      if (upto >= N) {
        var misses = sim.tasks.reduce(function (a, x) { return a + x.misses; }, 0);
        status.textContent = "Done. Context switches: " + sim.switches + ". Deadline misses: " + misses + ".";
        stop();
      }
    }

    function play() {
      if (timer) { stop(); return; }
      if (upto >= N) rebuild();
      bPlay.textContent = "Pause";
      timer = setInterval(step, 380);
    }
    function stop() {
      if (timer) clearInterval(timer);
      timer = null;
      bPlay.textContent = "Play";
    }

    bPlay.addEventListener("click", play);
    bStep.addEventListener("click", function () { stop(); step(); });
    bAll.addEventListener("click", function () { stop(); while (upto < N) step(); });
    bReset.addEventListener("click", rebuild);
    [modeSel, sliceSel, isrChk, twinChk].forEach(function (el) { el.addEventListener("change", rebuild); });
    window.addEventListener("resize", function () { ctx = D.setupCanvas(canvas, parseInt(canvas.style.height, 10)); draw(ctx, sim, upto, opts); });
    rebuild();
  }

  document.querySelectorAll("[data-demo=scheduler]").forEach(init);
})();
