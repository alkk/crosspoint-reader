#pragma once

#include <Arduino.h>

class NimBLEServer;
class NimBLECharacteristic;

// Command bytes written to AP Control characteristic
static constexpr uint8_t BLE_CMD_NONE = 0xFF;
static constexpr uint8_t BLE_CMD_STOP_AP = 0x00;
static constexpr uint8_t BLE_CMD_START_AP = 0x01;

using BleConnectionCallback = void (*)(bool connected, void* ctx);

class BleManager {
 public:
  BleManager() = default;
  ~BleManager();

  BleManager(const BleManager&) = delete;
  BleManager& operator=(const BleManager&) = delete;
  BleManager(BleManager&&) = delete;
  BleManager& operator=(BleManager&&) = delete;

  // Lifecycle
  bool begin(const char* deviceName);
  void end();

  // Advertising
  bool startAdvertising();
  void stopAdvertising();

  // Connection state
  bool isConnected() const;

  // Characteristic data
  void setDeviceInfoJson(const char* json);
  void notifyApControl(const char* json);
  void notifyStatus(const char* json);

  // Command polling (main-thread safe)
  bool hasPendingCommand() const;
  uint8_t consumePendingCommand();

  // Callbacks
  void setConnectionCallback(BleConnectionCallback cb, void* ctx);

  // Called by NimBLE callback classes (internal use)
  void onClientConnected();
  void onClientDisconnected();
  void onApControlWrite(const uint8_t* data, size_t len);

 private:
  NimBLEServer* server = nullptr;
  NimBLECharacteristic* deviceInfoChar = nullptr;
  NimBLECharacteristic* apControlChar = nullptr;
  NimBLECharacteristic* statusChar = nullptr;

  volatile uint8_t pendingCommand = BLE_CMD_NONE;
  volatile bool connected = false;
  bool initialized = false;

  BleConnectionCallback connectionCb = nullptr;
  void* connectionCbCtx = nullptr;
};
