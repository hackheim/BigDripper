#include "wifi_text_input.h"
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>

// Open the AP config page at http://bigdripper.local/ (or http://192.168.4.1/
// if the client doesn't resolve mDNS) after joining this network from a phone
// or laptop.
static const char *AP_SSID = "BigDripper";
static const char *AP_PASSWORD = "dripdrip1";  // WPA2 needs >=8 chars
static const char *MDNS_HOSTNAME = "bigdripper";

// This core is otherwise idle on this board (see example_valve_control.cpp's
// core-assignment comment) and keeping HTTP off VALVE_CORE means it can never
// jitter valve timing.
#define WEB_CORE 0

static WebServer server(80);
static SemaphoreHandle_t text_mutex;
static String print_text = "";

static const char PAGE_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head><title>BigDripper</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
body{font-family:sans-serif;max-width:480px;margin:2em auto;padding:0 1em}
input[type=text]{width:100%;font-size:1.2em;padding:.4em;box-sizing:border-box}
button{font-size:1.2em;padding:.5em 1.5em;margin-top:.5em}
</style></head><body>
<h1>BigDripper</h1>
<p>Text to print on the ground:</p>
<form method="POST" action="/set">
<input type="text" name="text" maxlength="64" value="%CURRENT%" autofocus>
<button type="submit">Set</button>
</form>
</body></html>
)HTML";

String get_print_text() {
  xSemaphoreTake(text_mutex, portMAX_DELAY);
  String t = print_text;
  xSemaphoreGive(text_mutex);
  return t;
}

static void handle_root() {
  String page = FPSTR(PAGE_HTML);
  page.replace("%CURRENT%", get_print_text());
  server.send(200, "text/html", page);
}

static void handle_set() {
  if (server.hasArg("text")) {
    String t = server.arg("text");
    xSemaphoreTake(text_mutex, portMAX_DELAY);
    print_text = t;
    xSemaphoreGive(text_mutex);
    log_i("print text set to \"%s\"", t.c_str());
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
  server.begin();

  for (;;) {
    server.handleClient();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void wifi_text_input_begin() {
  text_mutex = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(web_task, "web", 8192, NULL, 1, NULL, WEB_CORE);
}
