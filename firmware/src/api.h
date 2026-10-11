#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "printer.h"

// The board's API, whatever a request arrives over: web.cpp maps HTTP
// routes onto it, ble.cpp a Bluetooth characteristic. Every request is an
// operation name and a JSON object of arguments; the reply is a status code
// (HTTP's, also used over Bluetooth) and a JSON body. Documented in
// web/API.md.
//
// Safe to call from any task except the print task. Calls are taken one at
// a time.

// In every status as "api". Bump it when an operation, its arguments or
// replies change in a way an older page (or newer one) would get wrong: a
// page hosted elsewhere (Cloudflare) can be a different version from the
// firmware. Must match API_VERSION in web/app.js.
const int API_VERSION = 1;

// The settings password. Over HTTP it's basic auth with this user.
extern const char *const API_SETTINGS_USER;
extern const char *const API_SETTINGS_PASSWORD;

// Call once from setup(), before anything else here.
void api_begin();

// Carries out `op` with `args` and fills `reply` with the result, or with
// {"error": "..."} if it failed (and then changed nothing). `authorized`:
// the request came with the settings password. Returns 200, 400, 401, 404
// or 409.
int api_call(const char *op, JsonVariantConst args, bool authorized, JsonDocument &reply);

// For event streams: the status as the "status" op replies, and history
// records as the "history" op replies.
void api_status_json(JsonDocument &doc);
void api_history_json(JsonDocument &doc, const History::Read &rd, const Record *recs);

// Random per boot, in every status: a client that sees it change knows the
// board rebooted and history sequence numbers started over.
uint32_t api_boot_id();
