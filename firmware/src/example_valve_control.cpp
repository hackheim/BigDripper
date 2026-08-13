#include <Arduino.h>

// COIL1..COIL16 -> ESP32-S3 GPIO, in order. The state vector is MSB-first:
// bit 15 = COIL1 (coils[0]) ... bit 0 = COIL16 (coils[15]).
const int NUM_COILS = 16;
const uint8_t coils[NUM_COILS] = {9, 10, 11, 47, 48, 45, 1, 6, 7, 8, 38, 39, 40, 41, 42, 2};

// GPIO19/20 are USB D-/D+ on the ESP32-S3. This devkit is programmed over native
// USB (ARDUINO_USB_MODE=1, VID:PID 303A:1001), so driving them as outputs drops
// the board off the bus and the next upload can't find it. COIL1-3 have been
// rerouted off them, so this guard is now just a backstop against re-adding
// them. Set to 0 only once programming moves to UART0 (GPIO43/44).
#define SKIP_USB_PINS 1

static bool usable(uint8_t gpio) {
#if SKIP_USB_PINS
  return gpio != 19 && gpio != 20;
#else
  return true;
#endif
}

void setup() {
  Serial.begin(115200);

  // Native USB CDC only exists once the host has enumerated it, which takes a
  // second or two after boot. Without this wait, everything printed here is
  // written into the void. Bounded so the sketch still runs untethered.
  while (!Serial && millis() < 3000) {
    delay(10);
  }

  log_i("BigDripper valve test, %d coils", NUM_COILS);

  for (int i = 0; i < NUM_COILS; i++) {
    if (usable(coils[i])) {
      pinMode(coils[i], OUTPUT);
      digitalWrite(coils[i], LOW);
      log_d("COIL%d -> GPIO%u", i + 1, coils[i]);
    } else {
      log_w("COIL%d -> GPIO%u skipped (USB pin)", i + 1, coils[i]);
    }
  }
}

void set_coil_state(uint16_t bitVector) {
  uint16_t bit = 0x8000;  // bit 15 -> coils[0] (COIL1)

  for (int i = 0; i < NUM_COILS; i++) {
    if (usable(coils[i])) {
      digitalWrite(coils[i], (bitVector & bit) ? HIGH : LOW);
    }
    bit >>= 1;
  }
}

void loop() {
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
