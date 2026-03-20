/**
 * ============================================================
 * AuthHandler - Implementation
 * ============================================================
 */

#include "AuthHandler.h"
#include <SPI.h>

// Define duress code
const char* AuthHandler::DURESS_CODE = "9999";

/**
 * Constructor
 */
AuthHandler::AuthHandler()
    : _rfid(PIN_RFID_SS, PIN_RFID_RST),
      _keypad(makeKeymap(_keys), _rowPins, _colPins, ROWS, COLS),
      _pinBuffer(""),
      _lastRFIDUID(""),
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

/**
 * Initialize hardware
 */
void AuthHandler::init() {
    // Prepare RFID control pins before SPI init
    pinMode(PIN_RFID_SS, OUTPUT);
    digitalWrite(PIN_RFID_SS, HIGH);

    // Initialize SPI for RFID
    SPI.begin(
        SECURELOCK_PIN_SPI_SCK,
        SECURELOCK_PIN_SPI_MISO,
        SECURELOCK_PIN_SPI_MOSI,
        PIN_RFID_SS
    );  // SCK, MISO, MOSI, SS

    const int rstCandidates[2] = {PIN_RFID_RST, PIN_RFID_RST_FALLBACK};
    byte version = 0x00;
    bool detected = false;

    for (int i = 0; i < 2; i++) {
        const int rstPin = rstCandidates[i];

        if (i == 1 && rstPin == rstCandidates[0]) {
            continue; // Avoid duplicate probe if pins are the same
        }

        pinMode(rstPin, OUTPUT);
        digitalWrite(rstPin, LOW);
        delay(20);
        digitalWrite(rstPin, HIGH);
        delay(50);

        _rfid.PCD_Init(PIN_RFID_SS, rstPin);
        _rfid.PCD_AntennaOn();

        for (int attempt = 0; attempt < 3; attempt++) {
            version = _rfid.PCD_ReadRegister(_rfid.VersionReg);
            if (version != 0x00 && version != 0xFF) {
                _activeRfidRstPin = rstPin;
                detected = true;
                break;
            }

            _rfid.PCD_Reset();
            delay(50);
            _rfid.PCD_AntennaOn();
            delay(50);
        }

        if (detected) {
            break;
        }
    }

    if (!detected) {
        Serial.println("[AUTH] ⚠️ RFID reader not detected! Check wiring.");
        Serial.print("[AUTH] RFID pins → SS=");
        Serial.print(PIN_RFID_SS);
        Serial.print(", RST(primary)=");
        Serial.print(PIN_RFID_RST);
        Serial.print(", RST(fallback)=");
        Serial.print(PIN_RFID_RST_FALLBACK);
        Serial.print(", SCK=");
        Serial.print(SECURELOCK_PIN_SPI_SCK);
        Serial.print(", MOSI=");
        Serial.print(SECURELOCK_PIN_SPI_MOSI);
        Serial.print(", MISO=");
        Serial.println(SECURELOCK_PIN_SPI_MISO);
    } else {
        Serial.print("[AUTH] RFID RC522 v");
        Serial.print(version, HEX);
        Serial.print(" detected (RST GPIO ");
        Serial.print(_activeRfidRstPin);
        Serial.println(")");
    }
    
    // Initialize factory reset button
    pinMode(PIN_FACTORY, INPUT_PULLUP);
    
    // Initialize Preferences (NVS storage)
    _prefs.begin("smartguard", false);
    
    // Load user list from NVS
    _loadUserList();
    _migrateUserStorageKeys();
    _keypadReadyAtMs = 0;
    _keypadRuntimeSettlingStarted = false;
    
    Serial.println("[AUTH] Initialized");
    Serial.println("[AUTH] Keypad: 4x4 Matrix");
    Serial.println("[AUTH] Duress code: ****");
    
    // Create default admin user if none exists
    if (!userExists("DEFAULT_ADMIN")) {
        addUser("DEFAULT_ADMIN", "1234", "Admin");
        Serial.println("[AUTH] Default admin created (PIN: 1234)");
    }
    
    Serial.print("[AUTH] Registered users: ");
    Serial.println(_userCount);
}

/**
 * Update - Check factory reset
 */
