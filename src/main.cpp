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
bool vibrationAlarmLogged = false;
bool doorTamperLogged = false;

enum PendingAccessMode {
    ACCESS_IDLE,
    ACCESS_WAIT_OTP,
    ACCESS_WAIT_BACKUP_PIN
};

PendingAccessMode pendingAccessMode = ACCESS_IDLE;
String pendingAccessUID = "";
String pendingAccessUserName = "";
String pendingAccessChatId = "";
String pendingOTP = "";
String pendingBackupPIN = "";
unsigned long pendingOTPStartedAtMs = 0;

String authPrompt = "";

static const unsigned long OTP_TTL_MS = 30000;          // 30 seconds
static const size_t OTP_LENGTH = 6;
static const size_t BACKUP_PIN_LENGTH = 4;

// Temporary Guest Code (Telegram-triggered, one-time use, auto-expiry)
String activeGuestCode = "";
unsigned long guestCodeGeneratedAtMs = 0;
bool isGuestCodeActive = false;
bool guestCodeExpiredNotified = false;

// Telegram polling (non-blocking cadence)
unsigned long lastTelegramPollMs = 0;
static const unsigned long TELEGRAM_POLL_INTERVAL_MS = 1000;

// Guest code constants
static const unsigned long GUEST_CODE_TTL_MS = 300000;  // 5 minutes
static const size_t GUEST_CODE_LENGTH = 4;

// ============================================================
// FORWARD DECLARATIONS
// ============================================================

void handleAuthResult(AuthResult result, const char* method);
String generateGuestCode();
void handleTelegramCommands();
void checkGuestCodeExpiry();
bool handleTemporaryGuestCode(const String& enteredPin);
String generateOTPCode();
void beginPendingAccessForRFID(const String& uid);
void cancelPendingAccess(const String& reason, bool logFailure = false);
bool handlePendingAccessKey(char key);
void grantAccess(const String& actor, const String& method);

// Shared guest-code status accessors for WebServer API
String getActiveGuestCode();
bool isTemporaryGuestCodeActive();
unsigned long getTemporaryGuestCodeRemainingMs();
String getAuthPrompt();
bool isPendingAccessActive();

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

    // Seed random source for guest code generation
    randomSeed(static_cast<unsigned long>(esp_random()));
}

// ============================================================
// MAIN LOOP
// ============================================================

