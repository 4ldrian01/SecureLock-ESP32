# SecureLock - AI Agent Instructions

## Project Overview
SecureLock is an **ESP32-based smart security system** with component-based architecture. It's a PlatformIO project (not Arduino IDE) that combines hardware control (solenoid lock, RFID, keypad) with a web dashboard served from LittleFS filesystem.

## Architecture: Component-Based Design

The codebase follows a **4-component modular architecture** where each component is a separate library in `lib/`:

### Core Components (Read headers first to understand interfaces)
1. **LockManager** (`lib/LockManager/`) - Hardware control layer
   - Controls relay (GPIO 22), LED (GPIO 2), reed switch (GPIO 13)
   - Non-blocking auto-lock timer (5 seconds default)
   - Door tamper detection via reed switch

2. **SecurityManager** (`lib/SecurityManager/`) - Sensors & alarms
   - Vibration sensor (SW-420 on GPIO 27)
   - Active buzzer (GPIO 14) with patterns: beep(1-3), siren()
   - Alarm state management

3. **AuthHandler** (`lib/AuthHandler/`) - Multi-factor authentication
   - RFID RC522 (SPI: SS=5, RST=4, SCK=18, MOSI=25, MISO=19)
   - 4x4 Matrix Keypad (Rows[34,35,39,36], Cols[16,17,21,23], where 16=RX2 and 17=TX2 on many boards)
   - Duress code detection (9999 = silent alarm)
   - Factory reset via GPIO 0 (BOOT button held 10s)

4. **WebServer** (`lib/WebServer/`) - Network & API layer
   - ESPAsyncWebServer (async, non-blocking)
   - LittleFS file serving (`data/` folder)
   - RESTful API: `/api/status`, `/api/unlock`, `/api/guest-code`, `/api/users`, `/api/logs`
   - Depends on LockManager, SecurityManager, and AuthHandler for full system integration

### Component Dependencies
- `main.cpp` orchestrates all components (minimal glue code)
- WebServer takes references to LockManager, SecurityManager, & AuthHandler in constructor
- Components are **independent** except WebServer queries others for status
- All components have `.init()` and `.update()` methods called from main loop
- Telegram bot integration in main.cpp sends security alerts (vibration, tamper, duress)

## Critical Development Workflows

### Build & Deploy (PowerShell on Windows)
```powershell
# Clean build using helper script
.\build.ps1

# OR manual PlatformIO commands
pio run

# Upload filesystem (web dashboard files in data/)
pio run --target uploadfs

# Upload firmware
pio run --target upload

# Monitor serial output
pio device monitor

# Complete deployment workflow
pio run --target uploadfs && pio run --target upload && pio device monitor
```

### Two-Stage Deployment Pattern
1. **Filesystem Upload** (`--target uploadfs`) - Uploads `data/` folder to LittleFS
   - Required when HTML/CSS/JS/JSON files change
   - Files: `data/html/index.html`, `data/css/style.css`, `data/js/script.js`, `data/users.json`, `data/logs.json`

2. **Firmware Upload** (`--target upload`) - Compiles and uploads C++ code
   - Required when `.cpp`/`.h` files change in `src/`, `lib/`, `include/`

### Configuration Before Build
**ALWAYS check** `include/secrets.h` before deployment:
```cpp
#define WIFI_SSID           "YOUR_WIFI_SSID"
#define WIFI_PASSWORD       "YOUR_WIFI_PASSWORD"
#define BOT_TOKEN           "1234567890:ABCdefGHI..."  // Get from @BotFather
#define ADMIN_CHAT_ID       "123456789"              // Get from @userinfobot
```

## Project-Specific Conventions

### Hardware Pin Mapping (Critical - Don't change without hardware modification)
```cpp
// Defined in each component's .h file as static const int
RELAY_PIN = 22     // Solenoid lock (Active HIGH = unlocked)
LED_PIN = 2        // Onboard LED
DOOR_PIN = 13      // Reed switch (INPUT_PULLUP, HIGH = door open)
BUZZER_PIN = 14    // Active buzzer
VIBE_PIN = 27      // Vibration sensor (SW-420)
RFID_SS = 5        // RFID chip select
RFID_RST = 4       // RFID reset
BOOT_PIN = 0       // Factory reset button (builtin)
```

