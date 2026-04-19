#include "WiFiController.h"

WiFiController::WiFiController()
    : _started(false),
      _connected(false),
      _lastMaintenanceMs(0) {
}

void WiFiController::begin(const Credential* credentials, size_t count, const char* hostname) {
    WiFi.mode(WIFI_STA);
    if (hostname && hostname[0] != '\0') {
        WiFi.setHostname(hostname);
    }

    WiFi.setAutoReconnect(true);
    WiFi.persistent(false);
    WiFi.setSleep(false);

    for (size_t i = 0; i < count; i++) {
        const Credential& c = credentials[i];
        if (c.ssid == nullptr || c.ssid[0] == '\0') {
            continue;
        }

        _wifiMulti.addAP(c.ssid, c.password ? c.password : "");
    }

    _started = true;
    _lastMaintenanceMs = 0;

    Serial.println("[WIFI] WiFiController initialized (non-blocking)");
}

void WiFiController::update() {
    if (!_started) {
        return;
    }

    const unsigned long now = millis();
    if ((now - _lastMaintenanceMs) < MAINTENANCE_INTERVAL_MS) {
        return;
    }

    _lastMaintenanceMs = now;

    const wl_status_t status = static_cast<wl_status_t>(_wifiMulti.run());
    const bool connectedNow = (status == WL_CONNECTED);

    if (connectedNow && !_connected) {
        Serial.println("[WIFI] Connected to known AP");
        Serial.print("[WIFI] SSID: ");
        Serial.println(WiFi.SSID());
        Serial.print("[WIFI] IP: ");
        Serial.println(WiFi.localIP());

        Serial.print("[WIFI] Gateway: ");
        Serial.println(WiFi.gatewayIP());
        Serial.print("[WIFI] DNS1 (DHCP): ");
        Serial.println(WiFi.dnsIP(0));
        Serial.print("[WIFI] DNS2 (DHCP): ");
        Serial.println(WiFi.dnsIP(1));

        const IPAddress fallbackDns1(8, 8, 8, 8);
        const IPAddress fallbackDns2(1, 1, 1, 1);
        const IPAddress noAddress(0, 0, 0, 0);
        if (WiFi.config(noAddress, noAddress, noAddress, fallbackDns1, fallbackDns2)) {
            Serial.println("[WIFI] DNS override applied: 8.8.8.8 / 1.1.1.1");
        } else {
            Serial.println("[WIFI][WARN] DNS override failed");
        }

        Serial.print("[WIFI] DNS1 (active): ");
        Serial.println(WiFi.dnsIP(0));
        Serial.print("[WIFI] DNS2 (active): ");
        Serial.println(WiFi.dnsIP(1));
    } else if (!connectedNow && _connected) {
        Serial.println("[WIFI][WARN] Link lost. Reconnecting in background...");
    }

    _connected = connectedNow;
}

bool WiFiController::isConnected() const {
    return _connected;
}

String WiFiController::connectedSSID() const {
    return _connected ? WiFi.SSID() : "";
}
