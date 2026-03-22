/**
 * ============================================================
 * SecureLock - Hardware Pin Configuration
 * ============================================================
 *
 * IMPORTANT:
 * - This file is the single source of truth for GPIO mapping.
 * - MFRC522 is configured for: SS=32, RST=4, SCK=33, MOSI=25, MISO=26.
 * - Requested MOSI=GPIO35 is not electrically valid on ESP32 because
 *   GPIO35 is input-only. MOSI must be output-capable.
 * - ESP32 GPIO34-39 are input-only. Never assign them to any signal
 *   that is actively driven HIGH/LOW by software.
 *
 * NOTE ON RELAY VS RFID RST:
 * RFID reset uses GPIO4 while relay stays on GPIO22 (no overlap).
 * ============================================================
 */

#ifndef HARDWARE_PINS_H
#define HARDWARE_PINS_H

// ------------------------------------------------------------
// Core hardware (lock + sensors)
// ------------------------------------------------------------
#define SECURELOCK_PIN_RELAY           22
#define SECURELOCK_PIN_LED             2
#define SECURELOCK_PIN_DOOR            13
#define SECURELOCK_PIN_VIBRATION       27
#define SECURELOCK_PIN_BOOT            0

// ------------------------------------------------------------
// RFID RC522 (SPI)
// ------------------------------------------------------------
#define SECURELOCK_PIN_RFID_SS         32
#define SECURELOCK_PIN_RFID_RST        4
#define SECURELOCK_PIN_SPI_SCK         33
#define SECURELOCK_PIN_SPI_MOSI        25
#define SECURELOCK_PIN_SPI_MISO        26

// ------------------------------------------------------------
// Keypad 4x4
// ------------------------------------------------------------
// Safe scan mapping: keep columns on output-capable NON-STRAPPING GPIOs.
// Rows use input-capable ADC pins (34/35/39/36) and are never driven.
// Board silkscreen equivalents: GPIO39 = VN, GPIO36 = VP.
// This avoids: "gpio_set_level(...): GPIO output gpio_num error".
#define SECURELOCK_PIN_KEYPAD_R1       34
#define SECURELOCK_PIN_KEYPAD_R2       35
#define SECURELOCK_PIN_KEYPAD_R3       39
#define SECURELOCK_PIN_KEYPAD_R4       36

// Many ESP32 dev boards label GPIO16/17 as RX2/TX2.
#define SECURELOCK_PIN_KEYPAD_C1       16
#define SECURELOCK_PIN_KEYPAD_C2       17
#define SECURELOCK_PIN_KEYPAD_C3       21
#define SECURELOCK_PIN_KEYPAD_C4       23

// ------------------------------------------------------------
// Buzzer configuration (can be overridden from secrets.h)
// ------------------------------------------------------------
#ifndef SECURELOCK_PIN_BUZZER
#define SECURELOCK_PIN_BUZZER          14
#endif

#ifndef SECURELOCK_BUZZER_ACTIVE_HIGH
#define SECURELOCK_BUZZER_ACTIVE_HIGH  1
#endif

#endif // HARDWARE_PINS_H