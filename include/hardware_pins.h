/**
 * ============================================================
 * SecureLock - Hardware Pin Configuration
 * ============================================================
 *
 * IMPORTANT:
 * - This file is the single source of truth for GPIO mapping.
 * - MFRC522 is configured for hardware VSPI: SS=5, RST=4, SCK=18, MOSI=23, MISO=19.
 * - Vibration sensor uses GPIO34 (input-only). Keep it as INPUT (no INPUT_PULLUP).
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
#define SECURELOCK_PIN_VIBRATION       34
#define SECURELOCK_PIN_BOOT            0

// ------------------------------------------------------------
// RFID RC522 (SPI)
// ------------------------------------------------------------
#define SECURELOCK_PIN_RFID_SS         5
#define SECURELOCK_PIN_RFID_RST        4
#define SECURELOCK_PIN_SPI_SCK         18
#define SECURELOCK_PIN_SPI_MOSI        23
#define SECURELOCK_PIN_SPI_MISO        19

// ------------------------------------------------------------
// Keypad 4x4
// ------------------------------------------------------------
// Wiring blueprint:
// Rows (inputs): 32, 33, 25, 26
// Cols (outputs): 27, 16(RX2), 17(TX2), 21
#define SECURELOCK_PIN_KEYPAD_R1       32
#define SECURELOCK_PIN_KEYPAD_R2       33
#define SECURELOCK_PIN_KEYPAD_R3       25
#define SECURELOCK_PIN_KEYPAD_R4       26

#define SECURELOCK_PIN_KEYPAD_C1       27
#define SECURELOCK_PIN_KEYPAD_C2       16
#define SECURELOCK_PIN_KEYPAD_C3       17
#define SECURELOCK_PIN_KEYPAD_C4       21

// ------------------------------------------------------------
// Buzzer configuration (can be overridden from secrets.h)
// ------------------------------------------------------------
#ifndef SECURELOCK_PIN_BUZZER
#define SECURELOCK_PIN_BUZZER          14
#endif

#ifndef SECURELOCK_BUZZER_ACTIVE_HIGH
#define SECURELOCK_BUZZER_ACTIVE_HIGH  1
#endif

// ------------------------------------------------------------
// Vibration sensor digital polarity
// ------------------------------------------------------------
// Some SW-420 modules assert HIGH on strike, others assert LOW.
// 1 = HIGH means vibration event, 0 = LOW means vibration event.
#ifndef SECURELOCK_VIBRATION_ACTIVE_HIGH
#define SECURELOCK_VIBRATION_ACTIVE_HIGH 1
#endif

// ------------------------------------------------------------
// Compile-time pin-map validation
// ------------------------------------------------------------
#define SECURELOCK_IS_INPUT_ONLY_GPIO(gpio) \
	((gpio) == 34 || (gpio) == 35 || (gpio) == 36 || (gpio) == 39)

#define SECURELOCK_IS_FLASH_BUS_GPIO(gpio) \
	((gpio) >= 6 && (gpio) <= 11)

static_assert(!SECURELOCK_IS_FLASH_BUS_GPIO(SECURELOCK_PIN_RELAY), "RELAY pin must not use GPIO6-11");
static_assert(!SECURELOCK_IS_FLASH_BUS_GPIO(SECURELOCK_PIN_LED), "LED pin must not use GPIO6-11");
static_assert(!SECURELOCK_IS_FLASH_BUS_GPIO(SECURELOCK_PIN_DOOR), "DOOR pin must not use GPIO6-11");
static_assert(!SECURELOCK_IS_FLASH_BUS_GPIO(SECURELOCK_PIN_VIBRATION), "VIBRATION pin must not use GPIO6-11");
static_assert(!SECURELOCK_IS_FLASH_BUS_GPIO(SECURELOCK_PIN_BUZZER), "BUZZER pin must not use GPIO6-11");
static_assert(!SECURELOCK_IS_FLASH_BUS_GPIO(SECURELOCK_PIN_RFID_SS), "RFID SS pin must not use GPIO6-11");
static_assert(!SECURELOCK_IS_FLASH_BUS_GPIO(SECURELOCK_PIN_RFID_RST), "RFID RST pin must not use GPIO6-11");

static_assert(SECURELOCK_PIN_RELAY != SECURELOCK_PIN_RFID_RST, "RELAY and RFID_RST must be different GPIOs");
static_assert(SECURELOCK_PIN_RELAY != SECURELOCK_PIN_RFID_SS, "RELAY and RFID_SS must be different GPIOs");
static_assert(SECURELOCK_PIN_RELAY != SECURELOCK_PIN_BUZZER, "RELAY and BUZZER must be different GPIOs");
static_assert(SECURELOCK_PIN_DOOR != SECURELOCK_PIN_RELAY, "DOOR and RELAY must be different GPIOs");
static_assert(SECURELOCK_PIN_DOOR != SECURELOCK_PIN_BUZZER, "DOOR and BUZZER must be different GPIOs");
static_assert(SECURELOCK_PIN_VIBRATION != SECURELOCK_PIN_RELAY, "VIBRATION and RELAY must be different GPIOs");
static_assert(SECURELOCK_PIN_VIBRATION != SECURELOCK_PIN_BUZZER, "VIBRATION and BUZZER must be different GPIOs");

static_assert(SECURELOCK_PIN_KEYPAD_R1 != SECURELOCK_PIN_KEYPAD_R2, "KEYPAD rows must be unique");
static_assert(SECURELOCK_PIN_KEYPAD_R1 != SECURELOCK_PIN_KEYPAD_R3, "KEYPAD rows must be unique");
static_assert(SECURELOCK_PIN_KEYPAD_R1 != SECURELOCK_PIN_KEYPAD_R4, "KEYPAD rows must be unique");
static_assert(SECURELOCK_PIN_KEYPAD_R2 != SECURELOCK_PIN_KEYPAD_R3, "KEYPAD rows must be unique");
static_assert(SECURELOCK_PIN_KEYPAD_R2 != SECURELOCK_PIN_KEYPAD_R4, "KEYPAD rows must be unique");
static_assert(SECURELOCK_PIN_KEYPAD_R3 != SECURELOCK_PIN_KEYPAD_R4, "KEYPAD rows must be unique");

static_assert(SECURELOCK_PIN_KEYPAD_C1 != SECURELOCK_PIN_KEYPAD_C2, "KEYPAD cols must be unique");
static_assert(SECURELOCK_PIN_KEYPAD_C1 != SECURELOCK_PIN_KEYPAD_C3, "KEYPAD cols must be unique");
static_assert(SECURELOCK_PIN_KEYPAD_C1 != SECURELOCK_PIN_KEYPAD_C4, "KEYPAD cols must be unique");
static_assert(SECURELOCK_PIN_KEYPAD_C2 != SECURELOCK_PIN_KEYPAD_C3, "KEYPAD cols must be unique");
static_assert(SECURELOCK_PIN_KEYPAD_C2 != SECURELOCK_PIN_KEYPAD_C4, "KEYPAD cols must be unique");
static_assert(SECURELOCK_PIN_KEYPAD_C3 != SECURELOCK_PIN_KEYPAD_C4, "KEYPAD cols must be unique");

static_assert(!SECURELOCK_IS_INPUT_ONLY_GPIO(SECURELOCK_PIN_KEYPAD_R1), "KEYPAD R1 must support INPUT_PULLUP");
static_assert(!SECURELOCK_IS_INPUT_ONLY_GPIO(SECURELOCK_PIN_KEYPAD_R2), "KEYPAD R2 must support INPUT_PULLUP");
static_assert(!SECURELOCK_IS_INPUT_ONLY_GPIO(SECURELOCK_PIN_KEYPAD_R3), "KEYPAD R3 must support INPUT_PULLUP");
static_assert(!SECURELOCK_IS_INPUT_ONLY_GPIO(SECURELOCK_PIN_KEYPAD_R4), "KEYPAD R4 must support INPUT_PULLUP");

static_assert(!SECURELOCK_IS_INPUT_ONLY_GPIO(SECURELOCK_PIN_KEYPAD_C1), "KEYPAD C1 must be output-capable GPIO");
static_assert(!SECURELOCK_IS_INPUT_ONLY_GPIO(SECURELOCK_PIN_KEYPAD_C2), "KEYPAD C2 must be output-capable GPIO");
static_assert(!SECURELOCK_IS_INPUT_ONLY_GPIO(SECURELOCK_PIN_KEYPAD_C3), "KEYPAD C3 must be output-capable GPIO");
static_assert(!SECURELOCK_IS_INPUT_ONLY_GPIO(SECURELOCK_PIN_KEYPAD_C4), "KEYPAD C4 must be output-capable GPIO");

static_assert(SECURELOCK_PIN_VIBRATION == 34, "VIBRATION sensor pin is fixed to GPIO34 by wiring blueprint");

static_assert(SECURELOCK_PIN_BOOT == 0, "BOOT pin is expected on GPIO0 for factory reset button");

#endif // HARDWARE_PINS_H