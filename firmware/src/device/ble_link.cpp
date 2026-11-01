#include "device/ble_link.h"

#include <Arduino.h>
#include <esp_system.h>
#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include <string>

#include "core/protocol.h"
#include "device/transfers.h"

namespace monav::device::ble {
namespace {

// PROTOCOL.md, "GATT".
#if defined(MONAV_DEV_TOKEN)
// Development builds advertise a service one digit off (and "Monav Dev" as the name): Monav on
// the iPhone neither finds nor reconnects to them, so the test client on the Mac gets the display.
constexpr const char* kServiceUuid = "FCAA986B-5D31-472A-86AC-5426528CF28E";
constexpr const char* kNamePrefix = "Monav Dev";
#else
constexpr const char* kServiceUuid = "FCAA986B-5D31-472A-86AC-5426528CF28D";
constexpr const char* kNamePrefix = "Monav Display";
#endif
constexpr const char* kInboxUuid = "1359E742-830F-4294-BA43-4FE4F7A9E4A9";
constexpr const char* kOutboxUuid = "260A8427-49CC-4B70-B611-AF0A5215D625";
constexpr const char* kInfoUuid = "6DE16D5B-8847-40EF-AE08-31A8A73E3C4A";
constexpr const char* kBulkUuid = "F1B9AD33-D140-459E-82B0-F9BF8E8C4466";

constexpr uint16_t kNoConn = 0xFFFF;  // BLE_HS_CONN_HANDLE_NONE
constexpr int kPendingMax = 4;

NimBLEServer* s_server = nullptr;
NimBLECharacteristic* s_outbox = nullptr;
NimBLECharacteristic* s_info = nullptr;
QueueHandle_t s_events = nullptr;
SemaphoreHandle_t s_txLock = nullptr;

// Touched only on the NimBLE host task.
proto::Reassembler s_rx;
Event s_rxEvent;

volatile uint16_t s_conn = kNoConn;
volatile uint16_t s_mtu = 23;
volatile bool s_subscribed = false;
volatile uint32_t s_dropped = 0;
volatile bool s_advertising = false;

// Under s_txLock.
uint8_t s_txSeq = 0;
std::string s_pending[kPendingMax];
int s_pendingCount = 0;

void pushEvent(const Event& e) {
  if (xQueueSend(s_events, &e, 0) != pdTRUE) s_dropped = s_dropped + 1;
}

/** Notifies one message, chunked to the negotiated MTU. Call with s_txLock held. */
void notifyLocked(const char* json, size_t len) {
  size_t chunk = s_mtu > 3 ? s_mtu - 3 : proto::kDefaultChunk;
  if (chunk > 512) chunk = 512;
  proto::fragment(json, len, chunk, s_txSeq, [](const uint8_t* data, size_t n) { s_outbox->notify(data, n, true); });
}

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server, ble_gap_conn_desc* desc) override {
    if (s_conn != kNoConn && s_conn != desc->conn_handle) {
      // One phone at a time.
      server->disconnect(desc->conn_handle);
      return;
    }
    s_conn = desc->conn_handle;
    s_mtu = 23;
    s_subscribed = false;
    s_rx.reset();
    xSemaphoreTake(s_txLock, portMAX_DELAY);
    s_txSeq = 0;
    s_pendingCount = 0;
    xSemaphoreGive(s_txLock);
    Event e{};
    e.kind = Event::Connected;
    pushEvent(e);
    // Faster connection events and long link-layer packets for the binary transfers (Apple's
    // limits: interval ≥ 15 ms, max ≥ min + 15 ms, supervision 2–6 s).
    server->updateConnParams(desc->conn_handle, 12, 24, 0, 400);
    server->setDataLen(desc->conn_handle, 251);
    Serial.printf("ble: [%lu] connected (%s, interval %.2f ms)\n", static_cast<unsigned long>(millis()),
                  NimBLEAddress(desc->peer_ota_addr).toString().c_str(), desc->conn_itvl * 1.25f);
  }

  void onDisconnect(NimBLEServer*, ble_gap_conn_desc* desc) override {
    if (desc->conn_handle != s_conn) return;
    s_conn = kNoConn;
    s_subscribed = false;
    s_rx.reset();
    transfers::onDisconnect();
    Event e{};
    e.kind = Event::Disconnected;
    pushEvent(e);
    Serial.printf("ble: [%lu] disconnected - advertising again\n", static_cast<unsigned long>(millis()));
  }

  void onMTUChange(uint16_t mtu, ble_gap_conn_desc* desc) override {
    s_mtu = mtu;
    Serial.printf("ble: [%lu] MTU %u (chunks of %u bytes), interval %.2f ms\n", static_cast<unsigned long>(millis()), mtu,
                  mtu - 3, desc->conn_itvl * 1.25f);
  }
};

class InboxCallbacks : public NimBLECharacteristicCallbacks {
  // Runs on the NimBLE host task: reassemble, parse, queue - nothing slow.
  void onWrite(NimBLECharacteristic* c, ble_gap_conn_desc*) override {
    NimBLEAttValue value = c->getValue();
    if (!s_rx.feed(value.data(), value.length())) return;
    s_rxEvent = Event{};
    s_rxEvent.kind = Event::Received;
    if (proto::parse(s_rx.message(), s_rx.length(), s_rxEvent.msg)) {
      const MsgType t = s_rxEvent.msg.type;
      if (t == MsgType::Ota || t == MsgType::Map || t == MsgType::Route) {
        // Transfers are set up right here, so bytes that follow the announcement at once find it.
        transfers::control(s_rxEvent.msg);
      } else {
        pushEvent(s_rxEvent);
      }
    } else {
      Serial.println("ble: message is not JSON - ignored");
    }
  }
};

class BulkCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, ble_gap_conn_desc*) override {
    NimBLEAttValue value = c->getValue();
    transfers::onBulk(value.data(), value.length());
  }
};

class OutboxCallbacks : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic*, ble_gap_conn_desc*, uint16_t subValue) override {
    s_subscribed = subValue != 0;
    if (!s_subscribed) return;
    // A reply written before the phone subscribed (welcome / denied) goes out now.
    xSemaphoreTake(s_txLock, portMAX_DELAY);
    for (int i = 0; i < s_pendingCount; i++) notifyLocked(s_pending[i].data(), s_pending[i].size());
    s_pendingCount = 0;
    xSemaphoreGive(s_txLock);
  }
};

}  // namespace

void begin(const char* id) {
  s_events = xQueueCreate(12, sizeof(Event));
  s_txLock = xSemaphoreCreateMutex();

  char name[32];
  snprintf(name, sizeof(name), "%s %s", kNamePrefix, id);
  NimBLEDevice::init(name);
#if defined(MONAV_DEV_RANDOM_ADDR)
  // Development builds: a random static address (derived from the real one), so the paired
  // iPhone - which reconnects by address - leaves the display to the test client.
  {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    uint8_t rnd[6] = {static_cast<uint8_t>(mac[5] ^ 0x5A), mac[4], mac[3], mac[2], mac[1],
                      static_cast<uint8_t>(mac[0] | 0xC0)};  // little endian; top bits 11 = static
    const int rc = ble_hs_id_set_rnd(rnd);
    if (rc == 0) NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RANDOM);
    Serial.printf("ble: dev build, random static address %02x:%02x:%02x:%02x:%02x:%02x (rc %d)\n", rnd[5], rnd[4],
                  rnd[3], rnd[2], rnd[1], rnd[0], rc);
  }
#endif
  NimBLEDevice::setMTU(517);
  // No bonding: a reflashed display must never leave a stale bond on the phone.
  NimBLEDevice::setSecurityAuth(false, false, false);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  s_server = NimBLEDevice::createServer();
  s_server->setCallbacks(new ServerCallbacks());
  s_server->advertiseOnDisconnect(true);

  NimBLEService* service = s_server->createService(kServiceUuid);
  NimBLECharacteristic* inbox =
      service->createCharacteristic(kInboxUuid, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR, 512);
  inbox->setCallbacks(new InboxCallbacks());
  s_outbox = service->createCharacteristic(kOutboxUuid, NIMBLE_PROPERTY::NOTIFY);
  s_outbox->setCallbacks(new OutboxCallbacks());
  s_info = service->createCharacteristic(kInfoUuid, NIMBLE_PROPERTY::READ);
  NimBLECharacteristic* bulk = service->createCharacteristic(kBulkUuid, NIMBLE_PROPERTY::WRITE_NR, 512);
  bulk->setCallbacks(new BulkCallbacks());
  service->start();

  // Advertising: flags + the complete list of 128-bit service UUIDs; the name in the scan response.
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  NimBLEAdvertisementData data;
  data.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  data.setCompleteServices(NimBLEUUID(kServiceUuid));
  adv->setAdvertisementData(data);
  NimBLEAdvertisementData scan;
  scan.setName(name);
  adv->setScanResponseData(scan);
  // Fast (20–40 ms): the phone should find the display the moment the car powers it.
  adv->setMinInterval(32);
  adv->setMaxInterval(64);
  s_advertising = adv->start();
  Serial.printf("ble: advertising as \"%s\" (%s)\n", name, NimBLEDevice::getAddress().toString().c_str());
}

bool poll(Event& out) { return s_events != nullptr && xQueueReceive(s_events, &out, 0) == pdTRUE; }

void send(const char* json, size_t len) {
  if (s_conn == kNoConn || s_outbox == nullptr) return;
  xSemaphoreTake(s_txLock, portMAX_DELAY);
  if (s_subscribed || s_outbox->getSubscribedCount() > 0) {
    notifyLocked(json, len);
  } else if (s_pendingCount < kPendingMax) {
    s_pending[s_pendingCount++].assign(json, len);
  }
  xSemaphoreGive(s_txLock);
}

void disconnect() {
  const uint16_t conn = s_conn;
  if (conn != kNoConn && s_server != nullptr) s_server->disconnect(conn);
}

void setInfo(const char* json) {
  if (s_info != nullptr) s_info->setValue(std::string(json));
}

uint16_t mtu() { return s_mtu; }

int connIntervalUs() {
  const uint16_t conn = s_conn;
  if (conn == kNoConn) return 0;
  ble_gap_conn_desc desc;
  if (ble_gap_conn_find(conn, &desc) != 0) return 0;
  return desc.conn_itvl * 1250;
}
bool advertising() { return s_advertising; }
uint32_t droppedMessages() { return s_dropped; }

}  // namespace monav::device::ble
