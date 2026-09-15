#include "text_queue.h"
#include "params.h"
#include <deque>

static SemaphoreHandle_t queue_mutex = xSemaphoreCreateMutex();
static std::deque<String> queue;

// millis() timestamp until which text_queue_current() reports empty, i.e.
// the gap between one printed text and the next. 0 means "not paused".
static uint32_t pause_until_ms = 0;

bool text_queue_push(const String &text) {
  if (text.length() == 0 || text.length() > params_get_max_text_len()) {
    return false;
  }

  xSemaphoreTake(queue_mutex, portMAX_DELAY);
  bool ok = queue.size() < TEXT_QUEUE_MAX_DEPTH;
  if (ok) {
    queue.push_back(text);
  }
  xSemaphoreGive(queue_mutex);
  return ok;
}

String text_queue_current() {
  xSemaphoreTake(queue_mutex, portMAX_DELAY);
  bool paused = pause_until_ms != 0 && millis() < pause_until_ms;
  String current = (!paused && !queue.empty()) ? queue.front() : String("");
  xSemaphoreGive(queue_mutex);
  return current;
}

std::vector<String> text_queue_pending() {
  xSemaphoreTake(queue_mutex, portMAX_DELAY);
  // front() is "current", so pending is everything after it.
  std::vector<String> pending(queue.empty() ? 0 : queue.size() - 1);
  for (size_t i = 0; i < pending.size(); i++) {
    pending[i] = queue[i + 1];
  }
  xSemaphoreGive(queue_mutex);
  return pending;
}

void text_queue_advance() {
  xSemaphoreTake(queue_mutex, portMAX_DELAY);
  if (!queue.empty()) {
    queue.pop_front();
    pause_until_ms = millis() + params_get_print_pause_ms();
  }
  xSemaphoreGive(queue_mutex);
}
