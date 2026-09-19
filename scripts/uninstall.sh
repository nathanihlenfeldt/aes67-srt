#!/usr/bin/env bash
#
# Removes the appliance installed by scripts/install.sh.
#
#   sudo ./scripts/uninstall.sh                 # stop and remove the appliance
#   sudo ./scripts/uninstall.sh --purge         # also remove the config
#   sudo ./scripts/uninstall.sh --with-daemon   # also remove aes67-daemon
#
# The config is kept by default: it holds the site's address and passphrase, and
# an uninstaller that silently deletes a site's credentials is the kind of
# surprise this project refuses elsewhere. The RAVENNA kernel module is left
# alone even with --with-daemon — removing a DKMS module is a kernel concern, not
# this program's, and `dkms remove` is a command the operator should run knowing
# what it does.
set -euo pipefail

PREFIX="/usr/local"
SRC_DIR="/opt/aes67-srt-src"
CONFIG_FILE="/etc/aes67-srt.conf"
UNIT_FILE="/etc/systemd/system/aes67-srt.service"
POLKIT_FILE="/etc/polkit-1/rules.d/49-aes67-srt-daemon.rules"
SERVICE_USER="aes67-srt"
PURGE=0
WITH_DAEMON=0

log()  { printf '==> %s\n' "$*"; }
warn() { printf '[warn] %s\n' "$*" >&2; }

usage() {
  sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'
}

for argument in "$@"; do
  case "${argument}" in
    --purge) PURGE=1 ;;
    --with-daemon) WITH_DAEMON=1 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "uninstall: unknown option: ${argument}" >&2; exit 2 ;;
  esac
done

[[ "$(id -u)" -eq 0 ]] || { echo "uninstall: run with sudo" >&2; exit 1; }
command -v systemctl >/dev/null 2>&1 || { echo "uninstall: systemd is required" >&2; exit 1; }

# ---------------------------------------------------------------------------
# 1. The appliance service
# ---------------------------------------------------------------------------
log "stopping and disabling aes67-srt"
systemctl stop aes67-srt 2>/dev/null || true
systemctl disable aes67-srt 2>/dev/null || true
systemctl reset-failed aes67-srt 2>/dev/null || true

# ---------------------------------------------------------------------------
# 2. The daemon, only if asked
# ---------------------------------------------------------------------------
if [[ "${WITH_DAEMON}" -eq 1 ]]; then
  log "stopping and disabling aes67-daemon"
  systemctl stop aes67-daemon 2>/dev/null || true
  systemctl disable aes67-daemon 2>/dev/null || true
  rm -f /etc/systemd/system/aes67-daemon.service
  rm -f /usr/local/bin/aes67-daemon
  warn "the RAVENNA kernel module is left in place; remove it with 'dkms remove' if you mean to"
else
  log "aes67-daemon left alone (--with-daemon removes it)"
fi

# ---------------------------------------------------------------------------
# 3. Files
# ---------------------------------------------------------------------------
log "removing the binaries, unit and service user"
rm -f "${UNIT_FILE}"
rm -f "${PREFIX}/bin/aes67-srt"
rm -f "${POLKIT_FILE}"
systemctl daemon-reload
systemctl reset-failed aes67-srt 2>/dev/null || true

log "removing the source checkout"
rm -rf "${SRC_DIR}"

if id "${SERVICE_USER}" >/dev/null 2>&1; then
  userdel "${SERVICE_USER}" 2>/dev/null || warn "could not remove user ${SERVICE_USER}"
fi
rm -rf "/var/lib/${SERVICE_USER}" "/var/log/${SERVICE_USER}"

if [[ "${PURGE}" -eq 1 ]]; then
  log "removing ${CONFIG_FILE} (--purge)"
  rm -f "${CONFIG_FILE}"
else
  warn "kept ${CONFIG_FILE} (it holds the site's address and passphrase); --purge removes it"
fi

log "done"