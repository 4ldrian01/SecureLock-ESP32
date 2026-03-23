/**
 * ============================================================
 * AuthHandler - Implementation
 * ============================================================
 */

#include "AuthHandler.h"
#include <SPI.h>
#include <LittleFS.h>

const char* AuthHandler::DURESS_CODE = "2580";

namespace {
const char* USERS_FILE = "/users.json";
const char* USERS_KEY = "users";
const char* USERS_CARD_UID_KEY = "cardUID";
const char* USERS_UID_FALLBACK_KEY = "uid";
const char* USERS_NAME_KEY = "name";
const char* USERS_PIN_KEY = "pin";
const char* USERS_BACKUP_PIN_KEY = "backupPIN";
const char* USERS_CHAT_ID_KEY = "telegramChatID";

bool isFourDigitCode(const String& value) {
    if (value.length() != 4) {
        return false;
    }

    for (size_t i = 0; i < value.length(); i++) {
        if (!isDigit(value.charAt(i))) {
            return false;
        }
    }

    return true;
}
}

AuthHandler::AuthHandler()
    : _rfid(PIN_RFID_SS, PIN_RFID_RST),
      _pinBuffer(""),
      _lastRFIDUID(""),
    _lastRFIDScanMs(0),
      _rfidCooldownStartMs(0),
      _rfidCooldownDurationMs(0),
            _lastRFIDRecoverAttemptMs(0),
      _lastAcceptedKeyMs(0),
      _keypadNoiseWindowStartMs(0),
      _keypadNoiseCount(0),
      _keypadMutedUntilMs(0),
      _keypadReadyAtMs(0),
      _keypadRuntimeSettlingStarted(false),
            _heldKey('\0'),
      _activeRfidRstPin(PIN_RFID_RST),
    _rfidReady(false),
      _factoryPressStart(0),
      _factoryPressed(false),
      _userCount(0)
{
}

void AuthHandler::init() {
    pinMode(PIN_RFID_SS, OUTPUT);
    digitalWrite(PIN_RFID_SS, HIGH);

    SPI.begin(
        SECURELOCK_PIN_SPI_SCK,
        SECURELOCK_PIN_SPI_MISO,
        SECURELOCK_PIN_SPI_MOSI,
        PIN_RFID_SS
    );

    _activeRfidRstPin = PIN_RFID_RST;

    auto initReader = [this](int rstPin) -> byte {
        pinMode(rstPin, OUTPUT);
        digitalWrite(rstPin, HIGH);
        _rfid.PCD_Init(PIN_RFID_SS, rstPin);
        _rfid.PCD_AntennaOn();
        _rfid.PCD_SetAntennaGain(MFRC522::RxGain_max);
        return _rfid.PCD_ReadRegister(_rfid.VersionReg);
    };

    auto isReservedOrConflictingPin = [this](int pin) -> bool {
        if (pin < 0) {
            return true;
        }

        if (pin == PIN_RFID_SS
            || pin == SECURELOCK_PIN_SPI_SCK
            || pin == SECURELOCK_PIN_SPI_MISO
            || pin == SECURELOCK_PIN_SPI_MOSI
            || pin == PIN_FACTORY) {
            return true;
        }

        for (byte i = 0; i < ROWS; i++) {
            if (pin == _rowPins[i]) {
                return true;
            }
        }

        for (byte i = 0; i < COLS; i++) {
            if (pin == _colPins[i]) {
                return true;
            }
        }

        return false;
    };

    byte version = initReader(PIN_RFID_RST);
    const bool canTryFallback = (PIN_RFID_RST_FALLBACK >= 0)
        && (PIN_RFID_RST_FALLBACK != PIN_RFID_RST)
        && !isReservedOrConflictingPin(PIN_RFID_RST_FALLBACK);

    if ((version == 0x00 || version == 0xFF) && canTryFallback) {
        Serial.println("[AUTH] RFID not detected on primary RST pin, trying fallback...");
        version = initReader(PIN_RFID_RST_FALLBACK);
        if (version != 0x00 && version != 0xFF) {
            _activeRfidRstPin = PIN_RFID_RST_FALLBACK;
        }
    } else if ((version == 0x00 || version == 0xFF)
        && (PIN_RFID_RST_FALLBACK >= 0)
        && (PIN_RFID_RST_FALLBACK != PIN_RFID_RST)
        && isReservedOrConflictingPin(PIN_RFID_RST_FALLBACK)) {
        Serial.print("[AUTH][WARN] RFID fallback RST pin conflicts with existing mapping: GPIO");
        Serial.println(PIN_RFID_RST_FALLBACK);
    }

    if (version == 0x00 || version == 0xFF) {
        _rfidReady = false;
        Serial.println("[AUTH] RFID reader not detected");
    } else {
        _rfidReady = true;
        Serial.print("[AUTH] RFID RC522 version 0x");
        Serial.println(version, HEX);
        Serial.print("[AUTH] RFID RST pin active: ");
        Serial.println(_activeRfidRstPin);
        Serial.println("[AUTH] RFID antenna gain set to MAX");
    }

    pinMode(PIN_FACTORY, INPUT_PULLUP);

    _keypadReadyAtMs = 0;
    _keypadRuntimeSettlingStarted = false;

    _loadUsersFromFS();

    Serial.print("[AUTH] Keypad rows: ");
    for (byte i = 0; i < ROWS; i++) {
        Serial.print(_rowPins[i]);
        if (i + 1 < ROWS) {
            Serial.print(',');
        }
    }
    Serial.println();

    Serial.print("[AUTH] Keypad cols: ");
    for (byte i = 0; i < COLS; i++) {
        Serial.print(_colPins[i]);
        if (i + 1 < COLS) {
            Serial.print(',');
        }
    }
    Serial.println();

    Serial.println("[AUTH] Keypad startup diagnostics:");
    for (byte i = 0; i < ROWS; i++) {
        // GPIO34-39 are input-only and do not support internal pull-up.
        pinMode(_rowPins[i], (_rowPins[i] >= 34) ? INPUT : INPUT_PULLUP);
        Serial.print("  - Row R");
        Serial.print(i + 1);
        Serial.print(" GPIO");
        Serial.print(_rowPins[i]);
        Serial.print(" idle=");
        Serial.println(digitalRead(_rowPins[i]));
    }

    for (byte i = 0; i < COLS; i++) {
        pinMode(_colPins[i], OUTPUT);
        digitalWrite(_colPins[i], HIGH);
        Serial.print("  - Col C");
        Serial.print(i + 1);
        Serial.print(" GPIO");
        Serial.print(_colPins[i]);
        Serial.println(" drive=HIGH OK");
    }

    Serial.println("[AUTH] Initialized");
    Serial.print("[AUTH] Users loaded: ");
    Serial.println(_userCount);
}

