#include <Arduino.h>
#include "mode.h"
#include <atomic>

static std::atomic<PrintMode> current_mode(PrintMode::Text);

static const struct {
  PrintMode mode;
  const char *name;
} MODE_NAMES[] = {
  {PrintMode::Off, "off"},
  {PrintMode::Prime, "prime"},
  {PrintMode::Trace, "trace"},
  {PrintMode::Lines, "lines"},
  {PrintMode::Text, "text"},
};

PrintMode mode_get() {
  return current_mode.load();
}

void mode_set(PrintMode mode) {
  current_mode.store(mode);
}

const char *mode_name(PrintMode mode) {
  for (const auto &m : MODE_NAMES) {
    if (m.mode == mode) return m.name;
  }
  return "?";
}

bool mode_from_name(const String &name, PrintMode *out) {
  for (const auto &m : MODE_NAMES) {
    if (name == m.name) {
      *out = m.mode;
      return true;
    }
  }
  return false;
}
