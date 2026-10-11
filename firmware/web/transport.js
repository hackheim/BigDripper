// How the page reaches the board (API.md):
//
//   Board.call(op, args)  one API operation; resolves with its reply, or
//                         rejects with an Error fit to show a user
//   Board.listen(h)       the event stream: h.status(s), h.history(batch),
//                         h.clock(now), h.open() on each (re)connect,
//                         h.close() when the connection is lost
//
// Served by the board itself, that's HTTP and server-sent events. Served
// from anywhere else it's Bluetooth (Web Bluetooth, which needs HTTPS or
// localhost): Board.pick() asks the user to choose the board, and needs a
// tap to call it from. ?transport=http or ?transport=ble in the URL
// overrides the choice (e.g. a page on localhost talking to a mock board).

var Board = (function () {
  var forced = /[?&]transport=(http|ble)\b/.exec(location.search);
  var board = {
    kind: forced ? forced[1] : window.isSecureContext && navigator.bluetooth ? 'ble' : 'http',
    password: '',  // the settings password, sent with every request if set
    connected: false,
  };
  var h = {};
  function emit(name, arg) { if (h[name]) h[name](arg); }
  function lostError() { return new Error('Not connected to BigDripper.'); }

  // --- HTTP ---------------------------------------------------------------------

  var ROUTES = {
    status: 'GET /api/status', emoji: 'GET /api/emoji', history: 'GET /api/history',
    settings: 'GET /api/settings', queue: 'POST /api/queue', mode: 'POST /api/mode',
    lines: 'POST /api/lines', save_settings: 'POST /api/settings',
  };

  function httpCall(op, args) {
    var route = ROUTES[op].split(' '), path = route[1], opts = { method: route[0], headers: {} };
    if (route[0] == 'GET') {
      var q = Object.keys(args || {}).map(function (k) { return k + '=' + encodeURIComponent(args[k]); }).join('&');
      if (q) path += '?' + q;
      if (op != 'emoji') opts.cache = 'no-store';
    } else {
      opts.headers['Content-Type'] = 'application/json';
      opts.body = JSON.stringify(args || {});
    }
    if (board.password) opts.headers.Authorization = 'Basic ' + btoa(unescape(encodeURIComponent('admin:' + board.password)));
    return fetch(path, opts).then(function (r) {
      return r.json().catch(function () { return {}; }).then(function (j) {
        if (!r.ok) throw new Error(j.error || ('HTTP ' + r.status));
        return j;
      });
    }, function () { throw lostError(); });
  }

  // The event stream, reconnected by the browser on its own.
  function httpListen() {
    if (!window.EventSource) {
      // No event stream: poll the status instead (no live view).
      board.connected = true;
      emit('open');
      (function poll() {
        httpCall('status').then(function (s) { emit('status', s); }, function () {}).then(function () { setTimeout(poll, 1500); });
      })();
      return;
    }
    var es = new EventSource('/api/events');
    es.addEventListener('status', function (e) { emit('status', JSON.parse(e.data)); });
    es.addEventListener('history', function (e) { emit('history', JSON.parse(e.data)); });
    es.addEventListener('clock', function (e) { emit('clock', JSON.parse(e.data).now); });
    es.onopen = function () { board.connected = true; emit('open'); };
    es.onerror = function () { if (board.connected) { board.connected = false; emit('close'); } };
  }

  // --- Bluetooth ----------------------------------------------------------------
  //
  // Requests are written to one characteristic and everything comes back as
  // notifications on another. Every write and notification starts with a
  // type byte (0x80 set: the message continues in the next one).

  var SERVICE = 'c44f5ed8-6e56-4c8b-ae22-7164774163b9';
  var REQUEST = '2edbd9b7-7914-4c4d-8ae1-97fb8b1dc19b';
  var EVENTS = 'b6a173f9-9607-4151-8654-f814ffd5bdf6';
  var REPLY = 1, STATUS = 2, HISTORY = 3, CLOCK = 4, MORE = 0x80;
  var CHUNK = 500;          // request bytes per write (the board takes 512)
  var TIMEOUT_MS = 10000;

  var device = null, request = null, writes = Promise.resolve();
  var nextId = 1, waiting = {}, parts = [];

  function bleCall(op, args) {
    if (!request) return Promise.reject(lostError());
    var id = nextId++, msg = { id: id, op: op, args: args || {} };
    if (board.password) msg.auth = board.password;
    var bytes = new TextEncoder().encode(JSON.stringify(msg)), chr = request;
    return new Promise(function (resolve, reject) {
      var w = waiting[id] = { resolve: resolve, reject: reject };
      w.timer = setTimeout(function () { settle(id, new Error('BigDripper didn\'t answer.')); }, TIMEOUT_MS);
      // One GATT operation at a time, or Web Bluetooth refuses.
      for (var off = 0; off < bytes.length; off += CHUNK) {
        var n = Math.min(CHUNK, bytes.length - off), piece = new Uint8Array(n + 1);
        piece[0] = off + n < bytes.length ? MORE : 0;
        piece.set(bytes.subarray(off, off + n), 1);
        writes = writes.then(chr.writeValueWithResponse.bind(chr, piece));
      }
      writes = writes.catch(function () { settle(id, lostError()); });
    });
  }

  // Ends request `id` with `err`, or with its reply.
  function settle(id, err, reply) {
    var w = waiting[id];
    if (!w) return;
    delete waiting[id];
    clearTimeout(w.timer);
    if (err) w.reject(err);
    else if (reply.code == 200) w.resolve(reply.body);
    else w.reject(new Error((reply.body && reply.body.error) || ('Error ' + reply.code)));
  }

  function received(e) {
    var v = e.target.value, type = v.getUint8(0);
    parts.push(new Uint8Array(v.buffer.slice(v.byteOffset + 1, v.byteOffset + v.byteLength)));
    if (type & MORE) return;
    var len = 0;
    parts.forEach(function (p) { len += p.length; });
    var msg = new Uint8Array(len), off = 0;
    parts.forEach(function (p) { msg.set(p, off); off += p.length; });
    parts = [];
    var text = function () { return new TextDecoder().decode(msg); };
    switch (type) {
      case REPLY: var r = JSON.parse(text()); settle(r.id, null, r); break;
      case STATUS: emit('status', JSON.parse(text())); break;
      case HISTORY: emit('history', unpackHistory(new DataView(msg.buffer))); break;
      case CLOCK: emit('clock', Number(new DataView(msg.buffer).getBigInt64(0, true))); break;
    }
  }

  // The packed history stream (ble.cpp) as the JSON history reply.
  function unpackHistory(d) {
    var b = { first: d.getUint32(0, true), next: d.getUint32(4, true), lost: d.getUint32(8, true),
              reset: !!d.getUint8(12), now: Number(d.getBigInt64(13, true)), r: [] };
    for (var o = 21; o + 26 <= d.byteLength; o += 26) {
      b.r.push([d.getUint8(o), Number(d.getBigInt64(o + 8, true)), d.getInt32(o + 4, true), d.getUint16(o + 2, true),
                d.getInt32(o + 16, true), d.getUint8(o + 1), d.getUint32(o + 20, true), d.getUint16(o + 24, true)]);
    }
    return b;
  }

  function bleConnect() {
    return device.gatt.connect().then(function (gatt) { return gatt.getPrimaryService(SERVICE); }).then(function (svc) {
      return Promise.all([svc.getCharacteristic(REQUEST), svc.getCharacteristic(EVENTS)]);
    }).then(function (cs) {
      parts = [];
      cs[1].addEventListener('characteristicvaluechanged', received);
      return cs[1].startNotifications().then(function () {
        request = cs[0];
        writes = Promise.resolve();
        board.connected = true;
        emit('open');
      });
    });
  }

  // Keeps trying to get back to the board it had, without asking again.
  function reconnect() {
    if (!device || board.connected) return;
    bleConnect().catch(function () { setTimeout(reconnect, 3000); });
  }

  function use(d) {
    if (device == d) return reconnect();
    device = d;
    device.addEventListener('gattserverdisconnected', function () {
      request = null;
      board.connected = false;
      Object.keys(waiting).forEach(function (id) { settle(id, lostError()); });
      emit('close');
      setTimeout(reconnect, 1000);
    });
    reconnect();
  }

  // Asks the user to pick the board. Call from a tap.
  board.pick = function () {
    return navigator.bluetooth.requestDevice({ filters: [{ services: [SERVICE] }] }).then(use);
  };

  function bleListen() {
    // A board picked before on this phone, if the browser lets us at it
    // without asking again.
    if (navigator.bluetooth.getDevices) {
      navigator.bluetooth.getDevices().then(function (ds) { if (ds.length) use(ds[0]); }, function () {});
    }
  }

  board.call = board.kind == 'ble' ? bleCall : httpCall;
  board.listen = function (handlers) { h = handlers; if (board.kind == 'ble') bleListen(); else httpListen(); };
  return board;
})();
