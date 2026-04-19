/**
 * ============================================================
 * WebServer - Network & API Component
 * ============================================================
 * 
 * PURPOSE: Manages WiFi, web server, API endpoints, and file serving
 * 
 * DEPENDENCIES:
 *   - ESPAsyncWebServer (HTTP server)
 *   - LittleFS (filesystem)
 *   - ArduinoJson (JSON serialization)
 *   - LockManager (hardware status)
 *   - SecurityManager (alarm status)
 *   - AuthHandler (user management)
 * 
 * FILE SERVING:
 *   GET /                 → /html/pages/dashboard.html (canonical dashboard page)
 *   GET /css/style.css    → /css/style.css (modular stylesheet aggregator)
 *   Other static assets   → served by onNotFound LittleFS fallback
 * 
 * API ENDPOINTS:
 *   POST   /api/auth/login  → Authenticate admin and issue API session token
 *   POST   /api/auth/logout → Revoke active API session token
 *   GET    /api/auth/status → Current API authentication status
 *   GET    /api/status      → System status JSON
 *   POST   /api/unlock      → Remote unlock (emergency override)
 *   POST   /api/guest-code  → Generate/retrieve active guest PIN (web + Telegram synchronized)
 *   GET    /api/users       → List all registered users (+ first/middle/last name parts, dynamic admin-chat tag)
 *   POST   /api/users       → Add new user (JSON body: firstName,middleName?,lastName OR name, pin(4-digit), uid)
 *   PUT    /api/users       → Edit existing user (JSON body: uid, firstName,middleName?,lastName OR name, pin(4-digit))
 *   DELETE /api/users?uid=X → Delete user by UID (admin protected)
 *   GET    /api/logs        → Get activity logs
 *   GET    /api/diagnostics → Hardware diagnostics (RFID/keypad/buzzer)
 *   DELETE /api/logs        → Clear all activity logs
 *   POST   /api/rfid/enroll/start → Begin enrollment scan window (suppresses auth-deny workflow)
 *   POST   /api/rfid/enroll/stop  → End enrollment scan window
 *   GET    /api/rfid/scan   → Poll RFID reader for card enrollment
 * 
 * FEATURES:
 *   - WiFi connection management
 *   - LittleFS file serving with MIME types
 *   - RESTful API with JSON responses
 *   - Restricted CORS headers for browser clients
 *   - Component integration
 * 
 * USAGE:
 *   WebServer webServer(&lockManager, &securityManager, &authHandler);
 *   webServer.init();
 *   // No update needed - fully async
 * 
 * ============================================================
 */

#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include "LockManager.h"
#include "SecurityManager.h"
#include "AuthHandler.h"

class WebServer {
public:
    // Constructor - Requires component references
    WebServer(LockManager* lockManager, SecurityManager* securityManager, AuthHandler* authHandler);
    
    // Lifecycle
    void init();
    void init(const char* ssid, const char* password); // legacy compatibility overload
    void update();
    bool isConnected() const;
    String getIPAddress() const;
    void markEmergencyOverride();
    unsigned long getEmergencyCooldownRemainingMs() const;

    // Runtime activity logging hook (used by main authentication flow)
    void logActivity(const String& user, const String& method, const String& status);
    
private:
    // Component references
    LockManager* _lock;
    SecurityManager* _security;
    AuthHandler* _auth;
    AsyncWebServer _server;
    
    // WiFi state
    bool _wifiConnected;
    String _ipAddress;
    
    // Emergency command management
    unsigned long _lastEmergencyUnlockMs;
    String _apiSessionToken;
    unsigned long _apiSessionIssuedAtMs;
    unsigned long _apiSessionExpiresAtMs;
    String _apiSessionActorLabel;
    String _apiSessionAdminUsername;
    int _apiSessionAdminSlot;
    int _authFailedAttempts;
    unsigned long _authLockoutUntilMs;

    static constexpr unsigned long EMERGENCY_COOLDOWN_MS = 5000;
    static constexpr unsigned long API_SESSION_TTL_MS = 15UL * 60UL * 1000UL;
    static constexpr int AUTH_MAX_FAILED_ATTEMPTS = 5;
    static constexpr unsigned long AUTH_LOCKOUT_MS = 5UL * 60UL * 1000UL;
    
    // Initialization helpers
    void _initWiFi();
    void _initFileSystem();
    void _setupRoutes();
    
    // Route handlers - Static files
    void _handleRoot(AsyncWebServerRequest* request);
    void _handleCSS(AsyncWebServerRequest* request);
    void _handleNotFound(AsyncWebServerRequest* request);
    
