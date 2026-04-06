#include <Arduino.h>
#include <esp_system.h>
#include <ESPmDNS.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <LockManager.h>
#include <SecurityManager.h>
#include <AuthHandler.h>
#include <WebServer.h>
#include "app/controllers/WiFiController.h"
#include "secrets.h"

#ifndef ADMIN_CHAT_ID
#define ADMIN_CHAT_ID ADMIN_CHAT_IDS[0]
#endif

#ifndef WIFI_EXTRA_1_SSID
#define WIFI_EXTRA_1_SSID ""
#endif

#ifndef WIFI_EXTRA_1_PASSWORD
#define WIFI_EXTRA_1_PASSWORD ""
#endif

#ifndef WIFI_EXTRA_2_SSID
#define WIFI_EXTRA_2_SSID ""
#endif

#ifndef WIFI_EXTRA_2_PASSWORD
#define WIFI_EXTRA_2_PASSWORD ""
#endif

#ifndef WIFI_EXTRA_3_SSID
#define WIFI_EXTRA_3_SSID ""
#endif

#ifndef WIFI_EXTRA_3_PASSWORD
#define WIFI_EXTRA_3_PASSWORD ""
#endif

#ifndef WIFI_EXTRA_4_SSID
#define WIFI_EXTRA_4_SSID ""
#endif

#ifndef WIFI_EXTRA_4_PASSWORD
#define WIFI_EXTRA_4_PASSWORD ""
#endif

LockManager lockManager;
SecurityManager securityManager;
AuthHandler authHandler;
WebServer webServer(&lockManager, &securityManager, &authHandler);
WiFiController wifiController;

WiFiClientSecure telegramClient;
UniversalTelegramBot* bot = nullptr;
bool mdnsStarted = false;
bool lastWifiConnected = false;
static const char* MDNS_HOSTNAME = "securelock";
static const WiFiController::Credential WIFI_CREDENTIALS[] = {
    { WIFI_SSID, WIFI_PASSWORD },
    { WIFI_EXTRA_1_SSID, WIFI_EXTRA_1_PASSWORD },
    { WIFI_EXTRA_2_SSID, WIFI_EXTRA_2_PASSWORD },
    { WIFI_EXTRA_3_SSID, WIFI_EXTRA_3_PASSWORD },
    { WIFI_EXTRA_4_SSID, WIFI_EXTRA_4_PASSWORD }
};

enum TelegramRole {
    ROLE_UNKNOWN,
    ROLE_USER,
    ROLE_ADMIN
};

enum KeypadState {
    STATE_IDLE,
    STATE_AWAITING_2FA,
    STATE_BACKUP_ONLY
};

KeypadState keypadState = STATE_IDLE;
bool awaitingOfflineBackupMode = false;

String guestCode = "";
bool guestCodeActive = false;
unsigned long guestCodeIssuedAtMs = 0;
static const unsigned long GUEST_CODE_TTL_MS = 30000;
static const unsigned long GUEST_CODE_COMMAND_COOLDOWN_MS = 30000;

String pendingUID = "";
String pendingUserName = "";
String pendingUserChatId = "";
String pendingBackupPin = "";
String pendingOtp = "";
unsigned long pendingOtpIssuedAtMs = 0;
String keypadBuffer = "";
String authPrompt = "";

static const size_t OTP_LENGTH = 4;
static const size_t BACKUP_PIN_LENGTH = 4;
static const char* DURESS_CODE = "2580";
static const unsigned long OTP_TTL_MS = 30000;
static const unsigned long TELEGRAM_STARTUP_GRACE_MS = 12000;
static const unsigned long KEYPAD_LAST_KEY_FRESH_MS = 20000;

unsigned long lastTelegramPollMs = 0;
static const unsigned long TELEGRAM_POLL_FAST_MS = 1400;
static const unsigned long TELEGRAM_POLL_IDLE_MS = 5000;
static const unsigned long TELEGRAM_POLL_ERROR_MS = 12000;
static const unsigned long TELEGRAM_COMMAND_MIN_INTERVAL_MS = 1200;
static const unsigned long TELEGRAM_DANGEROUS_COMMAND_COOLDOWN_MS = 5000;
static const unsigned long TELEGRAM_UNKNOWN_NOTICE_COOLDOWN_MS = 15000;
static const unsigned long TELEGRAM_UNAUTHORIZED_ALERT_COOLDOWN_MS = 60000;
static const int TELEGRAM_TRACKED_CHATS_MAX = 24;
static const int TELEGRAM_MAX_PROCESS_PER_CYCLE = 3;
static const int TELEGRAM_SAFE_MESSAGE_SLOTS = 3;
static const int TELEGRAM_WEB_ACTIVITY_QUEUE_MAX = 16;
static const unsigned long TELEGRAM_WEB_ACTIVITY_MIN_INTERVAL_MS = 1500;
unsigned long telegramPollIntervalMs = TELEGRAM_POLL_FAST_MS;
unsigned long telegramReadyAfterMs = 0;
unsigned long telegramLastPollDurationMs = 0;
unsigned long telegramLastSuccessMs = 0;
unsigned long telegramLastErrorMs = 0;
unsigned long telegramLastErrorLogMs = 0;
unsigned long telegramLastCommandMs = 0;
unsigned long telegramLastCommandLatencyMs = 0;
unsigned long telegramCommandsHandled = 0;
unsigned long telegramPollErrors = 0;
int telegramPendingApprox = 0;
String telegramLastCommandText = "";
String telegramLastCommandRole = "";
String telegramLastCommandResult = "";
String telegramTrackedChatIds[TELEGRAM_TRACKED_CHATS_MAX];
unsigned long telegramTrackedLastCommandMs[TELEGRAM_TRACKED_CHATS_MAX];
unsigned long telegramTrackedLastDangerousMs[TELEGRAM_TRACKED_CHATS_MAX];
unsigned long telegramTrackedLastUnknownNoticeMs[TELEGRAM_TRACKED_CHATS_MAX];
unsigned long telegramTrackedLastUnauthorizedAlertMs[TELEGRAM_TRACKED_CHATS_MAX];
String telegramWebActivityQueue[TELEGRAM_WEB_ACTIVITY_QUEUE_MAX];
volatile int telegramWebActivityHead = 0;
volatile int telegramWebActivityTail = 0;
volatile int telegramWebActivityCount = 0;
unsigned long telegramLastWebActivityForwardMs = 0;
portMUX_TYPE telegramWebActivityMux = portMUX_INITIALIZER_UNLOCKED;
static const unsigned long EMERGENCY_LOCKOUT_MS = 5000;
int lastHandledTelegramUpdateId = 0;
unsigned long lastTelegramRfidAlertScanMs = 0;
unsigned long lastTelegramRfidAlertSentMs = 0;
String lastTelegramRfidAlertUid = "";
static const unsigned long RFID_ALERT_MIN_INTERVAL_MS = 5000;
unsigned long lastRfidSuccessFeedbackMs = 0;
unsigned long lastRfidDeniedFeedbackMs = 0;
static const unsigned long RFID_SUCCESS_FEEDBACK_MIN_INTERVAL_MS = 400;
static const unsigned long RFID_DENIED_FEEDBACK_MIN_INTERVAL_MS = 1200;
static const unsigned long PENDING_2FA_CONFLICT_FEEDBACK_MS = 1500;
static const unsigned long PENDING_2FA_CONFLICT_LOG_MS = 4000;
static const unsigned long KEYPAD_AUTH_ALERT_MIN_INTERVAL_MS = 5000;
static const unsigned long KEYPAD_IDLE_BUFFER_TIMEOUT_MS = 5000;
static const unsigned long BACKUP_ONLY_MODE_TIMEOUT_MS = 30000;
static const unsigned long AUTH_FAILURE_WINDOW_MS = 45000;
static const uint8_t AUTH_FAILURE_ALERT_THRESHOLD = 2;

unsigned long lastPending2FAConflictFeedbackMs = 0;
unsigned long lastPending2FAConflictLogMs = 0;
unsigned long lastGuestPinFailureAlertMs = 0;
unsigned long lastBackupPinFailureAlertMs = 0;
unsigned long lastBackupPinCollisionAlertMs = 0;
unsigned long lastGuestPinFailureMs = 0;
unsigned long lastBackupPinFailureMs = 0;
unsigned long backupOnlyModeStartedAtMs = 0;
unsigned long keypadIdleBufferLastInputMs = 0;
uint8_t guestPinFailureCount = 0;
uint8_t backupPinFailureCount = 0;

bool isLockdown = false;

// Part 5 mechanics: auto-relock verification and theft watchdog window.
bool pendingDoorSecuredLog = false;
bool previousDoorOpen = false;
bool previousLockedState = true;
unsigned long theftWindowStartMs = 0;
int theftStrikeCount = 0;
bool theftAlertSent = false;
static const unsigned long THEFT_WINDOW_MS = 2000;
static const int THEFT_STRIKE_THRESHOLD = 5;

