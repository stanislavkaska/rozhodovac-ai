// Vychází z anthropics/claude-desktop-buddy (Copyright 2026 Anthropic, PBC),
// licence MIT, viz LICENSE a LICENSES/MIT-claude-desktop-buddy.txt.
#include "ble_bridge.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLESecurity.h>
#if defined(CONFIG_NIMBLE_ENABLED)
#include <host/ble_hs.h>
#include <host/ble_store.h>
#else
#include <BLE2902.h>
#endif
#include <Arduino.h>
#include <string.h>
#include <atomic>

#define NUS_SERVICE_UUID "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_RX_UUID      "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_TX_UUID      "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

// Kruhový buffer příjmu: zapisuje jen úloha BLE (rxHead), čte jen hlavní smyčka (rxTail)
static const size_t RX_CAP = 8192;
static uint8_t  rxBuf[RX_CAP];
static std::atomic<size_t> rxHead{0};
static std::atomic<size_t> rxTail{0};
// Po zahozeném paketu nebo po odpojení se před další data vloží konec řádku, aby se
// rozdělaná zpráva neslepila s následující (rozbitý řádek pak zahodí parser JSON)
static std::atomic<bool> rxPendingBreak{false};
// Odpojení: buffer vyprázdní čtenář (smyčka), aby se indexy nepřepisovaly ze dvou úloh
static std::atomic<bool> rxClearReq{false};

static BLEServer*         server = nullptr;
static BLECharacteristic* txChar = nullptr;
static BLECharacteristic* rxChar = nullptr;
static volatile bool      connected = false;
static volatile bool      secure = false;
static volatile uint32_t  passkey = 0;
static volatile uint16_t  mtu = 23;
static volatile uint8_t   authResult = 0;   // 1 = párování OK, 2 = selhalo

static size_t rxFree() {
  size_t h = rxHead.load(std::memory_order_relaxed);
  size_t t = rxTail.load(std::memory_order_acquire);
  return (t + RX_CAP - h - 1) % RX_CAP;
}

static void rxPutByte(uint8_t c) {
  size_t h = rxHead.load(std::memory_order_relaxed);
  rxBuf[h] = c;
  rxHead.store((h + 1) % RX_CAP, std::memory_order_release);
}

static void rxPush(const uint8_t* p, size_t n) {
  if (rxPendingBreak.load()) {
    if (rxFree() < 1) return;
    rxPutByte('\n');
    rxPendingBreak.store(false);
  }
  // Paket se vejde celý, nebo se zahodí celý (useknutý by rozbil i další zprávu)
  if (n > rxFree()) {
    rxPendingBreak.store(true);
    return;
  }
  for (size_t i = 0; i < n; i++) rxPutByte(p[i]);
}

class RxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    size_t len = c->getLength();
    const uint8_t* ptr = c->getData();
    if (ptr && len > 0) {
      rxPush(ptr, len);
    }
  }
};

class ServerCallbacks : public BLEServerCallbacks {
  // Z callbacků BLE se na Serial nepíše: běží v úloze BLE a zápis na USB
  // bez čtoucího hostitele by zdržel celý zásobník
  void onConnect(BLEServer*) override {
    connected = true;
  }
  void onDisconnect(BLEServer*) override {
    connected = false;
    secure = false;
    passkey = 0;
    mtu = 23;
    rxClearReq.store(true);
    rxPendingBreak.store(true);
    BLEDevice::startAdvertising();
  }
#if defined(CONFIG_NIMBLE_ENABLED)
  void onMtuChanged(BLEServer*, ble_gap_conn_desc*, uint16_t newMtu) override {
    mtu = newMtu;
  }
#elif defined(CONFIG_BLUEDROID_ENABLED)
  void onMtuChanged(BLEServer*, esp_ble_gatts_cb_param_t* param) override {
    mtu = param->mtu.mtu;
  }
#endif
};

class SecCallbacks : public BLESecurityCallbacks {
  uint32_t onPassKeyRequest() override { return 0; }
  bool onConfirmPIN(uint32_t) override { return false; }
  bool onSecurityRequest() override { return true; }
  void onPassKeyNotify(uint32_t pk) override {
    passkey = pk;
  }
#if defined(CONFIG_NIMBLE_ENABLED)
  void onAuthenticationComplete(ble_gap_conn_desc* desc, int status) override {
    passkey = 0;
    bool ok = (status == 0) && desc && desc->sec_state.encrypted;
    secure = ok;
    authResult = ok ? 1 : 2;
    if (!ok && server && desc) {
      server->disconnect(desc->conn_handle);
    }
  }
#elif defined(CONFIG_BLUEDROID_ENABLED)
  void onAuthenticationComplete(esp_ble_auth_cmpl_t cmpl) override {
    passkey = 0;
    secure = cmpl.success;
    authResult = cmpl.success ? 1 : 2;
    if (!cmpl.success && server) {
      server->disconnect(server->getConnId());
    }
  }
#endif
};

