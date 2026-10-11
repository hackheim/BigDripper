#include "ble.h"
#include <ArduinoJson.h>
#include <NimBLEDevice.h>
#include <string>
#include "esp_timer.h"
#include "api.h"
#include "printer.h"

static const char *DEVICE_NAME = "BigDripper";

// Must match web/transport.js.
static const char *SERVICE_UUID = "c44f5ed8-6e56-4c8b-ae22-7164774163b9";
static const char *REQUEST_UUID = "2edbd9b7-7914-4c4d-8ae1-97fb8b1dc19b";  // write
static const char *EVENTS_UUID = "b6a173f9-9607-4151-8654-f814ffd5bdf6";   // notify

// Every write and notification starts with one byte: what it is, with MORE
// set if the message continues in the next one. A message is split to fit
// the connection's MTU, and one connection's messages never interleave.
enum : uint8_t {
  MSG_REQUEST = 0,  // client -> board: JSON {"id", "op", "args", "auth"}
  MSG_REPLY = 1,    // JSON {"id", "code", "body"}
  MSG_STATUS = 2,   // JSON, as the status op
  MSG_HISTORY = 3,  // binary, see history_binary()
  MSG_CLOCK = 4,    // binary: int64 board time, us
  MORE = 0x80,
};

// Same pacing as the web event stream (web.cpp).
#define BLE_CORE 0
static const uint32_t PUSH_INTERVAL_MS = 100;
static const size_t PUSH_MAX_RECORDS = 64;
static const uint32_t CLOCK_INTERVAL_MS = 2000;

// A request longer than this is dropped.
static const size_t MAX_REQUEST = 4096;

static NimBLEServer *server = nullptr;
static NimBLECharacteristic *events_chr = nullptr;

// --- From the Bluetooth stack's task to ours ------------------------------------
//
// The callbacks below run in the NimBLE host task. They only pass what
// happened on to the ble task, which owns all per-connection state, so
// nothing here needs a lock and a slow API call never holds up the stack.

struct Incoming {
  enum Kind : uint8_t { Connected, Disconnected, Subscribed, Unsubscribed, Write } kind;
  uint16_t conn;
  std::string *data;  // Write: what was written, freed by the ble task
};
static QueueHandle_t incoming;

static void post(Incoming::Kind kind, uint16_t conn, std::string *data = nullptr) {
  Incoming in = {kind, conn, data};
  if (xQueueSend(incoming, &in, 0) != pdTRUE) delete data;  // the client will time out
}

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *s, NimBLEConnInfo &info) override {
    // 15-30 ms: quick enough for the live view, slow enough for WiFi to
    // share the radio.
    s->updateConnParams(info.getConnHandle(), 12, 24, 0, 400);
    post(Incoming::Connected, info.getConnHandle());
    // Room for another phone.
    if (s->getConnectedCount() < CONFIG_BT_NIMBLE_MAX_CONNECTIONS) NimBLEDevice::startAdvertising();
  }
  void onDisconnect(NimBLEServer *, NimBLEConnInfo &info, int) override {
    post(Incoming::Disconnected, info.getConnHandle());
  }
};

class RequestCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &info) override {
    NimBLEAttValue v = c->getValue();
    post(Incoming::Write, info.getConnHandle(), new std::string((const char *)v.data(), v.size()));
  }
};

class EventsCallbacks : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic *, NimBLEConnInfo &info, uint16_t sub) override {
    post(sub ? Incoming::Subscribed : Incoming::Unsubscribed, info.getConnHandle());
  }
};

// --- Connections ---------------------------------------------------------------

struct Conn {
  uint16_t handle = BLE_HS_CONN_HANDLE_NONE;
  bool subscribed = false;
  uint32_t cursor = 0;        // next history record to send
  uint32_t sent_version = 0;  // printer_status_version() last sent
  uint32_t last_clock = 0;
  std::string request;        // a request arriving in pieces
};
static Conn conns[CONFIG_BT_NIMBLE_MAX_CONNECTIONS];

static Conn *find(uint16_t handle) {
  for (Conn &c : conns) {
    if (c.handle == handle) return &c;
  }
  return nullptr;
}

