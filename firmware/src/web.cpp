#include "web.h"
#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include "esp_random.h"
#include "esp_timer.h"
#include "encoder.h"
#include "font_data.h"
#include "generated/web_assets.h"
#include "mode.h"
#include "params.h"
#include "printer.h"

// Join this network from a phone or laptop, then browse to
// http://bigdripper.local/ (or http://192.168.4.1/ if the client doesn't
// resolve mDNS).
static const char *AP_SSID = "BigDripper";
static const char *AP_PASSWORD = "dripdrip1";  // WPA2 needs >=8 chars
static const char *MDNS_HOSTNAME = "bigdripper";

// Separate from the AP password: this gates the settings, not the network.
static const char *PARAMS_USER = "admin";
static const char *PARAMS_PASSWORD = "letmeprint9";

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

// Random per boot, in every status: a page that sees it change knows the
// board rebooted and its history sequence numbers started over.
static uint32_t boot_id = 0;

// --- JSON ----------------------------------------------------------------------

static void message_json(JsonObject o, const drip::MessageInfo &m) {
  o["text"] = m.text.c_str();
  o["font"] = font_label((Font)m.font);
  o["invert"] = m.invert;
}

static void status_json(JsonDocument &doc) {
  drip::Status st = printer_status();
  Settings s = params_get();
  doc["boot"] = boot_id;
  doc["mode"] = mode_name(st.mode);
  if (st.paused) {
    doc["current"] = nullptr;
  } else {
    message_json(doc["current"].to<JsonObject>(), st.current);
  }
  doc["default"] = st.printing_default;
  doc["paused"] = st.paused;
  JsonArray pending = doc["pending"].to<JsonArray>();
  for (const drip::MessageInfo &m : st.pending) message_json(pending.add<JsonObject>(), m);
  doc["lines_spacing"] = s.test_line_spacing;
  doc["max_text_len"] = s.max_text_len;
  doc["mm_per_count"] = s.scale_mm_per_detent / ENCODER_COUNTS_PER_DETENT;
  doc["history_size"] = HISTORY_SIZE;
#ifdef CONSOLE_PREVIEW_DRY_RUN
  doc["dry_run"] = true;
#else
  doc["dry_run"] = false;
#endif
}

static void settings_json(JsonDocument &doc) {
  Settings s = params_get();
  doc["scale_mm_per_detent"] = s.scale_mm_per_detent;
  doc["max_text_len"] = s.max_text_len;
  doc["print_pause_ms"] = s.print_pause_ms;
  doc["clicks_per_column"] = s.clicks_per_column;
  doc["column_burst_ms"] = s.column_burst_ms;
  doc["burst_duty_pct"] = s.burst_duty_pct;
  doc["min_burst_ms"] = s.min_burst_ms;
  doc["trace_gap_ms"] = s.trace_gap_ms;
  doc["test_line_spacing"] = s.test_line_spacing;
  doc["letter_spacing"] = s.letter_spacing;
  doc["min_column_ms"] = s.min_column_ms;
  doc["max_stretch_pct"] = s.max_stretch_pct;
}

// One read of the history: what came back, and from where. Records are
// arrays to keep the stream small; see web/API.md for the fields.
static void history_json(JsonDocument &doc, const History::Read &rd, const Record *recs) {
  doc["first"] = rd.first;
  doc["next"] = rd.next;
  doc["lost"] = rd.lost;
  doc["reset"] = rd.reset;
  doc["now"] = esp_timer_get_time();
  JsonArray out = doc["r"].to<JsonArray>();
  for (size_t i = 0; i < rd.count; i++) {
    const Record &r = recs[i];
    JsonArray a = out.add<JsonArray>();
    a.add(r.kind);
    a.add(r.at_us);
    a.add(r.value);
    a.add(r.bits);
    a.add(r.open_us == Record::HELD ? -1 : (int64_t)r.open_us);
    a.add(r.overrun);
    a.add(lroundf(r.counts_per_s));
    a.add(r.stretch_pct);
  }
}

