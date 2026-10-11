// Settings section: loads the settings into the form and saves it back as a
// whole. The board checks the lot together and either saves all of it or
// none, with the reason shown here.
//
// Each field has a help line, and a note worked out from the values in the
// form as they're typed (sizes on the ground, the speeds where things
// change), at an example speed.
//
// They need the settings password, asked for once and remembered on this
// phone (Board.password goes with every request).
var Settings = (function () {
  var details = $('settings'), form = $('sf'), unlock = $('unlock'), msg = $('smsg');
  var saved = null;  // as last loaded from or saved to the board

  // --- Working things out ---------------------------------------------------------
  //
  // The same rules as the engine (lib/drip_core, scanner.cpp), for the notes
  // only; the simulator runs the engine itself.

  function fmt(x, digits) {
    if (!isFinite(x)) return '–';
    var d = digits !== undefined ? digits : Math.abs(x) >= 100 ? 0 : Math.abs(x) >= 10 ? 1 : 2;
    return Number(x.toFixed(d)).toLocaleString('en');
  }

  // What happens at `kmh` with settings `s`.
  function at(s, kmh) {
    var column_mm = s.clicks_per_column * s.scale_mm_per_detent;
    var stretch = 1;
    if (s.stretch_above_kmh > 0) stretch = Math.min(Math.max(kmh / s.stretch_above_kmh, 1), s.max_stretch_pct / 100);
    var mm_per_ms = kmh / 3.6;
    var column_ms = column_mm * stretch / mm_per_ms;
    var spray_ms = Math.min(Math.max(s.burst_duty_pct / 100 * column_ms, s.min_burst_ms), s.column_burst_ms);
    return { column_mm: column_mm * stretch, stretch: stretch, column_ms: column_ms, spray_ms: spray_ms,
             dot_mm: spray_ms * mm_per_ms, mm_per_ms: mm_per_ms };
  }

  // The lowest speed (km/h, to 0.1) where `test` holds, or null below 150.
  function from(s, test) {
    for (var kmh = 0.1; kmh < 150; kmh += 0.1) if (test(at(s, kmh), kmh)) return Math.round(kmh * 10) / 10;
    return null;
  }

  // --- The fields -----------------------------------------------------------------

  var GROUPS = [
    { title: 'Ground', fields: [
      { name: 'scale_mm_per_detent', label: 'Distance per encoder click', unit: 'mm', decimal: true,
        help: 'How far the cart moves for one click of the encoder wheel. Roll it a measured distance and count the clicks.',
        note: function (s) { return '1 m is ' + fmt(1000 / s.scale_mm_per_detent, 0) + ' clicks.'; } },
      { name: 'clicks_per_column', label: 'Column width', unit: 'clicks', min: 1,
        help: 'How far the cart moves from one column of dots to the next. Sets how wide letters are.',
        note: function (s) {
          var c = s.clicks_per_column * s.scale_mm_per_detent;
          return 'Columns ' + fmt(c) + ' mm apart, so a 5-column letter is about ' + fmt(5 * c / 10) + ' cm wide.';
        } },
    ] },
    { title: 'Spraying', fields: [
      { name: 'burst_duty_pct', label: 'Spray share of column', unit: '%', min: 5, max: 95,
        help: 'Each column sprays for this share of the time the cart takes to cross it, so dots stay apart at any speed. 50 = dots half a column long.',
        note: function (s, kmh) {
          var a = at(s, kmh);
          var t = 'At ' + fmt(kmh) + ' km/h a column passes in ' + fmt(a.column_ms) + ' ms, the spray lasts ' + fmt(a.spray_ms) + ' ms';
          if (a.spray_ms >= a.column_ms) return t + ': longer than the column, so dots run together and columns overrun.';
          return t + ': dots ' + fmt(a.dot_mm) + ' mm long, ' + fmt(a.column_mm - a.dot_mm) + ' mm dry between.';
        } },
      { name: 'column_burst_ms', label: 'Longest spray', unit: 'ms', min: 1,
        help: 'The most a column sprays for, at slow speeds, and for the first columns before the speed is known. Also the Trace pulse.',
        note: function (s) {
          var v = from(s, function (a) { return s.burst_duty_pct / 100 * a.column_ms < s.column_burst_ms; });
          return v === null ? '' : 'Every spray is this long below ' + fmt(v) + ' km/h.';
        } },
      { name: 'min_burst_ms', label: 'Shortest spray', unit: 'ms', min: 1,
        help: 'The valves don’t reliably open for less. Sprays are never shorter, even when that makes dots run together.',
        note: function (s) {
          var v = from(s, function (a) { return s.burst_duty_pct / 100 * a.column_ms <= s.min_burst_ms; });
          var merge = from(s, function (a) { return a.column_ms <= s.min_burst_ms; });
          if (v === null) return 'Never reached below 150 km/h.';
          return 'From ' + fmt(v) + ' km/h every spray is this short' +
                 (merge === null ? '.' : '; from ' + fmt(merge) + ' km/h dots run together and columns overrun.');
        } },
    ] },
    { title: 'Stretching', fields: [
      { name: 'stretch_above_kmh', label: 'Stretch text above', unit: 'km/h', decimal: true, min: 0,
        help: 'Faster than this, columns get wider in step with speed, so the valves keep up and letters get longer instead of crowded. 0 = never.',
        note: function (s, kmh) {
          if (!(s.stretch_above_kmh > 0)) return 'Off: letters are the same width at any speed.';
          return 'At ' + fmt(kmh) + ' km/h letters are ' + fmt(at(s, kmh).stretch) + '× as wide. Stretching tops out at ' +
                 fmt(s.stretch_above_kmh * s.max_stretch_pct / 100) + ' km/h.';
        } },
      { name: 'max_stretch_pct', label: 'Most stretch', unit: '%', min: 100, max: 1000,
        help: 'How much wider letters may get. 300 = up to 3 times as wide. Past that, columns get shorter again.' },
    ] },
    { title: 'Text', fields: [
      { name: 'letter_spacing', label: 'Gap between letters', unit: 'columns', min: 0, max: 8,
        help: 'Blank columns after each letter. Applies to messages queued from now on.',
        note: function (s) { return fmt(s.letter_spacing * s.clicks_per_column * s.scale_mm_per_detent) + ' mm between letters.'; } },
      { name: 'print_pause_ms', label: 'Pause between messages', unit: 'ms', min: 0,
        help: 'Time after a queued message before the next one (or the default text) starts.',
        note: function (s, kmh) { return 'At ' + fmt(kmh) + ' km/h, ' + fmt(s.print_pause_ms / 1000 * kmh / 3.6) + ' m of blank ground.'; } },
      { name: 'max_text_len', label: 'Longest message', unit: 'characters', min: 1, max: 255,
        help: 'The most the page accepts. An emoji counts as one.' },
    ] },
    { title: 'Test modes', fields: [
      { name: 'trace_gap_ms', label: 'Trace pause', unit: 'ms', min: 0,
        help: 'In Trace, how long every valve stays shut between pulses.' },
    ] },
  ];
  var FIELDS = [];
  GROUPS.forEach(function (g) { FIELDS = FIELDS.concat(g.fields); });

  function build() {
    var box = $('fields');
    GROUPS.forEach(function (g) {
      var fs = document.createElement('fieldset');
      fs.appendChild(el('legend', '', g.title));
      g.fields.forEach(function (f) {
        var label = el('label', '', f.label + ' ');
        label.appendChild(el('span', 'unit', f.unit));
        var input = document.createElement('input');
        input.name = f.name;
        if (f.decimal) { input.type = 'text'; input.inputMode = 'decimal'; }
        else { input.type = 'number'; if (f.min !== undefined) input.min = f.min; if (f.max !== undefined) input.max = f.max; }
        label.appendChild(input);
        fs.appendChild(label);
        fs.appendChild(el('p', 'help', f.help));
        f.noteEl = el('p', 'note', '');
        fs.appendChild(f.noteEl);
      });
      box.appendChild(fs);
    });
  }

  // The form's values, as numbers; null while it's locked.
  function edited() {
    if (form.hidden || !saved) return null;
    var s = {};
    FIELDS.forEach(function (f) { s[f.name] = Number(form.elements[f.name].value); });
    s.test_line_spacing = saved.test_line_spacing;  // set on the main page
    return s;
  }

  // The simulator's speed.
  function exampleKmh() { var v = Number($('sim_kmh').value); return v > 0 ? v : 10; }

  function notes() {
    var s = edited();
    if (!s) return;
    var bad = !FIELDS.every(function (f) { return isFinite(s[f.name]); }) || !(s.scale_mm_per_detent > 0);
    FIELDS.forEach(function (f) { f.noteEl.textContent = f.note && !bad ? f.note(s, exampleKmh()) : ''; });
  }

  function changed() {
    notes();
    if (window.Simulator) Simulator.update();
  }

  // --- Board ----------------------------------------------------------------------

  function tell(text, ok) { msg.textContent = text || ''; msg.className = ok ? 'ok' : ''; }

  function fill(s) {
    saved = s;
    FIELDS.forEach(function (f) { if (f.name in s) form.elements[f.name].value = s[f.name]; });
    changed();
  }

  function locked(yes) { form.hidden = yes; unlock.hidden = !yes; }

  function load() {
    Board.call('settings').then(function (s) { locked(false); tell(''); fill(s); }, function (e) {
      if (/password/i.test(e.message)) locked(true);
      tell(e.message);
    });
  }

  unlock.onsubmit = function (ev) {
    ev.preventDefault();
    Board.password = $('password').value;
    try { localStorage.setItem('password', Board.password); } catch (e) {}
    load();
  };

  form.onsubmit = function (ev) {
    ev.preventDefault();
    var body = edited();
    delete body.test_line_spacing;
    Board.call('save_settings', body).then(function (s) { fill(s); tell('Saved.', true); }, function (e) { tell(e.message); });
  };
  form.addEventListener('input', function () { tell(''); changed(); });
  $('sim_kmh').addEventListener('input', notes);

  build();
  try { Board.password = localStorage.getItem('password') || ''; } catch (e) {}
  locked(!Board.password);
  details.addEventListener('toggle', function () { if (details.open && Board.connected) load(); });
  if (location.hash == '#settings') details.open = true;

  return {
    // Called by app.js on each (re)connect.
    connected: function () { if (details.open) load(); },
    saved: function () { return saved; },
    edited: edited,
  };
})();
