#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include "api.h"
#include "ble.h"
#include "console_preview.h"
#include "params.h"
#include "printer.h"
#include "web.h"

// What runs where. Arduino's loopTask (setup()/loop()) is pinned to core 1,
// which is also PRINT_CORE (printer.h): the print engine, the encoder and
// burst timer interrupts. Everything that can be slow or bursty goes on
// core 0: WiFi and the web server, Bluetooth, the neopixels, the console
// preview.
#define BACKGROUND_CORE 0

const int NUM_NEOPIXEL_STRIPS = 4;
const uint8_t neopixels[NUM_NEOPIXEL_STRIPS] = {15, 16, 17, 18};

// Strip 1 (first neopixel GPIO) pulses red; strip 2 (second GPIO) runs a
// yellow Cylon eye. The other two pins are reserved and aren't driven yet.
const int GLOW_STRIP_PIXELS = 48;
const int CYLON_STRIP_PIXELS = 28;
Adafruit_NeoPixel glow_strip(GLOW_STRIP_PIXELS, neopixels[0], NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel cylon_strip(CYLON_STRIP_PIXELS, neopixels[1], NEO_GRB + NEO_KHZ800);

// --- Neopixel power budget -------------------------------------------------
//
// All strips share a 1 A supply, so brightness is capped by design rather
// than measured at runtime. WS2812B figures, on the conservative side: ~20 mA
// per colour channel at full (255) and ~1 mA per pixel just for being
// powered, even when dark.
static constexpr float NEOPIXEL_BUDGET_MA = 1000.0f;
static constexpr float NEOPIXEL_MARGIN_MA = 100.0f;   // headroom for the estimate being off
static constexpr float MA_PER_CHANNEL_FULL = 20.0f;
static constexpr float MA_IDLE_PER_PIXEL = 1.0f;

static constexpr float NEOPIXEL_IDLE_MA =
    (GLOW_STRIP_PIXELS + CYLON_STRIP_PIXELS) * MA_IDLE_PER_PIXEL;

// Cylon eye colour: a warm yellow (pure 255,255,0 looks greenish on WS2812s).
static constexpr uint8_t CYLON_R = 255;
static constexpr uint8_t CYLON_G = 170;
static constexpr uint8_t CYLON_B = 0;

// The trail fades by CYLON_DECAY every frame and the head adds at most one
// full pixel's worth of light per frame, so the total lit amount is bounded
// by the geometric series 1 / (1 - CYLON_DECAY) pixels at full colour.
static constexpr float CYLON_DECAY = 0.8f;
static constexpr float CYLON_MAX_MA =
    (1.0f / (1.0f - CYLON_DECAY)) *
    ((CYLON_R + CYLON_G + CYLON_B) / 255.0f) * MA_PER_CHANNEL_FULL;

// Whatever is left goes to the red glow. All 48 pixels light up at once at
// the top of the pulse, so this is the strip that actually needs capping.
static constexpr float GLOW_AVAILABLE_MA =
    NEOPIXEL_BUDGET_MA - NEOPIXEL_MARGIN_MA - NEOPIXEL_IDLE_MA - CYLON_MAX_MA;
static constexpr float GLOW_RED_MAX_F =
    GLOW_AVAILABLE_MA / GLOW_STRIP_PIXELS / MA_PER_CHANNEL_FULL * 255.0f;
static constexpr uint8_t GLOW_RED_MAX = GLOW_RED_MAX_F > 255.0f ? 255 : (uint8_t)GLOW_RED_MAX_F;

static_assert(GLOW_AVAILABLE_MA > 0, "neopixel budget exhausted before the glow strip");
static_assert(NEOPIXEL_IDLE_MA + CYLON_MAX_MA +
              GLOW_STRIP_PIXELS * (GLOW_RED_MAX / 255.0f) * MA_PER_CHANNEL_FULL
              <= NEOPIXEL_BUDGET_MA - NEOPIXEL_MARGIN_MA,
              "neopixel worst case exceeds the power budget");

static void neopixel_task(void *arg) {
  log_i("neopixel task on core %d: glow red max %u/255, worst case ~%d mA of %d mA budget",
        xPortGetCoreID(), GLOW_RED_MAX,
        (int)(NEOPIXEL_IDLE_MA + CYLON_MAX_MA +
              GLOW_STRIP_PIXELS * (GLOW_RED_MAX / 255.0f) * MA_PER_CHANNEL_FULL),
        (int)NEOPIXEL_BUDGET_MA);

  glow_strip.begin();
  glow_strip.show();  // all off
  cylon_strip.begin();
  cylon_strip.show();

  const uint32_t PULSE_PERIOD_MS = 4000;
  const uint32_t CYLON_SWEEP_MS = 1200;    // one end to the other
  const uint32_t UPDATE_INTERVAL_MS = 20;  // ~50 Hz, smooth without flooding the bus

  // Per-pixel brightness of the Cylon eye and its trail, 0..1.
  float cylon_level[CYLON_STRIP_PIXELS] = {0};

  for (;;) {
    uint32_t now = millis();

    // Red glow.
    uint32_t phase_ms = now % PULSE_PERIOD_MS;
    float phase = (2.0f * PI * phase_ms) / PULSE_PERIOD_MS;
    float brightness = (sinf(phase - PI / 2.0f) + 1.0f) / 2.0f;  // 0..1, starts at 0

    uint8_t red = (uint8_t)(brightness * GLOW_RED_MAX);
    uint32_t color = glow_strip.Color(red, 0, 0);
    for (int i = 0; i < GLOW_STRIP_PIXELS; i++) {
      glow_strip.setPixelColor(i, color);
    }

    // Cylon: the eye bounces end to end (triangle wave), leaving a trail
    // that fades by CYLON_DECAY each frame. Fading the existing trail rather
    // than drawing it relative to the head means it follows the eye
    // naturally through the turnaround at each end.
    uint32_t t = now % (2 * CYLON_SWEEP_MS);
    float sweep = (t < CYLON_SWEEP_MS) ? (float)t / CYLON_SWEEP_MS
                                       : 2.0f - (float)t / CYLON_SWEEP_MS;  // 0..1..0
    float head = sweep * (CYLON_STRIP_PIXELS - 1);

    for (int i = 0; i < CYLON_STRIP_PIXELS; i++) {
      cylon_level[i] *= CYLON_DECAY;
    }
    // Split the head across the two pixels it sits between, so it glides
    // instead of stepping. The two weights sum to 1, which is what the
    // CYLON_MAX_MA bound relies on.
    int head_i = (int)head;
    float frac = head - head_i;
    cylon_level[head_i] = max(cylon_level[head_i], 1.0f - frac);
    if (head_i + 1 < CYLON_STRIP_PIXELS) {
      cylon_level[head_i + 1] = max(cylon_level[head_i + 1], frac);
    }

    for (int i = 0; i < CYLON_STRIP_PIXELS; i++) {
      float l = cylon_level[i];
      cylon_strip.setPixelColor(i, cylon_strip.Color((uint8_t)(l * CYLON_R),
                                                     (uint8_t)(l * CYLON_G),
                                                     (uint8_t)(l * CYLON_B)));
    }

    glow_strip.show();
    cylon_strip.show();

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

  log_i("BigDripper, setup() on core %d", xPortGetCoreID());

  // Before the print task starts, since it's created with the saved
  // settings.
  params_begin();

  // Before the print task starts, since it pushes to the preview queue.
  // No-op in normal builds.
  console_preview_begin();

  printer_begin(params_get());

  // 4096-byte stack: it calls log_i, and the vsnprintf underneath is the
  // stack-hungry part.
  xTaskCreatePinnedToCore(neopixel_task, "neopixel", 4096, NULL, 1, NULL, BACKGROUND_CORE);

  // The API, then the two ways to reach it.
  api_begin();
  web_begin();
  ble_begin();
}

void loop() {
  // Everything lives in its own pinned task, so loopTask has nothing to do.
  // It still has to yield: returning immediately would spin core 1 at full
  // tilt and starve the idle task.
  vTaskDelay(pdMS_TO_TICKS(1000));
}
