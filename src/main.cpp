#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <LockManager.h>
#include <SecurityManager.h>
#include <AuthHandler.h>
#include <WebServer.h>
#include "secrets.h"

LockManager lockManager;
SecurityManager securityManager;
AuthHandler authHandler;
WebServer webServer(&lockManager, &securityManager, &authHandler);

WiFiClientSecure telegramClient;
UniversalTelegramBot* bot = nullptr;

enum TelegramRole {
    ROLE_UNKNOWN,
    ROLE_USER,
    ROLE_ADMIN
};

enum KeypadState {
    STATE_IDLE,
    STATE_AWAITING_2FA
};

KeypadState keypadState = STATE_IDLE;
bool awaitingOfflineBackupMode = false;

String guestCode = "";
bool guestCodeActive = false;
unsigned long guestCodeIssuedAtMs = 0;
static const unsigned long GUEST_CODE_TTL_MS = 300000;

String pendingUID = "";
String pendingUserName = "";
String pendingUserChatId = "";
String pendingBackupPin = "";
String pendingOtp = "";
unsigned long pendingOtpIssuedAtMs = 0;
String keypadBuffer = "";
String authPrompt = "";

static const size_t OTP_LENGTH = 6;
static const size_t BACKUP_PIN_LENGTH = 4;
static const char* DURESS_CODE = "2580";
static const unsigned long OTP_TTL_MS = 30000;

unsigned long lastTelegramPollMs = 0;
static const unsigned long TELEGRAM_POLL_INTERVAL_MS = 1000;
static const unsigned long EMERGENCY_LOCKOUT_MS = 5000;
int lastHandledTelegramUpdateId = 0;

bool isLockdown = false;

// Part 5 mechanics: auto-relock verification and theft watchdog window.
bool pendingDoorSecuredLog = false;
bool previousDoorOpen = false;
unsigned long theftWindowStartMs = 0;
int theftStrikeCount = 0;
bool theftAlertSent = false;
static const unsigned long THEFT_WINDOW_MS = 2000;
static const int THEFT_STRIKE_THRESHOLD = 5;

void clearPending2FA(const String& reason, bool logFailure);
String generateNumericCode(size_t length);
void enterAwaiting2FAForUser(const String& uid);
void grantAccess(const String& actor, const String& method);
bool evaluateAwaiting2FABuffer(bool explicitSubmit);
void handleTelegramCommands();
void checkGuestCodeExpiry();
void processKeypad();
TelegramRole resolveTelegramRole(const String& chatId);
void handleAdminCommand(const String& chatId, const String& text);
void handleUserCommand(const String& chatId, const String& text);
void sendDuressAlert(String userName);
void sendTheftAlert();
void sendOTP(String chatID, String otp);
String normalizeTelegramCommand(const String& rawText);

String getActiveGuestCode();
bool isTemporaryGuestCodeActive();
unsigned long getTemporaryGuestCodeRemainingMs();
String getAuthPrompt();
bool isPendingAccessActive();

void setup() {
    Serial.begin(115200);

    lockManager.init();
    securityManager.init();
    authHandler.init();
    webServer.init(WIFI_SSID, WIFI_PASSWORD);

    if (webServer.isConnected()) {
        telegramClient.setInsecure();
        bot = new UniversalTelegramBot(BOT_TOKEN, telegramClient);
        Serial.println("[TELEGRAM] Bot initialized");
    }

    securityManager.beep(2);
    randomSeed(static_cast<unsigned long>(esp_random()));
    previousDoorOpen = lockManager.isDoorOpen();
}

