/**
 * ============================================================
 * SecureLock - ESP32 Smart Security System
 * ============================================================
 * 
 * ARCHITECTURE: Component-Based Modular Design
 * 
 * COMPONENTS:
 *   - LockManager: Hardware control (Relay, LED, Reed Switch)
 *   - SecurityManager: Sensors & alarms (Vibration, Buzzer)
 *   - AuthHandler: Authentication (RFID, Keypad, Duress)
 *   - WebServer: Network layer (WiFi, API, Dashboard)
 * 
 * FEATURES:
 *   - Multi-factor authentication (RFID + PIN)
 *   - Duress code detection (9999) - Silent alarm
 *   - Vibration detection & alarm
 *   - Door tamper detection
 *   - Web dashboard (Glassmorphism UI)
 *   - Factory reset (GPIO 0 long-press 10s)
 * 
 * HARDWARE:
 *   - ESP32 DevKit V1 (30-Pin)
 *   - GPIO assignments are centralized in include/hardware_pins.h
 *   - RFID RC522: SPI (SS=5, RST=4, SCK=18, MOSI=23, MISO=19)
 *   - Keypad 4x4 uses safe scan mapping (Rows[34,35,36,39], Cols[32,33,25,26])
 * 
 * DEPLOYMENT:
 *   1. Edit include/secrets.h (WiFi credentials)
 *   2. Upload filesystem: pio run --target uploadfs
 *   3. Upload firmware: pio run --target upload
 *   4. Monitor: pio device monitor
 * 
 * ============================================================
 */

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <LockManager.h>
#include <SecurityManager.h>
#include <AuthHandler.h>
#include <WebServer.h>
#include "secrets.h"

// ============================================================
// GLOBAL COMPONENTS
// ============================================================

LockManager lockManager;
SecurityManager securityManager;
AuthHandler authHandler;
WebServer webServer(&lockManager, &securityManager, &authHandler);

// Telegram bot for notifications
WiFiClientSecure telegramClient;
UniversalTelegramBot* bot = nullptr;

// ============================================================
// STATE VARIABLES
// ============================================================

bool systemArmed = true;        // System armed for intrusion detection
String currentUser = "";        // Current authenticated user

// ============================================================
// FORWARD DECLARATIONS
// ============================================================

void handleAuthResult(AuthResult result, const char* method);

// ============================================================
// SETUP
// ============================================================

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println();
    Serial.println("════════════════════════════════════════════════════════");
    Serial.println("       SecureLock - ESP32 Smart Security System          ");
    Serial.println("       Component-Based Architecture v2.0                ");
    Serial.println("════════════════════════════════════════════════════════");
    Serial.println();
    
    // Initialize all components
    Serial.println("[INIT] Starting component initialization...");
    Serial.println();
    
    lockManager.init();
    securityManager.init();
    authHandler.init();
    webServer.init(WIFI_SSID, WIFI_PASSWORD);
    
    // Initialize Telegram bot (after WiFi connection)
    if (webServer.isConnected()) {
        telegramClient.setInsecure();  // For testing - use setCACert() in production
        bot = new UniversalTelegramBot(BOT_TOKEN, telegramClient);
        Serial.println("[TELEGRAM] ✓ Bot initialized");
        Serial.print("[TELEGRAM] Admin Chat ID: ");
        Serial.println(ADMIN_CHAT_ID);
    }
    
    Serial.println();
    Serial.println("════════════════════════════════════════════════════════");
    Serial.println("✓ ALL COMPONENTS INITIALIZED");
    Serial.println("════════════════════════════════════════════════════════");
    
    if (webServer.isConnected()) {
        Serial.println();
        Serial.println("🌐 WEB DASHBOARD READY");
        Serial.print("   URL: http://");
        Serial.print(webServer.getIPAddress());
        Serial.println("/");
        Serial.println();
        Serial.println("📱 API ENDPOINTS:");
        Serial.print("   GET  http://");
        Serial.print(webServer.getIPAddress());
        Serial.println("/api/status");
        Serial.print("   POST http://");
        Serial.print(webServer.getIPAddress());
        Serial.println("/api/unlock");
        Serial.println();
    }
    
    Serial.println("🔒 SYSTEM ARMED - Monitoring all sensors");
    Serial.println("════════════════════════════════════════════════════════");
    Serial.println();
    
    // Ready beep
    securityManager.beep(2);
}

// ============================================================
// MAIN LOOP
// ============================================================

