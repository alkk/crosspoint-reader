#include "BleManager.h"

#include <Logging.h>
#include <NimBLEDevice.h>

static portMUX_TYPE bleCmdMux = portMUX_INITIALIZER_UNLOCKED;

static constexpr const char* SERVICE_UUID = "43505200-4d47-5400-0000-000000000001";
static constexpr const char* DEVICE_INFO_UUID = "43505200-4d47-5400-0000-000000000002";
static constexpr const char* AP_CONTROL_UUID = "43505200-4d47-5400-0000-000000000003";
static constexpr const char* STATUS_UUID = "43505200-4d47-5400-0000-000000000004";

// NimBLE server callbacks — forwards to BleManager via stored pointer
class BleServerCallbacks : public NimBLEServerCallbacks {
  BleManager* mgr;

 public:
  explicit BleServerCallbacks(BleManager* m) : mgr(m) {}

  void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
    (void)pServer;
    (void)connInfo;
    mgr->onClientConnected();
  }

  void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
    (void)pServer;
    (void)connInfo;
    (void)reason;
    mgr->onClientDisconnected();
  }
};

// AP Control characteristic write callback
class ApControlCallbacks : public NimBLECharacteristicCallbacks {
  BleManager* mgr;

 public:
  explicit ApControlCallbacks(BleManager* m) : mgr(m) {}

  void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override {
    (void)connInfo;
    auto val = pCharacteristic->getValue();
    if (val.size() >= 1) {
      mgr->onApControlWrite(val.data(), val.size());
    }
  }
};

BleManager::~BleManager() {
  if (initialized) {
    end();
  }
}

bool BleManager::begin(const char* deviceName) {
  if (initialized) {
    LOG_ERR("BLE", "Already initialized");
    return false;
  }

  LOG_DBG("BLE", "Initializing NimBLE as '%s'", deviceName);

  if (!NimBLEDevice::init(std::string(deviceName))) {
    LOG_ERR("BLE", "NimBLEDevice::init failed");
    return false;
  }

  server = NimBLEDevice::createServer();
  if (!server) {
    LOG_ERR("BLE", "createServer failed");
    NimBLEDevice::deinit(true);
    return false;
  }

  // NimBLE owns the callback object when deleteCallbacks=true (default)
  server->setCallbacks(new BleServerCallbacks(this));

  NimBLEService* service = server->createService(SERVICE_UUID);
  if (!service) {
    LOG_ERR("BLE", "createService failed");
    NimBLEDevice::deinit(true);
    server = nullptr;
    return false;
  }

  // Device Info: read-only
  deviceInfoChar = service->createCharacteristic(DEVICE_INFO_UUID, NIMBLE_PROPERTY::READ);

  // AP Control: write + notify
  apControlChar = service->createCharacteristic(AP_CONTROL_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY);

  // Status: read + notify
  statusChar = service->createCharacteristic(STATUS_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

  if (!deviceInfoChar || !apControlChar || !statusChar) {
    LOG_ERR("BLE", "createCharacteristic failed (low memory?)");
    NimBLEDevice::deinit(true);
    server = nullptr;
    deviceInfoChar = nullptr;
    apControlChar = nullptr;
    statusChar = nullptr;
    return false;
  }

  apControlChar->setCallbacks(new ApControlCallbacks(this));

  // Set initial status
  statusChar->setValue("{\"state\":\"idle\"}");

  if (!server->start()) {
    LOG_ERR("BLE", "server start failed");
    NimBLEDevice::deinit(true);
    server = nullptr;
    return false;
  }

  // Configure advertising data once (addServiceUUID accumulates, so must not be called per-start)
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  if (adv) {
    adv->addServiceUUID(SERVICE_UUID);
    adv->setMinInterval(0x20);  // 20ms in 0.625ms units
    adv->setMaxInterval(0xA0);  // 100ms in 0.625ms units
    adv->enableScanResponse(true);
  }

  initialized = true;
  pendingCommand = BLE_CMD_NONE;
  connected = false;

  LOG_DBG("BLE", "Initialized successfully");
  return true;
}

void BleManager::end() {
  if (!initialized) return;

  LOG_DBG("BLE", "Shutting down NimBLE");

  stopAdvertising();
  NimBLEDevice::deinit(true);

  server = nullptr;
  deviceInfoChar = nullptr;
  apControlChar = nullptr;
  statusChar = nullptr;
  pendingCommand = BLE_CMD_NONE;
  connected = false;
  initialized = false;

  LOG_DBG("BLE", "Shutdown complete");
}

bool BleManager::startAdvertising() {
  if (!initialized || !server) return false;

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  if (!adv) {
    LOG_ERR("BLE", "getAdvertising returned null");
    return false;
  }

  if (!adv->start()) {
    LOG_ERR("BLE", "Advertising start failed");
    return false;
  }

  LOG_DBG("BLE", "Advertising started");
  return true;
}

void BleManager::stopAdvertising() {
  if (!initialized) return;

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  if (adv) {
    adv->stop();
    LOG_DBG("BLE", "Advertising stopped");
  }
}

bool BleManager::isConnected() const { return connected; }

void BleManager::setDeviceInfoJson(const char* json) {
  if (deviceInfoChar) {
    deviceInfoChar->setValue(json);
  }
}

void BleManager::notifyApControl(const char* json) {
  if (!apControlChar || !connected) return;

  apControlChar->setValue(json);
  apControlChar->notify();
}

void BleManager::notifyStatus(const char* json) {
  if (!statusChar || !connected) return;

  statusChar->setValue(json);
  statusChar->notify();
}

bool BleManager::hasPendingCommand() const { return pendingCommand != BLE_CMD_NONE; }

uint8_t BleManager::consumePendingCommand() {
  portENTER_CRITICAL(&bleCmdMux);
  uint8_t cmd = pendingCommand;
  pendingCommand = BLE_CMD_NONE;
  portEXIT_CRITICAL(&bleCmdMux);
  return cmd;
}

void BleManager::setConnectionCallback(BleConnectionCallback cb, void* ctx) {
  portENTER_CRITICAL(&bleCmdMux);
  connectionCb = cb;
  connectionCbCtx = ctx;
  portEXIT_CRITICAL(&bleCmdMux);
}

void BleManager::onClientConnected() {
  LOG_DBG("BLE", "Client connected");
  connected = true;
  BleConnectionCallback cb;
  void* ctx;
  portENTER_CRITICAL(&bleCmdMux);
  cb = connectionCb;
  ctx = connectionCbCtx;
  portEXIT_CRITICAL(&bleCmdMux);
  if (cb) {
    cb(true, ctx);
  }
}

void BleManager::onClientDisconnected() {
  LOG_DBG("BLE", "Client disconnected");
  connected = false;
  BleConnectionCallback cb;
  void* ctx;
  portENTER_CRITICAL(&bleCmdMux);
  pendingCommand = BLE_CMD_NONE;
  cb = connectionCb;
  ctx = connectionCbCtx;
  portEXIT_CRITICAL(&bleCmdMux);
  if (cb) {
    cb(false, ctx);
  }
}

void BleManager::onApControlWrite(const uint8_t* data, size_t len) {
  if (len < 1) return;

  uint8_t cmd = data[0];
  if (cmd == BLE_CMD_START_AP || cmd == BLE_CMD_STOP_AP) {
    LOG_DBG("BLE", "AP control command: 0x%02X", cmd);
    portENTER_CRITICAL(&bleCmdMux);
    pendingCommand = cmd;
    portEXIT_CRITICAL(&bleCmdMux);
  } else {
    LOG_ERR("BLE", "Unknown AP control command: 0x%02X", cmd);
  }
}
