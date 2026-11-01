#include "core/bulk.h"

namespace monav::bulk {

bool parseChunk(const uint8_t* write, size_t len, Chunk& out) {
  if (write == nullptr || len < kHeader) return false;
  out.id = write[0];
  out.offset = static_cast<uint32_t>(write[1]) | (static_cast<uint32_t>(write[2]) << 8) |
               (static_cast<uint32_t>(write[3]) << 16) | (static_cast<uint32_t>(write[4]) << 24);
  out.data = write + kHeader;
  out.len = len - kHeader;
  return true;
}

void Slot::begin(uint8_t id, uint32_t size) {
  active_ = true;
  id_ = id;
  size_ = size;
  next_ = 0;
  acked_ = 0;
  asked_ = false;
}

Slot::Verdict Slot::offer(uint32_t offset, size_t len) {
  if (!active_) return Verdict::Drop;
  if (offset != next_ || len == 0) {
    // A repeat of bytes already taken, or a gap. Either way the phone should go back to next_,
    // but only one request per gap: the next one waits until bytes flow again.
    if (len == 0 || asked_) return Verdict::Drop;
    asked_ = true;
    return Verdict::AskFrom;
  }
  if (static_cast<uint64_t>(next_) + len > size_) return Verdict::Drop;  // beyond the announced size
  next_ += static_cast<uint32_t>(len);
  asked_ = false;
  return Verdict::Take;
}

bool Slot::stored(uint32_t stored, uint32_t& ack) {
  if (stored >= size_ && acked_ < size_) {
    acked_ = size_;
    ack = size_;
    return true;
  }
  const uint32_t boundary = (stored / kAckEvery) * kAckEvery;
  if (boundary > acked_) {
    acked_ = boundary;
    ack = boundary;
    return true;
  }
  return false;
}

}  // namespace monav::bulk
