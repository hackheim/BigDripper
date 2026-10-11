// Settings page: loads /api/settings into the form and saves it back as a
// whole. The board checks the lot together and either saves all of it or
// none, with the reason shown here.
var form = document.getElementById('f'), msg = document.getElementById('msg');

function say(text, ok) { msg.textContent = text; msg.className = ok ? 'ok' : 'err'; }

function fill(s) {
  Array.prototype.forEach.call(form.elements, function (e) { if (e.name in s) e.value = s[e.name]; });
}

function load() {
  fetch('/api/settings', { cache: 'no-store' }).then(function (r) { return r.json(); }).then(fill, function () {
    say('Could not load the settings.');
  });
}

form.onsubmit = function (ev) {
  ev.preventDefault();
  var body = {};
  Array.prototype.forEach.call(form.elements, function (e) {
    if (!e.name) return;
    body[e.name] = e.name == 'scale_mm_per_detent' ? parseFloat(e.value) : Number(e.value);
  });
  fetch('/api/settings', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) })
    .then(function (r) { return r.json().then(function (j) { return { ok: r.ok, j: j }; }); })
    .then(function (res) {
      if (res.ok) { fill(res.j); say('Saved.', true); }
      else say(res.j.error || 'Not saved.');
    }, function () { say('Not connected to BigDripper.'); });
};

load();
