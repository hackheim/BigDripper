// Main page: mode toolbar, now printing / queue, adding text. Talks to the
// JSON API (API.md), and keeps up to date from its event stream
// (/api/events): status changes, the print task's history and a clock.
//
// The history is kept here, in Drip.history, for the live view
// (monitor.js) to draw.

var $ = function (id) { return document.getElementById(id); };
var S = null, CODES = [];
var btns = document.querySelectorAll('#bar button');

function el(tag, cls, txt) { var e = document.createElement(tag); if (cls) e.className = cls; e.textContent = txt; return e; }

// --- Status ---------------------------------------------------------------------

function draw(s) {
  if (S && S.boot != s.boot) Drip.reset();  // rebooted: history starts over
  S = s;
  btns.forEach(function (b) {
    var on = b.dataset.m == s.mode;
    b.setAttribute('aria-pressed', on);
    b.querySelector('small').textContent = on ? 'ON' : 'OFF';
  });
  $('lines').style.display = s.mode == 'lines' ? 'block' : 'none';
  $('dry').style.display = s.dry_run ? 'block' : 'none';
  if (document.activeElement != $('spacing')) $('spacing').value = s.lines_spacing;
  var c = $('cur'); c.textContent = '';
  if (s.mode == 'off') c.appendChild(el('span', 'empty', '(nothing — all modes off)'));
  else if (s.mode == 'prime') c.appendChild(el('span', 'empty', '(priming — all valves open)'));
  else if (s.mode == 'trace') c.appendChild(el('span', 'empty', '(trace — pulsing all valves)'));
  else if (s.mode == 'lines') c.appendChild(el('span', 'empty', '(vertical line every ' + s.lines_spacing + ' columns)'));
  else if (s.paused) c.appendChild(el('span', 'empty', '(pause)'));
  else {
    c.textContent = s.current.text; c.appendChild(el('span', 'font', label(s.current)));
    if (s.default) c.appendChild(el('span', 'empty', ' (default, on repeat)'));
  }
  var p = $('pend'); p.textContent = '';
  if (!s.pending.length) p.appendChild(el('p', 'empty', '(nothing queued)'));
  else {
    var ol = document.createElement('ol');
    s.pending.forEach(function (m) { var li = el('li', '', m.text); li.appendChild(el('span', 'font', label(m))); ol.appendChild(li); });
    p.appendChild(ol);
  }
  count();
}
function label(m) { return ' · ' + m.font + (m.invert ? ' · inverted' : ''); }

// --- API ------------------------------------------------------------------------

// GET, or POST `body` as JSON. Resolves with the parsed reply; rejects with
// the API's error message (or a generic one if the board isn't reachable).
function api(path, body) {
  var opts = body ? { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) }
                  : { cache: 'no-store' };
  return fetch(path, opts).then(function (r) {
    return r.json().catch(function () { return {}; }).then(function (j) {
      if (!r.ok) throw new Error(j.error || ('HTTP ' + r.status));
      return j;
    });
  }, function () { throw new Error('Not connected to BigDripper.'); });
}

function say(text) { $('msg').textContent = text || ''; }

btns.forEach(function (b) {
  b.onclick = function () {
    api('/api/mode', { mode: S && b.dataset.m == S.mode ? 'off' : b.dataset.m }).then(draw, function (e) { say(e.message); });
  };
});
$('lines').onsubmit = function (e) {
  e.preventDefault(); $('spacing').blur();
  api('/api/lines', { spacing: Number($('spacing').value) }).then(draw, function (e) { say(e.message); });
};

// --- Connection -------------------------------------------------------------------
//
// The event stream, reconnected by the browser on its own. Anything that
// arrives counts as a sign of life; a clock event comes every 2 s, so 5 s of
// silence means we've lost the board.

var lastHeard = 0;
function heard() { lastHeard = Date.now(); $('offline').style.display = 'none'; }
setInterval(function () { if (Date.now() - lastHeard > 5000) $('offline').style.display = 'block'; }, 1000);

