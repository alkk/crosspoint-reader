#include "BleAppConnectActivity.h"

#include <BleManager.h>
#include <ESPmDNS.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_random.h>
#include <esp_task_wdt.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/icons/bluetooth.h"
#include "fontIds.h"
#include "util/QrUtils.h"

namespace {
constexpr const char* AP_HOSTNAME = "crosspoint";
constexpr uint8_t AP_CHANNEL = 1;
constexpr uint8_t AP_MAX_CONNECTIONS = 2;
constexpr int QR_CODE_WIDTH = 198;
constexpr int QR_CODE_HEIGHT = 198;
constexpr uint16_t HTTP_PORT = 80;
constexpr uint16_t WS_PORT = 81;
constexpr int AP_PASSWORD_LEN = 8;

void generateRandomPassword(char* buf, int len) {
  static constexpr char charset[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghjkmnpqrstuvwxyz23456789";
  static constexpr int charsetLen = sizeof(charset) - 1;
  for (int i = 0; i < len; i++) {
    buf[i] = charset[esp_random() % charsetLen];
  }
  buf[len] = '\0';
}
}  // namespace

BleAppConnectActivity::BleAppConnectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("BleAppConnect", renderer, mappedInput) {}

BleAppConnectActivity::~BleAppConnectActivity() = default;

void BleAppConnectActivity::onEnter() {
  Activity::onEnter();

  LOG_DBG("BLEACT", "Free heap at onEnter: %d bytes", ESP.getFreeHeap());

  state = BleAppConnectState::INITIALIZING;
  bleConnectedFlag = false;
  bleDisconnectedFlag = false;
  bleDisconnectTime = 0;
  lastHandleClientTime = 0;
  apSsid.clear();
  apPassword.clear();
  apIp.clear();

  // Build device name from MAC (CrossPoint-XXXX)
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char nameBuf[24];
  snprintf(nameBuf, sizeof(nameBuf), "CrossPoint-%02X%02X", mac[4], mac[5]);
  deviceName = nameBuf;

  // Build device info JSON
  char infoBuf[256];
  buildDeviceInfoJson(infoBuf, sizeof(infoBuf));

  // Init BLE
  bleManager = std::make_unique<BleManager>();
  bleManager->setConnectionCallback(onBleConnection, this);

  if (!bleManager->begin(deviceName.c_str())) {
    LOG_ERR("BLEACT", "BLE init failed");
    finish();
    return;
  }

  bleManager->setDeviceInfoJson(infoBuf);

  LOG_DBG("BLEACT", "Free heap after BLE init: %d bytes", ESP.getFreeHeap());

  if (!bleManager->startAdvertising()) {
    LOG_ERR("BLEACT", "BLE advertising failed");
    bleManager->end();
    bleManager.reset();
    finish();
    return;
  }

  state = BleAppConnectState::ADVERTISING;
  requestUpdate();
}

void BleAppConnectActivity::onExit() {
  Activity::onExit();

  LOG_DBG("BLEACT", "Free heap at onExit start: %d bytes", ESP.getFreeHeap());

  state = BleAppConnectState::SHUTTING_DOWN;

  stopWebServer();
  stopAccessPoint();

  if (bleManager) {
    bleManager->setConnectionCallback(nullptr, nullptr);
    bleManager->end();
    bleManager.reset();
  }

  WiFi.mode(WIFI_OFF);
  delay(30);

  LOG_DBG("BLEACT", "Free heap at onExit end: %d bytes", ESP.getFreeHeap());
}

void BleAppConnectActivity::loop() {
  // Handle BLE connection state changes (set by callback, consumed here on main thread)
  if (bleConnectedFlag) {
    bleConnectedFlag = false;
    bleDisconnectTime = 0;
    if (state == BleAppConnectState::ADVERTISING) {
      state = BleAppConnectState::CONNECTED;
      bleManager->notifyStatus("{\"state\":\"connected\"}");
      requestUpdate();
      LOG_DBG("BLEACT", "App connected, waiting for commands");
    } else if (state == BleAppConnectState::AP_RUNNING) {
      // App reconnected while AP is still running - re-send credentials
      char notifyBuf[256];
      snprintf(
          notifyBuf, sizeof(notifyBuf),
          "{\"state\":\"ap_running\",\"ssid\":\"%s\",\"pass\":\"%s\",\"ip\":\"%s\",\"http_port\":%d,\"ws_port\":%d}",
          apSsid.c_str(), apPassword.c_str(), apIp.c_str(), HTTP_PORT, WS_PORT);
      if (bleManager) {
        bleManager->notifyApControl(notifyBuf);
        bleManager->notifyStatus("{\"state\":\"ap_running\"}");
      }
      LOG_DBG("BLEACT", "App reconnected while AP running, re-sent credentials");
    }
  }

  if (bleDisconnectedFlag) {
    bleDisconnectedFlag = false;
    if (state == BleAppConnectState::AP_RUNNING) {
      // App disconnected while AP is running - start timeout and re-advertise for reconnection
      bleDisconnectTime = millis();
      if (bleManager) {
        bleManager->startAdvertising();
      }
      LOG_DBG("BLEACT", "BLE disconnected while AP running, re-advertising, starting %lu s timeout",
              BLE_DISCONNECT_TIMEOUT_MS / 1000);
    } else if (state == BleAppConnectState::CONNECTED) {
      // App disconnected before AP started - go back to advertising
      state = BleAppConnectState::ADVERTISING;
      bleManager->startAdvertising();
      requestUpdate();
      LOG_DBG("BLEACT", "App disconnected, resuming advertising");
    }
  }

  // Reconcile state with actual BLE connection (handles rapid disconnect-then-reconnect race)
  if (state == BleAppConnectState::ADVERTISING && bleManager && bleManager->isConnected()) {
    state = BleAppConnectState::CONNECTED;
    bleManager->notifyStatus("{\"state\":\"connected\"}");
    requestUpdate();
    LOG_DBG("BLEACT", "State reconciled: actually connected");
  }

  // Check BLE disconnect timeout (guard with isConnected to handle rapid disconnect-reconnect race)
  if (bleDisconnectTime > 0 && !(bleManager && bleManager->isConnected()) &&
      (millis() - bleDisconnectTime) >= BLE_DISCONNECT_TIMEOUT_MS) {
    LOG_DBG("BLEACT", "BLE disconnect timeout — shutting down AP");
    handleApStopCommand();
    bleDisconnectTime = 0;
  }

  // Poll for BLE commands
  if (bleManager && bleManager->hasPendingCommand()) {
    uint8_t cmd = bleManager->consumePendingCommand();
    if (cmd == BLE_CMD_START_AP) {
      handleApStartCommand();
    } else if (cmd == BLE_CMD_STOP_AP) {
      handleApStopCommand();
    }
  }

  // Handle web server requests when AP is running
  if (state == BleAppConnectState::AP_RUNNING && webServer && webServer->isRunning()) {
    const unsigned long timeSinceLastHandleClient = millis() - lastHandleClientTime;
    if (lastHandleClientTime > 0 && timeSinceLastHandleClient > 100) {
      LOG_DBG("BLEACT", "WARNING: %lu ms gap since last handleClient", timeSinceLastHandleClient);
    }

    esp_task_wdt_reset();

    constexpr int MAX_ITERATIONS = 500;
    for (int i = 0; i < MAX_ITERATIONS && webServer->isRunning(); i++) {
      webServer->handleClient();
      if ((i & 0x1F) == 0x1F) {
        esp_task_wdt_reset();
      }
      if ((i & 0x3F) == 0x3F) {
        yield();
        mappedInput.update();
        if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
          finish();
          return;
        }
      }
    }
    lastHandleClientTime = millis();
  }