void clearPending2FA(const String& reason, bool logFailure);
void clearBackupOnlyMode(const String& reason, bool logFailure);
String generateNumericCode(size_t length);
void updateNetworkServices();
void initializeTelegramBotIfNeeded();
void enterAwaiting2FAForUser(const String& uid);
void grantAccess(const String& actor, const String& method, const String& userChatId = "");
bool evaluateAwaiting2FABuffer(bool explicitSubmit);
bool evaluateBackupOnlyBuffer(bool explicitSubmit);
int resolveUserByBackupPin(const String& pin, String* matchedUid, String* matchedUserName, String* matchedChatId);
void handleTelegramCommands();
void checkGuestCodeExpiry();
void processKeypad();
bool isAdmin(String incoming_chat_id);
bool notifyAdmins(const String& message);
TelegramRole resolveTelegramRole(const String& chatId);
String getUserNameByTelegramChatId(const String& chatId);
void handleUnknownTelegramChat(const String& chatId);
void handleAdminCommand(const String& chatId, const String& text);
void handleUserCommand(const String& chatId, const String& text);
bool sendTelegramText(const String& chatId, const String& message);
void forwardWebAdminActivityToTelegram(const String& user, const String& method, const String& status);
void processQueuedWebAdminActivityTelegram();
bool configureTelegramPollingMode();
void sendDuressAlert(String userName);
void sendTheftAlert();
bool sendOTP(String chatID, String otp);
void sendRfidScanAlert(String uid, bool known, const String& userName);
String normalizeTelegramCommand(const String& rawText);
bool enqueueAdminNotification(const String& message);
void queueAccessEventForAdmins(const String& actor, const String& method);
const char* keypadStateToText(KeypadState state);
int getTelegramChatSlot(const String& chatId, bool createIfMissing = true);
bool isDangerousAdminCommand(const String& command);
bool shouldThrottleTelegramCommand(const String& chatId, bool dangerous, unsigned long* retryAfterMs = nullptr);
bool shouldThrottleUnknownNotice(const String& chatId, unsigned long* retryAfterMs = nullptr);
bool shouldThrottleUnauthorizedAdminAlert(const String& chatId, unsigned long* retryAfterMs = nullptr);
String roleToText(TelegramRole role);

String getActiveGuestCode();
bool isTemporaryGuestCodeActive();
unsigned long getTemporaryGuestCodeRemainingMs();
unsigned long getGuestCodeCommandCooldownRemainingMs();
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

const char* resetReasonToString(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_UNKNOWN: return "UNKNOWN";
        case ESP_RST_POWERON: return "POWERON";
        case ESP_RST_EXT: return "EXTERNAL";
        case ESP_RST_SW: return "SOFTWARE";
        case ESP_RST_PANIC: return "PANIC";
        case ESP_RST_INT_WDT: return "INT_WDT";
        case ESP_RST_TASK_WDT: return "TASK_WDT";
        case ESP_RST_WDT: return "OTHER_WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT: return "BROWNOUT";
        case ESP_RST_SDIO: return "SDIO";
        default: return "UNMAPPED";
    }
}

bool configureTelegramPollingMode() {
    if (!bot) {
        return false;
    }

    const String deleteWebhookResponse = bot->sendGetToTelegram(
        bot->buildCommand("deleteWebhook?drop_pending_updates=false")
    );
    const bool deleteWebhookOk = deleteWebhookResponse.indexOf("\"ok\":true") >= 0;
    if (deleteWebhookOk) {
        Serial.println("[TELEGRAM] Webhook disabled (polling mode active)");
    } else {
        Serial.println("[TELEGRAM][WARN] Could not confirm deleteWebhook; continuing");
    }

    const String getMeResponse = bot->sendGetToTelegram(bot->buildCommand("getMe"));
    const bool getMeOk = getMeResponse.indexOf("\"ok\":true") >= 0;
    if (getMeOk) {
        Serial.println("[TELEGRAM] Bot API reachable");
    } else {
        Serial.println("[TELEGRAM][WARN] Bot API not reachable during startup check");
    }

    return deleteWebhookOk && getMeOk;
}

void updateNetworkServices() {
    wifiController.update();

    const bool connectedNow = wifiController.isConnected();
    if (connectedNow && !lastWifiConnected) {
        if (!mdnsStarted) {
            if (MDNS.begin(MDNS_HOSTNAME)) {
                MDNS.addService("http", "tcp", 80);
                mdnsStarted = true;
                Serial.println("[MDNS] Service online: http://securelock.local/");
            } else {
                Serial.println("[MDNS][WARN] Failed to start mDNS responder");
            }
        }
    } else if (!connectedNow && lastWifiConnected) {
        if (mdnsStarted) {
            MDNS.end();
            mdnsStarted = false;
            Serial.println("[MDNS] Service stopped (WiFi disconnected)");
        }
    }

    lastWifiConnected = connectedNow;
}

void initializeTelegramBotIfNeeded() {
    if (bot || !webServer.isConnected()) {
        return;
    }

    telegramClient.setInsecure();
    telegramClient.setTimeout(250);
    bot = new UniversalTelegramBot(BOT_TOKEN, telegramClient);
    bot->longPoll = 0;
    telegramReadyAfterMs = millis() + TELEGRAM_STARTUP_GRACE_MS;
    Serial.println("[TELEGRAM] Bot initialized");
    configureTelegramPollingMode();
}

int getTelegramChatSlot(const String& chatId, bool createIfMissing) {
    String normalizedChatId = chatId;
    normalizedChatId.trim();
    if (normalizedChatId.length() == 0) {
        return -1;
    }

    int freeSlot = -1;
    int oldestSlot = -1;
    unsigned long oldestCommandMs = millis();

    for (int i = 0; i < TELEGRAM_TRACKED_CHATS_MAX; i++) {
        if (telegramTrackedChatIds[i] == normalizedChatId) {
            return i;
        }

        if (freeSlot < 0 && telegramTrackedChatIds[i].length() == 0) {
            freeSlot = i;
        }

        if (telegramTrackedLastCommandMs[i] <= oldestCommandMs) {
            oldestCommandMs = telegramTrackedLastCommandMs[i];
            oldestSlot = i;
        }
    }

    if (!createIfMissing) {
        return -1;
    }

    const int targetSlot = (freeSlot >= 0) ? freeSlot : oldestSlot;
    if (targetSlot < 0) {
        return -1;
    }

    telegramTrackedChatIds[targetSlot] = normalizedChatId;
    telegramTrackedLastCommandMs[targetSlot] = 0;
    telegramTrackedLastDangerousMs[targetSlot] = 0;
    telegramTrackedLastUnknownNoticeMs[targetSlot] = 0;
    telegramTrackedLastUnauthorizedAlertMs[targetSlot] = 0;
    return targetSlot;
}

bool isDangerousAdminCommand(const String& command) {
    return command == "/admin_open"
        || command == "/lockdown"
        || command == "/unlockdown"
        || command == "/guest_code"
        || command == "/reboot";
}

bool shouldThrottleTelegramCommand(const String& chatId, bool dangerous, unsigned long* retryAfterMs) {
    if (retryAfterMs) {
        *retryAfterMs = 0;
    }

    const int slot = getTelegramChatSlot(chatId, true);
    if (slot < 0) {
        return false;
    }

    const unsigned long now = millis();
    const unsigned long sinceLastCommand = now - telegramTrackedLastCommandMs[slot];
    if (telegramTrackedLastCommandMs[slot] > 0 && sinceLastCommand < TELEGRAM_COMMAND_MIN_INTERVAL_MS) {
        if (retryAfterMs) {
            *retryAfterMs = TELEGRAM_COMMAND_MIN_INTERVAL_MS - sinceLastCommand;
        }
        return true;
    }

    if (dangerous && telegramTrackedLastDangerousMs[slot] > 0) {
        const unsigned long sinceLastDangerous = now - telegramTrackedLastDangerousMs[slot];
        if (sinceLastDangerous < TELEGRAM_DANGEROUS_COMMAND_COOLDOWN_MS) {
            if (retryAfterMs) {
                *retryAfterMs = TELEGRAM_DANGEROUS_COMMAND_COOLDOWN_MS - sinceLastDangerous;
            }
            return true;
        }
    }

    telegramTrackedLastCommandMs[slot] = now;
    if (dangerous) {
        telegramTrackedLastDangerousMs[slot] = now;
    }

    return false;
}

bool shouldThrottleUnknownNotice(const String& chatId, unsigned long* retryAfterMs) {
    if (retryAfterMs) {
        *retryAfterMs = 0;
    }

    const int slot = getTelegramChatSlot(chatId, true);
    if (slot < 0) {
        return false;
    }

    const unsigned long now = millis();
    if (telegramTrackedLastUnknownNoticeMs[slot] > 0) {
        const unsigned long elapsed = now - telegramTrackedLastUnknownNoticeMs[slot];
        if (elapsed < TELEGRAM_UNKNOWN_NOTICE_COOLDOWN_MS) {
            if (retryAfterMs) {
                *retryAfterMs = TELEGRAM_UNKNOWN_NOTICE_COOLDOWN_MS - elapsed;
            }
            return true;
        }
    }

    telegramTrackedLastUnknownNoticeMs[slot] = now;
    return false;
}

