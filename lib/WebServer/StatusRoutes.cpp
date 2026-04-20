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
extern int getTelegramNotificationQueueDepth();
extern int getTelegramNotificationQueueCapacity();
extern unsigned long getTelegramNotificationQueueOldestAgeMs();
extern unsigned long getTelegramNotificationQueuedTotal();
extern unsigned long getTelegramNotificationDeliveredTotal();
extern unsigned long getTelegramNotificationDeliveryFailures();
extern unsigned long getTelegramNotificationDroppedFullTotal();
extern unsigned long getTelegramNotificationDroppedRetryTotal();
extern bool requestWebGuestCode(String* issuedCode, unsigned long* remainingMs, bool* reusedExisting, bool* blockedByCooldown);

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
    doc["autoLockActive"] = _lock->isAutoLockActive();
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

    const wifi_mode_t wifiMode = WiFi.getMode();
    const bool fallbackApActive = (wifiMode == WIFI_AP || wifiMode == WIFI_AP_STA)
        && WiFi.softAPSSID().length() > 0;
    doc["fallbackApActive"] = fallbackApActive;
    doc["fallbackApSSID"] = fallbackApActive ? WiFi.softAPSSID() : "";
    doc["fallbackApIP"] = fallbackApActive ? WiFi.softAPIP().toString() : "";

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
    doc["telegramNotifyQueueDepth"] = getTelegramNotificationQueueDepth();
    doc["telegramNotifyQueueCapacity"] = getTelegramNotificationQueueCapacity();
    doc["telegramNotifyQueueOldestAgeMs"] = getTelegramNotificationQueueOldestAgeMs();
    doc["telegramNotifyQueuedTotal"] = getTelegramNotificationQueuedTotal();
    doc["telegramNotifyDeliveredTotal"] = getTelegramNotificationDeliveredTotal();
    doc["telegramNotifyDeliveryFailures"] = getTelegramNotificationDeliveryFailures();
    doc["telegramNotifyDroppedFullTotal"] = getTelegramNotificationDroppedFullTotal();
    doc["telegramNotifyDroppedRetryTotal"] = getTelegramNotificationDroppedRetryTotal();
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
        _addLogEntry(_activeApiActorLabel(), "Emergency Override Blocked (Guest Code Active)", "fail");

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
        _addLogEntry(_activeApiActorLabel(), "Emergency Override Cooldown", "fail");

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

    _security->beepAccepted();

    _lastEmergencyUnlockMs = millis();
    _addLogEntry(_activeApiActorLabel(), "Emergency Override", "success");

    JsonDocument doc;
    doc["success"] = true;
    doc["message"] = "Emergency unlock activated";
    doc["timestamp"] = millis();
    doc["cooldownMs"] = EMERGENCY_COOLDOWN_MS;
    doc["autoLockDelayMs"] = _lock->getAutoLockDelayMs();
    doc["unlockRemainingMs"] = _lock->getRemainingAutoLockMs();
    doc["autoLockActive"] = _lock->isAutoLockActive();

    _sendJSON(request, 200, doc);
}

void WebServer::_handleAPIGuestCode(AsyncWebServerRequest* request) {
    if (!_requireApiAuth(request)) {
        return;
    }

    Serial.println("[API] POST /api/guest-code");

    String issuedCode = "";
    unsigned long remainingMs = 0;
    bool reusedExisting = false;
    bool blockedByCooldown = false;
    const bool generated = requestWebGuestCode(&issuedCode, &remainingMs, &reusedExisting, &blockedByCooldown);

    if (!generated && blockedByCooldown) {
        _addLogEntry(_activeApiActorLabel(), "Guest PIN Cooldown", "fail");

        JsonDocument doc;
        doc["success"] = false;
        doc["errorCode"] = "GUEST_CODE_COOLDOWN";
        doc["message"] = "Guest code generation is cooling down";
        doc["retryAfterMs"] = remainingMs;
        doc["retryAfterSec"] = (remainingMs + 999) / 1000;
        doc["cooldownRemainingMs"] = remainingMs;
        _sendJSON(request, 429, doc);
        return;
    }

    if (!generated || issuedCode.length() != 4) {
        _addLogEntry(_activeApiActorLabel(), "Guest PIN Generation", "fail");

        JsonDocument doc;
        doc["success"] = false;
        doc["message"] = "Unable to generate guest code";
        _sendJSON(request, 500, doc);
        return;
    }

    _addLogEntry(_activeApiActorLabel(), reusedExisting ? "Guest PIN Reused" : "Guest PIN Generated", "success");

    JsonDocument doc;
    doc["success"] = true;
    doc["guestCode"] = issuedCode;
    doc["guestCodeRemainingMs"] = remainingMs;
    doc["expiresInMs"] = remainingMs;
    doc["cooldownRemainingMs"] = getGuestCodeCommandCooldownRemainingMs();
    doc["reused"] = reusedExisting;
    doc["message"] = reusedExisting
        ? "Active guest code returned"
        : "Guest code generated";
    _sendJSON(request, 200, doc);
}
