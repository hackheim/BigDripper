#pragma once
#include <Arduino.h>

// Tunable settings, persisted to NVS via the Preferences library so they
// survive a reboot. Call params_begin() once from setup(), before anything
// else on either core reads or writes a parameter.

void params_begin();

// mm of carriage travel per encoder detent. Calibrates encoder position to a
// physical distance so printed text is neither stretched nor compressed;
// consumed by the (future) print engine, not by anything in this firmware
// yet.
float params_get_scale_mm_per_detent();
void params_set_scale_mm_per_detent(float v);

// Longest text the queue will accept, in characters.
size_t params_get_max_text_len();
void params_set_max_text_len(size_t v);

// Pause between one printed text finishing and the next one starting.
uint32_t params_get_print_pause_ms();
void params_set_print_pause_ms(uint32_t v);