### Non-Blocking Patterns
- **Always use millis() for timing**, never delay()
- Example in LockManager: auto-lock timer checks `millis() - _unlockStartTime >= _autoLockDelay`
- SecurityManager buzzer patterns use state machine with `_buzzerStartTime`
- WebServer is fully async (ESPAsyncWebServer) - no update() method needed

### Authentication Flow (Multi-stage)
```cpp
// 1. RFID scan (in main.cpp loop)
AuthResult rfidResult = authHandler.checkRFID();
if (rfidResult == AUTH_SUCCESS) {
    systemArmed = false;  // Disarm before PIN
    securityManager.beep(1);  // Feedback beep
}

// 2. Keypad entry (buffered)
char key = authHandler.getKeypadKey();
if (key == '#') {  // Submit PIN
    AuthResult pinResult = authHandler.validatePIN();
    if (pinResult == AUTH_SUCCESS) {
        lockManager.unlock();  // Grants 5-second access
    } else if (pinResult == AUTH_DURESS) {
        lockManager.unlock();  // Unlock but trigger silent alarm
        // Send Telegram notification in production
    }
}
```

### WebServer File Serving Pattern
- LittleFS mounted at root `/`
- Files served from `data/` folder: `/html/index.html` → root URL `/`
- MIME types detected in `WebServer::_getMimeType()`: `.html`, `.css`, `.js`, `.json`, `.png`, `.ico`
- CORS headers enabled for development: `Access-Control-Allow-Origin: *`

### Full-Stack Communication Flow
```
Frontend (JavaScript) → API Endpoints → WebServer Component → Hardware Components
                ↓                                                      ↓
    Fetch /api/status every 3s                          Query LockManager/SecurityManager
                ↓                                                      ↓
    Update UI (lock state, alarm)                       Return JSON with real-time status
```

**Critical**: Hardware events (vibration, door tamper, duress code) trigger Telegram notifications via UniversalTelegramBot library in main.cpp

### Telegram Bot Integration
- Sends real-time security alerts to admin
- Vibration detection → Intrusion alert
- Door tamper → Unauthorized access alert
- Duress code (9999) → Silent alarm (appears normal to attacker)
- Initialize after WiFi connection with `telegramClient.setInsecure()` for testing

## Common Pitfalls & Solutions

### Build Errors
- **"LittleFS.h not found"**: Requires ESP32 platform, check `platformio.ini` has `platform = espressif32`
- **Library conflicts**: Clean build with `.\build.ps1` (removes `.pio` folder)
- **Upload fails**: Check USB connection, ESP32 port auto-detected but verify with `pio device list`

### Runtime Issues
- **Web dashboard not loading**: Run `pio run --target uploadfs` BEFORE `upload`
- **WiFi not connecting**: Verify `secrets.h` credentials, check serial monitor for connection status
- **Sensors not responding**: Component `.init()` must be called in `setup()` before `.update()` in `loop()`

## Key Files to Reference

### For Architecture Understanding
- [ARCHITECTURE_SUMMARY.md](../ARCHITECTURE_SUMMARY.md) - Component diagram and design rationale
- [IMPLEMENTATION_SUMMARY.md](../IMPLEMENTATION_SUMMARY.md) - API endpoints and integration details

### For Deployment
- [DEPLOYMENT_GUIDE.md](../DEPLOYMENT_GUIDE.md) - Step-by-step production deployment
- [QUICK_REFERENCE.txt](../QUICK_REFERENCE.txt) - Command cheat sheet

### For Hardware Setup
- [platformio.ini](../platformio.ini) - Lines 1-30 have complete GPIO pin mapping and feature list
- Component headers (`lib/*/` folders) - Each has hardware pinout documentation

## When Adding New Features

1. **New hardware peripheral**: Create new component in `lib/ComponentName/` with `.h` and `.cpp`
2. **New API endpoint**: Add handler in `WebServer.cpp` `_setupRoutes()` method
3. **New authentication method**: Extend `AuthHandler` and add to `AuthResult` enum
4. **New web dashboard page**: Add file to `data/html/` and update WebServer routing

## Dependencies (From platformio.ini)
```ini
ESPAsyncWebServer ^1.2.4   # Async HTTP server
AsyncTCP ^1.1.1            # TCP for ESP32
ArduinoJson ^7.0.4         # JSON serialization
MFRC522 ^1.4.11            # RFID reader
Keypad ^3.1.1              # Matrix keypad
UniversalTelegramBot ^1.3.0 # Telegram API
```
