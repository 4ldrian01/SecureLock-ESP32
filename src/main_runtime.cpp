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
static const unsigned long GUEST_CODE_SUCCESS_BUZZER_MS = 1000;

String pendingUID = "";
String pendingUserName = "";
String pendingUserChatId = "";
String pendingBackupPin = "";
String pendingOtp = "";
unsigned long pendingOtpIssuedAtMs = 0;
unsigned long pendingOtpLastSentAtMs = 0;
String keypadBuffer = "";
String authPrompt = "";

static const size_t OTP_LENGTH = 4;
static const size_t BACKUP_PIN_LENGTH = 4;
static const char* DURESS_CODE = "2580";
static const unsigned long OTP_TTL_MS = 30000;
static const unsigned long OTP_RESEND_COOLDOWN_MS = 3000;
static const unsigned long TELEGRAM_STARTUP_GRACE_MS = 12000;
static const unsigned long KEYPAD_LAST_KEY_FRESH_MS = 20000;

unsigned long lastTelegramPollMs = 0;
static const unsigned long TELEGRAM_POLL_FAST_MS = 1400;
static const unsigned long TELEGRAM_POLL_IDLE_MS = 3500;
static const unsigned long TELEGRAM_POLL_ERROR_MS = 9000;
static const unsigned long TELEGRAM_COMMAND_MIN_INTERVAL_MS = 1200;
static const unsigned long TELEGRAM_DANGEROUS_COMMAND_COOLDOWN_MS = 5000;
static const unsigned long TELEGRAM_UNKNOWN_NOTICE_COOLDOWN_MS = 15000;
static const unsigned long TELEGRAM_UNAUTHORIZED_ALERT_COOLDOWN_MS = 60000;
static const int TELEGRAM_TRACKED_CHATS_MAX = 24;
static const int TELEGRAM_MAX_PROCESS_PER_CYCLE = 6;
static const int TELEGRAM_SAFE_MESSAGE_SLOTS = 6;
static const int TELEGRAM_WEB_ACTIVITY_QUEUE_MAX = 32;
static const int TELEGRAM_DIRECT_MESSAGE_QUEUE_MAX = 16;
static const int TELEGRAM_ADMIN_METRICS_MAX = 16;
static const unsigned long TELEGRAM_WEB_ACTIVITY_MIN_INTERVAL_MS = 900;
static const unsigned long TELEGRAM_DIRECT_MESSAGE_MIN_INTERVAL_MS = 700;
static const unsigned long TELEGRAM_POLLING_MODE_RETRY_MS = 8000;
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
unsigned long telegramWebActivityQueuedAtMs[TELEGRAM_WEB_ACTIVITY_QUEUE_MAX];
volatile int telegramWebActivityHead = 0;
volatile int telegramWebActivityTail = 0;
volatile int telegramWebActivityCount = 0;
String telegramDirectMessageChatQueue[TELEGRAM_DIRECT_MESSAGE_QUEUE_MAX];
String telegramDirectMessageBodyQueue[TELEGRAM_DIRECT_MESSAGE_QUEUE_MAX];
unsigned long telegramDirectMessageQueuedAtMs[TELEGRAM_DIRECT_MESSAGE_QUEUE_MAX];
uint8_t telegramDirectMessageRetries[TELEGRAM_DIRECT_MESSAGE_QUEUE_MAX];
volatile int telegramDirectMessageHead = 0;
volatile int telegramDirectMessageTail = 0;
volatile int telegramDirectMessageCount = 0;
unsigned long telegramLastWebActivityForwardMs = 0;
unsigned long telegramLastDirectMessageForwardMs = 0;
unsigned long lastTelegramQueueFullLogMs = 0;
unsigned long lastTelegramDirectQueueFullLogMs = 0;
unsigned long telegramNotifyQueuedTotal = 0;
unsigned long telegramNotifyDeliveredTotal = 0;
unsigned long telegramNotifyDeliveryFailures = 0;
unsigned long telegramNotifyDroppedFullTotal = 0;
unsigned long telegramNotifyDroppedRetryTotal = 0;
unsigned long telegramDirectNotifyDroppedFullTotal = 0;
unsigned long telegramDirectNotifyDroppedRetryTotal = 0;
unsigned long telegramAdminSendAttempts[TELEGRAM_ADMIN_METRICS_MAX];
unsigned long telegramAdminSendSuccess[TELEGRAM_ADMIN_METRICS_MAX];
unsigned long telegramAdminSendFailures[TELEGRAM_ADMIN_METRICS_MAX];
unsigned long telegramAdminLastSuccessMs[TELEGRAM_ADMIN_METRICS_MAX];
unsigned long telegramAdminLastFailureMs[TELEGRAM_ADMIN_METRICS_MAX];
portMUX_TYPE telegramWebActivityMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE telegramDirectMessageMux = portMUX_INITIALIZER_UNLOCKED;
bool telegramPollingConfigured = false;
unsigned long telegramLastPollingModeAttemptMs = 0;
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
static const unsigned long RFID_ENROLLMENT_WINDOW_DEFAULT_MS = 25000;
static const unsigned long RFID_ENROLLMENT_WINDOW_MIN_MS = 3000;
static const unsigned long RFID_ENROLLMENT_WINDOW_MAX_MS = 60000;
static const unsigned long RFID_ENROLLMENT_HEARTBEAT_STALE_MS = 4500;
volatile unsigned long rfidEnrollmentWindowStartedMs = 0;
volatile unsigned long rfidEnrollmentWindowUntilMs = 0;
volatile unsigned long rfidEnrollmentLastHeartbeatMs = 0;
char rfidEnrollmentWindowSource[25] = "idle";
portMUX_TYPE rfidEnrollmentMux = portMUX_INITIALIZER_UNLOCKED;
unsigned long lastEnrollmentRfidHandledScanMs = 0;
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
bool tamperAlertSent = false;
unsigned long lastTheftAlertMs = 0;
static const unsigned long THEFT_WINDOW_MS = 10000;
static const int THEFT_STRIKE_THRESHOLD = 3;
static const unsigned long THEFT_ALERT_COOLDOWN_MS = 30000;
bool rebootRequested = false;
unsigned long rebootRequestedAtMs = 0;
static const unsigned long REBOOT_GRACE_MS = 180;

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
bool enqueueTelegramUserNotification(const String& chatId, const String& message);
void processQueuedUserNotificationTelegram();
bool configureTelegramPollingMode();
bool ensureTelegramPollingMode();
void sendDuressAlert(String userName);
void sendTheftAlert();
void sendTamperAlert();
bool sendOTP(String chatID, String otp, bool* queuedFallback = nullptr);
void sendRfidScanAlert(String uid, bool known, const String& userName);
String normalizeTelegramCommand(const String& rawText);
bool enqueueAdminNotification(const String& message);
void queueAccessEventForAdmins(const String& actor, const String& method);
const char* keypadStateToText(KeypadState state);
String normalizeDisplayNameForLogs(const String& rawName, const String& fallback);
String formatChatIdForLogs(const String& chatId);
String getSeededAdminOverrideNameByChatId(const String& chatId);
String getSeededAdminNameByChatId(const String& chatId);
bool resolveEnrolledUserIdentityByChatId(const String& chatId, String* matchedName, String* matchedUid = nullptr);
String getTelegramActorLabel(const String& chatId, TelegramRole role);
bool sendAdminEnrollmentReport(const String& chatId, const String& actorLabel);
int getTelegramChatSlot(const String& chatId, bool createIfMissing = true);
bool isDangerousAdminCommand(const String& command);
bool shouldThrottleTelegramCommand(const String& chatId, bool dangerous, unsigned long* retryAfterMs = nullptr);
bool shouldThrottleUnknownNotice(const String& chatId, unsigned long* retryAfterMs = nullptr);
bool shouldThrottleUnauthorizedAdminAlert(const String& chatId, unsigned long* retryAfterMs = nullptr);
String roleToText(TelegramRole role);
void beginRfidEnrollmentWindow(const String& source, unsigned long durationMs = RFID_ENROLLMENT_WINDOW_DEFAULT_MS);
void endRfidEnrollmentWindow(const String& source = "");
void touchRfidEnrollmentWindowHeartbeat();
bool isRfidEnrollmentWindowActive();
unsigned long getRfidEnrollmentWindowRemainingMs();
String getRfidEnrollmentWindowSource();
bool handleRfidScanDuringEnrollment(AuthResult rfidResult);
bool requestWebGuestCode(String* issuedCode, unsigned long* remainingMs, bool* reusedExisting, bool* blockedByCooldown = nullptr);

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
int getTelegramNotificationQueueDepth();
int getTelegramNotificationQueueCapacity();
unsigned long getTelegramNotificationQueueOldestAgeMs();
unsigned long getTelegramNotificationQueuedTotal();
unsigned long getTelegramNotificationDeliveredTotal();
unsigned long getTelegramNotificationDeliveryFailures();
unsigned long getTelegramNotificationDroppedFullTotal();
unsigned long getTelegramNotificationDroppedRetryTotal();
int getTelegramAdminMetricsSlots();
String getTelegramAdminChatIdAt(int index);
unsigned long getTelegramAdminSendAttemptsAt(int index);
unsigned long getTelegramAdminSendSuccessAt(int index);
unsigned long getTelegramAdminSendFailuresAt(int index);
unsigned long getTelegramAdminLastSuccessMsAt(int index);
unsigned long getTelegramAdminLastFailureMsAt(int index);

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

    // Polling can still work when deleteWebhook confirmation is not explicit,
    // so rely on Bot API reachability as readiness gate.
    return getMeOk;
}

