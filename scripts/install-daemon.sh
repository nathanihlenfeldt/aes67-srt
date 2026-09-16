#!/usr/bin/env bash
#
# Provisions a Raspberry Pi so that the RAVENNA device and aes67-daemon exist, and
# sections 4 and 5 of scripts/measure-hardware.sh can finally be measured.
#
#   sudo bash install-daemon.sh [--dry-run]
#
# WHAT THIS DOES, and why it needs root:
#
#   1. build dependencies (cmake, boost, avahi, alsa, dkms, kernel headers)
#   2. kernel parameters AES67 needs (igmp_max_memberships, RT runtime)
#   3. the CPU governor pinned to performance by a systemd unit, surviving reboots
#   4. PulseAudio masked: it destabilises the RAVENNA device
#   5. the Merging RAVENNA/AES67 kernel module, built through DKMS for this kernel
#   6. aes67-daemon built from source, with its service, config and user
#
# It installs a kernel module and system services. Nothing else.
#
# MODELLED ON A TESTED INSTALLER. The sibling project aes67-sip has an install.sh
# that has been run on real Pis, and every step below follows it - including two
# details that are not obvious and that it learned the hard way: DKMS must be told
# the module lands in driver/, and the daemon's config must be pointed at the real
# network interface because its default is "lo", which never sees PTP or RTP. If
# this script fails, that one is the fallback:
#
#   git clone --depth 1 https://github.com/nathanihlenfeldt/aes67-sip /tmp/aes67-sip
#   sudo bash /tmp/aes67-sip/scripts/install.sh --skip-gateway
#
# NOT TESTED ON YOUR HARDWARE. This is a faithful reduction of a working script,
# but it has never run on a Pi itself. It is readable in a few minutes and it
# prints every command it runs.
#
# Expect 20-40 minutes: building aes67-daemon against Boost on a Pi is the slow part.
#
set -euo pipefail

DRY_RUN=0
for arg in "$@"; do
  case "${arg}" in
    --dry-run) DRY_RUN=1 ;;
    -h|--help) sed -n '2,34p' "$0"; exit 0 ;;
    *) echo "unknown option: ${arg}" >&2; exit 2 ;;
  esac
done

log()  { printf '\n==> %s\n' "$*"; }
warn() { printf '    ! %s\n' "$*" >&2; }

run() {
  if [[ ${DRY_RUN} -eq 1 ]]; then
    printf '[dry-run] %s\n' "$*"
  else
    printf '+ %s\n' "$*"
    "$@"
  fi
}

if [[ "$(id -u)" -ne 0 ]]; then
  echo "error: run as root. This installs a kernel module." >&2
  exit 1
fi

if [[ ! -r /etc/os-release ]]; then
  echo "error: cannot identify this system" >&2
  exit 1
fi
# shellcheck disable=SC1091
. /etc/os-release
ARCH="$(uname -m)"
log "target: ${PRETTY_NAME:-unknown OS}, ${ARCH}, kernel $(uname -r)"
case "${ARCH}" in
  aarch64|arm64|x86_64) ;;
  armv7l) warn "32-bit armv7: the RAVENNA module targets it but 64-bit is recommended" ;;
  *) warn "unexpected architecture ${ARCH}; continuing anyway" ;;
esac

# ---------------------------------------------------------------------------
# 1. Build dependencies
# ---------------------------------------------------------------------------
BASE_PACKAGES=(
  build-essential clang cmake ninja-build git curl ca-certificates pkg-config bc
  python3
  libasound2-dev alsa-utils linuxptp libssl-dev
  libavahi-client-dev libsystemd-dev libboost-all-dev
  dkms iproute2 net-tools
)
log "installing build dependencies"
run apt-get update -qq
run env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq "${BASE_PACKAGES[@]}"

# The module cannot be built without headers for the running kernel, and the
# package name differs between Raspberry Pi OS and Ubuntu.
if ! run env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
     "linux-headers-$(uname -r)"; then
  warn "no linux-headers-$(uname -r); trying linux-headers-generic"
  run env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq linux-headers-generic ||
    warn "install kernel headers by hand or the kernel module cannot be built"
fi

# ---------------------------------------------------------------------------
# 2. Kernel parameters AES67 needs
# ---------------------------------------------------------------------------
# igmp_max_memberships: one membership per AES67 stream joined. The daemon supports
# up to 64 streams and the default (20) runs out quietly.
# sched_rt_runtime_us: give real-time audio threads the whole CPU budget.
# perf_cpu_time_max_percent: frequency-scaling events disturb AES67 streams.
log "configuring kernel parameters"
if [[ ${DRY_RUN} -eq 0 ]]; then
  tee /etc/sysctl.d/90-aes67.conf >/dev/null <<'EOF'
