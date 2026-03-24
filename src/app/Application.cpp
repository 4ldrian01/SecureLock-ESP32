#include "Application.h"
#include "secrets.h"

namespace {
securelock::app::Application* g_appInstance = nullptr;

#ifndef WIFI_EXTRA_1_SSID
#define WIFI_EXTRA_1_SSID ""
#endif
#ifndef WIFI_EXTRA_1_PASSWORD
#define WIFI_EXTRA_1_PASSWORD ""
#endif
#ifndef WIFI_EXTRA_2_SSID
#define WIFI_EXTRA_2_SSID ""
#endif
#ifndef WIFI_EXTRA_2_PASSWORD
#define WIFI_EXTRA_2_PASSWORD ""
#endif
#ifndef WIFI_EXTRA_3_SSID
#define WIFI_EXTRA_3_SSID ""
#endif
#ifndef WIFI_EXTRA_3_PASSWORD
#define WIFI_EXTRA_3_PASSWORD ""
#endif
#ifndef WIFI_EXTRA_4_SSID
#define WIFI_EXTRA_4_SSID ""
#endif
#ifndef WIFI_EXTRA_4_PASSWORD
#define WIFI_EXTRA_4_PASSWORD ""
#endif

constexpr size_t kPinLength = 4;
}

namespace securelock {
namespace app {

Application::Application()
    : _lockManager(),
      _securityManager(),
      _authHandler(),
      _webServer(&_lockManager, &_securityManager, &_authHandler),
      _wifiController(),
      _keypadBuffer(""),
      _authPrompt(""),
      _lastKeypadKey('\0'),
      _lastKeypadKeyMs(0) {
    g_appInstance = this;
}

void Application::setup() {
    Serial.begin(115200);
    Serial.println("[APP] SecureLock modular backend bootstrap starting...");

    _lockManager.init();
    _securityManager.init();
    _authHandler.init();

    const WiFiController::Credential credentials[] = {
        { WIFI_SSID, WIFI_PASSWORD },
        { WIFI_EXTRA_1_SSID, WIFI_EXTRA_1_PASSWORD },
        { WIFI_EXTRA_2_SSID, WIFI_EXTRA_2_PASSWORD },
        { WIFI_EXTRA_3_SSID, WIFI_EXTRA_3_PASSWORD },
        { WIFI_EXTRA_4_SSID, WIFI_EXTRA_4_PASSWORD }
    };

    _wifiController.begin(credentials, sizeof(credentials) / sizeof(credentials[0]), "securelock");

    // Web server starts regardless of link state; it becomes reachable once WiFi is up.
    _webServer.init();

    Serial.println("[APP] Modular backend online (batch 1)");
}

void Application::loop() {
    _wifiController.update();

    _lockManager.update();
    _securityManager.update();
    _authHandler.update();
    _webServer.update();

    _processRFID();
    _processKeypad();

    yield();
}

void Application::_processRFID() {
    const AuthResult result = _authHandler.checkRFID();
    if (result == AUTH_SUCCESS) {
        const String uid = _authHandler.getLastRFIDUID();
        const String userName = _authHandler.getUserName(uid);

        _lockManager.unlock();
        _authHandler.startRFIDCooldown();
        _securityManager.beep(2);

        _authPrompt = "RFID access granted";
        _webServer.logActivity(userName, "RFID", "success");
        return;
    }

    if (result == AUTH_DENIED) {
        _authHandler.startRFIDCooldown();
        _securityManager.beep(3);
        _authPrompt = "RFID denied";
        _webServer.logActivity("Unknown RFID", "RFID", "fail");
    }
}

void Application::_processKeypad() {
    const char key = _authHandler.getKeypadKey();
    if (!key) {
        return;
    }

    _lastKeypadKey = key;
    _lastKeypadKeyMs = millis();

    if (key == '*') {
        _keypadBuffer = "";
        _authHandler.clearBuffer();
        _authPrompt = "Keypad cleared";
        return;
    }

    if (key == '#') {
        if (_keypadBuffer.length() == kPinLength) {
            const AuthResult pinResult = _authHandler.validatePIN(_keypadBuffer);
            if (pinResult == AUTH_SUCCESS) {
                _lockManager.unlock();
                _securityManager.beep(2);
                _webServer.logActivity("Keypad", "PIN", "success");
                _authPrompt = "PIN accepted";
            } else if (pinResult == AUTH_DURESS) {
                _lockManager.unlock();
                _securityManager.beep(2);
                _webServer.logActivity("Keypad", "Duress PIN", "alarm");
                _authPrompt = "Duress PIN accepted";
            } else {
                _securityManager.beep(3);
                _webServer.logActivity("Keypad", "PIN", "fail");
                _authPrompt = "PIN denied";
            }
        }

        _keypadBuffer = "";
        _authHandler.clearBuffer();
        return;
    }

    if (key >= '0' && key <= '9') {
        if (_keypadBuffer.length() < kPinLength) {
            _keypadBuffer += key;
            _authPrompt = "Entering PIN";
        }
        return;
    }
}

String Application::getActiveGuestCode() const {
    return "";
}

bool Application::isTemporaryGuestCodeActive() const {
    return false;
}

unsigned long Application::getTemporaryGuestCodeRemainingMs() const {
    return 0;
}

unsigned long Application::getGuestCodeCommandCooldownRemainingMs() const {
    return 0;
}

String Application::getAuthPrompt() const {
    return _authPrompt;
}

bool Application::isPendingAccessActive() const {
    return _keypadBuffer.length() > 0;
}

String Application::getLastKeypadKeyLabel() const {
    if (!_lastKeypadKey) {
        return "";
    }

    return String(_lastKeypadKey);
}

unsigned long Application::getLastKeypadKeyMs() const {
    return _lastKeypadKeyMs;
}

unsigned long Application::getTelegramPollIntervalMs() const {
    return 0;
}

unsigned long Application::getTelegramLastPollDurationMs() const {
    return 0;
}

unsigned long Application::getTelegramLastSuccessMs() const {
    return 0;
}

unsigned long Application::getTelegramLastErrorMs() const {
    return 0;
}

unsigned long Application::getTelegramLastCommandMs() const {
    return 0;
}

unsigned long Application::getTelegramLastCommandLatencyMs() const {
    return 0;
}

unsigned long Application::getTelegramCommandsHandled() const {
    return 0;
}

unsigned long Application::getTelegramPollErrors() const {
    return 0;
}

int Application::getTelegramPendingApprox() const {
    return 0;
}

String Application::getTelegramLastCommandText() const {
    return "";
}

String Application::getTelegramLastCommandRole() const {
    return "";
}

String Application::getTelegramLastCommandResult() const {
    return "";
}

} // namespace app
} // namespace securelock

