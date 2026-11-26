/* Shared site behaviour: highlight the current lesson in the sidebar,
 * mobile menu toggle, and keyboard navigation (← / → between lessons). */
(function () {
  "use strict";

  var page = location.pathname.split("/").pop() || "index.html";

  document.querySelectorAll(".sidebar a").forEach(function (a) {
    if (a.getAttribute("href") === page) {
      a.classList.add("active");
      a.setAttribute("aria-current", "page");
    }
  });

  var toggle = document.querySelector(".menu-toggle");
  var sidebar = document.querySelector(".sidebar");
  if (toggle && sidebar) {
    toggle.addEventListener("click", function () {
      sidebar.classList.toggle("open");
    });
  }

  var prev = document.querySelector(".pager a[rel=prev]");
  var next = document.querySelector(".pager a[rel=next]");
  document.addEventListener("keydown", function (e) {
    var tag = (e.target && e.target.tagName) || "";
    if (tag === "INPUT" || tag === "SELECT" || tag === "TEXTAREA") return;
    if (e.altKey || e.ctrlKey || e.metaKey) return;
    if (e.key === "ArrowLeft" && prev) location.href = prev.href;
    if (e.key === "ArrowRight" && next) location.href = next.href;
  });
})();

/* Small helpers shared by the animation scripts. */
window.RTOSDemo = (function () {
  "use strict";

  function cssVar(name, fallback) {
    var v = getComputedStyle(document.documentElement).getPropertyValue(name).trim();
    return v || fallback;
  }

  /* Prepare a canvas for crisp drawing on high-DPI screens.
   * Returns a 2D context whose units are CSS pixels. */
  function setupCanvas(canvas, cssHeight) {
    var dpr = window.devicePixelRatio || 1;
    var w = canvas.clientWidth || canvas.parentNode.clientWidth || 600;
    canvas.style.height = cssHeight + "px";
    canvas.width = Math.round(w * dpr);
    canvas.height = Math.round(cssHeight * dpr);
    var ctx = canvas.getContext("2d");
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.width = w;
    ctx.height = cssHeight;
    return ctx;
  }

  function hatch(ctx, x, y, w, h, color) {
    ctx.save();
    ctx.beginPath();
    ctx.rect(x, y, w, h);
    ctx.clip();
    ctx.strokeStyle = color;
    ctx.lineWidth = 1.5;
    for (var i = -h; i < w; i += 6) {
      ctx.beginPath();
      ctx.moveTo(x + i, y + h);
      ctx.lineTo(x + i + h, y);
      ctx.stroke();
    }
    ctx.restore();
  }

  function logger(el) {
    return {
      clear: function () { el.textContent = ""; },
      add: function (line) {
        el.textContent += line + "\n";
        el.scrollTop = el.scrollHeight;
      }
    };
  }

  return { cssVar: cssVar, setupCanvas: setupCanvas, hatch: hatch, logger: logger };
})();
