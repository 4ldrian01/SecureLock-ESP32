/**
 * ============================================================
 * SecurityManager - Implementation
 * ============================================================
 */

#include "SecurityManager.h"

namespace {
inline bool isVibrationActiveLevel(int rawDigitalState) {
#if SECURELOCK_VIBRATION_ACTIVE_HIGH
    return rawDigitalState == HIGH;
#else
    return rawDigitalState == LOW;
#endif
}

constexpr int BUZZER_PRIORITY_IDLE = 0;
constexpr int BUZZER_PRIORITY_BEEP1 = 10;
constexpr int BUZZER_PRIORITY_TIMED = 15;
constexpr int BUZZER_PRIORITY_MODE = 18;
constexpr int BUZZER_PRIORITY_BEEP2 = 20;
constexpr int BUZZER_PRIORITY_BEEP3 = 30;
constexpr int BUZZER_PRIORITY_SIREN = 100;
}

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
        _sirenMode(false),
            _beepOnDuration(KEYPRESS_ON_MS),
            _beepOffDuration(KEYPRESS_OFF_MS),
            _lastFeedbackBeepMs(0),
            _lastPatternStartMs(0),
            _timedBuzzMode(false),
                _timedBuzzDurationMs(0),
                _customSequenceMode(false),
                _customSequencePriority(BUZZER_PRIORITY_IDLE),
                _customSequenceStep(0),
                _customSequenceLength(0)
{
}

/**
 * Initialize hardware
 */
void SecurityManager::init() {
    pinMode(PIN_BUZZER, OUTPUT);
    pinMode(PIN_VIBE, INPUT);

    // Establish current sensor baseline to avoid false first-edge strikes.
    _lastVibeState = isVibrationActiveLevel(digitalRead(PIN_VIBE));
    _stableVibeState = _lastVibeState;
    _lastVibeTime = millis();
    
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
    Serial.print("[SECURITY] Vibration polarity: ");
#if SECURELOCK_VIBRATION_ACTIVE_HIGH
    Serial.println("ACTIVE-HIGH (HIGH = strike)");
#else
    Serial.println("ACTIVE-LOW (LOW = strike)");
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
    const bool rawState = isVibrationActiveLevel(digitalRead(PIN_VIBE));

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
    _lastVibeState = isVibrationActiveLevel(digitalRead(PIN_VIBE));
    _stableVibeState = _lastVibeState;
}

/**
 * Beep buzzer N times
 */
void SecurityManager::beep(int count) {
    if (count <= 1) {
        beepKeyPress();
        return;
    }

    if (count == 2) {
        beepAccepted();
        return;
    }

    beepRejected();
}

void SecurityManager::beepKeyPress() {
    // Never interrupt active alarm siren feedback.
    if (_sirenMode) {
        return;
    }

    const unsigned long now = millis();
    const int count = 1;
    const int requestPriority = _priorityForBeepCount(count);
    const int activePriority = _activePatternPriority();

    // Keep high-rate lightweight UI feedback from sounding jittery/noisy.
    if ((now - _lastFeedbackBeepMs) < FEEDBACK_BEEP_COOLDOWN_MS) {
        return;
    }

    // Ignore weaker feedback while a stronger pattern is active.
    if (_buzzerActive && requestPriority < activePriority) {
        return;
    }

    // During timed buzz windows (guest-code generation), only medium/high
    // feedback may preempt.
    if (_timedBuzzMode && requestPriority <= BUZZER_PRIORITY_TIMED) {
        return;
    }

    // Suppress ultra-fast restarts of same/lower priority to avoid buzz chatter.
    if (_buzzerActive && _lastPatternStartMs > 0
        && (now - _lastPatternStartMs) < BEEP_RESTART_GUARD_MS
        && requestPriority <= activePriority) {
        return;
    }

    _startBeepPattern(1, KEYPRESS_ON_MS, KEYPRESS_OFF_MS, requestPriority);
}

void SecurityManager::beepAccepted() {
    if (_sirenMode) {
        return;
    }

    _startBeepPattern(2, ACCEPT_ON_MS, ACCEPT_OFF_MS, BUZZER_PRIORITY_BEEP2);
}

void SecurityManager::beepRejected() {
    if (_sirenMode) {
        return;
    }

    _startBeepPattern(1, REJECT_ON_MS, REJECT_OFF_MS, BUZZER_PRIORITY_BEEP3);
}