function connect() {
  if (!window.EventSource) {
    // No event stream: poll the status instead (no live view).
    (function poll() { api('/api/status').then(function (s) { heard(); draw(s); }, function () {}).then(function () { setTimeout(poll, 1500); }); })();
    return;
  }
  var es = new EventSource('/api/events');
  es.addEventListener('status', function (e) { heard(); draw(JSON.parse(e.data)); });
  es.addEventListener('history', function (e) { heard(); Drip.batch(JSON.parse(e.data)); });
  es.addEventListener('clock', function (e) { heard(); Drip.clock(JSON.parse(e.data).now); });
  es.onopen = function () { heard(); Drip.catchUp(); };
}

// --- History ----------------------------------------------------------------------
//
// Records from the print task, in sequence order, as objects. Stream batches
// are applied in order; if one starts past where we've got to (a dropped
// event, a reconnect) the missing ones are fetched from /api/history, and only
// what the board itself no longer has is marked as lost.

var KINDS = ['coils', 'skipped', 'mode', 'message_started', 'message_finished'];
var MODES = ['off', 'prime', 'trace', 'lines', 'text'];
var KEEP = 6000;  // records kept for the live view

var Drip = {
  history: [],   // {seq, kind, at, value, bits, open, overrun, speed}
  gaps: [],      // {at, count}: records the board lost before we got them
  next: null,    // sequence number we expect next; null = nothing yet
  board: 0,      // board time (us) at `local` (ms, performance.now())
  local: 0,
  fetching: false,
  pending: [],   // stream batches that arrived while fetching

  reset: function () { this.history = []; this.gaps = []; this.next = null; this.pending = []; },

  now: function () { return this.board + (performance.now() - this.local) * 1000; },
  clock: function (us) { this.board = us; this.local = performance.now(); },

  batch: function (b) {
    if (this.fetching) { this.pending.push(b); return; }
    if (this.next === null || b.first > this.next) { this.pending.push(b); this.catchUp(); return; }
    this.apply(b, false);
  },

  // Fetches everything from where we are (or as much as the board keeps).
  catchUp: function () {
    if (this.fetching) return;
    var self = this;
    this.fetching = true;
    api('/api/history?since=' + (this.next === null ? 0 : this.next)).then(function (h) {
      self.fetching = false;
      self.apply(h, true);
      var queued = self.pending; self.pending = [];
      queued.forEach(function (b) { self.batch(b); });
    }, function () { self.fetching = false; });
  },

  apply: function (b, fetched) {
    this.clock(b.now);
    if (b.reset) this.reset();
    if (this.next === null) this.next = b.first;
    else if (b.first > this.next && fetched) {
      // The board had already overwritten these when we asked.
      var at = b.r.length ? b.r[0][1] : b.now;
      this.gaps.push({ at: at, count: b.first - this.next });
    }
    for (var i = 0; i < b.r.length; i++) {
      var seq = b.first + i;
      if (seq < this.next) continue;
      var r = b.r[i];
      this.history.push({ seq: seq, kind: KINDS[r[0]], at: r[1], value: r[2], bits: r[3],
                          open: r[4], overrun: !!r[5], speed: r[6], stretch: r[7] });
      this.next = seq + 1;
    }
    if (this.next < b.next) this.next = b.next;
    if (this.history.length > KEEP) this.history.splice(0, this.history.length - KEEP);
    while (this.gaps.length && this.history.length && this.gaps[0].at < this.history[0].at) this.gaps.shift();
  },
};

// --- Adding text --------------------------------------------------------------------

