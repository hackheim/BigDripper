#include "wifi_text_input.h"
#include "text_queue.h"
#include "params.h"
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
button{font-size:1.2em;padding:.5em 1.5em;margin-top:.5em}
ol{padding-left:1.3em} li{margin:.2em 0}
.current{font-weight:bold}
.empty{color:#888;font-style:italic}
</style></head><body>
<h1>BigDripper</h1>

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
  page.replace("%CURRENT%",
               current.length() ? html_escape(current) : "<span class=\"empty\">(none)</span>");
  page.replace("%PENDING%", pending_html);
  page.replace("%MAXLEN%", String(params_get_max_text_len()));
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
