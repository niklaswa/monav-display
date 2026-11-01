#include "device/transfers.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <mbedtls/sha256.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "core/bulk.h"
#include "core/rxstats.h"
#include "core/protocol.h"
#include "device/ble_link.h"
#include "ui/mapdecode.h"

namespace monav::device::transfers {
namespace {

constexpr int kMapMaxSide = 1024;
constexpr size_t kMapJpgMax = 1024 * 1024;
constexpr size_t kRouteMax = 64 * 1024;
// The phone keeps at most 32 KiB beyond the last `got` in flight, and `got` comes every 16 KiB
// written: 40 KiB of ring always hold what can arrive.
constexpr size_t kRingSize = 40 * 1024;
constexpr size_t kWriteChunk = 4096;
constexpr uint32_t kFailedShowMs = 6000;
// From `done` to the restart: time for `done` to go out (one connection event) and the log to drain.
constexpr uint64_t kRestartDelayUs = 600 * 1000;

using Phase = Frame::Update::Phase;

portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
std::atomic<bool> s_authorized{false};
TaskHandle_t s_worker = nullptr;

// --- Outgoing control messages (sent by the worker; the NimBLE task only queues them) ---
struct Out {
  char json[112];
  uint16_t len;
};
QueueHandle_t s_out = nullptr;

void queueOut(const char* json, size_t len) {
  if (s_out == nullptr) return;
  Out o;
  if (len >= sizeof(o.json)) len = sizeof(o.json) - 1;
  memcpy(o.json, json, len);
  o.json[len] = 0;
  o.len = static_cast<uint16_t>(len);
  xQueueSend(s_out, &o, 0);
  if (s_worker) xTaskNotifyGive(s_worker);
}

// Acknowledgements and resend requests for map and route bytes go out straight from the BLE task
// that took the bytes: through the worker they waited for core 0's drawing, and the phone's 32 KiB
// window made every late `got` a pause in the transfer.
void sendGotNow(int id, uint32_t got) {
  char buf[64];
  const size_t n = proto::buildBulkGot(buf, sizeof(buf), id, got);
  ble::send(buf, n);
}

void sendFromNow(int id, uint32_t from) {
  char buf[64];
  const size_t n = proto::buildBulkFrom(buf, sizeof(buf), id, from);
  ble::send(buf, n);
}

void queueState(const char* kind, const char* state, int id, const char* err = nullptr) {
  char buf[112];
  queueOut(buf, proto::buildTransferState(buf, sizeof(buf), kind, state, id, err));
}

// --- Map ----------------------------------------------------------------------------------------

uint16_t* s_mapBuf[2] = {nullptr, nullptr};
int s_front = 0;          // UI task
MapImage s_frontImg;      // UI task
bool s_haveFront = false; // UI task

struct MapJob {
  uint8_t* jpg = nullptr;
  size_t len = 0;
  int w = 0, h = 0;
  double lat = 0, lon = 0;
  float mpp = 0;
  uint8_t id = 0;
  char attr[40] = {0};
};

// Receiving (NimBLE task).
bulk::Slot s_mapSlot;
MapJob s_mapRx;
// Received, waiting for the worker (under s_lock).
MapJob s_job;
bool s_jobPending = false;
// Decoded into the spare buffer, waiting for the UI task to swap it in.
std::atomic<bool> s_swapPending{false};
MapImage s_backImg;

// --- Route --------------------------------------------------------------------------------------

bulk::Slot s_routeSlot;
uint8_t* s_routeRx = nullptr;
double s_routeRxLat = 0, s_routeRxLon = 0;
// Received, waiting for the UI task (under s_lock).
int16_t* s_routeNew = nullptr;
RouteLine s_routeNewLine;
bool s_routeClear = false;
// Current (UI task).
int16_t* s_routeCur = nullptr;
RouteLine s_routeLine;
bool s_haveRoute = false;

// --- Firmware update ------------------------------------------------------------------------------

struct Cmd {
  enum Kind : uint8_t { OtaBegin, OtaEnd, OtaAbort, Disconnected } kind;
  uint8_t id;
  uint32_t size;
  char sha[65];
  char version[24];
};
QueueHandle_t s_cmds = nullptr;

bulk::Slot s_otaSlot;  // offered on the NimBLE task, acknowledged on the worker: under s_lock
uint8_t* s_ring = nullptr;  // PSRAM: the flash writes go through s_wbuf (internal) anyway
std::atomic<uint32_t> s_ringHead{0};  // bytes put in (NimBLE task)
std::atomic<uint32_t> s_ringTail{0};  // bytes taken out (worker)

// Worker only.
bool s_otaActive = false;
uint8_t s_otaId = 0;
uint32_t s_otaSize = 0;
uint32_t s_otaStored = 0;
bool s_otaEndWanted = false;
esp_ota_handle_t s_otaHandle = 0;
const esp_partition_t* s_otaPart = nullptr;
mbedtls_sha256_context s_sha;
char s_otaShaHex[65] = {0};
uint8_t s_wbuf[kWriteChunk];
size_t s_wfill = 0;

// The restart into a new firmware is an esp_timer one-shot: it fires on the esp_timer task whatever
// the worker or the link are doing. (It used to be the worker's job after a Serial.flush(), which
// never returns without a USB host reading the port: in the car the glass stayed on "Neustart …".)
esp_timer_handle_t s_restartTimer = nullptr;
std::atomic<bool> s_restartPending{false};

void restartNow(void*) { esp_restart(); }

void feedWatchdog() { esp_task_wdt_reset(); }

// The screen's view of it (under s_lock).
Frame::Update s_update;
uint64_t s_failedAt = 0;

void setUpdate(Phase phase, float progress, const char* version = nullptr, const char* error = nullptr) {
  portENTER_CRITICAL(&s_lock);
  s_update.phase = phase;
  s_update.progress = progress;
  if (version) snprintf(s_update.version, sizeof(s_update.version), "%s", version);
  if (error) snprintf(s_update.error, sizeof(s_update.error), "%s", error);
  if (phase == Phase::Failed) s_failedAt = millis();
  portEXIT_CRITICAL(&s_lock);
}

void closeOtaSlot() {
  portENTER_CRITICAL(&s_lock);
  s_otaSlot.close();
  portEXIT_CRITICAL(&s_lock);
}

void otaFail(const char* err, bool show = true) {
  Serial.printf("ota: failed - %s\n", err);
  if (s_otaActive) esp_ota_abort(s_otaHandle);
  mbedtls_sha256_free(&s_sha);
  s_otaActive = false;
  closeOtaSlot();
  queueState("ota", "error", s_otaId, err);
  if (show) {
    setUpdate(Phase::Failed, 0, nullptr, err);
  } else {
    setUpdate(Phase::None, 0);
  }
}

void otaBegin(const Cmd& c) {
  if (s_ring == nullptr) {
    queueState("ota", "error", c.id, "no memory");
    return;
  }
  if (s_otaActive) {
    // A new transfer of the same kind replaces the running one.
    esp_ota_abort(s_otaHandle);
    mbedtls_sha256_free(&s_sha);
    s_otaActive = false;
    setUpdate(Phase::None, 0);
  }
  s_otaId = c.id;
  s_otaPart = esp_ota_get_next_update_partition(nullptr);
  if (s_otaPart == nullptr) {
    queueState("ota", "error", c.id, "no update partition");
    return;
  }
  if (c.size == 0 || c.size > s_otaPart->size) {
    queueState("ota", "error", c.id, "size");
    return;
  }
  if (strlen(c.sha) != 64) {
    queueState("ota", "error", c.id, "sha256 missing");
    return;
  }
  // Sequential writes: the partition is erased sector by sector as the bytes come, so begin
  // answers at once and BLE never waits seconds for one big erase.
  const esp_err_t err = esp_ota_begin(s_otaPart, OTA_WITH_SEQUENTIAL_WRITES, &s_otaHandle);
  if (err != ESP_OK) {
    char e[64];
    snprintf(e, sizeof(e), "begin %s", esp_err_to_name(err));
    queueState("ota", "error", c.id, e);
    return;
  }
  mbedtls_sha256_init(&s_sha);
  mbedtls_sha256_starts_ret(&s_sha, 0);
  snprintf(s_otaShaHex, sizeof(s_otaShaHex), "%s", c.sha);
  s_otaSize = c.size;
  s_otaStored = 0;
  s_wfill = 0;
  s_otaEndWanted = false;
  s_ringHead.store(0);
  s_ringTail.store(0);
  portENTER_CRITICAL(&s_lock);
  s_otaSlot.begin(c.id, c.size);
  portEXIT_CRITICAL(&s_lock);
  s_otaActive = true;
  setUpdate(Phase::Receiving, 0, c.version, "");
  Serial.printf("ota: %u bytes of %s into %s\n", static_cast<unsigned>(c.size), c.version, s_otaPart->label);
  queueState("ota", "ready", c.id);
}

void otaFinish() {
  setUpdate(Phase::Verifying, 1.0f);
  uint8_t digest[32];
  mbedtls_sha256_finish_ret(&s_sha, digest);
  mbedtls_sha256_free(&s_sha);
  char hex[65];
  for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", digest[i]);
  if (strcasecmp(hex, s_otaShaHex) != 0) {
    Serial.printf("ota: sha256 %s, expected %s\n", hex, s_otaShaHex);
    esp_ota_abort(s_otaHandle);
    s_otaActive = false;
    closeOtaSlot();
    queueState("ota", "error", s_otaId, "sha256 mismatch");
    setUpdate(Phase::Failed, 0, nullptr, "SHA-256");
    return;
  }
  esp_err_t err = esp_ota_end(s_otaHandle);  // checks the image itself
  s_otaActive = false;
  closeOtaSlot();
  if (err != ESP_OK) {
    queueState("ota", "error", s_otaId, "invalid image");
    setUpdate(Phase::Failed, 0, nullptr, esp_err_to_name(err));
    return;
  }
  err = esp_ota_set_boot_partition(s_otaPart);
  if (err != ESP_OK) {
    queueState("ota", "error", s_otaId, "boot partition");
    setUpdate(Phase::Failed, 0, nullptr, esp_err_to_name(err));
    return;
  }
  setUpdate(Phase::Restarting, 1.0f);
  s_restartPending.store(true);
  // Armed before `done` goes out: nothing from here on can keep the old firmware running.
  if (s_restartTimer == nullptr || esp_timer_start_once(s_restartTimer, kRestartDelayUs) != ESP_OK) {
    Serial.println("ota: no restart timer - restarting at once");
    esp_restart();
  }
  Serial.printf("ota: done, booting %s in %u ms\n", s_otaPart->label, static_cast<unsigned>(kRestartDelayUs / 1000));
  char buf[64];
  ble::send(buf, proto::buildTransferState(buf, sizeof(buf), "ota", "done", s_otaId));
}

/** Moves firmware bytes from the ring into flash, 4 KiB at a time; acknowledges what is written. */
void otaPump() {
  if (!s_otaActive) return;
  for (;;) {
    const uint32_t head = s_ringHead.load(std::memory_order_acquire);
    uint32_t tail = s_ringTail.load(std::memory_order_relaxed);
    const uint32_t avail = head - tail;
    const bool last = s_otaStored + s_wfill == s_otaSize;
    if (avail == 0 && !(last && s_wfill > 0)) break;
    size_t n = kWriteChunk - s_wfill;
    if (n > avail) n = avail;
    const size_t at = tail % kRingSize;
    const size_t first = n < kRingSize - at ? n : kRingSize - at;
    memcpy(s_wbuf + s_wfill, s_ring + at, first);
    if (n > first) memcpy(s_wbuf + s_wfill + first, s_ring, n - first);
    s_ringTail.store(tail + n, std::memory_order_release);
    s_wfill += n;
    if (s_wfill == kWriteChunk || s_otaStored + s_wfill == s_otaSize) {
      const esp_err_t err = esp_ota_write(s_otaHandle, s_wbuf, s_wfill);
      if (err != ESP_OK) {
        otaFail("flash write");
        return;
      }
      mbedtls_sha256_update_ret(&s_sha, s_wbuf, s_wfill);
      s_otaStored += s_wfill;
      s_wfill = 0;
      uint32_t ack = 0;
      portENTER_CRITICAL(&s_lock);
      const bool due = s_otaSlot.stored(s_otaStored, ack);
      portEXIT_CRITICAL(&s_lock);
      if (due) {
        char buf[64];
        ble::send(buf, proto::buildBulkGot(buf, sizeof(buf), s_otaId, ack));
      }
      setUpdate(Phase::Receiving, static_cast<float>(s_otaStored) / static_cast<float>(s_otaSize));
    }
  }
  if (s_otaEndWanted && s_otaStored == s_otaSize) otaFinish();
}

void handleCmd(const Cmd& c) {
  if (s_restartPending.load()) return;  // the new firmware boots in a moment
  switch (c.kind) {
    case Cmd::OtaBegin:
      otaBegin(c);
      break;
    case Cmd::OtaEnd:
      if (!s_otaActive || c.id != s_otaId) {
        queueState("ota", "error", c.id, "no transfer");
        break;
      }
      s_otaEndWanted = true;
      otaPump();
      if (s_otaActive && s_otaStored != s_otaSize) {
        // The phone only ends after the last `got`: anything missing now is lost.
        char e[48];
        snprintf(e, sizeof(e), "incomplete %u/%u", static_cast<unsigned>(s_otaStored), static_cast<unsigned>(s_otaSize));
        otaFail(e);
      }
      break;
    case Cmd::OtaAbort:
      if (s_otaActive && c.id == s_otaId) otaFail("aborted", false);
      break;
    case Cmd::Disconnected:
      if (s_otaActive) otaFail("connection lost");
      break;
  }
}

// --- Map decoding (worker) ------------------------------------------------------------------------

void mapPump() {
  if (s_swapPending.load()) return;  // the UI has not taken the last one yet
  if (s_restartPending.load()) return;  // no point: the firmware restarts in a moment
  // While an update is written the glass shows its progress, not the map: a decode (1.5–3 s of
  // this task) would only hold up the flash writes and their acknowledgements. The image waits.
  if (s_otaActive) return;
  MapJob job;
  bool have = false;
  portENTER_CRITICAL(&s_lock);
  if (s_jobPending) {
    job = s_job;
    s_jobPending = false;
    have = true;
  }
  portEXIT_CRITICAL(&s_lock);
  if (!have) return;
  const int back = 1 - s_front;  // stable: only the UI task swaps, and only while a swap is pending
  const int64_t t0 = esp_timer_get_time();
  const bool ok = ui::decodeMap(job.jpg, job.len, s_mapBuf[back], job.w, job.h, feedWatchdog);
  const int64_t us = esp_timer_get_time() - t0;
  heap_caps_free(job.jpg);
  if (!ok) {
    Serial.printf("map: #%u did not decode\n", job.id);
    queueState("map", "error", job.id, "decode");
    return;
  }
  s_backImg = MapImage{s_mapBuf[back], job.w, job.h, job.lat, job.lon, job.mpp, job.id,
                       ui::mapEdgeColour(s_mapBuf[back], job.w, job.h)};
  memcpy(s_backImg.attr, job.attr, sizeof(s_backImg.attr));
  s_swapPending.store(true);
  Serial.printf("map: #%u decoded (%dx%d, %u kB JPEG) in %d ms\n", job.id, job.w, job.h,
                static_cast<unsigned>(job.len / 1024), static_cast<int>(us / 1000));
}

void workerTask(void*) {
  // Watched: a worker stuck for 5 s (flash, decoding) resets the display - on probation that is a
  // rollback. The decode feeds the watchdog as it goes.
  esp_task_wdt_add(nullptr);
  for (;;) {
    esp_task_wdt_reset();
    Cmd c;
    while (xQueueReceive(s_cmds, &c, 0) == pdTRUE) handleCmd(c);
    otaPump();
    Out o;
    while (xQueueReceive(s_out, &o, 0) == pdTRUE) ble::send(o.json, o.len);
    mapPump();
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
  }
}

void postCmd(const Cmd& c) {
  if (xQueueSend(s_cmds, &c, 0) != pdTRUE) Serial.println("transfers: command queue full");
  if (s_worker) xTaskNotifyGive(s_worker);
}

// --- NimBLE task: receiving ---------------------------------------------------------------------

void mapControl(const Message& m) {
  if (m.op != TransferOp::Begin) return;
  rxstats::mapBegin++;
  if (m.w <= 0 || m.h <= 0 || m.w > kMapMaxSide || m.h > kMapMaxSide || m.size == 0 || m.size > kMapJpgMax ||
      m.mpp <= 0 || !m.hasPos) {
    rxstats::mapRejected++;
    queueState("map", "error", m.id, "unsupported");
    return;
  }
  uint8_t* buf = static_cast<uint8_t*>(heap_caps_malloc(m.size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (buf == nullptr) {
    queueState("map", "error", m.id, "no memory");
    return;
  }
  uint8_t* old = s_mapRx.jpg;
  s_mapRx = MapJob{buf, m.size, m.w, m.h, m.lat, m.lon, m.mpp, static_cast<uint8_t>(m.id)};
  memcpy(s_mapRx.attr, m.attr, sizeof(s_mapRx.attr));
  s_mapSlot.begin(static_cast<uint8_t>(m.id), m.size);
  if (old) heap_caps_free(old);
}

void routeControl(const Message& m) {
  if (m.op == TransferOp::Clear) {
    portENTER_CRITICAL(&s_lock);
    s_routeClear = true;
    portEXIT_CRITICAL(&s_lock);
    s_routeSlot.close();
    return;
  }
  if (m.op != TransferOp::Begin) return;
  rxstats::routeBegin++;
  if (m.size == 0 || m.size > kRouteMax || (m.size % 4) != 0 || !m.hasPos) return;
  uint8_t* buf = static_cast<uint8_t*>(heap_caps_malloc(m.size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (buf == nullptr) return;
  uint8_t* old = s_routeRx;
  s_routeRx = buf;
  s_routeRxLat = m.lat;
  s_routeRxLon = m.lon;
  s_routeSlot.begin(static_cast<uint8_t>(m.id), m.size);
  if (old) heap_caps_free(old);
}

void otaControl(const Message& m) {
  Cmd c{};
  c.id = static_cast<uint8_t>(m.id);
  c.size = m.size;
  snprintf(c.sha, sizeof(c.sha), "%s", m.sha256);
  snprintf(c.version, sizeof(c.version), "%s", m.version);
  switch (m.op) {
    case TransferOp::Begin:
      c.kind = Cmd::OtaBegin;
      // No more bytes into the ring for the old transfer: this task is the ring's only producer,
      // so once the slot is closed here the worker can reset the ring and open the new one.
      closeOtaSlot();
      break;
    case TransferOp::End:
      c.kind = Cmd::OtaEnd;
      break;
    case TransferOp::Abort:
      c.kind = Cmd::OtaAbort;
      break;
    default:
      return;
  }
  postCmd(c);
}

/** Map / route bytes: straight into their PSRAM buffer. */
void takeInto(bulk::Slot& slot, uint8_t* dst, const bulk::Chunk& ch, bool isMap) {
  const uint32_t at = slot.next();
  const bulk::Slot::Verdict v = slot.offer(ch.offset, ch.len);
  if (v == bulk::Slot::Verdict::AskFrom) {
    sendFromNow(slot.id(), slot.next());
    return;
  }
  if (v != bulk::Slot::Verdict::Take) return;
  memcpy(dst + at, ch.data, ch.len);
  uint32_t ack = 0;
  if (slot.stored(slot.next(), ack)) sendGotNow(slot.id(), ack);
  if (!slot.received()) return;
  slot.close();
  if (isMap) {
    MapJob dropped;
    portENTER_CRITICAL(&s_lock);
    if (s_jobPending) dropped = s_job;  // a newer image replaces one not decoded yet
    s_job = s_mapRx;
    s_jobPending = true;
    portEXIT_CRITICAL(&s_lock);
    s_mapRx = MapJob{};
    if (dropped.jpg) heap_caps_free(dropped.jpg);
    if (s_worker) xTaskNotifyGive(s_worker);
  } else {
    int16_t* dropped = nullptr;
    portENTER_CRITICAL(&s_lock);
    dropped = s_routeNew;
    s_routeNew = reinterpret_cast<int16_t*>(s_routeRx);
    s_routeNewLine = RouteLine{s_routeNew, static_cast<int>(slot.size() / 4), s_routeRxLat, s_routeRxLon};
    s_routeClear = false;
    portEXIT_CRITICAL(&s_lock);
    s_routeRx = nullptr;
    if (dropped) heap_caps_free(dropped);
  }
}

}  // namespace

bool begin() {
  esp_timer_create_args_t restart{};
  restart.callback = restartNow;
  restart.name = "ota-restart";
  if (esp_timer_create(&restart, &s_restartTimer) != ESP_OK) s_restartTimer = nullptr;
  s_out = xQueueCreate(16, sizeof(Out));
  s_cmds = xQueueCreate(4, sizeof(Cmd));
  s_ring = static_cast<uint8_t*>(heap_caps_malloc(kRingSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  const size_t bytes = static_cast<size_t>(mapPixels(kMapMaxSide, kMapMaxSide)) * 2;
  for (auto& b : s_mapBuf) {
    b = static_cast<uint16_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (b == nullptr) {
      Serial.println("transfers: no PSRAM for the map buffers - no map");
    }
  }
  // Core 0 next to BLE, the push and the second drawing task (same priority: it takes turns
  // with the drawing instead of waiting for a gap that never comes in a busy layout).
  xTaskCreatePinnedToCore(workerTask, "bulk", 8192, nullptr, 2, &s_worker, 0);
  return s_mapBuf[0] != nullptr && s_mapBuf[1] != nullptr;
}

void setAuthorized(bool authorized) { s_authorized.store(authorized); }

void control(const Message& m) {
  if (!s_authorized.load()) return;  // only after welcome
  switch (m.type) {
    case MsgType::Ota:
      otaControl(m);
      break;
    case MsgType::Map:
      if (s_mapBuf[0] && s_mapBuf[1]) mapControl(m);
      break;
    case MsgType::Route:
      routeControl(m);
      break;
    default:
      break;
  }
}

void onBulk(const uint8_t* data, size_t len) {
  rxstats::bulkBytes += len;
  if (!s_authorized.load()) return;
  bulk::Chunk ch;
  if (!bulk::parseChunk(data, len, ch)) return;

  if (s_mapSlot.active() && ch.id == s_mapSlot.id()) {
    takeInto(s_mapSlot, s_mapRx.jpg, ch, true);
    return;
  }
  if (s_routeSlot.active() && ch.id == s_routeSlot.id()) {
    takeInto(s_routeSlot, s_routeRx, ch, false);
    return;
  }

  // Firmware: into the ring for the worker, if there is room (there always is when the phone
  // keeps to its window; otherwise drop - it resends after 3 s without an acknowledgement).
  portENTER_CRITICAL(&s_lock);
  const bool mine = s_otaSlot.active() && ch.id == s_otaSlot.id();
  portEXIT_CRITICAL(&s_lock);
  if (!mine || s_ring == nullptr) return;
  const uint32_t head = s_ringHead.load(std::memory_order_relaxed);
  const uint32_t tail = s_ringTail.load(std::memory_order_acquire);
  if (ch.len > kRingSize - (head - tail)) return;
  portENTER_CRITICAL(&s_lock);
  const uint32_t next = s_otaSlot.next();
  const bulk::Slot::Verdict v = s_otaSlot.offer(ch.offset, ch.len);
  portEXIT_CRITICAL(&s_lock);
  if (v == bulk::Slot::Verdict::AskFrom) {
    sendFromNow(ch.id, next);
    return;
  }
  if (v != bulk::Slot::Verdict::Take) return;
  const size_t at = head % kRingSize;
  const size_t first = ch.len < kRingSize - at ? ch.len : kRingSize - at;
  memcpy(s_ring + at, ch.data, first);
  if (ch.len > first) memcpy(s_ring, ch.data + first, ch.len - first);
  s_ringHead.store(head + static_cast<uint32_t>(ch.len), std::memory_order_release);
  if (s_worker) xTaskNotifyGive(s_worker);
}

void onDisconnect() {
  s_authorized.store(false);
  s_mapSlot.close();
  s_routeSlot.close();
  Cmd c{};
  c.kind = Cmd::Disconnected;
  postCmd(c);
}

void frameBegin(uint64_t nowMs) {
  if (s_swapPending.load()) {
    s_front = 1 - s_front;
    s_frontImg = s_backImg;
    s_haveFront = true;
    s_swapPending.store(false);
    char buf[64];
    queueOut(buf, proto::buildTransferState(buf, sizeof(buf), "map", "ready", s_frontImg.id));
    if (s_worker) xTaskNotifyGive(s_worker);  // the next image may be waiting for the spare buffer
  }

  int16_t* oldRoute = nullptr;
  portENTER_CRITICAL(&s_lock);
  if (s_routeClear) {
    oldRoute = s_routeCur;
    s_routeCur = nullptr;
    s_haveRoute = false;
    s_routeClear = false;
  } else if (s_routeNew != nullptr) {
    oldRoute = s_routeCur;
    s_routeCur = s_routeNew;
    s_routeLine = s_routeNewLine;
    s_routeNew = nullptr;
    s_haveRoute = true;
  }
  if (s_update.phase == Phase::Failed && millis() - s_failedAt > kFailedShowMs) s_update.phase = Phase::None;
  portEXIT_CRITICAL(&s_lock);
  if (oldRoute) heap_caps_free(oldRoute);
  (void)nowMs;
}

const MapImage* map() { return s_haveFront ? &s_frontImg : nullptr; }

const RouteLine* route() { return s_haveRoute ? &s_routeLine : nullptr; }

Frame::Update update() {
  portENTER_CRITICAL(&s_lock);
  Frame::Update u = s_update;
  portEXIT_CRITICAL(&s_lock);
  return u;
}

bool loadLocalMap(const uint8_t* jpg, size_t len, int w, int h, double lat, double lon, float mpp) {
  if (!s_mapBuf[0] || !s_mapBuf[1] || w > kMapMaxSide || h > kMapMaxSide) return false;
  uint8_t* copy = static_cast<uint8_t*>(heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (copy == nullptr) return false;
  memcpy(copy, jpg, len);
  MapJob dropped;
  portENTER_CRITICAL(&s_lock);
  if (s_jobPending) dropped = s_job;
  s_job = MapJob{copy, len, w, h, lat, lon, mpp, 0};
  s_jobPending = true;
  portEXIT_CRITICAL(&s_lock);
  if (dropped.jpg) heap_caps_free(dropped.jpg);
  if (s_worker) xTaskNotifyGive(s_worker);
  return true;
}

}  // namespace monav::device::transfers
