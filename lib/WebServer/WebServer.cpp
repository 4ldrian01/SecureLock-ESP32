/**
 * ============================================================
 * WebServer - Implementation
 * ============================================================
 */

#include "WebServer.h"

#include "secrets.h"

#include <time.h>
#include <StorageService.h>

namespace {
constexpr bool kVerboseHttpLogs = false;

String normalizeUID(const String& input) {
    String uid = input;
    uid.trim();
    uid.toUpperCase();
    uid.replace(" ", "");
    uid.replace(":", "");
    uid.replace("-", "");
    return uid;
}

String* getOrCreateRequestBody(AsyncWebServerRequest* request, size_t totalLen) {
    String* body = reinterpret_cast<String*>(request->_tempObject);
    if (!body) {
        body = new String();
        body->reserve(totalLen);
        request->_tempObject = body;
    }
    return body;
}

void releaseRequestBody(AsyncWebServerRequest* request) {
    String* body = reinterpret_cast<String*>(request->_tempObject);
    if (body) {
        delete body;
        request->_tempObject = nullptr;
    }
}

bool isConfiguredAdminChatId(const String& chatId) {
    String normalizedChatId = chatId;
    normalizedChatId.trim();

    if (normalizedChatId.length() == 0) {
        return false;
    }

    for (int i = 0; i < NUM_ADMINS; i++) {
        String configuredChatId = ADMIN_CHAT_IDS[i];
        configuredChatId.trim();
        if (configuredChatId.length() == 0) {
            continue;
        }

        if (configuredChatId == normalizedChatId) {
            return true;
        }
    }

    return false;
}

void splitNameParts(const String& fullName, String* firstName, String* middleName, String* lastName) {
    if (firstName) {
        *firstName = "";
    }
    if (middleName) {
        *middleName = "";
    }
    if (lastName) {
        *lastName = "";
    }

    String normalized = fullName;
    normalized.trim();

    while (normalized.indexOf("  ") >= 0) {
        normalized.replace("  ", " ");
    }

    if (normalized.length() == 0) {
        return;
    }

    const int firstSpace = normalized.indexOf(' ');
    if (firstSpace < 0) {
        if (firstName) {
            *firstName = normalized;
        }
        return;
    }

    const int lastSpace = normalized.lastIndexOf(' ');

    if (firstName) {
        *firstName = normalized.substring(0, firstSpace);
        firstName->trim();
    }

    if (lastName) {
        *lastName = normalized.substring(lastSpace + 1);
        lastName->trim();
    }

    if (middleName && lastSpace > firstSpace) {
        *middleName = normalized.substring(firstSpace + 1, lastSpace);
        middleName->trim();
    }
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
      _lastEmergencyUnlockMs(0),
      _apiSessionToken(""),
      _apiSessionIssuedAtMs(0),
      _apiSessionExpiresAtMs(0),
    _apiSessionActorLabel(""),
    _apiSessionAdminUsername(""),
    _apiSessionAdminSlot(0),
      _authFailedAttempts(0),
      _authLockoutUntilMs(0) {
}

/**
 * Initialize WiFi and Web Server
 */
void WebServer::init(const char* ssid, const char* password) {
    if (ssid != nullptr && ssid[0] != '\0') {
        Serial.print("[WEB] Legacy init: connecting to SSID '");
        Serial.print(ssid);
        Serial.println("'...");

        WiFi.mode(WIFI_STA);
        WiFi.begin(ssid, (password != nullptr) ? password : "");

        const unsigned long startMs = millis();
        unsigned long lastDotMs = 0;
        while (WiFi.status() != WL_CONNECTED && (millis() - startMs) < 15000UL) {
            const unsigned long now = millis();
            if (lastDotMs == 0 || (now - lastDotMs) >= 250UL) {
                Serial.print('.');
                lastDotMs = now;
            }
            yield();
        }
        Serial.println();
    }

    init();
}

void WebServer::init() {
    Serial.println("[WEB] Initializing Web Server...");

    _initFileSystem();
    _cleanupGuestUsers();
    _initWiFi();
    _setupRoutes();

    _server.begin();
    Serial.println("[WEB] ✓ Server started on port 80");

    if (_wifiConnected) {
        Serial.println("[WEB] Dashboard URL: http://securelock.local/");
        Serial.print("[WEB] Fallback IP URL: http://");
        Serial.print(_ipAddress);
        Serial.println("/");
    }

    const wifi_mode_t mode = WiFi.getMode();
    const bool fallbackApActive = (mode == WIFI_AP || mode == WIFI_AP_STA)
        && WiFi.softAPSSID().length() > 0;
    if (fallbackApActive) {
        Serial.print("[WEB] Fallback AP URL: http://");
        Serial.print(WiFi.softAPIP());
        Serial.print("/ (SSID: ");
        Serial.print(WiFi.softAPSSID());
        Serial.println(")");
    }
}

void WebServer::update() {
    _flushQueuedLogs(LOG_FLUSH_BURST);

    const bool connectedNow = (WiFi.status() == WL_CONNECTED);

    if (connectedNow != _wifiConnected) {
        _wifiConnected = connectedNow;

        if (_wifiConnected) {
            _ipAddress = WiFi.localIP().toString();
            Serial.print("[WiFi] Link restored. IP: ");
            Serial.println(_ipAddress);
        } else {
            _ipAddress = "";
            Serial.println("[WiFi] Link lost");
        }
        return;
    }

    if (_wifiConnected) {
        const String currentIp = WiFi.localIP().toString();
        if (currentIp != _ipAddress) {
            _ipAddress = currentIp;
            Serial.print("[WiFi] IP changed: ");
            Serial.println(_ipAddress);
        }
    }
}

bool WebServer::isConnected() const {
    return _wifiConnected;
}

String WebServer::getIPAddress() const {
    return _ipAddress;
}

void WebServer::markEmergencyOverride() {
    _lastEmergencyUnlockMs = millis();
}

unsigned long WebServer::getEmergencyCooldownRemainingMs() const {
    return _remainingCooldownMs(_lastEmergencyUnlockMs, EMERGENCY_COOLDOWN_MS);
}

void WebServer::logActivity(const String& user, const String& method, const String& status) {
    _addLogEntry(user, method, status);
}

void WebServer::_queueLogEntry(const String& user, const String& method, const String& status) {
    String safeUser = user;
    safeUser.trim();
    if (safeUser.length() == 0) {
        safeUser = "System";
    }

    String safeMethod = method;
    safeMethod.trim();
    if (safeMethod.length() == 0) {
        safeMethod = "System";
    }

    String safeStatus = _normalizeLogStatus(status);

    QueuedLogEntry nextEntry{};
    snprintf(nextEntry.user, sizeof(nextEntry.user), "%s", safeUser.c_str());
    snprintf(nextEntry.method, sizeof(nextEntry.method), "%s", safeMethod.c_str());
    snprintf(nextEntry.status, sizeof(nextEntry.status), "%s", safeStatus.c_str());

    bool dropped = false;
    unsigned long droppedCount = 0;

    portENTER_CRITICAL(&_queuedLogMux);
    if (_queuedLogCount >= LOG_QUEUE_CAPACITY) {
        _queuedLogHead = (_queuedLogHead + 1) % LOG_QUEUE_CAPACITY;
        _queuedLogCount--;
        _droppedQueuedLogs++;
        dropped = true;
        droppedCount = _droppedQueuedLogs;
    }

    _queuedLogs[_queuedLogTail] = nextEntry;
    _queuedLogTail = (_queuedLogTail + 1) % LOG_QUEUE_CAPACITY;
    _queuedLogCount++;
    portEXIT_CRITICAL(&_queuedLogMux);

    if (dropped && (droppedCount == 1 || (droppedCount % 8UL) == 0UL)) {
        Serial.print("[LOG][WARN] Pending log queue overflow; dropped oldest entries: ");
        Serial.println(droppedCount);
    }
}

bool WebServer::_popQueuedLog(QueuedLogEntry* outEntry) {
    if (!outEntry) {
        return false;
    }

    bool hasItem = false;

    portENTER_CRITICAL(&_queuedLogMux);
    if (_queuedLogCount > 0) {
        *outEntry = _queuedLogs[_queuedLogHead];
        _queuedLogHead = (_queuedLogHead + 1) % LOG_QUEUE_CAPACITY;
        _queuedLogCount--;
        hasItem = true;
    }
    portEXIT_CRITICAL(&_queuedLogMux);

    return hasItem;
}

void WebServer::_clearQueuedLogs() {
    portENTER_CRITICAL(&_queuedLogMux);
    _queuedLogHead = 0;
    _queuedLogTail = 0;
    _queuedLogCount = 0;
    portEXIT_CRITICAL(&_queuedLogMux);
}

void WebServer::_flushQueuedLogs(uint8_t maxEntries) {
    if (maxEntries == 0) {
        return;
    }

    const uint8_t targetCount = (maxEntries > LOG_FLUSH_MAX_BATCH)
        ? LOG_FLUSH_MAX_BATCH
        : maxEntries;

    QueuedLogEntry pendingEntries[LOG_FLUSH_MAX_BATCH]{};
    size_t pendingCount = 0;

    while (pendingCount < targetCount) {
        if (!_popQueuedLog(&pendingEntries[pendingCount])) {
            break;
        }
        pendingCount++;
    }

    if (pendingCount == 0) {
        return;
    }

    _writeLogBatchToStorage(pendingEntries, pendingCount);
}

void WebServer::_invalidateUsersStatsCache() {
    _usersStatsCacheValid = false;
    _usersStatsCacheAtMs = 0;
}

String WebServer::_normalizeLogStatus(const String& status) const {
    String normalized = status;
    normalized.trim();
    normalized.toLowerCase();

    if (normalized == "ok" || normalized == "pass") {
        return "success";
    }

    if (normalized == "error" || normalized == "failed" || normalized == "deny" || normalized == "denied") {
        return "fail";
    }

    if (normalized == "warn" || normalized == "warning" || normalized == "alert") {
        return "alarm";
    }

    if (normalized == "success" || normalized == "fail" || normalized == "alarm" || normalized == "info") {
        return normalized;
    }

    if (normalized.length() == 0) {
        return "info";
    }

    return "info";
}

// ============================================================
// PRIVATE - Initialization
// ============================================================

void WebServer::_initWiFi() {
    Serial.println("[WiFi] Preparing station connection (WiFiMulti-native)...");

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.persistent(false);
    WiFi.setSleep(false);

    const unsigned long settleStartMs = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - settleStartMs) < 3000) {
        yield();
    }

