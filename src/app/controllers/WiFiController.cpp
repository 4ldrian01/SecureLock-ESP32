#include "WiFiController.h"

#ifndef WIFI_FALLBACK_AP_ENABLE
#define WIFI_FALLBACK_AP_ENABLE 1
#endif

#ifndef WIFI_FALLBACK_AP_SSID
#define WIFI_FALLBACK_AP_SSID "SecureLock-Setup"
#endif

#ifndef WIFI_FALLBACK_AP_PASSWORD
#define WIFI_FALLBACK_AP_PASSWORD "securelock1234"
#endif

#ifndef WIFI_FALLBACK_AP_MAX_CLIENTS
#define WIFI_FALLBACK_AP_MAX_CLIENTS 4
#endif

#ifndef WIFI_FORCE_PUBLIC_DNS
#define WIFI_FORCE_PUBLIC_DNS 0
#endif

namespace {
String buildFallbackApSsid() {
    String baseSsid = String(WIFI_FALLBACK_AP_SSID);
    baseSsid.trim();

    if (baseSsid.length() == 0) {
        baseSsid = "SecureLock-Setup";
    }

    if (baseSsid.length() > 20) {
        baseSsid = baseSsid.substring(0, 20);
        baseSsid.trim();
    }

    const uint32_t chipSuffix = static_cast<uint32_t>(ESP.getEfuseMac() & 0xFFFFFFULL);
    char suffix[7] = {0};
    snprintf(suffix, sizeof(suffix), "%06X", chipSuffix);

    return baseSsid + "-" + String(suffix);
}
}

WiFiController::WiFiController()
    : _started(false),
      _connected(false),
      _fallbackApActive(false),
      _fallbackApSsid(""),
      _fallbackApIpAddress(""),
      _lastMaintenanceMs(0) {
}

void WiFiController::begin(const Credential* credentials, size_t count, const char* hostname) {
    WiFi.mode(WIFI_AP_STA);
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

#if WIFI_FALLBACK_AP_ENABLE
    const String fallbackApSsid = buildFallbackApSsid();
    String fallbackApPassword = String(WIFI_FALLBACK_AP_PASSWORD);
    fallbackApPassword.trim();

    const bool useSecuredAp = fallbackApPassword.length() >= 8 && fallbackApPassword.length() <= 63;
    bool apStarted = false;

    if (useSecuredAp) {
        apStarted = WiFi.softAP(
            fallbackApSsid.c_str(),
            fallbackApPassword.c_str(),
            0,
            false,
            WIFI_FALLBACK_AP_MAX_CLIENTS
        );
    } else {
        apStarted = WiFi.softAP(
            fallbackApSsid.c_str(),
            nullptr,
            0,
            false,
            WIFI_FALLBACK_AP_MAX_CLIENTS
        );
    }

    _fallbackApActive = apStarted;
    if (_fallbackApActive) {
        _fallbackApSsid = WiFi.softAPSSID();
        _fallbackApIpAddress = WiFi.softAPIP().toString();

        Serial.println("[WIFI] Fallback AP started");
        Serial.print("[WIFI] AP SSID: ");
        Serial.println(_fallbackApSsid);
        Serial.print("[WIFI] AP URL: http://");
        Serial.print(_fallbackApIpAddress);
        Serial.println("/");
    } else {
        _fallbackApSsid = "";
        _fallbackApIpAddress = "";
        Serial.println("[WIFI][WARN] Fallback AP failed to start");
    }
#else
    _fallbackApActive = false;
    _fallbackApSsid = "";
    _fallbackApIpAddress = "";
#endif

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

        const IPAddress noAddress(0, 0, 0, 0);
        const IPAddress dhcpDns1 = WiFi.dnsIP(0);
        const bool missingDhcpDns = (dhcpDns1 == noAddress);

#if WIFI_FORCE_PUBLIC_DNS
        const IPAddress forcedDns1(8, 8, 8, 8);
        const IPAddress forcedDns2(1, 1, 1, 1);
        if (WiFi.config(noAddress, noAddress, noAddress, forcedDns1, forcedDns2)) {
            Serial.println("[WIFI] DNS override applied: 8.8.8.8 / 1.1.1.1");
        } else {
            Serial.println("[WIFI][WARN] DNS override failed");
        }
#else
        if (missingDhcpDns) {
            const IPAddress fallbackDns1(8, 8, 8, 8);
            const IPAddress fallbackDns2(1, 1, 1, 1);
            if (WiFi.config(noAddress, noAddress, noAddress, fallbackDns1, fallbackDns2)) {
                Serial.println("[WIFI][WARN] DHCP DNS missing. Applied fallback DNS: 8.8.8.8 / 1.1.1.1");
            } else {
                Serial.println("[WIFI][WARN] DHCP DNS missing and fallback DNS apply failed");
            }
        } else {
            Serial.println("[WIFI] Using DHCP-provided DNS servers");
        }
#endif

        if (WiFi.dnsIP(0) == noAddress) {
            Serial.println("[WIFI][WARN] DNS1 is still unset; Telegram/API DNS lookups may fail");
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

bool WiFiController::isFallbackApActive() const {
    return _fallbackApActive;
}

String WiFiController::fallbackApSSID() const {
    return _fallbackApSsid;
}

String WiFiController::fallbackApIPAddress() const {
    return _fallbackApIpAddress;
}
