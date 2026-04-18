#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
PIO_BIN="${PIO_BIN:-}"
PORT="${1:-}"
MONITOR_SECONDS="${MONITOR_SECONDS:-20}"
SKIP_CLEAN="${SKIP_CLEAN:-0}"
NO_MONITOR="${NO_MONITOR:-0}"
UPLOAD_MAX_ATTEMPTS="${UPLOAD_MAX_ATTEMPTS:-3}"
RETRY_DELAY_SEC="${RETRY_DELAY_SEC:-2}"

if ! [[ "$UPLOAD_MAX_ATTEMPTS" =~ ^[0-9]+$ ]] || (( UPLOAD_MAX_ATTEMPTS < 1 )); then
  UPLOAD_MAX_ATTEMPTS=3
fi

if ! [[ "$RETRY_DELAY_SEC" =~ ^[0-9]+$ ]] || (( RETRY_DELAY_SEC < 1 )); then
  RETRY_DELAY_SEC=2
fi

if ! [[ "$MONITOR_SECONDS" =~ ^[0-9]+$ ]] || (( MONITOR_SECONDS < 1 )); then
  MONITOR_SECONDS=20
fi

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

run_with_retry() {
  local title="$1"
  local max_attempts="$2"
  shift 2
  local attempt=1

  echo
  echo "============================================================"
  echo "[DEPLOY] ${title}"
  echo "============================================================"

  while (( attempt <= max_attempts )); do
    if "$@"; then
      echo "[DEPLOY] OK: ${title}"
      return 0
    fi

    if (( attempt < max_attempts )); then
      echo "[DEPLOY][WARN] ${title} failed on attempt ${attempt}/${max_attempts}."
      echo "[DEPLOY][HINT] If this is a boot-mode error, hold BOOT while retrying and release after 'Connecting...'."
      echo "[DEPLOY] Retrying in ${RETRY_DELAY_SEC}s..."
      sleep "$RETRY_DELAY_SEC"
    fi

    ((attempt++))
  done

  echo "[DEPLOY][ERROR] ${title} failed after ${max_attempts} attempt(s)."
  return 1
}

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
echo "[DEPLOY] Upload max attempts: $UPLOAD_MAX_ATTEMPTS"
echo "[DEPLOY] Retry delay: ${RETRY_DELAY_SEC}s"
echo "[DEPLOY] Skip clean: $SKIP_CLEAN"
echo "[DEPLOY] No monitor: $NO_MONITOR"

# Best effort: stop leftover monitor process locking the port.
pkill -f "pio device monitor.*$PORT" >/dev/null 2>&1 || true
pkill -f "esptool.py.*$PORT" >/dev/null 2>&1 || true

if [[ "$SKIP_CLEAN" != "1" ]]; then
  run_with_retry "Clean" 1 "$PIO_BIN" run --target clean
else
  echo "[DEPLOY] Skipping clean (SKIP_CLEAN=1)"
fi

"$PIO_BIN" run
run_with_retry "Upload filesystem (LittleFS)" "$UPLOAD_MAX_ATTEMPTS" "$PIO_BIN" run --target uploadfs --upload-port "$PORT"
run_with_retry "Upload firmware" "$UPLOAD_MAX_ATTEMPTS" "$PIO_BIN" run --target upload --upload-port "$PORT"

if [[ "$NO_MONITOR" != "1" ]]; then
  timeout "$MONITOR_SECONDS" "$PIO_BIN" device monitor -p "$PORT" -b 115200 -f direct || true
else
  echo "[DEPLOY] Monitor skipped (NO_MONITOR=1)"
fi

echo "[DEPLOY] Completed successfully"
