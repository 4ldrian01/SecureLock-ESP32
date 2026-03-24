#include "WebServer.h"
#include <esp_system.h>

bool WebServer::_secureEquals(const String& a, const String& b) const {
    const size_t aLen = a.length();
    const size_t bLen = b.length();
    const size_t maxLen = (aLen > bLen) ? aLen : bLen;

    uint8_t diff = static_cast<uint8_t>(aLen ^ bLen);
    for (size_t i = 0; i < maxLen; i++) {
        const char ca = (i < aLen) ? a.charAt(i) : '\0';
        const char cb = (i < bLen) ? b.charAt(i) : '\0';
        diff |= static_cast<uint8_t>(ca ^ cb);
    }

    return diff == 0;
}

String WebServer::_extractBearerToken(AsyncWebServerRequest* request) const {
    if (!request->hasHeader("Authorization")) {
        return "";
    }

    String header = request->header("Authorization");
    header.trim();
    if (header.length() < 8) {
        return "";
    }

    if (!header.startsWith("Bearer ")) {
        return "";
    }

    String token = header.substring(7);
    token.trim();
    return token;
}

String WebServer::_generateApiSessionToken() const {
    String token;
    token.reserve(64);

    for (int i = 0; i < 32; i++) {
        const uint8_t value = static_cast<uint8_t>(esp_random() & 0xFF);
        char hex[3];
        snprintf(hex, sizeof(hex), "%02x", value);
        token += hex;
    }

    return token;
}

void WebServer::_invalidateApiSession() {
    _apiSessionToken = "";
    _apiSessionIssuedAtMs = 0;
    _apiSessionExpiresAtMs = 0;
}

bool WebServer::_isApiSessionValid() const {
    if (_apiSessionToken.length() == 0 || _apiSessionExpiresAtMs == 0) {
        return false;
    }

    return millis() < _apiSessionExpiresAtMs;
}

bool WebServer::_requireApiAuth(AsyncWebServerRequest* request) {
    if (!_isApiSessionValid()) {
        _invalidateApiSession();

        JsonDocument doc;
        doc["success"] = false;
        doc["authenticated"] = false;
        doc["message"] = "Authentication required";
        _sendJSON(request, 401, doc);
        return false;
    }

    const String token = _extractBearerToken(request);
    if (token.length() == 0 || !_secureEquals(token, _apiSessionToken)) {
        JsonDocument doc;
        doc["success"] = false;
        doc["authenticated"] = false;
        doc["message"] = "Invalid or missing API token";
        _sendJSON(request, 401, doc);
        return false;
    }

    // Sliding session expiry while actively used by a valid admin client.
    _apiSessionExpiresAtMs = millis() + API_SESSION_TTL_MS;
    return true;
}
