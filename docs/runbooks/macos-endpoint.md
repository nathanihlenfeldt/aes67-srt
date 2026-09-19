# Runbook: the macOS endpoint

The second product: SRT to CoreAudio, so a DAW can record or play the audio this project's appliance
carries. It only ever talks to this project's own appliance — one wire format, no interop matrix — and
it has no AES67, no PTP and no daemon (ADR 0004).

**For installing and running it, the [user manual](../manual/index.md) is the front door** — especially
[Getting started](../manual/getting-started.md) and [Operating it](../manual/operating.md). This
runbook is the field procedure: the manual `install-mac.sh` steps, the two directions, and the
behaviour to expect. What has been proved is in `docs/research/macos-endpoint.md` and on issue #19.

## Installing it on a studio Mac

The endpoint installs **for the current user, with no sudo**, and a LaunchAgent starts it at login and
restarts it if it exits:

```
cmake -S . -B build && cmake --build build --target aes67-srt-mac
./scripts/install-mac.sh --peer <appliance-ip>:<port> --passphrase <shared-secret> --role rx
```

That copies the binary and config to `~/Library/Application Support/aes67-srt`, writes
`~/Library/LaunchAgents/com.aes67-srt.endpoint.plist`, loads it, and logs to
`~/Library/Logs/aes67-srt/endpoint.log`. `scripts/uninstall-mac.sh` reverses it; add `--purge` to
remove the config as well.

**BlackHole is the one step that needs sudo** — it is a CoreAudio driver, not something this installer
can place for you. Install BlackHole 64ch from <https://existential.audio/blackhole/>, then
`sudo killall coreaudiod` so the device appears. The installer checks for it and says so if it is
missing.

**An unsigned build and Gatekeeper.** A binary *downloaded* is marked quarantined and macOS refuses to
run it. Build it on the machine, or clear the mark:

```
xattr -d com.apple.quarantine ~/Library/Application\ Support/aes67-srt/aes67-srt-mac
```

A signed, notarized build removes this step; that is issue #30, not done.

**Then, in the DAW**, record from BlackHole 64ch. Stream channel 1 is input 1, and the live site audio
is on inputs 1–8.

## Security

The endpoint is a LAN device, like the appliance. Its API binds to `127.0.0.1` in the shipped config
and is not meant to be reachable from outside the studio. **The link to the site carries the shared
passphrase** — set it with `--passphrase` at install time, and use the same secret at both ends. It is
the one part of the product that crosses the public internet; see the spec's *Security, and what it
assumes*.

## What you need

- **Wired Ethernet on the Mac.** Not Wi-Fi: even at the default Opus (~1–8 Mbit/s for 64 channels)
  Wi-Fi drops the bursts SRT then has to recover, which the operator hears as loss; at lossless PCM
  (~74 Mbit/s per direction) it is worse. The same goes for the site end — this is a level of the
  stack the appliance cannot fix for you.
- **The Mac**: the endpoint built (`build/aes67-srt-mac`), and **BlackHole 64ch** installed. After
  installing BlackHole, `coreaudiod` must be restarted or the device is invisible:
  ```
  sudo killall coreaudiod
  ```
- **The Pi**: the appliance built (`build/aes67-srt`), with the RAVENNA device and `aes67-daemon` if
  you want real AES67 audio. For a first pass with no audio at all, `audio.backend: null` and
  `aes67_daemon.fake: true` prove the link on both ends without touching hardware.
- **A DAW** on the Mac that can select BlackHole as an input or an output.

## The one thing to decide first: which direction

One BlackHole **sums what is played into it** — its input carries what was written to its output. So a
single BlackHole cannot carry both directions without the endpoint hearing itself, and **duplex is not
supported with one device yet**. Run one direction at a time:

- **Appliance → Mac** (monitor or record the AES67 fabric in a DAW): the Pi transmits, the Mac
  receives. This is the shipped `config/aes67-srt-mac.conf` shape with `role: rx`.
- **Mac → Appliance** (feed the AES67 fabric from a DAW): the Mac transmits, the Pi receives.

Duplex needs two BlackHole instances or two disjoint channel groups on one, and a channel-map feature
the engine does not have yet. That is the next slice, not a config mistake.

## Direction A: appliance → Mac

**On the Pi** — `/tmp/pi-tx.conf`, a caller that transmits the 64 channels:

```json
{
  "version": 1,
  "audio": {"backend": "ravenna", "channels": 64, "device": "plughw:RAVENNA",
            "format": "s24_3le", "period_frames": 48, "periods": 8, "sample_rate": 48000},
  "link": {"alarm_delay_ms": 500, "blocks": 8, "latency_ms": 120, "local_port": 9100,
           "mode": "caller", "passphrase": "", "peer": "<MAC-LAN-IP>:9200", "role": "tx"},
  "aes67_daemon": {"address": "127.0.0.1", "fake": false, "port": 8080},
  "egress": {"delay_ms": 0.0, "test_signal_channel": -1},
  "http_addr": "0.0.0.0", "http_port": 8082
}
```

Start it **after** the Mac listener is up (a caller connects; a listener waits):

```
./build/aes67-srt -c /tmp/pi-tx.conf
```

**On the Mac** — `config/aes67-srt-mac.conf` with `role: rx` and `peer` empty (it listens):

```
./build/aes67-srt-mac -c config/aes67-srt-mac.conf
```

**In the DAW**: select **BlackHole 64ch** as an input. The first capture prompts for **microphone
permission** — macOS gates input from a virtual device, and the DAW will ask.

## Direction B: Mac → Appliance

Swap the roles: the Mac `role: tx` with `mode: caller`, `peer: <PI-IP>:9200`; the Pi `role: rx` with
`mode: listener`. In the DAW, select **BlackHole 64ch** as an *output*.

## Verifying it without a DAW

Both ends serve the same control surface. On the Mac, `http://127.0.0.1:8082/` shows a two-check
preflight (device, link — no daemon or PTP on this product) and the live figures:

```
curl -s http://127.0.0.1:8082/api/status | python3 -m json.tool | head -40
```

What good looks like, measured on 2026-09-17 (Pi tx, Mac rx, null audio, 8 seconds):

```
pi:  engine: stopped after 7969 frames sent, 0 received, 0 refused   (~996/s, 0 packets lost)
mac: engine: stopped after 0 frames sent, 7261 received, 0 refused;
     playout delay 115 ms, clock correction ~6 ppm
```

The Mac's delay settles near **116 ms** against a 120 ms target — the four missing milliseconds are the
resampler's working room, the same figure the appliance reports.

## Known behaviour, so it is not mistaken for a fault

- **Stopping the Mac after the peer has gone can take a few seconds.** The receive call can be parked
  inside libsrt until the connection is declared idle (~5 s), so `Ctrl-C` may take that long. It is
  bounded and it exits; `systemd`'s default `TimeoutStopSec` covers it. The proper fix (an epoll-gated
  or fully non-blocking receive) is a follow-up.
- **The appliance's own service may already be running on the Pi** (`/etc/aes67-srt.conf`, port 8082 and
  its own SRT port). Use a separate config with a different `local_port` and `http_port`, or stop the
  service first, so the two do not collide.
- **No audio at all** with `audio.backend: null` is expected: it carries silence to prove the link.
  Real audio needs the RAVENNA device and a locked PTP domain at the Pi, and a DAW or something playing
  into BlackHole at the Mac.
