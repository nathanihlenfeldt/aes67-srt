#!/usr/bin/env bash
#
# Installs the macOS endpoint for the current user: the binary, a configuration,
# and a LaunchAgent that starts it at login and restarts it if it exits.
#
#   ./scripts/install-mac.sh --peer 203.0.113.10:9000 --passphrase a-shared-secret
#
# **No sudo.** Everything lands under ~/Library. BlackHole is the one privileged
# step — it is a CoreAudio driver — and the guide in
# docs/runbooks/macos-endpoint.md covers it, including the Gatekeeper step for an
# unsigned build.
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP_DIR="${HOME}/Library/Application Support/aes67-srt"
LOG_DIR="${HOME}/Library/Logs/aes67-srt"
AGENT_DIR="${HOME}/Library/LaunchAgents"
AGENT_LABEL="com.aes67-srt.endpoint"
AGENT_PLIST="${AGENT_DIR}/${AGENT_LABEL}.plist"
CONFIG="${APP_DIR}/aes67-srt-mac.conf"
BINARY="${APP_DIR}/aes67-srt-mac"
BINARY_SRC="${REPO_ROOT}/build/aes67-srt-mac"
DEVICE="BlackHole 64ch"
ROLE="rx"
PEER=""
PASSPHRASE=""
LOCAL_PORT=""
HTTP_PORT=""
WITH_MENUBAR=1

usage() {
  cat <<'USAGE'
usage: install-mac.sh [options]

  --binary <path>       the built endpoint (default build/aes67-srt-mac)
  --device <name>       the CoreAudio device (default "BlackHole 64ch")
  --role <rx|tx|duplex> which direction this end runs (default rx: appliance -> Mac)
  --peer <host:port>    the appliance's SRT address (default: set it in the config)
  --passphrase <text>   the shared SRT passphrase (default: set it in the config)
  --local-port <port>   this end's SRT port (default 9100)
  --http-port <port>    the control surface's port (default 8082)
  --no-menubar          do not build or start the menu-bar app
  -h, --help            this message

Installs to ~/Library/Application Support/aes67-srt and loads a LaunchAgent.
Run scripts/uninstall-mac.sh to remove it again.
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --binary) BINARY_SRC="$2"; shift 2 ;;
    --device) DEVICE="$2"; shift 2 ;;
    --role) ROLE="$2"; shift 2 ;;
    --peer) PEER="$2"; shift 2 ;;
    --passphrase) PASSPHRASE="$2"; shift 2 ;;
    --local-port) LOCAL_PORT="$2"; shift 2 ;;
    --http-port) HTTP_PORT="$2"; shift 2 ;;
    --no-menubar) WITH_MENUBAR=0 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "install-mac: unknown option: $1" >&2; exit 2 ;;
  esac
done

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "install-mac: this is the macOS endpoint; run install.sh on the appliance" >&2
  exit 1
fi
if [[ ! -x "${BINARY_SRC}" ]]; then
  echo "install-mac: no endpoint binary at ${BINARY_SRC}" >&2
  echo "  build it first:  cmake -S . -B build && cmake --build build --target aes67-srt-mac" >&2
  exit 1
fi

# BlackHole is a CoreAudio driver and cannot be installed without sudo, so the
# installer checks for it and points at the guide rather than pretending.
if system_profiler SPAudioDataType 2>/dev/null | grep -q "${DEVICE}"; then
  blackhole="present"
else
  blackhole="missing"
fi

mkdir -p "${APP_DIR}" "${LOG_DIR}" "${AGENT_DIR}"
install -m 0755 "${BINARY_SRC}" "${BINARY}"
install -m 0644 "${REPO_ROOT}/config/aes67-srt-mac.conf" "${CONFIG}"

# Prefill the config when the caller gave the values. python3 is what renders it;
# without it the guide's manual edit is the fallback rather than a half-edited
# document this script guessed at.
if [[ -n "${PEER}" || -n "${PASSPHRASE}" || -n "${LOCAL_PORT}" ||
      -n "${HTTP_PORT}" || "${DEVICE}" != "BlackHole 64ch" ||
      "${ROLE}" != "rx" ]]; then
  if ! command -v python3 >/dev/null 2>&1; then
    echo "install-mac: python3 is needed to prefill the config; edit ${CONFIG} by hand instead" >&2
  else
    PEER="${PEER}" PASSPHRASE="${PASSPHRASE}" DEVICE="${DEVICE}" ROLE="${ROLE}" \
      LOCAL_PORT="${LOCAL_PORT}" HTTP_PORT="${HTTP_PORT}" CONFIG="${CONFIG}" \
      python3 - <<'PY'
import json, os
path = os.environ["CONFIG"]
with open(path) as handle:
    config = json.load(handle)
if os.environ["PEER"]:
    config["link"]["peer"] = os.environ["PEER"]
if os.environ["PASSPHRASE"]:
    config["link"]["passphrase"] = os.environ["PASSPHRASE"]