void SecurityManager::beepModeChange() {
    if (_sirenMode) {
        return;
    }

    const unsigned long sequence[] = {
        MODE_STEP1_ON_MS,
        MODE_STEP_GAP_MS,
        MODE_STEP2_ON_MS,
        MODE_STEP_GAP_MS,
        MODE_STEP3_ON_MS
    };

    _startCustomSequence(sequence, sizeof(sequence) / sizeof(sequence[0]), BUZZER_PRIORITY_MODE);
}

void SecurityManager::beepAlarmSiren() {
    siren();
}

void SecurityManager::buzzFor(unsigned long durationMs) {
    if (_sirenMode) {
        return;
    }

    if (durationMs == 0) {
        return;
    }

    if (durationMs > TIMED_BUZZ_MAX_MS) {
        durationMs = TIMED_BUZZ_MAX_MS;
    }

    const unsigned long now = millis();
    const int activePriority = _activePatternPriority();

    // Do not downgrade stronger active feedback (notably denial/error beeps).
    if (_buzzerActive && activePriority > BUZZER_PRIORITY_TIMED) {
        return;
    }

    _customSequenceMode = false;
    _customSequencePriority = BUZZER_PRIORITY_IDLE;
    _customSequenceStep = 0;
    _customSequenceLength = 0;

    _beepCount = 0;
    _currentBeep = 0;
    _buzzerActive = true;
    _timedBuzzMode = true;
    _timedBuzzDurationMs = durationMs;
    _buzzerStartTime = now;
    _lastPatternStartMs = now;
    _buzzerState = true;
    _setBuzzer(true);
    _lastFeedbackBeepMs = now;

    Serial.print("[SECURITY] Timed buzz ");
    Serial.print(durationMs);
    Serial.println("ms");
}

/**
 * Start continuous siren
 */
void SecurityManager::siren() {
    _beepCount = 0;
    _currentBeep = 0;
    _timedBuzzMode = false;
    _timedBuzzDurationMs = 0;
    _customSequenceMode = false;
    _customSequencePriority = BUZZER_PRIORITY_IDLE;
    _customSequenceStep = 0;
    _customSequenceLength = 0;
    _sirenMode = true;
    _buzzerActive = true;
    _buzzerStartTime = millis();
    _lastPatternStartMs = _buzzerStartTime;
    _buzzerState = true;
    _setBuzzer(true);
    
    Serial.println("[SECURITY] 🚨 SIREN ACTIVATED");
}

/**
 * Stop alarm/siren
 */