// Sends one notification, waiting for the stack to have room for it: a
// big reply or a burst of history is more than it buffers at once. False
// if it never did (the phone went away).
static bool notify(uint16_t handle, const uint8_t *data, size_t len) {
  for (int tries = 0; tries < 100; tries++) {
    if (events_chr->notify(data, len, handle)) return true;
    if (ble_gap_conn_find(handle, nullptr) != 0) return false;  // gone
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  return false;
}

// Sends a whole message, split to fit the MTU.
static bool send(Conn &c, uint8_t type, const uint8_t *data, size_t len) {
  uint16_t mtu = server->getPeerMTU(c.handle);
  size_t chunk = (mtu > 23 ? mtu : 23) - 3 - 1;  // ATT header, our type byte
  static uint8_t buf[600];
  if (chunk > sizeof(buf) - 1) chunk = sizeof(buf) - 1;
  size_t off = 0;
  do {
    size_t n = len - off < chunk ? len - off : chunk;
    bool more = off + n < len;
    buf[0] = type | (more ? MORE : 0);
    memcpy(buf + 1, data + off, n);
    if (!notify(c.handle, buf, n + 1)) return false;
    off += n;
  } while (off < len);
  return true;
}

static bool send_json(Conn &c, uint8_t type, const JsonDocument &doc) {
  std::string out;
  serializeJson(doc, out);
  return send(c, type, (const uint8_t *)out.data(), out.size());
}

static void send_status(Conn &c) {
  c.sent_version = printer_status_version();
  JsonDocument doc;
  api_status_json(doc);
  send_json(c, MSG_STATUS, doc);
}

// --- History, packed -----------------------------------------------------------
//
// The history stream is the bulk of what goes out, so it's packed: little
// endian, a 21-byte header then 26 bytes per record.
//
//   header  u32 first, u32 next, u32 lost, u8 reset, i64 now
//   record  u8 kind, u8 overrun, u16 bits, i32 value, i64 at,
//           i32 open (-1 = held), u32 speed (counts/s), u16 stretch
//
// The same fields as the JSON history (api_history_json()).

static const size_t HISTORY_HEADER = 21, HISTORY_RECORD = 26;

template <typename T>
static uint8_t *put(uint8_t *p, T v) {
  memcpy(p, &v, sizeof(v));  // the ESP32 is little endian
  return p + sizeof(v);
}

static size_t history_binary(uint8_t *out, const History::Read &rd, const Record *recs) {
  uint8_t *p = out;
  p = put<uint32_t>(p, rd.first);
  p = put<uint32_t>(p, rd.next);
  p = put<uint32_t>(p, rd.lost);
  p = put<uint8_t>(p, rd.reset ? 1 : 0);
  p = put<int64_t>(p, esp_timer_get_time());
  for (size_t i = 0; i < rd.count; i++) {
    const Record &r = recs[i];
    p = put<uint8_t>(p, r.kind);
    p = put<uint8_t>(p, r.overrun);
    p = put<uint16_t>(p, r.bits);
    p = put<int32_t>(p, r.value);
    p = put<int64_t>(p, r.at_us);
    p = put<int32_t>(p, r.open_us == Record::HELD ? -1 : (int32_t)r.open_us);
    p = put<uint32_t>(p, (uint32_t)lroundf(r.counts_per_s));
    p = put<uint16_t>(p, r.stretch_pct);
  }
  return p - out;
}

// --- The ble task --------------------------------------------------------------

// A complete request: carried out, and the reply sent back to whoever asked.
static void handle_request(Conn &c, const std::string &text) {
  JsonDocument req, body, reply;
  int code;
  if (deserializeJson(req, text) != DeserializationError::Ok) {
    body["error"] = "Bad request.";
    code = 400;
  } else {
    const char *auth = req["auth"] | "";
    code = api_call(req["op"] | "", req["args"], strcmp(auth, API_SETTINGS_PASSWORD) == 0, body);
  }
  reply["id"] = req["id"];
  reply["code"] = code;
  reply["body"] = body;
  send_json(c, MSG_REPLY, reply);
}

static void handle(const Incoming &in) {
  Conn *c = find(in.conn);
  switch (in.kind) {
    case Incoming::Connected:
      c = find(BLE_HS_CONN_HANDLE_NONE);
      if (c != nullptr) {
        *c = Conn();
        c->handle = in.conn;
      }
      log_i("ble: connected (%u)", in.conn);
      break;
    case Incoming::Disconnected:
      if (c != nullptr) *c = Conn();
      log_i("ble: disconnected (%u)", in.conn);
      break;
    case Incoming::Subscribed:
      // Like the web event stream: the status now, history from here on
      // (the client fetches what came before itself).
      if (c == nullptr) break;
      c->subscribed = true;
      c->cursor = printer_history_head();
      send_status(*c);
      break;
    case Incoming::Unsubscribed:
      if (c != nullptr) c->subscribed = false;
      break;
    case Incoming::Write: {
      const std::string &d = *in.data;
      if (c == nullptr || d.empty()) break;
      c->request.append(d, 1, std::string::npos);
      if (c->request.size() > MAX_REQUEST) {
        c->request.clear();
        break;
      }
      if ((uint8_t)d[0] & MORE) break;
      std::string text;
      text.swap(c->request);
      handle_request(*c, text);
      break;
    }
  }
  delete in.data;
}

// The event stream, for each subscribed connection: the status when it
// changes, every new history record in order, and a clock tick.
static void push(Conn &c) {
  static Record recs[PUSH_MAX_RECORDS];
  static uint8_t packed[HISTORY_HEADER + PUSH_MAX_RECORDS * HISTORY_RECORD];

  if (printer_status_version() != c.sent_version) send_status(c);

  for (int batch = 0; batch < 8; batch++) {
    History::Read rd = printer_history(c.cursor, recs, PUSH_MAX_RECORDS);
    if (rd.count == 0 && rd.lost == 0) break;
    if (!send(c, MSG_HISTORY, packed, history_binary(packed, rd, recs))) return;
    c.cursor = rd.next;
  }

  if (millis() - c.last_clock >= CLOCK_INTERVAL_MS) {
    c.last_clock = millis();
    int64_t now = esp_timer_get_time();
    send(c, MSG_CLOCK, (const uint8_t *)&now, sizeof(now));
  }
}

static void ble_task(void *) {
  NimBLEDevice::init(DEVICE_NAME);
  NimBLEDevice::setMTU(517);  // the most a phone will take: fewer pieces per message

  server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());
  server->advertiseOnDisconnect(true);  // so a phone that dropped can come back
  NimBLEService *service = server->createService(SERVICE_UUID);
  NimBLECharacteristic *request = service->createCharacteristic(REQUEST_UUID, NIMBLE_PROPERTY::WRITE);
  request->setCallbacks(new RequestCallbacks());
  events_chr = service->createCharacteristic(EVENTS_UUID, NIMBLE_PROPERTY::NOTIFY);
  events_chr->setCallbacks(new EventsCallbacks());

  // The service UUID is what a page looks for; with the name it doesn't fit
  // one advertisement, so the name goes in the scan response.
  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  NimBLEAdvertisementData data, scan;
  data.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  data.addServiceUUID(SERVICE_UUID);
  scan.setName(DEVICE_NAME);
  adv->setAdvertisementData(data);
  adv->setScanResponseData(scan);
  adv->enableScanResponse(true);
  NimBLEDevice::startAdvertising();
  log_i("ble: advertising as \"%s\"", DEVICE_NAME);

  uint32_t last_push = 0;
  for (;;) {
    Incoming in;
    if (xQueueReceive(incoming, &in, pdMS_TO_TICKS(10)) == pdTRUE) handle(in);
    if (millis() - last_push < PUSH_INTERVAL_MS) continue;
    last_push = millis();
    for (Conn &c : conns) {
      if (c.handle != BLE_HS_CONN_HANDLE_NONE && c.subscribed) push(c);
    }
  }
}

void ble_begin() {
  incoming = xQueueCreate(16, sizeof(Incoming));
  xTaskCreatePinnedToCore(ble_task, "ble", 8192, NULL, 1, NULL, BLE_CORE);
}
