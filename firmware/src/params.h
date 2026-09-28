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

// Encoder detents per printed pixel column. Governs how fast turning the
// wheel scans across the currently printing text: the print engine advances
// one font column every params_get_clicks_per_column() detents.
uint32_t params_get_clicks_per_column();
void params_set_clicks_per_column(uint32_t v);

// How long a column's coils stay energized once the scan lands on it, in
// milliseconds. The print engine fires this as a brief burst rather than
// holding the coils on until the scan reaches the next column.
uint32_t params_get_column_burst_ms();
void params_set_column_burst_ms(uint32_t v);

// Debug trace mode only: how long all coils stay off between bursts, in
// milliseconds. The burst itself uses params_get_column_burst_ms().
uint32_t params_get_trace_gap_ms();
void params_set_trace_gap_ms(uint32_t v);

// Longest text the queue will accept, in characters.
size_t params_get_max_text_len();
void params_set_max_text_len(size_t v);

// Pause between one printed text finishing and the next one starting.
uint32_t params_get_print_pause_ms();
void params_set_print_pause_ms(uint32_t v);