    // Route handlers - API
    void _handleAPIAuthLogin(AsyncWebServerRequest* request, uint8_t* data, size_t len);
    void _handleAPIAuthLogout(AsyncWebServerRequest* request);
    void _handleAPIAuthStatus(AsyncWebServerRequest* request);
    void _handleAPIStatus(AsyncWebServerRequest* request);
    void _handleAPIUnlock(AsyncWebServerRequest* request);
    void _handleAPIGuestCode(AsyncWebServerRequest* request);
    void _handleAPIUsers(AsyncWebServerRequest* request);
    void _handleAPIAddUser(AsyncWebServerRequest* request, uint8_t* data, size_t len);
    void _handleAPIEditUser(AsyncWebServerRequest* request, uint8_t* data, size_t len);
    void _handleAPIDeleteUser(AsyncWebServerRequest* request);
    void _handleAPIResetUsers(AsyncWebServerRequest* request, uint8_t* data, size_t len);
    void _handleAPILogs(AsyncWebServerRequest* request);
    void _handleAPIClearLogs(AsyncWebServerRequest* request);
    void _handleAPIRfidEnrollStart(AsyncWebServerRequest* request);
    void _handleAPIRfidEnrollStop(AsyncWebServerRequest* request);
    void _handleAPIRfidScan(AsyncWebServerRequest* request);
    void _handleAPIDiagnostics(AsyncWebServerRequest* request);
    
    // Activity logging
    struct QueuedLogEntry {
        char user[48];
        char method[64];
        char status[16];
    };

    static constexpr size_t LOG_QUEUE_CAPACITY = 64;
    static constexpr uint8_t LOG_FLUSH_BURST = 8;
    static constexpr uint8_t LOG_FLUSH_MAX_BATCH = 16;
    static constexpr unsigned long USERS_STATS_CACHE_TTL_MS = 5000UL;

    QueuedLogEntry _queuedLogs[LOG_QUEUE_CAPACITY];
    size_t _queuedLogHead = 0;
    size_t _queuedLogTail = 0;
    size_t _queuedLogCount = 0;
    unsigned long _droppedQueuedLogs = 0;
    portMUX_TYPE _queuedLogMux = portMUX_INITIALIZER_UNLOCKED;

    bool _usersStatsCacheValid = false;
    unsigned long _usersStatsCacheAtMs = 0;
    int _cachedRawUsers = 0;
    int _cachedUniqueUsers = 0;
    int _cachedInvalidUsers = 0;
    int _cachedDuplicateUsers = 0;

    void _addLogEntry(const String& user, const String& method, const String& status);
    void _queueLogEntry(const String& user, const String& method, const String& status);
    bool _popQueuedLog(QueuedLogEntry* outEntry);
    void _clearQueuedLogs();
    void _flushQueuedLogs(uint8_t maxEntries = LOG_FLUSH_BURST);
    String _normalizeLogStatus(const String& status) const;
    void _writeLogEntryToStorage(const String& user, const String& method, const String& status);
    void _writeLogBatchToStorage(const QueuedLogEntry* entries, size_t count);
    void _invalidateUsersStatsCache();
    
    // Utilities
    String _getMimeType(const String& filename);
    bool _clientAcceptsGzip(AsyncWebServerRequest* request) const;
    bool _serveStaticAsset(AsyncWebServerRequest* request, const String& path, bool cacheable);
    void _sendJSON(AsyncWebServerRequest* request, int code, const JsonDocument& doc);
    void _addCORSHeaders(AsyncWebServerRequest* request, AsyncWebServerResponse* response);
    void _addSecurityHeaders(AsyncWebServerResponse* response);
    void _addStaticCacheHeaders(AsyncWebServerResponse* response);
    void _addNoCacheHeaders(AsyncWebServerResponse* response);
    bool _isAllowedCORSOrigin(AsyncWebServerRequest* request, const String& origin) const;
    bool _isApiSessionValid() const;
    String _extractBearerToken(AsyncWebServerRequest* request) const;
    bool _requireApiAuth(AsyncWebServerRequest* request);
    String _generateApiSessionToken() const;
    void _invalidateApiSession();
    String _activeApiActorLabel() const;
    String _activeApiAdminUsername() const;
    int _activeApiAdminSlot() const;
    bool _secureEquals(const String& a, const String& b) const;
    unsigned long _remainingCooldownMs(unsigned long lastActionMs, unsigned long cooldownMs) const;
    bool _syncUsersFileFromAuth(JsonDocument* responseDoc = nullptr);
    void _collectUsersStorageStats(int* rawCount, int* uniqueCount, int* invalidCount, int* duplicateCount);
    void _cleanupGuestUsers();
};

#endif // WEB_SERVER_H
