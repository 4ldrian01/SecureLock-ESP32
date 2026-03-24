#ifndef APP_WIFI_CONTROLLER_H
#define APP_WIFI_CONTROLLER_H

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>

class WiFiController {
public:
    struct Credential {
        const char* ssid;
        const char* password;
    };

    WiFiController();

    void begin(const Credential* credentials, size_t count, const char* hostname = "securelock");
    void update();

    bool isConnected() const;
    String connectedSSID() const;

private:
    WiFiMulti _wifiMulti;
    bool _started;
    bool _connected;
    unsigned long _lastMaintenanceMs;

    static const unsigned long MAINTENANCE_INTERVAL_MS = 750;
};

#endif // APP_WIFI_CONTROLLER_H
