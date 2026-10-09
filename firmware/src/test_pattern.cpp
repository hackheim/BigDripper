#include "test_pattern.h"
#include "params.h"

uint16_t test_pattern_bits(int32_t column) {
  int32_t spacing = (int32_t)params_get_test_line_spacing();
  if (spacing <= 0) spacing = 1;
  return (column % spacing == 0) ? 0xFFFF : 0x0000;
}