void loop() {
    lockManager.update();
    securityManager.update();
    authHandler.update();
    webServer.update();

    handleTelegramCommands();
    checkGuestCodeExpiry();

    // Part 5A: once the user physically closes the door after an unlock,
    // record that the door is secured using reed switch transition.
    const bool currentDoorOpen = lockManager.isDoorOpen();
    if (previousDoorOpen && !currentDoorOpen && pendingDoorSecuredLog) {
        webServer.logActivity("System", "Door Physically Closed & Secured", "success");
        pendingDoorSecuredLog = false;
    }
    previousDoorOpen = currentDoorOpen;

    // Part 5B: anti-theft debounce window while lock is physically secured.
    if (lockManager.isLocked()) {
        const bool vibrationStrike = securityManager.pollVibrationStrike();
        if (vibrationStrike) {
            const unsigned long now = millis();
            if (theftWindowStartMs == 0 || (now - theftWindowStartMs) > THEFT_WINDOW_MS) {
                theftWindowStartMs = now;
                theftStrikeCount = 0;
                theftAlertSent = false;
            }

            theftStrikeCount++;
            if (theftStrikeCount > THEFT_STRIKE_THRESHOLD && !theftAlertSent) {
                securityManager.startAlarm();
                webServer.logActivity("System", "Theft Attempt Detected", "alarm");

                sendTheftAlert();

                theftAlertSent = true;
            }
        }

        if (theftWindowStartMs > 0 && (millis() - theftWindowStartMs) > THEFT_WINDOW_MS) {
            theftWindowStartMs = 0;
            theftStrikeCount = 0;
            if (!securityManager.isAlarming()) {
                theftAlertSent = false;
            }
        }
    } else {
        theftWindowStartMs = 0;
        theftStrikeCount = 0;
        theftAlertSent = false;
        securityManager.resetVibration();
    }

    if (!isLockdown) {
        AuthResult rfidResult = authHandler.checkRFID();
        if (rfidResult == AUTH_SUCCESS) {
            enterAwaiting2FAForUser(authHandler.getLastRFIDUID());
        } else if (rfidResult == AUTH_DENIED) {
            securityManager.beep(3);
            webServer.logActivity("Unknown RFID", "RFID Access Denied", "fail");
        }
    }

    if (keypadState == STATE_AWAITING_2FA && pendingOtpIssuedAtMs > 0) {
        if ((millis() - pendingOtpIssuedAtMs) >= OTP_TTL_MS) {
            securityManager.beep(3);
            clearPending2FA("OTP timeout", true);
        }
    }

    if (isLockdown) {
        if (keypadState == STATE_AWAITING_2FA) {
            clearPending2FA("Lockdown activated", false);
        }
        keypadBuffer = "";
        authHandler.clearBuffer();
        yield();
        return;
    }

    processKeypad();
    yield();
}

void processKeypad() {
    const char key = authHandler.getKeypadKey();
    if (!key) {
        return;
    }

    if (key == '*') {
        keypadBuffer = "";
        authHandler.clearBuffer();
        if (keypadState == STATE_AWAITING_2FA) {
            clearPending2FA("User cancelled", false);
        }
        securityManager.beep(1);
        return;
    }

    if (key == '#') {
        if (keypadState == STATE_IDLE) {
            if (guestCodeActive && keypadBuffer == guestCode && keypadBuffer.length() == BACKUP_PIN_LENGTH) {
                grantAccess("Guest", "Guest PIN");
                guestCode = "";
                guestCodeActive = false;
            } else if (keypadBuffer.length() > 0) {
                securityManager.beep(3);
                webServer.logActivity("Guest", "Guest PIN Failed", "fail");
            }
            keypadBuffer = "";
        } else {
            if (!awaitingOfflineBackupMode) {
                awaitingOfflineBackupMode = true;
                keypadBuffer = "";
                authHandler.clearBuffer();
                authPrompt = "Offline Mode: Enter Backup PIN";
                webServer.logActivity(pendingUserName, "Offline Backup Mode", "success");
            } else {
                evaluateAwaiting2FABuffer(true);
            }
        }
        securityManager.beep(1);
        return;
    }

    if (key < '0' || key > '9') {
        return;
    }

    if (keypadState == STATE_IDLE) {
        if (keypadBuffer.length() < BACKUP_PIN_LENGTH) {
            keypadBuffer += key;
            if (guestCodeActive && keypadBuffer.length() == BACKUP_PIN_LENGTH && keypadBuffer == guestCode) {
                grantAccess("Guest", "Guest PIN");
                guestCode = "";
                guestCodeActive = false;
                keypadBuffer = "";
            }
        }
        securityManager.beep(1);
        return;
    }

    const size_t maxLen = awaitingOfflineBackupMode ? BACKUP_PIN_LENGTH : OTP_LENGTH;
    if (keypadBuffer.length() < maxLen) {
        keypadBuffer += key;
        evaluateAwaiting2FABuffer(false);
    }
    securityManager.beep(1);
}

