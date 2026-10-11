// Host-side checks for the print engine core (lib/drip_core).
// Run with lib/drip_core/test/run.sh from firmware/.
//
// Most tests drive the engine through Rig, which plays the shell: it moves
// the encoder one count at a time and only calls step() when the last
// StepResult::wake says to, the way the firmware will with the pulse
// counter's thresholds and a timer.
#include <cstdio>
#include <vector>
#include "drip/engine.h"

using namespace drip;

static int failures = 0;
static const char *current_test = "";

#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      printf("FAIL  %s: %s (line %d)\n", current_test, #cond, __LINE__);         \
      failures++;                                                                \
    }                                                                            \
  } while (0)

#define CHECK_EQ(a, b)                                                           \
  do {                                                                           \
    long long va = (long long)(a), vb = (long long)(b);                          \
    if (va != vb) {                                                              \
      printf("FAIL  %s: %s == %s (%lld vs %lld, line %d)\n", current_test, #a,   \
             #b, va, vb, __LINE__);                                              \
      failures++;                                                                \
    }                                                                            \
  } while (0)

static Message msg(const char *text, std::vector<uint16_t> columns) {
  Message m;
  m.info.text = text;
  m.columns = columns;
  return m;
}

// Small numbers so positions are easy to follow: 4 counts per column.
static Params test_params() {
  Params p;
  p.counts_per_column = 4;
  p.max_burst_us = 20000;
  p.min_burst_us = 4000;
  p.burst_duty_pct = 50;
  p.trace_gap_us = 1000000;
  p.line_spacing = 2;
  p.message_pause_us = 2000000;
  p.speed_timeout_us = 500000;
  return p;
}

static const std::vector<uint16_t> DEFAULT_COLS = {0x0001, 0x0002, 0x0003};
static const std::vector<uint16_t> AB_COLS = {0x0010, 0x0020, 0x0030, 0x0040};

struct Fired {
  int32_t column;
  uint16_t bits;
  Micros burst;
};

struct Rig {
  Engine e;
  Micros now = 1000000;
  Position pos = 0;
  Micros edge_at = 1000000;
  StepResult last;
  int steps = 0;

  // Everything that came out of step(), accumulated.
  std::vector<Fired> fired;
  std::vector<Event> events;
  int coil_commands = 0;
  CoilCommand coils = {0, NEVER};

  explicit Rig(const Params &p = test_params()) : e(msg(" BD ", DEFAULT_COLS), p) { step(); }

  void step() {
    last = e.step(now, {pos, edge_at});
    steps++;
    for (int i = 0; i < last.event_count; i++) {
      events.push_back(last.events[i]);
      if (last.events[i].kind == Event::ColumnFired) {
        fired.push_back({last.events[i].value, last.coils.bits, last.coils.close_at - now});
      }
    }
    if (last.has_coils) {
      coil_commands++;
      coils = last.coils;
    }
  }

  bool should_wake() const {
    return pos < last.wake.lo || pos > last.wake.hi || now >= last.wake.deadline;
  }

  // Moves `counts` (negative = backwards), one count every `us_per_count`.
  void move(int counts, Micros us_per_count = 5000) {
    int dir = counts < 0 ? -1 : 1;
    for (int i = 0; i < counts * dir; i++) {
      advance_to(now + us_per_count);
      pos += dir;
      edge_at = now;
      if (should_wake()) step();
    }
  }

  // Jumps `counts` at once, as if the engine had been too slow to see the
  // counts in between.
  void jump(int counts) {
    now += 1000;
    pos += counts;
    edge_at = now;
    step();
  }

  // Lets time pass, stepping at any deadline on the way.
  void advance_to(Micros t) {
    while (last.wake.deadline <= t) {
      if (last.wake.deadline > now) now = last.wake.deadline;
      step();
    }
    now = t;
  }
  void wait(Micros dt) { advance_to(now + dt); }

  void set_mode(Mode m) {
    e.set_mode(m);
    step();
  }
  bool enqueue(const char *text, const std::vector<uint16_t> &cols) {
    bool ok = e.enqueue(msg(text, cols));
    step();
    return ok;
  }

  int count(Event::Kind kind) const {
    int n = 0;
    for (const Event &ev : events) n += ev.kind == kind;
    return n;
  }
  void clear() {
    fired.clear();
    events.clear();
    coil_commands = 0;
  }
  bool coils_open() const { return coils.bits != 0 && now < coils.close_at; }
};

typedef void (*TestFn)();
struct Test {
  const char *name;
  TestFn fn;
};
static std::vector<Test> &tests() {
  static std::vector<Test> t;
  return t;
}
struct Register {
  Register(const char *name, TestFn fn) { tests().push_back({name, fn}); }
};
#define TEST(name)                          \
  static void name();                       \
  static Register register_##name(#name, name); \
  static void name()

// --- Text --------------------------------------------------------------------

TEST(boot_starts_text_without_firing) {
  Rig r;
  CHECK(r.e.mode() == Mode::Text);
  CHECK(r.last.has_coils);
  CHECK_EQ(r.last.coils.bits, 0);
  CHECK_EQ(r.count(Event::ModeChanged), 1);
  CHECK_EQ(r.count(Event::MessageStarted), 1);
  CHECK(r.fired.empty());
  // Woken by leaving the column we stand in, and by nothing time-based.
  CHECK(r.last.wake.lo <= 0 && r.last.wake.hi == 0);
  CHECK_EQ(r.last.wake.deadline, NEVER);
}

TEST(standstill_never_wakes) {
  Rig r;
  int steps = r.steps;
  r.wait(60 * 1000000LL);
  CHECK_EQ(r.steps, steps);
}

TEST(first_count_forward_fires_column_zero) {
  Rig r;
  r.move(1);
  CHECK_EQ(r.fired.size(), 1);
  CHECK_EQ(r.fired[0].column, 0);
  CHECK_EQ(r.fired[0].bits, 0x0001);
}

TEST(one_step_per_column_at_constant_speed) {
  Rig r;
  int steps = r.steps;
  r.move(5 * 4);  // 5 columns
  CHECK_EQ(r.steps - steps, 5);
  CHECK_EQ(r.fired.size(), 5);
  CHECK_EQ(r.count(Event::ColumnsSkipped), 0);
}

TEST(default_text_loops) {
  Rig r;
  r.move(5 * 4);
  int32_t expect[] = {0, 1, 2, 0, 1};
  for (int i = 0; i < 5; i++) CHECK_EQ(r.fired[i].column, expect[i]);
  CHECK_EQ(r.count(Event::MessageFinished), 0);
}

TEST(burst_follows_speed) {
  Rig r;
  // 5000 us/count * 4 counts = 20 ms columns, 50% duty -> 10 ms.
  r.move(3 * 4, 5000);
  CHECK_EQ(r.fired.back().burst, 10000);
  // Faster: 2000 us/count -> 8 ms columns -> 4 ms as the EMA settles.
  r.move(20 * 4, 2000);
  CHECK(r.fired.back().burst >= 4000 && r.fired.back().burst < 4100);
  // Slower than walking pace: capped at the max.
  r.move(20 * 4, 20000);
  CHECK_EQ(r.fired.back().burst, 20000);
}

TEST(burst_floored_at_min) {
  Rig r;
  r.move(20 * 4, 300);  // 1.2 ms columns
  CHECK_EQ(r.fired.back().burst, 4000);
}

TEST(stopping_forgets_speed) {
  Rig r;
  r.move(3 * 4, 2000);
  CHECK(r.fired.back().burst < 20000);
  r.wait(1000000);
  r.move(4, 2000);
  CHECK_EQ(r.fired.back().burst, 20000);
}

TEST(overruns_counted) {
  Rig r;
  r.move(10 * 4, 500);  // 2 ms columns, but bursts are at least 4 ms
  CHECK(r.count(Event::Overrun) >= 8);
  CHECK_EQ(r.e.status().overruns, (uint32_t)r.count(Event::Overrun));
}

TEST(jump_reports_skipped_columns) {
  Rig r;
  r.jump(12);  // from before column 0 into column 2
  CHECK_EQ(r.fired.size(), 1);
  CHECK_EQ(r.fired[0].column, 2);
  CHECK_EQ(r.fired[0].bits, 0x0003);
  CHECK_EQ(r.count(Event::ColumnsSkipped), 1);
  CHECK_EQ(r.events[r.events.size() - 2].value, 2);
}

TEST(queued_message_interrupts_default_without_firing) {
  Rig r;
  r.move(4);
  r.clear();
  CHECK(r.enqueue("AB", AB_COLS));
  CHECK_EQ(r.count(Event::MessageStarted), 1);
  CHECK_EQ(r.events.back().value, 0);  // queued, not default
  CHECK(r.fired.empty());
  r.move(1);
  CHECK_EQ(r.fired[0].bits, 0x0010);
}

TEST(queued_message_prints_then_pauses_then_default) {
  Rig r;
  r.enqueue("AB", AB_COLS);
  r.clear();
  r.move(4 * 4);
  CHECK_EQ(r.fired.size(), 4);
  for (int i = 0; i < 4; i++) CHECK_EQ(r.fired[i].bits, AB_COLS[i]);

  r.move(1);  // off the end
  CHECK_EQ(r.count(Event::MessageFinished), 1);
  CHECK(!r.last.has_coils);  // the last burst closes on its own
  CHECK(r.e.status().paused);
  CHECK_EQ(r.last.wake.lo, POSITION_MIN);
  CHECK_EQ(r.last.wake.hi, POSITION_MAX);
  Micros pause_end = r.last.wake.deadline;
  CHECK_EQ(pause_end, r.now + 2000000);

  // Moving during the pause doesn't wake the engine at all.
  int steps = r.steps;
  r.move(40, 5000);  // 200 ms
  CHECK_EQ(r.steps, steps);

  r.clear();
  r.advance_to(pause_end);
  CHECK_EQ(r.count(Event::MessageStarted), 1);
  CHECK_EQ(r.events.back().value, 1);  // default
  CHECK(r.fired.empty());
  CHECK(!r.e.status().paused);
  r.move(1);
  CHECK_EQ(r.fired[0].bits, DEFAULT_COLS[0]);
}

TEST(queued_messages_print_in_order) {
  Rig r;
  r.enqueue("A", {0x0100});
  r.enqueue("B", {0x0200});
  r.clear();
  r.move(4);
  CHECK_EQ(r.fired.back().bits, 0x0100);
  r.move(4);
  r.wait(2000000);
  r.move(1);
  CHECK_EQ(r.fired.back().bits, 0x0200);
}

TEST(zero_pause_starts_next_message_immediately) {
  Params p = test_params();
  p.message_pause_us = 0;
  Rig r(p);
  r.enqueue("A", {0x0100});
  r.enqueue("B", {0x0200, 0x0300});
  r.clear();
  // Through A; B then starts one count past where A ended, at count 6.
  r.move(4 + 2);
  CHECK_EQ(r.count(Event::MessageFinished), 1);
  CHECK_EQ(r.fired.size(), 2);
  CHECK_EQ(r.fired[1].bits, 0x0200);
}

TEST(backwards_before_message_start_fires_nothing) {
  Rig r;
  r.enqueue("AB", AB_COLS);
  r.clear();
  r.move(-20);
  CHECK(r.fired.empty());
  CHECK_EQ(r.count(Event::ColumnsSkipped), 0);
}

TEST(enqueue_rejects_empty_and_full) {
  Rig r;
  CHECK(!r.enqueue("", {}));
  for (size_t i = 0; i < Engine::QUEUE_DEPTH; i++) CHECK(r.enqueue("x", {1}));
  CHECK(!r.enqueue("x", {1}));
  CHECK_EQ(r.e.status().pending.size(), Engine::QUEUE_DEPTH - 1);
}

TEST(column_width_change_keeps_current_column) {
  Rig r;
  r.move(4 + 2);  // column 1, partway in
  CHECK_EQ(r.fired.back().column, 1);
  Params p = test_params();
  p.counts_per_column = 10;
  r.e.set_params(p);
  r.step();
  CHECK_EQ(r.fired.size(), 2);  // nothing new fired
  r.move(9);
  CHECK_EQ(r.fired.size(), 2);
  r.move(1);
  CHECK_EQ(r.fired.back().column, 2);
}

// --- Modes -------------------------------------------------------------------

TEST(same_mode_is_a_no_op) {
  Rig r;
  r.clear();
  r.set_mode(Mode::Text);
  CHECK_EQ(r.count(Event::ModeChanged), 0);
  CHECK(!r.last.has_coils);
}

TEST(prime_holds_all_open_until_left) {
  Rig r;
  r.set_mode(Mode::Prime);
  CHECK_EQ(r.coils.bits, 0xFFFF);
  CHECK_EQ(r.coils.close_at, NEVER);
  CHECK_EQ(r.last.wake.deadline, NEVER);
  int steps = r.steps;
  r.move(40);
  r.wait(10000000);
  CHECK_EQ(r.steps, steps);
  CHECK(r.coils_open());

  r.clear();
  r.set_mode(Mode::Text);
  CHECK_EQ(r.coils.bits, 0);
  CHECK(r.fired.empty());
  r.move(1);
  CHECK_EQ(r.fired.size(), 1);
}

TEST(trace_pulses_on_a_timer) {
  Rig r;
  r.set_mode(Mode::Trace);
  Micros t0 = r.now;
  CHECK_EQ(r.coils.bits, 0xFFFF);
  CHECK_EQ(r.coils.close_at, t0 + 20000);
  CHECK_EQ(r.last.wake.deadline, t0 + 20000 + 1000000);
  CHECK_EQ(r.last.wake.lo, POSITION_MIN);

  int commands = r.coil_commands;
  r.wait(500000);
  CHECK(!r.coils_open());
  CHECK_EQ(r.coil_commands, commands);
  r.wait(520000);
  CHECK_EQ(r.coil_commands, commands + 1);
  CHECK_EQ(r.coils.close_at, t0 + 1020000 + 20000);
}

TEST(off_closes_and_ignores_everything) {
  Rig r;
  r.enqueue("AB", AB_COLS);
  r.move(5);
  r.set_mode(Mode::Off);
  CHECK_EQ(r.coils.bits, 0);
  int steps = r.steps;
  r.move(100);
  r.wait(10000000);
  CHECK_EQ(r.steps, steps);
  CHECK(r.e.status().mode == Mode::Off);
  CHECK(r.e.status().current.text == "AB");

  // Back in Text the same message restarts from its first column.
  r.clear();
  r.set_mode(Mode::Text);
  r.move(1);
  CHECK_EQ(r.fired[0].bits, AB_COLS[0]);
}

TEST(lines_prints_test_pattern) {
  Rig r;
  r.set_mode(Mode::Lines);
  r.clear();
  r.move(4 * 4);
  CHECK_EQ(r.fired.size(), 4);
  uint16_t expect[] = {0xFFFF, 0, 0xFFFF, 0};
  for (int i = 0; i < 4; i++) CHECK_EQ(r.fired[i].bits, expect[i]);
  CHECK_EQ(r.fired[1].burst, 0);  // a blank column just closes
  CHECK_EQ(r.e.status().total_columns, 0);
}

TEST(lines_ignores_the_queue) {
  Rig r;
  r.set_mode(Mode::Lines);
  r.clear();
  CHECK(r.enqueue("AB", AB_COLS));
  r.move(4 * 8);
  CHECK_EQ(r.count(Event::MessageStarted), 0);
  CHECK(r.e.status().current.text == "AB");
}

// --- Status and params -------------------------------------------------------

TEST(status_reports_queue) {
  Rig r;
  Status st = r.e.status();
  CHECK(st.printing_default);
  CHECK(st.current.text == " BD ");
  r.enqueue("A", {1});
  r.enqueue("B", {1});
  st = r.e.status();
  CHECK(!st.printing_default);
  CHECK(st.current.text == "A");
  CHECK_EQ(st.pending.size(), 1);
  CHECK_EQ(st.total_columns, 1);
  r.move(4 + 1);  // finish A
  st = r.e.status();
  CHECK(st.paused);
  CHECK(st.current.text == "");
  CHECK_EQ(st.pending.size(), 1);  // B, still waiting
}

TEST(params_validation) {
  Params p;
  CHECK(params_validate(p) == nullptr);
  p.min_burst_us = p.max_burst_us + 1;
  CHECK(params_validate(p) != nullptr);
  p = Params();
  p.burst_duty_pct = 99;
  CHECK(params_validate(p) != nullptr);
  p = Params();
  p.counts_per_column = 0;
  CHECK(params_validate(p) != nullptr);
}

int main() {
  for (const Test &t : tests()) {
    current_test = t.name;
    int before = failures;
    t.fn();
    printf("%s  %s\n", failures == before ? "ok  " : "FAIL", t.name);
  }
  printf("\n%d failure(s)\n", failures);
  return failures ? 1 : 0;
}