// ===== WebServer status bridge symbols (transitional ABI compatibility) =====
String getActiveGuestCode() {
    return g_appInstance ? g_appInstance->getActiveGuestCode() : "";
}

bool isTemporaryGuestCodeActive() {
    return g_appInstance ? g_appInstance->isTemporaryGuestCodeActive() : false;
}

unsigned long getTemporaryGuestCodeRemainingMs() {
    return g_appInstance ? g_appInstance->getTemporaryGuestCodeRemainingMs() : 0;
}

unsigned long getGuestCodeCommandCooldownRemainingMs() {
    return g_appInstance ? g_appInstance->getGuestCodeCommandCooldownRemainingMs() : 0;
}

String getAuthPrompt() {
    return g_appInstance ? g_appInstance->getAuthPrompt() : "";
}

bool isPendingAccessActive() {
    return g_appInstance ? g_appInstance->isPendingAccessActive() : false;
}

String getLastKeypadKeyLabel() {
    return g_appInstance ? g_appInstance->getLastKeypadKeyLabel() : "";
}

unsigned long getLastKeypadKeyMs() {
    return g_appInstance ? g_appInstance->getLastKeypadKeyMs() : 0;
}

unsigned long getTelegramPollIntervalMs() {
    return g_appInstance ? g_appInstance->getTelegramPollIntervalMs() : 0;
}

unsigned long getTelegramLastPollDurationMs() {
    return g_appInstance ? g_appInstance->getTelegramLastPollDurationMs() : 0;
}

unsigned long getTelegramLastSuccessMs() {
    return g_appInstance ? g_appInstance->getTelegramLastSuccessMs() : 0;
}

unsigned long getTelegramLastErrorMs() {
    return g_appInstance ? g_appInstance->getTelegramLastErrorMs() : 0;
}

unsigned long getTelegramLastCommandMs() {
    return g_appInstance ? g_appInstance->getTelegramLastCommandMs() : 0;
}

unsigned long getTelegramLastCommandLatencyMs() {
    return g_appInstance ? g_appInstance->getTelegramLastCommandLatencyMs() : 0;
}

unsigned long getTelegramCommandsHandled() {
    return g_appInstance ? g_appInstance->getTelegramCommandsHandled() : 0;
}

unsigned long getTelegramPollErrors() {
    return g_appInstance ? g_appInstance->getTelegramPollErrors() : 0;
}

int getTelegramPendingApprox() {
    return g_appInstance ? g_appInstance->getTelegramPendingApprox() : 0;
}

String getTelegramLastCommandText() {
    return g_appInstance ? g_appInstance->getTelegramLastCommandText() : "";
}

String getTelegramLastCommandRole() {
    return g_appInstance ? g_appInstance->getTelegramLastCommandRole() : "";
}

String getTelegramLastCommandResult() {
    return g_appInstance ? g_appInstance->getTelegramLastCommandResult() : "";
}
