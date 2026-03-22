/**
 * ============================================================
 * WebServer - Implementation
 * ============================================================
 */

#include "WebServer.h"
#include <time.h>
#include <ctype.h>

// Shared guest-code state provided by main authentication loop (Telegram path)
extern String getActiveGuestCode();
extern bool isTemporaryGuestCodeActive();
extern unsigned long getTemporaryGuestCodeRemainingMs();
extern String getAuthPrompt();
extern bool isPendingAccessActive();
extern String getLastKeypadKeyLabel();
extern unsigned long getLastKeypadKeyMs();
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

namespace {
String normalizeUID(const String& input) {
    String uid = input;
    uid.trim();
    uid.toUpperCase();
    uid.replace(" ", "");
    uid.replace(":", "");
    uid.replace("-", "");
    return uid;
}

bool isAlphabeticName(const String& input) {
    String name = input;
    name.trim();

    if (name.length() == 0) {
        return false;
    }

    bool hasLetter = false;
    for (size_t i = 0; i < name.length(); i++) {
        const char c = name.charAt(i);
        if (c == ' ') {
            continue;
        }

        if (!isalpha(static_cast<unsigned char>(c))) {
            return false;
        }

        hasLetter = true;
    }

    return hasLetter;
}

bool isFourDigitCode(const String& code) {
    if (code.length() != 4) {
        return false;
    }

    for (size_t i = 0; i < code.length(); i++) {
        if (!isDigit(code.charAt(i))) {
            return false;
        }
    }

    return true;
}

bool isValidTelegramChatId(const String& chatId) {
    String value = chatId;
    value.trim();
    if (value.length() == 0) {
        return false;
    }

    size_t start = 0;
    if (value.charAt(0) == '-') {
        if (value.length() == 1) {
            return false;
        }
        start = 1;
    }

    for (size_t i = start; i < value.length(); i++) {
        if (!isDigit(value.charAt(i))) {
            return false;
        }
    }

    return true;
}
}

/**
 * Constructor - Store component references
 */
WebServer::WebServer(LockManager* lockManager, SecurityManager* securityManager, AuthHandler* authHandler)
    : _lock(lockManager),
      _security(securityManager),
      _auth(authHandler),
      _server(80),
      _wifiConnected(false),
      _ipAddress(""),
      _guestCode(""),
            _guestCodeExpiry(0),
            _lastEmergencyUnlockMs(0),
        _lastGuestCodeRequestMs(0)
{
}

/**
 * Initialize WiFi and Web Server
 */
void WebServer::init(const char* ssid, const char* password) {
    Serial.println("[WEB] Initializing Web Server...");
    
    _initFileSystem();
    _cleanupGuestUsers();
    _initWiFi(ssid, password);
    _setupRoutes();
    
    // Start server
    _server.begin();
    Serial.println("[WEB] ✓ Server started on port 80");
    
    if (_wifiConnected) {
        Serial.print("[WEB] 🌐 Dashboard: http://");
        Serial.print(_ipAddress);
        Serial.println("/");
    }
}

void WebServer::update() {
    _expireGuestCodeIfNeeded();
}

/**
 * Check WiFi connection status
 */
bool WebServer::isConnected() const {
    return _wifiConnected;
}

/**
 * Get IP address
 */
String WebServer::getIPAddress() const {
    return _ipAddress;
}

void WebServer::markEmergencyOverride() {
    _lastEmergencyUnlockMs = millis();
}

void WebServer::logActivity(const String& user, const String& method, const String& status) {
    _addLogEntry(user, method, status);
}

// ============================================================
// PRIVATE - Initialization
// ============================================================

/**
 * Initialize WiFi connection
 */
void WebServer::_initWiFi(const char* ssid, const char* password) {
    Serial.print("[WiFi] Connecting to: ");
    Serial.println(ssid);

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.persistent(false);
    WiFi.setSleep(false);

    // Start from a clean station state to avoid stale auth/session issues.
    WiFi.disconnect(true, true);
    const unsigned long disconnectStartMs = millis();
    while ((millis() - disconnectStartMs) < 150) {
        yield();
    }

    static const int kMaxAttempts = 3;
    for (int attempt = 1; attempt <= kMaxAttempts && WiFi.status() != WL_CONNECTED; ++attempt) {
        Serial.print("[WiFi] Attempt ");
        Serial.print(attempt);
        Serial.print("/");
        Serial.println(kMaxAttempts);

        WiFi.begin(ssid, password);

        // Wait for connection (20 second timeout per attempt)
        const unsigned long attemptStartMs = millis();
        unsigned long lastDotMs = 0;
        while (WiFi.status() != WL_CONNECTED && (millis() - attemptStartMs) < 20000) {
            const unsigned long nowMs = millis();
            if (nowMs - lastDotMs >= 500) {
                Serial.print(".");
                lastDotMs = nowMs;
            }
            yield();
        }
        Serial.println();

        if (WiFi.status() == WL_CONNECTED) {
            break;
        }

        Serial.print("[WiFi] Attempt failed. Status code: ");
        Serial.println(static_cast<int>(WiFi.status()));
        WiFi.disconnect(true, true);
        const unsigned long backoffStartMs = millis();
        while ((millis() - backoffStartMs) < 300) {
            yield();
        }
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        _wifiConnected = true;
        _ipAddress = WiFi.localIP().toString();

        // Initialize NTP clock (UTC) for accurate world-time logging
        configTzTime("UTC0", "pool.ntp.org", "time.google.com", "time.nist.gov");

        bool timeReady = false;
        const unsigned long ntpWaitStartMs = millis();
        while ((millis() - ntpWaitStartMs) < 3000) {
            time_t now = time(nullptr);
            if (now > 1700000000) {
                timeReady = true;
                break;
            }
            yield();
        }
        
        Serial.println("[WiFi] ✓ Connected!");
        Serial.print("[WiFi] IP: ");
        Serial.println(_ipAddress);
        Serial.print("[WiFi] RSSI: ");
        Serial.print(WiFi.RSSI());
        Serial.println(" dBm");
        Serial.print("[WiFi] Channel: ");
        Serial.println(WiFi.channel());

        Serial.print("[TIME] NTP sync: ");
        Serial.println(timeReady ? "OK (UTC)" : "PENDING (using fallback until synced)");
    } else {
        _wifiConnected = false;
        Serial.println("[WiFi] ✗ Connection failed");
        Serial.print("[WiFi] Final status code: ");
        Serial.println(static_cast<int>(WiFi.status()));
        Serial.println("[WiFi] Check credentials in secrets.h (exact SSID/password, case-sensitive)");
        Serial.println("[WiFi] Ensure hotspot/router uses 2.4GHz and WPA2 or WPA2/WPA3 mixed mode");
    }
}