void loop() {
    // Update all components (non-blocking — uses millis() internally)
    lockManager.update();       // Auto-lock timer, door state, LED blink
    securityManager.update();   // Buzzer pattern state machine
    authHandler.update();       // Factory reset button monitor
    webServer.update();         // Guest code expiry maintenance

    // Telegram command polling and guest-code lifecycle (non-blocking)
    handleTelegramCommands();
    checkGuestCodeExpiry();

    // Re-arm when lock is physically secured (handles auto-lock path too)
    if (lockManager.isLocked() && !systemArmed) {
        systemArmed = true;
        Serial.println("[SYSTEM] System re-armed");
    }
    
    // ─────────────────────────────────────────────────────────
    // SECURITY: Vibration Detection (while locked)
    // ─────────────────────────────────────────────────────────
    if (systemArmed && lockManager.isLocked()) {
        const bool vibrationDetected = securityManager.isVibrationDetected();

        if (vibrationDetected && !vibrationAlarmLogged) {
            Serial.println("[SYSTEM] 🚨 INTRUSION DETECTED - Vibration!");
            securityManager.startAlarm();
            vibrationAlarmLogged = true;

            webServer.logActivity("System", "Vibration Alarm", "alarm");
            
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

        if (!vibrationDetected) {
            vibrationAlarmLogged = false;
        }
    }
    
    // ─────────────────────────────────────────────────────────
    // SECURITY: Door Tamper Detection
    // ─────────────────────────────────────────────────────────
    if (systemArmed && lockManager.isDoorTampered() && !doorTamperLogged) {
        Serial.println("[SYSTEM] 🚨 DOOR TAMPERED - Opened while locked!");
        securityManager.startAlarm();
        doorTamperLogged = true;

        webServer.logActivity("System", "Door Tamper Alarm", "alarm");
        
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

    if (!lockManager.isDoorTampered()) {
        doorTamperLogged = false;
    }
    
    // ─────────────────────────────────────────────────────────
    // AUTHENTICATION: RFID Scan
    // ─────────────────────────────────────────────────────────
    AuthResult rfidResult = authHandler.checkRFID();
    if (rfidResult != AUTH_NONE) {
        if (rfidResult == AUTH_SUCCESS) {
            beginPendingAccessForRFID(authHandler.getLastRFIDUID());
        } else {
            handleAuthResult(rfidResult, "RFID");
        }
    }

    // OTP timeout handling (strict 30-second window)
    if (pendingAccessMode == ACCESS_WAIT_OTP) {
        const unsigned long now = millis();
        if ((now - pendingOTPStartedAtMs) >= OTP_TTL_MS) {
            securityManager.beep(3);
            webServer.logActivity(pendingAccessUserName, "OTP Timeout", "fail");
            Serial.println("[2FA] OTP expired (30s timeout)");
            cancelPendingAccess("OTP timeout", false);
        }
    }
    
    // ─────────────────────────────────────────────────────────
    // AUTHENTICATION: Keypad Input
    // ─────────────────────────────────────────────────────────
    char key = authHandler.getKeypadKey();
    if (key) {
        bool processedKey = false;
        
        if (key == '*') {
            // Clear buffer
            authHandler.clearBuffer();
            Serial.println("[SYSTEM] PIN buffer cleared");
            if (pendingAccessMode != ACCESS_IDLE) {
                cancelPendingAccess("User cancelled pending auth", false);
            }
            processedKey = true;
        }
        else if (key == '#') {
            if (pendingAccessMode != ACCESS_IDLE) {
                processedKey = handlePendingAccessKey(key);
            } else {
                // Legacy submit path: only allow guest PIN or duress from keypad-only context.
                const String enteredPin = authHandler.getBuffer();
                const bool usedGuestCode = handleTemporaryGuestCode(enteredPin);

                if (!usedGuestCode) {
                    AuthResult pinResult = authHandler.validatePIN(enteredPin);
                    if (pinResult == AUTH_DURESS) {
                        handleAuthResult(pinResult, "PIN");
                    } else if (enteredPin.length() > 0) {
                        securityManager.beep(3);
                        webServer.logActivity("Unknown User", "PIN-only Access Blocked", "fail");
                        Serial.println("[SYSTEM] PIN-only access blocked. Scan RFID first.");
                    }
                }
                processedKey = true;
            }
            authHandler.clearBuffer();
        }
        else if (key >= '0' && key <= '9') {
            if (pendingAccessMode != ACCESS_IDLE) {
                processedKey = handlePendingAccessKey(key);
            } else {
                // Append to buffer
                if (authHandler.getBuffer().length() < 8) {
                    authHandler.appendToBuffer(key);
                    processedKey = true;

                    // 3x3 keypad-compatible flow: auto-check temporary guest code
                    // immediately when 4 digits are entered (no '#' required).
                    const String enteredPin = authHandler.getBuffer();
                    if (enteredPin.length() == GUEST_CODE_LENGTH) {
                        const bool usedGuestCode = handleTemporaryGuestCode(enteredPin);
                        if (usedGuestCode) {
                            authHandler.clearBuffer();
                        }
                    }
                } else {
                    Serial.println("[SYSTEM][WARN] PIN buffer full - ignoring extra keypad digit");
                }
            }
        }

        if (processedKey) {
            securityManager.beep(1);  // Keypress feedback only for accepted input
        }
    }
    
    // Small yield delay — prevents watchdog reset on ESP32
    delay(10);
}

String generateOTPCode() {
    const int value = random(0, 1000000);
    char code[7];
    snprintf(code, sizeof(code), "%06d", value);
    return String(code);
}

void beginPendingAccessForRFID(const String& uid) {
    const String normalizedUid = uid;
    pendingAccessUID = normalizedUid;
    pendingAccessUserName = authHandler.getUserName(normalizedUid);
    pendingAccessChatId = authHandler.getUserTelegramChatId(normalizedUid);
    pendingBackupPIN = authHandler.getUserBackupPIN(normalizedUid);
    pendingOTP = "";
    pendingOTPStartedAtMs = 0;

    authHandler.clearBuffer();

    // Default to OTP mode; allow '#' to enter offline backup mode.
    pendingAccessMode = ACCESS_WAIT_OTP;
    authPrompt = "Enter OTP (or press # for Offline Backup)";

    bool otpDelivered = false;
    if (bot && pendingAccessChatId.length() > 0) {
        pendingOTP = generateOTPCode();
        pendingOTPStartedAtMs = millis();

        String otpMessage = "🔐 SecureLock OTP: " + pendingOTP + "\n";
        otpMessage += "Valid for 30 seconds.";
        bot->sendMessage(pendingAccessChatId, otpMessage, "");
        otpDelivered = true;

        securityManager.beep(1);
        webServer.logActivity(pendingAccessUserName, "OTP Sent", "success");
        Serial.print("[2FA] OTP sent to user chat ID: ");
        Serial.println(pendingAccessChatId);
    } else {
        securityManager.beep(3);
        webServer.logActivity(pendingAccessUserName, "OTP Delivery Failed", "fail");
        Serial.println("[2FA][WARN] OTP could not be delivered (missing chat ID or bot unavailable)");
    }

    if (!otpDelivered) {
        authPrompt = "OTP unavailable. Press # for Offline Backup PIN";
    }

    Serial.print("[2FA] Pending auth started for UID: ");
    Serial.println(pendingAccessUID);
}

void cancelPendingAccess(const String& reason, bool logFailure) {
    if (pendingAccessMode != ACCESS_IDLE && logFailure) {
        webServer.logActivity(pendingAccessUserName, reason, "fail");
    }

    pendingAccessMode = ACCESS_IDLE;
    pendingAccessUID = "";
    pendingAccessUserName = "";
    pendingAccessChatId = "";
    pendingOTP = "";
    pendingBackupPIN = "";
    pendingOTPStartedAtMs = 0;
    authPrompt = "";
    authHandler.clearBuffer();
}

void grantAccess(const String& actor, const String& method) {
    lockManager.unlock();
    securityManager.beep(2);
    authHandler.startRFIDCooldown();

    if (securityManager.isAlarming()) {
        securityManager.clearAlarm();
    }

    systemArmed = false;
    webServer.logActivity(actor, method, "success");
    Serial.print("[ACCESS] Granted via ");
    Serial.println(method);

    cancelPendingAccess("Access completed", false);
}

bool handlePendingAccessKey(char key) {
    if (pendingAccessMode == ACCESS_IDLE) {
        return false;
    }

    if (key == '#') {
        if (pendingAccessMode == ACCESS_WAIT_OTP) {
            pendingAccessMode = ACCESS_WAIT_BACKUP_PIN;
            authPrompt = "Enter Backup PIN";
            authHandler.clearBuffer();
            Serial.println("[OFFLINE] Offline backup mode selected");
            webServer.logActivity(pendingAccessUserName, "Offline Backup Mode Selected", "success");
            return true;
        }

        return true;
    }

    if (key < '0' || key > '9') {
        return false;
    }

    // Input length is mode-dependent: OTP=6, Backup PIN=4
    const size_t maxLen = (pendingAccessMode == ACCESS_WAIT_BACKUP_PIN) ? BACKUP_PIN_LENGTH : OTP_LENGTH;
    if (authHandler.getBuffer().length() >= maxLen) {
        return false;
    }

    authHandler.appendToBuffer(key);
    const String entered = authHandler.getBuffer();

    if (pendingAccessMode == ACCESS_WAIT_OTP && entered.length() == OTP_LENGTH) {
        if (pendingOTP.length() == OTP_LENGTH && entered == pendingOTP) {
            grantAccess(pendingAccessUserName, "RFID + OTP");
        } else {
            securityManager.beep(3);
            webServer.logActivity(pendingAccessUserName, "OTP Verification Failed", "fail");
            Serial.println("[2FA] OTP verification failed");
            cancelPendingAccess("OTP failed", false);
        }
    }

    if (pendingAccessMode == ACCESS_WAIT_BACKUP_PIN && entered.length() == BACKUP_PIN_LENGTH) {
        if (pendingBackupPIN.length() == BACKUP_PIN_LENGTH && entered == pendingBackupPIN) {
            grantAccess(pendingAccessUserName, "RFID + Offline Backup PIN");
        } else {
            securityManager.beep(3);
            webServer.logActivity(pendingAccessUserName, "Offline Backup PIN Failed", "fail");
            Serial.println("[OFFLINE] Backup PIN verification failed");
            cancelPendingAccess("Backup PIN failed", false);
        }
    }

    return true;
}

/**
 * Generate a random 4-digit numeric guest code (0000-9999)
 */
String generateGuestCode() {
    const int value = random(0, 10000);
    char code[5];
    snprintf(code, sizeof(code), "%04d", value);
    return String(code);
}

/**
 * Poll Telegram bot and process admin commands (non-blocking cadence)
 */
void handleTelegramCommands() {
    if (!bot || !webServer.isConnected()) {
        return;
    }

    const unsigned long now = millis();
    if ((now - lastTelegramPollMs) < TELEGRAM_POLL_INTERVAL_MS) {
        return;
    }
    lastTelegramPollMs = now;

    int numNewMessages = bot->getUpdates(bot->last_message_received + 1);
    while (numNewMessages > 0) {
        for (int i = 0; i < numNewMessages; i++) {
            const String chatId = bot->messages[i].chat_id;
            String text = bot->messages[i].text;
            text.trim();

            // Strict authorization: only ADMIN_CHAT_ID can manage guest codes.
            if (chatId != String(ADMIN_CHAT_ID)) {
                continue;
            }

            if (text == "/guest_code") {
                activeGuestCode = generateGuestCode();
                guestCodeGeneratedAtMs = now;
                isGuestCodeActive = true;
                guestCodeExpiredNotified = false;

                securityManager.beep(2);
                webServer.logActivity("Admin (Telegram)", "Guest Code Generated (" + activeGuestCode + ")", "success");

                const String reply = "✅ Guest code generated: " + activeGuestCode +
                                     ". This code will automatically expire in 5 minutes.";
                bot->sendMessage(ADMIN_CHAT_ID, reply, "");
                Serial.print("[TELEGRAM] Temporary guest code generated: ");
                Serial.println(activeGuestCode);
            } else if (text == "/admin_open") {
                lockManager.unlock();
                authHandler.startRFIDCooldown();

                if (securityManager.isAlarming()) {
                    securityManager.clearAlarm();
                }

                securityManager.beep(2);
                systemArmed = false;
                cancelPendingAccess("Admin override", false);

                webServer.logActivity("Admin (Telegram)", "Emergency Override", "success");
                bot->sendMessage(ADMIN_CHAT_ID, "🔓 Emergency override activated.", "");
                Serial.println("[TELEGRAM] Admin emergency override executed");
            }
        }

        numNewMessages = bot->getUpdates(bot->last_message_received + 1);
    }
}

/**
 * Expire temporary guest code after TTL and notify admin exactly once
 */
void checkGuestCodeExpiry() {
    if (!isGuestCodeActive) {
        return;
    }

    const unsigned long now = millis();
    if ((now - guestCodeGeneratedAtMs) >= GUEST_CODE_TTL_MS) {
        isGuestCodeActive = false;
        activeGuestCode = "";

        if (!guestCodeExpiredNotified) {
            guestCodeExpiredNotified = true;
            if (bot) {
                bot->sendMessage(ADMIN_CHAT_ID, "⏳ Guest code expired and wiped from memory.", "");
            }
            Serial.println("[GUEST] Temporary guest code expired and wiped");
        }
    }
}

/**
 * Validate one-time temporary guest code from keypad buffer.
 * Returns true if guest code matched and was consumed.
 */
bool handleTemporaryGuestCode(const String& enteredPin) {
    if (!isGuestCodeActive || activeGuestCode.length() != GUEST_CODE_LENGTH) {
        return false;
    }

    if (enteredPin.length() != GUEST_CODE_LENGTH) {
        return false;
    }

    if (enteredPin != activeGuestCode) {
        return false;
    }

    // SUCCESS: unlock and immediately wipe code to prevent replay.
    lockManager.unlock();
    securityManager.beep(2);
    authHandler.startRFIDCooldown();

    if (securityManager.isAlarming()) {
        securityManager.clearAlarm();
    }

    systemArmed = false;

    isGuestCodeActive = false;
    activeGuestCode = "";
    guestCodeExpiredNotified = false;

    if (bot) {
        bot->sendMessage(ADMIN_CHAT_ID, "🔓 Door unlocked using temporary Guest Code.", "");
    }

    webServer.logActivity("Guest", "Temporary Guest Code Access Granted", "success");
    Serial.println("[GUEST] Temporary guest code accepted, door unlocked, code wiped");

    return true;
}

String getActiveGuestCode() {
    return isGuestCodeActive ? activeGuestCode : "";
}

String getAuthPrompt() {
    return authPrompt;
}

bool isPendingAccessActive() {
    return pendingAccessMode != ACCESS_IDLE;
}

bool isTemporaryGuestCodeActive() {
    return isGuestCodeActive;
}

unsigned long getTemporaryGuestCodeRemainingMs() {
    if (!isGuestCodeActive) {
        return 0;
    }

    const unsigned long elapsed = millis() - guestCodeGeneratedAtMs;
    if (elapsed >= GUEST_CODE_TTL_MS) {
        return 0;
    }

    return GUEST_CODE_TTL_MS - elapsed;
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

            if (String(method) == "RFID") {
                const String uid = authHandler.getLastRFIDUID();
                const String userName = authHandler.getUserName(uid);
                const String actor = (userName == "Unknown" || userName.length() == 0) ? "Registered RFID" : userName;
                webServer.logActivity(actor, "Access Granted (RFID)", "success");
            } else {
                webServer.logActivity("Registered User", "Access Granted (PIN)", "success");
            }
            
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

            if (String(method) == "RFID") {
                String deniedUid = authHandler.getLastRFIDUID();
                if (deniedUid.length() == 0) {
                    deniedUid = "Unknown RFID";
                }
                webServer.logActivity(deniedUid, "RFID Access Denied", "fail");
            } else {
                webServer.logActivity("Unknown User", "Access Denied (Invalid PIN)", "fail");
            }
            
            securityManager.beep(3);
            break;
            
        case AUTH_DURESS:
            Serial.println("[SYSTEM] 🆘 DURESS CODE DETECTED!");
            Serial.println("[SYSTEM] → Unlocking door (normal appearance)");
            Serial.println("[SYSTEM] → SILENT ALARM ACTIVATED");

            webServer.logActivity("Duress User", "Duress Code Entered", "alarm");
            
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
    
    // Re-arm handled in main loop to cover auto-lock path reliably.
}
