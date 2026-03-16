/**
 * ============================================================
 * AuthHandler - Authentication Component
 * ============================================================
 * 
 * PURPOSE: Handles multi-factor authentication (RFID + Keypad + Duress)
 * 
 * HARDWARE:
 *   - RFID RC522 (SPI): SS=5, SCK=18, MOSI=23, MISO=19, RST=4
 *   - Keypad 4x4 (safe scan): Rows[34,35,36,39], Cols[32,33,25,26]
 *   - Factory Reset: GPIO 0 (BOOT button - long press)
 * 
 * FEATURES:
 *   - RFID UID authentication
 *   - Keypad PIN entry with buffer
 *   - Duress code detection (9999)
 *   - Factory reset (GPIO 0 held 10 seconds)
 *   - User database (Preferences library)
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
#include <Keypad.h>
#include <Preferences.h>
#include "hardware_pins.h"

// Authentication result codes
enum AuthResult {
    AUTH_NONE,       // No authentication attempt
    AUTH_SUCCESS,    // Valid credentials
    AUTH_DENIED,     // Invalid credentials
    AUTH_DURESS      // Duress code entered (9999)
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
    
    // Factory reset
    bool checkFactoryReset();       // Check if BOOT button held 10s
    void performFactoryReset();     // Clear all users and settings
    
private:
    // Hardware pins
    static const int PIN_RFID_SS = SECURELOCK_PIN_RFID_SS;
    static const int PIN_RFID_RST = SECURELOCK_PIN_RFID_RST;
    static const int PIN_RFID_RST_FALLBACK = 21;  // Legacy wiring fallback
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
    
    // Hardware objects
    MFRC522 _rfid;
    Keypad _keypad;
    Preferences _prefs;
    
    // State variables
    String _pinBuffer;
    String _lastRFIDUID;
    int _activeRfidRstPin;
    unsigned long _factoryPressStart;
    bool _factoryPressed;
    
    // Private methods
    String _readRFIDUID();
    bool _validateStoredPIN(const String& pin);
    String _uidToString(byte* uid, byte size);
    String _buildUserKey(const String& uid) const;
    String _buildLegacyUserKey(const String& uid) const;
    String _getUserValue(const String& uid);
    void _migrateUserStorageKeys();
    void _saveUserList();
    void _loadUserList();
    
    // User tracking (UIDs of registered users for iteration)
    static const int MAX_USERS = 20;
    String _userUIDs[MAX_USERS];
    int _userCount;
};

#endif // AUTH_HANDLER_H