// The emoji the font has, for the page's buttons and its length counter.
// Built from FONT_EMOJI so it can't drift from what actually prints.
// Columns are as stored (bit 15 = top row), not mapped onto coils, so the
// page can draw them the right way up.
static void emoji_json(JsonDocument &doc) {
  JsonArray list = doc.to<JsonArray>();
  for (size_t i = 0; i < FONT_EMOJI_COUNT; i++) {
    const FontGlyph &g = FONT_EMOJI[i];
    JsonObject e = list.add<JsonObject>();
    e["code"] = g.token;
    e["width"] = g.width;
    JsonArray cols = e["columns"].to<JsonArray>();
    for (uint8_t x = 0; x < g.width; x++) cols.add(g.columns[x]);
  }
}

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

static void send_error(AsyncWebServerRequest *req, int code, const char *message) {
  JsonDocument doc;
  doc["error"] = message;
  send_json(req, code, doc);
}

static void send_status(AsyncWebServerRequest *req) {
  JsonDocument doc;
  status_json(doc);
  send_json(req, 200, doc);
}

static void send_settings(AsyncWebServerRequest *req) {
  JsonDocument doc;
  settings_json(doc);
  send_json(req, 200, doc);
}

static bool authorized(AsyncWebServerRequest *req) {
  if (req->authenticate(PARAMS_USER, PARAMS_PASSWORD)) return true;
  req->requestAuthentication(AsyncAuthType::AUTH_BASIC);
  return false;
}

// Reads whole-number field `name` into *out if it's there. False if it's
// there but isn't a whole number >= 0.
static bool field_uint(JsonObject body, const char *name, uint32_t *out) {
  JsonVariant v = body[name];
  if (v.isNull()) return true;
  if (!v.is<uint32_t>()) return false;
  *out = v.as<uint32_t>();
  return true;
}