void AuthHandler::update() {
    checkFactoryReset();
}

/**
 * Check RFID reader for card
 */
AuthResult AuthHandler::checkRFID() {
    // Check for new card
    if (!_rfid.PICC_IsNewCardPresent()) {
        return AUTH_NONE;
    }
    
    if (!_rfid.PICC_ReadCardSerial()) {
        return AUTH_NONE;
    }

    // Enforce anti-spam cooldown window after successful access
    if (isRFIDCooldownActive()) {
        _rfid.PICC_HaltA();
        _rfid.PCD_StopCrypto1();
        return AUTH_NONE;
    }
    
    // Read UID
    String uid = _normalizeUID(_uidToString(_rfid.uid.uidByte, _rfid.uid.size));
    _lastRFIDUID = uid;
    
    // Halt card
    _rfid.PICC_HaltA();
    _rfid.PCD_StopCrypto1();
    
    Serial.print("[AUTH] RFID detected: ");
    Serial.println(uid);
    
    // Check if user exists
    if (userExists(uid)) {
        String userName = getUserName(uid);
        Serial.print("[AUTH] ✅ User authenticated: ");
        Serial.println(userName);
        return AUTH_SUCCESS;
    } else {
        Serial.println("[AUTH] ❌ Unknown RFID card");
        return AUTH_DENIED;
    }
}

/**
 * Start strict RFID cooldown timer
 */
void AuthHandler::startRFIDCooldown(unsigned long cooldownMs) {
    _rfidCooldownStartMs = millis();
    _rfidCooldownDurationMs = cooldownMs;
}

/**
 * Check if RFID cooldown is currently active
 */
bool AuthHandler::isRFIDCooldownActive() const {
    if (_rfidCooldownDurationMs == 0) {
        return false;
    }

    return (millis() - _rfidCooldownStartMs) < _rfidCooldownDurationMs;
}

/**
 * Get last scanned RFID UID
 */
String AuthHandler::getLastRFIDUID() const {
    return _lastRFIDUID;
}

/**
 * Get keypad key press
 */
char AuthHandler::getKeypadKey() {
    const unsigned long now = millis();

    // Start keypad settle window when runtime polling actually begins (loop phase),
    // not during init, because setup may take several seconds (WiFi, web server).
    if (!_keypadRuntimeSettlingStarted) {
        _keypadRuntimeSettlingStarted = true;
        _keypadReadyAtMs = now + KEYPAD_STARTUP_SETTLE_MS;
    }

    // Allow keypad lines to electrically settle after boot to suppress phantom startup keys.
    if (now < _keypadReadyAtMs) {
        _keypad.getKey();  // Drain transient matrix reads during settle period.
        return '\0';
    }

    const char rawKey = _keypad.getKey();
    if (rawKey == NO_KEY) {
        return '\0';
    }

    // Ignore unsupported keypad symbols (A/B/C/D are not used in auth flow).
    const bool supported = ((rawKey >= '0' && rawKey <= '9') || rawKey == '*' || rawKey == '#');
    if (!supported) {
        return '\0';
    }

    if (_keypadMutedUntilMs > now) {
        return '\0';
    }

    // Basic per-key rate limiting to suppress switch bounce and floating-pin noise.
    if (now - _lastAcceptedKeyMs < KEYPAD_MIN_KEY_INTERVAL_MS) {
        return '\0';
    }

    // Detect key storms (common with floating input-only keypad lines) and temporarily mute keypad.
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

        Serial.println("[AUTH][WARN] Keypad noise storm detected - temporary input mute enabled");
        return '\0';
    }

    _lastAcceptedKeyMs = now;
    return rawKey;
}

/**
 * Append key to PIN buffer
 */
void AuthHandler::appendToBuffer(char key) {
    if (_pinBuffer.length() < 8) {  // Max 8 digits
        _pinBuffer += key;
        Serial.print("[AUTH] Key: ");
        Serial.print(key);
        Serial.print(" Buffer: ");
        for (size_t i = 0; i < _pinBuffer.length(); i++) {
            Serial.print("*");
        }
        Serial.println();
    }
}

/**
 * Clear PIN buffer
 */
void AuthHandler::clearBuffer() {
    _pinBuffer = "";
    Serial.println("[AUTH] Buffer cleared");
}

