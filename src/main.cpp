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
char lastKeypadKey = '\0';
unsigned long lastKeypadKeyMs = 0;

static const size_t OTP_LENGTH = 4;
static const size_t BACKUP_PIN_LENGTH = 4;
static const char* DURESS_CODE = "2580";
static const unsigned long OTP_TTL_MS = 30000;

unsigned long lastTelegramPollMs = 0;
static const unsigned long TELEGRAM_POLL_FAST_MS = 60;
static const unsigned long TELEGRAM_POLL_IDLE_MS = 120;
static const unsigned long TELEGRAM_POLL_ERROR_MS = 300;
static const int TELEGRAM_MAX_PROCESS_PER_CYCLE = 25;
unsigned long telegramPollIntervalMs = TELEGRAM_POLL_FAST_MS;
unsigned long telegramLastPollDurationMs = 0;
unsigned long telegramLastSuccessMs = 0;
unsigned long telegramLastErrorMs = 0;
unsigned long telegramLastCommandMs = 0;
unsigned long telegramLastCommandLatencyMs = 0;
unsigned long telegramCommandsHandled = 0;
unsigned long telegramPollErrors = 0;
int telegramPendingApprox = 0;
String telegramLastCommandText = "";
String telegramLastCommandRole = "";
String telegramLastCommandResult = "";
static const unsigned long EMERGENCY_LOCKOUT_MS = 5000;
int lastHandledTelegramUpdateId = 0;
unsigned long lastTelegramRfidAlertScanMs = 0;

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
bool sendOTP(String chatID, String otp);
void sendRfidScanAlert(String uid, bool known, const String& userName);
String normalizeTelegramCommand(const String& rawText);

String getActiveGuestCode();
bool isTemporaryGuestCodeActive();
unsigned long getTemporaryGuestCodeRemainingMs();
String getAuthPrompt();
bool isPendingAccessActive();
String getLastKeypadKeyLabel();
unsigned long getLastKeypadKeyMs();
unsigned long getTelegramPollIntervalMs();
unsigned long getTelegramLastPollDurationMs();
unsigned long getTelegramLastSuccessMs();
unsigned long getTelegramLastErrorMs();
unsigned long getTelegramLastCommandMs();
unsigned long getTelegramLastCommandLatencyMs();
unsigned long getTelegramCommandsHandled();
unsigned long getTelegramPollErrors();
int getTelegramPendingApprox();
String getTelegramLastCommandText();
String getTelegramLastCommandRole();
String getTelegramLastCommandResult();

void trackTelegramCommand(const String& role, const String& command, const String& result, unsigned long latencyMs);

