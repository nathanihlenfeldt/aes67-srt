#!/usr/bin/env bash
#
# Install the AES67-SRT HAL plug-in and restart the audio server (ADR 0007,
# spec 0002, issue #38). Run with sudo.
#
#   sudo scripts/install-hal.sh [--bundle PATH] [--no-restart]
#
# The driver is the macOS endpoint's own CoreAudio device, so it goes where every
# HAL plug-in goes: /Library/Audio/Plug-Ins/HAL. `coreaudiod` only notices a new
# plug-in when it restarts, which briefly interrupts system audio -- hence the
# explicit restart, and the flag to skip it when scripting.
#
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
INSTALL_DIR="/Library/Audio/Plug-Ins/HAL"
DEVICE_NAME="AES67-SRT"

# system_profiler hangs indefinitely when coreaudiod is wedged, which turns a
# preflight into an install that never returns. macOS has no `timeout`, so bound it
# with perl's alarm; an empty result is reported as "did not respond", not a hang.
sp_audio() {
  perl -e 'alarm 20; exec @ARGV' system_profiler SPAudioDataType 2>/dev/null || true
}

restart_audio() {
  echo "-- restarting the audio server (system audio will briefly interrupt)"
  # `launchctl kickstart` is refused under SIP on this macOS ("Operation not
  # permitted"); `killall` is the fallback that works, so it is not an error path.
  if ! launchctl kickstart -k system/com.apple.audio.coreaudiod 2>/dev/null; then
    killall -9 coreaudiod
  fi
  for _ in $(seq 1 20); do
    pgrep -x coreaudiod >/dev/null && break
    sleep 0.5
  done
  sleep 2
}

BUNDLE="${REPO_ROOT}/build/AES67SRT.driver"
RESTART=1

while [ $# -gt 0 ]; do
  case "$1" in
    --bundle) BUNDLE="$2"; shift 2 ;;
    --no-restart) RESTART=0; shift ;;
    -h|--help) echo "usage: install-hal.sh [--bundle PATH] [--no-restart]"; exit 0 ;;
    *) echo "install-hal: unknown option: $1" >&2; exit 2 ;;
  esac
done

if [ "${EUID}" -ne 0 ]; then
  echo "install-hal: needs root (it writes to ${INSTALL_DIR}): sudo $0 $*" >&2
  exit 1
fi

if [ ! -d "${BUNDLE}" ]; then
  echo "install-hal: no bundle at ${BUNDLE}" >&2
  echo "             build it first: cmake --build build --target AES67SRT" >&2
  exit 1
fi

DEST="${INSTALL_DIR}/$(basename "${BUNDLE}")"
echo "-- installing $(basename "${BUNDLE}") into ${INSTALL_DIR}"
rm -rf "${DEST}"
cp -R "${BUNDLE}" "${DEST}"

if [ "${RESTART}" -eq 1 ]; then
  restart_audio
fi

echo
echo "-- preflight report"
echo "   bundle            $(basename "${DEST}")  ($(du -sh "${DEST}" 2>/dev/null | cut -f1))"
report="$(sp_audio)"
if [ -z "${report}" ]; then
  echo "   device            CoreAudio did not respond to enumeration (coreaudiod may be wedged)"
  echo "                     try: sudo killall -9 coreaudiod"
elif printf '%s' "${report}" | grep -q "${DEVICE_NAME}"; then
  echo "   device            ${DEVICE_NAME} present"
  printf '%s\n' "${report}" | grep -A9 -F "${DEVICE_NAME}" | sed 's/^/   /'
else
  echo "   device            ${DEVICE_NAME} NOT present"
  echo "                     (a restart of coreaudiod may be needed, or the plug-in failed to load)"
fi
echo
echo "Uninstall: sudo scripts/uninstall-hal.sh"