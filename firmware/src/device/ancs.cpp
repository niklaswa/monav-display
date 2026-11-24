#include "device/ancs.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <atomic>
#include <cstring>

#include "nimble/nimble/host/include/host/ble_hs.h"
#include "nimble/porting/nimble/include/nimble/nimble_port.h"

namespace monav::device::ancs {
namespace {

namespace wire = monav::ancs;

constexpr uint16_t kNoConn = 0xFFFF;  // BLE_HS_CONN_HANDLE_NONE
constexpr uint32_t kAnswerMs = 2500;  // a Data Source answer that takes longer is given up
constexpr uint32_t kRetryMs = 6000;   // the iPhone still asks whether to share: try again then
constexpr int kRetries = 8;
constexpr int kPendingMax = 8;
constexpr int kNames = 16;
constexpr int kActionsMax = 4;

// --- Shared with the UI task ---
QueueHandle_t s_events = nullptr;
std::atomic<bool> s_wanted{false};
portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
struct Action {
  uint32_t uid;
  bool positive;
};
Action s_actions[kActionsMax];  // under s_lock
int s_actionCount = 0;
ble_npl_event s_kickEvent;
bool s_ready = false;

// --- NimBLE host task only ---
NimBLEUUID* s_svcUuid = nullptr;
NimBLEUUID* s_nsUuid = nullptr;
NimBLEUUID* s_cpUuid = nullptr;
NimBLEUUID* s_dsUuid = nullptr;
ble_gap_event_listener s_listener;
ble_npl_callout s_timeout;
ble_npl_callout s_retry;

enum class Phase : uint8_t { Idle, Securing, Discovering, Subscribing, On, Failed };
uint16_t s_conn = kNoConn;
bool s_enabled = false;
Phase s_phase = Phase::Idle;
int s_retries = 0;
uint16_t s_svcStart = 0, s_svcEnd = 0;
uint16_t s_nsVal = 0, s_cpVal = 0, s_dsVal = 0;
uint16_t s_nsCccd = 0, s_dsCccd = 0;

struct Pending {
  uint32_t uid;
  wire::Category category;
  uint8_t flags;
  bool modified;
};
Pending s_pending[kPendingMax];
int s_pendingCount = 0;
bool s_busy = false;
Pending s_current;
wire::Response s_resp;
wire::Notice s_waiting;  // fetched, waiting for its app's name
bool s_waitingName = false;

struct Name {
  char app[64];
  char name[32];
};
Name s_names[kNames];
int s_nameCount = 0, s_nameNext = 0;

uint8_t s_rx[520];  // one notification, up to the largest MTU

void pushState(const char* state) {
  Event e{};
  e.kind = Event::State;
  snprintf(e.state, sizeof(e.state), "%s", state);
  if (s_events) xQueueSend(s_events, &e, 0);
  Serial.printf("ancs: %s\n", state);
}

void emit(const wire::Notice& n, bool modified) {
  Event e{};
  e.kind = modified ? Event::Modified : Event::Added;
  e.notice = n;
  if (s_events && xQueueSend(s_events, &e, 0) != pdTRUE) Serial.println("ancs: event queue full - dropped");
}

const char* cachedName(const char* app) {
  for (int i = 0; i < s_nameCount; i++) {
    if (strcmp(s_names[i].app, app) == 0) return s_names[i].name;
  }
  return nullptr;
}

void cacheName(const char* app, const char* name) {
  Name& slot = s_names[s_nameNext];
  snprintf(slot.app, sizeof(slot.app), "%s", app);
  snprintf(slot.name, sizeof(slot.name), "%s", name);
  s_nameNext = (s_nameNext + 1) % kNames;
  if (s_nameCount < kNames) s_nameCount++;
}

void armTimeout(uint32_t ms) { ble_npl_callout_reset(&s_timeout, ble_npl_time_ms_to_ticks32(ms)); }

void resetConnection() {
  ble_npl_callout_stop(&s_timeout);
  ble_npl_callout_stop(&s_retry);
  s_phase = Phase::Idle;
  s_retries = 0;
  s_svcStart = s_svcEnd = 0;
  s_nsVal = s_cpVal = s_dsVal = 0;
  s_nsCccd = s_dsCccd = 0;
  s_pendingCount = 0;
  s_busy = false;
  s_waitingName = false;
  s_resp.reset();
}

void fail(const char* state, bool retry) {
  s_phase = Phase::Failed;
  pushState(state);
  if (retry && s_retries < kRetries) {
    s_retries++;
    ble_npl_callout_reset(&s_retry, ble_npl_time_ms_to_ticks32(kRetryMs));
  }
}

// --- Fetching one notification at a time ------------------------------------------------------

void kick();

void finish() {
  ble_npl_callout_stop(&s_timeout);
  s_busy = false;
  s_waitingName = false;
  s_resp.reset();
  kick();
}

int onControlWritten(uint16_t, const ble_gatt_error* error, ble_gatt_attr*, void*) {
  // 0xA2 and friends: the notification is gone already. Next one.
  if (error->status != 0 && s_busy) {
    Serial.printf("ancs: control point answered %d\n", error->status);
    finish();
  }
  return 0;
}

bool writeControl(const uint8_t* cmd, size_t len) {
  return ble_gattc_write_flat(s_conn, s_cpVal, cmd, static_cast<uint16_t>(len), onControlWritten, nullptr) == 0;
}

void kick() {
  if (s_busy || s_phase != Phase::On || s_pendingCount == 0) return;
  s_current = s_pending[0];
  for (int i = 1; i < s_pendingCount; i++) s_pending[i - 1] = s_pending[i];
  s_pendingCount--;
  uint8_t cmd[32];
  const size_t n = wire::buildGetNotification(cmd, sizeof(cmd), s_current.uid);
  s_resp.beginNotification(s_current.uid);
  s_busy = true;
  armTimeout(kAnswerMs);
  if (!writeControl(cmd, n)) finish();
}

void onSource(const uint8_t* data, size_t len) {
  wire::SourceEvent ev;
  if (!wire::parseSource(data, len, ev)) return;
  if (ev.event == wire::EventId::Removed) {
    int k = 0;
    for (int i = 0; i < s_pendingCount; i++) {
      if (s_pending[i].uid != ev.uid) s_pending[k++] = s_pending[i];
    }
    s_pendingCount = k;
    Event e{};
    e.kind = Event::Removed;
    e.uid = ev.uid;
    if (s_events) xQueueSend(s_events, &e, 0);
    return;
  }
  // What was there before the display subscribed stays on the iPhone.
  if ((ev.flags & wire::flag::kPreExisting) != 0) return;
  if (s_pendingCount == kPendingMax) {
    for (int i = 1; i < kPendingMax; i++) s_pending[i - 1] = s_pending[i];
    s_pendingCount--;
  }
  s_pending[s_pendingCount++] = Pending{ev.uid, ev.category, ev.flags, ev.event == wire::EventId::Modified};
  kick();
}

void onData(const uint8_t* data, size_t len) {
  if (!s_busy || !s_resp.active()) return;
  const wire::Response::Result r = s_resp.feed(data, len);
  if (r == wire::Response::Result::More) return;
  if (r == wire::Response::Result::Error) {
    if (s_waitingName) emit(s_waiting, s_current.modified);
    finish();
    return;
  }
  if (s_resp.forApp()) {
    cacheName(s_resp.app(), s_resp.appName());
    snprintf(s_waiting.appName, sizeof(s_waiting.appName), "%s", s_resp.appName());
    emit(s_waiting, s_current.modified);
    finish();
    return;
  }
  wire::Notice n = s_resp.notice();
  n.category = s_current.category;
  n.flags = s_current.flags;
  if (const char* name = cachedName(n.app)) {
    snprintf(n.appName, sizeof(n.appName), "%s", name);
    emit(n, s_current.modified);
    finish();
    return;
  }
  // The app's display name once, then from the cache.
  s_waiting = n;
  s_waitingName = true;
  uint8_t cmd[72];
  const size_t len2 = wire::buildGetApp(cmd, sizeof(cmd), n.app);
  if (len2 == 0) {
    emit(n, s_current.modified);
    finish();
    return;
  }
  s_resp.beginApp(n.app);
  armTimeout(kAnswerMs);
  if (!writeControl(cmd, len2)) {
    emit(n, s_current.modified);
    finish();
  }
}

void onTimeout(ble_npl_event*) {
  if (!s_busy) return;
  Serial.println("ancs: no answer from the iPhone - next");
  if (s_waitingName) emit(s_waiting, s_current.modified);
  finish();
}

// --- Finding the service and subscribing ------------------------------------------------------

int onNsSubscribed(uint16_t, const ble_gatt_error* error, ble_gatt_attr*, void*) {
  if (error->status != 0) {
    fail("blocked", true);
    return 0;
  }
  s_phase = Phase::On;
  s_retries = 0;
  pushState("on");
  kick();
  return 0;
}

int onDsSubscribed(uint16_t conn, const ble_gatt_error* error, ble_gatt_attr*, void*) {
  // Writing the CCCD is what makes iOS ask "Share system notifications?"; until that is allowed
  // it answers with an error. Then: try again in a few seconds.
  if (error->status != 0) {
    Serial.printf("ancs: subscribing refused (%d)\n", error->status);
    fail("blocked", true);
    return 0;
  }
  static const uint8_t on[2] = {1, 0};
  if (ble_gattc_write_flat(conn, s_nsCccd, on, 2, onNsSubscribed, nullptr) != 0) fail("blocked", true);
  return 0;
}

void subscribe() {
  s_phase = Phase::Subscribing;
  static const uint8_t on[2] = {1, 0};
  // The Data Source first, so no answer can arrive unheard.
  if (ble_gattc_write_flat(s_conn, s_dsCccd, on, 2, onDsSubscribed, nullptr) != 0) fail("blocked", true);
}

int onDescriptor(uint16_t, const ble_gatt_error* error, uint16_t, const ble_gatt_dsc* dsc, void*) {
  if (error->status == 0 && dsc != nullptr) {
    if (ble_uuid_u16(&dsc->uuid.u) == BLE_GATT_DSC_CLT_CFG_UUID16) {
      // The CCCD belongs to the characteristic just before it.
      uint16_t owner = 0;
      for (uint16_t v : {s_nsVal, s_cpVal, s_dsVal}) {
        if (v < dsc->handle && v > owner) owner = v;
      }
      if (owner == s_nsVal) s_nsCccd = dsc->handle;
      if (owner == s_dsVal) s_dsCccd = dsc->handle;
    }
    return 0;
  }
  if (error->status == BLE_HS_EDONE && s_nsCccd != 0 && s_dsCccd != 0) {
    subscribe();
  } else if (error->status != 0) {
    fail("blocked", true);
  }
  return 0;
}

int onCharacteristic(uint16_t conn, const ble_gatt_error* error, const ble_gatt_chr* chr, void*) {
  if (error->status == 0 && chr != nullptr) {
    if (ble_uuid_cmp(&chr->uuid.u, &s_nsUuid->getNative()->u) == 0) s_nsVal = chr->val_handle;
    if (ble_uuid_cmp(&chr->uuid.u, &s_cpUuid->getNative()->u) == 0) s_cpVal = chr->val_handle;
    if (ble_uuid_cmp(&chr->uuid.u, &s_dsUuid->getNative()->u) == 0) s_dsVal = chr->val_handle;
    return 0;
  }
  if (error->status == BLE_HS_EDONE && s_nsVal != 0 && s_cpVal != 0 && s_dsVal != 0) {
    uint16_t first = s_nsVal;
    if (s_dsVal < first) first = s_dsVal;
    if (s_cpVal < first) first = s_cpVal;
    if (ble_gattc_disc_all_dscs(conn, first, s_svcEnd, onDescriptor, nullptr) != 0) fail("blocked", true);
  } else if (error->status != 0) {
    fail("blocked", true);
  }
  return 0;
}

int onService(uint16_t conn, const ble_gatt_error* error, const ble_gatt_svc* svc, void*) {
  if (error->status == 0 && svc != nullptr) {
    s_svcStart = svc->start_handle;
    s_svcEnd = svc->end_handle;
    return 0;
  }
  if (error->status == BLE_HS_EDONE && s_svcStart != 0) {
    if (ble_gattc_disc_all_chrs(conn, s_svcStart, s_svcEnd, onCharacteristic, nullptr) != 0) fail("blocked", true);
  } else {
    // No ANCS: the iPhone does not share its notifications with the display (yet).
    fail("blocked", true);
  }
  return 0;
}

void discover() {
  if (s_conn == kNoConn) return;
  s_phase = Phase::Discovering;
  s_svcStart = s_svcEnd = 0;
  s_nsVal = s_cpVal = s_dsVal = s_nsCccd = s_dsCccd = 0;
  if (ble_gattc_disc_svc_by_uuid(s_conn, &s_svcUuid->getNative()->u, onService, nullptr) != 0) fail("blocked", true);
}

/** Encrypted already: on to the service. Not yet: ask the iPhone to (re-)encrypt or to pair. */
void start() {
  if (s_conn == kNoConn) return;
  ble_gap_conn_desc desc;
  if (ble_gap_conn_find(s_conn, &desc) != 0) return;
  if (desc.sec_state.encrypted) {
    discover();
    return;
  }
  if (!NimBLEDevice::isBonded(NimBLEAddress(desc.peer_id_addr))) pushState("pairing");
  s_phase = Phase::Securing;
  const int rc = ble_gap_security_initiate(s_conn);
  if (rc != 0 && rc != BLE_HS_EALREADY) {
    Serial.printf("ancs: security request failed (%d)\n", rc);
    fail("denied", false);
  }
}

void onRetry(ble_npl_event*) {
  if (s_enabled && s_conn != kNoConn && s_phase == Phase::Failed) start();
}

int onGap(ble_gap_event* event, void*) {
  switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
      if (event->connect.status == 0) {
        s_conn = event->connect.conn_handle;
        resetConnection();
      }
      break;
    case BLE_GAP_EVENT_DISCONNECT:
      if (event->disconnect.conn.conn_handle == s_conn) {
        s_conn = kNoConn;
        resetConnection();
      }
      break;
    case BLE_GAP_EVENT_ENC_CHANGE:
      if (event->enc_change.conn_handle != s_conn || !s_enabled) break;
      if (event->enc_change.status == 0) {
        if (s_phase == Phase::Securing || s_phase == Phase::Failed || s_phase == Phase::Idle) discover();
      } else if (event->enc_change.status == BLE_HS_HCI_ERR(BLE_ERR_PINKEY_MISSING)) {
        // The iPhone still has keys this display has forgotten (reflashed, or reset).
        fail("stale", false);
      } else {
        Serial.printf("ancs: encryption failed (%d)\n", event->enc_change.status);
        fail("denied", false);
      }
      break;
    case BLE_GAP_EVENT_NOTIFY_RX: {
      if (event->notify_rx.conn_handle != s_conn || s_phase != Phase::On) break;
      const uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
      const uint16_t n = len < sizeof(s_rx) ? len : sizeof(s_rx);
      if (os_mbuf_copydata(event->notify_rx.om, 0, n, s_rx) != 0) break;
      if (event->notify_rx.attr_handle == s_nsVal) {
        onSource(s_rx, n);
      } else if (event->notify_rx.attr_handle == s_dsVal) {
        onData(s_rx, n);
      }
      break;
    }
    default:
      break;
  }
  return 0;
}

/** On the host task: what the UI task asked for. */
void onKick(ble_npl_event*) {
  const bool wanted = s_wanted.load();
  if (wanted && !s_enabled) {
    s_enabled = true;
    s_retries = 0;
    start();
  } else if (!wanted && s_enabled) {
    s_enabled = false;
    if (s_conn != kNoConn && s_phase == Phase::On) {
      static const uint8_t off[2] = {0, 0};
      ble_gattc_write_flat(s_conn, s_nsCccd, off, 2, nullptr, nullptr);
      ble_gattc_write_flat(s_conn, s_dsCccd, off, 2, nullptr, nullptr);
    }
    resetConnection();
    pushState("off");
  }
  Action todo[kActionsMax];
  int count;
  portENTER_CRITICAL(&s_lock);
  count = s_actionCount;
  memcpy(todo, s_actions, sizeof(Action) * count);
  s_actionCount = 0;
  portEXIT_CRITICAL(&s_lock);
  for (int i = 0; i < count; i++) {
    if (s_phase != Phase::On) break;
    uint8_t cmd[8];
    const size_t n = wire::buildAction(cmd, sizeof(cmd), todo[i].uid, todo[i].positive);
    ble_gattc_write_flat(s_conn, s_cpVal, cmd, static_cast<uint16_t>(n), nullptr, nullptr);
  }
}

void post() {
  if (s_ready) ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_kickEvent);
}

}  // namespace