bool shouldThrottleUnauthorizedAdminAlert(const String& chatId, unsigned long* retryAfterMs) {
    if (retryAfterMs) {
        *retryAfterMs = 0;
    }

    const int slot = getTelegramChatSlot(chatId, true);
    if (slot < 0) {
        return false;
    }

    const unsigned long now = millis();
    if (telegramTrackedLastUnauthorizedAlertMs[slot] > 0) {
        const unsigned long elapsed = now - telegramTrackedLastUnauthorizedAlertMs[slot];
        if (elapsed < TELEGRAM_UNAUTHORIZED_ALERT_COOLDOWN_MS) {
            if (retryAfterMs) {
                *retryAfterMs = TELEGRAM_UNAUTHORIZED_ALERT_COOLDOWN_MS - elapsed;
            }
            return true;
        }
    }

    telegramTrackedLastUnauthorizedAlertMs[slot] = now;
    return false;
}

String roleToText(TelegramRole role) {
    switch (role) {
        case ROLE_ADMIN:
            return "admin";
        case ROLE_USER:
            return "user";
        default:
            return "unknown";
    }
}

const char* keypadStateToText(KeypadState state) {
    switch (state) {
        case STATE_AWAITING_2FA:
            return "AWAITING_2FA";
        case STATE_BACKUP_ONLY:
            return "BACKUP_ONLY";
        case STATE_IDLE:
        default:
            return "IDLE";
    }
}

void setup() {
    Serial.begin(115200);

    const esp_reset_reason_t resetReason = esp_reset_reason();
    Serial.print("[BOOT] Reset reason: ");
    Serial.println(resetReasonToString(resetReason));

    lockManager.init();
    securityManager.init();
    authHandler.init();

    wifiController.begin(
        WIFI_CREDENTIALS,
        sizeof(WIFI_CREDENTIALS) / sizeof(WIFI_CREDENTIALS[0]),
        MDNS_HOSTNAME
    );

    const unsigned long wifiBootstrapStartMs = millis();
    while (!wifiController.isConnected() && (millis() - wifiBootstrapStartMs) < 15000UL) {
        updateNetworkServices();
        delay(50);
        yield();
    }

    updateNetworkServices();
    webServer.init();
    initializeTelegramBotIfNeeded();

    randomSeed(static_cast<unsigned long>(esp_random()));
    previousDoorOpen = lockManager.isDoorOpen();
    previousLockedState = lockManager.isLocked();
}

void loop() {
    lockManager.update();
    securityManager.update();
    authHandler.update();
    updateNetworkServices();
    webServer.update();
    initializeTelegramBotIfNeeded();

    checkGuestCodeExpiry();

    const bool currentDoorOpen = lockManager.isDoorOpen();
    const bool currentLockedState = lockManager.isLocked();

    if (previousDoorOpen && !currentDoorOpen && pendingDoorSecuredLog) {
        webServer.logActivity("System", "Door Physically Closed & Secured", "success");
        pendingDoorSecuredLog = false;
    }

    if (!previousLockedState && currentLockedState && pendingDoorSecuredLog && !currentDoorOpen) {
        webServer.logActivity("System", "Access Window Secured (Relocked)", "success");
        pendingDoorSecuredLog = false;
    }

    previousDoorOpen = currentDoorOpen;
    previousLockedState = currentLockedState;

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

    if (!isLockdown && keypadState != STATE_BACKUP_ONLY) {
        AuthResult rfidResult = authHandler.checkRFID();
        if (rfidResult == AUTH_SUCCESS) {
            const String uid = authHandler.getLastRFIDUID();
            const String userName = authHandler.getUserName(uid);
            const unsigned long scanMs = authHandler.getLastRFIDScanMs();
            const unsigned long nowMs = millis();
            bool shouldEnter2FA = true;

            if (keypadState == STATE_AWAITING_2FA) {
                if (uid == pendingUID) {
                    authHandler.startRFIDCooldown();
                    shouldEnter2FA = false;
                }

                if (shouldEnter2FA) {
                    authHandler.startRFIDCooldown();
                    if ((nowMs - lastPending2FAConflictFeedbackMs) >= PENDING_2FA_CONFLICT_FEEDBACK_MS) {
                        securityManager.beep(2);
                        lastPending2FAConflictFeedbackMs = nowMs;
                    }

                    if ((nowMs - lastPending2FAConflictLogMs) >= PENDING_2FA_CONFLICT_LOG_MS) {
                        webServer.logActivity(
                            userName,
                            "RFID Blocked (2FA in progress for " + (pendingUserName.length() ? pendingUserName : String("another user")) + ")",
                            "fail"
                        );
                        lastPending2FAConflictLogMs = nowMs;
                    }

                    shouldEnter2FA = false;
                }
            }

            if (shouldEnter2FA) {
                authHandler.startRFIDCooldown();

                const bool successFeedbackAllowed = (nowMs - lastRfidSuccessFeedbackMs) >= RFID_SUCCESS_FEEDBACK_MIN_INTERVAL_MS;
                if (successFeedbackAllowed) {
                    lastRfidSuccessFeedbackMs = nowMs;
                    securityManager.beep(1);
                    webServer.logActivity(userName, "RFID Recognized", "success");

                    if (scanMs > 0 && scanMs != lastTelegramRfidAlertScanMs) {
                        const bool newUid = uid != lastTelegramRfidAlertUid;
                        const bool intervalElapsed = (millis() - lastTelegramRfidAlertSentMs) >= RFID_ALERT_MIN_INTERVAL_MS;
                        if (newUid || intervalElapsed) {
                            sendRfidScanAlert(uid, true, userName);
                            lastTelegramRfidAlertSentMs = millis();
                            lastTelegramRfidAlertUid = uid;
                        }

                        lastTelegramRfidAlertScanMs = scanMs;
                    }
                }

                enterAwaiting2FAForUser(uid);
            }
        } else if (rfidResult == AUTH_DENIED) {
            const String uid = authHandler.getLastRFIDUID();
            const unsigned long scanMs = authHandler.getLastRFIDScanMs();
            const unsigned long nowMs = millis();

            authHandler.startRFIDCooldown();

            const bool deniedFeedbackAllowed = (nowMs - lastRfidDeniedFeedbackMs) >= RFID_DENIED_FEEDBACK_MIN_INTERVAL_MS;
            if (deniedFeedbackAllowed) {
                lastRfidDeniedFeedbackMs = nowMs;

                securityManager.beep(2);
                webServer.logActivity("Unknown RFID", "RFID Access Denied", "fail");

                if (scanMs > 0 && scanMs != lastTelegramRfidAlertScanMs) {
                    const bool newUid = uid != lastTelegramRfidAlertUid;
                    const bool intervalElapsed = (millis() - lastTelegramRfidAlertSentMs) >= RFID_ALERT_MIN_INTERVAL_MS;
                    if (newUid || intervalElapsed) {
                        sendRfidScanAlert(uid, false, "");
                        lastTelegramRfidAlertSentMs = millis();
                        lastTelegramRfidAlertUid = uid;
                    }

                    lastTelegramRfidAlertScanMs = scanMs;
                }
            }
        }
    }

    if (keypadState == STATE_AWAITING_2FA && pendingOtpIssuedAtMs > 0) {
        if ((millis() - pendingOtpIssuedAtMs) >= OTP_TTL_MS) {
            securityManager.beep(2);
            clearPending2FA("OTP timeout", true);
        }
    }

    if (keypadState == STATE_BACKUP_ONLY && backupOnlyModeStartedAtMs > 0) {
        if ((millis() - backupOnlyModeStartedAtMs) >= BACKUP_ONLY_MODE_TIMEOUT_MS) {
            securityManager.beep(2);
            clearBackupOnlyMode("Backup PIN timeout", false);
        }
    }

    if (isLockdown) {
        // Keep keypad telemetry responsive for diagnostics/echo even in lockdown mode,
        // while intentionally skipping all local auth actions.
        authHandler.getKeypadKey();

        if (keypadState == STATE_AWAITING_2FA) {
            clearPending2FA("Lockdown activated", false);
        } else if (keypadState == STATE_BACKUP_ONLY) {
            clearBackupOnlyMode("Lockdown activated", false);
        }
        keypadBuffer = "";
        authHandler.clearBuffer();
        handleTelegramCommands();
        processQueuedWebAdminActivityTelegram();
        yield();
        return;
    }

    processKeypad();
    handleTelegramCommands();
    processQueuedWebAdminActivityTelegram();
    yield();
}

