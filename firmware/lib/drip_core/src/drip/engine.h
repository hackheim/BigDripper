#pragma once
#include <deque>
#include <string>
#include <vector>
#include "params.h"
#include "scanner.h"
#include "speed.h"
#include "types.h"

namespace drip {

// How a message was asked for. The engine only carries this around for
// status(); what it prints is Message::columns.
struct MessageInfo {
  std::string text;
  uint8_t font = 0;  // a Font, opaque here
  bool invert = false;
};

struct Message {
  MessageInfo info;
  std::vector<uint16_t> columns;  // font_render(), one per column
};

// A snapshot of what the engine is doing, for the web page.
struct Status {
  Mode mode = Mode::Text;
  // What Text prints now. Empty while paused between messages.
  MessageInfo current;
  bool printing_default = false;  // `current` is the default text
  bool paused = false;            // between two messages
  std::vector<MessageInfo> pending;  // queued behind `current`, in order
  int32_t column = -1;               // last column scanned, -1 for none yet
  int32_t total_columns = 0;         // of `current`; 0 in Lines
  uint32_t overruns = 0;
};

// The print engine: modes, the text queue and the firing strategy, as one
// single-threaded object with no clock, no pins and no locks of its own.
//
// The shell owns the concurrency. It gives commands (set_mode(), enqueue(),
// set_params()) and runs step() from one task, and calls step() again
// whenever the last StepResult::wake says to, or right after a command.
// Commands only record what's wanted; it's the next step() that acts on
// them, so every coil change comes out of step().
class Engine {
 public:
  static const size_t QUEUE_DEPTH = 8;

  // `default_text` prints on a loop while the queue is empty, and must have
  // at least one column. Starts in Text, with nothing fired until the
  // wheel moves.
  explicit Engine(Message default_text, const Params &params = Params());

  // The scanner points into the engine's own messages.
  Engine(const Engine &) = delete;
  Engine &operator=(const Engine &) = delete;

  // Takes effect on the next step(). Setting the mode it's already in does
  // nothing, so two phones setting the same mode don't restart anything.
  void set_mode(Mode mode);
  Mode mode() const { return requested_mode_; }

  // Takes effect on the next step(). The caller validates first
  // (params_validate()); the engine just guards against values that would
  // break its arithmetic.
  void set_params(const Params &params);
  const Params &params() const { return params_; }

  // Adds a message to the back of the queue. False (no-op) if it prints
  // nothing or the queue already holds QUEUE_DEPTH. A queued message
  // interrupts the default text right away, but never a queued one.
  bool enqueue(Message m);

  // Runs the engine at `now`, with the encoder last seen at `encoder`.
  StepResult step(Micros now, const Sample &encoder);

  Status status() const;

 private:
  void enter(Mode mode, Micros now, const Sample &s, StepResult &out);
  void step_text(Micros now, const Sample &s, StepResult &out);
  void start_message(const Sample &s, StepResult &out);

  Params params_;
  Message default_text_;
  std::deque<Message> queue_;

  bool started_ = false;  // has step() entered the first mode yet
  Mode mode_ = Mode::Text;
  Mode requested_mode_ = Mode::Text;

  SpeedEstimator speed_;
  ColumnScanner scanner_;

  // Text: whether a message is being scanned, and which. The front of the
  // queue is current while printing_queued_.
  bool scanning_message_ = false;
  bool printing_queued_ = false;
  Micros pause_until_ = 0;  // > now while paused between messages
  bool paused_ = false;

  Micros next_pulse_ = 0;  // Trace: when the next pulse fires
};

}  // namespace drip
