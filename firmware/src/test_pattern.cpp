#include "test_pattern.h"
#include "params.h"
#include <atomic>

static std::atomic<bool> test_pattern_active(false);

void test_pattern_start() {
  test_pattern_active.store(true);
}

void test_pattern_stop() {
  test_pattern_active.store(false);
}

bool test_pattern_is_active() {
  return test_pattern_active.load();
}

uint16_t test_pattern_bits(int32_t column) {
  int32_t spacing = (int32_t)params_get_test_line_spacing();
  if (spacing <= 0) spacing = 1;
  return (column % spacing == 0) ? 0xFFFF : 0x0000;
}
