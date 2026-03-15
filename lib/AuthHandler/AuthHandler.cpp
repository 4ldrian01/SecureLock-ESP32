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

    pinMode(PIN_RFID_RST, OUTPUT);
    digitalWrite(PIN_RFID_RST, LOW);
    delay(20);
    digitalWrite(PIN_RFID_RST, HIGH);
    delay(50);

    // Initialize SPI for RFID
    SPI.begin(
        SECURELOCK_PIN_SPI_SCK,
        SECURELOCK_PIN_SPI_MISO,
        SECURELOCK_PIN_SPI_MOSI,
        PIN_RFID_SS
    );  // SCK, MISO, MOSI, SS

    _rfid.PCD_Init();
    _rfid.PCD_AntennaOn();
    
    // Check RFID reader
    byte version = 0x00;
    for (int attempt = 0; attempt < 3; attempt++) {
        version = _rfid.PCD_ReadRegister(_rfid.VersionReg);
        if (version != 0x00 && version != 0xFF) {
            break;
        }

        _rfid.PCD_Reset();
        delay(50);
        _rfid.PCD_AntennaOn();
        delay(50);
    }

    if (version == 0x00 || version == 0xFF) {
        Serial.println("[AUTH] ⚠️ RFID reader not detected! Check wiring.");
        Serial.print("[AUTH] RFID pins → SS=");
        Serial.print(PIN_RFID_SS);
        Serial.print(", RST=");
        Serial.print(PIN_RFID_RST);
        Serial.print(", SCK=");
        Serial.print(SECURELOCK_PIN_SPI_SCK);
        Serial.print(", MOSI=");
        Serial.print(SECURELOCK_PIN_SPI_MOSI);
        Serial.print(", MISO=");
        Serial.println(SECURELOCK_PIN_SPI_MISO);
    } else {
        Serial.print("[AUTH] RFID RC522 v");
        Serial.print(version, HEX);
        Serial.println(" detected");
    }
    
    // Initialize factory reset button
    pinMode(PIN_FACTORY, INPUT_PULLUP);
    
    // Initialize Preferences (NVS storage)
    _prefs.begin("smartguard", false);
    
    // Load user list from NVS
    _loadUserList();
    _migrateUserStorageKeys();
    
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
    
    // Read UID
    String uid = _uidToString(_rfid.uid.uidByte, _rfid.uid.size);
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
 * Get last scanned RFID UID
 */
String AuthHandler::getLastRFIDUID() const {
    return _lastRFIDUID;
}

/**
 * Get keypad key press
 */
char AuthHandler::getKeypadKey() {
    return _keypad.getKey();
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
    String key = _buildUserKey(uid);
    String value = pin + ":" + name;
    size_t bytesWritten = _prefs.putString(key.c_str(), value);
    if (bytesWritten == 0) {
        Serial.print("[AUTH] ❌ Failed to store user record for UID: ");
        Serial.println(uid);
        return false;
    }

    // Cleanup legacy key if present
    String legacyKey = _buildLegacyUserKey(uid);
    if (legacyKey != key && _prefs.isKey(legacyKey.c_str())) {
        _prefs.remove(legacyKey.c_str());
    }
    
    // Track UID in list (avoid duplicates)
    bool alreadyTracked = false;
    for (int i = 0; i < _userCount; i++) {
        if (_userUIDs[i] == uid) {
            alreadyTracked = true;
            break;
        }
    }
    if (!alreadyTracked && _userCount < MAX_USERS) {
        _userUIDs[_userCount++] = uid;
        _saveUserList();
    }
    
    Serial.print("[AUTH] User added: ");
    Serial.print(name);
    Serial.print(" (UID: ");
    Serial.print(uid);
    Serial.println(")");
    
    return true;
}

/**
 * Remove user
 */
bool AuthHandler::removeUser(const String& uid) {
    String key = _buildUserKey(uid);
    String legacyKey = _buildLegacyUserKey(uid);

    bool removedRecord = false;
    if (_prefs.isKey(key.c_str())) {
        _prefs.remove(key.c_str());
        removedRecord = true;
    }
    if (legacyKey != key && _prefs.isKey(legacyKey.c_str())) {
        _prefs.remove(legacyKey.c_str());
        removedRecord = true;
    }

    if (!removedRecord) {
        return false;
    }
    
    // Remove from tracked UID list
    for (int i = 0; i < _userCount; i++) {
        if (_userUIDs[i] == uid) {
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
    Serial.println(uid);
    
    return true;
}

/**
 * Check if user exists
 */
bool AuthHandler::userExists(const String& uid) {
    String key = _buildUserKey(uid);
    if (_prefs.isKey(key.c_str())) {
        return true;
    }

    String legacyKey = _buildLegacyUserKey(uid);
    return legacyKey != key && _prefs.isKey(legacyKey.c_str());
}

/**
 * Get user name by UID
 */
String AuthHandler::getUserName(const String& uid) {
    String value = _getUserValue(uid);
    
    if (value.length() == 0) return "Unknown";
    
    // Format: "PIN:Name"
    int colonIndex = value.indexOf(':');
    if (colonIndex > 0) {
        return value.substring(colonIndex + 1);
    }
    
    return "Unknown";
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
 * Private: Build compact NVS key (NVS key max length is 15 chars)
 */
String AuthHandler::_buildUserKey(const String& uid) const {
    // 64-bit FNV-1a hash, truncated to 52 bits (13 hex chars)
    uint64_t hash = 1469598103934665603ULL;
    for (size_t i = 0; i < uid.length(); i++) {
        hash ^= static_cast<uint8_t>(uid.charAt(i));
        hash *= 1099511628211ULL;
    }

    uint64_t compact = (hash & 0x1FFFFFFFFFFFFFULL);
    char key[16];
    snprintf(key, sizeof(key), "u_%013llX", static_cast<unsigned long long>(compact));
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
    String key = _buildUserKey(uid);
    if (_prefs.isKey(key.c_str())) {
        return _prefs.getString(key.c_str(), "");
    }

    String legacyKey = _buildLegacyUserKey(uid);
    if (legacyKey != key && _prefs.isKey(legacyKey.c_str())) {
        return _prefs.getString(legacyKey.c_str(), "");
    }

    return "";
}

/**
 * Private: Migrate legacy key format to compact key format
 */
void AuthHandler::_migrateUserStorageKeys() {
    int migrated = 0;

    for (int i = 0; i < _userCount; i++) {
        String uid = _userUIDs[i];
        String compactKey = _buildUserKey(uid);
        String legacyKey = _buildLegacyUserKey(uid);

        if (_prefs.isKey(compactKey.c_str())) {
            continue;
        }

        if (!_prefs.isKey(legacyKey.c_str())) {
            continue;
        }

        String value = _prefs.getString(legacyKey.c_str(), "");
        if (value.length() == 0) {
            continue;
        }

        if (_prefs.putString(compactKey.c_str(), value) > 0) {
            _prefs.remove(legacyKey.c_str());
            migrated++;
        }
    }

    if (migrated > 0) {
        Serial.print("[AUTH] Migrated legacy user keys: ");
        Serial.println(migrated);
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