void processKeypad() {
    const char key = authHandler.getKeypadKey();
    if (!key) {
        return;
    }

    Serial.print("[KEYPAD] key=");
    Serial.print(key);
    Serial.print(" state=");
    Serial.print(keypadStateToText(keypadState));
    Serial.print(" len=");
    Serial.println(keypadBuffer.length());

    if (keypadState == STATE_IDLE && keypadBuffer.length() > 0) {
        const unsigned long nowMs = millis();
        const bool noRecentIdleInput = (keypadIdleBufferLastInputMs == 0);
        const bool idleBufferExpired = !noRecentIdleInput
            && ((nowMs - keypadIdleBufferLastInputMs) >= KEYPAD_IDLE_BUFFER_TIMEOUT_MS);

        if (noRecentIdleInput || idleBufferExpired) {
            keypadBuffer = "";
            keypadIdleBufferLastInputMs = 0;
            authHandler.clearBuffer();
        }
    }

    if (key == '*') {
        keypadBuffer = "";
        keypadIdleBufferLastInputMs = 0;
        authHandler.clearBuffer();
        if (keypadState == STATE_AWAITING_2FA) {
            clearPending2FA("User cancelled", false);
            securityManager.beep(1);
            Serial.println("[KEYPAD] 2FA cancelled by user");
        } else if (keypadState == STATE_BACKUP_ONLY) {
            clearBackupOnlyMode("Backup mode cancelled", false);
            securityManager.beep(1);
            Serial.println("[KEYPAD] Backup-only mode cancelled");
        }
        return;
    }

    if (key == 'B' || key == 'b') {
        if (keypadState == STATE_AWAITING_2FA && !awaitingOfflineBackupMode) {
            awaitingOfflineBackupMode = true;
            keypadBuffer = "";
            authHandler.clearBuffer();
            authPrompt = "Offline Mode: Enter Backup PIN";
            webServer.logActivity(pendingUserName, "Offline Backup Mode", "success");
            securityManager.beep(1);
            Serial.println("[KEYPAD] Switched to offline backup mode (2FA)");
        } else if (keypadState == STATE_IDLE) {
            keypadState = STATE_BACKUP_ONLY;
            keypadBuffer = "";
            keypadIdleBufferLastInputMs = 0;
            authHandler.clearBuffer();
            awaitingOfflineBackupMode = false;
            backupOnlyModeStartedAtMs = millis();
            authPrompt = "Backup Access: Enter 4-digit Backup PIN";
            webServer.logActivity("Backup Access", "Backup Mode Started", "success");
            securityManager.beep(1);
            Serial.println("[KEYPAD] Backup-only mode started via B key");
        }
        return;
    }

    if (key == '#') {
        if (keypadState == STATE_IDLE) {
            if (guestCodeActive && keypadBuffer.length() > 0) {
                if (keypadBuffer.length() == BACKUP_PIN_LENGTH && keypadBuffer == guestCode) {
                    grantAccess("Guest", "Guest PIN");
                    guestCode = "";
                    guestCodeActive = false;
                    guestPinFailureCount = 0;
                    Serial.println("[KEYPAD] Guest PIN accepted via # submit");
                } else {
                    securityManager.beep(2);
                    webServer.logActivity("Guest", "Guest PIN Failed", "fail");

                    const unsigned long nowMs = millis();
                    if (lastGuestPinFailureMs == 0 || (nowMs - lastGuestPinFailureMs) > AUTH_FAILURE_WINDOW_MS) {
                        guestPinFailureCount = 0;
                    }

                    guestPinFailureCount++;
                    lastGuestPinFailureMs = nowMs;

                    if (guestPinFailureCount >= AUTH_FAILURE_ALERT_THRESHOLD
                        && (nowMs - lastGuestPinFailureAlertMs) >= KEYPAD_AUTH_ALERT_MIN_INTERVAL_MS) {
                        const unsigned long remainingSec = (getTemporaryGuestCodeRemainingMs() + 999) / 1000;
                        String alertMessage = "⚠️ Repeated guest PIN failures detected";
                        if (remainingSec > 0) {
                            alertMessage += " (" + String(remainingSec) + "s guest window remaining).";
                        } else {
                            alertMessage += ".";
                        }
                        enqueueAdminNotification(alertMessage);
                        lastGuestPinFailureAlertMs = nowMs;
                        guestPinFailureCount = 0;
                        lastGuestPinFailureMs = 0;
                    }
                    Serial.println("[KEYPAD] Guest PIN failed via # submit");
                }

                keypadBuffer = "";
                keypadIdleBufferLastInputMs = 0;
                authHandler.clearBuffer();
                return;
            }

            keypadState = STATE_BACKUP_ONLY;
            awaitingOfflineBackupMode = false;
            keypadBuffer = "";
            keypadIdleBufferLastInputMs = 0;
            authHandler.clearBuffer();
            backupOnlyModeStartedAtMs = millis();
            authPrompt = "Backup Access: Enter 4-digit Backup PIN";
            webServer.logActivity("Backup Access", "Backup Mode Started", "success");
            securityManager.beep(1);
            Serial.println("[KEYPAD] Backup-only mode started via #");
        } else if (keypadState == STATE_AWAITING_2FA) {
            if (!awaitingOfflineBackupMode) {
                awaitingOfflineBackupMode = true;
                keypadBuffer = "";
                keypadIdleBufferLastInputMs = 0;
                authHandler.clearBuffer();
                authPrompt = "Offline Mode: Enter Backup PIN";
                webServer.logActivity(pendingUserName, "Offline Backup Mode", "success");
                Serial.println("[KEYPAD] Switched to offline backup mode via # (2FA)");
            } else {
                if (keypadBuffer.length() == 0) {
                    authPrompt = "Offline Mode: Enter Backup PIN";
                    Serial.println("[KEYPAD] # ignored: awaiting offline backup PIN");
                    return;
                }
                evaluateAwaiting2FABuffer(true);
            }
        } else if (keypadState == STATE_BACKUP_ONLY) {
            if (keypadBuffer.length() == 0) {
                authPrompt = "Backup Access: Enter 4-digit Backup PIN";
                Serial.println("[KEYPAD] # ignored: awaiting backup-only PIN");
                return;
            }
            evaluateBackupOnlyBuffer(true);
        }
        return;
    }

    if (key < '0' || key > '9') {
        return;
    }

    if (keypadState == STATE_IDLE) {
        if (!guestCodeActive) {
            keypadBuffer = "";
            keypadIdleBufferLastInputMs = 0;
            authHandler.clearBuffer();
            return;
        }

        if (keypadBuffer.length() >= BACKUP_PIN_LENGTH) {
            keypadBuffer = "";
        }

        keypadBuffer += key;
        keypadIdleBufferLastInputMs = millis();

        if (keypadBuffer.length() < BACKUP_PIN_LENGTH) {
            return;
        }

        if (keypadBuffer == guestCode) {
            grantAccess("Guest", "Guest PIN");
            guestCode = "";
            guestCodeActive = false;
            guestPinFailureCount = 0;
            keypadBuffer = "";
            keypadIdleBufferLastInputMs = 0;
            Serial.println("[KEYPAD] Guest PIN accepted via direct numeric entry");
            return;
        }

        securityManager.beep(2);
        webServer.logActivity("Guest", "Guest PIN Failed", "fail");

        const unsigned long nowMs = millis();
        if (lastGuestPinFailureMs == 0 || (nowMs - lastGuestPinFailureMs) > AUTH_FAILURE_WINDOW_MS) {
            guestPinFailureCount = 0;
        }

        guestPinFailureCount++;
        lastGuestPinFailureMs = nowMs;

        keypadBuffer = "";
        keypadIdleBufferLastInputMs = 0;
        authHandler.clearBuffer();
        Serial.println("[KEYPAD] Guest PIN failed via direct numeric entry");
        return;
    }

    if (keypadState == STATE_BACKUP_ONLY) {
        if (keypadBuffer.length() < BACKUP_PIN_LENGTH) {
            keypadBuffer += key;
            evaluateBackupOnlyBuffer(false);
        }
        return;
    }

    const size_t maxLen = awaitingOfflineBackupMode ? BACKUP_PIN_LENGTH : OTP_LENGTH;
    if (keypadBuffer.length() < maxLen) {
        keypadBuffer += key;
        evaluateAwaiting2FABuffer(false);
    }
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
            Serial.println("[AUTH] Duress code accepted during 2FA");
            return true;
        }

        if (awaitingOfflineBackupMode && pendingBackupPin.length() == BACKUP_PIN_LENGTH && keypadBuffer == pendingBackupPin) {
            grantAccess(pendingUserName, "RFID + Backup PIN", pendingUserChatId);
            Serial.println("[AUTH] RFID + Backup PIN accepted");
            return true;
        }

        if (awaitingOfflineBackupMode && explicitSubmit) {
            securityManager.beep(2);
            clearPending2FA("Backup PIN failed", true);
            Serial.println("[AUTH] RFID + Backup PIN failed");
            return true;
        }
    }

    if (!awaitingOfflineBackupMode && len == OTP_LENGTH) {
        if (pendingOtp.length() == OTP_LENGTH && keypadBuffer == pendingOtp) {
            grantAccess(pendingUserName, "RFID + Telegram OTP", pendingUserChatId);
            Serial.println("[AUTH] RFID + Telegram OTP accepted");
        } else {
            securityManager.beep(2);
            clearPending2FA("OTP failed", true);
            Serial.println("[AUTH] RFID + Telegram OTP failed");
        }
        return true;
    }

    if (explicitSubmit) {
        securityManager.beep(2);
        clearPending2FA("Invalid 2FA input length", true);
        return true;
    }

    return false;
}