if os.environ["LOCAL_PORT"]:
    config["link"]["local_port"] = int(os.environ["LOCAL_PORT"])
if os.environ["HTTP_PORT"]:
    config["http_port"] = int(os.environ["HTTP_PORT"])
config["audio"]["device"] = os.environ["DEVICE"]
config["link"]["role"] = os.environ["ROLE"]
with open(path, "w") as handle:
    json.dump(config, handle, indent=2)
    handle.write("\n")
PY
  fi
fi

cat > "${AGENT_PLIST}" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key>
  <string>${AGENT_LABEL}</string>
  <key>ProgramArguments</key>
  <array>
    <string>${BINARY}</string>
    <string>-c</string>
    <string>${CONFIG}</string>
  </array>
  <key>RunAtLoad</key>
  <true/>
  <!-- Restart on exit, but not in a tight loop: a config that cannot open must
       not spin. The reason lands in the log. -->
  <key>KeepAlive</key>
  <true/>
  <key>ThrottleInterval</key>
  <integer>10</integer>
  <key>StandardOutPath</key>
  <string>${LOG_DIR}/endpoint.log</string>
  <key>StandardErrorPath</key>
  <string>${LOG_DIR}/endpoint.log</string>
</dict>
</plist>
PLIST

# Load it. bootstrap/bootout is the modern pair; load is the fallback for older
# systems, and both are attempted so the script works across macOS versions.
launchctl bootout "gui/$(id -u)/${AGENT_LABEL}" 2>/dev/null || true
if launchctl bootstrap "gui/$(id -u)" "${AGENT_PLIST}" 2>/dev/null; then
  loaded="bootstrap"
else
  launchctl unload "${AGENT_PLIST}" 2>/dev/null || true
  launchctl load "${AGENT_PLIST}"
  loaded="load"
fi

# ---------------------------------------------------------------------------
# The menu bar: a visible face for a background service
# ---------------------------------------------------------------------------
# The endpoint is a LaunchAgent with no icon, so "is it running?" is otherwise
# only answerable from the web page. If a Swift compiler is present, build the
# menu-bar app and start it at login too. Optional: a Mac without the toolchain
# still gets the endpoint, just without the icon.
MENUBAR_LABEL="com.aes67-srt.menubar"
MENUBAR_PLIST="${AGENT_DIR}/${MENUBAR_LABEL}.plist"
MENUBAR_BIN="${APP_DIR}/aes67-srt-menubar"
menubar="skipped (no Swift compiler)"
if [[ "${WITH_MENUBAR}" -eq 1 ]] && command -v swiftc >/dev/null 2>&1; then
  if swiftc -O -o "${MENUBAR_BIN}.tmp" "${REPO_ROOT}/src/mac/menubar.swift" 2>"${APP_DIR}/menubar-build.log"; then
    install -m 0755 "${MENUBAR_BIN}.tmp" "${MENUBAR_BIN}"
    rm -f "${MENUBAR_BIN}.tmp"
    cat > "${MENUBAR_PLIST}" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key>
  <string>${MENUBAR_LABEL}</string>
  <key>ProgramArguments</key>
  <array>
    <string>${MENUBAR_BIN}</string>
    <string>--port</string>
    <string>$(python3 -c "import json;print(json.load(open('${CONFIG}'))['http_port'])" 2>/dev/null || echo 8082)</string>
    <string>--title</string>
    <string>aes67</string>
  </array>
  <key>RunAtLoad</key>
  <true/>
  <key>KeepAlive</key>
  <true/>
</dict>
</plist>
PLIST
    launchctl bootout "gui/$(id -u)/${MENUBAR_LABEL}" 2>/dev/null || true
    launchctl bootstrap "gui/$(id -u)" "${MENUBAR_PLIST}" 2>/dev/null ||
      launchctl load "${MENUBAR_PLIST}" 2>/dev/null || true
    menubar="installed"
  else
    menubar="build failed; see ${APP_DIR}/menubar-build.log"
  fi
elif [[ "${WITH_MENUBAR}" -eq 0 ]]; then
  menubar="skipped (--no-menubar)"
fi

echo
echo "installed:"
echo "  binary  ${BINARY}"
echo "  config  ${CONFIG}"
echo "  agent   ${AGENT_PLIST}  (${loaded})"
echo "  menubar ${menubar}"
echo "  log     ${LOG_DIR}/endpoint.log"
echo
echo "next:"
if [[ "${blackhole}" == "missing" ]]; then
  echo "  1. Install BlackHole 64ch (needs sudo): https://existential.audio/blackhole/"
  echo "     then:  sudo killall coreaudiod"
else
  echo "  1. BlackHole 64ch is installed."
fi
echo "  2. Check ${CONFIG}: link.peer is the appliance, passphrase is shared,"
echo "     audio.device is the one a DAW will record from."
echo "  3. If the endpoint is blocked by Gatekeeper (an unsigned build), see the"
echo "     guide in docs/runbooks/macos-endpoint.md."
echo "  4. In the DAW, record from ${DEVICE}; stream channel 1 is input 1."