# Added by aes67-srt/scripts/install-daemon.sh
net.ipv4.igmp_max_memberships = 66
kernel.sched_rt_runtime_us = 1000000
kernel.perf_cpu_time_max_percent = 0
EOF
fi
run sysctl --system >/dev/null 2>&1 || warn "sysctl --system reported an error"

# ---------------------------------------------------------------------------
# 3. CPU governor pinned to performance, across reboots
# ---------------------------------------------------------------------------
# Not a tuning nicety: RAVENNA's own documentation warns that scaling events cause
# "unexpected distortion for a few seconds", and every CPU measurement in this
# project is only comparable with the cores running flat out.
log "pinning the CPU governor to performance"
GOVERNOR_UNIT=/etc/systemd/system/aes67-cpu-governor.service
if [[ ${DRY_RUN} -eq 0 ]]; then
  tee "${GOVERNOR_UNIT}" >/dev/null <<'EOF'
[Unit]
Description=Pin the CPU governor to performance for glitch free AES67 audio
DefaultDependencies=no
After=sysinit.target local-fs.target
Before=multi-user.target aes67-daemon.service

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/bin/sh -c 'for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do [ -w "$f" ] && echo performance > "$f"; done'
ExecStop=/bin/true

[Install]
WantedBy=multi-user.target
EOF
fi
run systemctl daemon-reload
run systemctl enable --now aes67-cpu-governor || warn "could not enable the governor unit"
printf '    governor now: %s\n' \
  "$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo n/a)"

# ---------------------------------------------------------------------------
# 4. PulseAudio must not touch the RAVENNA device
# ---------------------------------------------------------------------------
log "masking PulseAudio"
if command -v pulseaudio >/dev/null 2>&1; then
  run systemctl --global mask pulseaudio.service pulseaudio.socket 2>/dev/null || true
  run systemctl mask pulseaudio.service pulseaudio.socket 2>/dev/null || true
  run pkill -x pulseaudio 2>/dev/null || true
fi
if command -v pipewire >/dev/null 2>&1; then
  warn "PipeWire is installed: make sure it does not grab hw:RAVENNA"
fi

# ---------------------------------------------------------------------------
# 5. The Merging RAVENNA/AES67 kernel module, through DKMS
# ---------------------------------------------------------------------------
RAVENNA_LKM_REPO="https://github.com/bondagit/ravenna-alsa-lkm"
RAVENNA_LKM_BRANCH="aes67-daemon"

log "checking whether the RAVENNA module is already loaded for this kernel"
if lsmod | grep -q '^MergingRavennaALSA' &&
   dkms status -m ravenna-alsa-lkm 2>/dev/null | grep -q "$(uname -r).*installed"; then
  printf '    already loaded and DKMS-managed for %s\n' "$(uname -r)"
else
  log "building the Merging RAVENNA/AES67 kernel module (DKMS)"
  LKM_DIR=/usr/src/ravenna-alsa-lkm
  if [[ -d "${LKM_DIR}/.git" ]]; then
    run git -C "${LKM_DIR}" fetch --depth 1 origin "${RAVENNA_LKM_BRANCH}"
    run git -C "${LKM_DIR}" checkout -q FETCH_HEAD
  else
    run rm -rf "${LKM_DIR}"
    run git clone --depth 1 --branch "${RAVENNA_LKM_BRANCH}" \
      "${RAVENNA_LKM_REPO}" "${LKM_DIR}"
  fi
  LKM_VERSION="$(git -C "${LKM_DIR}" rev-parse --short HEAD 2>/dev/null || echo 1.0)"
  DKMS_SRC="/usr/src/ravenna-alsa-lkm-${LKM_VERSION}"

  if [[ ${DRY_RUN} -eq 0 ]]; then
    rm -rf "${DKMS_SRC}"
    mkdir -p "${DKMS_SRC}"
    cp -a "${LKM_DIR}/." "${DKMS_SRC}/"
    rm -rf "${DKMS_SRC}/.git"
    cat > "${DKMS_SRC}/dkms.conf" <<EOF