void AuthHandler::update() {
    checkFactoryReset();
}

AuthResult AuthHandler::checkRFID() {
    if (!_rfidReady && !_attemptRFIDRecovery()) {
        return AUTH_NONE;
    }

    if (!_rfid.PICC_IsNewCardPresent()) {
        return AUTH_NONE;
    }

    if (!_rfid.PICC_ReadCardSerial()) {
        return AUTH_NONE;
    }

    if (isRFIDCooldownActive()) {
        _rfid.PICC_HaltA();
        _rfid.PCD_StopCrypto1();
        return AUTH_NONE;
    }

    String uid = _normalizeUID(_uidToString(_rfid.uid.uidByte, _rfid.uid.size));
    _lastRFIDUID = uid;
    _lastRFIDScanMs = millis();

    _rfid.PICC_HaltA();
    _rfid.PCD_StopCrypto1();

    if (userExists(uid)) {
        Serial.print("[AUTH] RFID accepted: ");
        Serial.println(uid);
        return AUTH_SUCCESS;
    }

    Serial.print("[AUTH] RFID denied: ");
    Serial.println(uid);
    return AUTH_DENIED;
}

bool AuthHandler::_attemptRFIDRecovery() {
    const unsigned long now = millis();
    if (_rfidReady) {
        return true;
    }

    if (_lastRFIDRecoverAttemptMs > 0 && (now - _lastRFIDRecoverAttemptMs) < RFID_RECOVERY_INTERVAL_MS) {
        return false;
    }

    _lastRFIDRecoverAttemptMs = now;

    pinMode(_activeRfidRstPin, OUTPUT);
    digitalWrite(_activeRfidRstPin, HIGH);
    _rfid.PCD_Init(PIN_RFID_SS, _activeRfidRstPin);
    _rfid.PCD_AntennaOn();
    _rfid.PCD_SetAntennaGain(MFRC522::RxGain_max);

    const byte version = _rfid.PCD_ReadRegister(_rfid.VersionReg);
    if (version == 0x00 || version == 0xFF) {
        return false;
    }

    _rfidReady = true;
    Serial.print("[AUTH] RFID recovered on GPIO");
    Serial.println(_activeRfidRstPin);
    return true;
}

