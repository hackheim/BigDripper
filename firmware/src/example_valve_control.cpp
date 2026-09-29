#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include "wifi_text_input.h"
#include "params.h"
#include "font.h"
#include "text_queue.h"
#include "priming.h"
#include "trace.h"

// COIL1..COIL16 -> ESP32-S3 GPIO, in order. The state vector is MSB-first:
// bit 15 = COIL1 (coils[0]) ... bit 0 = COIL16 (coils[15]).
const int NUM_COILS = 16;
const int NUM_NEOPIXEL_STRIPS = 4;
const uint8_t coils[NUM_COILS] = {9, 11, 10, 12, 14, 13, 1, 6, 7, 8, 38, 39, 40, 41, 42, 2};
const uint8_t neopixels[NUM_NEOPIXEL_STRIPS] = {15, 16, 17, 18};

// 8-pixel ring on the first neopixel GPIO. The other three pins are reserved
// for future strips and aren't driven yet.
const int NEOPIXEL_RING_PIXELS = 8;
Adafruit_NeoPixel neopixel_ring(NEOPIXEL_RING_PIXELS, neopixels[0], NEO_GRB + NEO_KHZ800);

// Encoder
const int NUM_ENCODER_CHANNELS = 2;
const uint8_t encoder[NUM_ENCODER_CHANNELS] = {4,5};

// --- Interrupt-driven quadrature decoder -----------------------------------
//
// State is (A << 1) | B. Indexing this table with (previous << 2) | current
// gives the step: +1 clockwise, -1 counter-clockwise, and 0 for the six
// impossible transitions (both channels changing at once), which is what
// contact bounce and missed steps look like. Rejecting those instead of
// guessing is what keeps the count from drifting on a cheap encoder.
//
// DRAM_ATTR keeps the table out of flash. An IRAM ISR must not touch flash,
// because the cache is disabled during flash writes and the fetch would fault.
static const DRAM_ATTR int8_t QUAD_TABLE[16] = {
   0, -1, +1,  0,
  +1,  0,  0, -1,
  -1,  0,  0, +1,
   0, +1, -1,  0
};

// Same reason: plain globals live in DRAM, so cache the pins here rather than
// reading the const encoder[] array (.rodata, i.e. flash) from the ISR.
static uint8_t enc_pin_a = 0;
static uint8_t enc_pin_b = 0;

static volatile int32_t encoder_raw = 0;   // quarter-steps, not detents
static volatile uint8_t encoder_prev = 0;
static portMUX_TYPE encoder_mux = portMUX_INITIALIZER_UNLOCKED;

// Set by the encoder task before it attaches the interrupt, so the ISR can wake
// it. A plain global lives in .bss (DRAM), which an IRAM ISR may read.
static volatile TaskHandle_t encoder_task_handle = NULL;

// Most detented encoders run through a full 4-state cycle per click, so shift
// by 2 to report detents. Set to 0 to count every edge instead.
static const int ENCODER_DETENT_SHIFT = 2;

static void ARDUINO_ISR_ATTR encoder_isr() {
  uint8_t state = (digitalRead(enc_pin_a) << 1) | digitalRead(enc_pin_b);

  portENTER_CRITICAL_ISR(&encoder_mux);
  encoder_raw += QUAD_TABLE[(encoder_prev << 2) | state];
  encoder_prev = state;
  portEXIT_CRITICAL_ISR(&encoder_mux);

  // Wake the encoder task instead of having it poll. The ISR stays short: it
  // only counts, the task does anything expensive.
  if (encoder_task_handle != NULL) {
    BaseType_t higher_woken = pdFALSE;
    vTaskNotifyGiveFromISR(encoder_task_handle, &higher_woken);
    portYIELD_FROM_ISR(higher_woken);
  }
}

// Safe to call from loop(): int32_t is not atomic against a concurrent ISR on
// the other core, so take the same lock the ISR uses.
int32_t encoder_position() {
  portENTER_CRITICAL(&encoder_mux);
  int32_t raw = encoder_raw;
  portEXIT_CRITICAL(&encoder_mux);

  // Arithmetic shift floors toward negative infinity. Dividing by 4 would
  // truncate toward zero and make the detent straddling zero twice as wide.
  return raw >> ENCODER_DETENT_SHIFT;
}

void encoder_zero() {
  portENTER_CRITICAL(&encoder_mux);
  encoder_raw = 0;
  portEXIT_CRITICAL(&encoder_mux);
}

void encoder_begin() {
  enc_pin_a = encoder[0];
  enc_pin_b = encoder[1];

  // Seed the previous state, otherwise the first edge is decoded against a
  // state the encoder was never in and can produce one bogus step.
  encoder_prev = (digitalRead(enc_pin_a) << 1) | digitalRead(enc_pin_b);

  // Both channels, both edges — a quadrature cycle has four transitions and
  // dropping any of them halves the resolution and breaks the state table.
  attachInterrupt(digitalPinToInterrupt(enc_pin_a), encoder_isr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(enc_pin_b), encoder_isr, CHANGE);
}
// ---------------------------------------------------------------------------