/**
 * Initialize LittleFS filesystem
 */
void WebServer::_initFileSystem() {
    Serial.print("[FS] Mounting LittleFS...");
    
    if (!LittleFS.begin(true)) {
        Serial.println(" ✗ FAILED!");
        return;
    }
    
    Serial.println(" ✓ OK");
    
    // List files
    File root = LittleFS.open("/");
    if (root && root.isDirectory()) {
        Serial.println("[FS] Files:");
        File file = root.openNextFile();
        while (file) {
            Serial.print("  - ");
            Serial.print(file.name());
            Serial.print(" (");
            Serial.print(file.size());
            Serial.println(" bytes)");
            file = root.openNextFile();
        }
    }
}

/**
 * Setup all routes
 */
void WebServer::_setupRoutes() {
    Serial.println("[WEB] Setting up routes...");
    
    // ==================== CORS Preflight ====================
    // Handle OPTIONS pre-flight requests for cross-origin API access
    _server.on("/api/*", HTTP_OPTIONS, [this](AsyncWebServerRequest* request) {
        AsyncWebServerResponse* response = request->beginResponse(204);
        _addCORSHeaders(response);
        request->send(response);
    });
    
    // ==================== STATIC FILES ====================
    
    // Root - Serve index.html
    _server.on("/", HTTP_GET, [this](AsyncWebServerRequest* request) {
        _handleRoot(request);
    });
    
    // CSS file
    _server.on("/css/style.css", HTTP_GET, [this](AsyncWebServerRequest* request) {
        _handleCSS(request);
    });

    // Optional pagination styles
    _server.on("/css/pagination.css", HTTP_GET, [](AsyncWebServerRequest* request) {
        if (LittleFS.exists("/css/pagination.css")) {
            request->send(LittleFS, "/css/pagination.css", "text/css");
            return;
        }

        request->send(404, "text/plain", "CSS not found");
    });
    
    // Legacy JavaScript file (compatibility shim to modular /js/main.js)
    _server.on("/js/script.js", HTTP_GET, [this](AsyncWebServerRequest* request) {
        _handleJS(request);
    });
    
    // ==================== API ENDPOINTS ====================
    
    // Get system status
    _server.on("/api/status", HTTP_GET, [this](AsyncWebServerRequest* request) {
        _handleAPIStatus(request);
    });
    
    // Remote unlock
    _server.on("/api/unlock", HTTP_POST, [this](AsyncWebServerRequest* request) {
        _handleAPIUnlock(request);
    });
    
    // Generate guest code
    _server.on("/api/guest-code", HTTP_POST, [this](AsyncWebServerRequest* request) {
        _handleAPIGuestCode(request);
    });
    
    // List users (GET)
    _server.on("/api/users", HTTP_GET, [this](AsyncWebServerRequest* request) {
        _handleAPIUsers(request);
    });
    
    // Add user (POST) - body handler
    _server.on("/api/users", HTTP_POST,
        [this](AsyncWebServerRequest* request) { /* handled in body callback */ },
        NULL,
        [this](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
            _handleAPIAddUser(request, data, len);
        }
    );

    // Alias: Add user using explicit endpoint name expected by some clients.
    _server.on("/api/addUser", HTTP_POST,
        [this](AsyncWebServerRequest* request) { /* handled in body callback */ },
        NULL,
        [this](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
            _handleAPIAddUser(request, data, len);
        }
    );
    
    // Edit user (PUT) - body handler
    _server.on("/api/users", HTTP_PUT,
        [this](AsyncWebServerRequest* request) { /* handled in body callback */ },
        NULL,
        [this](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
            _handleAPIEditUser(request, data, len);
        }
    );
    
    // Delete user (DELETE)
    _server.on("/api/users", HTTP_DELETE, [this](AsyncWebServerRequest* request) {
        _handleAPIDeleteUser(request);
    });
    
    // Get activity logs
    _server.on("/api/logs", HTTP_GET, [this](AsyncWebServerRequest* request) {
        _handleAPILogs(request);
    });

    // Clear activity logs
    _server.on("/api/logs", HTTP_DELETE, [this](AsyncWebServerRequest* request) {
        _handleAPIClearLogs(request);
    });
    
    // RFID scan polling
    _server.on("/api/rfid/scan", HTTP_GET, [this](AsyncWebServerRequest* request) {
        _handleAPIRfidScan(request);
    });

    _server.on("/api/diagnostics", HTTP_GET, [this](AsyncWebServerRequest* request) {
        _handleAPIDiagnostics(request);
    });
    
    // 404 handler
    _server.onNotFound([this](AsyncWebServerRequest* request) {
        _handleNotFound(request);
    });
    
    Serial.println("[WEB] ✓ Routes configured");
}