// Same rules as font_glyph_count(): an emoji shortcode (longest match, any
// case) is 1, any other character is 1. The board's check is the real one;
// this is feedback while typing.
function glyphs(t) {
  var n = 0, i = 0, lo = t.replace(/[A-Z]/g, function (c) { return c.toLowerCase(); });
  while (i < t.length) {
    var len = 0;
    CODES.forEach(function (c) { if (c.length > len && lo.startsWith(c, i)) len = c.length; });
    if (len) { n++; i += len; continue; }
    n++;
    i += t.codePointAt(i) > 0xffff ? 2 : 1;
  }
  return n;
}
var txt = $('text');
function count() {
  if (!S) return;
  var left = S.max_text_len - glyphs(txt.value);
  $('left').textContent = left + ' left';
  $('left').className = left < 0 ? 'over' : '';
  $('addbtn').disabled = left < 0;
}
txt.oninput = function () { say(''); count(); };
$('add').onsubmit = function (e) {
  e.preventDefault();
  api('/api/queue', { text: txt.value, font: font, invert: inverted }).then(function (s) {
    txt.value = ''; sel = [0, 0]; say(''); draw(s);
  }, function (e) { say(e.message); });
};

// Phones blur the input when a button is tapped, so remember where the
// cursor was and insert there.
var sel = [0, 0];
function keep() { if (document.activeElement == txt) sel = [txt.selectionStart, txt.selectionEnd]; }
document.addEventListener('selectionchange', keep);
['keyup', 'click', 'input', 'blur'].forEach(function (e) { txt.addEventListener(e, function () { sel = [txt.selectionStart, txt.selectionEnd]; }); });
function insert(code) {
  var v = txt.value, a = Math.min(sel[0], v.length), b = Math.min(sel[1], v.length);
  txt.value = v.slice(0, a) + code + v.slice(b);
  a += code.length; sel = [a, a];
  txt.focus(); txt.setSelectionRange(a, a);
  count();
}
function emojiButton(e) {
  var b = document.createElement('button'), cv = document.createElement('canvas'), px = 2, r = window.devicePixelRatio || 1;
  b.type = 'button'; b.title = e.code; b.setAttribute('aria-label', e.code);
  cv.width = cv.height = 16 * px * r;
  var g = cv.getContext('2d'); g.scale(r, r); g.fillStyle = '#000';
  e.columns.forEach(function (c, x) { for (var y = 0; y < 16; y++) if (c & (0x8000 >> y)) g.fillRect(x * px, y * px, px, px); });
  b.appendChild(cv);
  // preventDefault keeps focus (and the phone keyboard) on the text box.
  b.onpointerdown = function (ev) { ev.preventDefault(); };
  b.onclick = function () { insert(e.code); };
  return b;
}

// Font selector and Invert toggle, remembered on this phone. Storage can
// throw (private mode, blocked site data); the page then just starts at
// Drip, not inverted, each time.
var font = 'drip', inverted = false;
var fbtns = document.querySelectorAll('#fonts button');
function style() { txt.className = (font == 'drip_bold' ? 'bold ' : '') + (inverted ? 'inv' : ''); }
function pickFont(f) {
  font = f;
  fbtns.forEach(function (b) { b.setAttribute('aria-pressed', b.dataset.f == f); });
  style();
  try { localStorage.setItem('font', f); } catch (e) {}
}
function setInvert(on) {
  inverted = on;
  $('invert').setAttribute('aria-pressed', on);
  style();
  try { localStorage.setItem('invert', on ? '1' : '0'); } catch (e) {}
}
$('invert').onclick = function () { setInvert(!inverted); };
fbtns.forEach(function (b) { b.onclick = function () { pickFont(b.dataset.f); }; });
var saved = null;
try { saved = localStorage.getItem('font'); } catch (e) {}
pickFont(['spleen', 'drip', 'drip_bold'].indexOf(saved) >= 0 ? saved : 'drip');
saved = null;
try { saved = localStorage.getItem('invert'); } catch (e) {}
setInvert(saved == '1');

api('/api/emoji').then(function (list) {
  list.forEach(function (e) { CODES.push(e.code.toLowerCase()); $('emoji').appendChild(emojiButton(e)); });
  count();
}, function () {});
api('/api/status').then(function (s) { heard(); draw(s); }, function () {});
connect();