void begin() {
  s_events = xQueueCreate(6, sizeof(Event));
  s_svcUuid = new NimBLEUUID(wire::kService);
  s_nsUuid = new NimBLEUUID(wire::kNotificationSource);
  s_cpUuid = new NimBLEUUID(wire::kControlPoint);
  s_dsUuid = new NimBLEUUID(wire::kDataSource);
  ble_npl_event_init(&s_kickEvent, onKick, nullptr);
  ble_npl_callout_init(&s_timeout, nimble_port_get_dflt_eventq(), onTimeout, nullptr);
  ble_npl_callout_init(&s_retry, nimble_port_get_dflt_eventq(), onRetry, nullptr);
  const int rc = ble_gap_event_listener_register(&s_listener, onGap, nullptr);
  s_ready = rc == 0;
  if (!s_ready) Serial.printf("ancs: no GAP listener (%d) - no iPhone notifications\n", rc);
}

void setEnabled(bool on) {
  if (s_wanted.exchange(on) != on) post();
}

bool poll(Event& out) { return s_events != nullptr && xQueueReceive(s_events, &out, 0) == pdTRUE; }

void action(uint32_t uid, bool positive) {
  portENTER_CRITICAL(&s_lock);
  if (s_actionCount < kActionsMax) s_actions[s_actionCount++] = Action{uid, positive};
  portEXIT_CRITICAL(&s_lock);
  post();
}

void forgetBonds() {
  NimBLEDevice::deleteAllBonds();
  Serial.println("ancs: bonds forgotten");
}

}  // namespace monav::device::ancs