int resolveUserByBackupPin(const String& pin, String* matchedUid, String* matchedUserName, String* matchedChatId) {
    int matches = 0;

    if (matchedUid) {
        *matchedUid = "";
    }
    if (matchedUserName) {
        *matchedUserName = "";
    }
    if (matchedChatId) {
        *matchedChatId = "";
    }

    if (pin.length() != BACKUP_PIN_LENGTH) {
        return 0;
    }

    const int userCount = authHandler.getUserCount();
    for (int i = 0; i < userCount; i++) {
        const String uid = authHandler.getUserUIDAt(i);
        if (uid.length() == 0 || uid.startsWith("GUEST_")) {
            continue;
        }

        String backupPin = authHandler.getUserBackupPIN(uid);
        backupPin.trim();
        if (backupPin.length() != BACKUP_PIN_LENGTH) {
            continue;
        }

        if (backupPin != pin) {
            continue;
        }

        matches++;
        if (matches == 1) {
            if (matchedUid) {
                *matchedUid = uid;
            }

            if (matchedUserName) {
                String name = authHandler.getUserName(uid);
                name.trim();
                if (name.length() == 0 || name == "Unknown") {
                    name = "User";
                }
                *matchedUserName = name;
            }

            if (matchedChatId) {
                *matchedChatId = authHandler.getUserTelegramChatId(uid);
            }
        }
    }

    return matches;
}