/**
 * Get current buffer
 */
String AuthHandler::getBuffer() const {
    return _pinBuffer;
}

/**
 * Validate buffer PIN
 */
AuthResult AuthHandler::validatePIN() {
    return validatePIN(_pinBuffer);
}

/**
 * Validate specific PIN
 */
AuthResult AuthHandler::validatePIN(const String& pin) {
    if (pin.length() == 0) {
        return AUTH_NONE;
    }
    
    // Check duress code FIRST
    if (pin == DURESS_CODE) {
        Serial.println("[AUTH] 🆘 DURESS CODE ENTERED!");
        return AUTH_DURESS;
    }
    
    // Check stored PINs
    if (_validateStoredPIN(pin)) {
        Serial.println("[AUTH] ✅ PIN validated");
        return AUTH_SUCCESS;
    }
    
    Serial.println("[AUTH] ❌ Invalid PIN");
    return AUTH_DENIED;
}

/**
 * Add new user
 */
bool AuthHandler::addUser(const String& uid, const String& pin, const String& name) {
    String normalizedUid = _normalizeUID(uid);
    if (normalizedUid.length() == 0) {
        Serial.println("[AUTH] ❌ Refusing to add user with empty/invalid UID");
        return false;
    }

    String key = _buildUserKey(normalizedUid);
    String value = pin + ":" + name;
    size_t bytesWritten = _prefs.putString(key.c_str(), value);
    if (bytesWritten == 0) {
        Serial.print("[AUTH] ❌ Failed to store user record for UID: ");
        Serial.println(normalizedUid);
        return false;
    }

    // Cleanup legacy key if present
    String legacyKey = _buildLegacyUserKey(normalizedUid);
    if (legacyKey != key && _prefs.isKey(legacyKey.c_str())) {
        _prefs.remove(legacyKey.c_str());
    }

    // Ensure each user has a fallback offline backup PIN metadata record.
    // Do not overwrite if already explicitly configured.
    String backupKey = _buildMetadataKey(normalizedUid, 'b');
    if (!_prefs.isKey(backupKey.c_str())) {
        setUserBackupPIN(normalizedUid, pin);
    }
    
    // Track UID in list (avoid duplicates)
    bool alreadyTracked = false;
    for (int i = 0; i < _userCount; i++) {
        if (_normalizeUID(_userUIDs[i]) == normalizedUid) {
            alreadyTracked = true;
            break;
        }
    }
    if (!alreadyTracked && _userCount < MAX_USERS) {
        _userUIDs[_userCount++] = normalizedUid;
        _saveUserList();
    }
    
    Serial.print("[AUTH] User added: ");
    Serial.print(name);
    Serial.print(" (UID: ");
    Serial.print(normalizedUid);
    Serial.println(")");
    
    return true;
}

/**
 * Remove user
 */
bool AuthHandler::removeUser(const String& uid) {
    String normalizedUid = _normalizeUID(uid);
    if (normalizedUid.length() == 0) {
        return false;
    }

    String key = _buildUserKey(normalizedUid);
    String legacyKey = _buildLegacyUserKey(normalizedUid);
    String chatKey = _buildMetadataKey(normalizedUid, 'c');
    String backupKey = _buildMetadataKey(normalizedUid, 'b');

    bool removedRecord = false;
    if (_prefs.isKey(key.c_str())) {
        _prefs.remove(key.c_str());
        removedRecord = true;
    }
    if (legacyKey != key && _prefs.isKey(legacyKey.c_str())) {
        _prefs.remove(legacyKey.c_str());
        removedRecord = true;
    }

    if (_prefs.isKey(chatKey.c_str())) {
        _prefs.remove(chatKey.c_str());
    }

    if (_prefs.isKey(backupKey.c_str())) {
        _prefs.remove(backupKey.c_str());
    }

    // Backward-compat removal for any raw UID key variant
    String rawUid = uid;
    rawUid.trim();
    if (rawUid.length() > 0 && rawUid != normalizedUid) {
        String rawKey = _buildUserKey(rawUid);
        String rawLegacyKey = _buildLegacyUserKey(rawUid);
        String rawChatKey = _buildMetadataKey(rawUid, 'c');
        String rawBackupKey = _buildMetadataKey(rawUid, 'b');

        if (_prefs.isKey(rawKey.c_str())) {
            _prefs.remove(rawKey.c_str());
            removedRecord = true;
        }

        if (rawLegacyKey != rawKey && _prefs.isKey(rawLegacyKey.c_str())) {
            _prefs.remove(rawLegacyKey.c_str());
            removedRecord = true;
        }

        if (_prefs.isKey(rawChatKey.c_str())) {
            _prefs.remove(rawChatKey.c_str());
        }

        if (_prefs.isKey(rawBackupKey.c_str())) {
            _prefs.remove(rawBackupKey.c_str());
        }
    }

    if (!removedRecord) {
        return false;
    }
    
    // Remove from tracked UID list
    for (int i = 0; i < _userCount; i++) {
        if (_normalizeUID(_userUIDs[i]) == normalizedUid) {
            // Shift remaining elements
            for (int j = i; j < _userCount - 1; j++) {
                _userUIDs[j] = _userUIDs[j + 1];
            }
            _userUIDs[_userCount - 1] = "";
            _userCount--;
            _saveUserList();
            break;
        }
    }
    
    Serial.print("[AUTH] User removed: ");
    Serial.println(normalizedUid);
    
    return true;
}

