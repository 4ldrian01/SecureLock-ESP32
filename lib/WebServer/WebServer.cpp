/**
 * ============================================================
 * WebServer - Implementation
 * ============================================================
 */

#include "WebServer.h"
#include <time.h>

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
      _guestCodeExpiry(0)
{
}

/**
 * Initialize WiFi and Web Server
 */
void WebServer::init(const char* ssid, const char* password) {
    Serial.println("[WEB] Initializing Web Server...");
    
    _initFileSystem();
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
    WiFi.begin(ssid, password);
    
    // Wait for connection (20 second timeout)
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 40) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    Serial.println();
    
    if (WiFi.status() == WL_CONNECTED) {
        _wifiConnected = true;
        _ipAddress = WiFi.localIP().toString();

        // Initialize NTP clock (UTC) for accurate world-time logging
        configTzTime("UTC0", "pool.ntp.org", "time.google.com", "time.nist.gov");

        bool timeReady = false;
        for (int i = 0; i < 15; i++) {
            time_t now = time(nullptr);
            if (now > 1700000000) {
                timeReady = true;
                break;
            }
            delay(200);
        }
        
        Serial.println("[WiFi] ✓ Connected!");
        Serial.print("[WiFi] IP: ");
        Serial.println(_ipAddress);
        Serial.print("[WiFi] RSSI: ");
        Serial.print(WiFi.RSSI());
        Serial.println(" dBm");

        Serial.print("[TIME] NTP sync: ");
        Serial.println(timeReady ? "OK (UTC)" : "PENDING (using fallback until synced)");
    } else {
        _wifiConnected = false;
        Serial.println("[WiFi] ✗ Connection failed");
        Serial.println("[WiFi] Check credentials in secrets.h");
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
    
    // JavaScript file
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
    
    // RFID scan polling
    _server.on("/api/rfid/scan", HTTP_GET, [this](AsyncWebServerRequest* request) {
        _handleAPIRfidScan(request);
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
    Serial.print("[WEB] 404: ");
    Serial.println(request->url());
    
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
    JsonDocument doc;
    
    // Lock status
    doc["locked"] = _lock->isLocked();
    doc["doorOpen"] = _lock->isDoorOpen();
    doc["tampered"] = _lock->isDoorTampered();
    
    // Security status
    doc["alarm"] = _security->isAlarming();
    doc["vibration"] = _security->isVibrationDetected();
    
    // System info
    doc["uptime"] = millis() / 1000;
    doc["freeHeap"] = ESP.getFreeHeap();
    doc["wifiConnected"] = _wifiConnected;
    doc["ipAddress"] = _ipAddress;
    doc["rssi"] = WiFi.RSSI();
    
    _sendJSON(request, 200, doc);
    Serial.println("[API] GET /api/status");
}

/**
 * API: POST /api/unlock
 * Remote unlock command
 */
void WebServer::_handleAPIUnlock(AsyncWebServerRequest* request) {
    Serial.println("[API] POST /api/unlock - Emergency override");
    
    // Unlock door
    _lock->unlock();
    
    // Stop alarm if active
    if (_security->isAlarming()) {
        _security->clearAlarm();
    }
    
    // Log the event
    _addLogEntry("Admin (Web)", "Web", "success");
    
    // Send response
    JsonDocument doc;
    doc["success"] = true;
    doc["message"] = "Emergency unlock activated";
    doc["timestamp"] = millis();
    
    _sendJSON(request, 200, doc);
}

/**
 * API: POST /api/guest-code
 * Generate temporary guest access code
 */
void WebServer::_handleAPIGuestCode(AsyncWebServerRequest* request) {
    Serial.println("[API] POST /api/guest-code");
    
    // Generate new guest code
    _guestCode = _generateGuestCode();
    _guestCodeExpiry = millis() + 300000;  // 5 minutes
    
    // Add to AuthHandler (temporary PIN)
    _auth->addUser("GUEST_" + _guestCode, _guestCode, "Guest");
    
    // Send response
    JsonDocument doc;
    doc["success"] = true;
    doc["code"] = _guestCode;
    doc["expiresIn"] = 300;  // seconds
    doc["timestamp"] = millis();
    
    _sendJSON(request, 200, doc);
    
    Serial.print("[API] Guest code generated: ");
    Serial.println(_guestCode);
}

/**
 * API: GET /api/users
 * List all registered users
 */
void WebServer::_handleAPIUsers(AsyncWebServerRequest* request) {
    Serial.println("[API] GET /api/users");
    
    // Open users.json from LittleFS
    JsonDocument doc;
    
    if (LittleFS.exists("/users.json")) {
        File file = LittleFS.open("/users.json", "r");
        DeserializationError error = deserializeJson(doc, file);
        file.close();
        
        if (error) {
            doc["users"] = JsonArray();
        }
    } else {
        // Create default structure
        JsonArray users = doc["users"].to<JsonArray>();
        JsonObject admin = users.add<JsonObject>();
        admin["uid"] = "DEFAULT_ADMIN";
        admin["name"] = "Admin";
        admin["type"] = "admin";
    }
    
    _sendJSON(request, 200, doc);
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
    
    String uid = request->getParam("uid")->value();
    Serial.print("[API] DELETE /api/users?uid=");
    Serial.println(uid);
    
    // Prevent deleting the default admin
    if (uid == "DEFAULT_ADMIN") {
        JsonDocument doc;
        doc["success"] = false;
        doc["message"] = "Cannot delete default admin";
        _sendJSON(request, 403, doc);
        return;
    }
    
    // Remove from AuthHandler
    bool removed = _auth->removeUser(uid);
    
    // Update users.json on LittleFS
    if (removed && LittleFS.exists("/users.json")) {
        File file = LittleFS.open("/users.json", "r");
        JsonDocument usersDoc;
        deserializeJson(usersDoc, file);
        file.close();
        
        JsonArray users = usersDoc["users"].as<JsonArray>();
        for (size_t i = 0; i < users.size(); i++) {
            if (users[i]["uid"].as<String>() == uid) {
                users.remove(i);
                break;
            }
        }
        
        File wFile = LittleFS.open("/users.json", "w");
        serializeJson(usersDoc, wFile);
        wFile.close();
    }
    
    _addLogEntry(uid, "Web", removed ? "success" : "fail");
    
    // Send response
    JsonDocument doc;
    doc["success"] = removed;
    doc["message"] = removed ? "User deleted" : "User not found";
    
    _sendJSON(request, removed ? 200 : 404, doc);
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
    String uid  = body["uid"]  | "";
    String type = body["type"] | "user";
    
    if (name.isEmpty() || pin.isEmpty() || uid.isEmpty()) {
        JsonDocument doc;
        doc["success"] = false;
        doc["message"] = "Missing required fields: name, pin, uid";
        _sendJSON(request, 400, doc);
        return;
    }
    
    // Add to AuthHandler (NVS)
    bool added = _auth->addUser(uid, pin, name);
    
    // Also persist to users.json on LittleFS
    if (added) {
        JsonDocument usersDoc;
        if (LittleFS.exists("/users.json")) {
            File file = LittleFS.open("/users.json", "r");
            deserializeJson(usersDoc, file);
            file.close();
        }
        
        if (!usersDoc["users"].is<JsonArray>()) {
            usersDoc["users"] = JsonArray();
        }
        
        JsonArray users = usersDoc["users"].as<JsonArray>();
        JsonObject newUser = users.add<JsonObject>();
        newUser["uid"]  = uid;
        newUser["name"] = name;
        newUser["type"] = type;
        
        File wFile = LittleFS.open("/users.json", "w");
        serializeJson(usersDoc, wFile);
        wFile.close();
    }
    
    _addLogEntry(name, "Web", "success");
    
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
    
    if (uid.isEmpty() || name.isEmpty()) {
        JsonDocument doc;
        doc["success"] = false;
        doc["message"] = "Missing required fields: uid, name";
        _sendJSON(request, 400, doc);
        return;
    }
    
    // Update in AuthHandler (remove and re-add)
    if (!pin.isEmpty()) {
        _auth->removeUser(uid);
        _auth->addUser(uid, pin, name);
    }
    
    // Update users.json on LittleFS
    if (LittleFS.exists("/users.json")) {
        JsonDocument usersDoc;
        File file = LittleFS.open("/users.json", "r");
        deserializeJson(usersDoc, file);
        file.close();
        
        JsonArray users = usersDoc["users"].as<JsonArray>();
        for (size_t i = 0; i < users.size(); i++) {
            if (users[i]["uid"].as<String>() == uid) {
                users[i]["name"] = name;
                if (!rfid.isEmpty()) users[i]["uid"] = rfid;
                break;
            }
        }
        
        File wFile = LittleFS.open("/users.json", "w");
        serializeJson(usersDoc, wFile);
        wFile.close();
    }
    
    JsonDocument doc;
    doc["success"] = true;
    doc["message"] = "User updated";
    _sendJSON(request, 200, doc);
}

/**
 * API: GET /api/rfid/scan
 * Return the last scanned RFID UID (for frontend enrollment polling)
 */
void WebServer::_handleAPIRfidScan(AsyncWebServerRequest* request) {
    String lastUID = _auth->getLastRFIDUID();
    
    JsonDocument doc;
    doc["uid"] = lastUID;
    doc["timestamp"] = millis();
    
    _sendJSON(request, 200, doc);
}

/**
 * API: GET /api/logs
 * Get activity logs
 */
void WebServer::_handleAPILogs(AsyncWebServerRequest* request) {
    Serial.println("[API] GET /api/logs");
    
    // Open logs.json from LittleFS
    JsonDocument doc;
    
    if (LittleFS.exists("/logs.json")) {
        File file = LittleFS.open("/logs.json", "r");
        DeserializationError error = deserializeJson(doc, file);
        file.close();
        
        if (error) {
            doc["logs"] = JsonArray();
        }
    } else {
        // Create empty log structure
        doc["logs"] = JsonArray();
    }
    
    _sendJSON(request, 200, doc);
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
    // Fallback to uptime-style time if sync is not ready yet.
    char timeStr[32];
    time_t now = time(nullptr);
    if (now > 1700000000) {
        struct tm utcTime;
        gmtime_r(&now, &utcTime);
        strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S UTC", &utcTime);
    } else {
        unsigned long sec = millis() / 1000;
        unsigned long m = (sec / 60) % 60;
        unsigned long h = (sec / 3600) % 24;
        snprintf(timeStr, sizeof(timeStr), "%02lu:%02lu (uptime)", h, m);
    }
    
    JsonObject entry = logs.add<JsonObject>();
    entry["time"]   = String(timeStr);
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
