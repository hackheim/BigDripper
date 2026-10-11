// The print engine in the browser, for the simulator on the web page
// (web/sim.js). Built to web/sim.wasm by sim/build.sh, from the same
// sources as the firmware: the engine (lib/drip_core), the fonts and the
// settings checks and conversion (src/settings.cpp). So the simulator fires
// exactly what the board would for the same settings and ride.
//
// It drives drip::Engine the way printer.cpp does, against a simulated
// encoder instead of the PCNT: the engine is stepped when the position
// leaves its wake window or its deadline passes, never in between.
//
// Everything goes through static buffers, so JS needs no allocator:
//   sim_settings()  12 doubles, the fields of Settings in order
//   sim_text()      the text, UTF-8, NUL-terminated
//   sim_profile()   the ride: (time s, speed km/h) pairs, linear between
// and after sim_run(), sim_commands(), sim_track() and sim_ride_ms().
#include <math.h>
#include <string.h>
#include "drip/engine.h"
#include "font.h"
#include "params.h"

#define EXPORT extern "C" __attribute__((used, visibility("default")))

static const int SETTINGS_FIELDS = 12;
static double settings_buf[SETTINGS_FIELDS];
static char text_buf[1024];

static const int MAX_PROFILE = 4096;
static double profile_buf[MAX_PROFILE * 2];

// One coil command out of the engine, as printer.cpp would hand it to the
// coils: at t_ms, set the coils to `bits` until close_ms (or the next
// command). Doubles, for JS to read as a Float64Array: 6 per command.
struct Command {
  double t_ms, bits, close_ms;
  double column;       // the column it fires, -1 if it isn't one
  double stretch_pct;  // a column's width, % of normal
  double overrun;      // the previous burst was still open
};
static const int MAX_COMMANDS = 20000;
static Command commands[MAX_COMMANDS];

// Where the cart is, in mm, every millisecond of the ride.
static const int MAX_TRACK_MS = 120000;
static float track[MAX_TRACK_MS];

static const double STEP_US = 20;       // simulation time step
static const double TAIL_MS = 1000;     // ride on after the message, for dribble to land
static const double MAX_RIDE_MS = MAX_TRACK_MS - TAIL_MS;

static Settings settings_from_buf() {
  const double *v = settings_buf;
  Settings s;
  s.scale_mm_per_detent = (float)v[0];
  s.max_text_len = (uint32_t)v[1];
  s.print_pause_ms = (uint32_t)v[2];
  s.clicks_per_column = (uint32_t)v[3];
  s.column_burst_ms = (uint32_t)v[4];
  s.burst_duty_pct = (uint32_t)v[5];
  s.min_burst_ms = (uint32_t)v[6];
  s.trace_gap_ms = (uint32_t)v[7];
  s.test_line_spacing = (uint32_t)v[8];
  s.letter_spacing = (uint32_t)v[9];
  s.stretch_above_kmh = (float)v[10];
  s.max_stretch_pct = (uint32_t)v[11];
  return s;
}

EXPORT double *sim_settings() { return settings_buf; }
EXPORT char *sim_text() { return text_buf; }
EXPORT int sim_text_size() { return sizeof(text_buf); }
EXPORT double *sim_profile() { return profile_buf; }
EXPORT int sim_profile_max() { return MAX_PROFILE; }
EXPORT double *sim_commands() { return &commands[0].t_ms; }
EXPORT float *sim_track() { return track; }

// Fills sim_settings() with the board's defaults.
EXPORT void sim_defaults() {
  const Settings &s = SETTINGS_DEFAULTS;
  double v[SETTINGS_FIELDS] = {s.scale_mm_per_detent, (double)s.max_text_len, (double)s.print_pause_ms,
                               (double)s.clicks_per_column, (double)s.column_burst_ms,
                               (double)s.burst_duty_pct, (double)s.min_burst_ms, (double)s.trace_gap_ms,
                               (double)s.test_line_spacing, (double)s.letter_spacing,
                               s.stretch_above_kmh, (double)s.max_stretch_pct};
  memcpy(settings_buf, v, sizeof(v));
}