  // Handle Back button
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
}

void BleAppConnectActivity::render(RenderLock&&) {
  renderer.clearScreen();

  switch (state) {
    case BleAppConnectState::ADVERTISING:
      renderAdvertising();
      break;
    case BleAppConnectState::CONNECTED:
      renderConnected();
      break;
    case BleAppConnectState::AP_RUNNING:
      renderApRunning();
      break;
    default:
      break;
  }

  renderer.displayBuffer();
}

void BleAppConnectActivity::renderAdvertising() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_BLE_APP_CONNECT),
                 nullptr);

  const auto height10 = renderer.getLineHeight(UI_10_FONT_ID);
  const auto centerY = (pageHeight - height10 * 3) / 2;

  // BLE icon
  renderer.drawIcon(BluetoothIcon, (pageWidth - 32) / 2, centerY - 48, 32, 32);

  renderer.drawCenteredText(UI_12_FONT_ID, centerY, tr(STR_BLE_WAITING), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, centerY + height10 * 2, deviceName.c_str());

  const auto labels = mappedInput.mapLabels(tr(STR_EXIT), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void BleAppConnectActivity::renderConnected() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_BLE_APP_CONNECT),
                 nullptr);

  const auto height10 = renderer.getLineHeight(UI_10_FONT_ID);
  const auto centerY = (pageHeight - height10) / 2;

  renderer.drawCenteredText(UI_12_FONT_ID, centerY, tr(STR_BLE_CONNECTED), true, EpdFontFamily::BOLD);

  const auto labels = mappedInput.mapLabels(tr(STR_EXIT), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void BleAppConnectActivity::renderApRunning() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_BLE_APP_CONNECT),
                 nullptr);
  GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                    apSsid.c_str());

  int startY = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing * 2;
  const auto height10 = renderer.getLineHeight(UI_10_FONT_ID);

  renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, startY, tr(STR_BLE_TRANSFERRING), true,
                    EpdFontFamily::BOLD);
  startY += height10 + metrics.verticalSpacing * 2;

  // WiFi QR code
  std::string wifiConfig = std::string("WIFI:S:") + apSsid + ";T:WPA;P:" + apPassword + ";;";
  const Rect qrBoundsWifi(metrics.contentSidePadding, startY, QR_CODE_WIDTH, QR_CODE_HEIGHT);
  QrUtils::drawQrCode(renderer, qrBoundsWifi, wifiConfig);

  // SSID and IP next to QR
  const int textX = metrics.contentSidePadding + QR_CODE_WIDTH + metrics.verticalSpacing;
  renderer.drawText(UI_10_FONT_ID, textX, startY + 60, apSsid.c_str());

  std::string ipUrl = "http://" + apIp + "/";
  renderer.drawText(SMALL_FONT_ID, textX, startY + 80, ipUrl.c_str());

  startY += QR_CODE_HEIGHT + metrics.verticalSpacing * 2;

  // URL QR code
  renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, startY, tr(STR_OPEN_URL_HINT), true,
                    EpdFontFamily::BOLD);
  startY += height10 + metrics.verticalSpacing * 2;

  const Rect qrBoundsUrl(metrics.contentSidePadding, startY, QR_CODE_WIDTH, QR_CODE_HEIGHT);
  QrUtils::drawQrCode(renderer, qrBoundsUrl, ipUrl);

  std::string hostnameUrl = std::string("http://") + AP_HOSTNAME + ".local/";
  renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding + QR_CODE_WIDTH + metrics.verticalSpacing, startY + 60,
                    hostnameUrl.c_str());
  renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding + QR_CODE_WIDTH + metrics.verticalSpacing, startY + 80,
                    (std::string(tr(STR_OR_HTTP_PREFIX)) + apIp + "/").c_str());

  const auto labels = mappedInput.mapLabels(tr(STR_EXIT), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void BleAppConnectActivity::buildDeviceInfoJson(char* buf, size_t bufSize) const {
  const char* hwType = gpio.deviceIsX3() ? "x3" : "x4";
  uint16_t battery = powerManager.getBatteryPercentage();

  uint8_t mac[6];
  WiFi.macAddress(mac);
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  snprintf(buf, bufSize, "{\"name\":\"%s\",\"fw\":\"%s\",\"hw\":\"%s\",\"battery\":%d,\"mac\":\"%s\"}",
           deviceName.c_str(), CROSSPOINT_VERSION, hwType, battery, macStr);
}

void BleAppConnectActivity::generateApCredentials() {
  // SSID from device name (already has MAC suffix)
  apSsid = deviceName;

  // Random password using hardware TRNG
  char passBuf[AP_PASSWORD_LEN + 1];
  generateRandomPassword(passBuf, AP_PASSWORD_LEN);
  apPassword = passBuf;
}

bool BleAppConnectActivity::startAccessPoint() {
  LOG_DBG("BLEACT", "Starting WiFi AP...");
  LOG_DBG("BLEACT", "Free heap before AP start: %d bytes", ESP.getFreeHeap());

  WiFi.mode(WIFI_AP);
  delay(100);

  bool apStarted = WiFi.softAP(apSsid.c_str(), apPassword.c_str(), AP_CHANNEL, false, AP_MAX_CONNECTIONS);
  if (!apStarted) {
    LOG_ERR("BLEACT", "Failed to start AP");
    WiFi.mode(WIFI_OFF);
    return false;
  }

  delay(100);

  const IPAddress ip = WiFi.softAPIP();
  char ipStr[16];
  snprintf(ipStr, sizeof(ipStr), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
  apIp = ipStr;

  LOG_DBG("BLEACT", "AP started: SSID=%s IP=%s", apSsid.c_str(), apIp.c_str());

  if (MDNS.begin(AP_HOSTNAME)) {
    LOG_DBG("BLEACT", "mDNS started: http://%s.local/", AP_HOSTNAME);
  }

  LOG_DBG("BLEACT", "Free heap after AP start: %d bytes", ESP.getFreeHeap());
  return true;
}

void BleAppConnectActivity::stopAccessPoint() {
  MDNS.end();
  delay(50);
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(30);
  apIp.clear();
  LOG_DBG("BLEACT", "AP stopped");
}

void BleAppConnectActivity::startWebServer() {
  LOG_DBG("BLEACT", "Starting web server...");
  webServer.reset(new CrossPointWebServer());
  webServer->begin();

  if (!webServer->isRunning()) {
    LOG_ERR("BLEACT", "Web server failed to start");
    webServer.reset();
  } else {
    LOG_DBG("BLEACT", "Web server started");
  }
}

void BleAppConnectActivity::stopWebServer() {
  if (webServer) {
    if (webServer->isRunning()) {
      webServer->stop();
      LOG_DBG("BLEACT", "Web server stopped");
    }
    webServer.reset();
  }
}

void BleAppConnectActivity::handleApStartCommand() {
  if (state == BleAppConnectState::AP_RUNNING) {
    LOG_DBG("BLEACT", "AP already running, ignoring start command");
    return;
  }

  LOG_DBG("BLEACT", "AP start command received");

  generateApCredentials();

  if (!startAccessPoint()) {
    if (bleManager) {
      bleManager->notifyApControl("{\"state\":\"error\",\"msg\":\"ap_failed\"}");
      bleManager->notifyStatus("{\"state\":\"error\",\"msg\":\"ap_failed\"}");
    }
    return;
  }

  startWebServer();
  if (!webServer || !webServer->isRunning()) {
    stopAccessPoint();
    if (bleManager) {
      bleManager->notifyApControl("{\"state\":\"error\",\"msg\":\"server_failed\"}");
      bleManager->notifyStatus("{\"state\":\"error\",\"msg\":\"server_failed\"}");
    }
    return;
  }

  state = BleAppConnectState::AP_RUNNING;

  // Notify app with AP credentials
  char notifyBuf[256];
  snprintf(notifyBuf, sizeof(notifyBuf),
           "{\"state\":\"ap_running\",\"ssid\":\"%s\",\"pass\":\"%s\",\"ip\":\"%s\",\"http_port\":%d,\"ws_port\":%d}",
           apSsid.c_str(), apPassword.c_str(), apIp.c_str(), HTTP_PORT, WS_PORT);

  if (bleManager) {
    bleManager->notifyApControl(notifyBuf);
    bleManager->notifyStatus("{\"state\":\"ap_running\"}");
  }

  requestUpdate();
  LOG_DBG("BLEACT", "AP + server running, app notified");
}

void BleAppConnectActivity::handleApStopCommand() {
  if (state != BleAppConnectState::AP_RUNNING) {
    LOG_DBG("BLEACT", "AP not running, ignoring stop command");
    return;
  }

  LOG_DBG("BLEACT", "AP stop command received");

  stopWebServer();
  stopAccessPoint();

  if (bleManager && bleManager->isConnected()) {
    state = BleAppConnectState::CONNECTED;
    bleManager->notifyApControl("{\"state\":\"ap_stopped\"}");
    bleManager->notifyStatus("{\"state\":\"connected\"}");
  } else {
    state = BleAppConnectState::ADVERTISING;
    if (bleManager) {
      bleManager->startAdvertising();
    }
  }

  requestUpdate();
  LOG_DBG("BLEACT", "AP stopped, state=%d", static_cast<int>(state));
}

void BleAppConnectActivity::onBleConnection(bool connected, void* ctx) {
  auto* self = static_cast<BleAppConnectActivity*>(ctx);
  if (connected) {
    self->bleConnectedFlag = true;
  } else {
    self->bleDisconnectedFlag = true;
  }
}
