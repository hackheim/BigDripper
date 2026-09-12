#pragma once
#include <Arduino.h>

// Brings up a WiFi SoftAP and a web page for entering the text to print.
// Spawns its own background task, so call this once from setup() and forget
// about it; submitted text lands in the queue declared in text_queue.h.
void wifi_text_input_begin();
