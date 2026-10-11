#pragma once
#include <Arduino.h>
#include "drip/types.h"

// The carriage encoder, counted by the hardware pulse counter (PCNT) in
// full x4 quadrature with a glitch filter, so no edge depends on an
// interrupt being serviced in time.
//
// Positions are 64-bit counts (drip::Position). Instead of an interrupt per
// edge, the print engine's wake window is set as two PCNT watch points just
// outside it, so the only interrupt is the one that crosses out of it.

// Quadrature edges per detent.
const int ENCODER_COUNTS_PER_DETENT = 4;

// Sets up the counter and its interrupt. The interrupt lands on the core
// this runs on, so call it from the print engine's task on its core. Every
// watch point crossing notifies `task`.
void encoder_begin(TaskHandle_t task);

// The current position, and when it got there: the watch point's interrupt
// time if nothing has moved since, otherwise `now`.
drip::Sample encoder_sample(drip::Micros now);

// Wake `task` when the position leaves [lo, hi]. POSITION_MIN/POSITION_MAX
// leave that side unwatched. Returns false if the position is already
// outside by the time the watch points are set: no interrupt will come for
// that, so the caller should step again straight away.
bool encoder_set_window(drip::Position lo, drip::Position hi);