/**
 * Check if user exists
 */
bool AuthHandler::userExists(const String& uid) {
    String normalizedUid = _normalizeUID(uid);
    if (normalizedUid.length() == 0) {
        return false;
    }

    String key = _buildUserKey(normalizedUid);
    if (_prefs.isKey(key.c_str())) {
        return true;
    }

    String legacyKey = _buildLegacyUserKey(normalizedUid);
    if (legacyKey != key && _prefs.isKey(legacyKey.c_str())) {
        return true;
    }

    // Backward-compatible lookup if caller provided a non-canonical UID
    String rawUid = uid;
    rawUid.trim();
    if (rawUid.length() > 0 && rawUid != normalizedUid) {
        String rawKey = _buildUserKey(rawUid);
        if (_prefs.isKey(rawKey.c_str())) {
            return true;
        }

        String rawLegacyKey = _buildLegacyUserKey(rawUid);
        if (rawLegacyKey != rawKey && _prefs.isKey(rawLegacyKey.c_str())) {
            return true;
        }
    }

    return false;
}

/**
 * Get user name by UID
 */
String AuthHandler::getUserName(const String& uid) {
    String value = _getUserValue(_normalizeUID(uid));
    
    if (value.length() == 0) return "Unknown";
    
    // Format: "PIN:Name"
    int colonIndex = value.indexOf(':');
    if (colonIndex > 0) {
        return value.substring(colonIndex + 1);
    }
    
    return "Unknown";
}

/**
 * Get user PIN by UID
 */
String AuthHandler::getUserPIN(const String& uid) {
    String value = _getUserValue(_normalizeUID(uid));

    if (value.length() == 0) {
        return "";
    }

    // Format: "PIN:Name"
    int colonIndex = value.indexOf(':');
    if (colonIndex > 0) {
        return value.substring(0, colonIndex);
    }

    return "";
}

bool AuthHandler::setUserTelegramChatId(const String& uid, const String& chatId) {
    String normalizedUid = _normalizeUID(uid);
    if (normalizedUid.length() == 0) {
        return false;
    }

    String normalizedChatId = chatId;
    normalizedChatId.trim();

    String key = _buildMetadataKey(normalizedUid, 'c');
    if (normalizedChatId.length() == 0) {
        if (_prefs.isKey(key.c_str())) {
            _prefs.remove(key.c_str());
        }
        return true;
    }

    return _prefs.putString(key.c_str(), normalizedChatId) > 0;
}

String AuthHandler::getUserTelegramChatId(const String& uid) {
    String normalizedUid = _normalizeUID(uid);
    if (normalizedUid.length() == 0) {
        return "";
    }

    String key = _buildMetadataKey(normalizedUid, 'c');
    if (_prefs.isKey(key.c_str())) {
        return _prefs.getString(key.c_str(), "");
    }

    return "";
}

