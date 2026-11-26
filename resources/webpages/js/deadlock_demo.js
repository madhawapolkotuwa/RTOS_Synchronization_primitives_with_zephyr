/* Deadlock visualisation: resource-allocation graph plus the four Coffman
 * conditions.
 *
 * Each scenario is a list of steps. A step describes which task holds which
 * resource (edge resource → task), which task waits for which resource
 * (edge task → resource), message edges for the gatekeeper design, and the
 * status of each Coffman condition:
 *   "on"   – the condition holds at this moment
 *   "off"  – the condition does not hold at this moment
 *   "gone" – the design removes the condition entirely
 * Deadlock occurs only when all four hold at once.
 */
(function () {
  "use strict";
  var D = window.RTOSDemo;
  var SVGNS = "http://www.w3.org/2000/svg";

  var BASE_NODES = {
    A:  { x: 90,  y: 60,  kind: "task", label: "Task A" },
    M1: { x: 370, y: 60,  kind: "res",  label: "Mutex M1" },
    B:  { x: 370, y: 210, kind: "task", label: "Task B" },
    M2: { x: 90,  y: 210, kind: "res",  label: "Mutex M2" }
  };
  var GK_NODES = {
    A:  { x: 70,  y: 60,  kind: "task", label: "Task A" },
    B:  { x: 70,  y: 210, kind: "task", label: "Task B" },
    Q:  { x: 215, y: 135, kind: "queue", label: "Request queue" },
    G:  { x: 360, y: 135, kind: "task", label: "Gatekeeper" },
    R1: { x: 430, y: 40,  kind: "res",  label: "SPI bus" },
    R2: { x: 430, y: 230, kind: "res",  label: "Flash" }
  };

  function c(me, hw, np, cw) { return { me: me, hw: hw, np: np, cw: cw }; }

  var SCENARIOS = {
    deadlock: {
      title: "All four conditions present → deadlock",
      nodes: BASE_NODES,
      steps: [
        { text: "Two tasks each need both mutexes. Task A locks M1 then M2; Task B locks M2 then M1.", holds: [], waits: [], cond: c("on", "off", "on", "off") },
        { text: "Task A locks M1.", holds: [["M1", "A"]], waits: [], cond: c("on", "off", "on", "off") },
        { text: "Task A is preempted. Task B locks M2.", holds: [["M1", "A"], ["M2", "B"]], waits: [], cond: c("on", "off", "on", "off") },
        { text: "Task B requests M1 and blocks while still holding M2: hold and wait.", holds: [["M1", "A"], ["M2", "B"]], waits: [["B", "M1"]], cond: c("on", "on", "on", "off") },
        { text: "Task A resumes and requests M2. It blocks while holding M1. The wait-for graph now has a cycle A → M2 → B → M1 → A.", holds: [["M1", "A"], ["M2", "B"]], waits: [["B", "M1"], ["A", "M2"]], cond: c("on", "on", "on", "on"), verdict: "deadlock" },
        { text: "Neither task can run again. Nothing in the system will release M1 or M2. A watchdog reset is the only exit.", holds: [["M1", "A"], ["M2", "B"]], waits: [["B", "M1"], ["A", "M2"]], cond: c("on", "on", "on", "on"), verdict: "deadlock" }
      ]
    },
    ordering: {
      title: "Break circular wait: global lock order (always M1 before M2)",
      nodes: BASE_NODES,
      steps: [
        { text: "Design rule: every task acquires M1 before M2. Task B is rewritten to follow the rule.", holds: [], waits: [], cond: c("on", "off", "on", "gone") },
        { text: "Task A locks M1.", holds: [["M1", "A"]], waits: [], cond: c("on", "off", "on", "gone") },
        { text: "Task B starts and requests M1 first. It blocks holding nothing.", holds: [["M1", "A"]], waits: [["B", "M1"]], cond: c("on", "off", "on", "gone") },
        { text: "Task A locks M2. No one can hold M2 while waiting for M1, so no cycle can form.", holds: [["M1", "A"], ["M2", "A"]], waits: [["B", "M1"]], cond: c("on", "on", "on", "gone") },
        { text: "Task A finishes and unlocks M2 then M1. M1 is handed to Task B.", holds: [["M1", "B"]], waits: [], cond: c("on", "off", "on", "gone") },
        { text: "Task B locks M2, does its work, unlocks both. Everyone made progress.", holds: [["M1", "B"], ["M2", "B"]], waits: [], cond: c("on", "off", "on", "gone"), verdict: "ok" }
      ]
    },
    holdwait: {
      title: "Break hold and wait: acquire everything at once, or nothing",
      nodes: BASE_NODES,
      steps: [
        { text: "Design rule: a task never blocks while holding a lock. It acquires the full set in one step (for example under a single outer lock, or by try-lock and release-all on failure).", holds: [], waits: [], cond: c("on", "gone", "on", "off") },
        { text: "Task A acquires M1 and M2 together.", holds: [["M1", "A"], ["M2", "A"]], waits: [], cond: c("on", "gone", "on", "off") },
        { text: "Task B requests the set {M1, M2}. It is not all available, so B waits holding nothing.", holds: [["M1", "A"], ["M2", "A"]], waits: [["B", "M1"], ["B", "M2"]], cond: c("on", "gone", "on", "off") },
        { text: "Task A releases both. Task B acquires both.", holds: [["M1", "B"], ["M2", "B"]], waits: [], cond: c("on", "gone", "on", "off") },
        { text: "Task B releases both. Waiting tasks never hold a resource, so a cycle cannot close.", holds: [], waits: [], cond: c("on", "gone", "on", "off"), verdict: "ok" }
      ]
    },
    preempt: {
      title: "Break no preemption: lock with a timeout, back off and release",
      nodes: BASE_NODES,
      steps: [
        { text: "Design rule: the second lock is taken with a timeout. On timeout the task releases what it holds and retries later.", holds: [], waits: [], cond: c("on", "off", "gone", "off") },
        { text: "Task A locks M1. Task B locks M2.", holds: [["M1", "A"], ["M2", "B"]], waits: [], cond: c("on", "off", "gone", "off") },
        { text: "Both request the other mutex with a timeout. The cycle exists for now.", holds: [["M1", "A"], ["M2", "B"]], waits: [["A", "M2"], ["B", "M1"]], cond: c("on", "on", "gone", "on") },
        { text: "Task A's timeout expires. It gives up and unlocks M1: the resource is taken back from it. The cycle is broken.", holds: [["M2", "B"]], waits: [["B", "M1"]], cond: c("on", "on", "gone", "off") },
        { text: "Task B gets M1, finishes, and unlocks both.", holds: [["M1", "B"], ["M2", "B"]], waits: [], cond: c("on", "off", "gone", "off") },
        { text: "Task A retries after a delay and succeeds. Cost: wasted work and possible livelock if both retry in lockstep; add a randomised or priority-based back-off.", holds: [["M1", "A"], ["M2", "A"]], waits: [], cond: c("on", "off", "gone", "off"), verdict: "ok" }
      ]
    },
    gatekeeper: {
      title: "Break mutual exclusion between tasks: a single owner (gatekeeper) serves requests",
      nodes: GK_NODES,
      steps: [
        { text: "Design rule: only the gatekeeper task touches the SPI bus and the flash. Other tasks never lock them; they send request messages.", holds: [["R1", "G"], ["R2", "G"]], waits: [], msgs: [], cond: c("gone", "off", "off", "off") },
        { text: "Task A posts 'write log record' to the queue.", holds: [["R1", "G"], ["R2", "G"]], waits: [], msgs: [["A", "Q"]], cond: c("gone", "off", "off", "off") },
        { text: "Task B posts 'read config'. The queue serialises the requests.", holds: [["R1", "G"], ["R2", "G"]], waits: [], msgs: [["A", "Q"], ["B", "Q"]], cond: c("gone", "off", "off", "off") },
        { text: "The gatekeeper takes requests one at a time and performs them. No task ever waits for a lock held by another task.", holds: [["R1", "G"], ["R2", "G"]], waits: [], msgs: [["B", "Q"], ["Q", "G"]], cond: c("gone", "off", "off", "off") },
        { text: "Both requests are served. The resources are never shared between tasks, so no lock cycle can exist. (Keep the gatekeeper from waiting on its own clients, or you rebuild the cycle with messages.)", holds: [["R1", "G"], ["R2", "G"]], waits: [], msgs: [], cond: c("gone", "off", "off", "off"), verdict: "ok" }
      ]
    }
  };

  var COND_LABELS = {
    me: "Mutual exclusion",
    hw: "Hold and wait",
    np: "No preemption",
    cw: "Circular wait"
  };

  function el(name, attrs, text) {
    var e = document.createElementNS(SVGNS, name);
    for (var k in attrs) e.setAttribute(k, attrs[k]);
    if (text != null) e.textContent = text;
    return e;
  }

  function drawGraph(svg, nodes, step, verdict) {
    while (svg.firstChild) svg.removeChild(svg.firstChild);
    var text = D.cssVar("--text", "#222");
    var ok = D.cssVar("--ok", "#1a7f37"), bad = D.cssVar("--bad", "#c4314b"), acc = D.cssVar("--accent", "#1f6feb");
    var defs = el("defs", {});
    [["hold", ok], ["wait", bad], ["msg", acc]].forEach(function (m) {
      var mk = el("marker", { id: "dl-" + m[0] + svg.dataset.uid, viewBox: "0 0 10 10", refX: "9", refY: "5", markerWidth: "7", markerHeight: "7", orient: "auto-start-reverse" });
      mk.appendChild(el("path", { d: "M0,0 L10,5 L0,10 z", fill: m[1] }));
      defs.appendChild(mk);
    });
    svg.appendChild(defs);

    function edge(from, to, type) {
      var a = nodes[from], b = nodes[to];
      var dx = b.x - a.x, dy = b.y - a.y, len = Math.sqrt(dx * dx + dy * dy);
      var ra = a.kind === "task" ? 34 : 30, rb = b.kind === "task" ? 38 : 34;
      var x1 = a.x + dx / len * ra, y1 = a.y + dy / len * ra;
      var x2 = b.x - dx / len * rb, y2 = b.y - dy / len * rb;
      var color = type === "hold" ? ok : type === "wait" ? bad : acc;
      svg.appendChild(el("line", {
        x1: x1, y1: y1, x2: x2, y2: y2, stroke: color, "stroke-width": verdict === "deadlock" && type !== "msg" ? 4 : 3,
        "stroke-dasharray": type === "wait" ? "7 5" : type === "msg" ? "2 5" : "none",
        "marker-end": "url(#dl-" + type + svg.dataset.uid + ")"
      }));
      var lx = (x1 + x2) / 2, ly = (y1 + y2) / 2 - 6;
      svg.appendChild(el("text", { x: lx, y: ly, "text-anchor": "middle", "font-size": "11", fill: color, "font-weight": "600" },
        type === "hold" ? "held by" : type === "wait" ? "waits for" : "message"));
    }

    (step.holds || []).forEach(function (h) { edge(h[0], h[1], "hold"); });
    (step.waits || []).forEach(function (w) { edge(w[0], w[1], "wait"); });
    (step.msgs || []).forEach(function (m) { edge(m[0], m[1], "msg"); });

    Object.keys(nodes).forEach(function (k) {
      var n = nodes[k];
      var g = el("g", {});
      if (n.kind === "task") {
        var stuck = verdict === "deadlock";
        g.appendChild(el("circle", { cx: n.x, cy: n.y, r: 32, fill: stuck ? D.cssVar("--bad-soft", "#fde8ec") : D.cssVar("--surface", "#fff"), stroke: stuck ? bad : text, "stroke-width": 2 }));
      } else if (n.kind === "queue") {
        g.appendChild(el("rect", { x: n.x - 40, y: n.y - 16, width: 80, height: 32, rx: 4, fill: D.cssVar("--surface", "#fff"), stroke: acc, "stroke-width": 2 }));
        for (var i = 1; i < 4; i++) g.appendChild(el("line", { x1: n.x - 40 + i * 20, y1: n.y - 16, x2: n.x - 40 + i * 20, y2: n.y + 16, stroke: acc }));
      } else {
        g.appendChild(el("rect", { x: n.x - 30, y: n.y - 22, width: 60, height: 44, rx: 6, fill: D.cssVar("--surface", "#fff"), stroke: text, "stroke-width": 2 }));
        g.appendChild(el("rect", { x: n.x - 9, y: n.y - 6, width: 18, height: 14, rx: 2, fill: text }));
        g.appendChild(el("path", { d: "M" + (n.x - 6) + "," + (n.y - 6) + " v-5 a6,6 0 0 1 12,0 v5", fill: "none", stroke: text, "stroke-width": 2 }));
      }
      var ty = n.kind === "task" ? n.y + 4 : n.y + (n.kind === "queue" ? 32 : 38);
      g.appendChild(el("text", { x: n.x, y: ty, "text-anchor": "middle", "font-size": "13", "font-weight": "700", fill: text }, n.label));
      svg.appendChild(g);
    });
  }

  var uid = 0;
  function init(root) {
    var svg = root.querySelector("svg");
    svg.dataset.uid = String(++uid);
    var sel = root.querySelector("[data-opt=scenario]");
    var condList = root.querySelector(".conds");
    var stepText = root.querySelector(".dl-step");
    var verdictEl = root.querySelector(".dl-verdict");
    var bPrev = root.querySelector("[data-act=prev]");
    var bNext = root.querySelector("[data-act=next]");
    var bPlay = root.querySelector("[data-act=play]");
    var idx = 0, timer = null;

    function sc() { return SCENARIOS[sel.value]; }

    function render() {
      var s = sc(), step = s.steps[idx];
      drawGraph(svg, s.nodes, step, step.verdict);
      condList.innerHTML = Object.keys(COND_LABELS).map(function (k) {
        var v = step.cond[k];
        var flag = v === "on" ? "present" : v === "gone" ? "removed by design" : "not present now";
        return '<li class="' + (v === "on" ? "on" : v === "gone" ? "off" : "") + '"><span>' + COND_LABELS[k] + '</span><span class="flag">' + flag + "</span></li>";
      }).join("");
      stepText.textContent = "Step " + (idx + 1) + "/" + s.steps.length + ": " + step.text;
      var all = ["me", "hw", "np", "cw"].every(function (k) { return step.cond[k] === "on"; });
      verdictEl.className = "dl-verdict badge " + (step.verdict === "deadlock" ? "bad" : step.verdict === "ok" ? "ok" : "neutral");
      verdictEl.textContent = step.verdict === "deadlock" ? "DEADLOCK: all four conditions hold" :
        step.verdict === "ok" ? "Completed: no deadlock" : (all ? "All four hold: a deadlock exists" : "Running");
      bPrev.disabled = idx === 0;
      bNext.disabled = idx === s.steps.length - 1;
    }

    function stop() { if (timer) clearInterval(timer); timer = null; bPlay.textContent = "Play"; }

    bPrev.addEventListener("click", function () { stop(); if (idx > 0) idx--; render(); });
    bNext.addEventListener("click", function () { stop(); if (idx < sc().steps.length - 1) idx++; render(); });
    bPlay.addEventListener("click", function () {
      if (timer) { stop(); return; }
      if (idx === sc().steps.length - 1) idx = 0;
      render();
      bPlay.textContent = "Pause";
      timer = setInterval(function () {
        if (idx >= sc().steps.length - 1) { stop(); return; }
        idx++; render();
      }, 2800);
    });
    sel.addEventListener("change", function () { stop(); idx = 0; render(); });
    render();
  }

  document.querySelectorAll("[data-demo=deadlock]").forEach(init);
})();
