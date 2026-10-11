// The simulator's workings, no DOM: the board's own print engine compiled
// to WebAssembly (sim.wasm, from sim/sim.cpp), and a model of where the
// water lands.
//
//   Sim.load()                      resolves once sim.wasm is ready
//   Sim.defaults()                  the board's default settings
//   Sim.check(settings)             null, or why the board would refuse them
//   Sim.run(settings, text, opts)   what the engine fires on a simulated ride
//   Sim.ground(run, water)          where the water lands
//
// The engine part is exact: the same code as the board. The water model is
// a guess with knobs, to be matched against photos of real prints.

var Sim = (function () {
  // Settings fields in the order sim.cpp reads them (Settings in params.h).
  var FIELDS = ['scale_mm_per_detent', 'max_text_len', 'print_pause_ms', 'clicks_per_column', 'column_burst_ms',
                'burst_duty_pct', 'min_burst_ms', 'trace_gap_ms', 'test_line_spacing', 'letter_spacing',
                'stretch_above_kmh', 'max_stretch_pct'];
  var FONTS = { spleen: 0, drip: 1, drip_bold: 2 };
  var COUNTS_PER_DETENT = 4;
  var w = null, loading = null;

  function load(source) {
    if (!loading) {
      // The engine never prints or exits; these just satisfy the C library.
      var nothing = function () { return 0; };
      var imports = { env: { emscripten_notify_memory_growth: nothing },
                      wasi_snapshot_preview1: { fd_close: nothing, fd_write: nothing, fd_seek: nothing } };
      var bytes = source ? Promise.resolve(source) : fetch('sim.wasm').then(function (r) { return r.arrayBuffer(); });
      loading = bytes.then(function (b) { return WebAssembly.instantiate(b, imports); }).then(function (r) {
        w = r.instance.exports;
        w._initialize();
      });
    }
    return loading;
  }

  function f64(ptr, n) { return new Float64Array(w.memory.buffer, ptr, n); }

  function putSettings(s) {
    var v = f64(w.sim_settings(), FIELDS.length);
    FIELDS.forEach(function (k, i) { v[i] = Number(s[k]); });
  }

  function cString(ptr) {
    var m = new Uint8Array(w.memory.buffer), end = ptr;
    while (m[end]) end++;
    return new TextDecoder().decode(m.subarray(ptr, end));
  }

  function defaults() {
    w.sim_defaults();
    var v = f64(w.sim_settings(), FIELDS.length), s = {};
    FIELDS.forEach(function (k, i) { s[k] = Math.round(v[i] * 1000) / 1000; });
    return s;
  }

  function check(s) {
    putSettings(s);
    var err = w.sim_check();
    return err ? cString(err) : null;
  }

  // Prints `text` once over a ride. opts: font ('drip'), invert, and
  // profile: [[seconds, km/h], ...], linear in between and held after.
  // Returns {error} or {commands, track (mm, one per ms), mm_per_count}.
  function run(s, text, opts) {
    putSettings(s);
    var bytes = new TextEncoder().encode(text), size = w.sim_text_size();
    if (bytes.length >= size) return { error: 'Text too long for the simulator.' };
    var tb = new Uint8Array(w.memory.buffer, w.sim_text(), size);
    tb.set(bytes); tb[bytes.length] = 0;
    var prof = opts.profile.slice(0, w.sim_profile_max()), pv = f64(w.sim_profile(), prof.length * 2);
    prof.forEach(function (p, i) { pv[2 * i] = p[0]; pv[2 * i + 1] = p[1]; });

    var n = w.sim_run(prof.length, FONTS[opts.font || 'drip'], opts.invert ? 1 : 0);
    if (n == -1) return { error: check(s) };
    if (n == -2) return { error: 'Nothing to print.' };
    var c = f64(w.sim_commands(), n * 6), commands = [];
    for (var i = 0; i < n; i++) {
      commands.push({ t: c[6 * i], bits: c[6 * i + 1], close: c[6 * i + 2], column: c[6 * i + 3],
                      stretch: c[6 * i + 4], overrun: !!c[6 * i + 5] });
    }
    var len = w.sim_ride_ms();
    return { commands: commands, track: new Float32Array(w.memory.buffer, w.sim_track(), len).slice(),
             mm_per_count: s.scale_mm_per_detent / COUNTS_PER_DETENT };
  }

  // --- Water ----------------------------------------------------------------------
  //
  // Per nozzle: the valve opens open_ms after it's told to and closes
  // close_ms after. While open, water comes out at a steady rate (1 per ms).
  // After it closes, the tube between valve and nozzle drains: dribble_ms
  // worth of full flow in total, tailing off exponentially over dribble_tau_ms
  // (most of it within about 3x that), until the valve opens again. Water
  // lands under the nozzle where the cart is at that moment.

  var DT = 0.1;  // ms

  // When each nozzle's valve is actually open: [[from, to], ...] in ms.
  function valveTimes(commands, row, water) {
    var bit = 1 << row, told = [], out = [];
    commands.forEach(function (c, i) {
      if (!(c.bits & bit)) return;
      var next = i + 1 < commands.length ? commands[i + 1].t : Infinity;
      var end = Math.min(c.close, next);
      var last = told[told.length - 1];
      if (last && last[1] >= c.t) last[1] = Math.max(last[1], end);  // still open: carries on
      else told.push([c.t, end]);
    });
    told.forEach(function (t) {
      var from = t[0] + water.open_ms, to = t[1] + water.close_ms;
      if (to <= from) return;  // closed again before it ever opened
      var last = out[out.length - 1];
      if (last && last[1] >= from) last[1] = Math.max(last[1], to);
      else out.push([from, to]);
    });
    return out;
  }

  // Water per 1 mm of ground, for each of the 16 nozzles (row 0 = COIL16,
  // which prints the top of the letters).
  function ground(r, water) {
    var track = r.track, len = track.length;
    var mmAt = function (t) {
      if (t <= 0) return track[0];
      if (t >= len - 1) return track[len - 1];
      var i = Math.floor(t), f = t - i;
      return track[i] + f * (track[i + 1] - track[i]);
    };
    var bins = Math.ceil(track[len - 1]) + 2, rows = [];
    for (var row = 0; row < 16; row++) {
      var g = new Float32Array(bins), times = valveTimes(r.commands, row, water);
      var put = function (t, amount) { g[Math.floor(mmAt(t))] += amount; };
      times.forEach(function (o, i) {
        var t;
        for (t = o[0]; t < o[1]; t += DT) put(t, DT);
        // Dribble, until the valve opens again.
        var until = Math.min(i + 1 < times.length ? times[i + 1][0] : Infinity, o[1] + 6 * water.dribble_tau_ms, len - 1);
        if (water.dribble_ms > 0 && water.dribble_tau_ms > 0) {
          for (t = o[1]; t < until; t += DT) {
            put(t, water.dribble_ms / water.dribble_tau_ms * Math.exp(-(t - o[1]) / water.dribble_tau_ms) * DT);
          }
        }
      });
      rows.push(g);
    }
    return rows;
  }

  // Numbers about a run, for showing next to it.
  function summary(r) {
    var cols = r.commands.filter(function (c) { return c.column >= 0; });
    var maxStretch = 100, overruns = 0;
    cols.forEach(function (c) { maxStretch = Math.max(maxStretch, c.stretch); if (c.overrun) overruns++; });
    var last = cols[cols.length - 1];
    return { columns: cols.length, overruns: overruns, stretch: Math.round(maxStretch),
             length_mm: last ? r.track[Math.min(r.track.length - 1, Math.round(last.t))] : 0,
             seconds: last ? last.t / 1000 : 0 };
  }

  return { FIELDS: FIELDS, load: load, defaults: defaults, check: check, run: run, ground: ground,
           valveTimes: valveTimes, summary: summary };
})();
