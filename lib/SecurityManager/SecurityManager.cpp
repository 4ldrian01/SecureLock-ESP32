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
      _lastVibeState(LOW),
            _stableVibeState(LOW),
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
    pollVibrationStrike();
    return _vibrationDetected;
}

bool SecurityManager::pollVibrationStrike() {
    const unsigned long now = millis();
    const bool rawState = (digitalRead(PIN_VIBE) == HIGH);

    if (rawState != _lastVibeState) {
        _lastVibeTime = now;
        _lastVibeState = rawState;
    }

    if ((now - _lastVibeTime) < VIBE_DEBOUNCE) {
        return false;
    }

    if (_stableVibeState == _lastVibeState) {
        return false;
    }

    _stableVibeState = _lastVibeState;
    if (_stableVibeState) {
        _vibrationDetected = true;
        return true;
    }

    return false;
}

bool SecurityManager::isVibrationLatched() const {
    return _vibrationDetected;
}

/**
 * Reset vibration flag
 */
void SecurityManager::resetVibration() {
    _vibrationDetected = false;
    _lastVibeState = (digitalRead(PIN_VIBE) == HIGH);
    _stableVibeState = _lastVibeState;
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