void SecurityManager::stopAlarm() {
    _beepCount = 0;
    _currentBeep = 0;
    _sirenMode = false;
    _buzzerActive = false;
    _timedBuzzMode = false;
    _timedBuzzDurationMs = 0;
    _customSequenceMode = false;
    _customSequencePriority = BUZZER_PRIORITY_IDLE;
    _customSequenceStep = 0;
    _customSequenceLength = 0;
    _lastPatternStartMs = 0;
    _buzzerStartTime = 0;
    _buzzerState = false;
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
    beepAlarmSiren();
    
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

bool SecurityManager::isBuzzerActive() const {
    return _buzzerActive;
}

bool SecurityManager::isSirenActive() const {
    return _sirenMode;
}

int SecurityManager::_priorityForBeepCount(int count) const {
    if (count >= 3) {
        return BUZZER_PRIORITY_BEEP3;
    }

    if (count == 2) {
        return BUZZER_PRIORITY_BEEP2;
    }

    return BUZZER_PRIORITY_BEEP1;
}

int SecurityManager::_activePatternPriority() const {
    if (_sirenMode) {
        return BUZZER_PRIORITY_SIREN;
    }

    if (_customSequenceMode) {
        return _customSequencePriority;
    }

    if (_timedBuzzMode) {
        return BUZZER_PRIORITY_TIMED;
    }

    if (_buzzerActive && _beepCount > 0) {
        return _priorityForBeepCount(_beepCount);
    }

    return BUZZER_PRIORITY_IDLE;
}

void SecurityManager::_startBeepPattern(int count, unsigned long onMs, unsigned long offMs, int priority) {
    if (count <= 0) {
        return;
    }

    const unsigned long now = millis();
    const int activePriority = _activePatternPriority();

    if (_buzzerActive && priority < activePriority) {
        return;
    }

    if (_timedBuzzMode && priority <= BUZZER_PRIORITY_TIMED) {
        return;
    }

    if (_buzzerActive && _lastPatternStartMs > 0
        && (now - _lastPatternStartMs) < BEEP_RESTART_GUARD_MS
        && priority <= activePriority) {
        return;
    }

    _timedBuzzMode = false;
    _timedBuzzDurationMs = 0;
    _customSequenceMode = false;
    _customSequencePriority = BUZZER_PRIORITY_IDLE;
    _customSequenceStep = 0;
    _customSequenceLength = 0;

    _beepOnDuration = onMs;
    _beepOffDuration = offMs;
    _beepCount = count;
    _currentBeep = 0;
    _buzzerActive = true;
    _buzzerStartTime = now;
    _lastPatternStartMs = now;
    _buzzerState = true;
    _setBuzzer(true);
    _lastFeedbackBeepMs = now;
}

void SecurityManager::_startCustomSequence(const unsigned long* steps, uint8_t stepCount, int priority) {
    if (steps == nullptr || stepCount == 0) {
        return;
    }

    if (stepCount > CUSTOM_SEQUENCE_MAX_STEPS) {
        stepCount = CUSTOM_SEQUENCE_MAX_STEPS;
    }

    const unsigned long now = millis();
    const int activePriority = _activePatternPriority();

    if (_buzzerActive && priority < activePriority) {
        return;
    }

    if (_timedBuzzMode && priority <= BUZZER_PRIORITY_TIMED) {
        return;
    }

    if (_buzzerActive && _lastPatternStartMs > 0
        && (now - _lastPatternStartMs) < BEEP_RESTART_GUARD_MS
        && priority <= activePriority) {
        return;
    }

    _timedBuzzMode = false;
    _timedBuzzDurationMs = 0;
    _beepCount = 0;
    _currentBeep = 0;

    for (uint8_t i = 0; i < stepCount; i++) {
        _customSequenceStepsMs[i] = steps[i];
    }

    _customSequenceMode = true;
    _customSequencePriority = priority;
    _customSequenceStep = 0;
    _customSequenceLength = stepCount;
    _buzzerActive = true;
    _buzzerStartTime = now;
    _lastPatternStartMs = now;
    _buzzerState = true;
    _setBuzzer(true);
    _lastFeedbackBeepMs = now;
}

void SecurityManager::_selectBeepDurations(int count) {
    if (count >= 3) {
        _beepOnDuration = REJECT_ON_MS;
        _beepOffDuration = REJECT_OFF_MS;
        return;
    }

    if (count == 2) {
        _beepOnDuration = ACCEPT_ON_MS;
        _beepOffDuration = ACCEPT_OFF_MS;
        return;
    }

    _beepOnDuration = KEYPRESS_ON_MS;
    _beepOffDuration = KEYPRESS_OFF_MS;
}

/**
 * Private: Update buzzer patterns (non-blocking)
 */
void SecurityManager::_updateBuzzer() {
    if (!_buzzerActive) return;
    
    unsigned long now = millis();
    unsigned long elapsed = now - _buzzerStartTime;

    if (_timedBuzzMode) {
        if (elapsed >= _timedBuzzDurationMs) {
            _beepCount = 0;
            _currentBeep = 0;
            _timedBuzzMode = false;
            _timedBuzzDurationMs = 0;
            _buzzerActive = false;
            _lastPatternStartMs = 0;
            _setBuzzer(false);
        }
        return;
    }

    if (_customSequenceMode) {
        if (_customSequenceStep >= _customSequenceLength) {
            _customSequenceMode = false;
            _customSequencePriority = BUZZER_PRIORITY_IDLE;
            _customSequenceStep = 0;
            _customSequenceLength = 0;
            _buzzerActive = false;
            _lastPatternStartMs = 0;
            _setBuzzer(false);
            return;
        }

        const unsigned long stepDuration = _customSequenceStepsMs[_customSequenceStep];
        if (elapsed >= stepDuration) {
            _customSequenceStep++;
            _buzzerStartTime = now;

            if (_customSequenceStep >= _customSequenceLength) {
                _customSequenceMode = false;
                _customSequencePriority = BUZZER_PRIORITY_IDLE;
                _customSequenceStep = 0;
                _customSequenceLength = 0;
                _buzzerActive = false;
                _lastPatternStartMs = 0;
                _setBuzzer(false);
            } else {
                _buzzerState = !_buzzerState;
                _setBuzzer(_buzzerState);
            }
        }
        return;
    }
    
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
        if (_buzzerState && elapsed >= _beepOnDuration) {
            _setBuzzer(false);
            _buzzerState = false;
            _buzzerStartTime = now;
            _currentBeep++;
        }
        // OFF phase (pause between beeps)
        else if (!_buzzerState && elapsed >= _beepOffDuration) {
            if (_currentBeep < _beepCount) {
                _setBuzzer(true);
                _buzzerState = true;
                _buzzerStartTime = now;
            }
        }
    } else {
        // Pattern complete
        _beepCount = 0;
        _currentBeep = 0;
        _buzzerActive = false;
        _lastPatternStartMs = 0;
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
