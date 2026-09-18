#!/usr/bin/env bash
#
# One-command installer for the aes67-srt appliance.
#
#   curl -fsSL https://raw.githubusercontent.com/nathanihlenfeldt/aes67-srt/main/scripts/install.sh \
#     | sudo bash
#
# Target platform: a clean 64-bit Raspberry Pi OS (Bookworm) or Ubuntu 22.04/24.04 on
# arm64/x86_64. The script is idempotent: re-running it updates the checkout, rebuilds,
# and leaves an existing /etc/aes67-srt.conf alone.
#
# It installs, in order:
#   1. build dependencies                      (scripts/install-deps.sh)
#   2. the RAVENNA kernel module and aes67-daemon, with their sysctls, CPU governor,
#      and PulseAudio masked                   (scripts/install-daemon.sh, --skip-daemon
#                                               to leave the daemon alone)
#   3. this appliance: build, test, install the binary and the systemd unit
#   4. the service user, and the unit enabled to start on boot
# and then prints a preflight report naming PTP, daemon, device, link and configuration.
#
# MODELLED ON A TESTED INSTALLER. The sibling aes67-sip has an installer that has run
# on real Pis; the daemon half here is that project's scripts/install-daemon.sh,
# reduced and kept as its own script so it can be reviewed and run alone.
#
# NOT YET RUN ON A CLEAN PI. The pieces it calls have been run on hardware, but the
# orchestration has not had a from-scratch run; treat the first one as a rehearsal and
# read the preflight report at the end rather than the exit status alone.
#
set -euo pipefail

REPO_URL="${AES67_SRT_REPO:-https://github.com/nathanihlenfeldt/aes67-srt}"
REF="${AES67_SRT_REF:-main}"
SRC_DIR="/opt/aes67-srt-src"
PREFIX="/usr/local"
CONFIG_FILE="/etc/aes67-srt.conf"
SYSTEMD_UNIT="/etc/systemd/system/aes67-srt.service"
SERVICE_USER="aes67-srt"

SKIP_DEPS=0
SKIP_DAEMON=0
START_SERVICES=1
DRY_RUN=0

C_OK=$'\033[32m'; C_WARN=$'\033[33m'; C_ERR=$'\033[31m'; C_OFF=$'\033[0m'
log()  { printf '%s==>%s %s\n' "$C_OK" "$C_OFF" "$*"; }
warn() { printf '%s[warn]%s %s\n' "$C_WARN" "$C_OFF" "$*" >&2; }
die()  { printf '%s[error]%s %s\n' "$C_ERR" "$C_OFF" "$*" >&2; exit 1; }

usage() {
  sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
  cat <<'EOF'

Options:
  --ref <ref>        git ref to build (default: main)
  --skip-deps        do not install build dependencies
  --skip-daemon      do not install the RAVENNA module or aes67-daemon
  --skip-start       install and enable, but do not start the service
  --dry-run          print the commands without running them
  -h, --help         this message
EOF
}

for arg in "$@"; do
  case "${arg}" in
    --ref) REF="$2"; shift ;;
    --skip-deps) SKIP_DEPS=1 ;;
    --skip-daemon) SKIP_DAEMON=1 ;;
    --skip-start) START_SERVICES=0 ;;
    --dry-run) DRY_RUN=1 ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown option: ${arg}" ;;
  esac
done

run() {
  if [[ ${DRY_RUN} -eq 1 ]]; then
    printf '    would run: %s\n' "$*"
  else
    "$@"
  fi
}

report_line() { printf '  %-22s %s\n' "$1" "$2"; }

# ---------------------------------------------------------------------------
# 1. platform preflight
# ---------------------------------------------------------------------------
log "checking the platform"
[[ "$(id -u)" -eq 0 ]] || die "run with sudo: this installs a kernel module and services"
[[ -r /etc/os-release ]] || die "cannot read /etc/os-release: unknown platform"
# shellcheck disable=SC1091
. /etc/os-release
case "${ID}:${VERSION_ID:-}" in
  raspbian:*|debian:12|debian:13|ubuntu:22.04|ubuntu:24.04) ;;
  *) warn "untested platform ${ID} ${VERSION_ID:-}: continuing anyway" ;;
esac
case "$(uname -m)" in
  aarch64|x86_64|arm64) ;;
  *) die "unsupported architecture $(uname -m): need a 64-bit userland" ;;
esac
command -v apt-get >/dev/null 2>&1 || die "this installer is for Debian-derived systems"
command -v systemctl >/dev/null 2>&1 || die "systemctl is missing: systemd is required for the service"
report_line "platform" "${PRETTY_NAME:-${ID}} $(uname -m)"

# ---------------------------------------------------------------------------
# 2. the checkout
# ---------------------------------------------------------------------------
log "fetching ${REPO_URL} (${REF}) into ${SRC_DIR}"
if [[ -d "${SRC_DIR}/.git" ]]; then
  run git -C "${SRC_DIR}" fetch --depth 1 origin "${REF}"
  run git -C "${SRC_DIR}" checkout -q FETCH_HEAD
else
  run mkdir -p "$(dirname "${SRC_DIR}")"
  run git clone --depth 1 --branch "${REF}" "${REPO_URL}" "${SRC_DIR}"
fi

# ---------------------------------------------------------------------------
# 3. dependencies, and the daemon half
# ---------------------------------------------------------------------------
if [[ ${SKIP_DEPS} -eq 0 ]]; then
  log "installing build dependencies"
  run bash "${SRC_DIR}/scripts/install-deps.sh"