bool evaluateAwaiting2FABuffer(bool explicitSubmit) {
    const size_t len = keypadBuffer.length();

    if (len == BACKUP_PIN_LENGTH) {
        if (keypadBuffer == String(DURESS_CODE)) {
            lockManager.unlock();
            authHandler.startRFIDCooldown();
            securityManager.beep(2);
            pendingDoorSecuredLog = true;
            webServer.logActivity(pendingUserName, "Duress Code", "alarm");
            sendDuressAlert(pendingUserName);
            clearPending2FA("Duress accepted", false);
            return true;
        }

        if (awaitingOfflineBackupMode && pendingBackupPin.length() == BACKUP_PIN_LENGTH && keypadBuffer == pendingBackupPin) {
            grantAccess(pendingUserName, "RFID + Backup PIN");
            return true;
        }

        if (awaitingOfflineBackupMode && explicitSubmit) {
            securityManager.beep(3);
            clearPending2FA("Backup PIN failed", true);
            return true;
        }
    }

    if (!awaitingOfflineBackupMode && len == OTP_LENGTH) {
        if (pendingOtp.length() == OTP_LENGTH && keypadBuffer == pendingOtp) {
            grantAccess(pendingUserName, "RFID + Telegram OTP");
        } else {
            securityManager.beep(3);
            clearPending2FA("OTP failed", true);
        }
        return true;
    }

    if (explicitSubmit) {
        securityManager.beep(3);
        clearPending2FA("Invalid 2FA input length", true);
        return true;
    }

    return false;
}

void enterAwaiting2FAForUser(const String& uid) {
    pendingUID = uid;
    pendingUserName = authHandler.getUserName(uid);
    pendingUserChatId = authHandler.getUserTelegramChatId(uid);
    pendingBackupPin = authHandler.getUserBackupPIN(uid);
    pendingOtp = "";
    pendingOtpIssuedAtMs = 0;

    keypadBuffer = "";
    authHandler.clearBuffer();
    keypadState = STATE_AWAITING_2FA;
    awaitingOfflineBackupMode = false;

    if (bot && pendingUserChatId.length() > 0) {
        pendingOtp = generateNumericCode(OTP_LENGTH);
        pendingOtpIssuedAtMs = millis();

        sendOTP(pendingUserChatId, pendingOtp);
        webServer.logActivity(pendingUserName, "OTP Sent", "success");
        authPrompt = "Enter 6-digit OTP (press # for Offline Mode)";
    } else {
        awaitingOfflineBackupMode = true;
        authPrompt = "Offline Mode: Enter Backup PIN";
        webServer.logActivity(pendingUserName, "OTP Unavailable - Backup PIN", "success");
    }
}

void grantAccess(const String& actor, const String& method) {
    lockManager.unlock();
    authHandler.startRFIDCooldown();
    securityManager.beep(2);
    pendingDoorSecuredLog = true;

    if (securityManager.isAlarming()) {
        securityManager.clearAlarm();
    }

    webServer.logActivity(actor, method, "success");
    clearPending2FA("Access granted", false);
}

void clearPending2FA(const String& reason, bool logFailure) {
    if (keypadState == STATE_AWAITING_2FA && logFailure) {
        webServer.logActivity(pendingUserName.length() ? pendingUserName : "Unknown", reason, "fail");
    }

    keypadState = STATE_IDLE;
    pendingUID = "";
    pendingUserName = "";
    pendingUserChatId = "";
    pendingBackupPin = "";
    pendingOtp = "";
    pendingOtpIssuedAtMs = 0;
    awaitingOfflineBackupMode = false;
    authPrompt = "";
    keypadBuffer = "";
    authHandler.clearBuffer();
}

String generateNumericCode(size_t length) {
    String out = "";
    out.reserve(length);
    for (size_t i = 0; i < length; i++) {
        out += String(random(0, 10));
    }
    return out;
}

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
            const int updateId = bot->messages[i].update_id;
            if (updateId <= lastHandledTelegramUpdateId) {
                continue;
            }

            String chatId = bot->messages[i].chat_id;
            String text = normalizeTelegramCommand(bot->messages[i].text);
            chatId.trim();
            if (text.length() == 0) {
                lastHandledTelegramUpdateId = updateId;
                continue;
            }

            lastHandledTelegramUpdateId = updateId;

            const TelegramRole role = resolveTelegramRole(chatId);
            if (role == ROLE_UNKNOWN) {
                continue;
            }

            if (role == ROLE_ADMIN) {
                handleAdminCommand(chatId, text);
            } else if (role == ROLE_USER) {
                handleUserCommand(chatId, text);
            }
        }

        numNewMessages = bot->getUpdates(bot->last_message_received + 1);
    }
}

String normalizeTelegramCommand(const String& rawText) {
    String command = rawText;
    command.trim();
    if (command.length() == 0) {
        return "";
    }

    const int firstSpace = command.indexOf(' ');
    if (firstSpace > 0) {
        command = command.substring(0, firstSpace);
    }

    const int botMention = command.indexOf('@');
    if (botMention > 0) {
        command = command.substring(0, botMention);
    }

    command.trim();
    return command;
}

