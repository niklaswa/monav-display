#pragma once

// Binary transfers on the `bulk` characteristic (PROTOCOL.md, "Binary transfers"): the chunk
// header and the receive-side bookkeeping of one transfer — which offset comes next, when a gap
// is reported (once), when an acknowledgement is due. Portable, so the host checks cover it.

#include <cstddef>
#include <cstdint>

namespace monav::bulk {

constexpr size_t kHeader = 5;           // id (1) + offset (4, little endian)
constexpr uint32_t kAckEvery = 16384;   // `got` every 16 KiB taken in
constexpr uint32_t kWindow = 32768;     // the phone keeps at most this much beyond the last `got` in flight

struct Chunk {
  uint8_t id = 0;
  uint32_t offset = 0;
  const uint8_t* data = nullptr;
  size_t len = 0;
};

/** Splits one write into header and data. False when it is too short to carry a header. */
bool parseChunk(const uint8_t* write, size_t len, Chunk& out);

class Slot {
 public:
  enum class Verdict : uint8_t {
    Take,     // the next expected bytes: store them
    Drop,     // not for this slot, or a repeat inside a gap already reported
    AskFrom,  // a gap: drop it and ask for the rest from next() (once per gap)
  };

  void begin(uint8_t id, uint32_t size);
  void close() { active_ = false; }

  bool active() const { return active_; }
  uint8_t id() const { return id_; }
  uint32_t size() const { return size_; }
  /** The offset the next write must carry. */
  uint32_t next() const { return next_; }
  bool received() const { return active_ && next_ == size_; }

  /** A write for this transfer arrived. On Take, next() has moved past it. */
  Verdict offer(uint32_t offset, size_t len);

  /**
   * The bytes up to `stored` are taken in (in RAM, or written to flash). Returns true and the value
   * to acknowledge when a 16 KiB boundary was crossed or the transfer is complete.
   */
  bool stored(uint32_t stored, uint32_t& ack);

 private:
  bool active_ = false;
  uint8_t id_ = 0;
  uint32_t size_ = 0;
  uint32_t next_ = 0;
  uint32_t acked_ = 0;
  bool asked_ = false;
};

}  // namespace monav::bulk