else
  report_line "dependencies" "skipped (--skip-deps)"
fi

if [[ ${SKIP_DAEMON} -eq 0 ]]; then
  log "installing the RAVENNA kernel module and aes67-daemon (20-40 minutes)"
  run bash "${SRC_DIR}/scripts/install-daemon.sh"
else
  report_line "daemon" "skipped (--skip-daemon): PTP, the device and discovery must exist already"
fi

# ---------------------------------------------------------------------------
# 4. service user
# ---------------------------------------------------------------------------
log "creating the service user"
if id "${SERVICE_USER}" >/dev/null 2>&1; then
  report_line "user" "${SERVICE_USER} already exists"
else
  run useradd --system --home "/var/lib/${SERVICE_USER}" --shell /usr/sbin/nologin \
    --groups audio "${SERVICE_USER}"
fi

# ---------------------------------------------------------------------------
# 5. build and test
# ---------------------------------------------------------------------------
log "building"
run cmake -S "${SRC_DIR}" -B "${SRC_DIR}/build" -DCMAKE_BUILD_TYPE=Release
run cmake --build "${SRC_DIR}/build" --parallel 4
log "testing"
run ctest --test-dir "${SRC_DIR}/build" --output-on-failure

# ---------------------------------------------------------------------------
# 6. install the binary, the configuration and the unit
# ---------------------------------------------------------------------------
log "installing"
run install -d "${PREFIX}/bin"
run install -m 0755 "${SRC_DIR}/build/aes67-srt" "${PREFIX}/bin/aes67-srt"

if [[ -f "${CONFIG_FILE}" ]]; then
  report_line "configuration" "${CONFIG_FILE} exists: left alone"
else
  log "writing the sample configuration to ${CONFIG_FILE}"
  run install -m 0640 "${SRC_DIR}/config/aes67-srt.conf" "${CONFIG_FILE}"
fi
# The control surface saves configuration back to this file, so the service user
# has to own it; the unit's ReadWritePaths then lets it write.
run chown "${SERVICE_USER}:${SERVICE_USER}" "${CONFIG_FILE}"

if [[ -d "${SRC_DIR}/webui/dist" ]]; then
  run install -d "${PREFIX}/share/aes67-srt/webui"
  run cp -r "${SRC_DIR}/webui/dist/." "${PREFIX}/share/aes67-srt/webui/"
  report_line "web ui" "installed from webui/dist"
else
  report_line "web ui" "none built: the appliance serves its built-in status page"
fi

run install -m 0644 "${SRC_DIR}/systemd/aes67-srt.service" "${SYSTEMD_UNIT}"

# ---------------------------------------------------------------------------
# 7. enable and start
# ---------------------------------------------------------------------------
run systemctl daemon-reload
run systemctl enable aes67-srt
if [[ ${START_SERVICES} -eq 1 ]]; then
  run systemctl restart aes67-srt
else
  report_line "service" "enabled, not started (--skip-start)"
fi

# ---------------------------------------------------------------------------
# 8. preflight report
# ---------------------------------------------------------------------------
log "preflight report"
report_line "configuration" "$("${PREFIX}/bin/aes67-srt" -c "${CONFIG_FILE}" --validate 2>&1 || true)"
report_line "daemon" "$(systemctl is-active aes67-daemon 2>&1)"
if command -v curl >/dev/null 2>&1; then
  ptp="$(curl -fsS --max-time 3 http://127.0.0.1:8080/api/ptp/status 2>/dev/null || echo '')"
  report_line "ptp" "${ptp:-daemon unreachable on 127.0.0.1:8080}"
  status="$(curl -fsS --max-time 3 http://127.0.0.1:8082/api/status 2>/dev/null || echo '')"
  if [[ -n "${status}" ]]; then
    report_line "link" "$(printf '%s' "${status}" | python3 -c 'import json,sys;d=json.load(sys.stdin);p=d["preflight"];print("preflight", "ok" if p["ok"] else "FAILED")' 2>/dev/null || echo 'status did not parse')"
  else
    report_line "link" "the control surface is not answering on 127.0.0.1:8082"
  fi
else
  report_line "link" "curl is not installed: cannot probe the daemon or the link"
fi
if command -v aplay >/dev/null 2>&1; then
  if aplay -l 2>/dev/null | grep -qi ravenna; then
    report_line "device" "plughw:RAVENNA present"
  else
    report_line "device" "no RAVENNA card in 'aplay -l'"
  fi
else
  report_line "device" "alsa-utils is not installed: cannot list cards"
fi
report_line "service" "$(systemctl is-active aes67-srt 2>&1) / $(systemctl is-enabled aes67-srt 2>&1)"

cat <<EOF

The appliance runs at http://<this-host>:8082/ . The preflight page there leads with
PTP: an unlocked slave is the commonest way it looks healthy while producing silence.
Send the output of scripts/collect-diagnostics.sh if anything is wrong.

Security, and what it assumes: this appliance belongs behind a router or firewall
and never takes a public WAN IP. The API above is LAN-only — no auth, no TLS — and
exposing it to the internet is not supported; reach it over a VPN or tunnel if you
need remote management. The SRT link to the far end carries a passphrase
(link.passphrase in ${CONFIG_FILE}): set it, and use the same one at both ends.

Licence: GPL-3.0 (ADR 0002). Distribution carries the source-availability obligation,
which is why this script installs from the repository rather than a bundled binary.
EOF