bool ensureTelegramPollingMode() {
    if (!bot) {
        return false;
    }

    if (telegramPollingConfigured) {
        return true;
    }

    const unsigned long now = millis();
    if (telegramLastPollingModeAttemptMs > 0
        && (now - telegramLastPollingModeAttemptMs) < TELEGRAM_POLLING_MODE_RETRY_MS) {
        return false;
    }

    telegramLastPollingModeAttemptMs = now;
    telegramPollingConfigured = configureTelegramPollingMode();
    return telegramPollingConfigured;
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
    telegramClient.setTimeout(1200);
    bot = new UniversalTelegramBot(BOT_TOKEN, telegramClient);
    bot->longPoll = 0;
    telegramReadyAfterMs = millis() + TELEGRAM_STARTUP_GRACE_MS;
    telegramPollingConfigured = false;
    telegramLastPollingModeAttemptMs = 0;
    Serial.println("[TELEGRAM] Bot initialized");
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

void beginRfidEnrollmentWindow(const String& source, unsigned long durationMs) {
    String normalizedSource = source;
    normalizedSource.trim();
    if (normalizedSource.length() == 0) {
        normalizedSource = "dashboard";
    }

    if (normalizedSource.length() > 24) {
        normalizedSource = normalizedSource.substring(0, 24);
        normalizedSource.trim();
    }

    if (normalizedSource.length() == 0) {
        normalizedSource = "dashboard";
    }

    unsigned long windowMs = durationMs;
    if (windowMs < RFID_ENROLLMENT_WINDOW_MIN_MS) {
        windowMs = RFID_ENROLLMENT_WINDOW_DEFAULT_MS;
    }
    if (windowMs > RFID_ENROLLMENT_WINDOW_MAX_MS) {
        windowMs = RFID_ENROLLMENT_WINDOW_MAX_MS;
    }

    const unsigned long now = millis();
    const unsigned long untilMs = now + windowMs;

    portENTER_CRITICAL(&rfidEnrollmentMux);
    rfidEnrollmentWindowStartedMs = now;
    rfidEnrollmentWindowUntilMs = untilMs;
    rfidEnrollmentLastHeartbeatMs = now;
    normalizedSource.toCharArray(rfidEnrollmentWindowSource, sizeof(rfidEnrollmentWindowSource));
    portEXIT_CRITICAL(&rfidEnrollmentMux);

    Serial.print("[RFID][ENROLL] Window started source=");
    Serial.print(normalizedSource);
    Serial.print(" ttlMs=");
    Serial.println(windowMs);
}

void touchRfidEnrollmentWindowHeartbeat() {
    const unsigned long now = millis();

    portENTER_CRITICAL(&rfidEnrollmentMux);
    if (rfidEnrollmentWindowUntilMs != 0) {
        rfidEnrollmentLastHeartbeatMs = now;
    }
    portEXIT_CRITICAL(&rfidEnrollmentMux);
}

void endRfidEnrollmentWindow(const String& source) {
    String normalizedSource = source;
    normalizedSource.trim();
    if (normalizedSource.length() == 0) {
        normalizedSource = "dashboard";
    }

    const bool wasActive = isRfidEnrollmentWindowActive();

    portENTER_CRITICAL(&rfidEnrollmentMux);
    rfidEnrollmentWindowStartedMs = 0;
    rfidEnrollmentWindowUntilMs = 0;
    rfidEnrollmentLastHeartbeatMs = 0;
    snprintf(rfidEnrollmentWindowSource, sizeof(rfidEnrollmentWindowSource), "%s", "idle");
    portEXIT_CRITICAL(&rfidEnrollmentMux);

    if (wasActive) {
        Serial.print("[RFID][ENROLL] Window stopped source=");
        Serial.println(normalizedSource);
    }
}

bool isRfidEnrollmentWindowActive() {
    unsigned long untilMs = 0;
    portENTER_CRITICAL(&rfidEnrollmentMux);
    untilMs = rfidEnrollmentWindowUntilMs;
    portEXIT_CRITICAL(&rfidEnrollmentMux);

    if (untilMs == 0) {
        return false;
    }

    const unsigned long now = millis();
    const long remainingMs = static_cast<long>(untilMs - now);
    if (remainingMs > 0) {
        return true;
    }

    portENTER_CRITICAL(&rfidEnrollmentMux);
    if (rfidEnrollmentWindowUntilMs != 0) {
        rfidEnrollmentWindowStartedMs = 0;
        rfidEnrollmentWindowUntilMs = 0;
        rfidEnrollmentLastHeartbeatMs = 0;
        snprintf(rfidEnrollmentWindowSource, sizeof(rfidEnrollmentWindowSource), "%s", "idle");
    }
    portEXIT_CRITICAL(&rfidEnrollmentMux);

    return false;
}

unsigned long getRfidEnrollmentWindowRemainingMs() {
    unsigned long untilMs = 0;
    portENTER_CRITICAL(&rfidEnrollmentMux);
    untilMs = rfidEnrollmentWindowUntilMs;
    portEXIT_CRITICAL(&rfidEnrollmentMux);

    if (untilMs == 0) {
        return 0;
    }

    const unsigned long now = millis();
    const long remainingMs = static_cast<long>(untilMs - now);
    if (remainingMs <= 0) {
        return 0;
    }

    return static_cast<unsigned long>(remainingMs);
}

String getRfidEnrollmentWindowSource() {
    char sourceBuffer[25] = {0};

    portENTER_CRITICAL(&rfidEnrollmentMux);
    snprintf(sourceBuffer, sizeof(sourceBuffer), "%s", rfidEnrollmentWindowSource);
    portEXIT_CRITICAL(&rfidEnrollmentMux);

    String source = sourceBuffer;
    source.trim();
    if (source.length() == 0) {
        return "idle";
    }

    return source;
}

bool handleRfidScanDuringEnrollment(AuthResult rfidResult) {
    if (rfidResult != AUTH_SUCCESS && rfidResult != AUTH_DENIED) {
        return false;
    }

    if (!isRfidEnrollmentWindowActive()) {
        return false;
    }

    const unsigned long nowMs = millis();
    unsigned long heartbeatMs = 0;
    portENTER_CRITICAL(&rfidEnrollmentMux);
    heartbeatMs = rfidEnrollmentLastHeartbeatMs;
    portEXIT_CRITICAL(&rfidEnrollmentMux);

    if (heartbeatMs > 0 && (nowMs - heartbeatMs) > RFID_ENROLLMENT_HEARTBEAT_STALE_MS) {
        Serial.println("[RFID][ENROLL] Window released (stale dashboard heartbeat)");
        endRfidEnrollmentWindow("stale-heartbeat");
        return false;
    }

    authHandler.startRFIDCooldown();

    const String uid = authHandler.getLastRFIDUID();
    const unsigned long scanMs = authHandler.getLastRFIDScanMs();
    const bool known = (uid.length() > 0) ? authHandler.userExists(uid) : false;
    const String source = getRfidEnrollmentWindowSource();

    if (scanMs > 0 && scanMs != lastEnrollmentRfidHandledScanMs) {
        lastEnrollmentRfidHandledScanMs = scanMs;

        const String actor = known
            ? authHandler.getUserName(uid)
            : "RFID Enrollment";

        const String method = known
            ? ("Enrollment Scan (Registered Card) [" + uid + "]")
            : ("Enrollment Scan (Unregistered Card) [" + uid + "]");

        webServer.logActivity(actor, method, "info");

        if (known) {
            enqueueAdminNotification(
                "ℹ️ RFID enrollment mode: registered card scanned [" + uid + "] from " + source + "."
            );
        }
    }

    return true;
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

    if (rebootRequested && (millis() - rebootRequestedAtMs) >= REBOOT_GRACE_MS) {
        ESP.restart();
        return;
    }

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

    const bool doorTampered = lockManager.isDoorTampered();
    if (doorTampered && !tamperAlertSent) {
        if (!securityManager.isAlarming()) {
            securityManager.startAlarm();
        }
        if (!isLockdown) {
            isLockdown = true;
            webServer.logActivity("System", "Lockdown Enabled (Tamper Alarm)", "alarm");
        }
        webServer.logActivity("System", "Door Tamper Detected", "alarm");
        sendTamperAlert();
        tamperAlertSent = true;
    } else if (!doorTampered) {
        tamperAlertSent = false;
    }

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
            if (theftStrikeCount >= THEFT_STRIKE_THRESHOLD && !theftAlertSent) {
                if (!securityManager.isAlarming()) {
                    securityManager.startAlarm();
                }

                if (!isLockdown) {
                    isLockdown = true;
                    webServer.logActivity("System", "Lockdown Enabled (Theft Alarm)", "alarm");
                }

                if (lastTheftAlertMs == 0 || (now - lastTheftAlertMs) >= THEFT_ALERT_COOLDOWN_MS) {
                    webServer.logActivity("System", "Theft Attempt Detected", "alarm");
                    sendTheftAlert();
                    lastTheftAlertMs = now;
                }

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

    if (isLockdown && !securityManager.isAlarming() && !lockManager.isLocked()) {
        isLockdown = false;
        webServer.logActivity("System", "Lockdown Cleared (Admin Override)", "success");
    }

    if (!isLockdown && keypadState != STATE_BACKUP_ONLY) {
        AuthResult rfidResult = authHandler.checkRFID();
        if (handleRfidScanDuringEnrollment(rfidResult)) {
            // Enrollment mode intentionally suppresses standard registered/unregistered
            // RFID workflows while preserving scan visibility for dashboard enrollment.
        } else if (rfidResult == AUTH_SUCCESS) {
            const String uid = authHandler.getLastRFIDUID();
            const String userName = authHandler.getUserName(uid);
            const unsigned long scanMs = authHandler.getLastRFIDScanMs();
            const unsigned long nowMs = millis();
            bool shouldEnter2FA = true;

            if (keypadState == STATE_AWAITING_2FA) {
                if (uid == pendingUID) {
                    authHandler.startRFIDCooldown();

                    if (!awaitingOfflineBackupMode) {
                        String otpTargetChat = pendingUserChatId;
                        otpTargetChat.trim();

                        const bool canAttemptResend = otpTargetChat.length() > 0
                            && ((nowMs - pendingOtpLastSentAtMs) >= OTP_RESEND_COOLDOWN_MS);

                        if (canAttemptResend) {
                            const String nextOtp = generateNumericCode(OTP_LENGTH);
                            bool otpQueuedFallback = false;
                            const bool otpDispatched = sendOTP(otpTargetChat, nextOtp, &otpQueuedFallback);

                            if (otpDispatched) {
                                pendingOtp = nextOtp;
                                pendingOtpIssuedAtMs = nowMs;
                                pendingOtpLastSentAtMs = nowMs;

                                if (otpQueuedFallback) {
                                    webServer.logActivity(pendingUserName, "OTP Re-Queued (RFID Re-scan)", "info");
                                    authPrompt = "Enter 4-digit OTP (delivery retry in progress, auto-submit at 4 digits, or press B for Offline Mode)";
                                } else {
                                    webServer.logActivity(pendingUserName, "OTP Resent (RFID Re-scan)", "success");
                                    authPrompt = "Enter 4-digit OTP (auto-submit at 4 digits, or press B for Offline Mode)";
                                }
                            } else {
                                awaitingOfflineBackupMode = true;
                                pendingOtp = "";
                                pendingOtpIssuedAtMs = 0;
                                pendingOtpLastSentAtMs = 0;
                                authPrompt = "Offline Mode: Enter Backup PIN";
                                webServer.logActivity(pendingUserName, "OTP Re-send Failed - Backup PIN", "fail");
                                enqueueAdminNotification(
                                    "⚠️ OTP re-send failed for " + pendingUserName + ". Switched to Backup PIN mode."
                                );
                            }
                        }
                    }

                    shouldEnter2FA = false;
                }

                if (shouldEnter2FA) {
                    authHandler.startRFIDCooldown();
                    if ((nowMs - lastPending2FAConflictFeedbackMs) >= PENDING_2FA_CONFLICT_FEEDBACK_MS) {
                        securityManager.beep(1);
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

                securityManager.beep(3);
                String unknownCardLabel = "Unregistered RFID";
                if (uid.length() > 0) {
                    String uidSuffix = uid;
                    if (uidSuffix.length() > 8) {
                        uidSuffix = uidSuffix.substring(uidSuffix.length() - 8);
                    }
                    unknownCardLabel += " " + uidSuffix;
                }
                webServer.logActivity(unknownCardLabel, "RFID Access Denied", "fail");

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
            securityManager.beep(3);
            if (pendingUserName.length() > 0) {
                enqueueAdminNotification(
                    "⚠️ OTP timed out for " + pendingUserName + ". 2FA session expired."
                );
            }
            clearPending2FA("OTP timeout", true);
        }
    }

    if (keypadState == STATE_BACKUP_ONLY && backupOnlyModeStartedAtMs > 0) {
        if ((millis() - backupOnlyModeStartedAtMs) >= BACKUP_ONLY_MODE_TIMEOUT_MS) {
            securityManager.beep(3);
            clearBackupOnlyMode("Backup PIN timeout", false);
        }
    }

    if (isLockdown) {
        // Keep keypad telemetry responsive for diagnostics/echo even in lockdown mode,
        // while intentionally skipping all local auth actions.
        const char lockdownKey = authHandler.getKeypadKey();
        if (lockdownKey != '\0') {
            securityManager.beepKeyPress();
        }

        if (keypadState == STATE_AWAITING_2FA) {
            clearPending2FA("Lockdown activated", false);
        } else if (keypadState == STATE_BACKUP_ONLY) {
            clearBackupOnlyMode("Lockdown activated", false);
        }
        keypadBuffer = "";
        authHandler.clearBuffer();
        handleTelegramCommands();
        processQueuedUserNotificationTelegram();
        processQueuedWebAdminActivityTelegram();
        yield();
        return;
    }

    processKeypad();
    handleTelegramCommands();
    processQueuedUserNotificationTelegram();
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

    securityManager.beepKeyPress();

    if (keypadState == STATE_IDLE && keypadBuffer.length() > 0) {
        const unsigned long nowMs = millis();
        const unsigned long idleBufferTimeoutMs = guestCodeActive
            ? GUEST_CODE_TTL_MS
            : KEYPAD_IDLE_BUFFER_TIMEOUT_MS;
        const bool noRecentIdleInput = (keypadIdleBufferLastInputMs == 0);
        const bool idleBufferExpired = !noRecentIdleInput
            && ((nowMs - keypadIdleBufferLastInputMs) >= idleBufferTimeoutMs);

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
            Serial.println("[KEYPAD] 2FA cancelled by user");
        } else if (keypadState == STATE_BACKUP_ONLY) {
            clearBackupOnlyMode("Backup mode cancelled", false);
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
            securityManager.beepModeChange();
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
            securityManager.beepModeChange();
            Serial.println("[KEYPAD] Backup-only mode started via B key");
        }
        return;
    }

    if (key == '#') {
        if (keypadState == STATE_IDLE) {
            if (guestCodeActive) {
                if (keypadBuffer.length() == 0) {
                    authPrompt = "Guest PIN active: Enter 4 digits";
                    Serial.println("[KEYPAD] # ignored: awaiting guest PIN digits");
                    return;
                }

                if (keypadBuffer.length() < BACKUP_PIN_LENGTH) {
                    authPrompt = "Guest PIN active: Enter exactly 4 digits";
                    Serial.println("[KEYPAD] # ignored: incomplete guest PIN");
                    return;
                }

                if (keypadBuffer == guestCode) {
                    grantAccess("Guest", "Guest PIN");
                    guestCode = "";
                    guestCodeActive = false;
                    guestPinFailureCount = 0;
                    authPrompt = "";
                    Serial.println("[KEYPAD] Guest PIN accepted via # submit");
                } else {
                    securityManager.beep(3);
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
            securityManager.beepModeChange();
            Serial.println("[KEYPAD] Backup-only mode started via #");
        } else if (keypadState == STATE_AWAITING_2FA) {
            if (!awaitingOfflineBackupMode) {
                if (keypadBuffer.length() == 0) {
                    awaitingOfflineBackupMode = true;
                    keypadBuffer = "";
                    keypadIdleBufferLastInputMs = 0;
                    authHandler.clearBuffer();
                    authPrompt = "Offline Mode: Enter Backup PIN";
                    webServer.logActivity(pendingUserName, "Offline Backup Mode", "success");
                    Serial.println("[KEYPAD] Switched to offline backup mode via # (2FA)");
                } else if (keypadBuffer.length() == OTP_LENGTH) {
                    evaluateAwaiting2FABuffer(true);
                } else {
                    authPrompt = "Enter 4-digit OTP (auto-submit at 4 digits, or press B for Offline Mode)";
                    Serial.println("[KEYPAD] # ignored: waiting for complete 4-digit OTP");
                }
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

        securityManager.beep(3);
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
        if (!awaitingOfflineBackupMode && keypadBuffer.length() == OTP_LENGTH) {
            authPrompt = "Verifying OTP...";
            evaluateAwaiting2FABuffer(true);
            return;
        }
        evaluateAwaiting2FABuffer(false);
    }
}

bool evaluateAwaiting2FABuffer(bool explicitSubmit) {
    const size_t len = keypadBuffer.length();

    if (len == BACKUP_PIN_LENGTH) {
        if (keypadBuffer == String(DURESS_CODE)) {
            lockManager.unlock();
            authHandler.startRFIDCooldown();
            securityManager.beepAccepted();
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
            securityManager.beep(3);
            clearPending2FA("Backup PIN failed", true);
            Serial.println("[AUTH] RFID + Backup PIN failed");
            return true;
        }
    }

    if (!awaitingOfflineBackupMode && len == OTP_LENGTH) {
        Serial.print("[AUTH][OTP] submit entered=");
        Serial.print(keypadBuffer);
        Serial.print(" expected=");
        Serial.println(pendingOtp);

        if (pendingOtp.length() == OTP_LENGTH && keypadBuffer == pendingOtp) {
            grantAccess(pendingUserName, "RFID + Telegram OTP", pendingUserChatId);
            Serial.println("[AUTH] RFID + Telegram OTP accepted");
        } else {
            securityManager.beep(3);
            if (pendingUserName.length() > 0) {
                enqueueAdminNotification(
                    "⚠️ OTP verification failed for " + pendingUserName + "."
                );
            }
            clearPending2FA("OTP failed", true);
            Serial.println("[AUTH] RFID + Telegram OTP failed");
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
        securityManager.beep(3);
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
    pendingOtpLastSentAtMs = 0;

    keypadBuffer = "";
    keypadIdleBufferLastInputMs = 0;
    authHandler.clearBuffer();
    keypadState = STATE_AWAITING_2FA;
    awaitingOfflineBackupMode = false;
    backupOnlyModeStartedAtMs = 0;

    if (!bot) {
        initializeTelegramBotIfNeeded();
    }

    if (bot) {
        pendingOtp = generateNumericCode(OTP_LENGTH);
        pendingOtpIssuedAtMs = millis();
        pendingOtpLastSentAtMs = pendingOtpIssuedAtMs;

        String otpTargetChat = pendingUserChatId;
        otpTargetChat.trim();
        if (otpTargetChat.length() == 0) {
            awaitingOfflineBackupMode = true;
            pendingOtp = "";
            pendingOtpIssuedAtMs = 0;
            pendingOtpLastSentAtMs = 0;
            authPrompt = "Offline Mode: Enter Backup PIN";
            securityManager.beepModeChange();
            webServer.logActivity(pendingUserName, "OTP Unavailable - Missing Chat ID", "fail");
            enqueueAdminNotification(
                "⚠️ OTP delivery blocked for " + pendingUserName + " (missing Telegram Chat ID)."
            );
            return;
        }

        bool otpQueuedFallback = false;
        const bool otpDispatched = sendOTP(otpTargetChat, pendingOtp, &otpQueuedFallback);
        if (!otpDispatched) {
            awaitingOfflineBackupMode = true;
            pendingOtp = "";
            pendingOtpIssuedAtMs = 0;
            pendingOtpLastSentAtMs = 0;
            authPrompt = "Offline Mode: Enter Backup PIN";
            securityManager.beepModeChange();
            webServer.logActivity(pendingUserName, "OTP Send Failed - Backup PIN", "fail");
            enqueueAdminNotification(
                "⚠️ OTP delivery failed for " + pendingUserName + ". Switched to Backup PIN mode."
            );
            return;
        }

        if (otpQueuedFallback) {
            webServer.logActivity(pendingUserName, "OTP Queued (Retry Delivery)", "info");
            enqueueAdminNotification(
                "ℹ️ OTP direct send retried via queue for " + pendingUserName + "."
            );
            authPrompt = "Enter 4-digit OTP (delivery retry in progress, auto-submit at 4 digits, or press B for Offline Mode)";
        } else {
            webServer.logActivity(pendingUserName, "OTP Sent", "success");
            authPrompt = "Enter 4-digit OTP (auto-submit at 4 digits, or press B for Offline Mode)";
        }
    } else {
        awaitingOfflineBackupMode = true;
        authPrompt = "Offline Mode: Enter Backup PIN";
        webServer.logActivity(pendingUserName, "OTP Unavailable - Telegram Offline", "fail");
        enqueueAdminNotification(
            "⚠️ OTP unavailable for " + pendingUserName + " (Telegram bot offline)."
        );
    }
}

void grantAccess(const String& actor, const String& method, const String& userChatId) {
    lockManager.unlock();
    authHandler.startRFIDCooldown();

    if (securityManager.isAlarming()) {
        securityManager.clearAlarm();
    }

    securityManager.beepAccepted();
    isLockdown = false;
    pendingDoorSecuredLog = true;
    guestPinFailureCount = 0;
    backupPinFailureCount = 0;
    lastGuestPinFailureMs = 0;
    lastBackupPinFailureMs = 0;
    keypadIdleBufferLastInputMs = 0;

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
    pendingOtpLastSentAtMs = 0;
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

bool requestWebGuestCode(String* issuedCode, unsigned long* remainingMs, bool* reusedExisting, bool* blockedByCooldown) {
    if (issuedCode) {
        *issuedCode = "";
    }
    if (remainingMs) {
        *remainingMs = 0;
    }
    if (reusedExisting) {
        *reusedExisting = false;
    }
    if (blockedByCooldown) {
        *blockedByCooldown = false;
    }

    auto prepareGuestEntryWindow = []() {
        if (keypadState == STATE_AWAITING_2FA) {
            clearPending2FA("Guest PIN window started", false);
        } else if (keypadState == STATE_BACKUP_ONLY) {
            clearBackupOnlyMode("Guest PIN window started", false);
        }

        keypadBuffer = "";
        keypadIdleBufferLastInputMs = 0;
        authHandler.clearBuffer();
        authPrompt = "Guest PIN active: Enter 4 digits";
    };

    const unsigned long cooldownRemainingMs = getGuestCodeCommandCooldownRemainingMs();
    if (!guestCodeActive && cooldownRemainingMs > 0) {
        if (remainingMs) {
            *remainingMs = cooldownRemainingMs;
        }
        if (blockedByCooldown) {
            *blockedByCooldown = true;
        }
        return false;
    }

    if (guestCodeActive) {
        prepareGuestEntryWindow();

        if (issuedCode) {
            *issuedCode = guestCode;
        }
        if (remainingMs) {
            *remainingMs = getTemporaryGuestCodeRemainingMs();
        }
        if (reusedExisting) {
            *reusedExisting = true;
        }
        return guestCode.length() == BACKUP_PIN_LENGTH;
    }

    guestCode = generateNumericCode(BACKUP_PIN_LENGTH);
    guestCodeActive = true;
    guestCodeIssuedAtMs = millis();
    guestPinFailureCount = 0;
    lastGuestPinFailureMs = 0;

    prepareGuestEntryWindow();

    securityManager.buzzFor(GUEST_CODE_SUCCESS_BUZZER_MS);

    if (issuedCode) {
        *issuedCode = guestCode;
    }
    if (remainingMs) {
        *remainingMs = getTemporaryGuestCodeRemainingMs();
    }

    return guestCode.length() == BACKUP_PIN_LENGTH;
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

    if (!telegramPollingConfigured && !ensureTelegramPollingMode()) {
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

    const int safeLimit = (TELEGRAM_MAX_PROCESS_PER_CYCLE < TELEGRAM_SAFE_MESSAGE_SLOTS)
        ? TELEGRAM_MAX_PROCESS_PER_CYCLE
        : TELEGRAM_SAFE_MESSAGE_SLOTS;

    int nextUpdateOffset = (lastHandledTelegramUpdateId > 0)
        ? (lastHandledTelegramUpdateId + 1)
        : (bot->last_message_received + 1);

    int totalFetchedInCycle = 0;
    bool pendingApproxSet = false;

    while (processedInCycle < safeLimit) {
        const int numNewMessages = bot->getUpdates(nextUpdateOffset);
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

        if (numNewMessages == 0) {
            break;
        }

        totalFetchedInCycle += numNewMessages;

        const int remainingBudget = safeLimit - processedInCycle;
        const int processLimit = (numNewMessages < remainingBudget)
            ? numNewMessages
            : remainingBudget;

        for (int i = 0; i < processLimit; i++) {
            const int updateId = bot->messages[i].update_id;
            consumedUpdatesInCycle++;

            if (updateId <= lastHandledTelegramUpdateId) {
                continue;
            }

            lastHandledTelegramUpdateId = updateId;
            nextUpdateOffset = updateId + 1;

            String chatId = bot->messages[i].chat_id;
            String text = normalizeTelegramCommand(bot->messages[i].text);
            chatId.trim();

            const TelegramRole role = resolveTelegramRole(chatId);
            if (role == ROLE_UNKNOWN) {
                handleUnknownTelegramChat(chatId);
                continue;
            }

            if (text.length() == 0) {
                continue;
            }

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

        if (numNewMessages > processLimit) {
            telegramPendingApprox = (numNewMessages - processLimit);
            pendingApproxSet = true;
            break;
        }
    }

    const unsigned long pollDurationMs = millis() - pollStartMs;
    if (pollDurationMs > 2500) {
        telegramPollIntervalMs = TELEGRAM_POLL_ERROR_MS;
    }

    if (!pendingApproxSet) {
        telegramPendingApprox = totalFetchedInCycle - consumedUpdatesInCycle;
        if (telegramPendingApprox < 0) {
            telegramPendingApprox = 0;
        }
    }

    telegramLastPollDurationMs = millis() - pollStartMs;
    if (processedInCycle > 0) {
        telegramCommandsHandled += static_cast<unsigned long>(processedInCycle);
        telegramLastSuccessMs = millis();
        telegramPollIntervalMs = TELEGRAM_POLL_FAST_MS;
    } else if (totalFetchedInCycle > 0) {
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

String normalizeDisplayNameForLogs(const String& rawName, const String& fallback) {
    String value = rawName;
    value.trim();

    while (value.indexOf("  ") >= 0) {
        value.replace("  ", " ");
    }

    if (value.length() == 0 || value.equalsIgnoreCase("unknown")) {
        String fb = fallback;
        fb.trim();
        return fb.length() > 0 ? fb : "Unknown";
    }

    return value;
}

String formatChatIdForLogs(const String& chatId) {
    String normalizedChatId = chatId;
    normalizedChatId.trim();

    if (normalizedChatId.length() == 0) {
        return "#unknown";
    }

    if (normalizedChatId.length() <= 4) {
        return "#" + normalizedChatId;
    }

    return "#" + normalizedChatId.substring(normalizedChatId.length() - 4);
}

String getSeededAdminOverrideNameByChatId(const String& chatId) {
    String normalizedChatId = chatId;
    normalizedChatId.trim();

    if (normalizedChatId.length() == 0 || !LittleFS.exists("/users.json")) {
        return "";
    }

    JsonDocument usersDoc;
    File file = LittleFS.open("/users.json", "r");
    if (!file) {
        return "";
    }

    const DeserializationError readErr = deserializeJson(usersDoc, file);
    file.close();
    if (readErr || !usersDoc.is<JsonObject>()) {
        return "";
    }

    JsonObject settings = usersDoc["settings"].as<JsonObject>();
    if (settings.isNull() || !settings["seededAdminProfiles"].is<JsonArray>()) {
        return "";
    }

    JsonArray profiles = settings["seededAdminProfiles"].as<JsonArray>();
    for (JsonObject profile : profiles) {
        String listedChatId = profile["chatId"] | "";
        listedChatId.trim();
        if (listedChatId != normalizedChatId) {
            continue;
        }

        String overrideName = profile["name"] | "";
        overrideName.trim();
        while (overrideName.indexOf("  ") >= 0) {
            overrideName.replace("  ", " ");
        }

        if (overrideName.length() > 0) {
            return overrideName;
        }

        return "";
    }

    return "";
}

String getSeededAdminNameByChatId(const String& chatId) {
    String normalizedChatId = chatId;
    normalizedChatId.trim();

    if (normalizedChatId.length() == 0) {
        return "Admin";
    }

    for (int i = 0; i < NUM_ADMINS; i++) {
        String listedAdminId = ADMIN_CHAT_IDS[i];
        listedAdminId.trim();
        if (listedAdminId.length() == 0) {
            continue;
        }

        if (listedAdminId == normalizedChatId) {
            const String overrideName = getSeededAdminOverrideNameByChatId(normalizedChatId);
            if (overrideName.length() > 0) {
                return overrideName;
            }

            if (i == 0) {
                return "Alsamhel Admin";
            }

            return "Admin " + String(i + 1);
        }
    }

    return "Admin";
}

bool resolveEnrolledUserIdentityByChatId(const String& chatId, String* matchedName, String* matchedUid) {
    if (matchedName) {
        *matchedName = "";
    }
    if (matchedUid) {
        *matchedUid = "";
    }

    String normalizedChatId = chatId;
    normalizedChatId.trim();
    if (normalizedChatId.length() == 0) {
        return false;
    }

    const int userCount = authHandler.getUserCount();
    for (int i = 0; i < userCount; i++) {
        const String uid = authHandler.getUserUIDAt(i);
        if (uid.length() == 0) {
            continue;
        }

        String listedChatId = authHandler.getUserTelegramChatId(uid);
        listedChatId.trim();
        if (listedChatId != normalizedChatId) {
            continue;
        }

        const String fallbackName = "User " + formatChatIdForLogs(normalizedChatId);
        const String resolvedName = normalizeDisplayNameForLogs(authHandler.getUserName(uid), fallbackName);

        if (matchedName) {
            *matchedName = resolvedName;
        }
        if (matchedUid) {
            *matchedUid = uid;
        }
        return true;
    }

    return false;
}

String getTelegramActorLabel(const String& chatId, TelegramRole role) {
    String normalizedChatId = chatId;
    normalizedChatId.trim();

    String enrolledName = "";
    const bool enrolled = resolveEnrolledUserIdentityByChatId(normalizedChatId, &enrolledName, nullptr);

    if (role == ROLE_ADMIN) {
        String adminName = enrolled
            ? enrolledName
            : normalizeDisplayNameForLogs(getSeededAdminNameByChatId(normalizedChatId), "Admin");
        return adminName + " (Telegram Admin)";
    }

    if (role == ROLE_USER) {
        if (enrolled) {
            return enrolledName + " (Telegram User)";
        }

        return "Unregistered Chat " + formatChatIdForLogs(normalizedChatId) + " (Telegram)";
    }

    if (enrolled) {
        return enrolledName + " (Telegram)";
    }

    return "Unregistered Chat " + formatChatIdForLogs(normalizedChatId) + " (Telegram)";
}

String compactTelegramLabel(const String& rawLabel, size_t maxLen) {
    String normalized = rawLabel;
    normalized.trim();

    while (normalized.indexOf("  ") >= 0) {
        normalized.replace("  ", " ");
    }

    if (normalized.length() <= static_cast<int>(maxLen)) {
        return normalized;
    }

    if (maxLen <= 3) {
        return normalized.substring(0, maxLen);
    }

    return normalized.substring(0, maxLen - 3) + "...";
}

bool sendAdminEnrollmentReport(const String& chatId, const String& actorLabel) {
    static const int MAX_PROFILE_LINES = 48;
    static const int USERS_PER_MESSAGE = 8;

    String adminLines[TELEGRAM_ADMIN_METRICS_MAX];
    String adminChatCache[TELEGRAM_ADMIN_METRICS_MAX];
    int adminLineCount = 0;

    for (int i = 0; i < NUM_ADMINS && adminLineCount < TELEGRAM_ADMIN_METRICS_MAX; i++) {
        String adminChatId = ADMIN_CHAT_IDS[i];
        adminChatId.trim();
        if (adminChatId.length() == 0) {
            continue;
        }

        bool duplicate = false;
        for (int j = 0; j < adminLineCount; j++) {
            if (adminChatCache[j] == adminChatId) {
                duplicate = true;
                break;
            }
        }

        if (duplicate) {
            continue;
        }

        adminChatCache[adminLineCount] = adminChatId;

        String enrolledName = "";
        String enrolledUid = "";
        const bool hasEnrolledProfile = resolveEnrolledUserIdentityByChatId(adminChatId, &enrolledName, &enrolledUid);

        String displayName = normalizeDisplayNameForLogs(
            getSeededAdminNameByChatId(adminChatId),
            "Admin " + String(i + 1)
        );

        if ((displayName.length() == 0 || displayName.equalsIgnoreCase("admin")) && hasEnrolledProfile) {
            displayName = enrolledName;
        }

        displayName = compactTelegramLabel(displayName, 24);

        String line = String(adminLineCount + 1) + ") " + displayName
            + " | " + formatChatIdForLogs(adminChatId);

        if (hasEnrolledProfile && enrolledUid.length() > 0) {
            line += " | UID " + enrolledUid;
        } else {
            line += " | seeded";
        }

        adminLines[adminLineCount] = line;
        adminLineCount++;
    }

    String userLines[MAX_PROFILE_LINES];
    int userLineCount = 0;

    const int authUserCount = authHandler.getUserCount();
    for (int i = 0; i < authUserCount && userLineCount < MAX_PROFILE_LINES; i++) {
        String uid = authHandler.getUserUIDAt(i);
        uid.trim();

        if (uid.length() == 0 || uid.startsWith("GUEST_")) {
            continue;
        }

        String linkedChatId = authHandler.getUserTelegramChatId(uid);
        linkedChatId.trim();

        if (isAdmin(linkedChatId)) {
            // Listed in admin section; avoid duplicate role rows.
            continue;
        }

        String displayName = normalizeDisplayNameForLogs(authHandler.getUserName(uid), "User");
        displayName = compactTelegramLabel(displayName, 24);

        const String chatLabel = linkedChatId.length() > 0
            ? formatChatIdForLogs(linkedChatId)
            : String("no-chat");

        userLines[userLineCount] = String(userLineCount + 1) + ") " + displayName
            + " | UID " + uid + " | " + chatLabel;
        userLineCount++;
    }

    const int totalProfiles = adminLineCount + userLineCount;

    bool sentAll = true;
    const String summary =
        "👥 Enrolled Accounts Report\n"
        "Requested by: " + actorLabel + "\n"
        "• Total profiles: " + String(totalProfiles) + "\n"
        "• Admin profiles: " + String(adminLineCount) + "\n"
        "• User profiles: " + String(userLineCount) + "\n"
        "• Auth store users: " + String(authUserCount);
    sentAll = sendTelegramText(chatId, summary) && sentAll;

    String adminsMessage = "🛡️ Admin Profiles\n";
    if (adminLineCount == 0) {
        adminsMessage += "• No admin profiles configured";
    } else {
        for (int i = 0; i < adminLineCount; i++) {
            adminsMessage += adminLines[i] + "\n";
        }
    }
    sentAll = sendTelegramText(chatId, adminsMessage) && sentAll;

    if (userLineCount == 0) {
        sentAll = sendTelegramText(chatId, "👤 Enrolled Users\n• No non-admin user profiles enrolled") && sentAll;
        return sentAll;
    }

    int batchIndex = 0;
    while ((batchIndex * USERS_PER_MESSAGE) < userLineCount) {
        const int start = batchIndex * USERS_PER_MESSAGE;
        const int endExclusive = min(start + USERS_PER_MESSAGE, userLineCount);

        String usersMessage = "👤 Enrolled Users ("
            + String(start + 1) + "-" + String(endExclusive)
            + " of " + String(userLineCount) + ")\n";

        for (int i = start; i < endExclusive; i++) {
            usersMessage += userLines[i] + "\n";
        }

        sentAll = sendTelegramText(chatId, usersMessage) && sentAll;
        batchIndex++;
    }

    return sentAll;
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
    bool queuedForRetry = false;
    const int trackedSlots = (NUM_ADMINS < TELEGRAM_ADMIN_METRICS_MAX)
        ? NUM_ADMINS
        : TELEGRAM_ADMIN_METRICS_MAX;
    const unsigned long now = millis();

    for (int i = 0; i < NUM_ADMINS; i++) {
        String adminChatId = ADMIN_CHAT_IDS[i];
        adminChatId.trim();
        if (adminChatId.length() == 0) {
            continue;
        }

        const bool sent = sendTelegramText(adminChatId, message);
        if (i < trackedSlots) {
            telegramAdminSendAttempts[i]++;
            if (sent) {
                telegramAdminSendSuccess[i]++;
                telegramAdminLastSuccessMs[i] = now;
            } else {
                telegramAdminSendFailures[i]++;
                telegramAdminLastFailureMs[i] = now;
            }
        }

        if (sent) {
            deliveredToAtLeastOne = true;
        } else if (enqueueTelegramUserNotification(adminChatId, message)) {
            queuedForRetry = true;
        }
    }

    return deliveredToAtLeastOne || queuedForRetry;
}

String getUserNameByTelegramChatId(const String& chatId) {
    String matchedName = "";
    if (resolveEnrolledUserIdentityByChatId(chatId, &matchedName, nullptr)) {
        return matchedName;
    }

    return "Unregistered Chat " + formatChatIdForLogs(chatId);
}

void handleUnknownTelegramChat(const String& chatId) {
    const unsigned long cmdStartMs = millis();
    const String actorLabel = getTelegramActorLabel(chatId, ROLE_UNKNOWN);

    unsigned long retryAfterMs = 0;
    if (shouldThrottleUnknownNotice(chatId, &retryAfterMs)) {
        webServer.logActivity(actorLabel, "Unregistered chat throttled", "fail");
        trackTelegramCommand("unknown", "unregistered", "unregistered_throttled", millis() - cmdStartMs);
        return;
    }

    const String message = "🚫 Access not registered.\n"
        "Please send this Chat ID to your SecureLock admin for approval:\n"
        "🆔 " + chatId;
    const bool sent = sendTelegramText(chatId, message);
    webServer.logActivity(actorLabel, "Unregistered chat attempted access", sent ? "alarm" : "fail");
    trackTelegramCommand("unknown", "unregistered", sent ? "unregistered_notified" : "send_fail", millis() - cmdStartMs);
}

void handleAdminCommand(const String& chatId, const String& text) {
    const unsigned long cmdStartMs = millis();
    const String actorLabel = getTelegramActorLabel(chatId, ROLE_ADMIN);

    if (text == "/help") {
        const bool sent = sendTelegramText(
            chatId,
            "🛡️ SecureLock Admin Command Guide\n"
            "Identity: " + actorLabel + "\n\n"
            "Core control:\n"
            "• /status - live lock, alarm, WiFi, telemetry\n"
            "• /user - list enrolled admin and user profiles\n"
            "• /admin_open - emergency unlock (cooldown + guest-safe checks)\n"
            "• /guest_code - generate/reuse 30-second guest PIN\n"
            "• /lockdown - disable local RFID/keypad auth\n"
            "• /unlockdown - re-enable local auth\n"
            "• /reboot - controlled device restart\n\n"
            "Keypad quick controls:\n"
            "• B - instant Backup PIN mode\n"
            "• # - submit OTP/PIN (or start backup mode from idle)\n\n"
            "Diagnostics:\n"
            "• /my_info - admin identity and device link\n"
            "• /buzzer_test - buzzer diagnostic tone\n"
            "• /keypad_echo - last keypad key + age\n"
            "• /start - session heartbeat\n"
            "• /help - this command guide"
        );
        webServer.logActivity(actorLabel, "Help Command", sent ? "success" : "fail");
        trackTelegramCommand("admin", text, sent ? "ok" : "send_fail", millis() - cmdStartMs);
        return;
    }

    if (text == "/start") {
        const bool sent = sendTelegramText(
            chatId,
            "🛡️ Admin session verified. SecureLock is online and ready.\nUse /help to view all commands."
        );
        webServer.logActivity(actorLabel, "Start Command", sent ? "success" : "fail");
        trackTelegramCommand("admin", text, sent ? "ok" : "send_fail", millis() - cmdStartMs);
        return;
    }

    if (text == "/my_info") {
        const bool adminVerified = isAdmin(chatId);
        const bool online = webServer.isConnected() && (WiFi.status() == WL_CONNECTED);
        const String ipAddress = online ? WiFi.localIP().toString() : String("Unavailable");
        const String infoMessage =
            "👤 Admin Account Summary\n"
            "• Display Name: " + actorLabel + "\n"
            "• Role: Administrator\n"
            "• Chat ID: " + chatId + "\n"
            "• Admin Verified: " + String(adminVerified ? "Yes" : "No") + "\n"
            "• Command Access: Full\n"
            "• Device Link: " + String(online ? "Online" : "Offline") + "\n"
            "• Device IP: " + ipAddress + "\n"
            "Use /help to view all admin commands.";
        const bool sent = sendTelegramText(chatId, infoMessage);
        webServer.logActivity(actorLabel, "My Info Command", sent ? "success" : "fail");
        trackTelegramCommand("admin", text, sent ? "ok" : "send_fail", millis() - cmdStartMs);
        return;
    }

    if (text == "/user" || text == "/users") {
        const bool sent = sendAdminEnrollmentReport(chatId, actorLabel);
        webServer.logActivity(actorLabel, "Enrolled Accounts Report", sent ? "success" : "fail");
        trackTelegramCommand("admin", text, sent ? "ok" : "send_fail", millis() - cmdStartMs);
        return;
    }

    if (text == "/open") {
        const bool sent = sendTelegramText(chatId, "ℹ️ This command is deprecated. Please use /admin_open.");
        webServer.logActivity(actorLabel, "Deprecated Command /open", "fail");
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
            webServer.logActivity(actorLabel, "Emergency Override Blocked (Guest Code Active)", "fail");
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
            webServer.logActivity(actorLabel, "Emergency Override Cooldown", "fail");
            trackTelegramCommand("admin", text, sent ? "cooldown" : "send_fail", millis() - cmdStartMs);
            return;
        }

        lockManager.unlock();
        authHandler.startRFIDCooldown();

        if (securityManager.isAlarming()) {
            securityManager.clearAlarm();
        }

        securityManager.beepAccepted();
        isLockdown = false;
        pendingDoorSecuredLog = true;
        webServer.markEmergencyOverride();
        webServer.logActivity(actorLabel, "Emergency Override", "success");
        queueAccessEventForAdmins(actorLabel, "Emergency Override");
        clearPending2FA("Admin override", false);
        const bool sent = sendTelegramText(chatId, "✅ Emergency unlock executed. Auto-lock timer is active.");
        trackTelegramCommand("admin", text, sent ? "ok" : "send_fail", millis() - cmdStartMs);
        return;
    }

    if (text == "/guest_code") {
        String issuedCode = "";
        unsigned long remainingMs = 0;
        bool reusedExisting = false;
        bool blockedByCooldown = false;
        const bool generated = requestWebGuestCode(&issuedCode, &remainingMs, &reusedExisting, &blockedByCooldown);

        if (!generated && blockedByCooldown) {
            const unsigned long retrySec = (remainingMs + 999) / 1000;
            const bool sent = sendTelegramText(
                chatId,
                "⏳ Guest PIN generation is cooling down. Retry in " + String(retrySec) + "s."
            );
            webServer.logActivity(actorLabel, "Guest PIN Cooldown", "fail");
            trackTelegramCommand("admin", text, sent ? "cooldown" : "send_fail", millis() - cmdStartMs);
            return;
        }

        if (!generated || issuedCode.length() != BACKUP_PIN_LENGTH) {
            const bool sent = sendTelegramText(chatId, "❌ Unable to generate guest PIN right now. Please retry.");
            webServer.logActivity(actorLabel, "Guest PIN Generation", "fail");
            trackTelegramCommand("admin", text, sent ? "guest_generation_failed" : "send_fail", millis() - cmdStartMs);
            return;
        }

        const unsigned long remainingSec = (remainingMs + 999) / 1000;
        const String message = reusedExisting
            ? ("ℹ️ Guest PIN is already active: " + issuedCode + " (expires in " + String(remainingSec) + "s).")
            : ("🔐 Guest PIN: " + issuedCode + " (valid for 30 seconds). Share only with authorized visitors.");

        const bool sent = sendTelegramText(chatId, message);
        webServer.logActivity(actorLabel, reusedExisting ? "Guest PIN Reused" : "Guest PIN Generated", sent ? "success" : "fail");
        trackTelegramCommand("admin", text, sent ? (reusedExisting ? "existing_active" : "ok") : "send_fail", millis() - cmdStartMs);
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
        webServer.logActivity(actorLabel, "Status Check", sent ? "success" : "fail");
        trackTelegramCommand("admin", text, sent ? "ok" : "send_fail", millis() - cmdStartMs);
        return;
    }

    if (text == "/buzzer_test") {
        securityManager.beepAccepted();
        const bool sent = sendTelegramText(chatId, "🔔 Buzzer diagnostic executed (short double tone).");
        webServer.logActivity(actorLabel, "Buzzer Test", sent ? "success" : "fail");
        trackTelegramCommand("admin", text, sent ? "ok" : "send_fail", millis() - cmdStartMs);
        return;
    }

    if (text == "/keypad_echo") {
        const unsigned long keyMs = getLastKeypadKeyMs();
        const String keyLabel = getLastKeypadKeyLabel();
        if (keyLabel.length() == 0 || keyMs == 0) {
            const bool sent = sendTelegramText(chatId, "⌨️ No keypad key recorded yet.");
            webServer.logActivity(actorLabel, "Keypad Echo", sent ? "success" : "fail");
            trackTelegramCommand("admin", text, sent ? "ok" : "send_fail", millis() - cmdStartMs);
            return;
        }

        const unsigned long ageMs = millis() - keyMs;
        const bool sent = sendTelegramText(chatId, "⌨️ Last keypad key: " + keyLabel + " (" + String(ageMs) + " ms ago)");
        webServer.logActivity(actorLabel, "Keypad Echo", sent ? "success" : "fail");
        trackTelegramCommand("admin", text, sent ? "ok" : "send_fail", millis() - cmdStartMs);
        return;
    }

    if (text == "/lockdown") {
        isLockdown = true;
        clearPending2FA("Lockdown activated by admin", false);
        const bool sent = sendTelegramText(chatId, "🛡️ Lockdown enabled. Local RFID/keypad authentication is now disabled.");
        webServer.logActivity(actorLabel, "Lockdown Enabled", sent ? "success" : "fail");
        trackTelegramCommand("admin", text, sent ? "ok" : "send_fail", millis() - cmdStartMs);
        return;
    }

    if (text == "/unlockdown") {
        isLockdown = false;
        const bool sent = sendTelegramText(chatId, "✅ Lockdown disabled. Local RFID/keypad authentication is enabled.");
        webServer.logActivity(actorLabel, "Lockdown Disabled", sent ? "success" : "fail");
        trackTelegramCommand("admin", text, sent ? "ok" : "send_fail", millis() - cmdStartMs);
        return;
    }

    if (text == "/reboot") {
        const bool sent = sendTelegramText(chatId, "♻️ System rebooting now...");
        webServer.logActivity(actorLabel, "Reboot Command", sent ? "success" : "fail");
        trackTelegramCommand("admin", text, sent ? "ok" : "send_fail", millis() - cmdStartMs);
        rebootRequested = true;
        rebootRequestedAtMs = millis();
        return;
    }

    const bool sent = sendTelegramText(chatId, "❓ Unknown admin command. Send /help to view the available command list.");
    webServer.logActivity(actorLabel, "Unknown Command", "fail");
    trackTelegramCommand("admin", text, sent ? "unknown" : "send_fail", millis() - cmdStartMs);
}

void handleUserCommand(const String& chatId, const String& text) {
    const unsigned long cmdStartMs = millis();
    const String actorLabel = getTelegramActorLabel(chatId, ROLE_USER);
    const String userName = getUserNameByTelegramChatId(chatId);

    if (text == "/help") {
        const bool sent = sendTelegramText(
            chatId,
            "📘 SecureLock User Command Guide\n"
            "Identity: " + userName + "\n\n"
            "Standard access flow:\n"
            "1) Tap your registered RFID card\n"
            "2) Wait for 4-digit OTP in this chat\n"
            "3) Enter OTP on keypad (auto-submits after 4 digits)\n\n"
            "Offline fallback:\n"
            "1) Tap RFID card\n"
            "2) Press 'B' for instant Backup PIN mode (or '#' if preferred)\n"
            "3) Enter your 4-digit Backup PIN (press # to submit, or wait for 4 digits)\n\n"
            "Keypad-only fallback:\n"
            "• From idle, press 'B' to start Backup Access\n\n"
            "Allowed commands:\n"
            "• /start - connection check\n"
            "• /my_info - your linked account summary\n"
            "• /help - this user command guide\n\n"
            "Admin-only commands are restricted and monitored."
        );
        webServer.logActivity(actorLabel, "Help Command", sent ? "success" : "fail");
        trackTelegramCommand("user", text, sent ? "ok" : "send_fail", millis() - cmdStartMs);
        return;
    }

    if (text == "/start") {
        const bool sent = sendTelegramText(
            chatId,
            "👋 Welcome, " + userName + ". Your Telegram account is linked for OTP delivery after RFID scan.\nUse /help to view available commands."
        );
        webServer.logActivity(actorLabel, "Start Command", sent ? "success" : "fail");
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
        webServer.logActivity(actorLabel, "My Info Command", sent ? "success" : "fail");
        trackTelegramCommand("user", text, sent ? "ok" : "send_fail", millis() - cmdStartMs);
        return;
    }

    if (text.startsWith("/")) {
        const bool throttleAdminAlert = shouldThrottleUnauthorizedAdminAlert(chatId, nullptr);

        const bool sent = sendTelegramText(
            chatId,
            "🚫 You are not authorized to use this command.\n"
            "Allowed commands for your role: /start, /my_info, /help.\n"
            "This attempt has been logged."
        );
        if (!throttleAdminAlert) {
            notifyAdmins(
                "🚨 Security notice: " + actorLabel + " attempted restricted admin command " + text + "."
            );
        }

        const String logMethod = throttleAdminAlert
            ? ("Unauthorized " + text + " (admin alert throttled)")
            : ("Unauthorized " + text);
        webServer.logActivity(actorLabel, logMethod, "alarm");
        trackTelegramCommand("user", text, sent ? "unauthorized" : "send_fail", millis() - cmdStartMs);
        return;
    }

    const bool sent = sendTelegramText(chatId, "ℹ️ Use /help to see available commands.");
    webServer.logActivity(actorLabel, "Unsupported message", sent ? "success" : "fail");
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

    if (bot->sendMessage(normalizedChatId, safeMessage, "")) {
        return true;
    }

    if (WiFi.status() != WL_CONNECTED) {
        return false;
    }

    // Lightweight retry for transient TLS/API hiccups.
    yield();
    return bot->sendMessage(normalizedChatId, safeMessage, "");
}

void sendDuressAlert(String userName) {
    if (!enqueueAdminNotification("🆘 DURESS ALERT: code used by user [" + userName + "]")) {
        webServer.logActivity("System", "Duress Alert Queue Failed", "fail");
    }
}

void sendTheftAlert() {
    if (!enqueueAdminNotification("🚨 SECURITY ALERT: theft attempt detected")) {
        webServer.logActivity("System", "Theft Alert Queue Failed", "fail");
    }
}

void sendTamperAlert() {
    if (!enqueueAdminNotification("🚨 TAMPER ALERT: Door opened while lock was engaged")) {
        webServer.logActivity("System", "Tamper Alert Queue Failed", "fail");
    }
}

bool sendOTP(String chatID, String otp, bool* queuedFallback) {
    if (queuedFallback) {
        *queuedFallback = false;
    }

    if (!bot || chatID.length() == 0 || otp.length() != OTP_LENGTH) {
        return false;
    }

    const String otpMessage = "🔑 SecureLock OTP: " + otp + " (valid for 30 seconds).";

    if (sendTelegramText(chatID, otpMessage)) {
        return true;
    }

    if (enqueueTelegramUserNotification(chatID, otpMessage)) {
        if (queuedFallback) {
            *queuedFallback = true;
        }
        return true;
    }

    return false;
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

        enqueueAdminNotification(
            "✅ Registered RFID detected: " + label + " [" + cardUid + "]\n2FA flow started."
        );
        return;
    }

    enqueueAdminNotification(
        "🚨 Unregistered RFID detected: [" + cardUid + "]\nAccess denied."
    );
}

bool enqueueTelegramUserNotification(const String& chatId, const String& message) {
    String normalizedChatId = chatId;
    normalizedChatId.trim();

    String normalizedMessage = message;
    normalizedMessage.trim();
    if (normalizedMessage.length() > 900) {
        normalizedMessage = normalizedMessage.substring(0, 897) + "...";
    }

    if (normalizedChatId.length() == 0 || normalizedMessage.length() == 0) {
        return false;
    }

    bool queued = false;
    portENTER_CRITICAL(&telegramDirectMessageMux);
    if (telegramDirectMessageCount < TELEGRAM_DIRECT_MESSAGE_QUEUE_MAX) {
        const int enqueueIndex = telegramDirectMessageTail;
        telegramDirectMessageChatQueue[enqueueIndex] = normalizedChatId;
        telegramDirectMessageBodyQueue[enqueueIndex] = normalizedMessage;
        telegramDirectMessageQueuedAtMs[enqueueIndex] = millis();
        telegramDirectMessageRetries[enqueueIndex] = 0;
        telegramDirectMessageTail = (telegramDirectMessageTail + 1) % TELEGRAM_DIRECT_MESSAGE_QUEUE_MAX;
        telegramDirectMessageCount++;
        queued = true;
    } else {
        telegramDirectNotifyDroppedFullTotal++;
    }
    portEXIT_CRITICAL(&telegramDirectMessageMux);

    if (!queued) {
        const unsigned long now = millis();
        if ((now - lastTelegramDirectQueueFullLogMs) >= 10000UL) {
            lastTelegramDirectQueueFullLogMs = now;
            webServer.logActivity("System", "Telegram User Notification Queue Full", "fail");
        }
    }

    return queued;
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
        const int enqueueIndex = telegramWebActivityTail;
        telegramWebActivityQueue[telegramWebActivityTail] = payload;
        telegramWebActivityQueuedAtMs[enqueueIndex] = millis();
        telegramWebActivityTail = (telegramWebActivityTail + 1) % TELEGRAM_WEB_ACTIVITY_QUEUE_MAX;
        telegramWebActivityCount++;
        telegramNotifyQueuedTotal++;
        queued = true;
    } else {
        telegramNotifyDroppedFullTotal++;
    }
    portEXIT_CRITICAL(&telegramWebActivityMux);

    if (!queued) {
        const unsigned long now = millis();
        if ((now - lastTelegramQueueFullLogMs) >= 10000UL) {
            lastTelegramQueueFullLogMs = now;
            webServer.logActivity("System", "Telegram Notification Queue Full", "fail");
        }
    }

    return queued;
}

void processQueuedUserNotificationTelegram() {
    if (!bot || !webServer.isConnected() || WiFi.status() != WL_CONNECTED) {
        return;
    }

    const unsigned long now = millis();
    if (telegramLastDirectMessageForwardMs > 0
        && (now - telegramLastDirectMessageForwardMs) < TELEGRAM_DIRECT_MESSAGE_MIN_INTERVAL_MS) {
        return;
    }

    String chatId = "";
    String message = "";
    uint8_t retries = 0;

    portENTER_CRITICAL(&telegramDirectMessageMux);
    if (telegramDirectMessageCount > 0) {
        chatId = telegramDirectMessageChatQueue[telegramDirectMessageHead];
        message = telegramDirectMessageBodyQueue[telegramDirectMessageHead];
        retries = telegramDirectMessageRetries[telegramDirectMessageHead];
    }
    portEXIT_CRITICAL(&telegramDirectMessageMux);

    if (chatId.length() == 0 || message.length() == 0) {
        portENTER_CRITICAL(&telegramDirectMessageMux);
        if (telegramDirectMessageCount > 0
            && (telegramDirectMessageChatQueue[telegramDirectMessageHead].length() == 0
                || telegramDirectMessageBodyQueue[telegramDirectMessageHead].length() == 0)) {
            telegramDirectMessageChatQueue[telegramDirectMessageHead] = "";
            telegramDirectMessageBodyQueue[telegramDirectMessageHead] = "";
            telegramDirectMessageQueuedAtMs[telegramDirectMessageHead] = 0;
            telegramDirectMessageRetries[telegramDirectMessageHead] = 0;
            telegramDirectMessageHead = (telegramDirectMessageHead + 1) % TELEGRAM_DIRECT_MESSAGE_QUEUE_MAX;
            telegramDirectMessageCount--;
        }
        portEXIT_CRITICAL(&telegramDirectMessageMux);
        return;
    }

    if (sendTelegramText(chatId, message)) {
        portENTER_CRITICAL(&telegramDirectMessageMux);
        if (telegramDirectMessageCount > 0) {
            telegramDirectMessageChatQueue[telegramDirectMessageHead] = "";
            telegramDirectMessageBodyQueue[telegramDirectMessageHead] = "";
            telegramDirectMessageQueuedAtMs[telegramDirectMessageHead] = 0;
            telegramDirectMessageRetries[telegramDirectMessageHead] = 0;
            telegramDirectMessageHead = (telegramDirectMessageHead + 1) % TELEGRAM_DIRECT_MESSAGE_QUEUE_MAX;
            telegramDirectMessageCount--;
        }
        portEXIT_CRITICAL(&telegramDirectMessageMux);

        telegramLastDirectMessageForwardMs = now;
        return;
    }

    const uint8_t nextRetry = (retries < 255) ? static_cast<uint8_t>(retries + 1) : retries;
    bool dropped = false;

    portENTER_CRITICAL(&telegramDirectMessageMux);
    if (telegramDirectMessageCount > 0) {
        telegramDirectMessageRetries[telegramDirectMessageHead] = nextRetry;

        if (nextRetry >= 5) {
            telegramDirectMessageChatQueue[telegramDirectMessageHead] = "";
            telegramDirectMessageBodyQueue[telegramDirectMessageHead] = "";
            telegramDirectMessageQueuedAtMs[telegramDirectMessageHead] = 0;
            telegramDirectMessageRetries[telegramDirectMessageHead] = 0;
            telegramDirectMessageHead = (telegramDirectMessageHead + 1) % TELEGRAM_DIRECT_MESSAGE_QUEUE_MAX;
            telegramDirectMessageCount--;
            telegramDirectNotifyDroppedRetryTotal++;
            dropped = true;
        }
    }
    portEXIT_CRITICAL(&telegramDirectMessageMux);

    telegramLastDirectMessageForwardMs = now;

    if (dropped) {
        webServer.logActivity("System", "Telegram User Notification Dropped After Retries", "fail");
    }
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
    String actorLabel = user;
    actorLabel.trim();
    if (actorLabel.length() == 0) {
        actorLabel = "Admin (Web)";
    }

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
        "• Actor: " + actorLabel + "\n"
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

    static uint8_t consecutiveSendFailures = 0;
    static String failedFingerprint = "";

    String message = "";
    portENTER_CRITICAL(&telegramWebActivityMux);
    if (telegramWebActivityCount > 0) {
        message = telegramWebActivityQueue[telegramWebActivityHead];
    }
    portEXIT_CRITICAL(&telegramWebActivityMux);

    if (message.length() == 0) {
        // Heal queue head if an empty payload was ever inserted by legacy code.
        portENTER_CRITICAL(&telegramWebActivityMux);
        if (telegramWebActivityCount > 0
            && telegramWebActivityQueue[telegramWebActivityHead].length() == 0) {
            telegramWebActivityHead = (telegramWebActivityHead + 1) % TELEGRAM_WEB_ACTIVITY_QUEUE_MAX;
            telegramWebActivityCount--;
        }
        portEXIT_CRITICAL(&telegramWebActivityMux);
        return;
    }

    if (notifyAdmins(message)) {
        portENTER_CRITICAL(&telegramWebActivityMux);
        if (telegramWebActivityCount > 0) {
            telegramWebActivityQueue[telegramWebActivityHead] = "";
            telegramWebActivityQueuedAtMs[telegramWebActivityHead] = 0;
            telegramWebActivityHead = (telegramWebActivityHead + 1) % TELEGRAM_WEB_ACTIVITY_QUEUE_MAX;
            telegramWebActivityCount--;
        }
        portEXIT_CRITICAL(&telegramWebActivityMux);

        telegramLastWebActivityForwardMs = now;
        telegramNotifyDeliveredTotal++;
        consecutiveSendFailures = 0;
        failedFingerprint = "";
        return;
    }

    telegramNotifyDeliveryFailures++;

    String fingerprint = message;
    if (fingerprint.length() > 48) {
        fingerprint = fingerprint.substring(0, 48);
    }

    if (fingerprint == failedFingerprint) {
        if (consecutiveSendFailures < 255) {
            consecutiveSendFailures++;
        }
    } else {
        failedFingerprint = fingerprint;
        consecutiveSendFailures = 1;
    }

    // Prevent a permanently undeliverable message from blocking newer alerts forever.
    if (consecutiveSendFailures >= 8) {
        portENTER_CRITICAL(&telegramWebActivityMux);
        if (telegramWebActivityCount > 0) {
            telegramWebActivityQueue[telegramWebActivityHead] = "";
            telegramWebActivityQueuedAtMs[telegramWebActivityHead] = 0;
            telegramWebActivityHead = (telegramWebActivityHead + 1) % TELEGRAM_WEB_ACTIVITY_QUEUE_MAX;
            telegramWebActivityCount--;
        }
        portEXIT_CRITICAL(&telegramWebActivityMux);

        telegramNotifyDroppedRetryTotal++;
        webServer.logActivity("System", "Telegram Notification Dropped After Retries", "fail");
        consecutiveSendFailures = 0;
        failedFingerprint = "";
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

        webServer.logActivity("System", "Guest PIN Expired", "info");
        enqueueAdminNotification("⌛ Guest PIN expired and is no longer valid.");

        if (keypadState == STATE_IDLE) {
            keypadBuffer = "";
            keypadIdleBufferLastInputMs = 0;
            authHandler.clearBuffer();

            if (authPrompt.startsWith("Guest PIN active")) {
                authPrompt = "";
            }
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

int getTelegramNotificationQueueDepth() {
    int depth = 0;
    portENTER_CRITICAL(&telegramWebActivityMux);
    depth = telegramWebActivityCount;
    portEXIT_CRITICAL(&telegramWebActivityMux);
    return depth;
}

int getTelegramNotificationQueueCapacity() {
    return TELEGRAM_WEB_ACTIVITY_QUEUE_MAX;
}

unsigned long getTelegramNotificationQueueOldestAgeMs() {
    unsigned long enqueuedAtMs = 0;
    portENTER_CRITICAL(&telegramWebActivityMux);
    if (telegramWebActivityCount > 0) {
        enqueuedAtMs = telegramWebActivityQueuedAtMs[telegramWebActivityHead];
    }
    portEXIT_CRITICAL(&telegramWebActivityMux);

    if (enqueuedAtMs == 0) {
        return 0;
    }

    return millis() - enqueuedAtMs;
}

unsigned long getTelegramNotificationQueuedTotal() {
    return telegramNotifyQueuedTotal;
}

unsigned long getTelegramNotificationDeliveredTotal() {
    return telegramNotifyDeliveredTotal;
}

unsigned long getTelegramNotificationDeliveryFailures() {
    return telegramNotifyDeliveryFailures;
}

unsigned long getTelegramNotificationDroppedFullTotal() {
    return telegramNotifyDroppedFullTotal;
}

unsigned long getTelegramNotificationDroppedRetryTotal() {
    return telegramNotifyDroppedRetryTotal;
}

int getTelegramAdminMetricsSlots() {
    return (NUM_ADMINS < TELEGRAM_ADMIN_METRICS_MAX)
        ? NUM_ADMINS
        : TELEGRAM_ADMIN_METRICS_MAX;
}

String getTelegramAdminChatIdAt(int index) {
    const int slots = getTelegramAdminMetricsSlots();
    if (index < 0 || index >= slots) {
        return "";
    }

    String chatId = ADMIN_CHAT_IDS[index];
    chatId.trim();
    return chatId;
}

unsigned long getTelegramAdminSendAttemptsAt(int index) {
    const int slots = getTelegramAdminMetricsSlots();
    if (index < 0 || index >= slots) {
        return 0;
    }
    return telegramAdminSendAttempts[index];
}

unsigned long getTelegramAdminSendSuccessAt(int index) {
    const int slots = getTelegramAdminMetricsSlots();
    if (index < 0 || index >= slots) {
        return 0;
    }
    return telegramAdminSendSuccess[index];
}

unsigned long getTelegramAdminSendFailuresAt(int index) {
    const int slots = getTelegramAdminMetricsSlots();
    if (index < 0 || index >= slots) {
        return 0;
    }
    return telegramAdminSendFailures[index];
}

unsigned long getTelegramAdminLastSuccessMsAt(int index) {
    const int slots = getTelegramAdminMetricsSlots();
    if (index < 0 || index >= slots) {
        return 0;
    }
    return telegramAdminLastSuccessMs[index];
}

unsigned long getTelegramAdminLastFailureMsAt(int index) {
    const int slots = getTelegramAdminMetricsSlots();
    if (index < 0 || index >= slots) {
        return 0;
    }
    return telegramAdminLastFailureMs[index];
}
