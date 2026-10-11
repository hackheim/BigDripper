// Live view: what the print task did, drawn from Drip.history (app.js).
//
//   Time     16 rows, COIL1 at the top, scrolling left over the last 1/5/20 s:
//            each burst as a bar from when it opened to when it closed (or
//            the next command replaced it). Shows valve timing: burst length
//            against column period, overruns, Prime holding, Trace pulses.
//   Columns  The last columns that fired, one after another, the way they
//            land on the ground.
//
// Below it, speed and burst length over the same time span. Marks: red =
// records lost before reaching this page, orange = columns the wheel jumped
// over, purple = overrun, grey = mode change / new message.
//
// Only draws while the section is open and the page visible.
(function () {
  var live = $('live'), coils = $('coils'), speed = $('speed');
  var view = 'time', span = 5, raf = 0;
  var BLUE = '#2c3e91', RED = '#c0392b', ORANGE = '#e67e22', PURPLE = '#8e44ad', GREY = '#999', GREEN = '#1e8e3e';
  var LEFT = 22;            // px for row / axis labels
  var STOPPED_US = 500000;  // no column for this long: draw speed as 0

  function pick(group, attr, fn) {
    var bs = document.querySelectorAll('#' + group + ' button');
    bs.forEach(function (b) {
      b.onclick = function () {
        bs.forEach(function (o) { o.setAttribute('aria-pressed', o == b); });
        fn(b.dataset[attr]);
      };
    });
  }
  pick('views', 'v', function (v) { view = v; });
  pick('spans', 's', function (s) { span = Number(s); });

  function setup(c) {
    var r = window.devicePixelRatio || 1, w = c.clientWidth, h = c.clientHeight;
    if (c.width != Math.round(w * r) || c.height != Math.round(h * r)) { c.width = Math.round(w * r); c.height = Math.round(h * r); }
    var g = c.getContext('2d');
    g.setTransform(r, 0, 0, r, 0, 0);
    g.clearRect(0, 0, w, h);
    g.font = '10px sans-serif';
    return { g: g, w: w, h: h };
  }

  // Index of the first record at or after `t` (history is in time order).
  function from(t) {
    var h = Drip.history, lo = 0, hi = h.length;
    while (lo < hi) { var m = (lo + hi) >> 1; if (h[m].at < t) lo = m + 1; else hi = m; }
    return lo;
  }

  function vline(g, x, top, bottom, color, width) {
    g.fillStyle = color;
    g.fillRect(Math.round(x) - (width >> 1), top, width, bottom - top);
  }

  function rowLabels(g, top, rh) {
    g.fillStyle = '#555';
    g.textBaseline = 'middle';
    [0, 7, 15].forEach(function (i) { g.fillText(String(i + 1), 2, top + (i + 0.5) * rh); });
  }

  function drawTime() {
    var c = setup(coils), g = c.g, w = c.w, h = c.h;
    var now = Drip.now(), t0 = now - span * 1e6, top = 4, rh = (h - 8) / 16;
    var x = function (t) { return LEFT + (t - t0) / (span * 1e6) * (w - LEFT); };
    var hist = Drip.history;
    // Start one coil command early: one held open from before the window
    // (Prime) still has to be drawn.
    var i = from(t0);
    while (i > 0 && hist[i - 1].kind != 'coils') i--;
    if (i > 0) i--;

    var nextCoils = function (k) { for (var j = k + 1; j < hist.length; j++) if (hist[j].kind == 'coils') return hist[j].at; return now; };
    for (; i < hist.length; i++) {
      var r = hist[i];
      if (r.kind == 'coils') {
        if (r.overrun) vline(g, x(r.at), 0, top + 2, PURPLE, 3);
        if (!r.bits) continue;
        var end = nextCoils(i);
        if (r.open >= 0) end = Math.min(end, r.at + r.open);
        var x0 = Math.max(LEFT, x(r.at)), x1 = Math.max(x0 + 1, x(end));
        g.fillStyle = BLUE;
        for (var b = 0; b < 16; b++) if (r.bits & (0x8000 >> b)) g.fillRect(x0, top + b * rh, x1 - x0, Math.max(1, rh - 1));
      } else if (r.at >= t0) {
        if (r.kind == 'skipped') vline(g, x(r.at), top, h - 4, ORANGE, 2);
        else {
          vline(g, x(r.at), top, h - 4, GREY, 1);
          if (r.kind == 'mode') { g.fillStyle = '#555'; g.textBaseline = 'top'; g.fillText(MODES[r.value] || '', x(r.at) + 3, top); }
        }
      }
    }
    Drip.gaps.forEach(function (gp) { if (gp.at >= t0) vline(g, x(gp.at), 0, h, RED, 3); });
    rowLabels(g, top, rh);
  }

  function drawColumns() {
    var c = setup(coils), g = c.g, w = c.w, h = c.h;
    var top = 4, rh = (h - 8) / 16, cw = Math.max(2, Math.floor(rh * 0.6));
    var fit = Math.floor((w - LEFT) / cw), hist = Drip.history, gaps = Drip.gaps;
    // Walk back from the newest column, right to left.
    var col = 0, gi = gaps.length - 1;
    for (var i = hist.length - 1; i >= 0 && col < fit; i--) {
      var r = hist[i], xr = w - col * cw;
      while (gi >= 0 && gaps[gi].at > r.at) { vline(g, xr, 0, h, RED, 3); gi--; }
      if (r.kind == 'coils' && r.value >= 0) {
        g.fillStyle = BLUE;
        for (var b = 0; b < 16; b++) if (r.bits & (0x8000 >> b)) g.fillRect(xr - cw, top + b * rh, cw, Math.max(1, rh - 1));
        if (r.overrun) vline(g, xr - cw / 2, 0, top + 2, PURPLE, 2);
        col++;
      } else if (r.kind == 'skipped') vline(g, xr, top, h - 4, ORANGE, 2);
      else if (r.kind == 'message_started' || r.kind == 'mode') vline(g, xr, top, h - 4, GREY, 1);
    }
    rowLabels(g, top, rh);
  }

  // Speed (mm/s, left axis) and burst length (ms, right axis).
  function drawSpeed() {
    var c = setup(speed), g = c.g, w = c.w, h = c.h;
    var now = Drip.now(), t0 = now - span * 1e6, top = 12, bottom = h - 4;
    var x = function (t) { return LEFT + (t - t0) / (span * 1e6) * (w - LEFT - 22); };
    var mmpc = S ? S.mm_per_count : 0.25, hist = Drip.history;
    var pts = [];
    for (var i = from(t0 - STOPPED_US); i < hist.length; i++) {
      var r = hist[i];
      if (r.kind == 'coils' && r.value >= 0) pts.push(r);
    }
    var vmax = 50, bmax = 20;
    pts.forEach(function (r) { vmax = Math.max(vmax, r.speed * mmpc); if (r.open > 0) bmax = Math.max(bmax, r.open / 1000); });
    vmax = nice(vmax); bmax = nice(bmax);
    var y = function (v) { return bottom - v / vmax * (bottom - top); };
    var yb = function (ms) { return bottom - ms / bmax * (bottom - top); };

    g.strokeStyle = '#ddd'; g.beginPath(); g.moveTo(LEFT, y(0) + .5); g.lineTo(w - 22, y(0) + .5); g.stroke();
    g.fillStyle = BLUE;
    pts.forEach(function (r) { if (r.open > 0 && r.at >= t0) g.fillRect(x(r.at) - 1, yb(r.open / 1000) - 1, 3, 3); });

    // Speed: a line through each column's estimate, dropping to 0 across a
    // stop and after the last column once the wheel has stopped.
    g.strokeStyle = GREEN; g.lineWidth = 2; g.beginPath();
    var prev = null;
    pts.forEach(function (r) {
      var v = r.speed * mmpc;
      if (!prev) g.moveTo(x(r.at), y(v));
      else if (r.at - prev.at > STOPPED_US) { g.lineTo(x(prev.at + STOPPED_US), y(0)); g.lineTo(x(r.at), y(0)); g.lineTo(x(r.at), y(v)); }
      else g.lineTo(x(r.at), y(v));
      prev = r;
    });
    if (prev && now - prev.at > STOPPED_US) { g.lineTo(x(prev.at + STOPPED_US), y(0)); g.lineTo(x(now), y(0)); }
    g.stroke(); g.lineWidth = 1;

    g.textBaseline = 'top';
    g.fillStyle = GREEN; g.fillText(vmax + ' mm/s', 2, 0);
    g.fillStyle = BLUE; g.textAlign = 'right'; g.fillText(bmax + ' ms', w - 2, 0); g.textAlign = 'left';
  }

  function nice(v) {
    var p = Math.pow(10, Math.floor(Math.log10(v))), m = v / p;
    return (m <= 1 ? 1 : m <= 2 ? 2 : m <= 5 ? 5 : 10) * p;
  }

  // Numbers for the window shown: latest speed, column period and burst,
  // and how many overruns, skipped columns and lost records.
  function stats() {
    var now = Drip.now(), t0 = now - span * 1e6, hist = Drip.history, mmpc = S ? S.mm_per_count : 0.25;
    var cols = [], over = 0, skipped = 0, lost = 0;
    for (var i = from(t0); i < hist.length; i++) {
      var r = hist[i];
      if (r.kind == 'coils' && r.value >= 0) { cols.push(r); if (r.overrun) over++; }
      if (r.kind == 'skipped') skipped += r.value;
    }
    Drip.gaps.forEach(function (gp) { if (gp.at >= t0) lost += gp.count; });
    var last = cols[cols.length - 1], moving = last && now - last.at < STOPPED_US;
    var v = moving ? last.speed * mmpc : 0;
    var period = cols.length > 1 && moving ? (last.at - cols[cols.length - 2].at) / 1000 : null;
    var burst = moving && last.open > 0 ? last.open / 1000 : null;
    $('stats').textContent =
      'Speed ' + (v / 1000 * 3.6).toFixed(1) + ' km/h (' + Math.round(v) + ' mm/s)' +
      ' · column ' + (period ? period.toFixed(1) + ' ms' : '–') +
      ' · burst ' + (burst ? burst.toFixed(1) + ' ms' : '–') +
      ' · ' + cols.length + ' columns, ' + over + ' overruns, ' + skipped + ' skipped, ' + lost + ' lost';
  }

  function frame() {
    if (!live.open || document.hidden) { raf = 0; return; }
    if (view == 'time') drawTime(); else drawColumns();
    drawSpeed();
    stats();
    raf = requestAnimationFrame(frame);
  }
  function start() { if (!raf && live.open && !document.hidden) raf = requestAnimationFrame(frame); }
  live.addEventListener('toggle', start);
  document.addEventListener('visibilitychange', start);
})();
