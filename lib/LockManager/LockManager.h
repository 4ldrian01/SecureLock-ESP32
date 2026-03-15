/**
 * ============================================================
 * LockManager - Hardware Control Component
 * ============================================================
 * 
 * PURPOSE: Controls physical lock hardware (Relay, LED, Reed Switch)
 * 
 * HARDWARE:
 *   - Relay (Solenoid): SECURELOCK_PIN_RELAY (Active HIGH to unlock)
 *   - Status LED: SECURELOCK_PIN_LED
 *   - Reed Switch: SECURELOCK_PIN_DOOR (INPUT_PULLUP, HIGH=Door Open)
 * 
 * FEATURES:
 *   - Non-blocking auto-lock timer (5 seconds)
 *   - Door state monitoring via reed switch
 *   - Status LED indicators (blinking, steady)
 *   - Secure state transitions
 * 
 * USAGE:
 *   LockManager lock;
 *   lock.init();
 *   lock.update();              // Call in loop()
 *   lock.unlock();              // Open door for 5 seconds
 *   bool state = lock.isLocked();
 *   bool door = lock.isDoorOpen();
 * 
 * ============================================================
 */

#ifndef LOCK_MANAGER_H
#define LOCK_MANAGER_H

#include <Arduino.h>
#include "hardware_pins.h"

class LockManager {
public:
    // Constructor
    LockManager();
    
    // Lifecycle
    void init();                    // Setup GPIO pins
    void update();                  // Update timers and LED (call in loop)
    
    // Lock control
    void unlock();                  // Unlock for 5 seconds
    void lock();                    // Lock immediately
    bool isLocked() const;          // Current lock state
    
    // Door monitoring
    bool isDoorOpen() const;        // Reed switch state (true = open)
    bool isDoorTampered() const;    // Door opened while locked?
    
    // LED control
    void setLEDBlink(bool enable);  // Blinking LED
    void setLEDSolid(bool on);      // Solid LED
    
    // Configuration
    void setAutoLockDelay(unsigned long delayMs);
    
private:
    // Hardware pins
    static const int PIN_RELAY = SECURELOCK_PIN_RELAY;
    static const int PIN_LED = SECURELOCK_PIN_LED;
    static const int PIN_DOOR = SECURELOCK_PIN_DOOR;
    
    // Timing
    static const unsigned long DEFAULT_UNLOCK_TIME = 5000;  // 5 seconds
    static const unsigned long LED_BLINK_INTERVAL = 500;    // 500ms
    
    // State variables
    bool _locked;
    bool _doorOpen;
    bool _tampered;
    unsigned long _unlockStartTime;
    unsigned long _autoLockDelay;
    bool _autoLockActive;
    
    // LED state
    bool _ledBlinkEnabled;
    bool _ledState;
    unsigned long _lastLEDBlink;
    
    // Private methods
    void _setRelay(bool energized);
    void _updateLED();
    void _updateDoorState();
};

#endif // LOCK_MANAGER_H
