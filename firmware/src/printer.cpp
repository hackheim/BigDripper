#include "printer.h"
#include <atomic>
#include <utility>
#include "esp_timer.h"
#include "coils.h"
#include "console_preview.h"
#include "encoder.h"
#include "mode.h"
#include "params.h"

// Printed on a loop, in Drip and never inverted, while the queue is empty.
static const char DEFAULT_TEXT[] = " BIG DRIPPER ";
static const Font DEFAULT_FONT = Font::Drip;

// Bytes allowed per glyph of the length limit before a text is rejected
// outright. Above the longest emoji shortcode (":big_smile:", 11), so a
// text the glyph count accepts is never cut off by this.
static const size_t MAX_BYTES_PER_GLYPH = 16;

// A call into the print task. The caller fills in the request, waits on
// `done`, and reads the answer from the same struct.
struct Request {
  enum Kind { Enqueue, SetMode, SetParams, GetStatus } kind;
  drip::Message message;  // Enqueue; SetParams: new default text if has_default
  bool has_default = false;
  drip::Mode mode;        // SetMode
  drip::Params params;    // SetParams
  bool ok = false;        // Enqueue: queued
  drip::Status status;    // GetStatus
  SemaphoreHandle_t done;
};

static QueueHandle_t requests = nullptr;  // of Request*

static History history;
static std::atomic<uint32_t> status_version{0};

static void bump_status() {
  status_version.fetch_add(1, std::memory_order_relaxed);
}
static TaskHandle_t print_task_handle = nullptr;
static Settings initial_settings;

// Letter spacing the default text was last rendered with. Only touched by
// printer_begin() and printer_set_settings(), which only the web server
// calls, from its one task.
static uint32_t default_spacing = FONT_SPACING;

static void call(Request &r) {
  r.done = xSemaphoreCreateBinary();
  Request *p = &r;
  xQueueSend(requests, &p, portMAX_DELAY);
  xTaskNotifyGive(print_task_handle);
  xSemaphoreTake(r.done, portMAX_DELAY);
  vSemaphoreDelete(r.done);
}

static drip::Message make_message(const String &text, Font font, bool invert, uint32_t spacing) {
  drip::Message m;
  m.info.text = text.c_str();
  m.info.font = (uint8_t)font;
  m.info.invert = invert;
  m.columns = font_render(text, font, invert, (int)spacing);
  return m;
}

// --- The print task ----------------------------------------------------------

static void handle(drip::Engine &engine, Request &r) {
  switch (r.kind) {
    case Request::Enqueue:
      r.ok = engine.enqueue(std::move(r.message));
      bump_status();
      break;
    case Request::SetMode:
      engine.set_mode(r.mode);
      bump_status();
      break;
    case Request::SetParams:
      engine.set_params(r.params);
      if (r.has_default) engine.set_default_text(std::move(r.message));
      bump_status();
      break;
    case Request::GetStatus:
      r.status = engine.status();
      break;
  }
  xSemaphoreGive(r.done);
}

static void record(Record::Kind kind, drip::Micros at, int32_t value) {
  Record rec = {};
  rec.kind = kind;
  rec.at_us = at;
  rec.value = value;
  history.push(rec);
}

// History, console preview and logging. Logging is compiled out unless
// CORE_DEBUG_LEVEL is raised, and capped so a fast spin can't back up the
// UART into this task.
static void report(const drip::Engine &engine, const drip::StepResult &r, drip::Micros now) {
  static const drip::Micros LOG_INTERVAL_US = 50000;
  static drip::Micros last_log = 0;
  bool overrun = false;
  int32_t column = -1;

  for (int i = 0; i < r.event_count; i++) {
    const drip::Event &e = r.events[i];
    switch (e.kind) {
      case drip::Event::ModeChanged:
        record(Record::ModeChanged, now, e.value);
        bump_status();
        console_preview_mode((drip::Mode)e.value);
        log_i("mode: %s", mode_name((drip::Mode)e.value));
        break;
      case drip::Event::MessageStarted: {
        record(Record::MessageStarted, now, e.value);
        bump_status();
        drip::MessageInfo info = engine.status().current;
        console_preview_message(info, e.value == 1);
        log_i("now printing \"%s\"%s", info.text.c_str(), e.value ? " (default)" : "");
        break;
      }
      case drip::Event::MessageFinished:
        record(Record::MessageFinished, now, 0);
        bump_status();
        log_i("message done");
        break;
      case drip::Event::Overrun:
        overrun = true;
        break;
      case drip::Event::ColumnFired:
        column = e.value;
        console_preview_column(r.coils.bits);
        break;
      case drip::Event::ColumnsSkipped:
        record(Record::Skipped, now, e.value);
        console_preview_skipped(e.value);
        break;
    }
  }

  if (r.has_coils) {
    Record rec = {};
    rec.kind = Record::Coils;
    rec.at_us = now;
    rec.value = column;
    rec.bits = r.coils.bits;
    rec.overrun = overrun;
    if (r.coils.bits == 0) {
      rec.open_us = 0;
    } else if (r.coils.close_at == drip::NEVER) {
      rec.open_us = Record::HELD;
    } else {
      rec.open_us = (uint32_t)(r.coils.close_at - now);
    }
    const drip::SpeedEstimator &speed = engine.speed();
    rec.counts_per_s = speed.valid() ? 1e6f / speed.us_per_count() : 0;
    if (column >= 0) {
      int32_t base = engine.params().counts_per_column;
      rec.stretch_pct = (uint16_t)(engine.column_width() * 100 / (base > 0 ? base : 1));
    }
    history.push(rec);

    if (column >= 0 && now - last_log >= LOG_INTERVAL_US) {
      last_log = now;
      log_i("col=%ld bits=0x%04x open_us=%lu", (long)column, rec.bits, (unsigned long)rec.open_us);
    }
  }
}

