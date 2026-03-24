#include "StorageService.h"
#include <LittleFS.h>

StorageService::StorageService()
    : _mounted(false),
      _lastAttemptMs(0) {
}

StorageService& StorageService::instance() {
    static StorageService service;
    return service;
}

bool StorageService::ensureMounted() {
    if (_mounted) {
        return true;
    }

    const unsigned long now = millis();
    if (_lastAttemptMs > 0 && (now - _lastAttemptMs) < RETRY_INTERVAL_MS) {
        return false;
    }

    _lastAttemptMs = now;

    // Critical: never auto-format here.
    _mounted = LittleFS.begin(false);
    if (_mounted) {
        Serial.println("[STORAGE] LittleFS mounted (no auto-format)");
    } else {
        Serial.println("[STORAGE][WARN] LittleFS mount failed (auto-format disabled)");
    }

    return _mounted;
}

bool StorageService::isMounted() const {
    return _mounted;
}
