#pragma once
#include <Arduino.h>
#include "drip/types.h"

// The 16 valve coils, COIL1..COIL16.

const int NUM_COILS = 16;

// Configures the coil pins (all closed) and the burst timer. The timer's
// interrupt lands on the core this runs on, so call it from the print
// engine's task on its core.
void coils_begin();

// Applies a CoilCommand from the print engine: sets the coils now, and
// closes them all at `close_at` (esp_timer_get_time() time) unless another
// command comes first. NEVER holds them until the next command.
void coils_apply(const drip::CoilCommand &cmd);
