/**
 * ============================================================
 * SecurityManager - Sensor & Alarm Component
 * ============================================================
 * 
 * PURPOSE: Manages security sensors and alarm outputs
 * 
 * HARDWARE:
 *   - Vibration Sensor (SW-420): GPIO 34 (Digital input, input-only pin)
 *   - Active Buzzer: GPIO 14 (PWM capable)
 * 
 * FEATURES:
 *   - Vibration detection with debouncing
 *   - Enterprise audio profiles (keypress/accepted/rejected/mode/alarm)
 *   - Alarm state management
 *   - Non-blocking alarm patterns
 * 
 * BUZZER PROFILES:
 *   - beepKeyPress(): short ~50ms chirp
 *   - beepAccepted(): two rapid ~100ms beeps
 *   - beepRejected(): one long ~800ms denial beep
 *   - beepModeChange(): ascending non-blocking sequence
 *   - beepAlarmSiren(): continuous pulsing alarm tone
 * 
 * USAGE:
 *   SecurityManager security;
 *   security.init();
 *   security.update();                    // Call in loop()
 *   security.beepAccepted();              // Access granted profile
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
    bool isVibrationDetected();     // Latched event indicator
    bool pollVibrationStrike();     // Rising-edge strike event (debounced)
    bool isVibrationLatched() const;
    void resetVibration();          // Clear vibration flag
    
    // Buzzer control
    void beep(int count);           // Backward-compatible shim (maps to profile methods)
    void beepKeyPress();            // Profile: keypad keypress chirp
    void beepAccepted();            // Profile: access granted
    void beepRejected();            // Profile: access denied
    void beepModeChange();          // Profile: keypad mode change (ascending sequence)
    void beepAlarmSiren();          // Profile: alarm siren
    void buzzFor(unsigned long durationMs); // Continuous tone for exact duration (non-blocking)
    void siren();                   // Start continuous alarm
    void stopAlarm();               // Stop alarm/siren
    bool isAlarming() const;        // Check alarm state
    
    // Alarm management
    void startAlarm();              // Trigger full alarm (siren + flag)
    void clearAlarm();              // Clear alarm state
    bool isBuzzerActive() const;
    bool isSirenActive() const;
    
private:
    // Hardware pins
    static const int PIN_BUZZER = SECURELOCK_PIN_BUZZER;
    static const int PIN_VIBE = SECURELOCK_PIN_VIBRATION;
    
    // Timing constants
    static const unsigned long SIREN_PULSE = 95;         // ms per siren pulse (rapid pulse profile)
    static const unsigned long VIBE_DEBOUNCE = 50;       // ms strict debounce for vibration edges
    // Enterprise profile timings (active buzzer).
    static const unsigned long KEYPRESS_ON_MS = 50;
    static const unsigned long KEYPRESS_OFF_MS = 55;
    static const unsigned long ACCEPT_ON_MS = 100;
    static const unsigned long ACCEPT_OFF_MS = 90;
    static const unsigned long REJECT_ON_MS = 800;
    static const unsigned long REJECT_OFF_MS = 80;
    static const unsigned long MODE_STEP1_ON_MS = 40;
    static const unsigned long MODE_STEP_GAP_MS = 35;
    static const unsigned long MODE_STEP2_ON_MS = 85;
    static const unsigned long MODE_STEP3_ON_MS = 130;
    static const unsigned long FEEDBACK_BEEP_COOLDOWN_MS = 35;
    static const unsigned long BEEP_RESTART_GUARD_MS = 25;
    static const unsigned long TIMED_BUZZ_MAX_MS = 5000;
    
    // State variables
    bool _alarming;
    bool _vibrationDetected;
    unsigned long _lastVibeTime;
    bool _lastVibeState;
    bool _stableVibeState;
    
    // Buzzer pattern state
    bool _buzzerActive;
    int _beepCount;
    int _currentBeep;
    unsigned long _buzzerStartTime;
    bool _buzzerState;
    bool _sirenMode;
    unsigned long _beepOnDuration;
    unsigned long _beepOffDuration;
    unsigned long _lastFeedbackBeepMs;
    unsigned long _lastPatternStartMs;
    bool _timedBuzzMode;
    unsigned long _timedBuzzDurationMs;
    bool _customSequenceMode;
    int _customSequencePriority;
    uint8_t _customSequenceStep;
    uint8_t _customSequenceLength;
    static const uint8_t CUSTOM_SEQUENCE_MAX_STEPS = 8;
    unsigned long _customSequenceStepsMs[CUSTOM_SEQUENCE_MAX_STEPS];
    
    // Private methods
    int _priorityForBeepCount(int count) const;
    int _activePatternPriority() const;
    void _startBeepPattern(int count, unsigned long onMs, unsigned long offMs, int priority);
    void _startCustomSequence(const unsigned long* steps, uint8_t stepCount, int priority);
    void _selectBeepDurations(int count);
    void _updateBuzzer();
    void _setBuzzer(bool on);
};

#endif // SECURITY_MANAGER_H
