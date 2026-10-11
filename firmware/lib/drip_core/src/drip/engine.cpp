#include "engine.h"
#include <utility>

namespace drip {

const size_t Engine::QUEUE_DEPTH;

Engine::Engine(Message default_text, const Params &params) : default_text_(std::move(default_text)) {
  set_params(params);
}

void Engine::set_mode(Mode mode) {
  requested_mode_ = mode;
}

void Engine::set_params(const Params &params) {
  params_ = params;
  if (params_.counts_per_column < 1) params_.counts_per_column = 1;
  if (params_.line_spacing < 1) params_.line_spacing = 1;
}

bool Engine::enqueue(Message m) {
  if (m.columns.empty() || queue_.size() >= QUEUE_DEPTH) return false;
  // Safe while the scanner points at the front: push_back on a deque
  // doesn't move the elements already in it.
  queue_.push_back(std::move(m));
  return true;
}

void Engine::set_default_text(Message m) {
  if (m.columns.empty()) return;
  // The scanner points into default_text_.columns, so stop scanning it
  // before it's replaced; the next step starts it again.
  if (scanning_message_ && !printing_queued_) scanning_message_ = false;
  default_text_ = std::move(m);
}

StepResult Engine::step(Micros now, const Sample &encoder) {
  StepResult out;
  // Speed is a property of the bike, not the mode, so it's tracked
  // whatever is printing.
  speed_.update(encoder, params_.speed_timeout_us);
  if (paused_ && now >= pause_until_) paused_ = false;

  if (!started_ || requested_mode_ != mode_) {
    started_ = true;
    enter(requested_mode_, now, encoder, out);
  }

  switch (mode_) {
    case Mode::Off:
    case Mode::Prime:
      // Set on entry and held; nothing to wake up for.
      break;
    case Mode::Trace:
      if (now >= next_pulse_) {
        out.set_coils(0xFFFF, now + params_.max_burst_us);
        next_pulse_ = now + params_.max_burst_us + params_.trace_gap_us;
      }
      out.wake.deadline = next_pulse_;
      break;
    case Mode::Lines:
      scanner_.step(now, encoder, params_, speed_, out);
      break;
    case Mode::Text:
      step_text(now, encoder, out);
      break;
  }
  return out;
}

// Every mode starts by setting the coils outright, so nothing left open by
// the previous one (a burst, Prime, a Trace pulse) carries over.
void Engine::enter(Mode mode, Micros now, const Sample &s, StepResult &out) {
  mode_ = mode;
  out.add_event(Event::ModeChanged, (int32_t)mode);
  switch (mode) {
    case Mode::Off:
      out.set_coils(0x0000, now);
      break;
    case Mode::Prime:
      out.set_coils(0xFFFF, NEVER);
      break;
    case Mode::Trace:
      out.set_coils(0x0000, now);
      next_pulse_ = now;  // first pulse in this same step
      break;
    case Mode::Lines: {
      out.set_coils(0x0000, now);
      ColumnSource src;
      src.test_pattern = true;
      scanner_.start(s, src);
      break;
    }
    case Mode::Text:
      // Restarts whatever was printing from its first column (or keeps
      // waiting out a pause). The queue itself is left as it was.
      out.set_coils(0x0000, now);
      scanning_message_ = false;
      break;
  }
}

void Engine::step_text(Micros now, const Sample &s, StepResult &out) {
  if (paused_) {
    out.wake = {POSITION_MIN, POSITION_MAX, pause_until_};
    return;
  }
  if (!scanning_message_ || (!printing_queued_ && !queue_.empty())) {
    start_message(s, out);
  }
  if (scanner_.step(now, s, params_, speed_, out) != ColumnScanner::Result::Finished) return;

  // Scanned past the last column. The last burst closes on its own timer.
  queue_.pop_front();
  printing_queued_ = false;
  scanning_message_ = false;
  out.add_event(Event::MessageFinished);
  pause_until_ = now + params_.message_pause_us;
  paused_ = pause_until_ > now;
  // Either waits out the pause, or with no pause starts the next message
  // here. A message never finishes in the step that started it, so this
  // recurses at most once.
  step_text(now, s, out);
}

// Starts scanning the front of the queue, or the default text if it's
// empty, with column 0 one count forward of `s`.
void Engine::start_message(const Sample &s, StepResult &out) {
  printing_queued_ = !queue_.empty();
  const Message &m = printing_queued_ ? queue_.front() : default_text_;
  ColumnSource src;
  src.columns = &m.columns;
  src.loop = !printing_queued_;
  scanner_.start(s, src);
  scanning_message_ = true;
  out.add_event(Event::MessageStarted, printing_queued_ ? 0 : 1);
}

// Worked out from the queue rather than from what's being scanned, so it's
// already right between a command and the step that acts on it, and in
// modes other than Text.
Status Engine::status() const {
  Status st;
  st.mode = requested_mode_;
  st.paused = paused_;
  size_t first_pending = 0;
  if (!paused_) {
    if (queue_.empty()) {
      st.current = default_text_.info;
      st.printing_default = true;
    } else {
      st.current = queue_.front().info;
      first_pending = 1;
    }
  }
  for (size_t i = first_pending; i < queue_.size(); i++) st.pending.push_back(queue_[i].info);
  st.column = scanner_.column();
  st.total_columns = scanner_.total_columns();
  st.column_width = scanner_.column_width();
  st.overruns = scanner_.overruns();
  return st;
}

}  // namespace drip
