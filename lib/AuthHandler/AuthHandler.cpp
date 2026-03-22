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
}

AuthHandler::AuthHandler()
    : _rfid(PIN_RFID_SS, PIN_RFID_RST),
      _keypad(makeKeymap(_keys), _rowPins, _colPins, ROWS, COLS),
      _pinBuffer(""),
      _lastRFIDUID(""),
    _lastRFIDScanMs(0),
      _rfidCooldownStartMs(0),
      _rfidCooldownDurationMs(0),
      _lastAcceptedKeyMs(0),
      _keypadNoiseWindowStartMs(0),
      _keypadNoiseCount(0),
      _keypadMutedUntilMs(0),
      _keypadReadyAtMs(0),
      _keypadRuntimeSettlingStarted(false),
      _activeRfidRstPin(PIN_RFID_RST),
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

    pinMode(PIN_RFID_RST, OUTPUT);
    digitalWrite(PIN_RFID_RST, HIGH);

    _rfid.PCD_Init(PIN_RFID_SS, PIN_RFID_RST);
    _rfid.PCD_AntennaOn();

    const byte version = _rfid.PCD_ReadRegister(_rfid.VersionReg);
    if (version == 0x00 || version == 0xFF) {
        Serial.println("[AUTH] RFID reader not detected");
    } else {
        Serial.print("[AUTH] RFID RC522 version 0x");
        Serial.println(version, HEX);
    }

    pinMode(PIN_FACTORY, INPUT_PULLUP);

    _keypadReadyAtMs = 0;
    _keypadRuntimeSettlingStarted = false;

    _loadUsersFromFS();

    Serial.println("[AUTH] Initialized");
    Serial.print("[AUTH] Users loaded: ");
    Serial.println(_userCount);
}

void AuthHandler::update() {
    checkFactoryReset();
}

AuthResult AuthHandler::checkRFID() {
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

char AuthHandler::getKeypadKey() {
    const unsigned long now = millis();

    if (!_keypadRuntimeSettlingStarted) {
        _keypadRuntimeSettlingStarted = true;
        _keypadReadyAtMs = now + KEYPAD_STARTUP_SETTLE_MS;
    }

    if (now < _keypadReadyAtMs) {
        _keypad.getKey();
        return '\0';
    }

    const char rawKey = _keypad.getKey();
    if (rawKey == NO_KEY) {
        return '\0';
    }

    const bool supported = ((rawKey >= '0' && rawKey <= '9') || rawKey == '*' || rawKey == '#');
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
    String normalizedUid = _normalizeUID(uid);
    if (normalizedUid.length() == 0 || pin.length() == 0 || name.length() == 0) {
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
        if (users.isNull() || users.size() >= MAX_USERS) {
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
    String normalizedUid = _normalizeUID(uid);
    JsonArray users = _usersArray();
    if (users.isNull()) {
        return false;
    }

    bool removed = false;
    for (size_t i = users.size(); i > 0; i--) {
        String listedUid = _normalizeUID(users[i - 1]["uid"].as<String>());
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

bool AuthHandler::_loadUsersFromFS() {
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

    _userCount = 0;
    JsonArray users = _usersArray();
    for (JsonObject u : users) {
        if (_userCount >= MAX_USERS) {
            break;
        }

        String uid = _normalizeUID(u[USERS_CARD_UID_KEY] | "");
        if (uid.length() == 0) {
            uid = _normalizeUID(u[USERS_UID_FALLBACK_KEY] | "");
            if (uid.length() > 0) {
                u[USERS_CARD_UID_KEY] = uid;
            }
        }
        if (uid.length() == 0) {
            continue;
        }

        bool dup = false;
        for (int i = 0; i < _userCount; i++) {
            if (_userUIDs[i] == uid) {
                dup = true;
                break;
            }
        }

        if (!dup) {
            _userUIDs[_userCount++] = uid;
        }
    }

    for (int i = _userCount; i < MAX_USERS; i++) {
        _userUIDs[i] = "";
    }

    return true;
}

bool AuthHandler::_saveUsersToFS() {
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
        String listed = _normalizeUID(u[USERS_CARD_UID_KEY] | "");
        if (listed.length() == 0) {
            listed = _normalizeUID(u[USERS_UID_FALLBACK_KEY] | "");
        }
        if (listed == uid) {
            return u;
        }
    }

    return JsonObject();
}
