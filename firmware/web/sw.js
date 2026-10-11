// Keeps the hosted page (Cloudflare) working without internet once it's
// been opened: every request goes to the network first, and falls back to
// the last copy kept. Not used on the board itself (plain HTTP can't run
// service workers).
var CACHE = 'bigdripper';
var FILES = ['./', 'index.html', 'style.css', 'transport.js', 'app.js', 'monitor.js', 'settings.js',
             'sim.js', 'simulator.js', 'sim.wasm'];

self.addEventListener('install', function (e) {
  e.waitUntil(caches.open(CACHE).then(function (c) { return c.addAll(FILES); }).then(function () { return self.skipWaiting(); }));
});
self.addEventListener('activate', function (e) { e.waitUntil(self.clients.claim()); });

self.addEventListener('fetch', function (e) {
  if (e.request.method != 'GET' || new URL(e.request.url).origin != location.origin) return;
  e.respondWith(fetch(e.request).then(function (r) {
    if (r.ok) {
      var copy = r.clone();
      caches.open(CACHE).then(function (c) { c.put(e.request, copy); });
    }
    return r;
  }, function () {
    return caches.match(e.request, { ignoreSearch: true });
  }));
});
