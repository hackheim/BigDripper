#pragma once
#include <Arduino.h>

// Brings up a WiFi SoftAP and a web page for entering the text to print.
// Spawns its own background task, so call this once from setup() and forget
// about it; get_print_text() is how the rest of the firmware reads the result.
void wifi_text_input_begin();

// Thread-safe snapshot of the text most recently submitted via the web page.
// Empty until the first submission.
String get_print_text();