PACKAGE_NAME="ravenna-alsa-lkm"
PACKAGE_VERSION="${LKM_VERSION}"
BUILT_MODULE_NAME[0]="MergingRavennaALSA"
# The module is built by kbuild from driver/ (obj-m lives in driver/Makefile), so
# DKMS has to be told where the .ko lands. Without this it reports "Make sure the
# name and location of the generated module are correct" although the compile
# succeeded - a confusing half hour if you have not met it before.
BUILT_MODULE_LOCATION[0]="driver"
DEST_MODULE_LOCATION[0]="/updates/dkms"
MAKE[0]="make -C \${kernel_source_dir} M=\${dkms_tree}/ravenna-alsa-lkm/${LKM_VERSION}/build/driver modules"
CLEAN="make -C \${kernel_source_dir} M=\${dkms_tree}/ravenna-alsa-lkm/${LKM_VERSION}/build/driver clean"
EOF
  fi

  run dkms remove -m ravenna-alsa-lkm -v "${LKM_VERSION}" --all || true
  run dkms add -m ravenna-alsa-lkm -v "${LKM_VERSION}"
  if ! run dkms build -m ravenna-alsa-lkm -v "${LKM_VERSION}"; then
    warn "the DKMS build failed. This driver is known good on 6.6/6.8 LTS and may"
    warn "  not compile on a newer kernel. To iterate by hand:"
    warn "    cd ${LKM_DIR}/driver && make CC=clang"
    warn "  log: /var/lib/dkms/ravenna-alsa-lkm/${LKM_VERSION}/build/make.log"
    exit 1
  fi
  run dkms install -m ravenna-alsa-lkm -v "${LKM_VERSION}" --force || true

  if [[ ${DRY_RUN} -eq 0 ]]; then
    tee /etc/modules-load.d/ravenna.conf >/dev/null <<'EOF'
# Added by aes67-srt/scripts/install-daemon.sh
MergingRavennaALSA
EOF
  fi
  run modprobe MergingRavennaALSA || warn "modprobe MergingRavennaALSA failed"

  # From kernel 6.15 the 1 ms audio tick is a soft hrtimer, which paces RTP in
  # bursts rather than evenly (bondagit/ravenna-alsa-lkm issue 39). Worth knowing
  # before blaming our own code for a stuttering stream.
  KERNEL_MAJOR="$(uname -r | cut -d. -f1)"
  KERNEL_MINOR="$(uname -r | cut -d. -f2)"
  if (( KERNEL_MAJOR > 6 )) || { (( KERNEL_MAJOR == 6 )) && (( KERNEL_MINOR >= 15 )); }; then
    warn "kernel $(uname -r) runs the RAVENNA audio tick as a soft hrtimer, so RTP"
    warn "  may be emitted in bursts. If endpoints stutter, use an LTS kernel or"
    warn "  raise sink_delay_samples and the endpoints' playout buffers."
  fi
fi

# ---------------------------------------------------------------------------
# 6. aes67-daemon
# ---------------------------------------------------------------------------
DAEMON_REPO="https://github.com/bondagit/aes67-linux-daemon"
DAEMON_DIR=/opt/aes67-linux-daemon
PREFIX=/usr/local
# Boost wants roughly a gigabyte per compiler process, and a build that dies
# halfway through is a bad way to spend an evening.
JOBS="${JOBS:-2}"

log "fetching aes67-daemon"
if [[ -d "${DAEMON_DIR}/.git" ]]; then
  run git -C "${DAEMON_DIR}" fetch --depth 1 origin master
  run git -C "${DAEMON_DIR}" checkout -q FETCH_HEAD
else
  run rm -rf "${DAEMON_DIR}"
  run git clone --depth 1 "${DAEMON_REPO}" "${DAEMON_DIR}"
fi
run git -C "${DAEMON_DIR}" submodule update --init --recursive --depth 1

log "building aes67-daemon with ${JOBS} compiler processes (the slow part)"
if [[ ${DRY_RUN} -eq 0 ]]; then
  cmake -S "${DAEMON_DIR}/daemon" -B "${DAEMON_DIR}/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBoost_NO_WARN_NEW_VERSIONS=1 \
    -DCPP_HTTPLIB_DIR="${DAEMON_DIR}/3rdparty/cpp-httplib" \
    -DRAVENNA_ALSA_LKM_DIR="${DAEMON_DIR}/3rdparty/ravenna-alsa-lkm" \
    -DWITH_AVAHI=ON -DWITH_SYSTEMD=ON -DWITH_STREAMER=ON -DFAKE_DRIVER=OFF
  cmake --build "${DAEMON_DIR}/build" -j"${JOBS}"
  install -m 0755 "${DAEMON_DIR}/build/aes67-daemon" "${PREFIX}/bin/aes67-daemon"
fi

# The daemon repo's own systemd/install.sh assumes a full ./build.sh layout and
# uses paths relative to its own directory, so it fails when called from anywhere
# else. Install the four things the service actually needs instead.
log "installing the service, its user and its config"
run getent group audio >/dev/null || run groupadd --system audio
run id aes67-daemon >/dev/null 2>&1 || run useradd --system -g audio -M -l \
  -s /usr/sbin/nologin aes67-daemon -c "AES67 Linux daemon"
run install -d -o aes67-daemon -g audio /var/lib/aes67-daemon \
  /usr/local/share/aes67-daemon/scripts /usr/local/share/aes67-daemon/webui
