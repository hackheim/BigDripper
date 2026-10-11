#include "mode.h"

static const struct {
  drip::Mode mode;
  const char *name;
} MODE_NAMES[] = {
  {drip::Mode::Off, "off"},
  {drip::Mode::Prime, "prime"},
  {drip::Mode::Trace, "trace"},
  {drip::Mode::Lines, "lines"},
  {drip::Mode::Text, "text"},
};

const char *mode_name(drip::Mode mode) {
  for (const auto &m : MODE_NAMES) {
    if (m.mode == mode) return m.name;
  }
  return "?";
}

bool mode_from_name(const String &name, drip::Mode *out) {
  for (const auto &m : MODE_NAMES) {
    if (name == m.name) {
      *out = m.mode;
      return true;
    }
  }
  return false;
}
