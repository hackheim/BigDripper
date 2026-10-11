#pragma once
#include <Arduino.h>

// The WiFi access point, the web UI (web/, embedded at build time by
// scripts/embed_web.py) and the JSON API it talks to, including a
// server-sent event stream of status changes and the print task's history.
// The API is documented in web/API.md.
//
// Everything here runs on core 0, away from the print task. Call once from
// setup(), after printer_begin().
void web_begin();
