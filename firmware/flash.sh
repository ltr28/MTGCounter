#!/usr/bin/env bash
# Builds firmware/MTGCounter from source and flashes it to the Waveshare
# ESP32-S3 Knob board. Run from anywhere:
#
#   firmware/flash.sh                 # compile + upload (port auto-detected)
#   firmware/flash.sh -p /dev/ttyACM1 # use a specific port
#   firmware/flash.sh --monitor       # ...then open the serial monitor
#   firmware/flash.sh --compile-only  # just check it builds, no board needed
#   firmware/flash.sh --setup         # first time: install core + libraries
#
# See firmware/README.md ("Flashing the firmware") for the manual steps this
# automates.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SKETCH="$REPO_ROOT/firmware/MTGCounter"
LV_CONF_SRC="$REPO_ROOT/firmware/lv_conf/lv_conf.h"
FQBN="esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashMode=dio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi"
ESP32_INDEX_URL="https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json"
ESPRESSIF_USB_ID="303a"

PORT=""
MONITOR=0
COMPILE_ONLY=0
SETUP=0

usage() {
  sed -n '2,12p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -p|--port) PORT="${2:?--port needs a value}"; shift 2 ;;
    -m|--monitor) MONITOR=1; shift ;;
    -c|--compile-only) COMPILE_ONLY=1; shift ;;
    --setup) SETUP=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

step() { printf '\n\033[1;34m==> %s\033[0m\n' "$*"; }
die() { printf '\n\033[1;31mError:\033[0m %s\n' "$*" >&2; exit 1; }

# ---- arduino-cli ------------------------------------------------------------
if [[ -x "$REPO_ROOT/bin/arduino-cli" ]]; then
  CLI="$REPO_ROOT/bin/arduino-cli"
elif command -v arduino-cli >/dev/null 2>&1; then
  CLI="$(command -v arduino-cli)"
else
  die "arduino-cli not found. Install it into ./bin with:
  curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | BINDIR=$REPO_ROOT/bin sh"
fi

# ---- one-time setup -----------------------------------------------------------
if [[ $SETUP -eq 1 ]]; then
  step "Installing ESP32 core, LVGL 8.4.0 and NimBLE-Arduino"
  "$CLI" config init >/dev/null 2>&1 || true
  "$CLI" config add board_manager.additional_urls "$ESP32_INDEX_URL" 2>/dev/null || true
  "$CLI" core update-index
  "$CLI" core install esp32:esp32
  "$CLI" lib install "lvgl@8.4.0" "NimBLE-Arduino"
fi

# ---- sanity checks --------------------------------------------------------------
"$CLI" core list 2>/dev/null | grep -q '^esp32:esp32' ||
  die "ESP32 core not installed - run: $0 --setup"
LIBS="$("$CLI" lib list 2>/dev/null)"
grep -Eq '^lvgl +8\.' <<<"$LIBS" ||
  die "LVGL 8.x not installed (this project uses the v8 API, not v9) - run: $0 --setup"
grep -q '^NimBLE-Arduino' <<<"$LIBS" ||
  die "NimBLE-Arduino not installed - run: $0 --setup"

# lv_conf.h must sit next to the installed lvgl library, and the build reads
# that copy, not the repo's - keep it in sync (see the README "gotcha").
SKETCHBOOK="$("$CLI" config get directories.user 2>/dev/null || true)"
[[ -n "$SKETCHBOOK" ]] || SKETCHBOOK="$HOME/Arduino"
LV_CONF_DST="$SKETCHBOOK/libraries/lv_conf.h"
if ! cmp -s "$LV_CONF_SRC" "$LV_CONF_DST"; then
  step "Installing lv_conf.h -> $LV_CONF_DST (and clearing the build cache)"
  mkdir -p "$(dirname "$LV_CONF_DST")"
  cp "$LV_CONF_SRC" "$LV_CONF_DST"
  rm -rf "$HOME/.cache/arduino/sketches"
fi

# ---- compile ----------------------------------------------------------------------
step "Compiling $SKETCH"
"$CLI" compile --fqbn "$FQBN" "$SKETCH"

if [[ $COMPILE_ONLY -eq 1 ]]; then
  step "Compile OK (--compile-only, not uploading)"
  exit 0
fi

# ---- find the board -----------------------------------------------------------------
if [[ -z "$PORT" ]]; then
  # Prefer a port arduino-cli can attribute to the Espressif USB VID, then
  # fall back to the first ttyACM device (how the S3's native USB shows up).
  PORT="$("$CLI" board list --format json 2>/dev/null |
    grep -o '"address": *"[^"]*"' | sed 's/.*"\([^"]*\)"$/\1/' | grep -E 'ttyACM|cu\.usbmodem' | head -n1 || true)"
  if [[ -z "$PORT" ]]; then
    PORT="$(ls /dev/ttyACM* /dev/cu.usbmodem* 2>/dev/null | head -n1 || true)"
  fi
fi

if [[ -z "$PORT" || ! -e "$PORT" ]]; then
  if command -v lsusb >/dev/null 2>&1 && ! lsusb | grep -qi "ID $ESPRESSIF_USB_ID:"; then
    hint="The computer can't see the board over USB at all (no Espressif $ESPRESSIF_USB_ID device in lsusb).
  - Use a USB-C cable that carries data (many charging cables don't).
  - Plug straight into the PC rather than a hub.
  - Or hold BOOT while plugging in to force download mode."
  else
    hint="Pass the port explicitly with -p, e.g. $0 -p /dev/ttyACM0"
  fi
  die "No board serial port found.
$hint"
fi

if [[ ! -w "$PORT" ]]; then
  die "No write permission on $PORT. Fix for this session with:
  sudo chmod 666 $PORT
or permanently with: sudo usermod -aG dialout \$USER (then log out/in)."
fi

# ---- upload -------------------------------------------------------------------------
step "Uploading to $PORT"
if ! "$CLI" upload -p "$PORT" --fqbn "$FQBN" "$SKETCH"; then
  die "Upload failed. Unplug the board, hold BOOT while plugging it back in, then re-run.
(After a BOOT-mode upload, press reset or replug so the new firmware starts.)"
fi

step "Flashed successfully"

if [[ $MONITOR -eq 1 ]]; then
  step "Serial monitor on $PORT (Ctrl+C to exit)"
  exec "$CLI" monitor -p "$PORT" -c baudrate=115200
fi
