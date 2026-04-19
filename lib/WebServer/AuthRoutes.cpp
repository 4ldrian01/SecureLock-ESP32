#include "WebServer.h"
#include "WebServerRouteLimits.h"
#include "secrets.h"

#ifndef DASHBOARD_ADMIN_USERNAME
#define DASHBOARD_ADMIN_USERNAME "admin"
#endif

#ifndef DASHBOARD_ADMIN_PASSWORD
#define DASHBOARD_ADMIN_PASSWORD "CHANGE_ME_NOW"
#endif

#ifndef DASHBOARD_ADMIN_1_USERNAME
#define DASHBOARD_ADMIN_1_USERNAME DASHBOARD_ADMIN_USERNAME
#endif

#ifndef DASHBOARD_ADMIN_1_PASSWORD
#define DASHBOARD_ADMIN_1_PASSWORD DASHBOARD_ADMIN_PASSWORD
#endif

#ifndef DASHBOARD_ADMIN_2_USERNAME
#define DASHBOARD_ADMIN_2_USERNAME ""
#endif

#ifndef DASHBOARD_ADMIN_2_PASSWORD
#define DASHBOARD_ADMIN_2_PASSWORD ""
#endif

void WebServer::_handleAPIAuthLogin(AsyncWebServerRequest* request, uint8_t* data, size_t len) {
    if (len > webserver_limits::MAX_AUTH_PAYLOAD_BYTES) {
        JsonDocument doc;
        doc["success"] = false;
        doc["message"] = "Request body too large";
        _sendJSON(request, 413, doc);
        return;
    }

    JsonDocument body;
    const DeserializationError err = deserializeJson(body, data, len);

    if (err) {
        JsonDocument doc;
        doc["success"] = false;
        doc["message"] = "Invalid JSON body";
        _sendJSON(request, 400, doc);
        return;
    }

    const unsigned long nowMs = millis();
    if (_authLockoutUntilMs > nowMs) {
        const unsigned long retryAfterMs = _authLockoutUntilMs - nowMs;
        JsonDocument doc;
        doc["success"] = false;
        doc["authenticated"] = false;
        doc["message"] = "Too many failed login attempts. Try again later.";
        doc["retryAfterMs"] = retryAfterMs;
        doc["retryAfterSec"] = (retryAfterMs + 999) / 1000;
        _sendJSON(request, 429, doc);
        return;
    }

    String username = body["username"] | "";
    String password = body["password"] | "";
    username.trim();

    if (username.length() == 0 || password.length() == 0
        || username.length() > webserver_limits::MAX_ADMIN_CREDENTIAL_LENGTH
        || password.length() > webserver_limits::MAX_ADMIN_CREDENTIAL_LENGTH) {
        JsonDocument doc;
        doc["success"] = false;
        doc["authenticated"] = false;
        doc["message"] = "Invalid credential format";
        _sendJSON(request, 400, doc);
        return;
    }

    String expectedUsernames[2] = {
        String(DASHBOARD_ADMIN_1_USERNAME),
        String(DASHBOARD_ADMIN_2_USERNAME)
    };
    String expectedPasswords[2] = {
        String(DASHBOARD_ADMIN_1_PASSWORD),
        String(DASHBOARD_ADMIN_2_PASSWORD)
    };

    bool hasConfiguredCredential = false;
    for (int i = 0; i < 2; i++) {
        expectedUsernames[i].trim();
        expectedPasswords[i].trim();

        const bool configured = expectedUsernames[i].length() > 0
            && expectedPasswords[i].length() > 0
            && !_secureEquals(expectedPasswords[i], String("CHANGE_ME_NOW"));
        if (configured) {
            hasConfiguredCredential = true;
        }
    }

    if (!hasConfiguredCredential) {
        JsonDocument doc;
        doc["success"] = false;
        doc["authenticated"] = false;
        doc["message"] = "Dashboard admin credentials are not configured. Set DASHBOARD_ADMIN_1_* (and optionally DASHBOARD_ADMIN_2_*) in include/secrets.h";
        _sendJSON(request, 503, doc);
        return;
    }

    int matchedAdminSlot = 0;
    String matchedAdminUsername = "";
    for (int i = 0; i < 2; i++) {
        const bool configured = expectedUsernames[i].length() > 0
            && expectedPasswords[i].length() > 0
            && !_secureEquals(expectedPasswords[i], String("CHANGE_ME_NOW"));

        if (!configured) {
            continue;
        }

        if (_secureEquals(username, expectedUsernames[i])
            && _secureEquals(password, expectedPasswords[i])) {
            matchedAdminSlot = i + 1;
            matchedAdminUsername = expectedUsernames[i];
            break;
        }
    }

    const bool authSuccess = matchedAdminSlot > 0;

    if (!authSuccess) {
        _authFailedAttempts++;
        int attemptsRemaining = AUTH_MAX_FAILED_ATTEMPTS - _authFailedAttempts;
        if (attemptsRemaining < 0) {
            attemptsRemaining = 0;
        }

        if (_authFailedAttempts >= AUTH_MAX_FAILED_ATTEMPTS) {
            _authFailedAttempts = 0;
            _authLockoutUntilMs = nowMs + AUTH_LOCKOUT_MS;

            JsonDocument doc;
            doc["success"] = false;
            doc["authenticated"] = false;
            doc["message"] = "Too many failed login attempts. Login temporarily locked.";
            doc["retryAfterMs"] = AUTH_LOCKOUT_MS;
            doc["retryAfterSec"] = AUTH_LOCKOUT_MS / 1000;
            _sendJSON(request, 429, doc);
            _addLogEntry("Admin (Web)", "API Auth Login", "fail");
            return;
        }

        JsonDocument doc;
        doc["success"] = false;
        doc["authenticated"] = false;
        doc["message"] = "Invalid username or password";
        doc["attemptsRemaining"] = attemptsRemaining;
        _sendJSON(request, 401, doc);
        _addLogEntry("Admin (Web)", "API Auth Login", "fail");
        return;
    }

    _authFailedAttempts = 0;
    _authLockoutUntilMs = 0;

    _apiSessionToken = _generateApiSessionToken();
    _apiSessionIssuedAtMs = nowMs;
    _apiSessionExpiresAtMs = nowMs + API_SESSION_TTL_MS;
    _apiSessionAdminSlot = matchedAdminSlot;
    _apiSessionAdminUsername = matchedAdminUsername;
    _apiSessionActorLabel = "Admin " + String(matchedAdminSlot) + " (Web)";

    JsonDocument doc;
    doc["success"] = true;
    doc["authenticated"] = true;
    doc["token"] = _apiSessionToken;
    doc["expiresInMs"] = API_SESSION_TTL_MS;
    doc["adminSlot"] = _activeApiAdminSlot();
    doc["adminLabel"] = _activeApiActorLabel();
    doc["adminUsername"] = _activeApiAdminUsername();
    _sendJSON(request, 200, doc);
    _addLogEntry(_activeApiActorLabel(), "API Auth Login", "success");
}

