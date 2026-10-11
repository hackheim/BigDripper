#include "wifi_text_input.h"
#include "printer.h"
#include "params.h"
#include "mode.h"
#include "font_data.h"
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>

// Open the AP config page at http://bigdripper.local/ (or http://192.168.4.1/
// if the client doesn't resolve mDNS) after joining this network from a phone
// or laptop.
static const char *AP_SSID = "BigDripper";
static const char *AP_PASSWORD = "dripdrip1";  // WPA2 needs >=8 chars
static const char *MDNS_HOSTNAME = "bigdripper";

// Separate from the AP password: this gates /params, not the network itself.
static const char *PARAMS_USER = "admin";
static const char *PARAMS_PASSWORD = "letmeprint9";

// Off PRINT_CORE (printer.h), with WiFi itself, so HTTP can never jitter
// valve timing.
#define WEB_CORE 0

static WebServer server(80);

// Mobile-first: a toolbar fixed to the top with one toggle per mode, and
// everything else below it. The toolbar and the now-printing/queue section
// are drawn by the script from /status JSON (seeded inline as %STATUS% so the
// first paint is already right) and kept in sync by polling, so a mode
// change from another phone, the queue advancing or a reboot all show up
// without a reload. Everything is inline: phones on the AP have no internet.
static const char PAGE_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head><meta charset="utf-8"><title>BigDripper</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
*{box-sizing:border-box}
body{font-family:sans-serif;margin:0;background:#fff;color:#111;font-size:17px}
#bar{position:sticky;top:0;z-index:1;display:flex;gap:4px;padding:4px;background:#222}
#bar button{flex:1;min-width:0;min-height:56px;border:0;border-radius:6px;color:#fff;
font-size:1em;font-weight:bold;line-height:1.2;padding:4px 0;background:#c0392b}
#bar button[aria-pressed=true]{background:#1e8e3e}
#bar small{display:block;font-size:.7em;font-weight:normal}
#dry{display:none;background:#c0392b;color:#fff;text-align:center;padding:.4em;font-size:.9em;font-weight:bold}
#offline{display:none;background:#f1c40f;color:#111;text-align:center;padding:.4em;font-size:.9em}
main{max-width:480px;margin:0 auto;padding:0 16px 2em}
h2{font-size:1em;margin:1.4em 0 .4em;color:#555}
input{font-size:1.1em;padding:.5em;border:1px solid #888;border-radius:4px}
input[type=text]{width:100%}
input[type=number]{width:5em}
main button{font-size:1.1em;padding:.6em 1.2em;margin-top:.5em;min-height:48px}
ol{padding-left:1.5em;margin:0} li{margin:.3em 0;word-break:break-word}
#cur{font-weight:bold;word-break:break-word}
.empty{color:#888;font-style:italic;font-weight:normal}
#lines{display:none;margin-top:1em}
.hint{color:#555;font-size:.9em}
code{background:#eee;padding:0 .2em}
#fontrow{display:flex;gap:8px;margin:.4em 0}
#fontrow button{min-width:0;min-height:44px;margin:0;padding:0 .3em;font-size:.95em;
border:2px solid #2c3e91;background:#fff;color:#2c3e91}
#fonts{display:flex;flex:3}
#fonts button{flex:1}
#fonts button+button{border-left:0}
#fonts button:first-child{border-radius:6px 0 0 6px}
#fonts button:last-child{border-radius:0 6px 6px 0}
#fontrow button[aria-pressed=true]{background:#2c3e91;color:#fff}
#invert{flex:1;border-radius:6px}
#text.inv{background:#111;color:#fff}
#text.bold{font-weight:bold}
.font{color:#888;font-weight:normal}
#left{font-size:.9em;color:#555;margin:.3em 0}
#left.over{color:#c0392b;font-weight:bold}
#emoji{display:flex;flex-wrap:wrap;gap:6px;margin:.4em 0}
#emoji button{width:48px;height:48px;min-height:0;margin:0;padding:0;border:1px solid #888;border-radius:6px;background:#fff}
#emoji canvas{width:32px;height:32px;display:block;margin:auto}
</style></head><body>
<div id="bar">
<button data-m="prime">Prime<small>OFF</small></button>
<button data-m="trace">Trace<small>OFF</small></button>
<button data-m="lines">Lines<small>OFF</small></button>
<button data-m="text">Text<small>OFF</small></button>
</div>
<div id="dry">DRY RUN &mdash; valves disabled</div>
<div id="offline">Not connected to BigDripper</div>
<main>
<form id="lines">
<label>Vertical line every
<input type="number" id="spacing" name="spacing" min="1"> columns</label>
<button type="submit">Save</button>
</form>

<h2>Now printing</h2>
<p id="cur"></p>

<h2>Queued</h2>
<div id="pend"></div>

<form method="POST" action="/set">
<h2>Add text</h2>
<div id="fontrow">
<div id="fonts" role="group" aria-label="Font">
<button type="button" data-f="spleen">Spleen</button>
<button type="button" data-f="drip">Drip</button>
<button type="button" data-f="drip_bold">Drip bold</button>
</div>
<button type="button" id="invert" aria-pressed="false" title="Print dry letters in a sprayed band">Invert</button>
</div>
<input type="hidden" id="font" name="font" value="drip">
<input type="hidden" id="inv" name="invert" value="0">
<input type="text" id="text" name="text" placeholder="Text to print" autocomplete="off">
<p id="left"></p>
<div id="emoji"></div>
<button type="submit" id="add">Add to queue</button>
</form>
<p class="hint">Letters A&ndash;Z, &AElig;&Oslash;&Aring;, 0&ndash;9 and <code>. , ! ? - : '</code>.
Spleen only has A&ndash;Z, 0&ndash;9 and space. Anything a font
doesn&rsquo;t have prints as a space.</p>

<p><a href="/params">Settings</a></p>
</main>
<script>
var S=%STATUS%,MAX=%MAXLEN%,CODES=[];
var $=function(id){return document.getElementById(id)};
var btns=document.querySelectorAll('#bar button');
function el(tag,cls,txt){var e=document.createElement(tag);if(cls)e.className=cls;e.textContent=txt;return e}
function draw(s){
  S=s;
  btns.forEach(function(b){
    var on=b.dataset.m==s.mode;
    b.setAttribute('aria-pressed',on);
    b.querySelector('small').textContent=on?'ON':'OFF';
  });
  $('lines').style.display=s.mode=='lines'?'block':'none';
  $('dry').style.display=s.dry_run?'block':'none';
  if(document.activeElement!=$('spacing'))$('spacing').value=s.lines_spacing;
  var c=$('cur');c.textContent='';
  if(s.mode=='off')c.appendChild(el('span','empty','(nothing — all modes off)'));
  else if(s.mode=='prime')c.appendChild(el('span','empty','(priming — all valves open)'));
  else if(s.mode=='trace')c.appendChild(el('span','empty','(trace — pulsing all valves)'));
  else if(s.mode=='lines')c.appendChild(el('span','empty','(vertical line every '+s.lines_spacing+' columns)'));
  else if(s.paused)c.appendChild(el('span','empty','(pause)'));
  else{c.textContent=s.current.text;c.appendChild(el('span','font',label(s.current)));
    if(s.default)c.appendChild(el('span','empty',' (default, on repeat)'))}
  var p=$('pend');p.textContent='';
  if(!s.pending.length)p.appendChild(el('p','empty','(nothing queued)'));
  else{var ol=document.createElement('ol');s.pending.forEach(function(m){var li=el('li','',m.text);li.appendChild(el('span','font',label(m)));ol.appendChild(li)});p.appendChild(ol)}
}
function label(m){return ' \u00b7 '+m.font+(m.invert?' \u00b7 inverted':'')}
function online(ok){$('offline').style.display=ok?'none':'block'}
function req(url,body){
  return fetch(url,body?{method:'POST',body:new URLSearchParams(body)}:{cache:'no-store'})
    .then(function(r){if(!r.ok)throw r;return r.json()})
    .then(function(s){online(true);draw(s)},function(){online(false)});
}
btns.forEach(function(b){b.onclick=function(){
  req('/mode',{mode:b.dataset.m==S.mode?'off':b.dataset.m});
}});
$('lines').onsubmit=function(e){
  e.preventDefault();$('spacing').blur();
  req('/lines/spacing',{spacing:$('spacing').value});
};
var timer;
function poll(){clearTimeout(timer);if(document.hidden)return;
  req('/status').then(function(){timer=setTimeout(poll,1500)})}
document.addEventListener('visibilitychange',poll);
// Same rules as font_glyph_count(): an emoji shortcode (longest match,
// any case) is 1, any other character is 1. The server check is
// the real one; this is feedback while typing.
function glyphs(t){
  var n=0,i=0,lo=t.replace(/[A-Z]/g,function(c){return c.toLowerCase()});
  while(i<t.length){
    var len=0;
    CODES.forEach(function(c){if(c.length>len&&lo.startsWith(c,i))len=c.length});
    if(len){n++;i+=len;continue}
    n++;
    i+=t.codePointAt(i)>0xffff?2:1;
  }
  return n;
}
var txt=$('text');
function count(){
  var left=MAX-glyphs(txt.value);
  $('left').textContent=left+' left';
  $('left').className=left<0?'over':'';
  $('add').disabled=left<0;
}
txt.oninput=count;
// Phones blur the input when a button is tapped, so remember where the
// cursor was and insert there.
var sel=[0,0];
function keep(){if(document.activeElement==txt)sel=[txt.selectionStart,txt.selectionEnd]}
document.addEventListener('selectionchange',keep);
['keyup','click','input','blur'].forEach(function(e){txt.addEventListener(e,function(){sel=[txt.selectionStart,txt.selectionEnd]})});
function insert(code){
  var v=txt.value,a=Math.min(sel[0],v.length),b=Math.min(sel[1],v.length);
  txt.value=v.slice(0,a)+code+v.slice(b);
  a+=code.length;sel=[a,a];
  txt.focus();txt.setSelectionRange(a,a);
  count();
}
function emojiButton(e){
  var b=document.createElement('button'),cv=document.createElement('canvas'),px=2,r=window.devicePixelRatio||1;
  b.type='button';b.title=e.code;b.setAttribute('aria-label',e.code);
  cv.width=cv.height=16*px*r;
  var g=cv.getContext('2d');g.scale(r,r);g.fillStyle='#000';
  e.columns.forEach(function(c,x){for(var y=0;y<16;y++)if(c&(0x8000>>y))g.fillRect(x*px,y*px,px,px)});
  b.appendChild(cv);
  // preventDefault keeps focus (and the phone keyboard) on the text box.
  b.onpointerdown=function(ev){ev.preventDefault()};
  b.onclick=function(){insert(e.code)};
  return b;
}
// Font selector and Invert toggle, remembered on this phone. Storage can throw (private
// mode, blocked site data); the page then just starts at Drip, not
// inverted, each time.
var fbtns=document.querySelectorAll('#fonts button');
function style(){
  txt.className=($('font').value=='drip_bold'?'bold ':'')+($('inv').value=='1'?'inv':'');
}
function pickFont(f){
  fbtns.forEach(function(b){b.setAttribute('aria-pressed',b.dataset.f==f)});
  $('font').value=f;style();
  try{localStorage.setItem('font',f)}catch(e){}
}
function setInvert(on){
  $('invert').setAttribute('aria-pressed',on);
  $('inv').value=on?'1':'0';style();
  try{localStorage.setItem('invert',on?'1':'0')}catch(e){}
}
$('invert').onclick=function(){setInvert($('inv').value!='1')};
fbtns.forEach(function(b){b.onclick=function(){pickFont(b.dataset.f)}});
var saved=null;
try{saved=localStorage.getItem('font')}catch(e){}
pickFont(['spleen','drip','drip_bold'].indexOf(saved)>=0?saved:'drip');
saved=null;
try{saved=localStorage.getItem('invert')}catch(e){}
setInvert(saved=='1');
fetch('/emoji.json').then(function(r){return r.json()}).then(function(list){
  list.forEach(function(e){CODES.push(e.code.toLowerCase());$('emoji').appendChild(emojiButton(e))});
  count();
}).catch(function(){});
count();
draw(S);timer=setTimeout(poll,1500);
</script>
</body></html>
)HTML";

static const char PARAMS_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head><meta charset="utf-8"><title>BigDripper settings</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
body{font-family:sans-serif;max-width:480px;margin:2em auto;padding:0 1em}
label{display:block;margin-top:1.2em}
input{width:100%;font-size:1.1em;padding:.4em;box-sizing:border-box}
button{font-size:1.2em;padding:.5em 1.5em;margin-top:1.5em}
</style></head><body>
<h1>Settings</h1>
<form method="POST" action="/params/set">
<label>Scale (mm of travel per encoder detent)
<input type="text" inputmode="decimal" name="scale" value="%SCALE%"></label>
<label>Max text length (printed characters, emoji count as 1)
<input type="number" name="maxlen" min="1" max="255" value="%MAXLEN%"></label>
<label>Pause between prints (ms)
<input type="number" name="pausems" min="0" value="%PAUSEMS%"></label>
<label>Max column burst (ms)
<input type="number" name="burstms" min="0" value="%BURSTMS%"></label>
<label>Burst duty (% of column period, 5-95)
<input type="number" name="burstduty" min="5" max="95" value="%BURSTDUTY%"></label>
<label>Min column burst (ms)
<input type="number" name="minburstms" min="1" value="%MINBURSTMS%"></label>
<label>Trace delay between bursts (ms)
<input type="number" name="tracegapms" min="0" value="%TRACEGAPMS%"></label>
<button type="submit">Save</button>
</form>
<p><a href="/">&larr; Back</a></p>
</body></html>
)HTML";

// JSON string literal for s, quotes included. Escapes " and \ and control
// characters; everything else (UTF-8 included) passes through as-is, since
// the page inserts these with textContent, never as HTML.
static String json_escape(const String &s) {
  String out = "\"";
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if ((uint8_t)c < 0x20) {
      char buf[7];
      snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)(uint8_t)c);
      out += buf;
    } else {
      out += c;
    }
  }
  out += '"';
  return out;
}

// {"text":...,"font":"Drip bold","invert":false} for one message.
static String message_json(const drip::MessageInfo &m) {
  return "{\"text\":" + json_escape(String(m.text.c_str())) +
         ",\"font\":" + json_escape(font_label((Font)m.font)) +
         ",\"invert\":" + (m.invert ? "true" : "false") + "}";
}

// What /status returns, and what /mode and /lines/spacing reply with.
// "current" is null during the pause between prints.
static String status_json() {
  drip::Status st = printer_status();

  String json = "{\"mode\":\"";
  json += mode_name(st.mode);
  json += "\",\"current\":";
  json += st.paused ? String("null") : message_json(st.current);
  json += ",\"default\":";
  json += st.printing_default ? "true" : "false";
  json += ",\"paused\":";
  json += st.paused ? "true" : "false";
  json += ",\"pending\":[";
  for (size_t i = 0; i < st.pending.size(); i++) {
    if (i) json += ',';
    json += message_json(st.pending[i]);
  }
  json += "],\"lines_spacing\":" + String(params_get().test_line_spacing);
#ifdef CONSOLE_PREVIEW_DRY_RUN
  // Only in dry-run builds (Ticket 7.2), so the page can say so.
  json += ",\"dry_run\":true";
#endif
  json += "}";
  return json;
}

static void send_status() {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", status_json());
}

// The emoji the font has, for the page's buttons and its length counter.
// Built from FONT_EMOJI so it can't drift from what actually prints.
// Columns are as stored (bit 15 = top row), not mapped onto coils, so the
// page can draw them the right way up.
static void handle_emoji_json() {
  String json = "[";
  for (size_t i = 0; i < FONT_EMOJI_COUNT; i++) {
    const FontGlyph &g = FONT_EMOJI[i];
    if (i) json += ',';
    json += "{\"code\":" + json_escape(g.token) + ",\"width\":" + String(g.width) + ",\"columns\":[";
    for (uint8_t x = 0; x < g.width; x++) {
      if (x) json += ',';
      json += String(g.columns[x]);
    }
    json += "]}";
  }
  json += "]";
  // Only changes with a reflash.
  server.sendHeader("Cache-Control", "max-age=3600");
  server.send(200, "application/json", json);
}

static void handle_root() {
  String page = FPSTR(PAGE_HTML);
  page.replace("%MAXLEN%", String(params_get().max_text_len));
  // Last, so user text in the status can't be mistaken for a placeholder.
  // '<' only occurs inside JSON strings, and escaping it there keeps a
  // queued "</script>" from ending the script block early.
  String status = status_json();
  status.replace("<", "\\u003c");
  page.replace("%STATUS%", status);
  // no-store so a phone never shows a stale mode from a cached page after a
  // reboot.
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html; charset=utf-8", page);
}

static void handle_params_page() {
  if (!server.authenticate(PARAMS_USER, PARAMS_PASSWORD)) {
    return server.requestAuthentication();
  }
  Settings s = params_get();
  String page = FPSTR(PARAMS_HTML);
  page.replace("%SCALE%", String(s.scale_mm_per_detent, 4));
  page.replace("%MAXLEN%", String(s.max_text_len));
  page.replace("%PAUSEMS%", String(s.print_pause_ms));
  page.replace("%BURSTMS%", String(s.column_burst_ms));
  page.replace("%BURSTDUTY%", String(s.burst_duty_pct));
  page.replace("%MINBURSTMS%", String(s.min_burst_ms));
  page.replace("%TRACEGAPMS%", String(s.trace_gap_ms));
  server.send(200, "text/html; charset=utf-8", page);
}

// Reads whole-number form field `name` into *out, if it was sent. False if
// it was sent but isn't a number >= 0.
static bool arg_uint(const char *name, uint32_t *out) {
  if (!server.hasArg(name)) return true;
  String v = server.arg(name);
  v.trim();
  if (v.length() == 0) return false;
  for (size_t i = 0; i < v.length(); i++) {
    if (!isDigit(v[i])) return false;
  }
  *out = (uint32_t)v.toInt();
  return true;
}

// Saves a new set of settings and hands them to the print engine, or
// changes nothing and returns false (having sent the 400) if they don't
// hold together.
static bool apply_settings(const Settings &s) {
  const char *err = params_check(s);
  if (err != nullptr) {
    server.send(400, "text/plain", err);
    return false;
  }
  params_set(s);
  printer_set_params(params_to_engine(s));
  return true;
}

// Every field is read first (missing ones keep their saved value) and the
// result checked as a whole, so a rejected form saves none of it.
static void handle_params_set() {
  if (!server.authenticate(PARAMS_USER, PARAMS_PASSWORD)) {
    return server.requestAuthentication();
  }
  Settings s = params_get();
  if (server.hasArg("scale")) {
    s.scale_mm_per_detent = server.arg("scale").toFloat();
  }
  if (!arg_uint("maxlen", &s.max_text_len) || !arg_uint("pausems", &s.print_pause_ms) ||
      !arg_uint("burstms", &s.column_burst_ms) || !arg_uint("burstduty", &s.burst_duty_pct) ||
      !arg_uint("minburstms", &s.min_burst_ms) || !arg_uint("tracegapms", &s.trace_gap_ms)) {
    server.send(400, "text/plain", "Every field must be a whole number, 0 or more.");
    return;
  }
  if (!apply_settings(s)) return;
  log_i("parameters updated");
  server.sendHeader("Location", "/params");
  server.send(303);
}

static void handle_set() {
  if (server.hasArg("text")) {
    String t = server.arg("text");
    // Missing or unknown font -> Drip, so a page cached from before the font
    // selector still works.
    Font font = Font::Drip;
    font_from_id(server.arg("font"), &font);
    // Only "1" inverts; missing (e.g. an older cached page) means normal.
    bool invert = server.arg("invert") == "1";
    EnqueueResult r = printer_enqueue(t, font, invert);
    log_i("\"%s\" in %s%s: %s", t.c_str(), font_label(font), invert ? ", inverted" : "",
          enqueue_result_name(r));
  }
  // 303 -> GET / so a page refresh doesn't resubmit the form.
  server.sendHeader("Location", "/");
  server.send(303);
}

// Sets exactly the requested mode rather than toggling, so two phones
// tapping at once can't flip each other back. The page works out "tap on the
// green button -> off".
static void handle_mode() {
  drip::Mode mode;
  if (!mode_from_name(server.arg("mode"), &mode)) {
    server.send(400, "text/plain", "mode must be one of off, prime, trace, lines, text.");
    return;
  }
  printer_set_mode(mode);
  send_status();
}

// Applies from the next column on, whether or not Lines is running.
static void handle_lines_spacing() {
  long v = server.arg("spacing").toInt();
  if (v < 1) {
    server.send(400, "text/plain", "Line spacing must be at least 1 column.");
    return;
  }
  Settings s = params_get();
  s.test_line_spacing = (uint32_t)v;
  if (!apply_settings(s)) return;
  log_i("test pattern: vertical line every %lu columns", (unsigned long)v);
  send_status();
}

static void web_task(void *arg) {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  log_i("AP \"%s\" up, connect and browse to %s", AP_SSID,
        WiFi.softAPIP().toString().c_str());

  // Responder only; nothing else queries mDNS on this network, but the ESP32
  // library requires begin() before addService() will advertise anything.
  if (MDNS.begin(MDNS_HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    log_i("mDNS up, browse to http://%s.local/", MDNS_HOSTNAME);
  } else {
    log_e("mDNS.begin() failed, use http://%s/ instead",
          WiFi.softAPIP().toString().c_str());
  }

  server.on("/", HTTP_GET, handle_root);
  server.on("/set", HTTP_POST, handle_set);
  server.on("/status", HTTP_GET, send_status);
  server.on("/emoji.json", HTTP_GET, handle_emoji_json);
  server.on("/mode", HTTP_POST, handle_mode);
  server.on("/lines/spacing", HTTP_POST, handle_lines_spacing);
  server.on("/params", HTTP_GET, handle_params_page);
  server.on("/params/set", HTTP_POST, handle_params_set);
  server.begin();

  for (;;) {
    server.handleClient();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void wifi_text_input_begin() {
  xTaskCreatePinnedToCore(web_task, "web", 8192, NULL, 1, NULL, WEB_CORE);
}
