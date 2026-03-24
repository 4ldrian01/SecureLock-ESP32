#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
PIO_BIN="${PIO_BIN:-}"
PORT="${1:-}"
MONITOR_SECONDS="${MONITOR_SECONDS:-20}"

if [[ -z "$PIO_BIN" ]]; then
  CANDIDATES=(
    "${HOME}/.platformio/penv/bin/platformio"
    "${HOME}/.platformio/penv/bin/pio"
    "$(command -v platformio 2>/dev/null || true)"
    "$(command -v pio 2>/dev/null || true)"
  )

  for candidate in "${CANDIDATES[@]}"; do
    if [[ -n "$candidate" && -x "$candidate" ]]; then
      PIO_BIN="$candidate"
      break
    fi
  done
fi

if [[ -z "$PIO_BIN" || ! -x "$PIO_BIN" ]]; then
  echo "[DEPLOY][ERROR] PlatformIO binary not found. Set PIO_BIN or install PlatformIO Core."
  exit 1
fi

cd "$ROOT_DIR"

if [[ -z "$PORT" ]]; then
  if [[ -e /dev/ttyUSB0 ]]; then
    PORT="/dev/ttyUSB0"
  elif [[ -e /dev/ttyUSB1 ]]; then
    PORT="/dev/ttyUSB1"
  elif [[ -e /dev/ttyACM0 ]]; then
    PORT="/dev/ttyACM0"
  else
    echo "[DEPLOY][ERROR] No serial port found. Pass one manually, e.g. scripts/deploy_safe.sh /dev/ttyUSB0"
    exit 1
  fi
fi

echo "[DEPLOY] Using port: $PORT"
echo "[DEPLOY] PlatformIO: $PIO_BIN"

# Best effort: stop leftover monitor process locking the port.
pkill -f "pio device monitor.*$PORT" >/dev/null 2>&1 || true

"$PIO_BIN" run --target clean
"$PIO_BIN" run
"$PIO_BIN" run --target uploadfs --upload-port "$PORT"
"$PIO_BIN" run --target upload --upload-port "$PORT"

timeout "$MONITOR_SECONDS" "$PIO_BIN" device monitor -p "$PORT" -b 115200 -f direct || true

echo "[DEPLOY] Completed successfully"