void loop() {
    // Update all components (non-blocking — uses millis() internally)
    lockManager.update();       // Auto-lock timer, door state, LED blink
    securityManager.update();   // Buzzer pattern state machine
    authHandler.update();       // Factory reset button monitor
    
    // ─────────────────────────────────────────────────────────
    // SECURITY: Vibration Detection (while locked)
    // ─────────────────────────────────────────────────────────
    if (systemArmed && lockManager.isLocked()) {
        if (securityManager.isVibrationDetected()) {
            Serial.println("[SYSTEM] 🚨 INTRUSION DETECTED - Vibration!");
            securityManager.startAlarm();
            
            // Send Telegram alert
            if (bot) {
                String message = "🚨 SECURITY ALERT\n\n";
                message += "Intrusion Detected: Vibration Sensor Triggered\n";
                message += "Time: " + String(millis() / 1000) + "s\n";
                message += "Status: ALARM ACTIVE\n\n";
                message += "Check dashboard: http://" + webServer.getIPAddress();
                bot->sendMessage(ADMIN_CHAT_ID, message, "");
                Serial.println("[TELEGRAM] Alert sent to admin");
            }
        }
    }
    
    // ─────────────────────────────────────────────────────────
    // SECURITY: Door Tamper Detection
    // ─────────────────────────────────────────────────────────
    if (systemArmed && lockManager.isDoorTampered()) {
        Serial.println("[SYSTEM] 🚨 DOOR TAMPERED - Opened while locked!");
        securityManager.startAlarm();
        
        // Send Telegram alert
        if (bot) {
            String message = "🚨 SECURITY ALERT\n\n";
            message += "Door Tampering Detected\n";
            message += "Door opened while locked!\n";
            message += "Time: " + String(millis() / 1000) + "s\n\n";
            message += "Check dashboard: http://" + webServer.getIPAddress();
            bot->sendMessage(ADMIN_CHAT_ID, message, "");
            Serial.println("[TELEGRAM] Tamper alert sent");
        }
    }
    
    // ─────────────────────────────────────────────────────────
    // AUTHENTICATION: RFID Scan
    // ─────────────────────────────────────────────────────────
    AuthResult rfidResult = authHandler.checkRFID();
    if (rfidResult != AUTH_NONE) {
        handleAuthResult(rfidResult, "RFID");
    }
    
    // ─────────────────────────────────────────────────────────
    // AUTHENTICATION: Keypad Input
    // ─────────────────────────────────────────────────────────
    char key = authHandler.getKeypadKey();
    if (key) {
        securityManager.beep(1);  // Keypress feedback
        
        if (key == '*') {
            // Clear buffer
            authHandler.clearBuffer();
            Serial.println("[SYSTEM] PIN buffer cleared");
        }
        else if (key == '#') {
            // Submit PIN
            AuthResult pinResult = authHandler.validatePIN();
            handleAuthResult(pinResult, "PIN");
            authHandler.clearBuffer();
        }
        else if (key >= '0' && key <= '9') {
            // Append to buffer
            authHandler.appendToBuffer(key);
        }
    }
    
    // Small yield delay — prevents watchdog reset on ESP32
    delay(10);
}

// ============================================================
// HELPER FUNCTIONS
// ============================================================

/**
 * Handle authentication result
 */
void handleAuthResult(AuthResult result, const char* method) {
    switch (result) {
        case AUTH_SUCCESS:
            Serial.print("[SYSTEM] ✅ ACCESS GRANTED via ");
            Serial.println(method);
            
            // Unlock door
            lockManager.unlock();
            securityManager.beep(2);

            // Enforce RFID anti-spam cooldown while lock auto-timer is active
            authHandler.startRFIDCooldown();
            
            // Clear any alarms
            if (securityManager.isAlarming()) {
                securityManager.clearAlarm();
            }
            
            systemArmed = false;  // Disarm temporarily
            break;
            
        case AUTH_DENIED:
            Serial.print("[SYSTEM] ❌ ACCESS DENIED via ");
            Serial.println(method);
            
            securityManager.beep(3);
            break;
            
        case AUTH_DURESS:
            Serial.println("[SYSTEM] 🆘 DURESS CODE DETECTED!");
            Serial.println("[SYSTEM] → Unlocking door (normal appearance)");
            Serial.println("[SYSTEM] → SILENT ALARM ACTIVATED");
            
            // Unlock door normally (appears legitimate)
            lockManager.unlock();
            securityManager.beep(2);  // Normal success beep
            authHandler.startRFIDCooldown();
            
            // Send SILENT Telegram alert to admin/authorities
            if (bot) {
                String message = "🆘 DURESS CODE ACTIVATED\n\n";
                message += "⚠️ SILENT ALARM - User under duress\n";
                message += "Method: " + String(method) + "\n";
                message += "Time: " + String(millis() / 1000) + "s\n\n";
                message += "Door unlocked normally (to appear legitimate)\n";
                message += "IMMEDIATE RESPONSE REQUIRED\n\n";
                message += "Location: http://" + webServer.getIPAddress();
                bot->sendMessage(ADMIN_CHAT_ID, message, "");
                Serial.println("[TELEGRAM] 🆘 Duress alert sent silently");
            }
            
            // Clear visible alarm (maintain cover)
            if (securityManager.isAlarming()) {
                securityManager.stopAlarm();
            }
            
            systemArmed = false;
            break;
            
        case AUTH_NONE:
            // No action
            break;
    }
    
    // Re-arm system when door locks
    if (lockManager.isLocked() && !systemArmed) {
        systemArmed = true;
        Serial.println("[SYSTEM] System re-armed");
    }
}
