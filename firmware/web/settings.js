// Settings section: loads the settings into the form and saves it back as a
// whole. The board checks the lot together and either saves all of it or
// none, with the reason shown here.
//
// They need the settings password, asked for once and remembered on this
// phone (Board.password goes with every request).
var Settings = (function () {
  var details = $('settings'), form = $('sf'), unlock = $('unlock'), msg = $('smsg');

  function tell(text, ok) { msg.textContent = text || ''; msg.className = ok ? 'ok' : ''; }

  function fill(s) {
    Array.prototype.forEach.call(form.elements, function (e) { if (e.name in s) e.value = s[e.name]; });
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
    var body = {};
    Array.prototype.forEach.call(form.elements, function (e) {
      if (!e.name) return;
      body[e.name] = e.name == 'scale_mm_per_detent' ? parseFloat(e.value) : Number(e.value);
    });
    Board.call('save_settings', body).then(function (s) { fill(s); tell('Saved.', true); }, function (e) { tell(e.message); });
  };

  try { Board.password = localStorage.getItem('password') || ''; } catch (e) {}
  locked(!Board.password);
  details.addEventListener('toggle', function () { if (details.open && Board.connected) load(); });
  if (location.hash == '#settings') details.open = true;

  // Called by app.js on each (re)connect.
  return { connected: function () { if (details.open) load(); } };
})();