void bleInit(const char* deviceName) {
  BLEDevice::init(deviceName);
  BLEDevice::setMTU(517);
  BLEDevice::setSecurityCallbacks(new SecCallbacks());

  BLESecurity* sec = new BLESecurity();
  sec->setCapability(ESP_IO_CAP_OUT);
  sec->setPassKey(false);
  sec->regenPassKeyOnConnect(true);
  sec->setAuthenticationMode(true, true, true);
  sec->setKeySize(16);
  sec->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  sec->setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);

  server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  BLEService* svc = server->createService(NUS_SERVICE_UUID);

#if defined(CONFIG_NIMBLE_ENABLED)
  txChar = svc->createCharacteristic(
    NUS_TX_UUID,
    BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_READ_ENC
  );
  rxChar = svc->createCharacteristic(
    NUS_RX_UUID,
    BLECharacteristic::PROPERTY_WRITE |
    BLECharacteristic::PROPERTY_WRITE_NR |
    BLECharacteristic::PROPERTY_WRITE_ENC
  );
#else
  txChar = svc->createCharacteristic(
    NUS_TX_UUID,
    BLECharacteristic::PROPERTY_NOTIFY
  );
  txChar->setAccessPermissions(ESP_GATT_PERM_READ_ENCRYPTED);
  BLE2902* cccd = new BLE2902();
  cccd->setAccessPermissions(ESP_GATT_PERM_READ_ENCRYPTED | ESP_GATT_PERM_WRITE_ENCRYPTED);
  txChar->addDescriptor(cccd);

  rxChar = svc->createCharacteristic(
    NUS_RX_UUID,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
  );
  rxChar->setAccessPermissions(ESP_GATT_PERM_WRITE_ENCRYPTED);
#endif
  rxChar->setCallbacks(new RxCallbacks());

  svc->start();

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(NUS_SERVICE_UUID);
  adv->setScanResponse(true);
  adv->setMinPreferred(0x06);
  adv->setMaxPreferred(0x12);
  // Reklama po 100–200 ms (výchozí 20–40 ms zbytečně bere baterii); PC krabičku
  // najde stejně za zlomek sekundy, daemon hledá 3,5 s
  adv->setMinInterval(0xA0);
  adv->setMaxInterval(0x140);
  BLEDevice::startAdvertising();
  Serial.printf("[ble] advertising as '%s'\n", deviceName);
}

bool bleConnected()   { return connected; }
bool bleSecure()      { return secure; }
uint32_t blePasskey() { return passkey; }

uint8_t bleTakeAuthResult() {
  uint8_t r = authResult;
  authResult = 0;
  return r;
}

void bleClearBonds() {
#if defined(CONFIG_NIMBLE_ENABLED)
  int rc = ble_store_clear();
  Serial.printf("[ble] cleared NimBLE bonds (rc=%d)\n", rc);
#elif defined(CONFIG_BLUEDROID_ENABLED)
  int n = esp_ble_get_bond_device_num();
  if (n <= 0) return;
  esp_ble_bond_dev_t* list = (esp_ble_bond_dev_t*)malloc(n * sizeof(esp_ble_bond_dev_t));
  if (!list) return;
  esp_ble_get_bond_device_list(&n, list);
  for (int i = 0; i < n; i++) esp_ble_remove_bond_device(list[i].bd_addr);
  free(list);
  Serial.printf("[ble] cleared %d bond(s)\n", n);
#endif
}

static void rxServiceClear() {
  if (rxClearReq.exchange(false)) {
    rxTail.store(rxHead.load(std::memory_order_acquire), std::memory_order_release);
  }
}

size_t bleAvailable() {
  rxServiceClear();
  size_t h = rxHead.load(std::memory_order_acquire);
  size_t t = rxTail.load(std::memory_order_relaxed);
  return (h + RX_CAP - t) % RX_CAP;
}

int bleRead() {
  rxServiceClear();
  size_t t = rxTail.load(std::memory_order_relaxed);
  if (rxHead.load(std::memory_order_acquire) == t) return -1;
  int b = rxBuf[t];
  rxTail.store((t + 1) % RX_CAP, std::memory_order_release);
  return b;
}

size_t bleWrite(const uint8_t* data, size_t len) {
  if (!connected || !txChar) return 0;
  size_t chunk = mtu > 3 ? mtu - 3 : 20;
  if (chunk > 180) chunk = 180;
  size_t sent = 0;
  while (sent < len) {
    if (!connected) break;   // odpojení uprostřed zápisu: zbytek nemá komu jít
    size_t n = len - sent;
    if (n > chunk) n = chunk;
    txChar->setValue(data + sent, n);
    txChar->notify();
    sent += n;
    delay(4);
  }
  return sent;
}
