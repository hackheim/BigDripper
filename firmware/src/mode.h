#pragma once
#include <Arduino.h>
#include "drip/types.h"

// Names for the print engine's modes (drip::Mode) as the web page and
// /status use them: "off", "prime", "trace", "lines", "text".
//
// The mode itself lives in the print engine; set it with printer_set_mode().
// It starts as Text and isn't persisted, so a reboot always comes back
// printing text with every valve closed -- in particular Prime never comes
// back on by itself after a reset.

const char *mode_name(drip::Mode mode);

// Inverse of mode_name(). Returns false (and leaves *out alone) for anything
// else.
bool mode_from_name(const String &name, drip::Mode *out);
