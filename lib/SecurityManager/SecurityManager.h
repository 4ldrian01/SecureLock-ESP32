/**
 * ============================================================
 * SecurityManager - Sensor & Alarm Component
 * ============================================================
 * 
 * PURPOSE: Manages security sensors and alarm outputs
 * 
 * HARDWARE:
 *   - Vibration Sensor (SW-420): GPIO 27 (Digital input)
 *   - Active Buzzer: GPIO 14 (PWM capable)
 * 
 * FEATURES:
 *   - Vibration detection with debouncing
 *   - Multi-pattern buzzer (beep, siren)
 *   - Alarm state management
 *   - Non-blocking alarm patterns
 * 
 * BUZZER PATTERNS:
 *   - beep(1): Single beep (keypress/RFID scan)
 *   - beep(2): Double beep (access granted)
 *   - beep(3): Triple beep (access denied)
 *   - siren(): Continuous pulsing (alarm mode)
 * 
 * USAGE:
 *   SecurityManager security;
 *   security.init();
 *   security.update();                    // Call in loop()
 *   security.beep(2);                     // Access granted beep
 *   if (security.isVibrationDetected()) { // Check vibration
 *       security.startAlarm();
 *   }
 * 
 * ============================================================
 */

#ifndef SECURITY_MANAGER_H
#define SECURITY_MANAGER_H

#include <Arduino.h>
#include "hardware_pins.h"

class SecurityManager {
public:
    // Constructor
    SecurityManager();
    
    // Lifecycle
    void init();                    // Setup GPIO pins
    void update();                  // Update buzzer patterns (call in loop)
    
    // Vibration detection
    bool isVibrationDetected();     // Check sensor with debouncing
    void resetVibration();          // Clear vibration flag
    
    // Buzzer control
    void beep(int count);           // 1=keypress, 2=granted, 3=denied
    void siren();                   // Start continuous alarm
    void stopAlarm();               // Stop alarm/siren
    bool isAlarming() const;        // Check alarm state
    
    // Alarm management
    void startAlarm();              // Trigger full alarm (siren + flag)
    void clearAlarm();              // Clear alarm state
    
private:
    // Hardware pins
    static const int PIN_BUZZER = SECURELOCK_PIN_BUZZER;
    static const int PIN_VIBE = SECURELOCK_PIN_VIBRATION;
    
    // Timing constants
    static const unsigned long BEEP_DURATION = 100;      // ms
    static const unsigned long BEEP_PAUSE = 80;          // ms between beeps
    static const unsigned long SIREN_PULSE = 200;        // ms per siren pulse
    static const unsigned long VIBE_DEBOUNCE = 50;       // ms edge debounce
    static const unsigned long VIBE_CONFIRM_HIGH_MS = 120; // ms stable HIGH required
    static const unsigned long VIBE_RETRIGGER_COOLDOWN_MS = 1500;
    
    // State variables
    bool _alarming;
    bool _vibrationDetected;
    unsigned long _lastVibeTime;
    unsigned long _vibeHighSince;
    unsigned long _lastVibrationTriggerMs;
    bool _lastVibeState;
    
    // Buzzer pattern state
    bool _buzzerActive;
    int _beepCount;
    int _currentBeep;
    unsigned long _buzzerStartTime;
    bool _buzzerState;
    bool _sirenMode;
    
    // Private methods
    void _updateBuzzer();
    void _setBuzzer(bool on);
};

#endif // SECURITY_MANAGER_H