// Saves a new set of settings and hands them to the print engine. On a
// problem, changes nothing and sends the 400.
static bool apply_settings(AsyncWebServerRequest *req, const Settings &s) {
  const char *err = params_check(s);
  if (err != nullptr) {
    send_error(req, 400, err);
    return false;
  }
  params_set(s);
  printer_set_settings(s);
  return true;
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

static void handle_queue(AsyncWebServerRequest *req, JsonVariant &json) {
  JsonObject body = json.as<JsonObject>();
  if (!body["text"].is<const char *>()) return send_error(req, 400, "text must be a string.");
  String text = body["text"].as<const char *>();
  Font font = Font::Drip;
  if (!body["font"].isNull() && !font_from_id(body["font"].as<String>(), &font)) {
    return send_error(req, 400, "font must be one of spleen, drip, drip_bold.");
  }
  bool invert = body["invert"] | false;

  EnqueueResult r = printer_enqueue(text, font, invert);
  log_i("\"%s\" in %s%s: %s", text.c_str(), font_label(font), invert ? ", inverted" : "",
        enqueue_result_name(r));
  switch (r) {
    case EnqueueResult::Ok:
      return send_status(req);
    case EnqueueResult::Empty:
      return send_error(req, 400, "Nothing to print.");
    case EnqueueResult::TooLong:
      return send_error(req, 400, "Too long.");
    case EnqueueResult::QueueFull:
      return send_error(req, 409, "The queue is full.");
  }
}

static void handle_mode(AsyncWebServerRequest *req, JsonVariant &json) {
  drip::Mode mode;
  if (!mode_from_name(json["mode"] | "", &mode)) {
    return send_error(req, 400, "mode must be one of off, prime, trace, lines, text.");
  }
  printer_set_mode(mode);
  send_status(req);
}

static void handle_lines(AsyncWebServerRequest *req, JsonVariant &json) {
  Settings s = params_get();
  JsonObject body = json.as<JsonObject>();
  if (body["spacing"].isNull() || !field_uint(body, "spacing", &s.test_line_spacing)) {
    return send_error(req, 400, "spacing must be a whole number of columns.");
  }
  if (!apply_settings(req, s)) return;
  send_status(req);
}

static void handle_settings_set(AsyncWebServerRequest *req, JsonVariant &json) {
  if (!authorized(req)) return;
  // Every field is read first (missing ones keep their saved value) and the
  // result checked as a whole, so a rejected request saves none of it.
  Settings s = params_get();
  JsonObject body = json.as<JsonObject>();
  JsonVariant scale = body["scale_mm_per_detent"];
  if (!scale.isNull()) {
    if (!scale.is<float>() || !(scale.as<float>() > 0)) {
      return send_error(req, 400, "scale_mm_per_detent must be a number above 0.");
    }
    s.scale_mm_per_detent = scale.as<float>();
  }
  if (!field_uint(body, "max_text_len", &s.max_text_len) || !field_uint(body, "print_pause_ms", &s.print_pause_ms) ||
      !field_uint(body, "clicks_per_column", &s.clicks_per_column) ||
      !field_uint(body, "column_burst_ms", &s.column_burst_ms) ||
      !field_uint(body, "burst_duty_pct", &s.burst_duty_pct) ||
      !field_uint(body, "min_burst_ms", &s.min_burst_ms) || !field_uint(body, "trace_gap_ms", &s.trace_gap_ms) ||
      !field_uint(body, "test_line_spacing", &s.test_line_spacing) ||
      !field_uint(body, "letter_spacing", &s.letter_spacing) ||
      !field_uint(body, "min_column_ms", &s.min_column_ms) ||
      !field_uint(body, "max_stretch_pct", &s.max_stretch_pct)) {
    return send_error(req, 400, "Settings other than the scale must be whole numbers, 0 or more.");
  }
  if (!apply_settings(req, s)) return;
  log_i("settings updated");
  send_settings(req);
}

static void handle_history(AsyncWebServerRequest *req) {
  uint32_t since = 0;
  if (req->hasParam("since")) since = (uint32_t)strtoul(req->getParam("since")->value().c_str(), nullptr, 10);
  static Record recs[HISTORY_SIZE];  // only ever used from the async server's task
  History::Read rd = printer_history(since, recs, HISTORY_SIZE);
  JsonDocument doc;
  history_json(doc, rd, recs);
  send_json(req, 200, doc);
}

static void routes() {
  for (size_t i = 0; i < WEB_ASSET_COUNT; i++) {
    const WebAsset *a = &WEB_ASSETS[i];
    // The settings page itself asks for the password, so the browser has
    // it for the settings API calls the page makes.
    bool locked = strcmp(a->path, "/params.html") == 0;
    server.on(a->path, HTTP_GET, [a, locked](AsyncWebServerRequest *req) {
      if (locked && !authorized(req)) return;
      serve_asset(req, *a);
    });
  }
  const WebAsset *index = find_asset("/index.html");
  const WebAsset *params = find_asset("/params.html");
  server.on("/", HTTP_GET, [index](AsyncWebServerRequest *req) { serve_asset(req, *index); });
  server.on("/params", HTTP_GET, [params](AsyncWebServerRequest *req) {
    if (authorized(req)) serve_asset(req, *params);
  });

  server.on("/api/status", HTTP_GET, send_status);
  server.on("/api/emoji", HTTP_GET, [](AsyncWebServerRequest *req) {
    JsonDocument doc;
    emoji_json(doc);
    send_json(req, 200, doc, "max-age=3600");  // only changes with a reflash
  });
  server.on("/api/history", HTTP_GET, handle_history);
  server.on("/api/settings", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (authorized(req)) send_settings(req);
  });

  // JSON bodies (Content-Type: application/json).
  server.on("/api/queue", HTTP_POST, handle_queue);
  server.on("/api/mode", HTTP_POST, handle_mode);
  server.on("/api/lines", HTTP_POST, handle_lines);
  server.on("/api/settings", HTTP_POST, handle_settings_set);

  // A new stream starts with the current status; history it fetches itself
  // (/api/history), from wherever it got to.
  events.onConnect([](AsyncEventSourceClient *client) {
    JsonDocument doc;
    status_json(doc);
    client->send(to_string(doc).c_str(), "status", 0, 2000);
  });
  server.addHandler(&events);

  server.onNotFound([](AsyncWebServerRequest *req) { send_error(req, 404, "Not found."); });
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
      status_json(doc);
      events.send(to_string(doc).c_str(), "status");
    }

    for (int batch = 0; batch < 8; batch++) {
      History::Read rd = printer_history(cursor, recs, PUSH_MAX_RECORDS);
      if (rd.count == 0 && rd.lost == 0) break;
      JsonDocument doc;
      history_json(doc, rd, recs);
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
  boot_id = esp_random();
  xTaskCreatePinnedToCore(web_task, "web", 8192, NULL, 1, NULL, WEB_CORE);
}