void setup() {
    Serial.begin(115200);

    lockManager.init();
    securityManager.init();
    authHandler.init();
    webServer.init(WIFI_SSID, WIFI_PASSWORD);

    if (webServer.isConnected()) {
        telegramClient.setInsecure();
        telegramClient.setTimeout(1200);
        bot = new UniversalTelegramBot(BOT_TOKEN, telegramClient);
        bot->longPoll = 0;
        Serial.println("[TELEGRAM] Bot initialized");
    }

    // Startup should stay quiet unless explicitly requested by command/action.
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
            const String uid = authHandler.getLastRFIDUID();
            const String userName = authHandler.getUserName(uid);
            const unsigned long scanMs = authHandler.getLastRFIDScanMs();

            // User feedback: one beep when a registered RFID is recognized.
            securityManager.beep(1);

            webServer.logActivity(userName, "RFID Recognized", "success");

            if (scanMs > 0 && scanMs != lastTelegramRfidAlertScanMs) {
                sendRfidScanAlert(uid, true, userName);
                lastTelegramRfidAlertScanMs = scanMs;
            }

            enterAwaiting2FAForUser(uid);
        } else if (rfidResult == AUTH_DENIED) {
            const String uid = authHandler.getLastRFIDUID();
            const unsigned long scanMs = authHandler.getLastRFIDScanMs();

            securityManager.beep(3);
            webServer.logActivity("Unknown RFID", "RFID Access Denied", "fail");

            if (scanMs > 0 && scanMs != lastTelegramRfidAlertScanMs) {
                sendRfidScanAlert(uid, false, "");
                lastTelegramRfidAlertScanMs = scanMs;
            }
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

    lastKeypadKey = key;
    lastKeypadKeyMs = millis();

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
        securityManager.beep(1);
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

    if (bot) {
        pendingOtp = generateNumericCode(OTP_LENGTH);
        pendingOtpIssuedAtMs = millis();

        String otpTargetChat = pendingUserChatId;
        otpTargetChat.trim();
        if (otpTargetChat.length() == 0) {
            securityManager.beep(3);
            webServer.logActivity(pendingUserName, "OTP Blocked - Missing Chat ID", "fail");
            clearPending2FA("Missing Telegram Chat ID", false);
            return;
        }

        const bool otpSent = sendOTP(otpTargetChat, pendingOtp);
        if (!otpSent) {
            securityManager.beep(3);
            webServer.logActivity(pendingUserName, "OTP Send Failed", "fail");
            clearPending2FA("OTP send failed", false);
            return;
        }

        webServer.logActivity(pendingUserName, "OTP Sent", "success");
        authPrompt = "Enter 4-digit OTP (press # for Offline Mode)";
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
    if ((now - lastTelegramPollMs) < telegramPollIntervalMs) {
        return;
    }
    lastTelegramPollMs = now;

    const unsigned long pollStartMs = millis();
    int processedInCycle = 0;

    int numNewMessages = bot->getUpdates(bot->last_message_received + 1);
    if (numNewMessages < 0) {
        telegramPollErrors++;
        telegramLastErrorMs = millis();
        telegramPollIntervalMs = TELEGRAM_POLL_ERROR_MS;
        telegramLastPollDurationMs = millis() - pollStartMs;
        return;
    }

    if (numNewMessages > 0) {
        const int processLimit = (numNewMessages < TELEGRAM_MAX_PROCESS_PER_CYCLE)
            ? numNewMessages
            : TELEGRAM_MAX_PROCESS_PER_CYCLE;

        for (int i = 0; i < processLimit; i++) {
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

            processedInCycle++;
        }

        telegramPendingApprox = numNewMessages - processedInCycle;
        if (telegramPendingApprox < 0) {
            telegramPendingApprox = 0;
        }
    } else {
        telegramPendingApprox = 0;
    }

    telegramLastPollDurationMs = millis() - pollStartMs;
    if (processedInCycle > 0) {
        telegramCommandsHandled += static_cast<unsigned long>(processedInCycle);
        telegramLastSuccessMs = millis();
        telegramPollIntervalMs = TELEGRAM_POLL_FAST_MS;
    } else {
        // Idle cycles back off slightly to reduce API churn while staying responsive.
        telegramPollIntervalMs = TELEGRAM_POLL_IDLE_MS;
    }
}

void trackTelegramCommand(const String& role, const String& command, const String& result, unsigned long latencyMs) {
    telegramLastCommandRole = role;
    telegramLastCommandText = command;
    telegramLastCommandResult = result;
    telegramLastCommandMs = millis();
    telegramLastCommandLatencyMs = latencyMs;
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
    command.toLowerCase();
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
    const unsigned long cmdStartMs = millis();

    if (text == "/help") {
        bot->sendMessage(
            chatId,
            "Commands: /status, /open, /admin_open, /guest_code, /buzzer_test, /keypad_echo, /lockdown, /unlockdown",
            ""
        );
        webServer.logActivity("Admin (Telegram)", "Help Command", "success");
        trackTelegramCommand("admin", text, "ok", millis() - cmdStartMs);
        return;
    }

    if (text == "/start") {
        bot->sendMessage(chatId, "🚪 System Armed & Ready. Admin recognized.", "");
        webServer.logActivity("Admin (Telegram)", "Start Command", "success");
        trackTelegramCommand("admin", text, "ok", millis() - cmdStartMs);
        return;
    }

    if (text == "/admin_open" || text == "/open") {
        lockManager.unlock();
        authHandler.startRFIDCooldown();
        securityManager.beep(1);
        pendingDoorSecuredLog = true;
        webServer.markEmergencyOverride();
        webServer.logActivity("Admin (Telegram)", "Emergency Override", "success");
        clearPending2FA("Admin override", false);
        bot->sendMessage(chatId, "🚨 Emergency Remote Unlock Executed.", "");
        trackTelegramCommand("admin", text, "ok", millis() - cmdStartMs);
        return;
    }

    if (text == "/guest_code") {
        guestCode = generateNumericCode(BACKUP_PIN_LENGTH);
        guestCodeActive = true;
        guestCodeIssuedAtMs = millis();
        bot->sendMessage(chatId, "⏳ Guest code " + guestCode + " active for 5 mins.", "");
        webServer.logActivity("Admin (Telegram)", "Guest PIN Generated", "success");
        trackTelegramCommand("admin", text, "ok", millis() - cmdStartMs);
        return;
    }

    if (text == "/status") {
        const String doorState = lockManager.isDoorOpen() ? "Open" : "Closed";
        const String systemState = isLockdown ? "Lockdown" : "Armed";
        const String wifiState = String(WiFi.RSSI()) + " dBm";
        const String buzzerState = securityManager.isSirenActive()
            ? "SIREN"
            : (securityManager.isBuzzerActive() ? "BEEPING" : "IDLE");
        const String statusMsg = "Door: " + doorState + ", System: " + systemState + ", Buzzer: " + buzzerState + ", WiFi: " + wifiState;
        bot->sendMessage(chatId, statusMsg, "");
        webServer.logActivity("Admin (Telegram)", "Status Check", "success");
        trackTelegramCommand("admin", text, "ok", millis() - cmdStartMs);
        return;
    }

    if (text == "/buzzer_test") {
        securityManager.beep(3);
        bot->sendMessage(chatId, "🔊 Buzzer test triggered (triple tone).", "");
        webServer.logActivity("Admin (Telegram)", "Buzzer Test", "success");
        trackTelegramCommand("admin", text, "ok", millis() - cmdStartMs);
        return;
    }

    if (text == "/keypad_echo") {
        const String keyLabel = getLastKeypadKeyLabel();
        if (keyLabel.length() == 0) {
            bot->sendMessage(chatId, "⌨️ No keypad key recorded yet.", "");
            webServer.logActivity("Admin (Telegram)", "Keypad Echo", "success");
            trackTelegramCommand("admin", text, "ok", millis() - cmdStartMs);
            return;
        }

        const unsigned long ageMs = millis() - getLastKeypadKeyMs();
        bot->sendMessage(chatId, "⌨️ Last keypad key: " + keyLabel + " (" + String(ageMs) + " ms ago)", "");
        webServer.logActivity("Admin (Telegram)", "Keypad Echo", "success");
        trackTelegramCommand("admin", text, "ok", millis() - cmdStartMs);
        return;
    }

    if (text == "/lockdown") {
        isLockdown = true;
        clearPending2FA("Lockdown activated by admin", false);
        bot->sendMessage(chatId, "🛑 SYSTEM LOCKDOWN ACTIVE.", "");
        webServer.logActivity("Admin (Telegram)", "Lockdown Enabled", "success");
        trackTelegramCommand("admin", text, "ok", millis() - cmdStartMs);
        return;
    }

    if (text == "/unlockdown") {
        isLockdown = false;
        bot->sendMessage(chatId, "✅ Lockdown lifted. Hardware re-enabled.", "");
        webServer.logActivity("Admin (Telegram)", "Lockdown Disabled", "success");
        trackTelegramCommand("admin", text, "ok", millis() - cmdStartMs);
        return;
    }

    bot->sendMessage(chatId, "Unknown command. Use /help", "");
    webServer.logActivity("Admin (Telegram)", "Unknown Command", "fail");
    trackTelegramCommand("admin", text, "unknown", millis() - cmdStartMs);
}

void handleUserCommand(const String& chatId, const String& text) {
    const unsigned long cmdStartMs = millis();

    if (text == "/help") {
        bot->sendMessage(chatId, "Commands: /start, /status, /keypad_echo", "");
        webServer.logActivity("User (Telegram)", "Help Command", "success");
        trackTelegramCommand("user", text, "ok", millis() - cmdStartMs);
        return;
    }

    if (text == "/start") {
        bot->sendMessage(chatId, "✅ Welcome! Your Telegram is linked. You will receive 2FA OTPs here.", "");
        webServer.logActivity("User (Telegram)", "Start Command", "success");
        trackTelegramCommand("user", text, "ok", millis() - cmdStartMs);
        return;
    }

    if (text == "/status") {
        const String doorState = lockManager.isDoorOpen() ? "Open" : "Closed";
        const String systemState = isLockdown ? "Lockdown" : "Armed";
        bot->sendMessage(chatId, "Door: " + doorState + ", System: " + systemState, "");
        webServer.logActivity("User (Telegram)", "Status Check", "success");
        trackTelegramCommand("user", text, "ok", millis() - cmdStartMs);
        return;
    }

    if (text == "/keypad_echo") {
        const String keyLabel = getLastKeypadKeyLabel();
        if (keyLabel.length() == 0) {
            bot->sendMessage(chatId, "⌨️ No keypad key recorded yet.", "");
        } else {
            const unsigned long ageMs = millis() - getLastKeypadKeyMs();
            bot->sendMessage(chatId, "⌨️ Last keypad key: " + keyLabel + " (" + String(ageMs) + " ms ago)", "");
        }
        webServer.logActivity("User (Telegram)", "Keypad Echo", "success");
        trackTelegramCommand("user", text, "ok", millis() - cmdStartMs);
        return;
    }

    if (text == "/admin_open" || text == "/open" || text == "/lockdown" || text == "/unlockdown" || text == "/guest_code" || text == "/buzzer_test") {
        bot->sendMessage(chatId, "⛔ Unauthorized", "");
        webServer.logActivity("User (Telegram)", "Unauthorized " + text, "fail");
        trackTelegramCommand("user", text, "unauthorized", millis() - cmdStartMs);
        return;
    }

    bot->sendMessage(chatId, "Unknown command. Use /help", "");
    webServer.logActivity("User (Telegram)", "Unknown Command", "fail");
    trackTelegramCommand("user", text, "unknown", millis() - cmdStartMs);
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

bool sendOTP(String chatID, String otp) {
    if (!bot || chatID.length() == 0) {
        return false;
    }

    return bot->sendMessage(chatID, "🔑 Your Temporary OTP is: " + otp + ". Valid for 30 seconds.", "");
}

void sendRfidScanAlert(String uid, bool known, const String& userName) {
    if (!bot) {
        return;
    }

    const String cardUid = uid.length() > 0 ? uid : "UNKNOWN_UID";

    if (known) {
        String label = userName;
        if (label.length() == 0) {
            label = "Known User";
        }

        bot->sendMessage(
            ADMIN_CHAT_ID,
            "✅ Registered RFID detected: " + label + " [" + cardUid + "]\n2FA flow started.",
            ""
        );
        return;
    }

    bot->sendMessage(
        ADMIN_CHAT_ID,
        "⚠ Unregistered RFID detected: [" + cardUid + "]\nAccess denied.",
        ""
    );
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

String getLastKeypadKeyLabel() {
    if (!lastKeypadKey) {
        return "";
    }

    return String(lastKeypadKey);
}

unsigned long getLastKeypadKeyMs() {
    return lastKeypadKeyMs;
}

unsigned long getTelegramPollIntervalMs() {
    return telegramPollIntervalMs;
}

unsigned long getTelegramLastPollDurationMs() {
    return telegramLastPollDurationMs;
}

unsigned long getTelegramLastSuccessMs() {
    return telegramLastSuccessMs;
}

unsigned long getTelegramLastErrorMs() {
    return telegramLastErrorMs;
}

unsigned long getTelegramLastCommandMs() {
    return telegramLastCommandMs;
}

unsigned long getTelegramLastCommandLatencyMs() {
    return telegramLastCommandLatencyMs;
}

unsigned long getTelegramCommandsHandled() {
    return telegramCommandsHandled;
}

unsigned long getTelegramPollErrors() {
    return telegramPollErrors;
}

int getTelegramPendingApprox() {
    return telegramPendingApprox;
}

String getTelegramLastCommandText() {
    return telegramLastCommandText;
}

String getTelegramLastCommandRole() {
    return telegramLastCommandRole;
}

String getTelegramLastCommandResult() {
    return telegramLastCommandResult;
}
