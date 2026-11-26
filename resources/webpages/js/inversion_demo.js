/* Priority inversion timeline: the same task set scheduled twice, side by side.
 * Left: plain mutex (no protocol). Right: priority inheritance, or the
 * immediate priority ceiling protocol, selected by the learner.
 *
 * Task set (priority: larger number = more urgent)
 *   H (3): released t=2.  compute 1, lock, compute 2, unlock, compute 1
 *   M (2): released t=3.  compute 6 (never touches the mutex)
 *   L (1): released t=0.  compute 1, lock, compute 4, unlock, compute 1
 * The mutex ceiling is 3 (the highest priority of any task that locks it).
 *
 * Scheduling: one CPU, fully preemptive, highest effective priority runs.
 * Ties keep the running task on the CPU, otherwise first-come first-served.
 */
(function () {
  "use strict";
  var D = window.RTOSDemo;
  var N = 18;
  var CEILING = 3;

  function taskSet() {
    return [
      { id: "H", base: 3, release: 2, segs: [["c", 1], ["lock"], ["c", 2], ["unlock"], ["c", 1]] },
      { id: "M", base: 2, release: 3, segs: [["c", 6]] },
      { id: "L", base: 1, release: 0, segs: [["c", 1], ["lock"], ["c", 4], ["unlock"], ["c", 1]] }
    ].map(function (t) {
      t.eff = t.base; t.si = 0; t.left = t.segs[0][1]; t.active = false; t.blocked = false;
      t.done = false; t.finish = null; t.order = 0; t.holds = false; t.blockedTicks = 0;
      return t;
    });
  }

  function simulate(protocol) {
    var tasks = taskSet();
    var byId = {}; tasks.forEach(function (t) { byId[t.id] = t; });
    var mutex = { owner: null, waiters: [] };
    var current = null, order = 0, trace = [];

    function advance(t) {
      t.si++;
      if (t.si >= t.segs.length) return;
      if (t.segs[t.si][0] === "c") t.left = t.segs[t.si][1];
    }

    for (var tick = 0; tick < N; tick++) {
      var ev = [];
      tasks.forEach(function (t) {
        if (t.release === tick) { t.active = true; t.order = order++; ev.push(t.id + " released"); }
      });

      var run = null;
      for (var guard = 0; guard < 20; guard++) {
        var cands = tasks.filter(function (t) { return t.active && !t.blocked && !t.done; });
        if (!cands.length) break;
        cands.sort(function (a, b) {
          if (b.eff !== a.eff) return b.eff - a.eff;
          if (a === current) return -1;
          if (b === current) return 1;
          return a.order - b.order;
        });
        var p = cands[0];
        var seg = p.segs[p.si];
        if (seg[0] === "lock") {
          if (mutex.owner === null) {
            mutex.owner = p; p.holds = true; advance(p);
            if (protocol === "ceiling") { p.eff = Math.max(p.eff, CEILING); ev.push(p.id + " locks the mutex; priority raised to the ceiling (" + CEILING + ")"); }
            else ev.push(p.id + " locks the mutex");
            current = p;
            continue;
          }
          p.blocked = true;
          mutex.waiters.push(p);
          ev.push(p.id + " tries to lock, mutex owned by " + mutex.owner.id + ": " + p.id + " blocks");
          if (protocol === "inherit" && mutex.owner.eff < p.eff) {
            mutex.owner.eff = p.eff;
            ev.push(mutex.owner.id + " inherits priority " + p.eff + " from " + p.id);
          }
          continue;
        }
        if (seg[0] === "unlock") {
          p.holds = false; mutex.owner = null; advance(p);
          var was = p.eff; p.eff = p.base;
          ev.push(p.id + " unlocks the mutex" + (was !== p.base ? "; priority returns to " + p.base : ""));
          if (mutex.waiters.length) {
            mutex.waiters.sort(function (a, b) { return b.eff - a.eff || a.order - b.order; });
            var w = mutex.waiters.shift();
            w.blocked = false; mutex.owner = w; w.holds = true; advance(w);
            if (protocol === "ceiling") w.eff = Math.max(w.eff, CEILING);
            if (protocol === "inherit") mutex.waiters.forEach(function (x) { w.eff = Math.max(w.eff, x.eff); });
            ev.push("mutex handed to " + w.id);
          }
          if (p.si >= p.segs.length) { p.done = true; p.finish = tick; }
          current = p;
          continue;
        }
        run = p;
        break;
      }

      var snap = { t: tick, run: run ? run.id : null, st: {}, cs: {}, eff: {}, ev: ev };
      if (run) {
        if (current !== run) ev.push("context switch → " + run.id + (run.eff !== run.base ? " (running at priority " + run.eff + ")" : ""));
        current = run;
        run.left--;
      }
      tasks.forEach(function (t) {
        snap.eff[t.id] = t.eff;
        snap.cs[t.id] = t.holds;
        snap.st[t.id] = !t.active ? "inactive" : t.done ? "done" : t === run ? "run" : t.blocked ? "blocked" : "ready";
        if (t.blocked) t.blockedTicks++;
      });
      if (run && run.left === 0) {
        advance(run);
        if (run.si >= run.segs.length) { run.done = true; run.finish = tick + 1; ev.push(run.id + " completes at t=" + (tick + 1)); }
      }
      trace.push(snap);
    }
    return { trace: trace, tasks: byId };
  }

  var COLORS = function () {
    return { H: D.cssVar("--t-high", "#d1495b"), M: D.cssVar("--t-med", "#edae49"), L: D.cssVar("--t-low", "#00798c") };
  };

  function prioColor(p) {
    var c = COLORS();
    return p === 3 ? c.H : p === 2 ? c.M : c.L;
  }

  function draw(ctx, sim, upto) {
    var W = ctx.width, Hh = ctx.height, col = COLORS();
    var left = 34, top = 8, rowH = 28, gap = 10, cw = (W - left - 8) / N;
    var text = D.cssVar("--text", "#222"), muted = D.cssVar("--muted", "#666"), bad = D.cssVar("--bad", "#c4314b");
    ctx.clearRect(0, 0, W, Hh);
    ctx.font = "12px system-ui, sans-serif";
    ctx.textBaseline = "middle";
    ["H", "M", "L"].forEach(function (id, i) {
      var y = top + i * (rowH + gap);
      ctx.fillStyle = text;
      ctx.font = "bold 13px system-ui, sans-serif";
      ctx.fillText(id, 8, y + rowH / 2);
      ctx.font = "12px system-ui, sans-serif";
      var rel = sim.tasks[id].release;
      if (rel < upto) {
        var rx = left + rel * cw;
        ctx.beginPath(); ctx.moveTo(rx, y - 3); ctx.lineTo(rx - 4, y + 5); ctx.lineTo(rx + 4, y + 5); ctx.closePath(); ctx.fill();
      }
      for (var t = 0; t < upto; t++) {
        var s = sim.trace[t], x = left + t * cw, st = s.st[id];
        if (st === "run") {
          ctx.fillStyle = col[id];
          ctx.fillRect(x + 0.5, y, cw - 1, rowH);
          if (s.cs[id]) D.hatch(ctx, x + 0.5, y, cw - 1, rowH, "rgba(255,255,255,0.75)");
          if (s.eff[id] !== sim.tasks[id].base) {
            ctx.fillStyle = prioColor(s.eff[id]);
            ctx.fillRect(x + 0.5, y - 5, cw - 1, 5);
          }
        } else if (st === "ready") {
          ctx.globalAlpha = 0.3; ctx.fillStyle = col[id];
          ctx.fillRect(x + 0.5, y + rowH * 0.32, cw - 1, rowH * 0.36);
          ctx.globalAlpha = 1;
        } else if (st === "blocked") {
          ctx.strokeStyle = bad; ctx.lineWidth = 1.5;
          ctx.strokeRect(x + 1.5, y + rowH * 0.25, cw - 3, rowH * 0.5);
          ctx.beginPath();
          ctx.moveTo(x + 3, y + rowH * 0.3); ctx.lineTo(x + cw - 3, y + rowH * 0.7);
          ctx.stroke();
        }
      }
      var f = sim.tasks[id].finish;
      if (f !== null && f <= upto) {
        ctx.fillStyle = text;
        ctx.fillRect(left + f * cw - 1, y - 2, 2, rowH + 4);
      }
    });
    var ay = top + 3 * (rowH + gap) + 2;
    ctx.fillStyle = muted; ctx.textAlign = "center";
    for (var k = 0; k <= N; k += 2) { var ax = left + k * cw; ctx.fillRect(ax, ay - 3, 1, 6); ctx.fillText(String(k), ax, ay + 12); }
    ctx.textAlign = "left";
    if (upto < N) { ctx.fillStyle = D.cssVar("--accent", "#1f6feb"); ctx.fillRect(left + upto * cw - 1, 0, 2, ay); }
  }

  function metrics(sim, upto) {
    var h = sim.tasks.H;
    var blocked = 0, readyWait = 0;
    for (var t = 0; t < upto; t++) {
      if (sim.trace[t].st.H === "blocked") blocked++;
      if (sim.trace[t].st.H === "ready") readyWait++;
    }
    var done = h.finish !== null && h.finish <= upto;
    return "H blocked on mutex <b>" + blocked + "</b> · H ready but not running <b>" + readyWait + "</b>" +
      (done ? " · H response time <b>" + (h.finish - h.release) + "</b> ticks (released t=" + h.release + ", done t=" + h.finish + ")" : " · H not finished yet");
  }

  function init(root) {
    var cL = root.querySelector("canvas[data-side=left]");
    var cR = root.querySelector("canvas[data-side=right]");
    var mL = root.querySelector(".metric[data-side=left]");
    var mR = root.querySelector(".metric[data-side=right]");
    var hR = root.querySelector("h4[data-side=right]");
    var proto = root.querySelector("[data-opt=protocol]");
    var bPlay = root.querySelector("[data-act=play]");
    var bStep = root.querySelector("[data-act=step]");
    var bAll = root.querySelector("[data-act=all]");
    var bReset = root.querySelector("[data-act=reset]");
    var logL = D.logger(root.querySelector(".log[data-side=left]"));
    var logR = D.logger(root.querySelector(".log[data-side=right]"));
    var simL, simR, upto, timer = null, ctxL, ctxR;

    function rebuild() {
      stop();
      simL = simulate("none");
      simR = simulate(proto.value);
      hR.textContent = proto.value === "inherit" ? "Mutex with priority inheritance" : "Immediate priority ceiling (ceiling = H)";
      upto = 0;
      ctxL = D.setupCanvas(cL, 150);
      ctxR = D.setupCanvas(cR, 150);
      logL.clear(); logR.clear();
      render();
    }
    function render() {
      draw(ctxL, simL, upto); draw(ctxR, simR, upto);
      mL.innerHTML = metrics(simL, upto); mR.innerHTML = metrics(simR, upto);
    }
    function step() {
      if (upto >= N) { stop(); return; }
      simL.trace[upto].ev.forEach(function (e) { logL.add("t=" + upto + " " + e); });
      simR.trace[upto].ev.forEach(function (e) { logR.add("t=" + upto + " " + e); });
      upto++;
      render();
      if (upto >= N) stop();
    }
    function stop() { if (timer) clearInterval(timer); timer = null; bPlay.textContent = "Play"; }

    bPlay.addEventListener("click", function () {
      if (timer) { stop(); return; }
      if (upto >= N) rebuild();
      bPlay.textContent = "Pause";
      timer = setInterval(step, 650);
    });
    bStep.addEventListener("click", function () { stop(); step(); });
    bAll.addEventListener("click", function () { stop(); while (upto < N) step(); });
    bReset.addEventListener("click", rebuild);
    proto.addEventListener("change", rebuild);
    window.addEventListener("resize", function () { ctxL = D.setupCanvas(cL, 150); ctxR = D.setupCanvas(cR, 150); render(); });
    rebuild();
  }

  document.querySelectorAll("[data-demo=inversion]").forEach(init);
})();
