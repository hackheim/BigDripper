#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace drip {

// A fixed-size history written by one task and read by any number of
// others, possibly on the other core, without locks and without the writer
// ever waiting.
//
// Every value pushed gets the next sequence number (0, 1, 2, ...). The
// ring keeps the last N; a reader asks for everything from the sequence
// number it has got to, and is told how many it missed if the writer has
// lapped it. Readers keep their own position, so each can be at a
// different point and none affects the others or the writer.
//
// T must be trivially copyable. N must be a power of two.
template <typename T, size_t N>
class Ring {
  static_assert((N & (N - 1)) == 0, "N must be a power of two");

 public:
  struct Read {
    size_t count;    // values copied out
    uint32_t first;  // sequence number of the first one copied
    uint32_t next;   // where to read from next time
    uint32_t lost;   // between `since` and `first`: overwritten before they were read
    bool reset;      // `since` was ahead of the writer, e.g. from before a reboot
  };

  // Writer only.
  void push(const T &v) {
    uint32_t seq = head_.load(std::memory_order_relaxed);
    slots_[seq % N] = v;
    head_.store(seq + 1, std::memory_order_release);
  }

  // The sequence number the next push() will get.
  uint32_t head() const { return head_.load(std::memory_order_acquire); }

  // Copies out up to `max` values from sequence number `since` on, oldest
  // first. Values the writer overwrites while they're being copied are
  // dropped and counted as lost, so what comes back is always whole.
  Read read(uint32_t since, T *out, size_t max) const {
    Read r = {0, since, since, 0, false};
    uint32_t head = head_.load(std::memory_order_acquire);
    if ((int32_t)(head - since) < 0) {
      r.reset = true;
      since = head > N ? head - N : 0;
    }
    uint32_t oldest = head > N ? head - N : 0;
    uint32_t from = (int32_t)(since - oldest) < 0 ? oldest : since;
    uint32_t to = head;
    if (to - from > max) to = from + (uint32_t)max;
    for (uint32_t s = from; s != to; s++) out[s - from] = slots_[s % N];

    // Anything the writer may have started overwriting since `head` was
    // read is suspect: the slot it's writing now held `new_head - N`.
    std::atomic_thread_fence(std::memory_order_acquire);
    uint32_t new_head = head_.load(std::memory_order_relaxed);
    uint32_t safe = new_head + 1 > N ? new_head + 1 - N : 0;
    uint32_t skip = (int32_t)(safe - from) > 0 ? safe - from : 0;
    if (skip > to - from) skip = to - from;
    for (uint32_t i = skip; i < to - from; i++) out[i - skip] = out[i];

    r.first = from + skip;
    r.count = to - from - skip;
    r.next = r.first + (uint32_t)r.count;
    if (!r.reset) r.lost = r.first - since;
    return r;
  }

 private:
  T slots_[N];
  std::atomic<uint32_t> head_{0};
};

}  // namespace drip