    if (WiFi.status() == WL_CONNECTED) {
        _wifiConnected = true;
        _ipAddress = WiFi.localIP().toString();

        // Initialize NTP clock (UTC) for accurate world-time logging.
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

        Serial.println("[WiFi] Reusing existing active connection");
        Serial.print("[WiFi] SSID: ");
        Serial.println(WiFi.SSID());
        Serial.print("[WiFi] IP: ");
        Serial.println(_ipAddress);
        Serial.print("[WiFi] RSSI: ");
        Serial.print(WiFi.RSSI());
        Serial.println(" dBm");
        Serial.print("[WiFi] Channel: ");
        Serial.println(WiFi.channel());
        Serial.print("[TIME] NTP sync: ");
        Serial.println(timeReady ? "OK (UTC)" : "PENDING (using fallback until synced)");
        return;
    }

    _wifiConnected = false;
    _ipAddress = "";
    Serial.println("[WiFi][WARN] No active link at WebServer init.");
    Serial.println("[WiFi][WARN] Web server will stay available and become reachable once WiFiMulti reconnects.");
}

void WebServer::_initFileSystem() {
    Serial.print("[FS] Mounting LittleFS...");

    if (!StorageService::instance().ensureMounted()) {
        Serial.println(" ✗ FAILED!");
        Serial.println("[FS][WARN] Auto-format is disabled by policy. Data preserved for manual recovery.");
        return;
    }

    Serial.println(" ✓ OK");

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

void WebServer::_setupRoutes() {
    Serial.println("[WEB] Setting up routes...");

    // ==================== CORS Preflight ====================
    _server.on("/api/*", HTTP_OPTIONS, [this](AsyncWebServerRequest* request) {
        AsyncWebServerResponse* response = request->beginResponse(204);
        _addCORSHeaders(request, response);
        _addNoCacheHeaders(response);
        request->send(response);
    });

    // ==================== STATIC FILES ====================

    _server.on("/", HTTP_GET, [this](AsyncWebServerRequest* request) {
        _handleRoot(request);
    });

    _server.on("/css/style.css", HTTP_GET, [this](AsyncWebServerRequest* request) {
        _handleCSS(request);
    });

    // ==================== API ENDPOINTS ====================

    _server.on("/api/auth/login", HTTP_POST,
        [this](AsyncWebServerRequest* request) { /* handled in body callback */ },
        NULL,
        [this](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
            String* body = getOrCreateRequestBody(request, total);
            if (len > 0) {
                body->concat(reinterpret_cast<const char*>(data), len);
            }

            if ((index + len) >= total) {
                _handleAPIAuthLogin(
                    request,
                    reinterpret_cast<uint8_t*>(const_cast<char*>(body->c_str())),
                    body->length()
                );
                releaseRequestBody(request);
            }
        }
    );

    _server.on("/api/auth/logout", HTTP_POST, [this](AsyncWebServerRequest* request) {
        _handleAPIAuthLogout(request);
    });

    _server.on("/api/auth/status", HTTP_GET, [this](AsyncWebServerRequest* request) {
        _handleAPIAuthStatus(request);
    });

    _server.on("/api/status", HTTP_GET, [this](AsyncWebServerRequest* request) {
        _handleAPIStatus(request);
    });

    _server.on("/api/unlock", HTTP_POST, [this](AsyncWebServerRequest* request) {
        _handleAPIUnlock(request);
    });

    _server.on("/api/guest-code", HTTP_POST, [this](AsyncWebServerRequest* request) {
        _handleAPIGuestCode(request);
    });

    _server.on("/api/users", HTTP_GET, [this](AsyncWebServerRequest* request) {
        _handleAPIUsers(request);
    });

    _server.on("/api/users", HTTP_POST,
        [this](AsyncWebServerRequest* request) { /* handled in body callback */ },
        NULL,
        [this](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
            String* body = getOrCreateRequestBody(request, total);
            if (len > 0) {
                body->concat(reinterpret_cast<const char*>(data), len);
            }

            if ((index + len) >= total) {
                _handleAPIAddUser(
                    request,
                    reinterpret_cast<uint8_t*>(const_cast<char*>(body->c_str())),
                    body->length()
                );
                releaseRequestBody(request);
            }
        }
    );

    _server.on("/api/addUser", HTTP_POST,
        [this](AsyncWebServerRequest* request) { /* handled in body callback */ },
        NULL,
        [this](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
            String* body = getOrCreateRequestBody(request, total);
            if (len > 0) {
                body->concat(reinterpret_cast<const char*>(data), len);
            }

            if ((index + len) >= total) {
                _handleAPIAddUser(
                    request,
                    reinterpret_cast<uint8_t*>(const_cast<char*>(body->c_str())),
                    body->length()
                );
                releaseRequestBody(request);
            }
        }
    );

    _server.on("/api/users", HTTP_PUT,
        [this](AsyncWebServerRequest* request) { /* handled in body callback */ },
        NULL,
        [this](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
            String* body = getOrCreateRequestBody(request, total);
            if (len > 0) {
                body->concat(reinterpret_cast<const char*>(data), len);
            }

            if ((index + len) >= total) {
                _handleAPIEditUser(
                    request,
                    reinterpret_cast<uint8_t*>(const_cast<char*>(body->c_str())),
                    body->length()
                );
                releaseRequestBody(request);
            }
        }
    );

    _server.on("/api/users", HTTP_DELETE, [this](AsyncWebServerRequest* request) {
        _handleAPIDeleteUser(request);
    });

    _server.on("/api/users/reset", HTTP_POST,
        [this](AsyncWebServerRequest* request) { /* handled in body callback */ },
        NULL,
        [this](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
            String* body = getOrCreateRequestBody(request, total);
            if (len > 0) {
                body->concat(reinterpret_cast<const char*>(data), len);
            }

            if ((index + len) >= total) {
                _handleAPIResetUsers(
                    request,
                    reinterpret_cast<uint8_t*>(const_cast<char*>(body->c_str())),
                    body->length()
                );
                releaseRequestBody(request);
            }
        }
    );

    _server.on("/api/logs", HTTP_GET, [this](AsyncWebServerRequest* request) {
        _handleAPILogs(request);
    });

    _server.on("/api/logs", HTTP_DELETE, [this](AsyncWebServerRequest* request) {
        _handleAPIClearLogs(request);
    });

    _server.on("/api/rfid/enroll/start", HTTP_POST, [this](AsyncWebServerRequest* request) {
        _handleAPIRfidEnrollStart(request);
    });

    _server.on("/api/rfid/enroll/stop", HTTP_POST, [this](AsyncWebServerRequest* request) {
        _handleAPIRfidEnrollStop(request);
    });

    _server.on("/api/rfid/scan", HTTP_GET, [this](AsyncWebServerRequest* request) {
        _handleAPIRfidScan(request);
    });

    _server.on("/api/diagnostics", HTTP_GET, [this](AsyncWebServerRequest* request) {
        _handleAPIDiagnostics(request);
    });

    _server.onNotFound([this](AsyncWebServerRequest* request) {
        _handleNotFound(request);
    });

    Serial.println("[WEB] ✓ Routes configured");
}

// ============================================================
// PRIVATE - Utilities
// ============================================================

void WebServer::_sendJSON(AsyncWebServerRequest* request, int code, const JsonDocument& doc) {
    String response;
    serializeJson(doc, response);
    AsyncWebServerResponse* resp = request->beginResponse(code, "application/json", response);
    _addCORSHeaders(request, resp);
    _addNoCacheHeaders(resp);
    request->send(resp);
}

void WebServer::_addCORSHeaders(AsyncWebServerRequest* request, AsyncWebServerResponse* response) {
    if (!response) {
        return;
    }

    response->addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    response->addHeader("Access-Control-Allow-Headers", "Content-Type, Authorization, X-Requested-With");
    response->addHeader("Access-Control-Max-Age", "600");

    if (!request || !request->hasHeader("Origin")) {
        return;
    }

    String origin = request->header("Origin");
    if (!_isAllowedCORSOrigin(request, origin)) {
        if (kVerboseHttpLogs) {
            Serial.print("[WEB][CORS] Blocked Origin: ");
            Serial.println(origin);
        }
        return;
    }

    response->addHeader("Access-Control-Allow-Origin", origin);
    response->addHeader("Vary", "Origin");
}

bool WebServer::_isAllowedCORSOrigin(AsyncWebServerRequest* request, const String& origin) const {
    if (!request) {
        return false;
    }

    String normalizedOrigin = origin;
    normalizedOrigin.trim();
    normalizedOrigin.toLowerCase();

    if (normalizedOrigin.length() == 0 || normalizedOrigin == "null") {
        return false;
    }

    auto isPrefixAllowed = [](const String& value, const String& prefix) -> bool {
        return value == prefix || value.startsWith(prefix + ":");
    };

    auto isGitHubPagesOrigin = [&](const String& value) -> bool {
        if (!value.startsWith("https://")) {
            return false;
        }

        const String suffix = ".github.io";
        int suffixIndex = value.indexOf(suffix);
        if (suffixIndex <= static_cast<int>(strlen("https://"))) {
            return false;
        }

        int endIndex = suffixIndex + suffix.length();
        return endIndex == value.length() || value.charAt(endIndex) == ':';
    };

    auto isHostOrigin = [&](const String& hostValue) -> bool {
        if (hostValue.length() == 0) {
            return false;
        }

        const String httpOrigin = String("http://") + hostValue;
        const String httpsOrigin = String("https://") + hostValue;
        return normalizedOrigin == httpOrigin || normalizedOrigin == httpsOrigin;
    };

    String requestHost;
    if (request->hasHeader("Host")) {
        requestHost = request->header("Host");
        requestHost.trim();
        requestHost.toLowerCase();
    }

    if (isHostOrigin(requestHost)) {
        return true;
    }

    String currentIp = _ipAddress;
    currentIp.trim();
    currentIp.toLowerCase();
    if (isHostOrigin(currentIp)) {
        return true;
    }

    if (isPrefixAllowed(normalizedOrigin, "http://securelock.local")
        || isPrefixAllowed(normalizedOrigin, "https://securelock.local")
        || isPrefixAllowed(normalizedOrigin, "http://localhost")
        || isPrefixAllowed(normalizedOrigin, "https://localhost")
        || isPrefixAllowed(normalizedOrigin, "http://127.0.0.1")
        || isPrefixAllowed(normalizedOrigin, "https://127.0.0.1")
        || isPrefixAllowed(normalizedOrigin, "http://[::1]")
        || isPrefixAllowed(normalizedOrigin, "https://[::1]")
        || isGitHubPagesOrigin(normalizedOrigin)) {
        return true;
    }

    return false;
}

void WebServer::_addSecurityHeaders(AsyncWebServerResponse* response) {
    response->addHeader("X-Content-Type-Options", "nosniff");
    response->addHeader("X-Frame-Options", "DENY");
    response->addHeader("Referrer-Policy", "no-referrer");
    response->addHeader("Cross-Origin-Resource-Policy", "same-origin");
    response->addHeader("Permissions-Policy", "camera=(), microphone=(), geolocation=()");
}

void WebServer::_addStaticCacheHeaders(AsyncWebServerResponse* response) {
    _addSecurityHeaders(response);

    response->addHeader("Cache-Control", "public, max-age=60, must-revalidate, stale-while-revalidate=120");
    response->addHeader("Vary", "Accept-Encoding");
}

void WebServer::_addNoCacheHeaders(AsyncWebServerResponse* response) {
    _addSecurityHeaders(response);

    response->addHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    response->addHeader("Pragma", "no-cache");
    response->addHeader("Expires", "0");
}

bool WebServer::_syncUsersFileFromAuth(JsonDocument* responseDoc) {
    JsonDocument currentDoc;
    if (LittleFS.exists("/users.json")) {
        File file = LittleFS.open("/users.json", "r");
        if (file) {
            DeserializationError error = deserializeJson(currentDoc, file);
            file.close();

            if (error || !currentDoc.is<JsonObject>()) {
                currentDoc.clear();
            }
        }
    }

    if (!currentDoc["users"].is<JsonArray>()) {
        currentDoc["users"].to<JsonArray>();
    }

    JsonArray existingUsers = currentDoc["users"].as<JsonArray>();

    JsonDocument localResponseDoc;
    JsonDocument& outDoc = responseDoc ? *responseDoc : localResponseDoc;
    JsonArray outUsers = outDoc["users"].to<JsonArray>();

    for (int i = 0; i < _auth->getUserCount(); i++) {
        String uid = normalizeUID(_auth->getUserUIDAt(i));
        if (uid.isEmpty()) {
            continue;
        }

        if (uid.startsWith("GUEST_")) {
            continue;
        }

        String name = _auth->getUserName(uid);
        if (name == "Unknown" || name.isEmpty()) {
            name = "User";
        }

        String firstName = "";
        String middleName = "";
        String lastName = "";
        splitNameParts(name, &firstName, &middleName, &lastName);

        String userChatId = _auth->getUserTelegramChatId(uid);
        const bool isAdminChat = isConfiguredAdminChatId(userChatId);

        String type = isAdminChat ? "admin" : "user";

        JsonObject user = outUsers.add<JsonObject>();
        user["cardUID"] = uid;
        user["uid"] = uid;
        user["name"] = name;
        user["firstName"] = firstName;
        user["middleName"] = middleName;
        user["lastName"] = lastName;
        user["type"] = type;
        user["telegramChatID"] = userChatId;
        user["chat_id"] = userChatId;
        user["isAdminChat"] = isAdminChat;
        user["backupPIN"] = _auth->getUserBackupPIN(uid);
    }

    if (currentDoc["settings"].is<JsonObject>()) {
        JsonObject currentSettings = currentDoc["settings"].as<JsonObject>();
        JsonObject outSettings = outDoc["settings"].to<JsonObject>();
        for (JsonPair kv : currentSettings) {
            outSettings[kv.key().c_str()] = kv.value();
        }
    }

    auto normalizeType = [](const JsonObjectConst& obj) {
        String type = obj["type"] | "user";
        type.trim();
        type.toLowerCase();
        if (type.isEmpty()) {
            type = "user";
        }
        return type;
    };

    auto normalizedUidFromObject = [](const JsonObjectConst& obj) {
        String uid = normalizeUID(obj["uid"] | "");
        if (uid.isEmpty()) {
            uid = normalizeUID(obj["cardUID"] | "");
        }
        if (uid.isEmpty()) {
            uid = normalizeUID(obj["cardUid"] | "");
        }
        return uid;
    };

    auto usersEquivalent = [&](const JsonArrayConst& lhs, const JsonArrayConst& rhs) {
        if (lhs.size() != rhs.size()) {
            return false;
        }

        for (JsonObjectConst rhsUser : rhs) {
            const String rhsUid = normalizedUidFromObject(rhsUser);
            if (rhsUid.isEmpty()) {
                return false;
            }

            bool matched = false;
            for (JsonObjectConst lhsUser : lhs) {
                if (normalizedUidFromObject(lhsUser) != rhsUid) {
                    continue;
                }

                String lhsName = lhsUser["name"] | "";
                String rhsName = rhsUser["name"] | "";
                lhsName.trim();
                rhsName.trim();

                String lhsChat = lhsUser["telegramChatID"] | "";
                String rhsChat = rhsUser["telegramChatID"] | "";
                if (lhsChat.length() == 0) {
                    lhsChat = lhsUser["chat_id"] | "";
                }
                if (rhsChat.length() == 0) {
                    rhsChat = rhsUser["chat_id"] | "";
                }
                lhsChat.trim();
                rhsChat.trim();

                String lhsBackup = lhsUser["backupPIN"] | "";
                String rhsBackup = rhsUser["backupPIN"] | "";
                lhsBackup.trim();
                rhsBackup.trim();

                String lhsFirstName = lhsUser["firstName"] | "";
                String rhsFirstName = rhsUser["firstName"] | "";
                lhsFirstName.trim();
                rhsFirstName.trim();

                String lhsMiddleName = lhsUser["middleName"] | "";
                String rhsMiddleName = rhsUser["middleName"] | "";
                lhsMiddleName.trim();
                rhsMiddleName.trim();

                String lhsLastName = lhsUser["lastName"] | "";
                String rhsLastName = rhsUser["lastName"] | "";
                lhsLastName.trim();
                rhsLastName.trim();

                bool lhsIsAdminChat = lhsUser["isAdminChat"] | false;
                bool rhsIsAdminChat = rhsUser["isAdminChat"] | false;

                if (lhsName == rhsName
                    && normalizeType(lhsUser) == normalizeType(rhsUser)
                    && lhsChat == rhsChat
                    && lhsBackup == rhsBackup
                    && lhsFirstName == rhsFirstName
                    && lhsMiddleName == rhsMiddleName
                    && lhsLastName == rhsLastName
                    && lhsIsAdminChat == rhsIsAdminChat) {
                    matched = true;
                }
                break;
            }

            if (!matched) {
                return false;
            }
        }

        return true;
    };

    bool shouldRewriteUsersFile = !LittleFS.exists("/users.json");
    if (!shouldRewriteUsersFile) {
        shouldRewriteUsersFile = !usersEquivalent(existingUsers, outUsers);
    }

    if (!shouldRewriteUsersFile) {
        return true;
    }

    File file = LittleFS.open("/users.json", "w");
    if (!file) {
        Serial.println("[API][WARN] Failed to rewrite users.json during sync");
        return false;
    }

    serializeJson(outDoc, file);
    file.close();
    _invalidateUsersStatsCache();
    Serial.println("[API] users.json synchronized from auth storage");
    return true;
}

void WebServer::_collectUsersStorageStats(int* rawCount, int* uniqueCount, int* invalidCount, int* duplicateCount) {
    const unsigned long nowMs = millis();
    if (_usersStatsCacheValid && (nowMs - _usersStatsCacheAtMs) < USERS_STATS_CACHE_TTL_MS) {
        if (rawCount) {
            *rawCount = _cachedRawUsers;
        }
        if (uniqueCount) {
            *uniqueCount = _cachedUniqueUsers;
        }
        if (invalidCount) {
            *invalidCount = _cachedInvalidUsers;
        }
        if (duplicateCount) {
            *duplicateCount = _cachedDuplicateUsers;
        }
        return;
    }

    int computedRaw = 0;
    int computedUnique = 0;
    int computedInvalid = 0;
    int computedDuplicate = 0;

    if (LittleFS.exists("/users.json")) {
        JsonDocument usersDoc;
        File file = LittleFS.open("/users.json", "r");
        if (file) {
            const DeserializationError err = deserializeJson(usersDoc, file);
            file.close();

            if (!err && usersDoc["users"].is<JsonArray>()) {
                JsonArray users = usersDoc["users"].as<JsonArray>();
                String seen[64];
                int seenCount = 0;

                for (JsonObject user : users) {
                    computedRaw++;

                    String uid = normalizeUID(user["cardUID"] | "");
                    if (uid.length() == 0) {
                        uid = normalizeUID(user["uid"] | "");
                    }
                    if (uid.length() == 0) {
                        uid = normalizeUID(user["cardUid"] | "");
                    }
                    if (uid.length() == 0) {
                        uid = normalizeUID(user["rfid"] | "");
                    }
                    if (uid.length() == 0) {
                        uid = normalizeUID(user["rfidUID"] | "");
                    }

                    if (uid.length() == 0) {
                        computedInvalid++;
                        continue;
                    }

                    bool duplicate = false;
                    for (int i = 0; i < seenCount; i++) {
                        if (seen[i] == uid) {
                            duplicate = true;
                            break;
                        }
                    }

                    if (duplicate) {
                        computedDuplicate++;
                        continue;
                    }

                    if (seenCount < 64) {
                        seen[seenCount++] = uid;
                    }

                    computedUnique++;
                }
            }
        }
    }

    _cachedRawUsers = computedRaw;
    _cachedUniqueUsers = computedUnique;
    _cachedInvalidUsers = computedInvalid;
    _cachedDuplicateUsers = computedDuplicate;
    _usersStatsCacheAtMs = nowMs;
    _usersStatsCacheValid = true;

    if (rawCount) {
        *rawCount = computedRaw;
    }
    if (uniqueCount) {
        *uniqueCount = computedUnique;
    }
    if (invalidCount) {
        *invalidCount = computedInvalid;
    }
    if (duplicateCount) {
        *duplicateCount = computedDuplicate;
    }
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
