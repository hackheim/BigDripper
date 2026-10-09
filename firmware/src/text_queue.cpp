#include "text_queue.h"
#include "params.h"
#include <deque>

static SemaphoreHandle_t queue_mutex = xSemaphoreCreateMutex();
static std::deque<QueuedText> queue;

// Bytes allowed per glyph of the length limit before text_queue_push()
// rejects a text outright. Above the longest emoji shortcode
// (":big_smile:", 11), so a text the glyph count accepts is never cut off
// by this.
static const size_t TEXT_QUEUE_MAX_BYTES_PER_GLYPH = 16;

// millis() timestamp until which text_queue_current() reports empty, i.e.
// the gap between one printed text and the next. 0 means "not paused".
static uint32_t pause_until_ms = 0;

const char TEXT_QUEUE_DEFAULT_TEXT[] = " BIG DRIPPER ";

bool text_queue_push(const String &text, Font font, bool invert) {
  // Limit is in printed glyphs (font_glyph_count()), so an emoji
  // shortcode or Æ counts as one. The byte cap is only a safety net against
  // a huge POST, generous enough that it never bites before the real limit.
  size_t max_len = params_get_max_text_len();
  if (text.length() > TEXT_QUEUE_MAX_BYTES_PER_GLYPH * max_len) {
    return false;
  }
  size_t glyphs = font_glyph_count(text, font);
  if (glyphs == 0 || glyphs > max_len) {
    return false;
  }

  xSemaphoreTake(queue_mutex, portMAX_DELAY);
  bool ok = queue.size() < TEXT_QUEUE_MAX_DEPTH;
  if (ok) {
    queue.push_back({text, font, invert});
  }
  xSemaphoreGive(queue_mutex);
  return ok;
}

QueuedText text_queue_current() {
  xSemaphoreTake(queue_mutex, portMAX_DELAY);
  bool paused = pause_until_ms != 0 && millis() < pause_until_ms;
  QueuedText current = (!paused && !queue.empty()) ? queue.front()
                                                   : QueuedText{String(""), TEXT_QUEUE_DEFAULT_FONT, false};
  xSemaphoreGive(queue_mutex);
  return current;
}

bool text_queue_is_idle() {
  xSemaphoreTake(queue_mutex, portMAX_DELAY);
  bool paused = pause_until_ms != 0 && millis() < pause_until_ms;
  bool idle = !paused && queue.empty();
  xSemaphoreGive(queue_mutex);
  return idle;
}

std::vector<QueuedText> text_queue_pending() {
  xSemaphoreTake(queue_mutex, portMAX_DELAY);
  // front() is "current", so pending is everything after it.
  std::vector<QueuedText> pending(queue.empty() ? 0 : queue.size() - 1);
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