// --- Core assignment -------------------------------------------------------
//
// Arduino's loopTask is pinned to ARDUINO_RUNNING_CORE, which this board sets to
// 1 (see the board manifest's extra_flags). So core 1 is where setup()/loop()
// already run, and core 0 is otherwise idle in this sketch.
#define ENCODER_CORE 0
#define VALVE_CORE   1

// Defined below. This is a .cpp, so unlike an .ino there is no automatic
// prototype generation.
void test_GPIO_outputs();
void print_engine_run();

static void encoder_task(void *arg) {
  // Publish the handle before attaching, so the ISR never fires with it unset.
  encoder_task_handle = xTaskGetCurrentTaskHandle();

  // attachInterrupt MUST be called from this task, not from setup(). The first
  // attachInterrupt anywhere in the sketch calls gpio_install_isr_service(),
  // which allocates the shared GPIO interrupt on whatever core is executing at
  // that moment (see __attachInterruptFunctionalArg in esp32-hal-gpio.c).
  // Calling it from setup() would bind the ISR to core 1 no matter where this
  // task runs. Caveat: that service is shared, so every GPIO interrupt added
  // later also lands on this core.
  encoder_begin();

  log_i("encoder task + ISR on core %d", xPortGetCoreID());

  // Logging every single detent works fine for a slow test turn, but a fast
  // spin can notify this task faster than a UART write at 115200 baud can
  // drain (log_i's underlying uart_tx busy-waits for FIFO space), which was
  // starving IDLE0 on this core long enough to trip the task watchdog. Cap
  // how often we actually log; position tracking itself stays uncapped.
  const uint32_t LOG_INTERVAL_MS = 50;
  uint32_t last_log_ms = 0;

  int32_t last = encoder_position();
  for (;;) {
    // Block until the ISR reports an edge. No polling, so IDLE0 keeps running
    // and the task watchdog stays fed.
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    int32_t pos = encoder_position();
    if (pos != last) {
      uint32_t now = millis();
      if (now - last_log_ms >= LOG_INTERVAL_MS) {
        log_i("encoder %+ld (%s) [core %d]",
              (long)pos, pos > last ? "CW" : "CCW", xPortGetCoreID());
        last_log_ms = now;
      }
      last = pos;
    }
  }
}

static void valve_task(void *arg) {
  log_i("valve task on core %d", xPortGetCoreID());

  // Blocking sequencer; delay() inside a task is vTaskDelay, so it yields the
  // core rather than spinning.
  print_engine_run();  // never returns

  vTaskDelete(NULL);
}

// Slow red breathe: a sine wave keeps the fade smooth at both ends, unlike a
// linear ramp which looks like it "hangs" near full brightness/off.
static void neopixel_task(void *arg) {
  log_i("neopixel task on core %d", xPortGetCoreID());

  neopixel_ring.begin();
  neopixel_ring.show();  // all off

  const uint32_t PULSE_PERIOD_MS = 4000;
  const uint32_t UPDATE_INTERVAL_MS = 20;  // ~50 Hz, smooth without flooding the bus

  for (;;) {
    uint32_t phase_ms = millis() % PULSE_PERIOD_MS;
    float phase = (2.0f * PI * phase_ms) / PULSE_PERIOD_MS;
    float brightness = (sinf(phase - PI / 2.0f) + 1.0f) / 2.0f;  // 0..1, starts at 0

    uint8_t red = (uint8_t)(brightness * 255.0f);
    uint32_t color = neopixel_ring.Color(red, 0, 0);
    for (int i = 0; i < NEOPIXEL_RING_PIXELS; i++) {
      neopixel_ring.setPixelColor(i, color);
    }
    neopixel_ring.show();

    vTaskDelay(pdMS_TO_TICKS(UPDATE_INTERVAL_MS));
  }
}
// ---------------------------------------------------------------------------