void AuthHandler::startRFIDCooldown(unsigned long cooldownMs) {
    _rfidCooldownStartMs = millis();
    _rfidCooldownDurationMs = cooldownMs;
}

bool AuthHandler::isRFIDCooldownActive() const {
    if (_rfidCooldownDurationMs == 0) {
        return false;
    }

    return (millis() - _rfidCooldownStartMs) < _rfidCooldownDurationMs;
}

String AuthHandler::getLastRFIDUID() const {
    return _lastRFIDUID;
}

unsigned long AuthHandler::getLastRFIDScanMs() const {
    return _lastRFIDScanMs;
}

bool AuthHandler::isRFIDReady() const {
    return _rfidReady;
}

int AuthHandler::getActiveRFIDRstPin() const {
    return _activeRfidRstPin;
}

bool AuthHandler::isKeypadReady() const {
    if (!_keypadRuntimeSettlingStarted) {
        return false;
    }

    return millis() >= _keypadReadyAtMs;
}

bool AuthHandler::isKeypadMuted() const {
    return _keypadMutedUntilMs > millis();
}

unsigned long AuthHandler::getKeypadMuteRemainingMs() const {
    const unsigned long now = millis();
    if (_keypadMutedUntilMs <= now) {
        return 0;
    }

    return _keypadMutedUntilMs - now;
}

char AuthHandler::getKeypadKey() {
    const unsigned long now = millis();

    if (!_keypadRuntimeSettlingStarted) {
        _keypadRuntimeSettlingStarted = true;
        _keypadReadyAtMs = now + KEYPAD_STARTUP_SETTLE_MS;
    }

    if (now < _keypadReadyAtMs) {
        _scanKeypadRaw();
        return '\0';
    }

    const char rawKey = _scanKeypadRaw();
    if (rawKey == '\0') {
        _heldKey = '\0';
        return '\0';
    }

    // Emit one key event per physical press; ignore repeats while held.
    if (_heldKey == rawKey) {
        return '\0';
    }
    _heldKey = rawKey;

    if (rawKey == '\0') {
        return '\0';
    }

    const bool supported = ((rawKey >= '0' && rawKey <= '9') || rawKey == '*' || rawKey == '#'
        || rawKey == 'A' || rawKey == 'B' || rawKey == 'C' || rawKey == 'D');
    if (!supported) {
        return '\0';
    }

    if (_keypadMutedUntilMs > now) {
        return '\0';
    }

    if (now - _lastAcceptedKeyMs < KEYPAD_MIN_KEY_INTERVAL_MS) {
        return '\0';
    }

    if (_keypadNoiseWindowStartMs == 0 || (now - _keypadNoiseWindowStartMs) > KEYPAD_NOISE_WINDOW_MS) {
        _keypadNoiseWindowStartMs = now;
        _keypadNoiseCount = 0;
    }

    _keypadNoiseCount++;
    if (_keypadNoiseCount > KEYPAD_NOISE_THRESHOLD) {
        _keypadMutedUntilMs = now + KEYPAD_MUTE_DURATION_MS;
        _keypadNoiseWindowStartMs = 0;
        _keypadNoiseCount = 0;
        clearBuffer();
        return '\0';
    }

    _lastAcceptedKeyMs = now;
    return rawKey;
}

char AuthHandler::_scanKeypadRaw() {
    // Drive one column LOW at a time and read rows.
    // On some ESP32 boards row pins may be input-only without internal pull-ups,
    // so we require a stable single-key detection across two scans.
    auto scanOnce = [this]() -> char {
        for (byte c = 0; c < COLS; c++) {
            digitalWrite(_colPins[c], HIGH);
        }

        char detected = '\0';
        int hits = 0;

        for (byte c = 0; c < COLS; c++) {
            digitalWrite(_colPins[c], LOW);
            delayMicroseconds(25);

            for (byte r = 0; r < ROWS; r++) {
                if (digitalRead(_rowPins[r]) == LOW) {
                    detected = _keys[r][c];
                    hits++;
                }
            }

            digitalWrite(_colPins[c], HIGH);
        }

        if (hits != 1) {
            return '\0';
        }

        return detected;
    };

    const char first = scanOnce();
    if (first == '\0') {
        return '\0';
    }

    delayMicroseconds(600);
    const char second = scanOnce();
    if (second == first) {
        return second;
    }

    return '\0';
}

