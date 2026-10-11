#pragma once
#include <Arduino.h>

// The API (api.h) over Bluetooth LE, for the web UI served from somewhere
// other than the board (Web Bluetooth), so a phone can stay on its own
// network. One GATT service, advertised as "BigDripper": requests are
// written to one characteristic, and replies and the event stream (status,
// history, clock) come back as notifications on another. The protocol is in
// web/API.md, "Bluetooth".
//
// Runs on core 0, beside WiFi. Call once from setup(), after printer_begin()
// and api_begin().
void ble_begin();
