#!/usr/bin/env bash
#
# Remove the AES67-SRT HAL plug-in and restart the audio server (ADR 0007,
# spec 0002, issue #38). Run with sudo.
#
#   sudo scripts/uninstall-hal.sh [--no-restart]
#
# This is the promise ADR 0007 makes: a fault in the plug-in must not be permanent,
# so removing it and restarting coreaudiod restores the machine. Nothing else is
# touched -- the mapping goes with the name, and no files outside the HAL directory
# are written.
#
set -uo pipefail

INSTALL_DIR="/Library/Audio/Plug-Ins/HAL"
BUNDLE_NAME="AES67SRT.driver"
DEVICE_NAME="AES67-SRT"

RESTART=1
while [ $# -gt 0 ]; do
  case "$1" in
    --no-restart) RESTART=0; shift ;;
    -h|--help) echo "usage: uninstall-hal.sh [--no-restart]"; exit 0 ;;
    *) echo "uninstall-hal: unknown option: $1" >&2; exit 2 ;;
  esac
done

if [ "${EUID}" -ne 0 ]; then
  echo "uninstall-hal: needs root: sudo $0 $*" >&2
  exit 1
fi

DEST="${INSTALL_DIR}/${BUNDLE_NAME}"
if [ -d "${DEST}" ]; then
  echo "-- removing ${DEST}"
  rm -rf "${DEST}"
else
  echo "-- ${DEST} is not installed"
fi

if [ "${RESTART}" -eq 1 ]; then
  echo "-- restarting the audio server"
  if ! launchctl kickstart -k system/com.apple.audio.coreaudiod 2>/dev/null; then
    killall -9 coreaudiod
  fi
  for _ in $(seq 1 20); do
    pgrep -x coreaudiod >/dev/null && break
    sleep 0.5
  done
  sleep 2
fi

# Bounded, so a wedged coreaudiod cannot hang the uninstall (macOS has no timeout).
report="$(perl -e 'alarm 20; exec @ARGV' system_profiler SPAudioDataType 2>/dev/null || true)"
if [ -z "${report}" ]; then
  echo "-- CoreAudio did not respond to enumeration; try: sudo killall -9 coreaudiod"
elif printf '%s' "${report}" | grep -q "${DEVICE_NAME}"; then
  echo "-- WARNING: ${DEVICE_NAME} is still present; a reboot clears it"
else
  echo "-- ${DEVICE_NAME} removed"
fi