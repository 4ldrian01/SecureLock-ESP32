/**
 * ============================================================
 * LockManager - Implementation
 * ============================================================
 */

#include "LockManager.h"

/**
 * Constructor - Initialize state variables
 */
LockManager::LockManager()
    : _locked(true),
      _doorOpen(false),
      _tampered(false),
      _unlockStartTime(0),
      _autoLockDelay(DEFAULT_UNLOCK_TIME),
      _autoLockActive(false),
      _ledBlinkEnabled(false),
      _ledState(false),
      _lastLEDBlink(0)
{
}

/**
 * Initialize hardware
 */
void LockManager::init() {
    pinMode(PIN_RELAY, OUTPUT);
    pinMode(PIN_LED, OUTPUT);
    pinMode(PIN_DOOR, INPUT_PULLUP);  // Reed switch with pull-up
    
    digitalWrite(PIN_RELAY, LOW);     // Start locked
    digitalWrite(PIN_LED, LOW);       // LED off
    
    _locked = true;
    _doorOpen = digitalRead(PIN_DOOR) == HIGH;
    _tampered = false;
    
    Serial.println("[LOCK] Initialized - State: LOCKED");
    Serial.print("[LOCK] Door: ");
    Serial.println(_doorOpen ? "OPEN" : "CLOSED");
}

/**
 * Update state - Call in loop()
 * WHAT: Non-blocking state machine for LED patterns and auto-lock
 * HOW: Checks millis() timer without blocking, updates LED blink pattern
 * WHY: Ensures door auto-locks after 5 seconds while maintaining responsive system
 */
void LockManager::update() {
    _updateLED();         // Update LED blink pattern (non-blocking)
    _updateDoorState();   // Monitor reed switch for tamper detection
    
    // Check auto-lock timer (non-blocking)
    if (_autoLockActive && !_locked) {
        if (millis() - _unlockStartTime >= _autoLockDelay) {
            lock();       // Automatically re-lock after delay
            Serial.println("[LOCK] Auto-lock timer expired");
        }
    }
}

/**
 * Unlock the door
 * WHAT: Energizes relay to open solenoid lock
 * HOW: Sets relay GPIO HIGH, starts 5-second auto-lock timer
 * WHY: Provides temporary access while ensuring automatic re-locking for security
 */
void LockManager::unlock() {
    if (_locked) {
        _setRelay(true);              // Energize relay
        _locked = false;
        _unlockStartTime = millis();  // Start timer for auto-lock
        _autoLockActive = true;
        _tampered = false;            // Clear tamper flag on legitimate unlock
        
        Serial.println("[LOCK] 🔓 UNLOCKED - Auto-lock in 5s");
        setLEDSolid(true);  // LED solid when unlocked
    } else {
        // Already unlocked, reset timer
        _unlockStartTime = millis();
        Serial.println("[LOCK] Unlock timer reset");
    }
}

/**
 * Lock the door immediately
 */
void LockManager::lock() {
    if (!_locked) {
        _setRelay(false);
        _locked = true;
        _autoLockActive = false;
        
        Serial.println("[LOCK] 🔒 LOCKED");
        setLEDBlink(true);  // LED blinks when locked
    }
}

/**
 * Get lock state
 */
bool LockManager::isLocked() const {
    return _locked;
}

/**
 * Get door state from reed switch
 */
bool LockManager::isDoorOpen() const {
    return _doorOpen;
}

/**
 * Check if door was tampered (opened while locked)
 */
bool LockManager::isDoorTampered() const {
    return _tampered;
}

/**
 * Enable/disable LED blinking
 */
void LockManager::setLEDBlink(bool enable) {
    _ledBlinkEnabled = enable;
    if (!enable) {
        digitalWrite(PIN_LED, LOW);
        _ledState = false;
    }
}

/**
 * Set LED solid on/off
 */
void LockManager::setLEDSolid(bool on) {
    _ledBlinkEnabled = false;
    _ledState = on;
    digitalWrite(PIN_LED, on ? HIGH : LOW);
}

/**
 * Set auto-lock delay
 */
void LockManager::setAutoLockDelay(unsigned long delayMs) {
    _autoLockDelay = delayMs;
    Serial.print("[LOCK] Auto-lock delay: ");
    Serial.print(delayMs / 1000);
    Serial.println("s");
}

/**
 * Private: Set relay state
 */
void LockManager::_setRelay(bool energized) {
    digitalWrite(PIN_RELAY, energized ? HIGH : LOW);
}

/**
 * Private: Update LED (blinking or solid)
 */
void LockManager::_updateLED() {
    if (_ledBlinkEnabled) {
        unsigned long now = millis();
        if (now - _lastLEDBlink >= LED_BLINK_INTERVAL) {
            _lastLEDBlink = now;
            _ledState = !_ledState;
            digitalWrite(PIN_LED, _ledState ? HIGH : LOW);
        }
    }
}

/**
 * Private: Monitor door state for tampering
 */
void LockManager::_updateDoorState() {
    bool currentDoorOpen = digitalRead(PIN_DOOR) == HIGH;
    
    // Detect door state change
    if (currentDoorOpen != _doorOpen) {
        _doorOpen = currentDoorOpen;
        
        Serial.print("[LOCK] Door: ");
        Serial.println(_doorOpen ? "OPEN" : "CLOSED");
        
        // Check for tampering (door opened while locked)
        if (_doorOpen && _locked) {
            _tampered = true;
            Serial.println("[LOCK] ⚠️ TAMPER DETECTED - Door opened while locked!");
        }
        
        // Auto-lock when door closes (if unlocked)
        if (!_doorOpen && !_locked) {
            Serial.println("[LOCK] Door closed - Triggering lock");
            lock();
        }
    }
}