void AuthHandler::appendToBuffer(char key) {
    if (_pinBuffer.length() < 8) {
        _pinBuffer += key;
    }
}

void AuthHandler::clearBuffer() {
    _pinBuffer = "";
}

String AuthHandler::getBuffer() const {
    return _pinBuffer;
}

AuthResult AuthHandler::validatePIN() {
    return validatePIN(_pinBuffer);
}

AuthResult AuthHandler::validatePIN(const String& pin) {
    if (pin.length() == 0) {
        return AUTH_NONE;
    }

    if (pin == DURESS_CODE) {
        return AUTH_DURESS;
    }

    if (_validateStoredPIN(pin)) {
        return AUTH_SUCCESS;
    }

    return AUTH_DENIED;
}

bool AuthHandler::addUser(const String& uid, const String& pin, const String& name) {
    if (!_ensureFileSystemReady()) {
        return false;
    }

    _loadUsersFromFS();

    String normalizedUid = _normalizeUID(uid);
    if (normalizedUid.length() == 0 || pin.length() == 0 || name.length() == 0) {
        return false;
    }

    if (!isFourDigitCode(pin)) {
        return false;
    }

    JsonObject existing = _findUserByUID(normalizedUid);
    if (!existing.isNull()) {
        existing[USERS_CARD_UID_KEY] = normalizedUid;
        existing[USERS_UID_FALLBACK_KEY] = normalizedUid;
        existing[USERS_NAME_KEY] = name;
        existing[USERS_PIN_KEY] = pin;
        if (!existing["type"].is<const char*>()) {
            existing["type"] = "user";
        }
    } else {
        JsonArray users = _usersArray();
        if (users.isNull() || _userCount >= MAX_USERS) {
            return false;
        }

        JsonObject u = users.add<JsonObject>();
        u[USERS_CARD_UID_KEY] = normalizedUid;
        u[USERS_UID_FALLBACK_KEY] = normalizedUid;
        u[USERS_NAME_KEY] = name;
        u[USERS_PIN_KEY] = pin;
        u["type"] = "user";
    }

    if (!_saveUsersToFS()) {
        return false;
    }

    _loadUsersFromFS();
    return true;
}

bool AuthHandler::removeUser(const String& uid) {
    if (!_ensureFileSystemReady()) {
        return false;
    }

    String normalizedUid = _normalizeUID(uid);
    JsonArray users = _usersArray();
    if (users.isNull()) {
        return false;
    }

    bool removed = false;
    for (size_t i = users.size(); i > 0; i--) {
        String listedUid = _extractUserUID(users[i - 1].as<JsonObjectConst>());
        if (listedUid == normalizedUid) {
            users.remove(i - 1);
            removed = true;
        }
    }

    if (!removed) {
        return false;
    }

    if (!_saveUsersToFS()) {
        return false;
    }

    _loadUsersFromFS();
    return true;
}

bool AuthHandler::userExists(const String& uid) {
    return !_findUserByUID(_normalizeUID(uid)).isNull();
}

String AuthHandler::getUserName(const String& uid) {
    JsonObject user = _findUserByUID(_normalizeUID(uid));
    if (user.isNull()) {
        return "Unknown";
    }

    return user[USERS_NAME_KEY] | "Unknown";
}

String AuthHandler::getUserPIN(const String& uid) {
    JsonObject user = _findUserByUID(_normalizeUID(uid));
    if (user.isNull()) {
        return "";
    }

    return user[USERS_PIN_KEY] | "";
}

bool AuthHandler::setUserTelegramChatId(const String& uid, const String& chatId) {
    JsonObject user = _findUserByUID(_normalizeUID(uid));
    if (user.isNull()) {
        return false;
    }

    String normalizedChat = chatId;
    normalizedChat.trim();

    if (normalizedChat.length() == 0) {
        user.remove(USERS_CHAT_ID_KEY);
    } else {
        user[USERS_CHAT_ID_KEY] = normalizedChat;
    }

    return _saveUsersToFS();
}

String AuthHandler::getUserTelegramChatId(const String& uid) {
    JsonObject user = _findUserByUID(_normalizeUID(uid));
    if (user.isNull()) {
        return "";
    }

    return user[USERS_CHAT_ID_KEY] | "";
}

