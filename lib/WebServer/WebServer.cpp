/**
 * ============================================================
 * WebServer - Implementation
 * ============================================================
 */

#include "WebServer.h"

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
        while (WiFi.status() != WL_CONNECTED && (millis() - startMs) < 15000UL) {
            delay(250);
            Serial.print('.');
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
}

void WebServer::update() {
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
        _addCORSHeaders(response);
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
// PRIVATE - Static File Handlers
// ============================================================

void WebServer::_handleRoot(AsyncWebServerRequest* request) {
    const char* candidates[] = {
        "/html/pages/dashboard.html",
        "/html/index.html",
        "/index.html"
    };

    for (const char* path : candidates) {
        if (LittleFS.exists(path)) {
            AsyncWebServerResponse* response = request->beginResponse(LittleFS, path, "text/html");
            _addNoCacheHeaders(response);
            request->send(response);
            if (kVerboseHttpLogs) {
                Serial.print("[WEB] GET / -> ");
                Serial.println(path);
            }
            return;
        }
    }

    Serial.println("[WEB] ✗ dashboard HTML not found in LittleFS!");
    request->send(404, "text/html",
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>SecureLock Dashboard Missing</title></head><body>"
        "<h2>SecureLock Dashboard files are missing</h2>"
        "<p>Upload LittleFS data first, then reload this page.</p>"
        "<pre>platformio run --target uploadfs</pre>"
        "<p>Expected file: <code>data/html/pages/dashboard.html</code></p>"
        "</body></html>");
}

void WebServer::_handleCSS(AsyncWebServerRequest* request) {
    if (LittleFS.exists("/css/style.css")) {
        AsyncWebServerResponse* response = request->beginResponse(LittleFS, "/css/style.css", "text/css");
        _addNoCacheHeaders(response);
        request->send(response);
        if (kVerboseHttpLogs) {
            Serial.println("[WEB] GET /css/style.css -> OK");
        }
    } else if (LittleFS.exists("/style.css")) {
        AsyncWebServerResponse* response = request->beginResponse(LittleFS, "/style.css", "text/css");
        _addNoCacheHeaders(response);
        request->send(response);
        if (kVerboseHttpLogs) {
            Serial.println("[WEB] GET /css/style.css -> fallback /style.css");
        }
    } else {
        Serial.println("[WEB] ✗ style.css not found!");
        request->send(404, "text/plain", "CSS not found");
    }
}

void WebServer::_handleNotFound(AsyncWebServerRequest* request) {
    const String url = request->url();
    String staticPath = url;

    const int queryPos = staticPath.indexOf('?');
    if (queryPos >= 0) {
        staticPath = staticPath.substring(0, queryPos);
    }

    const int fragmentPos = staticPath.indexOf('#');
    if (fragmentPos >= 0) {
        staticPath = staticPath.substring(0, fragmentPos);
    }

    if (!staticPath.startsWith("/api/") && LittleFS.exists(staticPath)) {
        AsyncWebServerResponse* response = request->beginResponse(LittleFS, staticPath, _getMimeType(staticPath));
        const bool noCacheUiAsset = staticPath.endsWith(".html")
            || staticPath.endsWith(".css")
            || staticPath.endsWith(".js");

        if (noCacheUiAsset) {
            _addNoCacheHeaders(response);
        } else {
            _addStaticCacheHeaders(response);
        }
        request->send(response);
        if (kVerboseHttpLogs) {
            Serial.print("[WEB] Static fallback served: ");
            Serial.println(staticPath);
        }
        return;
    }

    Serial.print("[WEB] 404: ");
    Serial.println(url);

    request->send(404, "text/plain", "404 - Not Found");
}

// ============================================================
// PRIVATE - Utilities
// ============================================================

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

void WebServer::_sendJSON(AsyncWebServerRequest* request, int code, const JsonDocument& doc) {
    String response;
    serializeJson(doc, response);
    AsyncWebServerResponse* resp = request->beginResponse(code, "application/json", response);
    _addCORSHeaders(resp);
    _addNoCacheHeaders(resp);
    request->send(resp);
}

void WebServer::_addCORSHeaders(AsyncWebServerResponse* response) {
    response->addHeader("Access-Control-Allow-Origin", "*");
    response->addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    response->addHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
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

    response->addHeader("Cache-Control", "public, max-age=600, stale-while-revalidate=120");
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
        currentDoc["users"] = JsonArray();
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

        String type = "user";

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

        JsonObject user = outUsers.add<JsonObject>();
        user["cardUID"] = uid;
        user["uid"] = uid;
        user["name"] = name;
        user["type"] = type;
        user["telegramChatID"] = _auth->getUserTelegramChatId(uid);
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
                lhsChat.trim();
                rhsChat.trim();

                String lhsBackup = lhsUser["backupPIN"] | "";
                String rhsBackup = rhsUser["backupPIN"] | "";
                lhsBackup.trim();
                rhsBackup.trim();

                if (lhsName == rhsName
                    && normalizeType(lhsUser) == normalizeType(rhsUser)
                    && lhsChat == rhsChat
                    && lhsBackup == rhsBackup) {
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
    Serial.println("[API] users.json synchronized from auth storage");
    return true;
}

void WebServer::_collectUsersStorageStats(int* rawCount, int* uniqueCount, int* invalidCount, int* duplicateCount) {
    if (rawCount) {
        *rawCount = 0;
    }
    if (uniqueCount) {
        *uniqueCount = 0;
    }
    if (invalidCount) {
        *invalidCount = 0;
    }
    if (duplicateCount) {
        *duplicateCount = 0;
    }

    if (!LittleFS.exists("/users.json")) {
        return;
    }

    JsonDocument usersDoc;
    File file = LittleFS.open("/users.json", "r");
    if (!file) {
        return;
    }

    const DeserializationError err = deserializeJson(usersDoc, file);
    file.close();

    if (err || !usersDoc["users"].is<JsonArray>()) {
        return;
    }

    JsonArray users = usersDoc["users"].as<JsonArray>();
    String seen[64];
    int seenCount = 0;

    for (JsonObject user : users) {
        if (rawCount) {
            (*rawCount)++;
        }

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
            if (invalidCount) {
                (*invalidCount)++;
            }
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
            if (duplicateCount) {
                (*duplicateCount)++;
            }
            continue;
        }

        if (seenCount < 64) {
            seen[seenCount++] = uid;
        }

        if (uniqueCount) {
            (*uniqueCount)++;
        }
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