// ============================================================
// PRIVATE - Static File Handlers
// ============================================================

/**
 * Handle root path - Serve index.html
 */
void WebServer::_handleRoot(AsyncWebServerRequest* request) {
    const char* candidates[] = {
        "/html/index.html",
        "/index.html"
    };

    for (const char* path : candidates) {
        if (LittleFS.exists(path)) {
            request->send(LittleFS, path, "text/html");
            Serial.print("[WEB] GET / → ");
            Serial.println(path);
            return;
        }
    }

    Serial.println("[WEB] ✗ index.html not found in LittleFS!");
    request->send(404, "text/html",
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>SecureLock Dashboard Missing</title></head><body>"
        "<h2>SecureLock Dashboard files are missing</h2>"
        "<p>Upload LittleFS data first, then reload this page.</p>"
        "<pre>platformio run --target uploadfs</pre>"
        "<p>Expected file: <code>data/html/index.html</code></p>"
        "</body></html>");
}

/**
 * Handle CSS request
 */
void WebServer::_handleCSS(AsyncWebServerRequest* request) {
    if (LittleFS.exists("/css/style.css")) {
        request->send(LittleFS, "/css/style.css", "text/css");
        Serial.println("[WEB] GET /css/style.css → OK");
    } else if (LittleFS.exists("/style.css")) {
        request->send(LittleFS, "/style.css", "text/css");
        Serial.println("[WEB] GET /css/style.css → fallback /style.css");
    } else {
        Serial.println("[WEB] ✗ style.css not found!");
        request->send(404, "text/plain", "CSS not found");
    }
}

/**
 * Handle JavaScript request
 */
void WebServer::_handleJS(AsyncWebServerRequest* request) {
    if (LittleFS.exists("/js/script.js")) {
        request->send(LittleFS, "/js/script.js", "application/javascript");
        Serial.println("[WEB] GET /js/script.js → OK");
    } else if (LittleFS.exists("/script.js")) {
        request->send(LittleFS, "/script.js", "application/javascript");
        Serial.println("[WEB] GET /js/script.js → fallback /script.js");
    } else {
        Serial.println("[WEB] ✗ script.js not found!");
        request->send(404, "text/plain", "JavaScript not found");
    }
}

/**
 * Handle 404 errors
 */
void WebServer::_handleNotFound(AsyncWebServerRequest* request) {
    const String url = request->url();

    if ((url.startsWith("/js/") || url.startsWith("/css/")) && LittleFS.exists(url)) {
        request->send(LittleFS, url, _getMimeType(url));
        Serial.print("[WEB] Static fallback served: ");
        Serial.println(url);
        return;
    }

    Serial.print("[WEB] 404: ");
    Serial.println(url);
    
    request->send(404, "text/plain", "404 - Not Found");
}

// ============================================================
// PRIVATE - API Handlers
// ============================================================

/**
 * API: GET /api/status
 * Return system status as JSON
 */