bool AuthHandler::setUserBackupPIN(const String& uid, const String& backupPin) {
    String normalizedUid = _normalizeUID(uid);
    if (normalizedUid.length() == 0) {
        return false;
    }

    String normalizedPin = backupPin;
    normalizedPin.trim();

    if (normalizedPin.length() != 4) {
        return false;
    }

    for (size_t i = 0; i < normalizedPin.length(); i++) {
        if (!isDigit(normalizedPin.charAt(i))) {
            return false;
        }
    }

    String key = _buildMetadataKey(normalizedUid, 'b');
    return _prefs.putString(key.c_str(), normalizedPin) > 0;
}

String AuthHandler::getUserBackupPIN(const String& uid) {
    String normalizedUid = _normalizeUID(uid);
    if (normalizedUid.length() == 0) {
        return "";
    }

    String key = _buildMetadataKey(normalizedUid, 'b');
    if (_prefs.isKey(key.c_str())) {
        String storedPin = _prefs.getString(key.c_str(), "");
        storedPin.trim();

        if (storedPin.length() == 4) {
            bool valid = true;
            for (size_t i = 0; i < storedPin.length(); i++) {
                if (!isDigit(storedPin.charAt(i))) {
                    valid = false;
                    break;
                }
            }

            if (valid) {
                return storedPin;
            }
        }
    }

    // Backward compatibility: fallback to primary keypad PIN if no backup PIN metadata exists.
    return getUserPIN(normalizedUid);
}

/**
 * Get number of tracked users in auth storage
 */
int AuthHandler::getUserCount() const {
    return _userCount;
}

/**
 * Get tracked UID at index (normalized)
 */
String AuthHandler::getUserUIDAt(int index) const {
    if (index < 0 || index >= _userCount) {
        return "";
    }

    return _normalizeUID(_userUIDs[index]);
}

/**
 * Check for factory reset (BOOT button held 10 seconds)
 */
bool AuthHandler::checkFactoryReset() {
    bool currentPressed = digitalRead(PIN_FACTORY) == LOW;  // Active LOW
    unsigned long now = millis();
    
    if (currentPressed && !_factoryPressed) {
        // Button just pressed
        _factoryPressed = true;
        _factoryPressStart = now;
        Serial.println("[AUTH] Factory reset button pressed...");
    } else if (currentPressed && _factoryPressed) {
        // Button held - check duration
        unsigned long duration = now - _factoryPressStart;
        
        if (duration >= FACTORY_RESET_TIME) {
            Serial.println("[AUTH] 🔄 FACTORY RESET TRIGGERED!");
            performFactoryReset();
            _factoryPressed = false;
            return true;
        }
        
        // Print countdown every second
        static unsigned long lastPrint = 0;
        if (now - lastPrint >= 1000) {
            lastPrint = now;
            int remaining = (FACTORY_RESET_TIME - duration) / 1000;
            Serial.print("[AUTH] Factory reset in ");
            Serial.print(remaining);
            Serial.println("s...");
        }
    } else if (!currentPressed && _factoryPressed) {
        // Button released
        _factoryPressed = false;
        Serial.println("[AUTH] Factory reset cancelled");
    }
    
    return false;
}

/**
 * Perform factory reset
 */
void AuthHandler::performFactoryReset() {
    Serial.println("[AUTH] ════════════════════════════════");
    Serial.println("[AUTH] PERFORMING FACTORY RESET");
    Serial.println("[AUTH] ════════════════════════════════");
    
    // Clear all preferences
    _prefs.clear();
    
    // Reset user tracking
    _userCount = 0;
    for (int i = 0; i < MAX_USERS; i++) {
        _userUIDs[i] = "";
    }
    
    // Recreate default admin
    addUser("DEFAULT_ADMIN", "1234", "Admin");
    
    Serial.println("[AUTH] ✓ All users deleted");
    Serial.println("[AUTH] ✓ Settings reset");
    Serial.println("[AUTH] ✓ Default admin created (PIN: 1234)");
    Serial.println("[AUTH] ════════════════════════════════");
    Serial.println("[AUTH] FACTORY RESET COMPLETE");
    Serial.println("[AUTH] ════════════════════════════════");
}

/**
 * Private: Validate PIN against ALL stored users
 */
