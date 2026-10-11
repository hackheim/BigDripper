#include "web.h"
#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include "esp_timer.h"
#include "api.h"
#include "generated/web_assets.h"
#include "printer.h"

// Join this network from a phone or laptop, then browse to
// http://bigdripper.local/ (or http://192.168.4.1/ if the client doesn't
// resolve mDNS).
static const char *AP_SSID = "BigDripper";
static const char *AP_PASSWORD = "dripdrip1";  // WPA2 needs >=8 chars
static const char *MDNS_HOSTNAME = "bigdripper";

// Off PRINT_CORE (printer.h), with WiFi itself and the async server's own
// task (CONFIG_ASYNC_TCP_RUNNING_CORE in platformio.ini), so HTTP can never
// jitter valve timing.
#define WEB_CORE 0

// How often the event stream is fed, and the most history records per
// event. 10 Hz keeps the coil timeline smooth without flooding a phone.
static const uint32_t PUSH_INTERVAL_MS = 100;
static const size_t PUSH_MAX_RECORDS = 64;

// A "clock" event this often, so a page can keep its picture of board time
// in step and notice a dead connection even when nothing else happens.
static const uint32_t CLOCK_INTERVAL_MS = 2000;

static AsyncWebServer server(80);
static AsyncEventSource events("/api/events");

static String to_string(const JsonDocument &doc) {
  String out;
  serializeJson(doc, out);
  return out;
}

// --- Requests ------------------------------------------------------------------

static void send_json(AsyncWebServerRequest *req, int code, const JsonDocument &doc,
                      const char *cache = "no-store") {
  AsyncWebServerResponse *res = req->beginResponse(code, "application/json", to_string(doc));
  res->addHeader("Cache-Control", cache);
  req->send(res);
}

// Basic auth, checked but never asked for: a 401 comes without
// WWW-Authenticate, so the browser doesn't pop up its own password box over
// the page's.
static bool authorized(AsyncWebServerRequest *req) {
  return req->authenticate(API_SETTINGS_USER, API_SETTINGS_PASSWORD);
}

// Carries out API operation `op` (api.h) and sends its reply.
static void call(AsyncWebServerRequest *req, const char *op, JsonVariantConst args,
                 const char *cache = "no-store") {
  JsonDocument reply;
  int code = api_call(op, args, authorized(req), reply);
  send_json(req, code, reply, code == 200 ? cache : "no-store");
}

static void serve_asset(AsyncWebServerRequest *req, const WebAsset &a) {
  if (req->hasHeader("If-None-Match") && req->header("If-None-Match") == a.etag) {
    AsyncWebServerResponse *res = req->beginResponse(304);
    res->addHeader("ETag", a.etag);
    req->send(res);
    return;
  }
  AsyncWebServerResponse *res = req->beginResponse(200, a.content_type, a.gz, a.gz_len);
  res->addHeader("Content-Encoding", "gzip");
  res->addHeader("ETag", a.etag);
  // Always revalidated (cheap with the ETag), so a phone never runs an old
  // page against new firmware after a reflash.
  res->addHeader("Cache-Control", "no-cache");
  req->send(res);
}

static const WebAsset *find_asset(const char *path) {
  for (size_t i = 0; i < WEB_ASSET_COUNT; i++) {
    if (strcmp(WEB_ASSETS[i].path, path) == 0) return &WEB_ASSETS[i];
  }
  return nullptr;
}

static void history(AsyncWebServerRequest *req) {
  JsonDocument args;
  if (req->hasParam("since")) args["since"] = (uint32_t)strtoul(req->getParam("since")->value().c_str(), nullptr, 10);
  call(req, "history", args);
}

