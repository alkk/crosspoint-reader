#pragma once

#include <memory>
#include <string>

#include "activities/Activity.h"
#include "network/CrossPointWebServer.h"

class BleManager;

enum class BleAppConnectState {
  INITIALIZING,
  ADVERTISING,
  CONNECTED,
  AP_RUNNING,
  SHUTTING_DOWN,
};

class BleAppConnectActivity final : public Activity {
  BleAppConnectState state = BleAppConnectState::INITIALIZING;

  std::unique_ptr<BleManager> bleManager;
  std::unique_ptr<CrossPointWebServer> webServer;

  std::string apSsid;
  std::string apPassword;
  std::string apIp;
  std::string deviceName;

  volatile bool bleConnectedFlag = false;
  volatile bool bleDisconnectedFlag = false;

  unsigned long bleDisconnectTime = 0;
  unsigned long lastHandleClientTime = 0;

  static constexpr unsigned long BLE_DISCONNECT_TIMEOUT_MS = 60000;

  void renderAdvertising() const;
  void renderConnected() const;
  void renderApRunning() const;

  void buildDeviceInfoJson(char* buf, size_t bufSize) const;
  void generateApCredentials();
  bool startAccessPoint();
  void stopAccessPoint();
  void startWebServer();
  void stopWebServer();
  void handleApStartCommand();
  void handleApStopCommand();

  static void onBleConnection(bool connected, void* ctx);

 public:
  explicit BleAppConnectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  ~BleAppConnectActivity() override;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return state == BleAppConnectState::AP_RUNNING; }
  bool preventAutoSleep() override { return state != BleAppConnectState::SHUTTING_DOWN; }
};