void setup() {
  Serial.begin(115200);

  // Native USB CDC only exists once the host has enumerated it, which takes a
  // second or two after boot. Without this wait, everything printed here is
  // written into the void. Bounded so the sketch still runs untethered.
  while (!Serial && millis() < 3000) {
    delay(10);
  }

  log_i("BigDripper valve test, %d coils", NUM_COILS);

  log_i("GPIO outputs:");
  for (int i = 0; i < NUM_COILS; i++) {
      pinMode(coils[i], OUTPUT);
      digitalWrite(coils[i], LOW);
      log_i("COIL%d -> GPIO%u", i + 1, coils[i]);
  }

  log_i("GPIO inputs:");
  for (int i = 0; i < NUM_ENCODER_CHANNELS; i++) {
      pinMode(encoder[i], INPUT_PULLUP);
      log_i("ENCODER %s -> GPIO%u", i == 0 ? "A": "B", encoder[i]);
  }

  log_i("setup() on core %d", xPortGetCoreID());

  // params_begin()/font_init() must complete before the tasks below start,
  // since valve_task's print engine reads params and glyphs from the moment
  // it runs, and valve_task's priority is high enough to preempt setup()
  // (still running as loopTask) as soon as it's created.
  params_begin();
  font_init();

  // 4096-byte stacks: both tasks call log_i, and the vsnprintf underneath it is
  // the stack-hungry part. Priorities are above loopTask's 1 so neither is
  // starved by it; being on separate cores makes that mostly academic.
  xTaskCreatePinnedToCore(encoder_task,   "encoder",   4096, NULL, 3, NULL, ENCODER_CORE);
  xTaskCreatePinnedToCore(valve_task,     "valve",     4096, NULL, 2, NULL, VALVE_CORE);
  xTaskCreatePinnedToCore(neopixel_task,  "neopixel",  2048, NULL, 1, NULL, ENCODER_CORE);

  wifi_text_input_begin();
}

void set_coil_state(uint16_t bitVector) {
  uint16_t bit = 0x8000;  // bit 15 -> coils[0] (COIL1)

  for (int i = 0; i < NUM_COILS; i++) {
    digitalWrite(coils[i], (bitVector & bit) ? HIGH : LOW);
    bit >>= 1;
  }
}

// Naive test function for flipping all coils on / off
void test_GPIO_outputs() {
  while (true) {
    static unsigned tick = 0;

    // Serial.printf: always compiled in, plain text, you control the format.
    set_coil_state(0xFFFF);
    log_d("[%u] coils ON  (%lu ms)", tick, millis());
    delay(1000);

    // log_*: adds level, timestamp, file:line and function; compiled out entirely
    // when CORE_DEBUG_LEVEL is below the macro's level.
    set_coil_state(0x0000);
    log_d("[%u] coils OFF", tick++);
    delay(1000);
  }
}

// naive test function for reading encoder output
void test_encoder() {
  int a = digitalRead(encoder[0]);
  int b = digitalRead(encoder[1]);
  log_i("A:%d - B:%d", a, b);

}