bool AuthHandler::_validateStoredPIN(const String& pin) {
    for (int i = 0; i < _userCount; i++) {
        String value = _getUserValue(_userUIDs[i]);
        
        if (value.length() > 0) {
            int colonIndex = value.indexOf(':');
            if (colonIndex > 0) {
                String storedPin = value.substring(0, colonIndex);
                if (pin == storedPin) {
                    Serial.print("[AUTH] PIN matched user: ");
                    Serial.println(value.substring(colonIndex + 1));
                    return true;
                }
            }
        }
    }
    
    return false;
}

/**
 * Private: Convert RFID UID to hex string
 */
String AuthHandler::_uidToString(byte* uid, byte size) {
    String result = "";
    for (byte i = 0; i < size; i++) {
        if (uid[i] < 0x10) result += "0";
        result += String(uid[i], HEX);
    }
    result.toUpperCase();
    return result;
}

/**
 * Private: Normalize UID for stable comparisons/storage
 */
String AuthHandler::_normalizeUID(const String& uid) const {
    String normalized = uid;
    normalized.trim();
    normalized.toUpperCase();

    // Accept multiple display formats: "AA BB", "AA:BB", "AA-BB"
    normalized.replace(" ", "");
    normalized.replace(":", "");
    normalized.replace("-", "");

    return normalized;
}

/**
 * Private: Build compact NVS key (NVS key max length is 15 chars)
 */
String AuthHandler::_buildUserKey(const String& uid) const {
    return _buildMetadataKey(uid, 'u');
}

String AuthHandler::_buildMetadataKey(const String& uid, char prefix) const {
    // 64-bit FNV-1a hash, truncated to 52 bits (13 hex chars)
    uint64_t hash = 1469598103934665603ULL;
    for (size_t i = 0; i < uid.length(); i++) {
        hash ^= static_cast<uint8_t>(uid.charAt(i));
        hash *= 1099511628211ULL;
    }

    uint64_t compact = (hash & 0x1FFFFFFFFFFFFFULL);
    char key[16];
    snprintf(key, sizeof(key), "%c_%013llX", prefix, static_cast<unsigned long long>(compact));
    return String(key);
}

/**
 * Private: Build legacy user key format used by older firmware
 */
String AuthHandler::_buildLegacyUserKey(const String& uid) const {
    return "user_" + uid;
}

/**
 * Private: Read user value from compact key, fallback to legacy key
 */
String AuthHandler::_getUserValue(const String& uid) {
    String normalizedUid = _normalizeUID(uid);

    String key = _buildUserKey(normalizedUid);
    if (_prefs.isKey(key.c_str())) {
        return _prefs.getString(key.c_str(), "");
    }

    String legacyKey = _buildLegacyUserKey(normalizedUid);
    if (legacyKey != key && _prefs.isKey(legacyKey.c_str())) {
        return _prefs.getString(legacyKey.c_str(), "");
    }

    // Backward-compatible fallback for any pre-normalization key variant
    String rawUid = uid;
    rawUid.trim();
    if (rawUid.length() > 0 && rawUid != normalizedUid) {
        String rawKey = _buildUserKey(rawUid);
        if (_prefs.isKey(rawKey.c_str())) {
            return _prefs.getString(rawKey.c_str(), "");
        }

        String rawLegacyKey = _buildLegacyUserKey(rawUid);
        if (rawLegacyKey != rawKey && _prefs.isKey(rawLegacyKey.c_str())) {
            return _prefs.getString(rawLegacyKey.c_str(), "");
        }
    }

    return "";
}

/**
 * Private: Migrate legacy key format to compact key format
 */
