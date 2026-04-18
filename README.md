# 🔐 SecureLock — ESP32 Smart Security System

SecureLock is a component-based smart lock system built on **ESP32 + PlatformIO** with a responsive web dashboard served from **LittleFS**.

It combines:
- RFID card authentication
- Keypad-based second factor (Telegram OTP or offline backup PIN)
- Tamper/theft detection with alarm logic
- Web-based admin management (users, logs, diagnostics, emergency unlock)
- Telegram alerting and remote command support

---

## 📚 Table of Contents

- [Overview](#-overview)
- [Core Features](#-core-features)
- [Architecture](#-architecture)
- [Hardware & GPIO Mapping](#-hardware--gpio-mapping)
- [Project Structure](#-project-structure)
- [Prerequisites](#-prerequisites)
- [Quick Start](#-quick-start)
- [Build, Upload, Monitor](#-build-upload-monitor)
- [Web Dashboard](#-web-dashboard)
- [REST API](#-rest-api)
- [Telegram Commands](#-telegram-commands)
- [Data Storage](#-data-storage)
- [Security Notes](#-security-notes)
- [Troubleshooting](#-troubleshooting)
- [Development Workflow](#-development-workflow)
- [License](#-license)

---

## 🎯 Overview

SecureLock is a full-stack IoT access-control project for ESP32 with:
- **Firmware layer** in C++ (PlatformIO / Arduino framework)
- **Web UI layer** in modular vanilla JavaScript + CSS
- **Persistence** via LittleFS JSON files (`users.json`, `logs.json`)
- **Async networking** via ESPAsyncWebServer

The system is designed around non-blocking patterns (`millis()` timers + async HTTP) to keep lock/security logic responsive.

---

## ✨ Core Features

### Access Control
- RFID card scan (RC522)
- 2FA flow after valid RFID:
  - Primary: **Telegram OTP (4 digits, 30s TTL)**
  - Fallback: **Offline Backup PIN (4 digits)**
- Guest PIN support via Telegram admin command (`/guest_code`, 30 seconds)
- Duress code support (`2580`) with unlock + silent escalation path

### Security & Alarming
- Door tamper detection via reed switch
- Anti-theft vibration strike window detection
- Alarm siren + buzzer feedback patterns
- Emergency lock/unlock state transitions with cooldown protections

### Admin Dashboard
- Token-based admin login (`/api/auth/login`)
- Real-time status polling
- Emergency override action
- User CRUD + RFID enrollment polling
- Logs view with pagination and clear/reset actions
- Diagnostics panel data exposure (RFID/keypad/buzzer/storage integrity)

### Telegram Integration
- Alerts for duress/theft/RFID events
- Admin control commands (`/status`, `/my_info`, `/admin_open`, lockdown controls)
- User informational commands (`/help`, `/my_info`)

---

## 🧱 Architecture

SecureLock uses a 4-component modular firmware design:

1. **`LockManager`** (`lib/LockManager/`)
   - Relay control, lock state, door reed monitoring
   - Auto-lock timer logic

2. **`SecurityManager`** (`lib/SecurityManager/`)
   - Vibration sensing
   - Buzzer/siren state machine

3. **`AuthHandler`** (`lib/AuthHandler/`)
   - RFID reading + keypad scanning
   - User store loading/compaction from LittleFS
   - Backup PIN / Telegram Chat ID user metadata

4. **`WebServer`** (`lib/WebServer/`)
   - WiFi + LittleFS + async API routes
   - Token-based API auth
   - Logs/users synchronization utilities

`src/main_runtime.cpp` orchestrates runtime flows, including Telegram bot logic and cross-component events.

---

## 🔌 Hardware & GPIO Mapping

> Source of truth: `include/hardware_pins.h`

| Function | GPIO |
|---|---:|
| Relay (solenoid lock) | 22 |
| Status LED | 2 |
| Door reed switch | 13 |
| Vibration sensor (SW-420) | 27 |
| Buzzer | 14 *(override-capable)* |
| Factory reset / BOOT | 0 |
| RFID SS | 5 |
| RFID RST | 4 |
| SPI SCK | 18 |
| SPI MOSI | 25 |
| SPI MISO | 19 |
| Keypad rows | 34, 35, 39, 36 |
| Keypad cols | 16, 17, 21, 23 |

---

## 🗂 Project Structure

```text
SecureLock/
├─ src/
│  ├─ main_runtime.cpp
│  └─ main.cpp (placeholder)
├─ lib/
│  ├─ LockManager/
│  ├─ SecurityManager/
│  ├─ AuthHandler/
│  └─ WebServer/
├─ include/
│  ├─ hardware_pins.h
│  ├─ secrets.h
│  └─ secrets.h.example
├─ data/                      # uploaded to LittleFS
│  ├─ html/pages/dashboard.html
│  ├─ css/
│  ├─ js/
│  │  ├─ entry/main.js
│  │  ├─ app/index.js
│  │  ├─ core/
│  │  ├─ features/
│  │  └─ ui/
│  ├─ users.json
│  └─ logs.json
├─ scripts/
│  ├─ deploy_all.ps1
│  └─ deploy_safe.sh
├─ platformio.ini
└─ build.ps1
```

---

## 🧰 Prerequisites

- ESP32 DevKit (or compatible `esp32dev` target)
- PlatformIO Core / VS Code PlatformIO extension
- USB serial driver for your ESP32 board
- 2.4GHz WiFi network
- Telegram bot token + chat IDs

---

## ⚡ Quick Start

1. **Configure secrets**
   - Edit `include/secrets.h`
   - Set WiFi, Telegram, and dashboard admin credentials

2. **Build firmware**
   - `pio run`

3. **Upload web filesystem (required for dashboard changes)**
   - `pio run --target uploadfs`

4. **Upload firmware**
   - `pio run --target upload`

5. **Open serial monitor**
   - `pio device monitor -b 115200 -f direct`

6. **Open dashboard**
   - Visit `http://securelock.local/`
   - Fallback: `http://<ESP32_IP>/` if `.local` is not resolved on your client

---

## 🏗 Build, Upload, Monitor

### Common commands
- Build: `pio run`
- Upload firmware: `pio run --target upload`
- Upload LittleFS assets: `pio run --target uploadfs`
- Monitor serial: `pio device monitor -b 115200 -f direct`

### Helper scripts
- `build.ps1` — clean/install/update/build helper
- `scripts/deploy_all.ps1` — Windows full pipeline (`clean -> build -> uploadfs -> upload -> monitor`)
- `scripts/deploy_safe.sh` — Linux/macOS safe deployment helper

> If your machine cannot find `pio` in PATH, use your PlatformIO executable path directly.

---

## 🌐 Web Dashboard

Main UI file: `data/html/pages/dashboard.html`

### Frontend architecture
- Entry: `data/js/entry/main.js`
- Composition root: `data/js/app/index.js`
- Modules:
  - `core/` → config, state, DOM mapping, API client, helpers
  - `features/` → auth, status, guest, logs, users
  - `ui/feedback.js` → modal + toast UX

### Notable dashboard behavior
- Admin overlay lock until authenticated
- Polling intervals (from `data/js/core/config.js`):
   - Status: 2.8s desktop / 3.6s mobile (adaptive backoff + jitter)
   - Logs: 10s (20s when tab hidden)
   - Users: 15s (30s when tab hidden)
   - Diagnostics: 5s desktop / 9s mobile (12s when hidden)
   - RFID enrollment poll: 400ms (adaptive backoff under errors/hidden tab)
 - Role-aware logs (source badges + actor role badges + search/status/date filters)
 - Lock telemetry chips (API RTT, Telegram queue depth, Telegram polling cadence)
 - Guest code generation from dashboard (`/api/guest-code`) with cooldown awareness
- Responsive logs pagination
- Add/Edit user dialogs with split name fields and seeded-admin safeguards

---

## 📡 REST API

Base path: `/api`

### Authentication
Most endpoints require `Authorization: Bearer <token>` from `/api/auth/login`.

### Endpoints

| Method | Endpoint | Purpose |
|---|---|---|
| POST | `/api/auth/login` | Admin login, issue session token |
| POST | `/api/auth/logout` | Revoke active session |
| GET | `/api/auth/status` | Validate session state |
| GET | `/api/status` | Realtime system status |
| POST | `/api/unlock` | Emergency unlock (cooldown protected) |
| POST | `/api/guest-code` | Generate/reuse guest PIN (same policy and cooldown rules as Telegram admin flow) |
| GET | `/api/users` | List users (includes split names + seeded-admin metadata) |
| POST | `/api/users` | Add user (supports split name fields) |
| PUT | `/api/users` | Edit user / seeded-admin profile |
| DELETE | `/api/users?uid=<UID>` | Delete user |
| POST | `/api/users/reset` | Reset all users (explicit confirmation body) |
| GET | `/api/logs` | Get logs (normalized status/methodCode/actorRole) |
| DELETE | `/api/logs` | Clear logs |
| GET | `/api/rfid/scan` | Last RFID scan state (for enrollment flow) |
| GET | `/api/diagnostics` | Runtime hardware/storage diagnostics |
| POST | `/api/addUser` | Alias of add-user endpoint |

### Notes
- CORS is origin-restricted (device host/IP, `securelock.local`, localhost loopbacks).
- Logs are persisted to LittleFS and returned newest-first.
- `users.json` is synchronized from auth storage to avoid drift.

---

## 🤖 Telegram Commands

### Admin commands
- `/help` — full admin command guide
- `/start` — admin session heartbeat/verification
- `/my_info` — admin account + device link summary
- `/status` — live lock/alarm/WiFi status snapshot
- `/admin_open` — policy-protected emergency unlock
- `/guest_code` — generate/reuse 30-second guest PIN
- `/buzzer_test` — buzzer diagnostic command
- `/keypad_echo` — return last keypad key + age
- `/lockdown` — disable local RFID/keypad authentication
- `/unlockdown` — re-enable local RFID/keypad authentication
- `/reboot` — controlled device reboot

### User commands
- `/help` — standard user access instructions + allowed commands
- `/start` — user connectivity check
- `/my_info` — linked account summary

Unknown or unauthorized command attempts are logged and handled explicitly.

---

## 💾 Data Storage

LittleFS files:
- `data/users.json` (user list + optional settings block)
- `data/logs.json` (activity logs)

Runtime behavior:
- User records are compacted/self-healed for schema consistency
- Seeded admin display-name overrides are stored under `settings.seededAdminProfiles`
- Log list is capped to prevent uncontrolled growth

---

## 🔐 Security Notes

- `include/secrets.h` is sensitive and should remain private.
- Use `include/secrets.h.example` as the template for fresh setup.
- Keep `include/secrets.h` out of version control (already covered by `.gitignore`).
- Rotate WiFi and Telegram credentials if leaked.
- Set a strong `DASHBOARD_ADMIN_PASSWORD` before deployment (login is blocked when left as `CHANGE_ME_NOW`).
- Prefer isolated network/VLAN for production devices.
- Do not expose device API directly to public internet.
- Firmware credentials are sourced from `include/secrets.h`.

---

## 🛠 Troubleshooting

### Dashboard loads but styles/scripts missing
- Upload filesystem first: `pio run --target uploadfs`

### Upload fails / serial port busy
- Close monitor sessions, retry upload
- Re-check active COM port and USB cable quality

### WiFi not connecting
- Verify credentials in `include/secrets.h`
- Ensure 2.4GHz AP availability

### API returns 401
- Login first via dashboard (`/api/auth/login`)
- Ensure Bearer token is attached by frontend client

### RFID/keypad enrollment issues
- Use `/api/diagnostics` to inspect readiness/mute/cooldown state
- Confirm GPIO wiring matches `include/hardware_pins.h`

---

## 👨‍💻 Development Workflow

### Firmware changes (`src/`, `lib/`, `include/`)
1. Edit code
2. `pio run`
3. `pio run --target upload`

### Dashboard changes (`data/`)
1. Edit HTML/CSS/JS/JSON
2. `pio run --target uploadfs`
3. Hard refresh browser

### Recommended full cycle
1. Upload filesystem
2. Upload firmware
3. Monitor serial output
4. Validate dashboard/API flows

---

## 📄 License

No license file is currently present in this repository.

If you want open-source distribution, add a `LICENSE` file (e.g., MIT/Apache-2.0) and update this section.

---

Built for **IoT Group 7** with ❤️ on ESP32.