// Scans across the text currently at the front of the queue, driven by the
// encoder: params_get_clicks_per_column() detents move the scan one glyph
// column. Re-zeroes the encoder every time a new text becomes current, so
// column 0 always lines up with wherever the wheel happens to be when that
// text starts printing.
void print_engine_run() {
  String current = "";
  int32_t last_column = -1;

  // True while printing TEXT_QUEUE_DEFAULT_TEXT because nothing is queued.
  // Tracked as a flag rather than by comparing strings, so a user who
  // queues the same text still gets it printed once and advanced past.
  bool looping_default = false;

  // Coils fire a brief burst when the scan lands on a column, rather than
  // staying on until the scan reaches the next one. coil_off_at_ms is when
  // the current burst should end.
  bool coil_on = false;
  uint32_t coil_off_at_ms = 0;

  // See encoder_task()'s LOG_INTERVAL_MS comment: a fast spin can change
  // columns faster than a UART write drains, so cap how often we log a
  // column change even though we still render every one.
  const uint32_t LOG_INTERVAL_MS = 50;
  uint32_t last_log_ms = 0;

  bool was_priming = false;
  bool was_tracing = false;

  // Set when leaving trace mode: the next column the scan reports is taken
  // as the new baseline instead of being fired, so stopping trace leaves
  // every valve closed rather than jumping straight into a burst for
  // wherever the wheel ended up.
  bool resync_column = false;

  while (true) {
    if (priming_is_active()) {
      // Manual override: hold every valve open regardless of scan state
      // until the web UI's "priming finished" clears this.
      set_coil_state(0xFFFF);
      coil_on = false;
      was_priming = true;
      delay(5);
      continue;
    }
    if (was_priming) {
      // Just came out of priming: close everything before resuming the
      // scan, rather than falling through to whatever bits the current
      // column happens to render.
      set_coil_state(0x0000);
      was_priming = false;
    }

    if (trace_is_active()) {
      // Debug pulse: all coils on for exactly the configured burst, then
      // all off for the configured trace gap. Blocking is fine here since nothing else
      // runs while tracing. Log after the burst, not during, so a slow
      // UART write can't stretch the pulse being measured.
      was_tracing = true;
      uint32_t burst_ms = params_get_column_burst_ms();
      uint32_t start_us = micros();
      set_coil_state(0xFFFF);
      // Poll rather than delay(burst_ms) so "trace off" closes the valves
      // immediately even in the middle of a long burst.
      uint32_t burst_start = millis();
      while (millis() - burst_start < burst_ms && trace_is_active() && !priming_is_active()) {
        delay(1);
      }
      set_coil_state(0x0000);
      uint32_t elapsed_us = micros() - start_us;
      coil_on = false;
      log_i("trace: all coils ON for %lu ms (measured %lu us)",
            (unsigned long)burst_ms, (unsigned long)elapsed_us);

      // Off period, polled in small steps so "trace off" and priming take
      // effect promptly instead of after the full gap.
      uint32_t gap_ms = params_get_trace_gap_ms();
      uint32_t off_start = millis();
      while (millis() - off_start < gap_ms && trace_is_active() && !priming_is_active()) {
        delay(5);
      }
      continue;
    }
    if (was_tracing) {
      set_coil_state(0x0000);
      coil_on = false;
      resync_column = true;
      was_tracing = false;
      log_i("trace: stopped, all coils closed");
    }

    String text = text_queue_current();
    bool use_default = text.length() == 0 && text_queue_is_idle();
    if (use_default) {
      text = TEXT_QUEUE_DEFAULT_TEXT;
    }
    if (text != current || use_default != looping_default) {
      current = text;
      looping_default = use_default;
      last_column = -1;
      coil_on = false;
      // A new text starting is a deliberate print, so fire its first column.
      resync_column = false;
      encoder_zero();
      log_i("print engine: now printing \"%s\"", current.c_str());
    }

    if (current.length() == 0) {
      set_coil_state(0x0000);
      coil_on = false;
      delay(5);
      continue;
    }

    int32_t clicks_per_column = (int32_t)params_get_clicks_per_column();
    if (clicks_per_column <= 0) clicks_per_column = 1;

    // Scanning backwards past the start of the text clamps at column 0
    // rather than going negative.
    int32_t pos = encoder_position();
    int32_t column = (pos > 0) ? (pos / clicks_per_column) : 0;

    int32_t total_columns = (int32_t)current.length() * FONT_CHAR_WIDTH;
    if (looping_default) {
      // Wrap instead of advancing, so the default text repeats seamlessly
      // for as long as the wheel keeps turning.
      column %= total_columns;
    } else if (column >= total_columns) {
      log_i("print engine: done, advancing queue");
      set_coil_state(0x0000);
      coil_on = false;
      text_queue_advance();
      delay(5);
      continue;
    }

    if (resync_column) {
      last_column = column;
      resync_column = false;
    }

    if (column != last_column) {
      int char_index = column / FONT_CHAR_WIDTH;
      int col_in_char = column % FONT_CHAR_WIDTH;

      // col_in_char == FONT_GLYPH_WIDTH is the blank inter-character
      // spacing column, so bits stays 0 (all coils off, no burst) for it.
      uint16_t bits = 0;
      if (col_in_char < FONT_GLYPH_WIDTH) {
        uint16_t glyph[FONT_GLYPH_WIDTH];
        font_get_glyph(current[char_index], glyph);
        bits = glyph[col_in_char];
      }

      set_coil_state(bits);
      coil_on = true;
      coil_off_at_ms = millis() + params_get_column_burst_ms();
      last_column = column;

      uint32_t now = millis();
      if (now - last_log_ms >= LOG_INTERVAL_MS) {
        log_i("print engine: pos=%ld col=%ld/%ld char='%c' bits=0x%04x",
              (long)pos, (long)column, (long)total_columns, current[char_index], bits);
        last_log_ms = now;
      }
    } else if (coil_on && (int32_t)(millis() - coil_off_at_ms) >= 0) {
      // Wraparound-safe "has the burst timer elapsed" check: millis()
      // overflows every ~49 days, and the signed subtraction stays correct
      // across that wrap.
      set_coil_state(0x0000);
      coil_on = false;
    }

    delay(5);
  }
}

void demoLEDs() {
  for (int i=0; i< 16; i++) {
    digitalWrite(coils[i], HIGH);
    vTaskDelay(pdMS_TO_TICKS(100));
    digitalWrite(coils[i], LOW);
    vTaskDelay(pdMS_TO_TICKS(50));
  }

  for (int j=0; j< 3; j++) {
    for (int i=0; i< 16; i++) {
        digitalWrite(coils[i], HIGH);
      }
      vTaskDelay(pdMS_TO_TICKS(100));

      for (int i=0; i< 16; i++) {
        digitalWrite(coils[i], LOW);
      }
      vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void loop() {

  // Both jobs now live in their own pinned tasks, so loopTask has nothing to do.
  // It still has to yield: returning immediately would spin core 1 at full tilt
  // and starve the idle task. Deleting loopTask outright is the alternative, but
  // keeping it costs nothing and leaves somewhere to put ad-hoc test code.
  vTaskDelay(pdMS_TO_TICKS(1000));
}