void AuthHandler::_migrateUserStorageKeys() {
    int migrated = 0;
    bool listChanged = false;

    for (int i = 0; i < _userCount; i++) {
        String rawUid = _userUIDs[i];
        rawUid.trim();
        String normalizedUid = _normalizeUID(rawUid);

        if (normalizedUid.length() == 0) {
            continue;
        }

        String normalizedCompactKey = _buildUserKey(normalizedUid);
        String normalizedLegacyKey = _buildLegacyUserKey(normalizedUid);

        String value = "";
        if (_prefs.isKey(normalizedCompactKey.c_str())) {
            value = _prefs.getString(normalizedCompactKey.c_str(), "");
        } else if (_prefs.isKey(normalizedLegacyKey.c_str())) {
            value = _prefs.getString(normalizedLegacyKey.c_str(), "");
        }

        String rawCompactKey = _buildUserKey(rawUid);
        String rawLegacyKey = _buildLegacyUserKey(rawUid);

        if (value.length() == 0) {
            if (_prefs.isKey(rawCompactKey.c_str())) {
                value = _prefs.getString(rawCompactKey.c_str(), "");
            } else if (_prefs.isKey(rawLegacyKey.c_str())) {
                value = _prefs.getString(rawLegacyKey.c_str(), "");
            }
        }

        if (value.length() > 0) {
            if (!_prefs.isKey(normalizedCompactKey.c_str())) {
                if (_prefs.putString(normalizedCompactKey.c_str(), value) > 0) {
                    migrated++;
                }
            }

            if (rawCompactKey != normalizedCompactKey && _prefs.isKey(rawCompactKey.c_str())) {
                _prefs.remove(rawCompactKey.c_str());
            }

            if (rawLegacyKey != normalizedLegacyKey && _prefs.isKey(rawLegacyKey.c_str())) {
                _prefs.remove(rawLegacyKey.c_str());
            }

            if (_prefs.isKey(normalizedLegacyKey.c_str()) && normalizedLegacyKey != normalizedCompactKey) {
                _prefs.remove(normalizedLegacyKey.c_str());
            }
        }

        if (_userUIDs[i] != normalizedUid) {
            _userUIDs[i] = normalizedUid;
            listChanged = true;
        }
    }

    // Deduplicate normalized UID list
    int uniqueCount = 0;
    String uniqueUIDs[MAX_USERS];
    for (int i = 0; i < _userCount; i++) {
        String uid = _normalizeUID(_userUIDs[i]);
        if (uid.length() == 0) {
            continue;
        }

        bool exists = false;
        for (int j = 0; j < uniqueCount; j++) {
            if (uniqueUIDs[j] == uid) {
                exists = true;
                break;
            }
        }

        if (!exists && uniqueCount < MAX_USERS) {
            uniqueUIDs[uniqueCount++] = uid;
        }
    }

    if (uniqueCount != _userCount) {
        listChanged = true;
    }

    for (int i = 0; i < MAX_USERS; i++) {
        _userUIDs[i] = (i < uniqueCount) ? uniqueUIDs[i] : "";
    }
    _userCount = uniqueCount;

    if (migrated > 0) {
        Serial.print("[AUTH] Migrated legacy user keys: ");
        Serial.println(migrated);
    }

    if (listChanged) {
        _saveUserList();
        Serial.println("[AUTH] Normalized user UID list in NVS");
    }
}

/**
 * Private: Save user UID list to NVS for persistence
 */
void AuthHandler::_saveUserList() {
    String list = "";
    for (int i = 0; i < _userCount; i++) {
        if (i > 0) list += ",";
        list += _userUIDs[i];
    }
    _prefs.putString("user_list", list);
}

/**
 * Private: Load user UID list from NVS
 */
void AuthHandler::_loadUserList() {
    String list = _prefs.getString("user_list", "");
    _userCount = 0;
    bool deduplicated = false;
    
    if (list.length() == 0) return;
    
    int start = 0;
    while (start < (int)list.length() && _userCount < MAX_USERS) {
        int comma = list.indexOf(',', start);
        if (comma < 0) comma = list.length();
        
        String uid = list.substring(start, comma);
        uid.trim();
        if (uid.length() > 0) {
            bool exists = false;
            for (int i = 0; i < _userCount; i++) {
                if (_userUIDs[i] == uid) {
                    exists = true;
                    deduplicated = true;
                    break;
                }
            }

            if (!exists && _userCount < MAX_USERS) {
                _userUIDs[_userCount++] = uid;
            }
        }
        start = comma + 1;
    }

    if (deduplicated) {
        _saveUserList();
        Serial.println("[AUTH] Deduplicated user list in NVS");
    }
    
    Serial.print("[AUTH] Loaded ");
    Serial.print(_userCount);
    Serial.println(" users from NVS");
}
