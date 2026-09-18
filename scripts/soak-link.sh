#!/usr/bin/env bash
#
# Samples a running link and says whether it is actually carrying audio.
#
#   ./scripts/soak-link.sh [endpoint] [seconds] [sample_seconds]
#   ./scripts/soak-link.sh http://127.0.0.1:18082 3600 5
#
# It polls the appliance's own status endpoint rather than the socket, so it
# measures what an operator sees: receive rate, loss, the retransmissions that
# recover it, the packets given up as too late to play, and the delay. It changes
# nothing and restarts nothing.
#
# Exit status is 0 only if the link was still delivering at the end and the delay
# never tripped the alarm, so it can gate a soak in a pipeline or be watched by
# hand. A link that carries 74 Mbit/s and never drops a packet prints a clean
# summary; a link that sags prints which of the three it did: loss recovered,
# loss given up, or delay climbing.
#
set -uo pipefail

endpoint=${1:-http://127.0.0.1:18082}
seconds=${2:-60}
sample_seconds=${3:-5}

if ! command -v curl >/dev/null 2>&1; then
  echo "soak-link: curl is required" >&2
  exit 2
fi
if ! command -v python3 >/dev/null 2>&1; then
  echo "soak-link: python3 is required to read the status document" >&2
  exit 2
fi

read_status() {
  curl -sf --max-time 4 "$endpoint/api/status" 2>/dev/null | python3 -c '
import json, sys
d = json.load(sys.stdin)
e, l = d.get("engine", {}), d.get("link", {})
if not l.get("available"):
    print("unavailable")
else:
    print("\t".join(str(x) for x in (
        l.get("receive_rate_mbps", 0),
        l.get("packets_received", 0),
        l.get("packets_lost", 0),
        l.get("packets_retransmitted", 0),
        l.get("packets_dropped", 0),
        e.get("delay_ms", 0),
        l.get("receive_buffer_ms", 0),
        e.get("frames_refused", 0),
        e.get("silence_periods", 0),
    )))
' 2>/dev/null
}

printf 'sampling %s every %ss for %ss\n' "$endpoint" "$sample_seconds" "$seconds"
printf '%8s %9s %10s %9s %11s %9s %9s %9s\n' \
  'elapsed' 'Mbps' 'received' 'lost' 'retrans' 'dropped' 'delay_ms' 'buf_ms'

start=$(date +%s)
first=""
last=""
unavailable=0
alarm=0
while :; do
  now=$(date +%s)
  elapsed=$((now - start))
  row=$(read_status)
  if [[ -z "$row" ]]; then
    printf '%8s %s\n' "$elapsed" "no answer from $endpoint"
    unavailable=$((unavailable + 1))
  elif [[ "$row" == "unavailable" ]]; then
    printf '%8s %s\n' "$elapsed" "link not open yet"
  else
    IFS=$'\t' read -r rate received lost retrans dropped delay buffer refused silence <<<"$row"
    printf '%8s %9s %10s %9s %11s %9s %9s %9s\n' \
      "$elapsed" "$rate" "$received" "$lost" "$retrans" "$dropped" "$delay" "$buffer"
    [[ -z "$first" ]] && first="$row"
    last="$row"
    python3 -c 'import sys; sys.exit(0 if float(sys.argv[1]) > 500.0 else 1)' "$delay" && alarm=1
  fi
  if ((elapsed >= seconds)); then
    break
  fi
  sleep "$sample_seconds"
done

if [[ -z "$first" || -z "$last" ]]; then
  echo "soak-link: the link never opened; nothing was measured" >&2
  exit 1
fi

IFS=$'\t' read -r _ recv0 lost0 retr0 drop0 _ _ _ _ <<<"$first"
IFS=$'\t' read -r rate1 recv1 lost1 retr1 drop1 delay1 _ refused1 silence1 <<<"$last"

delivered=$((recv1 - recv0))
lost=$((lost1 - lost0))
retrans=$((retr1 - retr0))
dropped=$((drop1 - drop0))

printf '\nover %ss: delivered %s packets at %s Mbps, lost %s, retransmitted %s, dropped %s\n' \
  "$seconds" "$delivered" "$rate1" "$lost" "$retrans" "$dropped"
printf 'at the end: delay %s ms, buffer %s ms, refused %s, silence periods %s\n' \
  "$delay1" "$buffer" "$refused1" "$silence1"

status=0
if ((delivered <= 0)); then
  echo "FAIL: the link stopped delivering packets" >&2
  status=1
fi
if ((alarm)); then
  echo "FAIL: the delay tripped the $((500)) ms alarm during the run" >&2
  status=1
fi
if ((unavailable > 0)); then
  echo "WARN: $unavailable samples had no status answer" >&2
fi
if ((dropped > 0)); then
  echo "WARN: $dropped packets were too late to play and were discarded" >&2
fi
if ((status == 0)); then
  echo "OK: the link kept delivering and stayed inside the delay budget"
fi
exit "$status"
