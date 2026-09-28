#pragma once

// Debug mode that repeatedly pulses every valve: all on for the configured
// column burst duration, then all off for the configured trace gap
// (params_get_trace_gap_ms()), until stopped. Useful for
// checking the burst timing and that every coil fires. Same threading story
// as priming.h: set by the web server (on WEB_CORE), read by the print engine
// (on VALVE_CORE), so it's a plain atomic.

void trace_start();
void trace_stop();
bool trace_is_active();