static void print_task(void *) {
  // Both from this task, so their interrupts are allocated on PRINT_CORE.
  coils_begin();
  encoder_begin(xTaskGetCurrentTaskHandle());

  drip::Engine *engine = new drip::Engine(
      make_message(DEFAULT_TEXT, DEFAULT_FONT, false, initial_settings.letter_spacing),
      params_to_engine(initial_settings));
  log_i("print task on core %d", xPortGetCoreID());

  for (;;) {
    Request *req;
    while (xQueueReceive(requests, &req, 0) == pdTRUE) handle(*engine, *req);

    drip::Micros now = esp_timer_get_time();
    drip::StepResult r = engine->step(now, encoder_sample(now));
    if (r.has_coils) coils_apply(r.coils);
    report(*engine, r, now);

    // Already outside the new window: no interrupt will come, step again.
    if (!encoder_set_window(r.wake.lo, r.wake.hi)) continue;

    TickType_t wait = portMAX_DELAY;
    if (r.wake.deadline != drip::NEVER) {
      drip::Micros in_us = r.wake.deadline - esp_timer_get_time();
      if (in_us <= 0) continue;
      // Rounded up, so it never wakes early; tick-level precision is plenty
      // for what has deadlines (Trace pulses, the pause between messages).
      wait = pdMS_TO_TICKS((in_us + 999) / 1000) + 1;
    }
    // Woken by the encoder leaving the window, a request, or the deadline.
    ulTaskNotifyTake(pdTRUE, wait);
  }
}

// --- The API -----------------------------------------------------------------

void printer_begin(const Settings &settings) {
  initial_settings = settings;
  default_spacing = settings.letter_spacing;
  requests = xQueueCreate(8, sizeof(Request *));
  // Above everything else on its core. It blocks whenever it has nothing to
  // do, which is all the time at standstill.
  xTaskCreatePinnedToCore(print_task, "print", 6144, nullptr, 10, &print_task_handle, PRINT_CORE);
}

EnqueueResult printer_enqueue(const String &text, Font font, bool invert) {
  // Rendered and checked here, on the caller's task, so the print task only
  // has to queue it. The length limit is in printed glyphs, so an emoji or
  // Æ counts as one; the byte cap is only a safety net against a huge POST.
  Settings s = params_get();
  size_t max_len = s.max_text_len;
  if (text.length() > MAX_BYTES_PER_GLYPH * max_len) return EnqueueResult::TooLong;
  size_t glyphs = font_glyph_count(text, font);
  if (glyphs == 0) return EnqueueResult::Empty;
  if (glyphs > max_len) return EnqueueResult::TooLong;

  Request r;
  r.kind = Request::Enqueue;
  r.message = make_message(text, font, invert, s.letter_spacing);
  call(r);
  return r.ok ? EnqueueResult::Ok : EnqueueResult::QueueFull;
}

const char *enqueue_result_name(EnqueueResult r) {
  switch (r) {
    case EnqueueResult::Ok: return "queued";
    case EnqueueResult::Empty: return "empty";
    case EnqueueResult::TooLong: return "too long";
    case EnqueueResult::QueueFull: return "queue full";
  }
  return "?";
}

void printer_set_mode(drip::Mode mode) {
  Request r;
  r.kind = Request::SetMode;
  r.mode = mode;
  call(r);
}

void printer_set_settings(const Settings &settings) {
  Request r;
  r.kind = Request::SetParams;
  r.params = params_to_engine(settings);
  if (settings.letter_spacing != default_spacing) {
    r.message = make_message(DEFAULT_TEXT, DEFAULT_FONT, false, settings.letter_spacing);
    r.has_default = true;
    default_spacing = settings.letter_spacing;
  }
  call(r);
}

drip::Status printer_status() {
  Request r;
  r.kind = Request::GetStatus;
  call(r);
  return r.status;
}

History::Read printer_history(uint32_t since, Record *out, size_t max) {
  return history.read(since, out, max);
}

uint32_t printer_history_head() {
  return history.head();
}

uint32_t printer_status_version() {
  return status_version.load(std::memory_order_relaxed);
}
