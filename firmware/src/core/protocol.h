#pragma once

// Wire format of PROTOCOL.md: chunk framing in both directions, parsing of the phone's JSON
// messages and the JSON the display sends back. Portable (firmware + host preview).

#include <cstddef>
#include <cstdint>

#include "core/model.h"

namespace monav::proto {

constexpr int kVersion = 1;
constexpr size_t kMaxMessage = 1024;
/** Before the MTU exchange a chunk may not be longer than this (ATT default MTU 23 − 3). */
constexpr size_t kDefaultChunk = 20;

constexpr uint8_t kFirst = 0x80;
constexpr uint8_t kLast = 0x40;
constexpr uint8_t kSeqMask = 0x3F;

/**
 * Collects chunks into messages. A FIRST chunk always starts over (dropping a half-collected
 * message); a sequence gap or an oversized message drops what was collected.
 */
class Reassembler {
 public:
  /** Feeds one write. Returns true when it completed a message (see message()/length()). */
  bool feed(const uint8_t* data, size_t len);
  void reset();

  const char* message() const { return buf_; }
  size_t length() const { return len_; }
  /** Messages dropped because of a gap or the size limit (diagnostics). */
  uint32_t dropped() const { return dropped_; }

 private:
  char buf_[kMaxMessage + 1] = {0};
  size_t len_ = 0;
  bool active_ = false;
  uint8_t nextSeq_ = 0;
  uint32_t dropped_ = 0;
};

/**
 * Splits `json` into chunks of at most `maxChunk` bytes (header included) and hands each to
 * `emit(const uint8_t*, size_t)`. `seq` is the connection's running sequence number.
 */
template <typename Emit>
void fragment(const char* json, size_t len, size_t maxChunk, uint8_t& seq, Emit&& emit) {
  if (maxChunk < 2) maxChunk = 2;
  const size_t payload = maxChunk - 1;
  uint8_t chunk[kMaxMessage + 1];
  size_t off = 0;
  do {
    const size_t n = (len - off) < payload ? (len - off) : payload;
    uint8_t hdr = seq & kSeqMask;
    if (off == 0) hdr |= kFirst;
    if (off + n >= len) hdr |= kLast;
    chunk[0] = hdr;
    for (size_t i = 0; i < n; i++) chunk[1 + i] = static_cast<uint8_t>(json[off + i]);
    emit(chunk, n + 1);
    seq = (seq + 1) & kSeqMask;
    off += n;
  } while (off < len);
}

/** Parses one complete message. Unknown types come back as MsgType::Unknown (and are ignored). */
bool parse(const char* json, size_t len, Message& out);

// --- Display → phone -------------------------------------------------------------------------

const char* layoutName(Layout layout);

size_t buildWelcome(char* out, size_t cap, const char* fw, const char* id, bool fresh, int bright, Layout layout);
size_t buildDenied(char* out, size_t cap, const char* id);
/** `status`: brightness and/or layout (pass −1 / nullptr to leave one out). */
size_t buildStatus(char* out, size_t cap, int bright, const Layout* layout = nullptr);
/** `{"t":"bulk","id":7,"got":49152}` or, with `from`, the resend request. */
size_t buildBulkGot(char* out, size_t cap, int id, uint32_t got);
size_t buildBulkFrom(char* out, size_t cap, int id, uint32_t from);
/** `{"t":"<kind>","st":"<state>","id":7[,"err":"…"]}` for ota and map. */
size_t buildTransferState(char* out, size_t cap, const char* kind, const char* state, int id,
                          const char* err = nullptr);
/** Value of the `info` characteristic. */
size_t buildInfo(char* out, size_t cap, const char* fw, const char* id, bool paired);

}  // namespace monav::proto
