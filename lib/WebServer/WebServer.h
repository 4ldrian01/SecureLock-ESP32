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
 *   GET /                 → /html/index.html
 *   GET /css/style.css    → /css/style.css
 *   GET /js/main.js       → /js/main.js (modular entrypoint)
 *   GET /js/script.js     → /js/script.js (legacy compatibility shim)
 * 
 * API ENDPOINTS:
 *   GET    /api/status      → System status JSON
 *   POST   /api/unlock      → Remote unlock (emergency override)
 *   POST   /api/guest-code  → Generate temporary guest PIN (5-min)
 *   GET    /api/users       → List all registered users
 *   POST   /api/users       → Add new user (JSON body: name, pin(4-digit), uid)
 *   PUT    /api/users       → Edit existing user (JSON body: uid, name, pin(4-digit))
 *   DELETE /api/users?uid=X → Delete user by UID (admin protected)
 *   GET    /api/logs        → Get activity logs
 *   GET    /api/diagnostics → Hardware diagnostics (RFID/keypad/buzzer)
 *   DELETE /api/logs        → Clear all activity logs
 *   GET    /api/rfid/scan   → Poll RFID reader for card enrollment
 * 
 * FEATURES:
 *   - WiFi connection management
 *   - LittleFS file serving with MIME types
 *   - RESTful API with JSON responses
 *   - CORS headers for development
 *   - Component integration
 * 
 * USAGE:
 *   WebServer webServer(&lockManager, &securityManager, &authHandler);
 *   webServer.init("SSID", "PASSWORD");
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
#include "LockManager.h"
#include "SecurityManager.h"
#include "AuthHandler.h"

class WebServer {
public:
    // Constructor - Requires component references
    WebServer(LockManager* lockManager, SecurityManager* securityManager, AuthHandler* authHandler);
    
    // Lifecycle
    void init(const char* ssid, const char* password);
    void update();
    bool isConnected() const;
    String getIPAddress() const;
    void markEmergencyOverride();

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
    
    // Guest code management
    String _guestCode;
    unsigned long _guestCodeExpiry;
    unsigned long _lastEmergencyUnlockMs;
    unsigned long _lastGuestCodeRequestMs;
    unsigned long _lastRfidScanServedMs;

    static constexpr unsigned long EMERGENCY_COOLDOWN_MS = 5000;
    static constexpr unsigned long GUEST_CODE_COOLDOWN_MS = 300000;
    
    // Initialization helpers
    void _initWiFi(const char* ssid, const char* password);
    void _initFileSystem();
    void _setupRoutes();
    
    // Route handlers - Static files
    void _handleRoot(AsyncWebServerRequest* request);
    void _handleCSS(AsyncWebServerRequest* request);
    void _handleJS(AsyncWebServerRequest* request);
    void _handleNotFound(AsyncWebServerRequest* request);
    
    // Route handlers - API
    void _handleAPIStatus(AsyncWebServerRequest* request);
    void _handleAPIUnlock(AsyncWebServerRequest* request);
    void _handleAPIGuestCode(AsyncWebServerRequest* request);
    void _handleAPIUsers(AsyncWebServerRequest* request);
    void _handleAPIAddUser(AsyncWebServerRequest* request, uint8_t* data, size_t len);
    void _handleAPIEditUser(AsyncWebServerRequest* request, uint8_t* data, size_t len);
    void _handleAPIDeleteUser(AsyncWebServerRequest* request);
    void _handleAPILogs(AsyncWebServerRequest* request);
    void _handleAPIClearLogs(AsyncWebServerRequest* request);
    void _handleAPIRfidScan(AsyncWebServerRequest* request);
    void _handleAPIDiagnostics(AsyncWebServerRequest* request);
    
    // Activity logging
    void _addLogEntry(const String& user, const String& method, const String& status);
    
    // Utilities
    String _getMimeType(const String& filename);
    void _sendJSON(AsyncWebServerRequest* request, int code, const JsonDocument& doc);
    void _addCORSHeaders(AsyncWebServerResponse* response);
    String _generateGuestCode();
    unsigned long _remainingCooldownMs(unsigned long lastActionMs, unsigned long cooldownMs) const;
    void _cleanupGuestUsers();
    void _expireGuestCodeIfNeeded();
};

#endif // WEB_SERVER_H
