#include "trace.h"
#include <atomic>

static std::atomic<bool> trace_active(false);

void trace_start() {
  trace_active.store(true);
}

void trace_stop() {
  trace_active.store(false);
}

bool trace_is_active() {
  return trace_active.load();
}