bool evaluateBackupOnlyBuffer(bool explicitSubmit) {
    const size_t len = keypadBuffer.length();

    if (len == BACKUP_PIN_LENGTH) {
        String matchedUid = "";
        String matchedUserName = "";
        String matchedChatId = "";
        const int matches = resolveUserByBackupPin(keypadBuffer, &matchedUid, &matchedUserName, &matchedChatId);

        if (matches == 1) {
            backupPinFailureCount = 0;
            lastBackupPinFailureMs = 0;
            grantAccess(matchedUserName, "Backup PIN (Keypad Only)", matchedChatId);
            Serial.print("[AUTH] Backup-only PIN accepted for user=");
            Serial.println(matchedUserName);
            return true;
        }

        if (matches > 1) {
            securityManager.beep(3);
            webServer.logActivity("Backup Access", "Backup PIN Collision", "alarm");

            const unsigned long nowMs = millis();
            if ((nowMs - lastBackupPinCollisionAlertMs) >= KEYPAD_AUTH_ALERT_MIN_INTERVAL_MS) {
                enqueueAdminNotification(
                    "🚨 Backup PIN collision detected during keypad-only access. Ensure each user has a unique backup PIN."
                );
                lastBackupPinCollisionAlertMs = nowMs;
            }

            clearBackupOnlyMode("Backup PIN collision", false);
            Serial.println("[AUTH] Backup-only PIN rejected due to collision");
            return true;
        }

        if (guestCodeActive && keypadBuffer == guestCode) {
            grantAccess("Guest", "Guest PIN");
            guestCode = "";
            guestCodeActive = false;
            guestPinFailureCount = 0;
            keypadIdleBufferLastInputMs = 0;
            Serial.println("[AUTH] Guest PIN accepted in backup-only mode");
            return true;
        }

        securityManager.beep(3);
        webServer.logActivity("Backup Access", "Backup PIN Failed", "fail");

        const unsigned long nowMs = millis();
        if (lastBackupPinFailureMs == 0 || (nowMs - lastBackupPinFailureMs) > AUTH_FAILURE_WINDOW_MS) {
            backupPinFailureCount = 0;
        }

        backupPinFailureCount++;
        lastBackupPinFailureMs = nowMs;

        if (backupPinFailureCount >= AUTH_FAILURE_ALERT_THRESHOLD
            && (nowMs - lastBackupPinFailureAlertMs) >= KEYPAD_AUTH_ALERT_MIN_INTERVAL_MS) {
            enqueueAdminNotification("⚠️ Backup PIN failed attempt detected in keypad-only mode.");
            lastBackupPinFailureAlertMs = nowMs;
            backupPinFailureCount = 0;
            lastBackupPinFailureMs = 0;
        }

        clearBackupOnlyMode("Backup PIN failed", false);
        Serial.println("[AUTH] Backup-only PIN failed (no match)");
        return true;
    }

    if (explicitSubmit) {
        securityManager.beep(2);
        authPrompt = "Backup Access: Enter exactly 4 digits";
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
    keypadIdleBufferLastInputMs = 0;
    authHandler.clearBuffer();
    keypadState = STATE_AWAITING_2FA;
    awaitingOfflineBackupMode = false;
    backupOnlyModeStartedAtMs = 0;

    if (bot) {
        pendingOtp = generateNumericCode(OTP_LENGTH);
        pendingOtpIssuedAtMs = millis();

        String otpTargetChat = pendingUserChatId;
        otpTargetChat.trim();
        if (otpTargetChat.length() == 0) {
            awaitingOfflineBackupMode = true;
            pendingOtp = "";
            pendingOtpIssuedAtMs = 0;
            authPrompt = "Offline Mode: Enter Backup PIN";
            securityManager.beep(1);
            webServer.logActivity(pendingUserName, "OTP Unavailable - Missing Chat ID", "success");
            return;
        }

        const bool otpSent = sendOTP(otpTargetChat, pendingOtp);
        if (!otpSent) {
            awaitingOfflineBackupMode = true;
            pendingOtp = "";
            pendingOtpIssuedAtMs = 0;
            authPrompt = "Offline Mode: Enter Backup PIN";
            securityManager.beep(1);
            webServer.logActivity(pendingUserName, "OTP Send Failed - Backup PIN", "success");
            return;
        }

        webServer.logActivity(pendingUserName, "OTP Sent", "success");
        authPrompt = "Enter 4-digit OTP (press # or B for Offline Mode)";
    } else {
        awaitingOfflineBackupMode = true;
        authPrompt = "Offline Mode: Enter Backup PIN";
        webServer.logActivity(pendingUserName, "OTP Unavailable - Backup PIN", "success");
    }
}

void grantAccess(const String& actor, const String& method, const String& userChatId) {
    lockManager.unlock();
    authHandler.startRFIDCooldown();
    securityManager.beep(2);
    pendingDoorSecuredLog = true;
    guestPinFailureCount = 0;
    backupPinFailureCount = 0;
    lastGuestPinFailureMs = 0;
    lastBackupPinFailureMs = 0;
    keypadIdleBufferLastInputMs = 0;

    if (securityManager.isAlarming()) {
        securityManager.clearAlarm();
    }

    webServer.logActivity(actor, method, "success");
    queueAccessEventForAdmins(actor, method);

    String targetChatId = userChatId;
    targetChatId.trim();
    if (targetChatId.length() > 0) {
        const unsigned long autoLockSec = (lockManager.getAutoLockDelayMs() + 999) / 1000;
        const bool userNotified = sendTelegramText(
            targetChatId,
            "✅ Access granted via " + method + ". Door will auto-lock in " + String(autoLockSec) + "s."
        );

        if (!userNotified) {
            webServer.logActivity(actor, "User Access Telegram Notify Failed", "fail");
        }
    } else if (actor != "Guest") {
        webServer.logActivity(actor, "User Access Telegram Notify Skipped (No Chat ID)", "fail");
    }

    clearPending2FA("Access granted", false);
}

void clearBackupOnlyMode(const String& reason, bool logFailure) {
    if (keypadState == STATE_BACKUP_ONLY && logFailure) {
        webServer.logActivity("Backup Access", reason, "fail");
    }

    keypadState = STATE_IDLE;
    awaitingOfflineBackupMode = false;
    authPrompt = "";
    keypadBuffer = "";
    keypadIdleBufferLastInputMs = 0;
    backupOnlyModeStartedAtMs = 0;
    authHandler.clearBuffer();
}

void clearPending2FA(const String& reason, bool logFailure) {
    if (logFailure) {
        if (keypadState == STATE_AWAITING_2FA) {
            webServer.logActivity(pendingUserName.length() ? pendingUserName : "Unknown", reason, "fail");
        } else if (keypadState == STATE_BACKUP_ONLY) {
            webServer.logActivity("Backup Access", reason, "fail");
        }
    }

    keypadState = STATE_IDLE;
    pendingUID = "";
    pendingUserName = "";
    pendingUserChatId = "";
    pendingBackupPin = "";
    pendingOtp = "";
    pendingOtpIssuedAtMs = 0;
    awaitingOfflineBackupMode = false;
    backupOnlyModeStartedAtMs = 0;
    authPrompt = "";
    keypadBuffer = "";
    keypadIdleBufferLastInputMs = 0;
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

    if (millis() < telegramReadyAfterMs) {
        return;
    }

    if (WiFi.status() != WL_CONNECTED) {
        telegramPollIntervalMs = TELEGRAM_POLL_ERROR_MS;
        return;
    }

    const unsigned long now = millis();
    if ((now - lastTelegramPollMs) < telegramPollIntervalMs) {
        return;
    }
    lastTelegramPollMs = now;

    const unsigned long pollStartMs = millis();
    int processedInCycle = 0;
    int consumedUpdatesInCycle = 0;

    int numNewMessages = bot->getUpdates(bot->last_message_received + 1);
    if (numNewMessages < 0) {
        telegramPollErrors++;
        telegramLastErrorMs = millis();
        telegramPollIntervalMs = TELEGRAM_POLL_ERROR_MS;
        telegramPendingApprox = 0;
        telegramLastPollDurationMs = millis() - pollStartMs;

        const unsigned long nowMs = millis();
        if (telegramLastErrorLogMs == 0 || (nowMs - telegramLastErrorLogMs) >= 5000UL) {
            Serial.print("[TELEGRAM][WARN] getUpdates failed, error=");
            Serial.print(bot->_lastError);
            Serial.print(", pollErrors=");
            Serial.println(telegramPollErrors);
            telegramLastErrorLogMs = nowMs;
        }

        return;
    }

    const unsigned long pollDurationMs = millis() - pollStartMs;
    if (pollDurationMs > 2500) {
        telegramPollIntervalMs = TELEGRAM_POLL_ERROR_MS;
    }

    if (numNewMessages > 0) {
        const int safeLimit = (TELEGRAM_MAX_PROCESS_PER_CYCLE < TELEGRAM_SAFE_MESSAGE_SLOTS)
            ? TELEGRAM_MAX_PROCESS_PER_CYCLE
            : TELEGRAM_SAFE_MESSAGE_SLOTS;
        const int processLimit = (numNewMessages < safeLimit)
            ? numNewMessages
            : safeLimit;

        for (int i = 0; i < processLimit; i++) {
            const int updateId = bot->messages[i].update_id;
            if (updateId <= lastHandledTelegramUpdateId) {
                continue;
            }

            String chatId = bot->messages[i].chat_id;
            String text = normalizeTelegramCommand(bot->messages[i].text);
            chatId.trim();

            const TelegramRole role = resolveTelegramRole(chatId);
            if (role == ROLE_UNKNOWN) {
                lastHandledTelegramUpdateId = updateId;
                consumedUpdatesInCycle++;
                handleUnknownTelegramChat(chatId);
                continue;
            }

            if (text.length() == 0) {
                lastHandledTelegramUpdateId = updateId;
                consumedUpdatesInCycle++;
                continue;
            }

            lastHandledTelegramUpdateId = updateId;
            consumedUpdatesInCycle++;

            const bool dangerous = (role == ROLE_ADMIN) && isDangerousAdminCommand(text);
            unsigned long retryAfterMs = 0;
            if (shouldThrottleTelegramCommand(chatId, dangerous, &retryAfterMs)) {
                if (dangerous) {
                    sendTelegramText(
                        chatId,
                        "Command cooling down. Retry in " + String((retryAfterMs + 999) / 1000) + "s."
                    );
                }
                trackTelegramCommand(roleToText(role), text, "rate_limited", 0);
                continue;
            }

            if (role == ROLE_ADMIN) {
                handleAdminCommand(chatId, text);
            } else if (role == ROLE_USER) {
                handleUserCommand(chatId, text);
            }

            processedInCycle++;
        }

        telegramPendingApprox = numNewMessages - consumedUpdatesInCycle;
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
    } else if (numNewMessages > 0) {
        telegramLastSuccessMs = millis();
        telegramPollIntervalMs = TELEGRAM_POLL_FAST_MS;
    } else {
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
    String normalizedChatId = chatId;
    normalizedChatId.trim();

    if (isAdmin(normalizedChatId)) {
        return ROLE_ADMIN;
    }

    if (authHandler.isKnownTelegramChatId(normalizedChatId)) {
        return ROLE_USER;
    }

    return ROLE_UNKNOWN;
}

bool isAdmin(String incoming_chat_id) {
    incoming_chat_id.trim();
    if (incoming_chat_id.length() == 0) {
        return false;
    }

    for (int i = 0; i < NUM_ADMINS; i++) {
        String knownAdminId = ADMIN_CHAT_IDS[i];
        knownAdminId.trim();
        if (incoming_chat_id == knownAdminId) {
            return true;
        }
    }

    return false;
}

bool notifyAdmins(const String& message) {
    bool deliveredToAtLeastOne = false;

    for (int i = 0; i < NUM_ADMINS; i++) {
        String adminChatId = ADMIN_CHAT_IDS[i];
        adminChatId.trim();
        if (adminChatId.length() == 0) {
            continue;
        }

        if (sendTelegramText(adminChatId, message)) {
            deliveredToAtLeastOne = true;
        }
    }

    return deliveredToAtLeastOne;
}

String getUserNameByTelegramChatId(const String& chatId) {
    String normalizedChatId = chatId;
    normalizedChatId.trim();
    if (normalizedChatId.length() == 0) {
        return "User";
    }

    const int userCount = authHandler.getUserCount();
    for (int i = 0; i < userCount; i++) {
        const String uid = authHandler.getUserUIDAt(i);
        if (uid.length() == 0) {
            continue;
        }

        String listedChatId = authHandler.getUserTelegramChatId(uid);
        listedChatId.trim();
        if (listedChatId == normalizedChatId) {
            String userName = authHandler.getUserName(uid);
            userName.trim();
            if (userName.length() == 0 || userName == "Unknown") {
                return "User";
            }

            return userName;
        }
    }

    return "User";
}

void handleUnknownTelegramChat(const String& chatId) {
    const unsigned long cmdStartMs = millis();

    unsigned long retryAfterMs = 0;
    if (shouldThrottleUnknownNotice(chatId, &retryAfterMs)) {
        webServer.logActivity("Unregistered Telegram", "Unregistered chat throttled", "fail");
        trackTelegramCommand("unknown", "unregistered", "unregistered_throttled", millis() - cmdStartMs);
        return;
    }

    const String message = "🚫 Access not registered.\n"
        "Please send this Chat ID to your SecureLock admin for approval:\n"
        "🆔 " + chatId;
    const bool sent = sendTelegramText(chatId, message);
    webServer.logActivity("Unregistered Telegram", "Unregistered chat attempted access", sent ? "alarm" : "fail");
    trackTelegramCommand("unknown", "unregistered", sent ? "unregistered_notified" : "send_fail", millis() - cmdStartMs);
}

void handleAdminCommand(const String& chatId, const String& text) {
    const unsigned long cmdStartMs = millis();

    if (text == "/help") {
        const bool sent = sendTelegramText(
            chatId,
            "SecureLock Admin Commands:\n"
            "🟢 /start - verify admin session\n"
            "📊 /status - live lock/system status\n"
            "👤 /my_info - view your admin account summary\n"
            "🔓 /admin_open - emergency unlock (policy protected)\n"
            "⏱️ /guest_code - generate a 30-second guest PIN\n"
            "🔔 /buzzer_test - run buzzer diagnostic\n"
            "⌨️ /keypad_echo - read last keypad key event\n"
            "🛡️ /lockdown - disable local auth inputs\n"
            "✅ /unlockdown - re-enable local auth inputs\n"
            "♻️ /reboot - restart device"
        );
        webServer.logActivity("Admin (Telegram)", "Help Command", sent ? "success" : "fail");
        trackTelegramCommand("admin", text, sent ? "ok" : "send_fail", millis() - cmdStartMs);
        return;
    }

    if (text == "/start") {
        const bool sent = sendTelegramText(
            chatId,
            "🛡️ Admin session verified. SecureLock is online and ready.\nUse /help to view all commands."
        );
        webServer.logActivity("Admin (Telegram)", "Start Command", sent ? "success" : "fail");
        trackTelegramCommand("admin", text, sent ? "ok" : "send_fail", millis() - cmdStartMs);
        return;
    }

    if (text == "/my_info") {
        const bool adminVerified = isAdmin(chatId);
        const bool online = webServer.isConnected() && (WiFi.status() == WL_CONNECTED);
        const String ipAddress = online ? WiFi.localIP().toString() : String("Unavailable");
        const String infoMessage =
            "👤 Admin Account Summary\n"
            "• Role: Administrator\n"
            "• Chat ID: " + chatId + "\n"
            "• Admin Verified: " + String(adminVerified ? "Yes" : "No") + "\n"
            "• Command Access: Full\n"
            "• Device Link: " + String(online ? "Online" : "Offline") + "\n"
            "• Device IP: " + ipAddress + "\n"
            "Use /help to view all admin commands.";
        const bool sent = sendTelegramText(chatId, infoMessage);
        webServer.logActivity("Admin (Telegram)", "My Info Command", sent ? "success" : "fail");
        trackTelegramCommand("admin", text, sent ? "ok" : "send_fail", millis() - cmdStartMs);
        return;
    }

    if (text == "/open") {
        const bool sent = sendTelegramText(chatId, "ℹ️ This command is deprecated. Please use /admin_open.");
        webServer.logActivity("Admin (Telegram)", "Deprecated Command /open", "fail");
        trackTelegramCommand("admin", text, sent ? "deprecated" : "send_fail", millis() - cmdStartMs);
        return;
    }

    if (text == "/admin_open") {
        const unsigned long guestLockRemainingMs = getTemporaryGuestCodeRemainingMs();
        if (guestLockRemainingMs > 0) {
            const unsigned long retrySec = (guestLockRemainingMs + 999) / 1000;
            const bool sent = sendTelegramText(
                chatId,
                "⏳ Emergency unlock is temporarily unavailable while guest access is active. Retry in " + String(retrySec) + "s."
            );
            webServer.logActivity("Admin (Telegram)", "Emergency Override Blocked (Guest Code Active)", "fail");
            trackTelegramCommand("admin", text, sent ? "blocked_guest_code" : "send_fail", millis() - cmdStartMs);
            return;
        }

        const unsigned long emergencyCooldownRemainingMs = webServer.getEmergencyCooldownRemainingMs();
        if (emergencyCooldownRemainingMs > 0) {
            const unsigned long retrySec = (emergencyCooldownRemainingMs + 999) / 1000;
            const bool sent = sendTelegramText(
                chatId,
                "⏳ Emergency unlock is cooling down. Retry in " + String(retrySec) + "s."
            );
            webServer.logActivity("Admin (Telegram)", "Emergency Override Cooldown", "fail");
            trackTelegramCommand("admin", text, sent ? "cooldown" : "send_fail", millis() - cmdStartMs);
            return;
        }

        lockManager.unlock();
        authHandler.startRFIDCooldown();
        securityManager.beep(1);
        pendingDoorSecuredLog = true;
        webServer.markEmergencyOverride();
        webServer.logActivity("Admin (Telegram)", "Emergency Override", "success");
        queueAccessEventForAdmins("Admin (Telegram)", "Emergency Override");
        clearPending2FA("Admin override", false);
        const bool sent = sendTelegramText(chatId, "✅ Emergency unlock executed. Auto-lock timer is active.");
        trackTelegramCommand("admin", text, sent ? "ok" : "send_fail", millis() - cmdStartMs);
        return;
    }

    if (text == "/guest_code") {
        if (guestCodeActive) {
            const unsigned long remainingSec = (getTemporaryGuestCodeRemainingMs() + 999) / 1000;
            const bool sent = sendTelegramText(
                chatId,
                "ℹ️ Guest PIN is already active: " + guestCode + " (expires in " + String(remainingSec) + "s)."
            );
            webServer.logActivity("Admin (Telegram)", "Guest PIN Reused", sent ? "success" : "fail");
            trackTelegramCommand("admin", text, sent ? "existing_active" : "send_fail", millis() - cmdStartMs);
            return;
        }

        guestCode = generateNumericCode(BACKUP_PIN_LENGTH);
        guestCodeActive = true;
        guestCodeIssuedAtMs = millis();
        guestPinFailureCount = 0;

        if (keypadState == STATE_IDLE) {
            keypadBuffer = "";
            keypadIdleBufferLastInputMs = 0;
            authHandler.clearBuffer();
        }

        securityManager.beep(1);
        const bool sent = sendTelegramText(chatId, "🔐 Guest PIN: " + guestCode + " (valid for 30 seconds). Share only with authorized visitors.");
        webServer.logActivity("Admin (Telegram)", "Guest PIN Generated", sent ? "success" : "fail");
        trackTelegramCommand("admin", text, sent ? "ok" : "send_fail", millis() - cmdStartMs);
        return;
    }

    if (text == "/status") {
        const String doorState = lockManager.isDoorOpen() ? "Open" : "Closed";
        const String systemState = isLockdown ? "Lockdown" : "Armed";
        const String wifiState = String(WiFi.RSSI()) + " dBm";
        const String buzzerState = securityManager.isSirenActive()
            ? "SIREN"
            : (securityManager.isBuzzerActive() ? "BEEPING" : "IDLE");
        const String statusMsg = "📡 SecureLock Status\n"
            "• Door: " + doorState + "\n"
            "• System: " + systemState + "\n"
            "• Buzzer: " + buzzerState + "\n"
            "• WiFi RSSI: " + wifiState;
        const bool sent = sendTelegramText(chatId, statusMsg);
        webServer.logActivity("Admin (Telegram)", "Status Check", sent ? "success" : "fail");
        trackTelegramCommand("admin", text, sent ? "ok" : "send_fail", millis() - cmdStartMs);
        return;
    }

    if (text == "/buzzer_test") {
        securityManager.beep(2);
        const bool sent = sendTelegramText(chatId, "🔔 Buzzer diagnostic executed (short double tone).");
        webServer.logActivity("Admin (Telegram)", "Buzzer Test", sent ? "success" : "fail");
        trackTelegramCommand("admin", text, sent ? "ok" : "send_fail", millis() - cmdStartMs);
        return;
    }

    if (text == "/keypad_echo") {
        const unsigned long keyMs = getLastKeypadKeyMs();
        const String keyLabel = getLastKeypadKeyLabel();
        if (keyLabel.length() == 0 || keyMs == 0) {
            const bool sent = sendTelegramText(chatId, "⌨️ No keypad key recorded yet.");
            webServer.logActivity("Admin (Telegram)", "Keypad Echo", sent ? "success" : "fail");
            trackTelegramCommand("admin", text, sent ? "ok" : "send_fail", millis() - cmdStartMs);
            return;
        }

        const unsigned long ageMs = millis() - keyMs;
        const bool sent = sendTelegramText(chatId, "⌨️ Last keypad key: " + keyLabel + " (" + String(ageMs) + " ms ago)");
        webServer.logActivity("Admin (Telegram)", "Keypad Echo", sent ? "success" : "fail");
        trackTelegramCommand("admin", text, sent ? "ok" : "send_fail", millis() - cmdStartMs);
        return;
    }

    if (text == "/lockdown") {
        isLockdown = true;
        clearPending2FA("Lockdown activated by admin", false);
        const bool sent = sendTelegramText(chatId, "🛡️ Lockdown enabled. Local RFID/keypad authentication is now disabled.");
        webServer.logActivity("Admin (Telegram)", "Lockdown Enabled", sent ? "success" : "fail");
        trackTelegramCommand("admin", text, sent ? "ok" : "send_fail", millis() - cmdStartMs);
        return;
    }

    if (text == "/unlockdown") {
        isLockdown = false;
        const bool sent = sendTelegramText(chatId, "✅ Lockdown disabled. Local RFID/keypad authentication is enabled.");
        webServer.logActivity("Admin (Telegram)", "Lockdown Disabled", sent ? "success" : "fail");
        trackTelegramCommand("admin", text, sent ? "ok" : "send_fail", millis() - cmdStartMs);
        return;
    }

    if (text == "/reboot") {
        const bool sent = sendTelegramText(chatId, "♻️ System rebooting now...");
        webServer.logActivity("Admin (Telegram)", "Reboot Command", sent ? "success" : "fail");
        trackTelegramCommand("admin", text, sent ? "ok" : "send_fail", millis() - cmdStartMs);
        delay(150);
        ESP.restart();
        return;
    }

    const bool sent = sendTelegramText(chatId, "❓ Unknown admin command. Send /help to view the available command list.");
    webServer.logActivity("Admin (Telegram)", "Unknown Command", "fail");
    trackTelegramCommand("admin", text, sent ? "unknown" : "send_fail", millis() - cmdStartMs);
}

void handleUserCommand(const String& chatId, const String& text) {
    const unsigned long cmdStartMs = millis();
    const String userName = getUserNameByTelegramChatId(chatId);

    if (text == "/help") {
        const bool sent = sendTelegramText(
            chatId,
            "SecureLock User Guide:\n"
            "Standard entry:\n"
            "1) Tap your registered RFID card\n"
            "2) Wait for your 4-digit OTP in this chat\n"
            "3) Enter the OTP on keypad\n\n"
            "Offline fallback:\n"
            "1) Tap RFID card\n"
            "2) Press '#' or 'B' for Backup PIN mode\n"
            "3) Enter your 4-digit Backup PIN\n\n"
            "ℹ️ Available commands:\n"
            "🟢 /start\n"
            "👤 /my_info\n"
            "❓ /help"
        );
        webServer.logActivity("User (Telegram)", "Help Command", sent ? "success" : "fail");
        trackTelegramCommand("user", text, sent ? "ok" : "send_fail", millis() - cmdStartMs);
        return;
    }

    if (text == "/start") {
        const bool sent = sendTelegramText(
            chatId,
            "👋 Welcome, " + userName + ". Your Telegram account is linked for OTP delivery after RFID scan.\nUse /help to view available commands."
        );
        webServer.logActivity("User (Telegram)", "Start Command", sent ? "success" : "fail");
        trackTelegramCommand("user", text, sent ? "ok" : "send_fail", millis() - cmdStartMs);
        return;
    }

    if (text == "/my_info") {
        const bool sent = sendTelegramText(
            chatId,
            "👤 Account summary:\n"
            "• User: " + userName + "\n"
            "• RFID: Registered\n"
            "• Backup PIN: Registered"
        );
        webServer.logActivity("User (Telegram)", "My Info Command", sent ? "success" : "fail");
        trackTelegramCommand("user", text, sent ? "ok" : "send_fail", millis() - cmdStartMs);
        return;
    }

    if (text.startsWith("/")) {
        const bool throttleAdminAlert = shouldThrottleUnauthorizedAdminAlert(chatId, nullptr);

        const bool sent = sendTelegramText(
            chatId,
            "🚫 You are not authorized to use this command. This attempt has been logged."
        );
        bool adminAlertSent = false;
        if (!throttleAdminAlert) {
            adminAlertSent = notifyAdmins(
                "🚨 Security notice: user " + userName + " attempted restricted admin command " + text + "."
            );
        }

        const String logMethod = throttleAdminAlert
            ? ("Unauthorized " + text + " (admin alert throttled)")
            : ("Unauthorized " + text);
        webServer.logActivity("User (Telegram)", logMethod, adminAlertSent ? "alarm" : "fail");
        trackTelegramCommand("user", text, sent ? "unauthorized" : "send_fail", millis() - cmdStartMs);
        return;
    }

    const bool sent = sendTelegramText(chatId, "ℹ️ Use /help to see available commands.");
    webServer.logActivity("User (Telegram)", "Unsupported message", sent ? "success" : "fail");
    trackTelegramCommand("user", text, sent ? "unsupported" : "send_fail", millis() - cmdStartMs);
}

bool sendTelegramText(const String& chatId, const String& message) {
    if (!bot) {
        return false;
    }

    String normalizedChatId = chatId;
    normalizedChatId.trim();

    String safeMessage = message;
    safeMessage.trim();
    if (safeMessage.length() > 900) {
        safeMessage = safeMessage.substring(0, 897) + "...";
    }

    if (normalizedChatId.length() == 0 || safeMessage.length() == 0) {
        return false;
    }

    return bot->sendMessage(normalizedChatId, safeMessage, "");
}

void sendDuressAlert(String userName) {
    if (!notifyAdmins("🆘 DURESS ALERT: code used by user [" + userName + "]")) {
        webServer.logActivity("System", "Duress Alert Send Failed", "fail");
    }
}

void sendTheftAlert() {
    if (!notifyAdmins("🚨 SECURITY ALERT: theft attempt detected")) {
        webServer.logActivity("System", "Theft Alert Send Failed", "fail");
    }
}

bool sendOTP(String chatID, String otp) {
    if (!bot || chatID.length() == 0) {
        return false;
    }

    return sendTelegramText(chatID, "🔑 SecureLock OTP: " + otp + " (valid for 30 seconds).");
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

        notifyAdmins(
            "✅ Registered RFID detected: " + label + " [" + cardUid + "]\n2FA flow started."
        );
        return;
    }

    notifyAdmins(
        "🚨 Unregistered RFID detected: [" + cardUid + "]\nAccess denied."
    );
}

bool enqueueAdminNotification(const String& message) {
    String payload = message;
    payload.trim();

    if (payload.length() == 0) {
        return false;
    }

    bool queued = false;
    portENTER_CRITICAL(&telegramWebActivityMux);
    if (telegramWebActivityCount < TELEGRAM_WEB_ACTIVITY_QUEUE_MAX) {
        telegramWebActivityQueue[telegramWebActivityTail] = payload;
        telegramWebActivityTail = (telegramWebActivityTail + 1) % TELEGRAM_WEB_ACTIVITY_QUEUE_MAX;
        telegramWebActivityCount++;
        queued = true;
    }
    portEXIT_CRITICAL(&telegramWebActivityMux);

    if (!queued) {
        webServer.logActivity("System", "Telegram Notification Queue Full", "fail");
    }

    return queued;
}

void queueAccessEventForAdmins(const String& actor, const String& method) {
    String actorLabel = actor;
    actorLabel.trim();
    if (actorLabel.length() == 0) {
        actorLabel = "Unknown";
    }

    String methodLabel = method;
    methodLabel.trim();
    if (methodLabel.length() == 0) {
        methodLabel = "Unknown Method";
    }

    const unsigned long autoLockMs = lockManager.getAutoLockDelayMs();
    const unsigned long autoLockSec = (autoLockMs + 999) / 1000;

    const String message =
        "🔓 Access Granted\n"
        "• User: " + actorLabel + "\n"
        "• Method: " + methodLabel + "\n"
        "• Auto-lock: " + String(autoLockSec) + "s";

    enqueueAdminNotification(message);
}

void forwardWebAdminActivityToTelegram(const String& user, const String& method, const String& status) {
    (void)user;

    String normalizedMethod = method;
    normalizedMethod.trim();
    if (normalizedMethod.length() == 0) {
        normalizedMethod = "Unknown Action";
    }

    String normalizedStatus = status;
    normalizedStatus.trim();
    normalizedStatus.toLowerCase();

    String statusLabel = "ℹ️ INFO";
    if (normalizedStatus == "success") {
        statusLabel = "✅ SUCCESS";
    } else if (normalizedStatus == "fail") {
        statusLabel = "❌ FAILED";
    } else if (normalizedStatus == "alarm") {
        statusLabel = "🚨 ALERT";
    }

    const String message =
        "🖥️ Web Admin Activity\n"
        "• Action: " + normalizedMethod + "\n"
        "• Result: " + statusLabel;

    if (message.length() == 0) {
        return;
    }

    enqueueAdminNotification(message);
}

void processQueuedWebAdminActivityTelegram() {
    if (!bot || !webServer.isConnected() || WiFi.status() != WL_CONNECTED) {
        return;
    }

    const unsigned long now = millis();
    if (telegramLastWebActivityForwardMs > 0
        && (now - telegramLastWebActivityForwardMs) < TELEGRAM_WEB_ACTIVITY_MIN_INTERVAL_MS) {
        return;
    }

    String message = "";
    portENTER_CRITICAL(&telegramWebActivityMux);
    if (telegramWebActivityCount > 0) {
        message = telegramWebActivityQueue[telegramWebActivityHead];
        telegramWebActivityQueue[telegramWebActivityHead] = "";
        telegramWebActivityHead = (telegramWebActivityHead + 1) % TELEGRAM_WEB_ACTIVITY_QUEUE_MAX;
        telegramWebActivityCount--;
    }
    portEXIT_CRITICAL(&telegramWebActivityMux);

    if (message.length() == 0) {
        return;
    }

    if (notifyAdmins(message)) {
        telegramLastWebActivityForwardMs = now;
    }
}

void checkGuestCodeExpiry() {
    if (!guestCodeActive) {
        return;
    }

    if ((millis() - guestCodeIssuedAtMs) >= GUEST_CODE_TTL_MS) {
        guestCodeActive = false;
        guestCode = "";
        guestPinFailureCount = 0;
        lastGuestPinFailureMs = 0;

        if (keypadState == STATE_IDLE) {
            keypadBuffer = "";
            keypadIdleBufferLastInputMs = 0;
            authHandler.clearBuffer();
        }
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

unsigned long getGuestCodeCommandCooldownRemainingMs() {
    if (guestCodeIssuedAtMs == 0) {
        return 0;
    }

    const unsigned long elapsed = millis() - guestCodeIssuedAtMs;
    if (elapsed >= GUEST_CODE_COMMAND_COOLDOWN_MS) {
        return 0;
    }

    return GUEST_CODE_COMMAND_COOLDOWN_MS - elapsed;
}

String getAuthPrompt() {
    return authPrompt;
}

bool isPendingAccessActive() {
    return keypadState == STATE_AWAITING_2FA || keypadState == STATE_BACKUP_ONLY;
}

String getLastKeypadKeyLabel() {
    const char lastKey = authHandler.getLastAcceptedKey();
    const unsigned long lastKeyMs = authHandler.getLastAcceptedKeyMs();
    if (!lastKey || lastKeyMs == 0) {
        return "";
    }

    if ((millis() - lastKeyMs) > KEYPAD_LAST_KEY_FRESH_MS) {
        return "";
    }

    return String(lastKey);
}

unsigned long getLastKeypadKeyMs() {
    const unsigned long lastKeyMs = authHandler.getLastAcceptedKeyMs();
    if (lastKeyMs == 0) {
        return 0;
    }

    if ((millis() - lastKeyMs) > KEYPAD_LAST_KEY_FRESH_MS) {
        return 0;
    }

    return lastKeyMs;
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
