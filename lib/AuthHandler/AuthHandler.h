/**
 * ============================================================
 * AuthHandler - Authentication Component
 * ============================================================
 * 
 * PURPOSE: Handles multi-factor authentication (RFID + Keypad + Duress)
 * 
 * HARDWARE:
 *   - RFID RC522 (SPI): SS=5, SCK=18, MOSI=25, MISO=19, RST=4
 *   - Keypad 4x4 (safe scan): Rows[34,35,39,36], Cols[16,17,21,23] (16=RX2, 17=TX2)
 *   - Factory Reset: GPIO 0 (BOOT button - long press)
 * 
 * FEATURES:
 *   - RFID UID authentication
 *   - Keypad PIN entry with buffer
 *   - Duress code detection (2580)
 *   - Factory reset (GPIO 0 held 10 seconds)
 *   - User database (LittleFS users.json)
 * 
 * AUTHENTICATION RESULTS:
 *   - AUTH_SUCCESS: Valid credentials
 *   - AUTH_DENIED: Invalid credentials
 *   - AUTH_DURESS: Duress code entered (unlock + silent alarm)
 *   - AUTH_NONE: No authentication attempt
 * 
 * USAGE:
 *   AuthHandler auth;
 *   auth.init();
 *   auth.update();                // Call in loop()
 *   
 *   AuthResult result = auth.checkRFID();
 *   if (result == AUTH_SUCCESS) {
 *       // Unlock
 *   }
 *   
 *   char key = auth.getKeypadKey();
 *   if (key == '#') {
 *       AuthResult result = auth.validatePIN();
 *   }
 * 
 * ============================================================
 */

#ifndef AUTH_HANDLER_H
#define AUTH_HANDLER_H

#include <Arduino.h>
#include <MFRC522.h>
#include <ArduinoJson.h>
#include "hardware_pins.h"

// Authentication result codes
enum AuthResult {
    AUTH_NONE,       // No authentication attempt
    AUTH_SUCCESS,    // Valid credentials
    AUTH_DENIED,     // Invalid credentials
    AUTH_DURESS      // Duress code entered (2580)
};

class AuthHandler {
public:
    // Constructor
    AuthHandler();
    
    // Lifecycle
    void init();                    // Setup RFID, Keypad, Preferences
    void update();                  // Check factory reset (call in loop)
    
    // RFID authentication
    AuthResult checkRFID();         // Poll RFID reader
    String getLastRFIDUID() const;  // Get last scanned UID
    unsigned long getLastRFIDScanMs() const;
    bool isRFIDReady() const;
    int getActiveRFIDRstPin() const;
    bool isKeypadReady() const;
    bool isKeypadMuted() const;
    unsigned long getKeypadMuteRemainingMs() const;
    char getLastAcceptedKey() const;
    unsigned long getLastAcceptedKeyMs() const;
    void startRFIDCooldown(unsigned long cooldownMs = RFID_COOLDOWN_MS);
    bool isRFIDCooldownActive() const;
    
    // Keypad input
    char getKeypadKey();            // Get pressed key (or '\0')
    void appendToBuffer(char key);  // Add key to PIN buffer
    void clearBuffer();             // Clear PIN buffer
    String getBuffer() const;       // Get current buffer
    
    // PIN validation
    AuthResult validatePIN();       // Validate buffer against stored PINs
    AuthResult validatePIN(const String& pin);  // Validate specific PIN
    
    // User management
    bool addUser(const String& uid, const String& pin, const String& name);
    bool removeUser(const String& uid);
    bool userExists(const String& uid);
    String getUserName(const String& uid);
    String getUserPIN(const String& uid);
    bool setUserTelegramChatId(const String& uid, const String& chatId);
    String getUserTelegramChatId(const String& uid);
    bool isKnownTelegramChatId(const String& chatId);
    bool setUserBackupPIN(const String& uid, const String& backupPin);
    String getUserBackupPIN(const String& uid);
    int getUserCount() const;
    String getUserUIDAt(int index) const;
    