bool AuthHandler::isKnownTelegramChatId(const String& chatId) {
    String normalizedChat = chatId;
    normalizedChat.trim();
    if (normalizedChat.length() == 0) {
        return false;
    }

    JsonArray users = _usersArray();
    if (users.isNull()) {
        return false;
    }

    for (JsonObject u : users) {
        String listedChat = u[USERS_CHAT_ID_KEY] | "";
        listedChat.trim();
        if (listedChat == normalizedChat) {
            return true;
        }
    }

    return false;
}

bool AuthHandler::setUserBackupPIN(const String& uid, const String& backupPin) {
    if (backupPin.length() != 4) {
        return false;
    }

    for (size_t i = 0; i < backupPin.length(); i++) {
        if (!isDigit(backupPin.charAt(i))) {
            return false;
        }
    }

    JsonObject user = _findUserByUID(_normalizeUID(uid));
    if (user.isNull()) {
        return false;
    }

    user[USERS_BACKUP_PIN_KEY] = backupPin;
    return _saveUsersToFS();
}

String AuthHandler::getUserBackupPIN(const String& uid) {
    JsonObject user = _findUserByUID(_normalizeUID(uid));
    if (user.isNull()) {
        return "";
    }

    String backup = user[USERS_BACKUP_PIN_KEY] | "";
    if (backup.length() == 4) {
        return backup;
    }

    return getUserPIN(uid);
}

int AuthHandler::getUserCount() const {
    return _userCount;
}

String AuthHandler::getUserUIDAt(int index) const {
    if (index < 0 || index >= _userCount) {
        return "";
    }

    return _userUIDs[index];
}

bool AuthHandler::checkFactoryReset() {
    const bool currentPressed = digitalRead(PIN_FACTORY) == LOW;
    const unsigned long now = millis();

    if (currentPressed && !_factoryPressed) {
        _factoryPressed = true;
        _factoryPressStart = now;
    } else if (currentPressed && _factoryPressed) {
        if ((now - _factoryPressStart) >= FACTORY_RESET_TIME) {
            performFactoryReset();
            _factoryPressed = false;
            return true;
        }
    } else if (!currentPressed && _factoryPressed) {
        _factoryPressed = false;
    }

    return false;
}

void AuthHandler::performFactoryReset() {
    _usersDoc.clear();
    _usersDoc[USERS_KEY] = JsonArray();
    _saveUsersToFS();
    _loadUsersFromFS();
}

bool AuthHandler::_validateStoredPIN(const String& pin) {
    JsonArray users = _usersArray();
    if (users.isNull()) {
        return false;
    }

    for (JsonObject u : users) {
        String storedPin = u[USERS_PIN_KEY] | "";
        if (storedPin == pin) {
            return true;
        }
    }

    return false;
}

String AuthHandler::_uidToString(byte* uid, byte size) {
    String result = "";
    for (byte i = 0; i < size; i++) {
        if (uid[i] < 0x10) {
            result += "0";
        }
        result += String(uid[i], HEX);
    }
    result.toUpperCase();
    return result;
}

String AuthHandler::_normalizeUID(const String& uid) const {
    String normalized = uid;
    normalized.trim();
    normalized.toUpperCase();
    normalized.replace(" ", "");
    normalized.replace(":", "");
    normalized.replace("-", "");
    return normalized;
}

String AuthHandler::_extractUserUID(JsonObjectConst user) const {
    if (user.isNull()) {
        return "";
    }

    String uid = _normalizeUID(user[USERS_CARD_UID_KEY] | "");
    if (uid.length() > 0) {
        return uid;
    }

    uid = _normalizeUID(user[USERS_UID_FALLBACK_KEY] | "");
    if (uid.length() > 0) {
        return uid;
    }

    // Backward compatibility with older schema variants used by legacy UI builds.
    uid = _normalizeUID(user["cardUid"] | "");
    if (uid.length() > 0) {
        return uid;
    }

    uid = _normalizeUID(user["rfid"] | "");
    if (uid.length() > 0) {
        return uid;
    }

    uid = _normalizeUID(user["rfidUID"] | "");
    return uid;
}

bool AuthHandler::_loadUsersFromFS() {
    if (!_ensureFileSystemReady()) {
        return false;
    }

    _usersDoc.clear();

    if (!LittleFS.exists(USERS_FILE)) {
        _usersDoc[USERS_KEY] = JsonArray();
        _saveUsersToFS();
    }

    File file = LittleFS.open(USERS_FILE, "r");
    if (!file) {
        return false;
    }

    const DeserializationError err = deserializeJson(_usersDoc, file);
    file.close();

    if (err || !_usersDoc.is<JsonObject>()) {
        _usersDoc.clear();
        _usersDoc[USERS_KEY] = JsonArray();
        _saveUsersToFS();
    }

    if (!_usersDoc[USERS_KEY].is<JsonArray>()) {
        _usersDoc[USERS_KEY] = JsonArray();
        _saveUsersToFS();
    }

    _compactUsers();

    return true;
}

