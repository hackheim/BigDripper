#include "api.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "encoder.h"
#include "font_data.h"
#include "mode.h"
#include "params.h"

// Separate from the WiFi password: this gates the settings, not the network.
const char *const API_SETTINGS_USER = "admin";
const char *const API_SETTINGS_PASSWORD = "letmeprint9";

static uint32_t boot_id = 0;

// One call at a time: the web server and Bluetooth each have their own
// task, and the history op shares one big buffer.
static SemaphoreHandle_t lock = nullptr;

void api_begin() {
  boot_id = esp_random();
  lock = xSemaphoreCreateMutex();
}

uint32_t api_boot_id() { return boot_id; }

// --- JSON ----------------------------------------------------------------------

static void message_json(JsonObject o, const drip::MessageInfo &m) {
  o["text"] = m.text.c_str();
  o["font"] = font_label((Font)m.font);
  o["invert"] = m.invert;
}

void api_status_json(JsonDocument &doc) {
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

// Records are arrays to keep the stream small; see web/API.md for the
// fields.
void api_history_json(JsonDocument &doc, const History::Read &rd, const Record *recs) {
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

// --- Operations ----------------------------------------------------------------

static int error(JsonDocument &reply, int code, const char *message) {
  reply.clear();
  reply["error"] = message;
  return code;
}

static int status(JsonDocument &reply) {
  api_status_json(reply);
  return 200;
}

// Reads whole-number field `name` into *out if it's there. False if it's
// there but isn't a whole number >= 0.
static bool field_uint(JsonObjectConst args, const char *name, uint32_t *out) {
  JsonVariantConst v = args[name];
  if (v.isNull()) return true;
  if (!v.is<uint32_t>()) return false;
  *out = v.as<uint32_t>();
  return true;
}

// Saves a new set of settings and hands them to the print engine, or
// leaves everything as it was and returns why not.
static const char *apply_settings(const Settings &s) {
  const char *err = params_check(s);
  if (err != nullptr) return err;
  params_set(s);
  printer_set_settings(s);
  return nullptr;
}

static int queue(JsonObjectConst args, JsonDocument &reply) {
  if (!args["text"].is<const char *>()) return error(reply, 400, "text must be a string.");
  String text = args["text"].as<const char *>();
  Font font = Font::Drip;
  if (!args["font"].isNull() && !font_from_id(args["font"].as<String>(), &font)) {
    return error(reply, 400, "font must be one of spleen, drip, drip_bold.");
  }
  bool invert = args["invert"] | false;

  EnqueueResult r = printer_enqueue(text, font, invert);
  log_i("\"%s\" in %s%s: %s", text.c_str(), font_label(font), invert ? ", inverted" : "",
        enqueue_result_name(r));
  switch (r) {
    case EnqueueResult::Ok:
      return status(reply);
    case EnqueueResult::Empty:
      return error(reply, 400, "Nothing to print.");
    case EnqueueResult::TooLong:
      return error(reply, 400, "Too long.");
    case EnqueueResult::QueueFull:
      return error(reply, 409, "The queue is full.");
  }
  return error(reply, 400, "Not queued.");
}

static int mode(JsonObjectConst args, JsonDocument &reply) {
  drip::Mode m;
  if (!mode_from_name(args["mode"] | "", &m)) {
    return error(reply, 400, "mode must be one of off, prime, trace, lines, text.");
  }
  printer_set_mode(m);
  return status(reply);
}

static int lines(JsonObjectConst args, JsonDocument &reply) {
  Settings s = params_get();
  if (args["spacing"].isNull() || !field_uint(args, "spacing", &s.test_line_spacing)) {
    return error(reply, 400, "spacing must be a whole number of columns.");
  }
  const char *err = apply_settings(s);
  if (err != nullptr) return error(reply, 400, err);
  return status(reply);
}

static int save_settings(JsonObjectConst args, JsonDocument &reply) {
  // Every field is read first (missing ones keep their saved value) and the
  // result checked as a whole, so a rejected request saves none of it.
  Settings s = params_get();
  JsonVariantConst scale = args["scale_mm_per_detent"];
  if (!scale.isNull()) {
    if (!scale.is<float>() || !(scale.as<float>() > 0)) {
      return error(reply, 400, "scale_mm_per_detent must be a number above 0.");
    }
    s.scale_mm_per_detent = scale.as<float>();
  }
  if (!field_uint(args, "max_text_len", &s.max_text_len) || !field_uint(args, "print_pause_ms", &s.print_pause_ms) ||
      !field_uint(args, "clicks_per_column", &s.clicks_per_column) ||
      !field_uint(args, "column_burst_ms", &s.column_burst_ms) ||
      !field_uint(args, "burst_duty_pct", &s.burst_duty_pct) ||
      !field_uint(args, "min_burst_ms", &s.min_burst_ms) || !field_uint(args, "trace_gap_ms", &s.trace_gap_ms) ||
      !field_uint(args, "test_line_spacing", &s.test_line_spacing) ||
      !field_uint(args, "letter_spacing", &s.letter_spacing) ||
      !field_uint(args, "min_column_ms", &s.min_column_ms) ||
      !field_uint(args, "max_stretch_pct", &s.max_stretch_pct)) {
    return error(reply, 400, "Settings other than the scale must be whole numbers, 0 or more.");
  }
  const char *err = apply_settings(s);
  if (err != nullptr) return error(reply, 400, err);
  log_i("settings updated");
  settings_json(reply);
  return 200;
}

static int history(JsonObjectConst args, JsonDocument &reply) {
  static Record recs[HISTORY_SIZE];  // under the lock
  uint32_t since = args["since"] | 0u;
  History::Read rd = printer_history(since, recs, HISTORY_SIZE);
  api_history_json(reply, rd, recs);
  return 200;
}

static int dispatch(const char *op, JsonObjectConst args, bool authorized, JsonDocument &reply) {
  if (strcmp(op, "status") == 0) return status(reply);
  if (strcmp(op, "emoji") == 0) {
    emoji_json(reply);
    return 200;
  }
  if (strcmp(op, "history") == 0) return history(args, reply);
  if (strcmp(op, "queue") == 0) return queue(args, reply);
  if (strcmp(op, "mode") == 0) return mode(args, reply);
  if (strcmp(op, "lines") == 0) return lines(args, reply);
  bool get = strcmp(op, "settings") == 0, set = strcmp(op, "save_settings") == 0;
  if (get || set) {
    if (!authorized) return error(reply, 401, "Wrong or missing settings password.");
    if (set) return save_settings(args, reply);
    settings_json(reply);
    return 200;
  }
  return error(reply, 404, "No such request.");
}

int api_call(const char *op, JsonVariantConst args, bool authorized, JsonDocument &reply) {
  xSemaphoreTake(lock, portMAX_DELAY);
  int code = dispatch(op != nullptr ? op : "", args.as<JsonObjectConst>(), authorized, reply);
  xSemaphoreGive(lock);
  return code;
}