static void routes() {
  for (size_t i = 0; i < WEB_ASSET_COUNT; i++) {
    const WebAsset *a = &WEB_ASSETS[i];
    server.on(a->path, HTTP_GET, [a](AsyncWebServerRequest *req) { serve_asset(req, *a); });
  }
  const WebAsset *index = find_asset("/index.html");
  server.on("/", HTTP_GET, [index](AsyncWebServerRequest *req) { serve_asset(req, *index); });
  // The settings used to be a page of their own.
  server.on("/params", HTTP_GET, [](AsyncWebServerRequest *req) { req->redirect("/#settings"); });

  // Each route is one API operation (api.h); POST bodies are its arguments.
  JsonVariantConst none;
  server.on("/api/status", HTTP_GET, [none](AsyncWebServerRequest *req) { call(req, "status", none); });
  server.on("/api/emoji", HTTP_GET, [none](AsyncWebServerRequest *req) {
    call(req, "emoji", none, "max-age=3600");  // only changes with a reflash
  });
  server.on("/api/history", HTTP_GET, history);
  server.on("/api/settings", HTTP_GET, [none](AsyncWebServerRequest *req) { call(req, "settings", none); });

  // JSON bodies (Content-Type: application/json).
  server.on("/api/queue", HTTP_POST, [](AsyncWebServerRequest *req, JsonVariant &json) { call(req, "queue", json); });
  server.on("/api/mode", HTTP_POST, [](AsyncWebServerRequest *req, JsonVariant &json) { call(req, "mode", json); });
  server.on("/api/lines", HTTP_POST, [](AsyncWebServerRequest *req, JsonVariant &json) { call(req, "lines", json); });
  server.on("/api/settings", HTTP_POST,
            [](AsyncWebServerRequest *req, JsonVariant &json) { call(req, "save_settings", json); });

  // A new stream starts with the current status; history it fetches itself
  // (/api/history), from wherever it got to.
  events.onConnect([](AsyncEventSourceClient *client) {
    JsonDocument doc;
    api_status_json(doc);
    client->send(to_string(doc).c_str(), "status", 0, 2000);
  });
  server.addHandler(&events);

  server.onNotFound([](AsyncWebServerRequest *req) {
    JsonDocument doc;
    doc["error"] = "Not found.";
    send_json(req, 404, doc);
  });
}

// --- The web task --------------------------------------------------------------

// Feeds the event stream: the status when it changes, every new history
// record in order, and a clock tick. Reads the print task's history ring
// and status version without disturbing it; only a status change costs a
// round trip into the print task.
static void push_loop() {
  static Record recs[PUSH_MAX_RECORDS];
  uint32_t cursor = printer_history_head();
  uint32_t sent_version = printer_status_version();
  uint32_t last_clock = 0;

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(PUSH_INTERVAL_MS));
    if (events.count() == 0) {
      // Nobody listening: stay at the head, so the next listener isn't sent
      // a backlog it will fetch with /api/history anyway.
      cursor = printer_history_head();
      sent_version = printer_status_version();
      continue;
    }

    uint32_t version = printer_status_version();
    if (version != sent_version) {
      sent_version = version;
      JsonDocument doc;
      api_status_json(doc);
      events.send(to_string(doc).c_str(), "status");
    }

    for (int batch = 0; batch < 8; batch++) {
      History::Read rd = printer_history(cursor, recs, PUSH_MAX_RECORDS);
      if (rd.count == 0 && rd.lost == 0) break;
      JsonDocument doc;
      api_history_json(doc, rd, recs);
      events.send(to_string(doc).c_str(), "history");
      cursor = rd.next;
    }

    if (millis() - last_clock >= CLOCK_INTERVAL_MS) {
      last_clock = millis();
      JsonDocument doc;
      doc["now"] = esp_timer_get_time();
      events.send(to_string(doc).c_str(), "clock");
    }
  }
}

static void web_task(void *) {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  log_i("AP \"%s\" up, connect and browse to %s", AP_SSID, WiFi.softAPIP().toString().c_str());

  // Responder only; nothing else queries mDNS on this network, but the ESP32
  // library requires begin() before addService() will advertise anything.
  if (MDNS.begin(MDNS_HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    log_i("mDNS up, browse to http://%s.local/", MDNS_HOSTNAME);
  } else {
    log_e("mDNS.begin() failed, use http://%s/ instead", WiFi.softAPIP().toString().c_str());
  }

  routes();
  server.begin();
  push_loop();  // never returns
}

void web_begin() {
  xTaskCreatePinnedToCore(web_task, "web", 8192, NULL, 1, NULL, WEB_CORE);
}
