#!/usr/bin/env bash
#
# Collects everything needed to diagnose an aes67-srt appliance in one go.
#
#   sudo ./scripts/collect-diagnostics.sh > /tmp/aes67-srt-diag.txt
#
# It is read-only: no configuration is changed, no service is restarted. The link
# passphrase is redacted, so the output is safe to paste into a ticket or a chat.
#
set -uo pipefail

if [[ "$(id -u)" -ne 0 ]]; then
  echo "note: running without root, some sections will be incomplete (use sudo)" >&2
fi

section() { printf '\n===== %s =====\n' "$*"; }
have() { command -v "$1" >/dev/null 2>&1; }

section "host"
uname -a
grep -E '^(PRETTY_NAME|VERSION_ID)=' /etc/os-release 2>/dev/null
uptime

section "appliance version"
/usr/local/bin/aes67-srt --version 2>&1

section "services"
for service in aes67-daemon aes67-srt; do
  printf '%-16s %-12s %s\n' "${service}" \
    "$(systemctl is-active "${service}" 2>&1)" \
    "$(systemctl is-enabled "${service}" 2>&1)"
done

section "configuration (passphrase redacted)"
if [[ -f /etc/aes67-srt.conf ]]; then
  python3 - <<'PY'
import json

try:
    config = json.load(open('/etc/aes67-srt.conf'))
except Exception as exc:
    print(f'cannot parse /etc/aes67-srt.conf: {exc}')
else:
    link = config.get('link')
    if isinstance(link, dict) and link.get('passphrase'):
        link['passphrase'] = '***redacted***'
    print(json.dumps(config, indent=2, sort_keys=True))
PY
else
  echo "/etc/aes67-srt.conf is missing"
fi

section "preflight and status"
if have curl; then
  curl -fsS --max-time 5 http://127.0.0.1:8082/api/status 2>/dev/null |
    python3 -m json.tool 2>/dev/null | head -160 ||
    echo "the control surface is unreachable on 127.0.0.1:8082"
else
  echo "curl is not installed"
fi

section "AES67 sources, sinks and discovered"
if have curl; then
  curl -fsS --max-time 5 http://127.0.0.1:8082/api/aes67/status 2>/dev/null |
    python3 -c '
import json, sys
try:
    data = json.load(sys.stdin)
except Exception as exc:
    print("the AES67 panel did not return JSON:", exc)
else:
    print("reachable:", data.get("reachable"), "endpoint:", data.get("endpoint"))
    print("ptp:", data.get("ptp"))
    print("sources:", [s.get("name") for s in data.get("sources", [])])
    for sink in data.get("sinks", []):
        print("  sink", sink.get("id"), "in_use:", sink.get("in_use"),
              "receiving:", sink.get("receiving"))
    for found in data.get("discovered", []):
        print("  discovered:", found.get("name"), found.get("address"))
' 2>/dev/null || echo "the AES67 panel is unreachable"
fi

section "aes67-daemon"
if have curl; then
  printf 'ptp: '; curl -fsS --max-time 3 http://127.0.0.1:8080/api/ptp/status 2>&1; echo
  echo "sinks:"; curl -fsS --max-time 3 http://127.0.0.1:8080/api/sinks 2>/dev/null |
    python3 -m json.tool 2>/dev/null | head -40
  echo "sources:"; curl -fsS --max-time 3 http://127.0.0.1:8080/api/sources 2>/dev/null |
    python3 -m json.tool 2>/dev/null | head -40
fi

section "journal: aes67-srt (last 60)"
journalctl -u aes67-srt -n 60 --no-pager 2>/dev/null

section "journal: aes67-srt (link, clock, refusal lines, last 500)"
journalctl -u aes67-srt -n 500 --no-pager 2>/dev/null |
  grep -iE 'link:|refused|overrun|underrun|silence|clock|playout buffer|preflight|commission' |
  tail -40

section "journal: aes67-daemon (last 15)"
journalctl -u aes67-daemon -n 15 --no-pager 2>/dev/null

section "ALSA / RAVENNA device"
if have aplay; then
  aplay -l 2>/dev/null | grep -i ravenna || echo "no RAVENNA card in 'aplay -l'"
fi
lsmod | grep -i ravenna || echo "the MergingRavennaALSA module is not loaded"
if have arecord; then
  echo "-- 64 channels, S24_3LE (the appliance's shape):"
  arecord -D plughw:RAVENNA -c 64 -f S24_3LE -r 48000 -d 1 /tmp/aes67-srt-probe.wav 2>&1 | tail -2
fi

section "kernel parameters"
sysctl kernel.sched_rt_runtime_us kernel.perf_cpu_time_max_percent \
  net.ipv4.igmp_max_memberships 2>/dev/null
echo "scaling governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo n/a)"

section "network"
ip -brief address show 2>/dev/null
echo
ip route show default 2>/dev/null

section "appliance link"
if have ldd; then
  ldd /usr/local/bin/aes67-srt 2>/dev/null | grep -i 'not found' ||
    echo "all shared libraries resolved"
fi

section "resources"
free -m 2>/dev/null

echo
echo "done. Paste this output (it contains no passphrase)."
