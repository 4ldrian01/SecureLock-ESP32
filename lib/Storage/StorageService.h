#ifndef STORAGE_SERVICE_H
#define STORAGE_SERVICE_H

#include <Arduino.h>

class StorageService {
public:
    static StorageService& instance();

    // Mount LittleFS without auto-format. Returns true when mounted.
    bool ensureMounted();
    bool isMounted() const;

private:
    StorageService();

    bool _mounted;
    unsigned long _lastAttemptMs;

    static const unsigned long RETRY_INTERVAL_MS = 1500;
};

#endif // STORAGE_SERVICE_H
