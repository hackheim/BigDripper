// Host-side checks for drip::Ring, single-threaded: sequence numbers,
// limits, being lapped by the writer, and a reader from before a reboot.
// Run with lib/drip_core/test/run.sh from firmware/.
#include "check.h"
#include "drip/ring.h"

using namespace drip;

typedef Ring<uint32_t, 8> R8;

// Pushes the sequence numbers themselves, so values show what was read.
static void fill(R8 &r, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) r.push(r.head());
}

TEST(ring_empty) {
  R8 r;
  uint32_t out[8];
  R8::Read rd = r.read(0, out, 8);
  CHECK_EQ(rd.count, 0);
  CHECK_EQ(rd.next, 0);
  CHECK_EQ(rd.lost, 0);
  CHECK(!rd.reset);
}

TEST(ring_reads_from_since) {
  R8 r;
  fill(r, 5);
  uint32_t out[8];
  R8::Read rd = r.read(0, out, 8);
  CHECK_EQ(rd.count, 5);
  CHECK_EQ(rd.first, 0);
  CHECK_EQ(rd.next, 5);
  for (uint32_t i = 0; i < 5; i++) CHECK_EQ(out[i], i);
  rd = r.read(3, out, 8);
  CHECK_EQ(rd.count, 2);
  CHECK_EQ(out[0], 3);
  CHECK_EQ(rd.next, 5);
  rd = r.read(5, out, 8);
  CHECK_EQ(rd.count, 0);
  CHECK_EQ(rd.next, 5);
}

TEST(ring_respects_max) {
  R8 r;
  fill(r, 6);
  uint32_t out[8];
  R8::Read rd = r.read(0, out, 2);
  CHECK_EQ(rd.count, 2);
  CHECK_EQ(rd.next, 2);
  rd = r.read(rd.next, out, 2);
  CHECK_EQ(out[0], 2);
  CHECK_EQ(rd.next, 4);
}

TEST(ring_lapped_reader_is_told_what_it_lost) {
  R8 r;
  fill(r, 20);
  uint32_t out[8];
  R8::Read rd = r.read(0, out, 8);
  // The slot the writer would fill next (seq 12's) counts as suspect, so
  // 13..19 come back and 0..12 are lost.
  CHECK_EQ(rd.first, 13);
  CHECK_EQ(rd.count, 7);
  CHECK_EQ(rd.lost, 13);
  CHECK_EQ(rd.next, 20);
  CHECK_EQ(out[0], 13);
  CHECK_EQ(out[6], 19);
  // Caught up: nothing more lost.
  fill(r, 3);
  rd = r.read(rd.next, out, 8);
  CHECK_EQ(rd.lost, 0);
  CHECK_EQ(rd.count, 3);
  CHECK_EQ(out[0], 20);
}

TEST(ring_reader_ahead_of_writer_resets) {
  R8 r;
  fill(r, 5);
  uint32_t out[8];
  R8::Read rd = r.read(1000, out, 8);
  CHECK(rd.reset);
  CHECK_EQ(rd.lost, 0);
  CHECK_EQ(rd.first, 0);
  CHECK_EQ(rd.count, 5);
  CHECK_EQ(rd.next, 5);
}
