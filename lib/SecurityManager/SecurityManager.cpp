/**
 * ============================================================
 * SecurityManager - Implementation
 * ============================================================
 */

#include "SecurityManager.h"

/**
 * Constructor
 */
SecurityManager::SecurityManager()
    : _alarming(false),
      _vibrationDetected(false),
      _lastVibeTime(0),
    _vibeHighSince(0),
    _lastVibrationTriggerMs(0),
      _lastVibeState(LOW),
      _buzzerActive(false),
      _beepCount(0),
      _currentBeep(0),
      _buzzerStartTime(0),
      _buzzerState(false),
      _sirenMode(false)
{
}

/**
 * Initialize hardware
 */
void SecurityManager::init() {
    pinMode(PIN_BUZZER, OUTPUT);
    pinMode(PIN_VIBE, INPUT);
    
    _setBuzzer(false);
    
    Serial.println("[SECURITY] Initialized");
    Serial.print("[SECURITY] Vibration sensor GPIO: ");
    Serial.println(PIN_VIBE);
    Serial.print("[SECURITY] Buzzer GPIO: ");
    Serial.println(PIN_BUZZER);
    Serial.print("[SECURITY] Buzzer polarity: ");
#if SECURELOCK_BUZZER_ACTIVE_HIGH
    Serial.println("ACTIVE-HIGH (HIGH = ON)");
#else
    Serial.println("ACTIVE-LOW (LOW = ON)");
#endif
}

/**
 * Update state - Call in loop()
 */
void SecurityManager::update() {
    _updateBuzzer();
}

/**
 * Check vibration sensor with debouncing
 */
bool SecurityManager::isVibrationDetected() {
    unsigned long now = millis();
    bool currentState = digitalRead(PIN_VIBE);

    if (currentState == HIGH) {
        if (_lastVibeState == LOW) {
            _lastVibeTime = now;
            _vibeHighSince = now;
        }

        const bool edgeDebounced = (now - _lastVibeTime) >= VIBE_DEBOUNCE;
        const bool stableHigh = (now - _vibeHighSince) >= VIBE_CONFIRM_HIGH_MS;
        const bool cooldownElapsed = (now - _lastVibrationTriggerMs) >= VIBE_RETRIGGER_COOLDOWN_MS;

        // Trigger only on confirmed stable vibration level with cooldown to avoid chatter spikes.
        if (!_vibrationDetected && edgeDebounced && stableHigh && cooldownElapsed) {
            _vibrationDetected = true;
            _lastVibrationTriggerMs = now;
            Serial.println("[SECURITY] ⚠️ VIBRATION DETECTED (filtered)");
        }
    } else {
        _vibeHighSince = 0;
    }

    _lastVibeState = currentState;
    return _vibrationDetected;
}

/**
 * Reset vibration flag
 */
void SecurityManager::resetVibration() {
    _vibrationDetected = false;
    _vibeHighSince = 0;
    _lastVibeState = digitalRead(PIN_VIBE);
}

/**
 * Beep buzzer N times
 */
void SecurityManager::beep(int count) {
    if (_sirenMode) return;  // Don't interrupt siren
    
    _beepCount = count;
    _currentBeep = 0;
    _buzzerActive = true;
    _buzzerStartTime = millis();
    _buzzerState = true;
    _setBuzzer(true);
    
    Serial.print("[SECURITY] Beep x");
    Serial.println(count);
}

/**
 * Start continuous siren
 */
void SecurityManager::siren() {
    _sirenMode = true;
    _buzzerActive = true;
    _buzzerStartTime = millis();
    _buzzerState = true;
    _setBuzzer(true);
    
    Serial.println("[SECURITY] 🚨 SIREN ACTIVATED");
}

/**
 * Stop alarm/siren
 */
void SecurityManager::stopAlarm() {
    _sirenMode = false;
    _buzzerActive = false;
    _alarming = false;
    _setBuzzer(false);
    
    Serial.println("[SECURITY] Alarm stopped");
}

/**
 * Check if currently alarming
 */
bool SecurityManager::isAlarming() const {
    return _alarming;
}

/**
 * Start full alarm (siren + flag)
 */
void SecurityManager::startAlarm() {
    _alarming = true;
    siren();
    
    Serial.println("[SECURITY] 🚨 ALARM TRIGGERED!");
}

/**
 * Clear alarm state
 */
void SecurityManager::clearAlarm() {
    stopAlarm();
    resetVibration();
    
    Serial.println("[SECURITY] Alarm cleared");
}

/**
 * Private: Update buzzer patterns (non-blocking)
 */
void SecurityManager::_updateBuzzer() {
    if (!_buzzerActive) return;
    
    unsigned long now = millis();
    unsigned long elapsed = now - _buzzerStartTime;
    
    // Siren mode - continuous pulsing
    if (_sirenMode) {
        if (elapsed >= SIREN_PULSE) {
            _buzzerStartTime = now;
            _buzzerState = !_buzzerState;
            _setBuzzer(_buzzerState);
        }
        return;
    }
    
    // Beep pattern
    if (_currentBeep < _beepCount) {
        // ON phase
        if (_buzzerState && elapsed >= BEEP_DURATION) {
            _setBuzzer(false);
            _buzzerState = false;
            _buzzerStartTime = now;
            _currentBeep++;
        }
        // OFF phase (pause between beeps)
        else if (!_buzzerState && elapsed >= BEEP_PAUSE) {
            if (_currentBeep < _beepCount) {
                _setBuzzer(true);
                _buzzerState = true;
                _buzzerStartTime = now;
            }
        }
    } else {
        // Pattern complete
        _buzzerActive = false;
        _setBuzzer(false);
    }
}

/**
 * Private: Set buzzer state
 */
void SecurityManager::_setBuzzer(bool on) {
#if SECURELOCK_BUZZER_ACTIVE_HIGH
    digitalWrite(PIN_BUZZER, on ? HIGH : LOW);
#else
    digitalWrite(PIN_BUZZER, on ? LOW : HIGH);
#endif
}
