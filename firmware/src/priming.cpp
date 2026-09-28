#include "priming.h"
#include <atomic>

static std::atomic<bool> priming_active(false);

void priming_start() {
  priming_active.store(true);
}

void priming_stop() {
  priming_active.store(false);
}

bool priming_is_active() {
  return priming_active.load();
}