void WebServer::_handleAPIStatus(AsyncWebServerRequest* request) {
    _expireGuestCodeIfNeeded();

    JsonDocument doc;
    
    // Lock status
    doc["locked"] = _lock->isLocked();
    doc["doorOpen"] = _lock->isDoorOpen();
    doc["tampered"] = _lock->isDoorTampered();
    doc["autoLockDelayMs"] = _lock->getAutoLockDelayMs();
    doc["unlockRemainingMs"] = _lock->getRemainingAutoLockMs();
    doc["emergencyCooldownRemainingMs"] = _remainingCooldownMs(_lastEmergencyUnlockMs, EMERGENCY_COOLDOWN_MS);
    doc["guestCodeCooldownRemainingMs"] = _remainingCooldownMs(_lastGuestCodeRequestMs, GUEST_CODE_COOLDOWN_MS);
    
    // Security status
    doc["alarm"] = _security->isAlarming();
    doc["vibration"] = _security->isVibrationLatched();
    doc["buzzerActive"] = _security->isBuzzerActive();
    doc["sirenActive"] = _security->isSirenActive();
    
    // System info
    doc["uptime"] = millis() / 1000;
    doc["freeHeap"] = ESP.getFreeHeap();
    doc["wifiConnected"] = _wifiConnected;
    doc["ipAddress"] = _ipAddress;
    doc["rssi"] = WiFi.RSSI();

    // Temporary guest code status (source of truth: Telegram/main.cpp flow)
    const bool telegramGuestActive = isTemporaryGuestCodeActive();
    const String telegramGuestCode = getActiveGuestCode();
    const unsigned long telegramGuestRemainingMs = getTemporaryGuestCodeRemainingMs();

    doc["guestCodeActive"] = telegramGuestActive;
    doc["guestCode"] = telegramGuestActive ? telegramGuestCode : "";
    doc["guestCodeRemainingMs"] = telegramGuestRemainingMs;
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

/**
 * API: POST /api/unlock
 * Remote unlock command
 */
void WebServer::_handleAPIUnlock(AsyncWebServerRequest* request) {
    Serial.println("[API] POST /api/unlock - Emergency override");

    const unsigned long retryAfterMs = _remainingCooldownMs(_lastEmergencyUnlockMs, EMERGENCY_COOLDOWN_MS);
    if (retryAfterMs > 0) {
        _addLogEntry("Admin (Web)", "Emergency Override Cooldown", "fail");

        JsonDocument doc;
        doc["success"] = false;
        doc["message"] = "Emergency override is cooling down";
        doc["retryAfterMs"] = retryAfterMs;
        doc["retryAfterSec"] = (retryAfterMs + 999) / 1000;
        doc["cooldownMs"] = EMERGENCY_COOLDOWN_MS;
        _sendJSON(request, 429, doc);
        return;
    }
    
    // Unlock door
    _lock->unlock();
    _auth->startRFIDCooldown();
    
    // Stop alarm if active
    if (_security->isAlarming()) {
        _security->clearAlarm();
    }

    // Single confirmation beep for emergency override.
    _security->beep(1);

    _lastEmergencyUnlockMs = millis();
    
    // Log the event
    _addLogEntry("Admin (Web)", "Emergency Override", "success");
    
    // Send response
    JsonDocument doc;
    doc["success"] = true;
    doc["message"] = "Emergency unlock activated";
    doc["timestamp"] = millis();
    doc["cooldownMs"] = EMERGENCY_COOLDOWN_MS;
    doc["autoLockDelayMs"] = _lock->getAutoLockDelayMs();
    doc["unlockRemainingMs"] = _lock->getRemainingAutoLockMs();
    
    _sendJSON(request, 200, doc);
}

/**
 * API: POST /api/guest-code
 * Generate temporary guest access code
 */
void WebServer::_handleAPIGuestCode(AsyncWebServerRequest* request) {
    Serial.println("[API] POST /api/guest-code");

    _addLogEntry("Admin (Web)", "Guest Code API Disabled", "fail");

    JsonDocument doc;
    doc["success"] = false;
    doc["message"] = "Guest code generation is managed via Telegram command /guest_code";
    _sendJSON(request, 403, doc);
}

/**
 * API: GET /api/users
 * List all registered users
 */
void WebServer::_handleAPIUsers(AsyncWebServerRequest* request) {
    // Read current users.json (if available) so we can preserve non-auth metadata (e.g., settings).
    JsonDocument currentDoc;
    if (LittleFS.exists("/users.json")) {
        File file = LittleFS.open("/users.json", "r");
        DeserializationError error = deserializeJson(currentDoc, file);
        file.close();

        if (error || !currentDoc.is<JsonObject>()) {
            currentDoc.clear();
        }
    }

    if (!currentDoc["users"].is<JsonArray>()) {
        currentDoc["users"] = JsonArray();
    }

    JsonArray existingUsers = currentDoc["users"].as<JsonArray>();

    // Build authoritative users payload from AuthHandler (NVS), not from users.json.
    JsonDocument responseDoc;
    JsonArray responseUsers = responseDoc["users"].to<JsonArray>();

    for (int i = 0; i < _auth->getUserCount(); i++) {
        String uid = normalizeUID(_auth->getUserUIDAt(i));
        if (uid.isEmpty()) {
            continue;
        }

        // Do not expose temporary guest PIN pseudo-users in User Management.
        if (uid.startsWith("GUEST_")) {
            continue;
        }

        String name = _auth->getUserName(uid);
        if (name == "Unknown" || name.isEmpty()) {
            name = "User";
        }

        String type = "user";

        // Preserve explicit role from existing users.json if one exists.
        for (JsonObject existing : existingUsers) {
            String existingUid = normalizeUID(existing["uid"] | "");
            if (existingUid != uid) {
                continue;
            }

            String existingType = existing["type"] | "";
            existingType.trim();
            existingType.toLowerCase();
            if (!existingType.isEmpty()) {
                type = existingType;
            }
            break;
        }

        JsonObject user = responseUsers.add<JsonObject>();
        user["cardUID"] = uid;
        user["uid"] = uid;
        user["name"] = name;
        user["type"] = type;
        user["telegramChatID"] = _auth->getUserTelegramChatId(uid);
        user["backupPIN"] = _auth->getUserBackupPIN(uid);
    }

    // Keep optional settings section if present.
    if (currentDoc["settings"].is<JsonObject>()) {
        JsonObject currentSettings = currentDoc["settings"].as<JsonObject>();
        JsonObject responseSettings = responseDoc["settings"].to<JsonObject>();
        for (JsonPair kv : currentSettings) {
            responseSettings[kv.key().c_str()] = kv.value();
        }
    }

    // Self-heal users.json whenever it diverges from auth storage
    // (prevents "empty UI but RFID still grants access" drift after uploadfs).
    bool shouldRewriteUsersFile = !LittleFS.exists("/users.json");
    String existingUsersSerialized;
    String responseUsersSerialized;
    serializeJson(existingUsers, existingUsersSerialized);
    serializeJson(responseUsers, responseUsersSerialized);
    if (existingUsersSerialized != responseUsersSerialized) {
        shouldRewriteUsersFile = true;
    }

    if (shouldRewriteUsersFile) {
        File file = LittleFS.open("/users.json", "w");
        if (file) {
            serializeJson(responseDoc, file);
            file.close();
            Serial.println("[API] users.json synchronized from auth storage");
        } else {
            Serial.println("[API][WARN] Failed to rewrite users.json during sync");
        }
    }

    _sendJSON(request, 200, responseDoc);
}

/**
 * API: DELETE /api/users?uid=XXX
 * Delete a user
 */
void WebServer::_handleAPIDeleteUser(AsyncWebServerRequest* request) {
    if (!request->hasParam("uid")) {
        JsonDocument doc;
        doc["success"] = false;
        doc["message"] = "Missing uid parameter";
        _sendJSON(request, 400, doc);
        return;
    }
    
    String uid = normalizeUID(request->getParam("uid")->value());
    Serial.print("[API] DELETE /api/users?uid=");
    Serial.println(uid);
    
    // Remove from AuthHandler (NVS credentials)
    bool removedFromAuth = _auth->removeUser(uid);

    const bool deleted = removedFromAuth;

    if (deleted) {
        _security->beep(1);  // Success tone parity with Add User action
    }

    _addLogEntry("Admin (Web)", "Delete User (" + uid + ")", deleted ? "success" : "fail");

    // Send response
    JsonDocument doc;
    doc["success"] = deleted;
    doc["removedFromAuth"] = removedFromAuth;
    doc["removedFromList"] = removedFromAuth;

    if (deleted) {
        doc["message"] = "User deleted";
        _sendJSON(request, 200, doc);
        return;
    }

    doc["message"] = "User not found";
    _sendJSON(request, 404, doc);
}

/**
 * API: POST /api/users (body: {name, pin, uid, type})
 * Add a new user
 */
void WebServer::_handleAPIAddUser(AsyncWebServerRequest* request, uint8_t* data, size_t len) {
    Serial.println("[API] POST /api/users - Add user");
    
    JsonDocument body;
    DeserializationError err = deserializeJson(body, data, len);
    
    if (err) {
        JsonDocument doc;
        doc["success"] = false;
        doc["message"] = "Invalid JSON body";
        _sendJSON(request, 400, doc);
        return;
    }
    
    String name = body["name"] | "";
    String pin  = body["pin"]  | "";
    String uid  = body["cardUID"] | "";
    if (uid.isEmpty()) {
        uid = body["uid"] | "";
    }
    String type = body["type"] | "user";
    String telegramChatID = body["telegramChatID"] | "";
    if (telegramChatID.isEmpty()) {
        telegramChatID = body["chat_id"] | "";
    }
    String backupPIN = body["backupPIN"] | "";
    if (backupPIN.isEmpty()) {
        backupPIN = body["backup_pin"] | "";
    }

    name.trim();
    uid = normalizeUID(uid);
    telegramChatID.trim();
    backupPIN.trim();
    
    if (pin.isEmpty() && !backupPIN.isEmpty()) {
        pin = backupPIN;
    }

    if (name.isEmpty() || pin.isEmpty() || uid.isEmpty()) {
        JsonDocument doc;
        doc["success"] = false;
        doc["message"] = "Missing required fields: name, cardUID/uid, and pin or backupPIN";
        _sendJSON(request, 400, doc);
        return;
    }

    if (!isValidTelegramChatId(telegramChatID)) {
        JsonDocument doc;
        doc["success"] = false;
        doc["field"] = "telegramChatID";
        doc["message"] = "A valid Telegram Chat ID is required for OTP delivery";
        _sendJSON(request, 400, doc);
        return;
    }

    if (!isFourDigitCode(pin)) {
        JsonDocument doc;
        doc["success"] = false;
        doc["field"] = "pin";
        doc["message"] = "PIN must be exactly 4 digits";
        _sendJSON(request, 400, doc);
        return;
    }

    if (!isAlphabeticName(name)) {
        JsonDocument doc;
        doc["success"] = false;
        doc["field"] = "name";
        doc["message"] = "Name must contain alphabetic characters only";
        _sendJSON(request, 400, doc);
        return;
    }

    if (!backupPIN.isEmpty()) {
        if (!isFourDigitCode(backupPIN)) {
            JsonDocument doc;
            doc["success"] = false;
            doc["field"] = "backupPIN";
            doc["message"] = "Backup PIN must be exactly 4 digits";
            _sendJSON(request, 400, doc);
            return;
        }
    }

    // Professional duplicate RFID protection
    bool duplicateRFID = _auth->userExists(uid);
    if (!duplicateRFID && LittleFS.exists("/users.json")) {
        JsonDocument usersDoc;
        File file = LittleFS.open("/users.json", "r");
        if (file) {
            DeserializationError fileErr = deserializeJson(usersDoc, file);
            file.close();

            if (!fileErr && usersDoc["users"].is<JsonArray>()) {
                JsonArray users = usersDoc["users"].as<JsonArray>();
                for (size_t i = 0; i < users.size(); i++) {
                    String existingUID = users[i]["uid"].as<String>();
                    existingUID = normalizeUID(existingUID);
                    if (existingUID == uid) {
                        duplicateRFID = true;
                        break;
                    }
                }
            }
        }
    }

    if (duplicateRFID) {
        Serial.print("[API][WARN] Duplicate RFID enrollment blocked (Add User): ");
        Serial.println(uid);
        _security->beep(3);  // Error tone
        _addLogEntry("Admin (Web)", "Duplicate RFID " + uid, "fail");

        JsonDocument doc;
        doc["success"] = false;
        doc["errorCode"] = "RFID_ALREADY_REGISTERED";
        doc["field"] = "uid";
        doc["message"] = "This RFID card is already registered. Please scan a different card.";
        _sendJSON(request, 409, doc);
        return;
    }
    
    // Add to AuthHandler (users.json)
    bool added = _auth->addUser(uid, pin, name);

    if (added) {
        const bool chatSaved = _auth->setUserTelegramChatId(uid, telegramChatID);
        if (!chatSaved) {
            _auth->removeUser(uid);
            added = false;
        }

        if (added && !backupPIN.isEmpty()) {
            const bool backupSaved = _auth->setUserBackupPIN(uid, backupPIN);
            if (!backupSaved) {
                _auth->removeUser(uid);
                added = false;
            }
        }
    }
    
    if (added) {
        // Audible confirmation when admin saves a new user
        _security->beep(1);
    }
    
    _addLogEntry(name, "Add User", added ? "success" : "fail");
    
    JsonDocument doc;
    doc["success"] = added;
    doc["message"] = added ? "User added" : "Failed to add user";
    _sendJSON(request, added ? 201 : 500, doc);
}

/**
 * API: PUT /api/users (body: {uid, name, pin?, rfid?})
 * Edit an existing user
 */
void WebServer::_handleAPIEditUser(AsyncWebServerRequest* request, uint8_t* data, size_t len) {
    Serial.println("[API] PUT /api/users - Edit user");
    
    JsonDocument body;
    DeserializationError err = deserializeJson(body, data, len);
    
    if (err) {
        JsonDocument doc;
        doc["success"] = false;
        doc["message"] = "Invalid JSON body";
        _sendJSON(request, 400, doc);
        return;
    }
    
    String uid  = body["uid"]  | "";
    String name = body["name"] | "";
    String pin  = body["pin"]  | "";
    String rfid = body["rfid"] | "";
    String telegramChatID = body["telegramChatID"] | "";
    String backupPIN = body["backupPIN"] | "";

    name.trim();
    uid = normalizeUID(uid);
    rfid = normalizeUID(rfid);
    telegramChatID.trim();
    backupPIN.trim();
    
    if (uid.isEmpty() || name.isEmpty()) {
        JsonDocument doc;
        doc["success"] = false;
        doc["message"] = "Missing required fields: uid, name";
        _sendJSON(request, 400, doc);
        return;
    }

    if (!isValidTelegramChatId(telegramChatID)) {
        JsonDocument doc;
        doc["success"] = false;
        doc["field"] = "telegramChatID";
        doc["message"] = "A valid Telegram Chat ID is required for OTP delivery";
        _sendJSON(request, 400, doc);
        return;
    }

    if (!pin.isEmpty() && !isFourDigitCode(pin)) {
        JsonDocument doc;
        doc["success"] = false;
        doc["field"] = "pin";
        doc["message"] = "PIN must be exactly 4 digits";
        _sendJSON(request, 400, doc);
        return;
    }

    if (!isAlphabeticName(name)) {
        JsonDocument doc;
        doc["success"] = false;
        doc["field"] = "name";
        doc["message"] = "Name must contain alphabetic characters only";
        _sendJSON(request, 400, doc);
        return;
    }

    if (!backupPIN.isEmpty()) {
        if (!isFourDigitCode(backupPIN)) {
            JsonDocument doc;
            doc["success"] = false;
            doc["field"] = "backupPIN";
            doc["message"] = "Backup PIN must be exactly 4 digits";
            _sendJSON(request, 400, doc);
            return;
        }
    }

    // Duplicate RFID protection for card replacement/edit flow
    bool duplicateRFID = false;
    if (!rfid.isEmpty() && rfid != uid) {
        duplicateRFID = _auth->userExists(rfid);

        if (!duplicateRFID && LittleFS.exists("/users.json")) {
            JsonDocument usersDoc;
            File usersFile = LittleFS.open("/users.json", "r");
            if (usersFile) {
                DeserializationError usersErr = deserializeJson(usersDoc, usersFile);
                usersFile.close();

                if (!usersErr && usersDoc["users"].is<JsonArray>()) {
                    JsonArray users = usersDoc["users"].as<JsonArray>();
                    for (size_t i = 0; i < users.size(); i++) {
                        String existingUID = users[i]["uid"].as<String>();
                        existingUID = normalizeUID(existingUID);

                        if (existingUID == rfid && existingUID != uid) {
                            duplicateRFID = true;
                            break;
                        }
                    }
                }
            }
        }
    }

    if (duplicateRFID) {
        Serial.print("[API][WARN] Duplicate RFID replacement blocked (Edit User): old=");
        Serial.print(uid);
        Serial.print(" new=");
        Serial.println(rfid);

        _security->beep(3);  // Error tone
        _addLogEntry("Admin (Web)", "Duplicate RFID " + rfid, "fail");

        JsonDocument doc;
        doc["success"] = false;
        doc["errorCode"] = "RFID_ALREADY_REGISTERED";
        doc["field"] = "uid";
        doc["message"] = "This RFID card is already registered. Please scan a different card.";
        _sendJSON(request, 409, doc);
        return;
    }
    
    String effectivePin = pin;
    if (effectivePin.isEmpty()) {
        effectivePin = _auth->getUserPIN(uid);
    }

    if (effectivePin.isEmpty()) {
        JsonDocument doc;
        doc["success"] = false;
        doc["message"] = "PIN required to update this user. Please provide a valid 4-digit PIN.";
        _sendJSON(request, 400, doc);
        return;
    }

    if (!isFourDigitCode(effectivePin)) {
        JsonDocument doc;
        doc["success"] = false;
        doc["message"] = "PIN must be exactly 4 digits";
        _sendJSON(request, 400, doc);
        return;
    }

    const String targetUid = rfid.isEmpty() ? uid : rfid;
    const bool uidChanged = (targetUid != uid);

    String effectiveTelegramChatId = telegramChatID;
    if (effectiveTelegramChatId.isEmpty()) {
        effectiveTelegramChatId = _auth->getUserTelegramChatId(uid);
    }

    String effectiveBackupPIN = backupPIN;
    if (effectiveBackupPIN.isEmpty()) {
        effectiveBackupPIN = _auth->getUserBackupPIN(uid);
    }

    bool authUpdated = false;
    if (uidChanged) {
        _auth->removeUser(uid);
        authUpdated = _auth->addUser(targetUid, effectivePin, name);
    } else {
        // Re-add same UID to persist any name/PIN updates.
        authUpdated = _auth->addUser(uid, effectivePin, name);
    }

    if (!authUpdated) {
        JsonDocument doc;
        doc["success"] = false;
        doc["message"] = "Failed to update user credentials";
        _sendJSON(request, 500, doc);
        return;
    }

    if (!_auth->setUserTelegramChatId(targetUid, effectiveTelegramChatId)) {
        JsonDocument doc;
        doc["success"] = false;
        doc["message"] = "Failed to persist Telegram Chat ID";
        _sendJSON(request, 500, doc);
        return;
    }
    if (effectiveBackupPIN.length() == 4) {
        _auth->setUserBackupPIN(targetUid, effectiveBackupPIN);
    }
    
    JsonDocument doc;
    doc["success"] = true;
    doc["message"] = "User updated";
    doc["uid"] = targetUid;

    _addLogEntry(name, uidChanged ? "Edit User (RFID Replaced)" : "Edit User", "success");

    _sendJSON(request, 200, doc);
}

/**
 * API: GET /api/rfid/scan
 * Return the last scanned RFID UID (for frontend enrollment polling)
 */
void WebServer::_handleAPIRfidScan(AsyncWebServerRequest* request) {
    const unsigned long scanMs = _auth->getLastRFIDScanMs();
    const String lastUID = _auth->getLastRFIDUID();
    const bool scanned = (scanMs > 0 && lastUID.length() > 0);
    const bool known = scanned ? _auth->userExists(lastUID) : false;
    const String knownUserName = known ? _auth->getUserName(lastUID) : "";

    JsonDocument doc;
    doc["scanned"] = scanned;
    doc["known"] = known;
    doc["status"] = known ? "registered" : "unregistered";
    if (scanned) {
        doc["uid"] = lastUID;
        doc["scanTimestamp"] = scanMs;
        doc["userName"] = knownUserName;
    }
    doc["lastUid"] = lastUID;
    doc["lastScanTimestamp"] = scanMs;
    doc["timestamp"] = millis();
    
    _sendJSON(request, 200, doc);
}

void WebServer::_handleAPIDiagnostics(AsyncWebServerRequest* request) {
    JsonDocument doc;
    doc["rfidReady"] = _auth->isRFIDReady();
    doc["rfidRstActivePin"] = _auth->getActiveRFIDRstPin();
    doc["rfidCooldownActive"] = _auth->isRFIDCooldownActive();
    doc["lastRfidUid"] = _auth->getLastRFIDUID();
    doc["lastRfidScanMs"] = _auth->getLastRFIDScanMs();
    doc["keypadReady"] = _auth->isKeypadReady();
    doc["keypadMuted"] = _auth->isKeypadMuted();
    doc["keypadMuteRemainingMs"] = _auth->getKeypadMuteRemainingMs();
    doc["keypadLastKey"] = getLastKeypadKeyLabel();
    doc["keypadLastKeyMs"] = getLastKeypadKeyMs();
    doc["buzzerActive"] = _security->isBuzzerActive();
    doc["sirenActive"] = _security->isSirenActive();
    doc["vibrationLatched"] = _security->isVibrationLatched();
    doc["timestamp"] = millis();
    _sendJSON(request, 200, doc);
}

/**
 * API: GET /api/logs
 * Get activity logs
 */
void WebServer::_handleAPILogs(AsyncWebServerRequest* request) {
    Serial.println("[API] GET /api/logs");

    JsonDocument storageDoc;
    if (LittleFS.exists("/logs.json")) {
        File file = LittleFS.open("/logs.json", "r");
        DeserializationError error = deserializeJson(storageDoc, file);
        file.close();

        if (error || !storageDoc["logs"].is<JsonArray>()) {
            storageDoc.clear();
            storageDoc["logs"] = JsonArray();
        }
    } else {
        storageDoc["logs"] = JsonArray();
    }

    // Return newest-first for instant top-of-list updates in UI.
    JsonDocument responseDoc;
    JsonArray responseLogs = responseDoc["logs"].to<JsonArray>();
    JsonArray sourceLogs = storageDoc["logs"].as<JsonArray>();

    for (int i = static_cast<int>(sourceLogs.size()) - 1; i >= 0; --i) {
        if (!sourceLogs[i].is<JsonObject>()) {
            continue;
        }

        JsonObject src = sourceLogs[i].as<JsonObject>();
        JsonObject dst = responseLogs.add<JsonObject>();
        for (JsonPair kv : src) {
            dst[kv.key().c_str()] = kv.value();
        }
    }

    _sendJSON(request, 200, responseDoc);
}

/**
 * API: DELETE /api/logs
 * Clear all activity logs (fresh start)
 */
void WebServer::_handleAPIClearLogs(AsyncWebServerRequest* request) {
    Serial.println("[API] DELETE /api/logs - Clear all logs");

    JsonDocument logsDoc;
    logsDoc["logs"] = JsonArray();

    bool cleared = false;
    File file = LittleFS.open("/logs.json", "w");
    if (file) {
        serializeJson(logsDoc, file);
        file.close();
        cleared = true;
    }

    JsonDocument response;
    response["success"] = cleared;
    response["message"] = cleared ? "All logs cleared" : "Failed to clear logs";

    _sendJSON(request, cleared ? 200 : 500, response);
}

// ============================================================
// PRIVATE - Utilities
// ============================================================

/**
 * Get MIME type from filename
 */
String WebServer::_getMimeType(const String& filename) {
    if (filename.endsWith(".html")) return "text/html";
    if (filename.endsWith(".css"))  return "text/css";
    if (filename.endsWith(".js"))   return "application/javascript";
    if (filename.endsWith(".json")) return "application/json";
    if (filename.endsWith(".png"))  return "image/png";
    if (filename.endsWith(".jpg"))  return "image/jpeg";
    if (filename.endsWith(".ico"))  return "image/x-icon";
    if (filename.endsWith(".svg"))  return "image/svg+xml";
    return "text/plain";
}

/**
 * Send JSON response with CORS headers
 */
void WebServer::_sendJSON(AsyncWebServerRequest* request, int code, const JsonDocument& doc) {
    String response;
    serializeJson(doc, response);
    AsyncWebServerResponse* resp = request->beginResponse(code, "application/json", response);
    _addCORSHeaders(resp);
    request->send(resp);
}

/**
 * Add CORS headers to response
 */
void WebServer::_addCORSHeaders(AsyncWebServerResponse* response) {
    response->addHeader("Access-Control-Allow-Origin", "*");
    response->addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    response->addHeader("Access-Control-Allow-Headers", "Content-Type");
}

/**
 * Generate random guest code (4-digit PIN)
 */
String WebServer::_generateGuestCode() {
    String code = "";
    for (int i = 0; i < 4; i++) {
        code += String(random(0, 10));
    }
    return code;
}

unsigned long WebServer::_remainingCooldownMs(unsigned long lastActionMs, unsigned long cooldownMs) const {
    if (lastActionMs == 0) {
        return 0;
    }

    const unsigned long elapsed = millis() - lastActionMs;
    if (elapsed >= cooldownMs) {
        return 0;
    }

    return cooldownMs - elapsed;
}

void WebServer::_cleanupGuestUsers() {
    String guestUids[32];
    int guestCount = 0;

    const int totalUsers = _auth->getUserCount();
    for (int i = 0; i < totalUsers && guestCount < 32; i++) {
        String uid = _auth->getUserUIDAt(i);
        uid.trim();
        uid.toUpperCase();

        if (uid.startsWith("GUEST_")) {
            guestUids[guestCount++] = uid;
        }
    }

    for (int i = 0; i < guestCount; i++) {
        _auth->removeUser(guestUids[i]);
    }

    if (guestCount > 0) {
        Serial.print("[WEB] Cleaned stale guest auth entries: ");
        Serial.println(guestCount);
    }
}

void WebServer::_expireGuestCodeIfNeeded() {
    if (_guestCode.isEmpty() || _guestCodeExpiry == 0) {
        return;
    }

    const long remainingMs = static_cast<long>(_guestCodeExpiry - millis());
    if (remainingMs > 0) {
        return;
    }

    const String expiredGuestUid = "GUEST_" + _guestCode;
    _auth->removeUser(expiredGuestUid);
    _addLogEntry("System", "Guest Code Expired", "success");

    _guestCode = "";
    _guestCodeExpiry = 0;
    Serial.println("[WEB] Guest code expired and was removed from auth storage");
}

/**
 * Add an entry to the activity log (logs.json on LittleFS)
 */
void WebServer::_addLogEntry(const String& user, const String& method, const String& status) {
    JsonDocument logsDoc;
    
    if (LittleFS.exists("/logs.json")) {
        File file = LittleFS.open("/logs.json", "r");
        DeserializationError err = deserializeJson(logsDoc, file);
        file.close();
        if (err || !logsDoc.is<JsonObject>()) {
            logsDoc.clear();
        }
    }
    
    if (!logsDoc["logs"].is<JsonArray>()) {
        logsDoc["logs"] = JsonArray();
    }
    
    JsonArray logs = logsDoc["logs"].as<JsonArray>();
    
    // Cap at 50 entries — remove oldest (FIFO) to prevent LittleFS overflow
    while (logs.size() >= 50) {
        logs.remove(0);
    }
    
    // Build accurate world time (UTC) when NTP is available.
    // Store both display text and machine-readable timestamps.
    char timeStr[40];
    unsigned long long epochMs = 0;
    time_t now = time(nullptr);
    if (now > 1700000000) {
        struct tm utcTime;
        gmtime_r(&now, &utcTime);
        strftime(timeStr, sizeof(timeStr), "%Y-%m-%dT%H:%M:%SZ", &utcTime);
        epochMs = static_cast<unsigned long long>(now) * 1000ULL;
    } else {
        unsigned long sec = millis() / 1000;
        unsigned long m = (sec / 60) % 60;
        unsigned long h = (sec / 3600) % 24;
        snprintf(timeStr, sizeof(timeStr), "%02lu:%02lu (uptime)", h, m);
    }
    
    JsonObject entry = logs.add<JsonObject>();
    entry["time"]   = String(timeStr);
    entry["epochMs"] = epochMs;
    entry["user"]   = user;
    entry["method"] = method;
    entry["status"] = status;
    
    File wFile = LittleFS.open("/logs.json", "w");
    serializeJson(logsDoc, wFile);
    wFile.close();
    
    Serial.print("[LOG] ");
    Serial.print(user);
    Serial.print(" | ");
    Serial.print(method);
    Serial.print(" | ");
    Serial.println(status);
}