    // Factory reset
    bool checkFactoryReset();       // Check if BOOT button held 10s
    void performFactoryReset();     // Clear all users and settings
    
private:
    // Hardware pins
    static const int PIN_RFID_SS = SECURELOCK_PIN_RFID_SS;
    static const int PIN_RFID_RST = SECURELOCK_PIN_RFID_RST;
#ifdef SECURELOCK_PIN_RFID_RST_FALLBACK
    static const int PIN_RFID_RST_FALLBACK = SECURELOCK_PIN_RFID_RST_FALLBACK;
#else
    static const int PIN_RFID_RST_FALLBACK = -1;  // Disabled unless explicitly configured
#endif
    static const int PIN_FACTORY = SECURELOCK_PIN_BOOT;
    
    // Keypad configuration
    static const byte ROWS = 4;
    static const byte COLS = 4;
    byte _rowPins[ROWS] = {
        SECURELOCK_PIN_KEYPAD_R1,
        SECURELOCK_PIN_KEYPAD_R2,
        SECURELOCK_PIN_KEYPAD_R3,
        SECURELOCK_PIN_KEYPAD_R4
    };
    byte _colPins[COLS] = {
        SECURELOCK_PIN_KEYPAD_C1,
        SECURELOCK_PIN_KEYPAD_C2,
        SECURELOCK_PIN_KEYPAD_C3,
        SECURELOCK_PIN_KEYPAD_C4
    };
    char _keys[ROWS][COLS] = {
        {'1', '2', '3', 'A'},
        {'4', '5', '6', 'B'},
        {'7', '8', '9', 'C'},
        {'*', '0', '#', 'D'}
    };
    
    // Duress code
    static const char* DURESS_CODE;
    
    // Factory reset timing
    static const unsigned long FACTORY_RESET_TIME = 10000;  // 10 seconds
    static const unsigned long RFID_COOLDOWN_MS = 450;      // Fast re-detect while still debouncing held cards
    static const unsigned long RFID_RECOVERY_INTERVAL_MS = 1500;
    static const unsigned long KEYPAD_MIN_KEY_INTERVAL_MS = 60;
    static const unsigned long KEYPAD_NOISE_WINDOW_MS = 2000;
    static const int KEYPAD_NOISE_THRESHOLD = 20;
    static const unsigned long KEYPAD_MUTE_DURATION_MS = 3000;
    static const unsigned long KEYPAD_STARTUP_SETTLE_MS = 600;
    static const unsigned long KEYPAD_STABLE_PRESS_MS = 12;
    static const unsigned long KEYPAD_STABLE_RELEASE_MS = 10;
    static const unsigned long KEYPAD_SAME_KEY_REPRESS_MS = 140;
    
    // Hardware objects
    MFRC522 _rfid;
    JsonDocument _usersDoc;
    
    // State variables
    String _pinBuffer;
    String _lastRFIDUID;
    unsigned long _lastRFIDScanMs;
    unsigned long _rfidCooldownStartMs;
    unsigned long _rfidCooldownDurationMs;
    unsigned long _lastRFIDRecoverAttemptMs;
    unsigned long _lastAcceptedKeyMs;
    char _lastAcceptedKeyChar;
    char _lastRawKey;
    unsigned long _lastRawKeyChangeMs;
    bool _sameKeyRetriggerUsed;
    unsigned long _keypadNoiseWindowStartMs;
    int _keypadNoiseCount;
    unsigned long _keypadMutedUntilMs;
    unsigned long _keypadReadyAtMs;
    bool _keypadRuntimeSettlingStarted;
    char _heldKey;
    int _activeRfidRstPin;
    bool _rfidReady;
    unsigned long _factoryPressStart;
    bool _factoryPressed;
    
    // Private methods
    String _readRFIDUID();
    bool _attemptRFIDRecovery();
    bool _validateStoredPIN(const String& pin);
    String _uidToString(byte* uid, byte size);
    String _normalizeUID(const String& uid) const;
    String _extractUserUID(JsonObjectConst user) const;
    bool _loadUsersFromFS();
    bool _saveUsersToFS();
    JsonArray _usersArray();
    JsonObject _findUserByUID(const String& uid);
    bool _ensureFileSystemReady();
    bool _compactUsers();
    char _scanKeypadRaw();
    
    // User tracking (UIDs of registered users for iteration)
    static const int MAX_USERS = 20;
    String _userUIDs[MAX_USERS];
    int _userCount;
};

#endif // AUTH_HANDLER_H