bool AuthHandler::_saveUsersToFS() {
    if (!_ensureFileSystemReady()) {
        return false;
    }

    File file = LittleFS.open(USERS_FILE, "w");
    if (!file) {
        return false;
    }

    const size_t written = serializeJson(_usersDoc, file);
    file.close();

    return written > 0;
}

JsonArray AuthHandler::_usersArray() {
    if (!_usersDoc[USERS_KEY].is<JsonArray>()) {
        _usersDoc[USERS_KEY] = JsonArray();
    }

    return _usersDoc[USERS_KEY].as<JsonArray>();
}

JsonObject AuthHandler::_findUserByUID(const String& uid) {
    JsonArray users = _usersArray();
    for (JsonObject u : users) {
        JsonObjectConst userConst = u;
        String listed = _extractUserUID(userConst);
        if (listed == uid) {
            // Self-heal key aliases on first successful lookup.
            u[USERS_CARD_UID_KEY] = listed;
            u[USERS_UID_FALLBACK_KEY] = listed;
            return u;
        }
    }

    return JsonObject();
}

bool AuthHandler::_ensureFileSystemReady() {
    if (LittleFS.begin(false)) {
        return true;
    }

    Serial.println("[AUTH][WARN] LittleFS not ready for auth storage");
    return false;
}

bool AuthHandler::_compactUsers() {
    if (!_usersDoc[USERS_KEY].is<JsonArray>()) {
        _usersDoc[USERS_KEY] = JsonArray();
    }

    JsonArray users = _usersArray();
    JsonDocument compactDoc;
    JsonArray compactUsers = compactDoc[USERS_KEY].to<JsonArray>();

    int compactCount = 0;
    bool changed = false;

    for (JsonObject u : users) {
        if (compactCount >= MAX_USERS) {
            changed = true;
            continue;
        }

        JsonObjectConst userConst = u;
        const String uid = _extractUserUID(userConst);
        String pin = u[USERS_PIN_KEY] | "";
        String name = u[USERS_NAME_KEY] | "";
        String chat = u[USERS_CHAT_ID_KEY] | "";
        String backup = u[USERS_BACKUP_PIN_KEY] | "";

        pin.trim();
        name.trim();
        chat.trim();
        backup.trim();

        if (uid.length() == 0 || name.length() == 0 || !isFourDigitCode(pin)) {
            changed = true;
            continue;
        }

        bool duplicate = false;
        for (int i = 0; i < compactCount; i++) {
            if (_userUIDs[i] == uid) {
                duplicate = true;
                break;
            }
        }

        if (duplicate) {
            changed = true;
            continue;
        }

        JsonObject nu = compactUsers.add<JsonObject>();
        nu[USERS_CARD_UID_KEY] = uid;
        nu[USERS_UID_FALLBACK_KEY] = uid;
        nu[USERS_NAME_KEY] = name;
        nu[USERS_PIN_KEY] = pin;
        if (u["type"].is<const char*>()) {
            nu["type"] = u["type"].as<const char*>();
        } else {
            nu["type"] = "user";
        }

        if (chat.length() > 0) {
            nu[USERS_CHAT_ID_KEY] = chat;
        }

        if (isFourDigitCode(backup)) {
            nu[USERS_BACKUP_PIN_KEY] = backup;
        }

        _userUIDs[compactCount++] = uid;
    }

    for (int i = compactCount; i < MAX_USERS; i++) {
        _userUIDs[i] = "";
    }

    const bool sizeDiffers = compactCount != static_cast<int>(users.size());
    if (changed || sizeDiffers) {
        _usersDoc.clear();
        _usersDoc[USERS_KEY] = JsonArray();
        JsonArray dst = _usersArray();
        for (JsonObject srcUser : compactUsers) {
            JsonObject du = dst.add<JsonObject>();
            for (JsonPair kv : srcUser) {
                du[kv.key().c_str()] = kv.value();
            }
        }

        _saveUsersToFS();
        Serial.print("[AUTH] users.json compacted. active users=");
        Serial.println(compactCount);
    }

    _userCount = compactCount;
    return true;
}