TelegramRole resolveTelegramRole(const String& chatId) {
    const String adminChatId = String(ADMIN_CHAT_ID);
    if (chatId == adminChatId) {
        return ROLE_ADMIN;
    }

    if (authHandler.isKnownTelegramChatId(chatId)) {
        return ROLE_USER;
    }

    return ROLE_UNKNOWN;
}

void handleAdminCommand(const String& chatId, const String& text) {
    if (text == "/start") {
        bot->sendMessage(chatId, "🚪 System Armed & Ready. Admin recognized.", "");
        return;
    }

    if (text == "/admin_open") {
        lockManager.unlock();
        authHandler.startRFIDCooldown();
        securityManager.beep(2);
        pendingDoorSecuredLog = true;
        webServer.markEmergencyOverride();
        webServer.logActivity("Admin (Telegram)", "Emergency Override", "success");
        clearPending2FA("Admin override", false);
        bot->sendMessage(chatId, "🚨 Emergency Remote Unlock Executed.", "");
        return;
    }

    if (text == "/guest_code") {
        guestCode = generateNumericCode(BACKUP_PIN_LENGTH);
        guestCodeActive = true;
        guestCodeIssuedAtMs = millis();
        bot->sendMessage(chatId, "⏳ Guest code " + guestCode + " active for 5 mins.", "");
        webServer.logActivity("Admin (Telegram)", "Guest PIN Generated", "success");
        return;
    }

    if (text == "/status") {
        const String doorState = lockManager.isDoorOpen() ? "Open" : "Closed";
        const String systemState = isLockdown ? "Lockdown" : "Armed";
        const String wifiState = String(WiFi.RSSI()) + " dBm";
        const String statusMsg = "Door: " + doorState + ", System: " + systemState + ", WiFi: " + wifiState;
        bot->sendMessage(chatId, statusMsg, "");
        return;
    }

    if (text == "/lockdown") {
        isLockdown = true;
        clearPending2FA("Lockdown activated by admin", false);
        bot->sendMessage(chatId, "🛑 SYSTEM LOCKDOWN ACTIVE.", "");
        webServer.logActivity("Admin (Telegram)", "Lockdown Enabled", "success");
        return;
    }

    if (text == "/unlockdown") {
        isLockdown = false;
        bot->sendMessage(chatId, "✅ Lockdown lifted. Hardware re-enabled.", "");
        webServer.logActivity("Admin (Telegram)", "Lockdown Disabled", "success");
        return;
    }
}

void handleUserCommand(const String& chatId, const String& text) {
    if (text == "/start") {
        bot->sendMessage(chatId, "✅ Welcome! Your Telegram is linked. You will receive 2FA OTPs here.", "");
        return;
    }

    if (text == "/admin_open") {
        bot->sendMessage(chatId, "⛔ Unauthorized", "");
        webServer.logActivity("User (Telegram)", "Unauthorized /admin_open", "fail");
        return;
    }

    bot->sendMessage(chatId, "⛔ Unauthorized", "");
    webServer.logActivity("User (Telegram)", "Unauthorized Command", "fail");
}

void sendDuressAlert(String userName) {
    if (!bot) {
        return;
    }

    bot->sendMessage(ADMIN_CHAT_ID, "🚨 HELP: DURESS CODE USED BY USER [" + userName + "]!", "");
}

void sendTheftAlert() {
    if (!bot) {
        return;
    }

    bot->sendMessage(ADMIN_CHAT_ID, "⚠ THEFT ATTEMPT DETECTED!", "");
}

void sendOTP(String chatID, String otp) {
    if (!bot || chatID.length() == 0) {
        return;
    }

    bot->sendMessage(chatID, "🔑 Your Temporary OTP is: " + otp + ". Valid for 30 seconds.", "");
}

void checkGuestCodeExpiry() {
    if (!guestCodeActive) {
        return;
    }

    if ((millis() - guestCodeIssuedAtMs) >= GUEST_CODE_TTL_MS) {
        guestCodeActive = false;
        guestCode = "";
    }
}

String getActiveGuestCode() {
    return guestCodeActive ? guestCode : "";
}

bool isTemporaryGuestCodeActive() {
    return guestCodeActive;
}

unsigned long getTemporaryGuestCodeRemainingMs() {
    if (!guestCodeActive) {
        return 0;
    }

    const unsigned long elapsed = millis() - guestCodeIssuedAtMs;
    if (elapsed >= GUEST_CODE_TTL_MS) {
        return 0;
    }

    return GUEST_CODE_TTL_MS - elapsed;
}

String getAuthPrompt() {
    return authPrompt;
}

bool isPendingAccessActive() {
    return keypadState == STATE_AWAITING_2FA;
}
