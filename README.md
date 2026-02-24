# 🔐 SecureLock - ESP32 Smart Security System

**Version**: 2.0  
**Platform**: ESP32 DevKit V1 (30-Pin)  
**Architecture**: Component-Based Modular Design  
**Framework**: PlatformIO (Arduino)

---

## 📋 Table of Contents

- [Overview](#overview)
- [Features](#features)
- [Hardware Requirements](#hardware-requirements)
- [Quick Start](#quick-start)
- [Architecture](#architecture)
- [API Documentation](#api-documentation)
- [Web Dashboard](#web-dashboard)
- [Development](#development)
- [Deployment](#deployment)
- [Troubleshooting](#troubleshooting)
- [License](#license)

---

## 🎯 Overview

SecureLock is a **production-grade IoT security system** built on ESP32 with a glassmorphism web dashboard. It combines multi-factor authentication (RFID + PIN), real-time Telegram alerts, and a responsive web interface for complete access control and user management.

### Key Capabilities

- **Multi-Factor Authentication**: RFID RC522 + 4x4 Matrix Keypad
- **Duress Code**: Silent alarm (9999) for emergency situations
- **Tamper Detection**: Vibration sensor + Reed switch monitoring
- **Web Dashboard**: Real-time control via glassmorphism UI
- **User Management**: Add, edit, and delete users via web interface with RFID enrollment
- **Telegram Notifications**: Instant security alerts (intrusion, tamper, duress)
- **Guest Access**: Temporary 5-minute PIN codes
- **Auto-Lock**: 5-second timer after unlock

---

## ✨ Features

### 🔒 Security Features
- ✅ Multi-factor authentication (RFID + PIN)
- ✅ Duress code detection (9999 = silent alarm)
- ✅ Vibration-based intrusion detection (SW-420)
- ✅ Door tamper detection (reed switch)
- ✅ Real-time Telegram security alerts
- ✅ Activity logging with timestamps
- ✅ Factory reset (BOOT button held 10s)

### 🌐 Web Dashboard
- ✅ Responsive glassmorphism dark-theme design
- ✅ Real-time lock status polling (3s interval)
- ✅ Emergency override with confirmation modal
- ✅ Guest code generation (5-minute expiry)
- ✅ Activity logs viewer (table format)
- ✅ Full user management (CRUD + RFID enrollment)

### 🔧 Technical Features
- ✅ Component-based modular architecture (4 libraries)
- ✅ Fully non-blocking (millis-based, async web server)
- ✅ RESTful API (9 endpoints with CORS support)
- ✅ LittleFS filesystem for web assets and data
- ✅ NVS persistence for user credentials
- ✅ Low memory footprint (~35% RAM)

---

## 🛠️ Hardware Requirements

### Core Components

| Component | Model | GPIO | Purpose |
|-----------|-------|------|---------|
| **Microcontroller** | ESP32 DevKit V1 | - | Main controller |
| **Solenoid Lock** | 12V DC | 4 (Relay) | Physical lock |
| **RFID Reader** | RC522 | SPI (5, 21) | Card authentication |
| **Keypad** | 4x4 Matrix | 32-36, 39 | PIN entry |
| **Vibration Sensor** | SW-420 | 27 | Intrusion detection |
| **Reed Switch** | - | 13 | Door open/close |
| **Active Buzzer** | - | 14 | Audio feedback |
| **Status LED** | Onboard | 2 | Visual indicator |

### Pin Mapping (ESP32 DevKit V1)

```
GPIO 4  → Relay Module (Solenoid Lock) - Active HIGH = Unlocked
GPIO 2  → Onboard LED (Status Indicator)
GPIO 14 → Active Buzzer (Audio Feedback)
GPIO 27 → SW-420 Vibration Sensor (Intrusion Detection)
GPIO 13 → Reed Switch (Door Open/Close) - INPUT_PULLUP

RFID RC522 (SPI):
  SS   = GPIO 5
  SCK  = GPIO 18
  MOSI = GPIO 23
  MISO = GPIO 19
  RST  = GPIO 21

Keypad 4x4 Matrix:
  Rows    = GPIO 32, 33, 25, 26
  Columns = GPIO 34, 35, 36, 39

Factory Reset:
  GPIO 0 (BOOT button) - Hold 10 seconds
```

---

## ⚡ Quick Start

### Prerequisites

```powershell
# Install PlatformIO
pip install platformio

# Verify ESP32 connection
pio device list
```

### 1. Configure Credentials

Edit `include/secrets.h`:

```cpp
#define WIFI_SSID           "YourWiFiNetwork"
#define WIFI_PASSWORD       "YourWiFiPassword"
#define BOT_TOKEN           "1234567890:ABCdefGHI..."  // From @BotFather
#define ADMIN_CHAT_ID       "123456789"              // From @userinfobot
```

### 2. Upload Filesystem (Web Dashboard)

```powershell
pio run --target uploadfs
```

### 3. Upload Firmware

```powershell
pio run --target upload
```

### 4. Monitor Serial Output

```powershell
pio device monitor
```

### 5. Access Dashboard

Open browser: `http://[ESP32_IP]/`

---

## 🏗️ Architecture

### Component-Based Design

```
SecureLock v2.0
│
├── LockManager          [Hardware Control Layer]
│   ├── Relay (GPIO 4)        — Solenoid lock, active HIGH
│   ├── LED (GPIO 2)          — Onboard status indicator
│   └── Reed Switch (GPIO 13) — Door open/close detection
│
├── SecurityManager      [Sensor & Alarm Layer]
│   ├── Vibration Sensor (GPIO 27) — SW-420 intrusion detection
│   └── Active Buzzer (GPIO 14)    — Feedback patterns (beep, siren)
│
├── AuthHandler          [Authentication Layer]
│   ├── RFID RC522 (SPI)      — Card-based authentication
│   ├── 4x4 Matrix Keypad     — PIN entry
│   ├── Duress Detection       — Code 9999 triggers silent alarm
│   └── NVS User Storage       — Persistent user credentials
│
└── WebServer            [Network Layer]
    ├── WiFi Manager           — Auto-connect with status reporting
    ├── LittleFS File Server   — Serves HTML/CSS/JS/JSON
    ├── RESTful API            — 9 endpoints with CORS support
    └── Activity Logging       — Writes events to logs.json
```

### File Structure

```
SecureLock/
├── data/                       # Web Dashboard (uploaded to LittleFS)
│   ├── html/index.html         #   Dashboard UI
│   ├── css/style.css           #   Glassmorphism theme
│   ├── js/script.js            #   Frontend controller (API client)
│   ├── users.json              #   User data store
│   └── logs.json               #   Activity log store
│
├── lib/                        # Component Libraries
│   ├── LockManager/            #   Relay, LED, reed switch control
│   ├── SecurityManager/        #   Vibration sensor, buzzer patterns
│   ├── AuthHandler/            #   RFID, keypad, duress, NVS users
│   └── WebServer/              #   Async HTTP server, API routes
│
├── src/
│   └── main.cpp                # Orchestrator + Telegram integration
│
├── include/
│   ├── secrets.h               # WiFi & Telegram credentials (gitignored)
│   └── secrets.h.example       # Credential template for setup
│
├── .github/
│   └── copilot-instructions.md # AI agent guidance
│
├── platformio.ini              # Build configuration & dependencies
├── build.ps1                   # PowerShell build helper script
└── README.md
```

---

## 📡 API Documentation

### Base URL: `http://[ESP32_IP]/api/`

All endpoints return JSON and include CORS headers for cross-origin access.

### Endpoints Summary

| Method | Endpoint | Description |
|--------|----------|-------------|
| `GET` | `/api/status` | Real-time system status |
| `POST` | `/api/unlock` | Emergency door unlock |
| `POST` | `/api/guest-code` | Generate temporary PIN |
| `GET` | `/api/users` | List registered users |
| `POST` | `/api/users` | Add new user (with RFID + PIN) |
| `PUT` | `/api/users` | Edit existing user |
| `DELETE` | `/api/users?uid=XX` | Remove user |
| `GET` | `/api/logs` | Retrieve activity logs |
| `GET` | `/api/rfid/scan` | Poll RFID reader for card tap |

---

#### `GET /api/status`
Returns real-time system status including lock state, sensor readings, and network info.

```json
{
  "locked": true,
  "doorOpen": false,
  "tampered": false,
  "alarm": false,
  "vibration": false,
  "uptime": 12345,
  "freeHeap": 234567,
  "wifiConnected": true,
  "ipAddress": "192.168.1.100",
  "rssi": -45
}
```

#### `POST /api/unlock`
Triggers emergency door unlock (5-second auto-lock timer).

```json
{
  "success": true,
  "message": "Emergency unlock activated",
  "timestamp": 123456789
}
```

#### `POST /api/guest-code`
Generates a temporary 4-digit PIN valid for 5 minutes.

```json
{
  "success": true,
  "code": "4821",
  "expiresIn": 300,
  "timestamp": 123456789
}
```

#### `GET /api/users`
Lists all registered users from `users.json`.

```json
{
  "users": [
    { "uid": "DEADBEEF", "name": "Admin", "type": "admin" }
  ]
}
```

#### `POST /api/users`
Add a new user. Requires JSON body with `name`, `pin`, and `uid` fields. Persists to NVS and `users.json`.

**Request Body**:
```json
{
  "name": "John Doe",
  "pin": "1234",
  "uid": "AABBCCDD"
}
```

**Response**:
```json
{
  "success": true,
  "message": "User added successfully"
}
```

#### `PUT /api/users`
Edit an existing user. Requires JSON body with `uid` (identifier) plus fields to update (`name`, `pin`, `newUid`).

**Request Body**:
```json
{
  "uid": "AABBCCDD",
  "name": "Jane Doe",
  "pin": "5678",
  "newUid": "11223344"
}
```

**Response**:
```json
{
  "success": true,
  "message": "User updated successfully"
}
```

#### `DELETE /api/users?uid=AABBCCDD`
Remove a user by UID. Admin user cannot be deleted.

```json
{
  "success": true,
  "message": "User deleted successfully"
}
```

#### `GET /api/logs`
Retrieve activity logs from `logs.json`.

```json
{
  "logs": [
    { "time": "12:30:00", "user": "Admin", "method": "RFID", "status": "granted" }
  ]
}
```

#### `GET /api/rfid/scan`
Poll the RFID reader for a newly scanned card. Used by the web dashboard during user enrollment.

```json
{
  "scanned": true,
  "uid": "AABBCCDD"
}
```

If no card detected:
```json
{
  "scanned": false
}
```

---

## 🌐 Web Dashboard

The web dashboard is a single-page application served from LittleFS with a glassmorphism dark theme.

### Sections
- **Lock Status** — Animated lock visual with real-time state (locked/unlocked)
- **Emergency Override** — One-click unlock with confirmation modal
- **Guest Access** — Generate temporary 4-digit codes with countdown timer
- **Activity Logs** — Table view of recent access events
- **User Management** — Add, edit, delete users with live RFID card enrollment

### Communication Flow
```
Browser (JavaScript)  →  REST API Calls  →  WebServer Component  →  Hardware Components
       ↓                                                                    ↓
  Poll /api/status (3s)                                     Query LockManager/SecurityManager
       ↓                                                                    ↓
  Update UI elements                                        Return JSON with real-time data
```

---

## 💻 Development

### Build Commands

```powershell
# Clean build (removes .pio cache)
.\build.ps1

# Manual compile
pio run

# Upload filesystem only
pio run --target uploadfs

# Upload firmware only
pio run --target upload

# Complete deployment
pio run --target uploadfs && pio run --target upload && pio device monitor
```

### Development Workflow

**Frontend Changes** (HTML/CSS/JS):
1. Edit files in `data/` folder
2. `pio run --target uploadfs`
3. Refresh browser (Ctrl+F5)

**Backend Changes** (C++ code):
1. Edit files in `src/` or `lib/`
2. `pio run --target upload`
3. ESP32 auto-restarts

### Component Development

Each component follows this pattern:

```cpp
class ComponentName {
public:
    void init();      // Setup hardware
    void update();    // Non-blocking loop function
    
private:
    // State variables
    // Private helper methods
};
```

---

## 🚀 Deployment

### Production Checklist

- [ ] Update `secrets.h` with real credentials
- [ ] Test all hardware components
- [ ] Verify WiFi connectivity
- [ ] Test Telegram bot alerts
- [ ] Test emergency override
- [ ] Test duress code (9999)
- [ ] Verify guest code expiry
- [ ] Test door tamper detection
- [ ] Test vibration sensor
- [ ] Check activity logging

### Security Considerations

1. **Network Security**:
   - Use WPA2/WPA3 WiFi encryption
   - Keep ESP32 on isolated VLAN
   - Do NOT expose to public internet without VPN

2. **Credentials**:
   - Never commit `secrets.h` to git
   - Use strong WiFi passwords
   - Rotate Telegram bot token periodically

3. **Physical Security**:
   - Mount ESP32 in tamper-proof enclosure
   - Secure relay module from physical access
   - Position sensors strategically

---

## 🔍 Troubleshooting

### Build Errors

**"LittleFS.h not found"**
- Verify `platformio.ini` has `platform = espressif32`
- Run: `pio lib install`

**Library conflicts**
```powershell
# Clean build
Remove-Item .pio -Recurse -Force
pio run
```

### Runtime Issues

**WiFi not connecting**
1. Check `secrets.h` credentials
2. Verify 2.4GHz network (ESP32 doesn't support 5GHz)
3. Monitor serial output: `pio device monitor`

**Dashboard not loading**
1. Upload filesystem first: `pio run --target uploadfs`
2. Check serial for "[FS] Mounting LittleFS... ✓ OK"
3. Verify files exist with `ls data/`

**Sensors not responding**
1. Verify GPIO connections match pin map
2. Check component `.init()` called in setup()
3. Monitor serial for sensor readings

**Telegram not working**
1. Verify `BOT_TOKEN` and `ADMIN_CHAT_ID`
2. Test token: `https://api.telegram.org/bot<TOKEN>/getMe`
3. Check WiFi connection

### Common Issues

| Issue | Solution |
|-------|----------|
| Upload fails | Close serial monitor, press BOOT button |
| Door won't unlock | Check relay wiring, verify GPIO 4 |
| RFID not reading | Check SPI connections, verify SS=5 |
| Keypad not working | Verify row/col GPIOs, check wiring |

---

## 🤝 Contributing

This is an academic project for IoT coursework. If you fork this project:

1. Maintain component-based architecture
2. Follow existing code style
3. Add tests for new features
4. Update documentation

---

## 📄 License

**MIT License**

Copyright (c) 2026 SecureLock Team - IoT Group 7

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

---

## 🎓 Credits

**Internet of Things - Group 7**

- **Hardware Design**: ESP32 DevKit V1 integration
- **Backend Development**: C++ component architecture
- **Frontend Development**: Glassmorphism web dashboard
- **Security Implementation**: Multi-factor auth + Telegram alerts

**Technologies Used**:
- PlatformIO (ESP32 Framework)
- ESPAsyncWebServer (Non-blocking HTTP)
- ArduinoJson (JSON serialization)
- LittleFS (Filesystem)
- UniversalTelegramBot (Notifications)
- MFRC522 (RFID reader)
- Keypad (Matrix keypad library)

---

## 📞 Support

For issues, questions, or contributions:
1. Check [Troubleshooting](#troubleshooting) section above
2. Review [API Documentation](#api-documentation) for endpoint details
3. Open an issue on the [GitHub repository](https://github.com/4ldrian01/SecureLock-ESP32)

---

**Made with 💙 by IoT Group 7 | ESP32 Smart Security System | 2026**