if [[ -f "${DAEMON_DIR}/daemon/scripts/ptp_status.sh" ]]; then
  run install -m 0755 -o aes67-daemon -g audio \
    "${DAEMON_DIR}/daemon/scripts/ptp_status.sh" \
    /usr/local/share/aes67-daemon/scripts/
fi
if [[ ${DRY_RUN} -eq 0 ]]; then
  if [[ -f "${DAEMON_DIR}/systemd/daemon.conf" && ! -f /etc/daemon.conf ]]; then
    install -m 0644 -o aes67-daemon -g audio "${DAEMON_DIR}/systemd/daemon.conf" \
      /etc/daemon.conf
  fi
  if [[ -f "${DAEMON_DIR}/systemd/status.json" && ! -f /etc/status.json ]]; then
    install -m 0644 -o aes67-daemon -g audio "${DAEMON_DIR}/systemd/status.json" \
      /etc/status.json
  fi
fi
if [[ -f "${DAEMON_DIR}/systemd/aes67-daemon.service" ]]; then
  run install -m 0644 "${DAEMON_DIR}/systemd/aes67-daemon.service" \
    /etc/systemd/system/aes67-daemon.service
else
  warn "aes67-daemon.service not found under ${DAEMON_DIR}/systemd"
fi
run systemctl daemon-reload

# ---------------------------------------------------------------------------
# 7. Point the daemon at the real network interface
# ---------------------------------------------------------------------------
# The shipped config uses "lo", which never receives PTP or RTP, so a fresh install
# reports PTP unlocked and carries nothing. This step is mandatory on a fresh
# machine, not tuning.
PRIMARY_IF="$(ip route show default 2>/dev/null | awk '/default/ {print $5; exit}')"
if [[ -z "${PRIMARY_IF}" ]]; then
  warn "cannot detect the default interface: set interface_name in /etc/daemon.conf"
elif [[ ${DRY_RUN} -eq 0 && -f /etc/daemon.conf ]]; then
  printf '    setting interface_name to %s\n' "${PRIMARY_IF}"
  python3 - /etc/daemon.conf "${PRIMARY_IF}" <<'PY'
import json, sys
path, iface = sys.argv[1], sys.argv[2]
try:
    cfg = json.load(open(path))
except Exception as exc:
    sys.exit(f"cannot parse {path}: {exc}")
if cfg.get("interface_name") in (None, "", "lo"):
    cfg["interface_name"] = iface
# The streamer captures the RAVENNA device, which would fight our own capture path.
cfg["streamer_enabled"] = False
# A daemon that rewrites sink SDP from discovered announcements can retarget a sink
# this project programmed deliberately, so it must not touch sink wiring.
cfg["auto_sinks_update"] = False
cfg.setdefault("mdns_enabled", True)
cfg.setdefault("sap_mcast_addr", "239.255.255.255")
cfg.setdefault("sample_rate", 48000)
cfg.setdefault("tic_frame_size_at_1fs", 48)
json.dump(cfg, open(path, "w"), indent=2, sort_keys=True)
open(path, "a").write("\n")
print(f"interface_name={cfg['interface_name']} streamer_enabled=False "
      f"auto_sinks_update=False")
PY
fi

log "starting the daemon"
run systemctl enable aes67-daemon || warn "could not enable aes67-daemon"
# restart rather than start: `enable --now` does nothing to a unit that is already
# running, which is how a rebuild ends up running the previous binary.
run systemctl restart aes67-daemon ||
  warn "could not start aes67-daemon; check journalctl -u aes67-daemon"

if [[ ${DRY_RUN} -eq 0 ]]; then
  printf '\n==> waiting for the daemon API on 127.0.0.1:8080\n'
  for attempt in $(seq 1 30); do
    if curl -fsS --max-time 2 http://127.0.0.1:8080/api/ptp/status >/dev/null 2>&1; then
      printf '    answering after %s attempt(s)\n' "${attempt}"
      break
    fi
    sleep 1
  done
fi

# ---------------------------------------------------------------------------
# 8. What you should see now
# ---------------------------------------------------------------------------
printf '\n==> verification\n'
printf -- '--- cards\n'
aplay -l 2>/dev/null | grep -i ravenna || warn "no RAVENNA card yet"
printf -- '--- service\n'
systemctl is-active aes67-daemon 2>/dev/null || true
printf -- '--- ptp\n'
curl -fsS --max-time 3 http://127.0.0.1:8080/api/ptp/status 2>&1 | head -c 400
printf '\n'

cat <<'NEXT'

==> done. Now re-run the measurement, which can finally take sections 4 and 5:

      bash /tmp/measure.sh

PTP will report "unlocked" until a grandmaster is present on that interface. The
ALSA device exists either way, which is what sections 4 and 5 need - and it is the
section 4 answer that decides whether the audio module's central assumption holds.
NEXT

