#include "wifi_text_input.h"
#include "text_queue.h"
#include "params.h"
#include "priming.h"
#include "trace.h"
#include "test_pattern.h"
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

// This core is otherwise idle on this board (see example_valve_control.cpp's
// core-assignment comment) and keeping HTTP off VALVE_CORE means it can never
// jitter valve timing.
#define WEB_CORE 0

static WebServer server(80);

static const char PAGE_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head><title>BigDripper</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
body{font-family:sans-serif;max-width:480px;margin:2em auto;padding:0 1em}
input[type=text]{width:100%;font-size:1.2em;padding:.4em;box-sizing:border-box}
input[type=number]{width:5em;font-size:1.2em;padding:.3em}
button{font-size:1.2em;padding:.5em 1.5em;margin-top:.5em}
ol{padding-left:1.3em} li{margin:.2em 0}
.current{font-weight:bold}
.empty{color:#888;font-style:italic}
.priming button{background:#c0392b;color:#fff}
.priming-status{font-weight:bold}
</style></head><body>
<h1>BigDripper</h1>

<p>Priming: <span class="priming-status">%PRIMING%</span></p>
<form class="priming" method="POST" action="/prime/start">
<button type="submit">Prime system</button>
</form>
<form method="POST" action="/prime/stop">
<button type="submit">Priming finished</button>
</form>

<p>Trace: <span class="priming-status">%TRACE%</span></p>
<form method="POST" action="/trace/start">
<button type="submit">Trace on</button>
</form>
<form method="POST" action="/trace/stop">
<button type="submit">Trace off</button>
</form>

<p>Test pattern: <span class="priming-status">%TEST%</span></p>
<form method="POST" action="/test/start">
<label>Vertical line every
<input type="number" name="spacing" min="1" value="%TESTSPACING%"> columns</label><br>
<button type="submit">Lines on</button>
</form>
<form method="POST" action="/test/stop">
<button type="submit">Test off</button>
</form>

<p>Now printing:</p>
<p class="current">%CURRENT%</p>

<p>Queued:</p>
%PENDING%

<form method="POST" action="/set">
<input type="text" name="text" maxlength="%MAXLEN%" autofocus placeholder="Text to print">
<button type="submit">Add to queue</button>
</form>

<p><a href="/params">Settings</a></p>
</body></html>
)HTML";

static const char PARAMS_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head><title>BigDripper settings</title>
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
<label>Max text length (characters)
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

static String html_escape(const String &s) {
  String out = s;
  out.replace("&", "&amp;");
  out.replace("<", "&lt;");
  out.replace(">", "&gt;");
  return out;
}

static void handle_root() {
  String current = text_queue_current();
  std::vector<String> pending = text_queue_pending();

  String pending_html;
  if (pending.empty()) {
    pending_html = "<p class=\"empty\">(nothing queued)</p>";
  } else {
    pending_html = "<ol>";
    for (const String &t : pending) {
      pending_html += "<li>" + html_escape(t) + "</li>";
    }
    pending_html += "</ol>";
  }

  String page = FPSTR(PAGE_HTML);
  String current_html;
  if (current.length()) {
    current_html = html_escape(current);
  } else if (text_queue_is_idle()) {
    current_html = html_escape(TEXT_QUEUE_DEFAULT_TEXT) + " <span class=\"empty\">(default, on repeat)</span>";
  } else {
    current_html = "<span class=\"empty\">(pause)</span>";
  }
  page.replace("%CURRENT%", current_html);
  page.replace("%PENDING%", pending_html);
  page.replace("%MAXLEN%", String(params_get_max_text_len()));
  page.replace("%PRIMING%", priming_is_active() ? "ON" : "off");
  page.replace("%TRACE%", trace_is_active() ? "ON" : "off");
  page.replace("%TEST%", test_pattern_is_active() ? "ON" : "off");
  page.replace("%TESTSPACING%", String(params_get_test_line_spacing()));
  server.send(200, "text/html", page);
}

static void handle_params_page() {
  if (!server.authenticate(PARAMS_USER, PARAMS_PASSWORD)) {
    return server.requestAuthentication();
  }
  String page = FPSTR(PARAMS_HTML);
  page.replace("%SCALE%", String(params_get_scale_mm_per_detent(), 4));
  page.replace("%MAXLEN%", String(params_get_max_text_len()));
  page.replace("%PAUSEMS%", String(params_get_print_pause_ms()));
  page.replace("%BURSTMS%", String(params_get_column_burst_ms()));
  page.replace("%BURSTDUTY%", String(params_get_burst_duty_pct()));
  page.replace("%MINBURSTMS%", String(params_get_min_burst_ms()));
  page.replace("%TRACEGAPMS%", String(params_get_trace_gap_ms()));
  server.send(200, "text/html", page);
}

static void handle_params_set() {
  if (!server.authenticate(PARAMS_USER, PARAMS_PASSWORD)) {
    return server.requestAuthentication();
  }
  if (server.hasArg("scale")) {
    params_set_scale_mm_per_detent(server.arg("scale").toFloat());
  }
  if (server.hasArg("maxlen")) {
    long v = server.arg("maxlen").toInt();
    if (v > 0) {
      params_set_max_text_len((size_t)v);
    }
  }
  if (server.hasArg("pausems")) {
    long v = server.arg("pausems").toInt();
    if (v >= 0) {
      params_set_print_pause_ms((uint32_t)v);
    }
  }
  if (server.hasArg("burstms")) {
    long v = server.arg("burstms").toInt();
    if (v >= 0) {
      params_set_column_burst_ms((uint32_t)v);
    }
  }
  // Duty and min-burst are validated together against the (possibly
  // just-updated) max burst above, rather than independently like the fields
  // above -- a min burst only makes sense relative to the current ceiling.
  if (server.hasArg("burstduty") || server.hasArg("minburstms")) {
    uint32_t max_burst_ms = params_get_column_burst_ms();
    long duty = server.hasArg("burstduty") ? server.arg("burstduty").toInt()
                                            : (long)params_get_burst_duty_pct();
    long min_burst = server.hasArg("minburstms") ? server.arg("minburstms").toInt()
                                                  : (long)params_get_min_burst_ms();
    if (duty < 5 || duty > 95) {
      server.send(400, "text/plain", "Burst duty must be between 5 and 95.");
      return;
    }
    if (min_burst < 1 || min_burst > (long)max_burst_ms) {
      server.send(400, "text/plain",
                  "Min column burst must be between 1 and the max column burst (" +
                      String(max_burst_ms) + " ms).");
      return;
    }
    if (server.hasArg("burstduty")) {
      params_set_burst_duty_pct((uint32_t)duty);
    }
    if (server.hasArg("minburstms")) {
      params_set_min_burst_ms((uint32_t)min_burst);
    }
  }
  if (server.hasArg("tracegapms")) {
    long v = server.arg("tracegapms").toInt();
    if (v >= 0) {
      params_set_trace_gap_ms((uint32_t)v);
    }
  }
  log_i("parameters updated");
  server.sendHeader("Location", "/params");
  server.send(303);
}

static void handle_set() {
  if (server.hasArg("text")) {
    String t = server.arg("text");
    if (text_queue_push(t)) {
      log_i("queued \"%s\"", t.c_str());
    } else {
      log_w("rejected \"%s\" (empty, too long, or queue full)", t.c_str());
    }
  }
  // 303 -> GET / so a page refresh doesn't resubmit the form.
  server.sendHeader("Location", "/");
  server.send(303);
}

static void handle_prime_start() {
  priming_start();
  log_i("priming started, all valves open");
  server.sendHeader("Location", "/");
  server.send(303);
}

static void handle_prime_stop() {
  priming_stop();
  log_i("priming finished, all valves closed");
  server.sendHeader("Location", "/");
  server.send(303);
}

static void handle_trace_start() {
  trace_start();
  log_i("trace started, pulsing all valves for %lu ms with %lu ms between bursts",
        (unsigned long)params_get_column_burst_ms(),
        (unsigned long)params_get_trace_gap_ms());
  server.sendHeader("Location", "/");
  server.send(303);
}

static void handle_trace_stop() {
  trace_stop();
  log_i("trace stopped");
  server.sendHeader("Location", "/");
  server.send(303);
}

// Also how the spacing is changed: pressing "Lines on" again while the
// pattern runs applies the new spacing from the next column on.
static void handle_test_start() {
  if (server.hasArg("spacing")) {
    long v = server.arg("spacing").toInt();
    if (v < 1) {
      server.send(400, "text/plain", "Line spacing must be at least 1 column.");
      return;
    }
    params_set_test_line_spacing((uint32_t)v);
  }
  test_pattern_start();
  log_i("test pattern started, vertical line every %lu columns",
        (unsigned long)params_get_test_line_spacing());
  server.sendHeader("Location", "/");
  server.send(303);
}

static void handle_test_stop() {
  test_pattern_stop();
  log_i("test pattern stopped");
  server.sendHeader("Location", "/");
  server.send(303);
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
  server.on("/prime/start", HTTP_POST, handle_prime_start);
  server.on("/prime/stop", HTTP_POST, handle_prime_stop);
  server.on("/trace/start", HTTP_POST, handle_trace_start);
  server.on("/trace/stop", HTTP_POST, handle_trace_stop);
  server.on("/test/start", HTTP_POST, handle_test_start);
  server.on("/test/stop", HTTP_POST, handle_test_stop);
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