// The settings in sim_settings(), checked as the board checks them: NULL if
// fine, otherwise the board's reason.
EXPORT const char *sim_check() { return params_check(settings_from_buf()); }

static double speed_kmh(const double *pts, int n, double t_s) {
  if (n <= 0) return 0;
  if (t_s <= pts[0]) return pts[1];
  for (int i = 1; i < n; i++) {
    double t0 = pts[2 * i - 2], t1 = pts[2 * i];
    if (t_s <= t1) {
      double f = t1 > t0 ? (t_s - t0) / (t1 - t0) : 1;
      return pts[2 * i - 1] + f * (pts[2 * i + 1] - pts[2 * i - 1]);
    }
  }
  return pts[2 * n - 1];
}

// Prints sim_text() once in `font` (0 Spleen, 1 Drip, 2 Drip bold) with the
// settings in sim_settings(), over the ride in the first `points` pairs of
// sim_profile(). Returns how many commands are in sim_commands(), or -1 if
// the settings are invalid, -2 if the text prints nothing. sim_ride_ms()
// then says how long sim_track() is.
static int ride_ms = 0;
EXPORT int sim_ride_ms() { return ride_ms; }

EXPORT int sim_run(int points, int font, int invert) {
  Settings s = settings_from_buf();
  if (params_check(s) != nullptr) return -1;
  drip::Params p = params_to_engine(s);

  drip::Message m;
  m.columns = font_render(String(text_buf), (Font)font, invert != 0, (int)s.letter_spacing);
  if (m.columns.empty()) return -2;

  // The message is queued behind a blank default text, so it prints once,
  // from its first column, as soon as the cart moves.
  drip::Message blank;
  blank.columns.push_back(0);
  drip::Engine engine(blank, p);
  engine.enqueue(m);

  double mm_per_count = s.scale_mm_per_detent / ENCODER_COUNTS_PER_DETENT;
  double pos_mm = 0;
  drip::Sample sample = {0, 0};
  drip::Wake wake = {drip::POSITION_MIN, drip::POSITION_MAX, drip::NEVER};
  int n = 0;
  double end_ms = MAX_RIDE_MS;
  double next_track_ms = 0;
  int tracked = 0;

  bool first = true;
  for (double t_us = 0;; t_us += STEP_US) {
    double t_ms = t_us / 1000;
    if (t_ms >= next_track_ms) {
      if (tracked >= MAX_TRACK_MS || t_ms > end_ms) break;
      track[tracked++] = (float)pos_mm;
      next_track_ms += 1;
    }

    // The encoder: whole counts only, as the PCNT sees them.
    drip::Position counts = (drip::Position)floor(pos_mm / mm_per_count);
    if (counts != sample.position) sample = {counts, (drip::Micros)t_us};

    bool due = first || counts < wake.lo || counts > wake.hi ||
               (wake.deadline != drip::NEVER && (drip::Micros)t_us >= wake.deadline);
    if (due) {
      first = false;
      drip::StepResult r = engine.step((drip::Micros)t_us, sample);
      wake = r.wake;
      bool overrun = false;
      int32_t column = -1;
      for (int i = 0; i < r.event_count; i++) {
        const drip::Event &e = r.events[i];
        if (e.kind == drip::Event::Overrun) overrun = true;
        if (e.kind == drip::Event::ColumnFired) column = e.value;
        if (e.kind == drip::Event::MessageFinished && end_ms == MAX_RIDE_MS) end_ms = t_ms + TAIL_MS;
      }
      if (r.has_coils && n < MAX_COMMANDS) {
        Command &c = commands[n++];
        c.t_ms = t_ms;
        c.bits = r.coils.bits;
        c.close_ms = r.coils.close_at == drip::NEVER ? 1e12 : r.coils.close_at / 1000.0;
        c.column = column;
        c.stretch_pct = column >= 0 ? 100.0 * engine.column_width() / p.counts_per_column : 0;
        c.overrun = overrun;
      }
    }

    double v_mm_per_us = speed_kmh(profile_buf, points, t_ms / 1000) / 3600.0;
    if (v_mm_per_us > 0) pos_mm += v_mm_per_us * STEP_US;  // forward only
  }
  ride_ms = tracked;
  return n;
}