void WebServer::_handleAPIAuthLogout(AsyncWebServerRequest* request) {
    if (!_requireApiAuth(request)) {
        return;
    }

    const String actor = _activeApiActorLabel();
    _invalidateApiSession();

    JsonDocument doc;
    doc["success"] = true;
    doc["authenticated"] = false;
    doc["message"] = "Logged out";
    _sendJSON(request, 200, doc);
    _addLogEntry(actor, "API Auth Logout", "success");
}

void WebServer::_handleAPIAuthStatus(AsyncWebServerRequest* request) {
    const String presented = _extractBearerToken(request);
    const bool sessionValid = _isApiSessionValid();
    const bool authenticated = sessionValid
        && presented.length() > 0
        && _secureEquals(presented, _apiSessionToken);

    if (authenticated) {
        _apiSessionExpiresAtMs = millis() + API_SESSION_TTL_MS;
    }

    unsigned long expiresInMs = 0;
    if (authenticated && _apiSessionExpiresAtMs > millis()) {
        expiresInMs = _apiSessionExpiresAtMs - millis();
    }

    JsonDocument doc;
    doc["success"] = true;
    doc["authenticated"] = authenticated;
    doc["expiresInMs"] = expiresInMs;
    if (authenticated) {
        doc["adminSlot"] = _activeApiAdminSlot();
        doc["adminLabel"] = _activeApiActorLabel();
        doc["adminUsername"] = _activeApiAdminUsername();
    }
    _sendJSON(request, 200, doc);
}
