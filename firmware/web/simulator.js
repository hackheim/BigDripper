// Simulator, at the top of the settings: runs a text through the engine
// (sim.js) over a ride, and draws where the water lands, top-down and to
// scale. Once with the board's saved settings (or the defaults, until
// they've been unlocked), and again with the settings form's edits when they
// differ. The pictures stay in view while the settings are scrolled.
//
// The ride, the water model and the text are remembered on this phone.
var Simulator = (function () {
  var box = $('settings'), ride = 'steady', timer = 0;

  var WATER = [
    { name: 'open_ms', label: 'Valve opens after', unit: 'ms', value: 3,
      help: 'From the board opening a valve to water leaving the nozzle.' },
    { name: 'close_ms', label: 'Valve closes after', unit: 'ms', value: 5,
      help: 'From the board closing a valve to the flow stopping.' },
    { name: 'dribble_ms', label: 'Dribble', unit: 'ms of flow', value: 2,
      help: 'Water left in the tube that drains out after the valve shuts, as that many ms of full flow.' },
    { name: 'dribble_tau_ms', label: 'Dribble time', unit: 'ms', value: 15,
      help: 'How quickly it drains: most of it is out within 3 times this.' },
    { name: 'nozzle_mm', label: 'Nozzle spacing', unit: 'mm', value: 10,
      help: 'Between neighbouring nozzles across the head: sets letter height.' },
    { name: 'spot_mm', label: 'Spray spot', unit: 'mm', value: 8,
      help: 'How wide one nozzle’s spray lands.' },
  ];

  // --- Remembered inputs --------------------------------------------------------

  var KEEP = ['sim_text', 'sim_font', 'sim_kmh', 'sim_from', 'sim_to', 'sim_secs'];
  var DEFAULTS = { sim_text: 'HELLO', sim_font: 'drip', sim_kmh: 10, sim_from: 3, sim_to: 15, sim_secs: 3 };
  var stored = {};
  try { stored = JSON.parse(localStorage.getItem('sim') || '{}') || {}; } catch (e) {}
  function remember() {
    var o = { ride: ride };
    KEEP.forEach(function (id) { o[id] = $(id).value; });
    WATER.forEach(function (f) { o[f.name] = $('w_' + f.name).value; });
    try { localStorage.setItem('sim', JSON.stringify(o)); } catch (e) {}
  }

  function buildWater() {
    var div = $('water_fields');
    WATER.forEach(function (f) {
      var label = el('label', '', f.label + ' ');
      label.appendChild(el('span', 'unit', f.unit));
      var input = document.createElement('input');
      input.type = 'number'; input.min = 0; input.step = 'any'; input.id = 'w_' + f.name;
      input.value = stored[f.name] !== undefined ? stored[f.name] : f.value;
      label.appendChild(input);
      div.appendChild(label);
      div.appendChild(el('p', 'help', f.help));
    });
  }

  function water() {
    var w = {};
    WATER.forEach(function (f) { var v = Number($('w_' + f.name).value); w[f.name] = v >= 0 ? v : f.value; });
    w.nozzle_mm = Math.max(w.nozzle_mm, 1);
    w.spot_mm = Math.max(w.spot_mm, 1);
    return w;
  }

  // --- The ride -----------------------------------------------------------------

  // [[seconds, km/h], ...], or a string saying why there isn't one.
  function profile() {
    if (ride == 'steady') return [[0, Math.max(Number($('sim_kmh').value) || 0, 0.5)]];
    if (ride == 'ramp') {
      return [[0, Math.max(Number($('sim_from').value) || 0, 0)],
              [Math.max(Number($('sim_secs').value) || 0, 0.1), Math.max(Number($('sim_to').value) || 0, 0.5)]];
    }
    // Replay: each column's speed estimate over the last 20 s of history.
    if (!S) return 'Not connected to the board.';
    var now = Drip.now(), pts = [];
    Drip.history.forEach(function (r) {
      if (r.kind == 'coils' && r.value >= 0 && r.speed > 0 && r.at > now - 20e6) {
        pts.push([r.at, r.speed * S.mm_per_count * 3.6 / 1000]);
      }
    });
    if (pts.length < 2) return 'Nothing to replay yet: turn the wheel, then try again.';
    var t0 = pts[0][0];
    return pts.map(function (p) { return [(p[0] - t0) / 1e6, p[1]]; });
  }

  // --- Drawing ------------------------------------------------------------------

  var PX_HIGH = 80;       // ground height, px
  var MAX_PX_WIDE = 12000;
  var WET = 0.02;         // water per mm² (ms of flow) that looks soaked

  // Water per mm of ground and nozzle (Sim.ground()) as an image, 1 px per
  // mm: each nozzle's water spread over a spot, darker the wetter.
  function paint(rows, w, from_mm, to_mm) {
    var spot = Math.round(w.spot_mm), half = spot >> 1;
    var width = to_mm - from_mm, height = Math.round(15 * w.nozzle_mm + spot + 2);
    var dens = new Float32Array(width * height);
    rows.forEach(function (g, row) {
      // Spread along the ground (a running sum over the spot) ...
      var line = new Float32Array(width), sum = 0;
      for (var x = -half; x < width + half; x++) {
        var add = g[from_mm + x + half], drop = g[from_mm + x - half - 1];
        sum += (add || 0) - (drop || 0);
        if (x >= 0 && x < width) line[x] = sum / (spot * spot);
      }
      // ... and across it.
      var y0 = Math.round(1 + row * w.nozzle_mm);
      for (var y = y0; y < y0 + spot && y < height; y++) {
        for (var i = 0; i < width; i++) dens[y * width + i] += line[i];
      }
    });
    var img = new ImageData(width, height), d = img.data;
    for (var p = 0; p < dens.length; p++) {
      var a = 1 - Math.exp(-dens[p] / WET);
      // Ink #1d3d8f over pavement #d8d4cc.
      d[4 * p] = 216 + (29 - 216) * a; d[4 * p + 1] = 212 + (61 - 212) * a; d[4 * p + 2] = 204 + (143 - 204) * a; d[4 * p + 3] = 255;
    }
    return img;
  }

  function draw(canvas, r, w) {
    var rows = Sim.ground(r, w), from_mm = Infinity, to_mm = 0;
    rows.forEach(function (g) {
      for (var i = 0; i < g.length; i++) if (g[i] > 1e-4) { from_mm = Math.min(from_mm, i); to_mm = Math.max(to_mm, i); }
    });
    if (to_mm == 0) { canvas.width = 0; return; }
    var margin = Math.round(w.spot_mm * 2 + 20);
    from_mm = Math.max(0, from_mm - margin); to_mm += margin;
    var img = paint(rows, w, from_mm, to_mm);
    var scale = Math.min(PX_HIGH / img.height, MAX_PX_WIDE / img.width);
    var off = document.createElement('canvas');
    off.width = img.width; off.height = img.height;
    off.getContext('2d').putImageData(img, 0, 0);

    var ratio = window.devicePixelRatio || 1, cw = Math.round(img.width * scale), ch = Math.round(img.height * scale);
    canvas.style.width = cw + 'px'; canvas.style.height = (ch + 14) + 'px';
    canvas.width = cw * ratio; canvas.height = (ch + 14) * ratio;
    var g = canvas.getContext('2d');
    g.setTransform(ratio, 0, 0, ratio, 0, 0);
    g.fillStyle = '#fff'; g.fillRect(0, 0, cw, ch + 14);
    g.drawImage(off, 0, 0, cw, ch);
    // Where each column fired, under the ground; overruns in purple.
    r.commands.forEach(function (c) {
      if (c.column < 0) return;
      var x = (r.track[Math.min(r.track.length - 1, Math.round(c.t))] - from_mm) * scale;
      g.fillStyle = c.overrun ? '#8e44ad' : '#999';
      g.fillRect(Math.round(x), ch + 2, 1, c.overrun ? 10 : 5);
    });
    // A 10 cm scale bar.
    g.fillStyle = '#333'; g.fillRect(4, ch + 9, 100 * scale, 2);
    g.font = '10px sans-serif'; g.textBaseline = 'bottom'; g.fillText('10 cm', 8 + 100 * scale, ch + 14);
  }

  function info(r) {
    var s = Sim.summary(r);
    var t = s.columns + ' columns, ' + (s.length_mm / 1000).toFixed(2) + ' m long, printed in ' + s.seconds.toFixed(1) + ' s';
    if (s.stretch > 100) t += ' · stretched up to ' + s.stretch + '%';
    if (s.overruns) t += ' · ' + s.overruns + ' overrun' + (s.overruns > 1 ? 's' : '') + ' (purple)';
    return t;
  }

  function show(which, settings, prof, w) {
    var r = Sim.run(settings, $('sim_text').value, { font: $('sim_font').value, profile: prof });
    if (r.error) { $(which).width = 0; $(which + '_info').textContent = r.error; return; }
    draw($(which), r, w);
    $(which + '_info').textContent = info(r);
  }

  function same(a, b) { return Sim.FIELDS.every(function (k) { return Number(a[k]) == Number(b[k]); }); }

  function render() {
    timer = 0;
    if (!box.open) return;
    var prof = profile(), w = water();
    var saved = Settings.saved(), edited = Settings.edited();
    $('sim_a_title').textContent = saved ? 'Saved settings' : 'Default settings (unlock the settings to use the board’s)';
    var a = saved || Sim.defaults();
    var showB = !!edited && !same(edited, a);
    $('sim_b_box').hidden = !showB;
    if (typeof prof == 'string') {
      ['sim_a', 'sim_b'].forEach(function (id) { $(id).width = 0; $(id + '_info').textContent = prof; });
      return;
    }
    show('sim_a', a, prof, w);
    if (showB) show('sim_b', edited, prof, w);
  }

  // Redraws soon: typing in a field doesn't run it on every key.
  function update() {
    if (!box.open) return;
    clearTimeout(timer);
    timer = setTimeout(function () { Sim.load().then(render, function () { $('sim_a_info').textContent = 'Could not load the simulator.'; }); }, 150);
  }

  function pickRide(r) {
    ride = r;
    document.querySelectorAll('#rides button').forEach(function (b) { b.setAttribute('aria-pressed', b.dataset.r == r); });
    document.querySelectorAll('.ride').forEach(function (p) { p.hidden = p.dataset.r != r; });
    $('sim_kmh').parentNode.firstChild.textContent = r == 'steady' ? 'At ' : 'Notes at ';
  }

  buildWater();
  KEEP.forEach(function (id) { $(id).value = stored[id] !== undefined ? stored[id] : DEFAULTS[id]; });
  pickRide(stored.ride || 'steady');
  document.querySelectorAll('#rides button').forEach(function (b) {
    b.onclick = function () { pickRide(b.dataset.r); remember(); update(); };
  });
  $('sim').addEventListener('input', function () { remember(); update(); });
  box.addEventListener('toggle', update);
  update();

  return { update: update };
})();
