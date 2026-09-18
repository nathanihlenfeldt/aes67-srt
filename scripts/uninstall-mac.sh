#!/usr/bin/env bash
#
# Removes the macOS endpoint installed by scripts/install-mac.sh.
#
#   ./scripts/uninstall-mac.sh          # stop it and remove the binary and agent
#   ./scripts/uninstall-mac.sh --purge  # also remove the config and the log
#
# The config is kept by default: it holds the site's address and passphrase, and
# removing it silently is the kind of surprise an uninstaller should not spring.
set -euo pipefail

APP_DIR="${HOME}/Library/Application Support/aes67-srt"
LOG_DIR="${HOME}/Library/Logs/aes67-srt"
AGENT_LABEL="com.aes67-srt.endpoint"
AGENT_PLIST="${HOME}/Library/LaunchAgents/${AGENT_LABEL}.plist"
PURGE=0

for argument in "$@"; do
  case "${argument}" in
    --purge) PURGE=1 ;;
    -h|--help) echo "usage: uninstall-mac.sh [--purge]"; exit 0 ;;
    *) echo "uninstall-mac: unknown option: ${argument}" >&2; exit 2 ;;
  esac
done

launchctl bootout "gui/$(id -u)/${AGENT_LABEL}" 2>/dev/null ||
  launchctl unload "${AGENT_PLIST}" 2>/dev/null || true
rm -f "${AGENT_PLIST}"
rm -f "${APP_DIR}/aes67-srt-mac"

if [[ "${PURGE}" -eq 1 ]]; then
  rm -rf "${APP_DIR}" "${LOG_DIR}"
  echo "removed the endpoint, its agent, config and log"
else
  echo "stopped the endpoint and removed its binary and agent"
  echo "kept: ${APP_DIR}/aes67-srt-mac.conf (use --purge to remove it too)"
fi
