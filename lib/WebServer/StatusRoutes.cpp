#include "WebServer.h"

// Shared guest-code and diagnostics state from runtime orchestrator
extern String getActiveGuestCode();
extern bool isTemporaryGuestCodeActive();
extern unsigned long getTemporaryGuestCodeRemainingMs();
extern unsigned long getGuestCodeCommandCooldownRemainingMs();
extern String getAuthPrompt();
extern bool isPendingAccessActive();
extern unsigned long getTelegramPollIntervalMs();
extern unsigned long getTelegramLastPollDurationMs();
extern unsigned long getTelegramLastSuccessMs();
extern unsigned long getTelegramLastErrorMs();
extern unsigned long getTelegramLastCommandMs();
extern unsigned long getTelegramLastCommandLatencyMs();
extern unsigned long getTelegramCommandsHandled();
extern unsigned long getTelegramPollErrors();
extern int getTelegramPendingApprox();
extern String getTelegramLastCommandText();
extern String getTelegramLastCommandRole();
extern String getTelegramLastCommandResult();

void WebServer::_handleAPIStatus(AsyncWebServerRequest* request) {
    if (!_requireApiAuth(request)) {
        return;
    }

    JsonDocument doc;

    doc["locked"] = _lock->isLocked();
    doc["doorOpen"] = _lock->isDoorOpen();
    doc["tampered"] = _lock->isDoorTampered();
    doc["autoLockDelayMs"] = _lock->getAutoLockDelayMs();
    doc["unlockRemainingMs"] = _lock->getRemainingAutoLockMs();
    doc["emergencyCooldownRemainingMs"] = _remainingCooldownMs(_lastEmergencyUnlockMs, EMERGENCY_COOLDOWN_MS);
    const unsigned long emergencyGuestLockRemainingMs = getTemporaryGuestCodeRemainingMs();
    doc["emergencyGuestLockRemainingMs"] = emergencyGuestLockRemainingMs;
    doc["emergencyOverrideBlockedByGuestCode"] = emergencyGuestLockRemainingMs > 0;

    doc["alarm"] = _security->isAlarming();
    doc["vibration"] = _security->isVibrationLatched();
    doc["buzzerActive"] = _security->isBuzzerActive();
    doc["sirenActive"] = _security->isSirenActive();

    doc["uptime"] = millis() / 1000;
    doc["freeHeap"] = ESP.getFreeHeap();
    doc["wifiConnected"] = _wifiConnected;
    doc["ipAddress"] = _ipAddress;
    doc["rssi"] = WiFi.RSSI();

    const bool telegramGuestActive = isTemporaryGuestCodeActive();
    const String telegramGuestCode = getActiveGuestCode();
    const unsigned long telegramGuestRemainingMs = getTemporaryGuestCodeRemainingMs();

    doc["guestCodeActive"] = telegramGuestActive;
    doc["guestCode"] = telegramGuestActive ? telegramGuestCode : "";
    doc["guestCodeRemainingMs"] = telegramGuestRemainingMs;
    doc["guestCodeCooldownRemainingMs"] = getGuestCodeCommandCooldownRemainingMs();
    doc["authPrompt"] = getAuthPrompt();
    doc["pendingAccess"] = isPendingAccessActive();
    doc["timestampMs"] = millis();

    const unsigned long nowMs = millis();
    const unsigned long lastTgCommandMs = getTelegramLastCommandMs();
    const unsigned long lastTgErrorMs = getTelegramLastErrorMs();
    doc["telegramPollIntervalMs"] = getTelegramPollIntervalMs();
    doc["telegramLastPollDurationMs"] = getTelegramLastPollDurationMs();
    doc["telegramLastSuccessMs"] = getTelegramLastSuccessMs();
    doc["telegramLastErrorMs"] = lastTgErrorMs;
    doc["telegramLastCommandMs"] = lastTgCommandMs;
    doc["telegramLastCommandLatencyMs"] = getTelegramLastCommandLatencyMs();
    doc["telegramCommandsHandled"] = getTelegramCommandsHandled();
    doc["telegramPollErrors"] = getTelegramPollErrors();
    doc["telegramPendingApprox"] = getTelegramPendingApprox();
    doc["telegramLastCommandText"] = getTelegramLastCommandText();
    doc["telegramLastCommandRole"] = getTelegramLastCommandRole();
    doc["telegramLastCommandResult"] = getTelegramLastCommandResult();
    doc["telegramLastCommandAgeMs"] = (lastTgCommandMs > 0) ? (nowMs - lastTgCommandMs) : -1;
    doc["telegramLastErrorAgeMs"] = (lastTgErrorMs > 0) ? (nowMs - lastTgErrorMs) : -1;

    _sendJSON(request, 200, doc);
}

void WebServer::_handleAPIUnlock(AsyncWebServerRequest* request) {
    if (!_requireApiAuth(request)) {
        return;
    }

    Serial.println("[API] POST /api/unlock - Emergency override");

    const unsigned long guestLockRemainingMs = getTemporaryGuestCodeRemainingMs();
    if (guestLockRemainingMs > 0) {
        _addLogEntry("Admin (Web)", "Emergency Override Blocked (Guest Code Active)", "fail");

        JsonDocument doc;
        doc["success"] = false;
        doc["errorCode"] = "GUEST_CODE_ACTIVE";
        doc["message"] = "Emergency override is temporarily unavailable while a guest code is active";
        doc["retryAfterMs"] = guestLockRemainingMs;
        doc["retryAfterSec"] = (guestLockRemainingMs + 999) / 1000;
        doc["guestCodeRemainingMs"] = guestLockRemainingMs;
        _sendJSON(request, 429, doc);
        return;
    }

    const unsigned long retryAfterMs = _remainingCooldownMs(_lastEmergencyUnlockMs, EMERGENCY_COOLDOWN_MS);
    if (retryAfterMs > 0) {
        _addLogEntry("Admin (Web)", "Emergency Override Cooldown", "fail");

        JsonDocument doc;
        doc["success"] = false;
        doc["errorCode"] = "EMERGENCY_COOLDOWN";
        doc["message"] = "Emergency override is cooling down";
        doc["retryAfterMs"] = retryAfterMs;
        doc["retryAfterSec"] = (retryAfterMs + 999) / 1000;
        doc["cooldownMs"] = EMERGENCY_COOLDOWN_MS;
        _sendJSON(request, 429, doc);
        return;
    }

    _lock->unlock();
    _auth->startRFIDCooldown();

    if (_security->isAlarming()) {
        _security->clearAlarm();
    }

    _security->beep(1);

    _lastEmergencyUnlockMs = millis();
    _addLogEntry("Admin (Web)", "Emergency Override", "success");

    JsonDocument doc;
    doc["success"] = true;
    doc["message"] = "Emergency unlock activated";
    doc["timestamp"] = millis();
    doc["cooldownMs"] = EMERGENCY_COOLDOWN_MS;
    doc["autoLockDelayMs"] = _lock->getAutoLockDelayMs();
    doc["unlockRemainingMs"] = _lock->getRemainingAutoLockMs();

    _sendJSON(request, 200, doc);
}

void WebServer::_handleAPIGuestCode(AsyncWebServerRequest* request) {
    if (!_requireApiAuth(request)) {
        return;
    }

    Serial.println("[API] POST /api/guest-code");

    _addLogEntry("Admin (Web)", "Guest Code API Disabled", "fail");

    JsonDocument doc;
    doc["success"] = false;
    doc["message"] = "Guest code generation is managed via Telegram command /guest_code";
    _sendJSON(request, 403, doc);
}
