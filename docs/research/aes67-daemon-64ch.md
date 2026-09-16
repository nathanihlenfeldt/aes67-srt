# aes67-daemon at 64 channels, verified on a Pi 5

Status: **researched** 2026-09-16 on `rpi5-nathan` (Raspberry Pi 5 Model B Rev 1.1, kernel 6.18.34),
for ticket 03 (issue #4). Every claim here comes from a running daemon and from the ALSA device it
exposes — measured, not read. The raw report is attached to issue #18.

## The answer that mattered

**`plughw:RAVENNA` accepts 64 channels of `S24_3LE` at 48 kHz.** The audio module's central
assumption holds, and it is no longer an assumption.

```
$ arecord -D plughw:RAVENNA --dump-hw-params -f S24_3LE -r 48000 -c 64 -d 1 /dev/null
CHANNELS:      [1 10000]
SAMPLE_BITS:   [4 64]
RATE:          [4000 4294967295)
PERIOD_TIME:   [125 8708)
FORMAT:        ... S24_3LE S24_3BE ... S32_LE ... FLOAT_LE ... (a long list)
Recording WAVE '/dev/null' : Signed 24 bit Little Endian in 3bytes, Rate 48000 Hz, Channels 64
```

Read those limits as *declared* ranges rather than tested ceilings: 10000 channels and a 4 GHz rate
are the driver being permissive, not capable. What is proven is the case we need — 64 channels,
48 kHz, `S24_3LE`, opened, and recording started.

## The daemon's surface

Binary `/usr/local/bin/aes67-daemon`, config `/etc/daemon.conf`, status file `/etc/status.json`, web
UI directory `/usr/local/share/aes67-daemon/webui`. `GET /api/config` returns, among others:

| Setting | Value | Why it matters here |
|---|---|---|
| `interface_name` | `eth0` | The shipped default is `lo`, which never sees PTP or RTP. This had to be fixed during provisioning |
| `tic_frame_size_at_1fs` | **48** | 1 ms at 48 kHz — the same figure as our `audio.period_frames` |
| `sample_rate` | 48000 | |
| `max_tic_frame_size` | 1024 | |
| `rtp_mcast_base` / `rtp_port` | `239.1.0.1` / `5004` | Where our own sources would be announced |
| `ptp_domain` | 0 | The domain must match the site's grandmaster |
| `sap_interval` | 30 | Discovery cadence |
| `streamer_enabled` | `false` | Set by provisioning: it would capture the RAVENNA device |
| `auto_sinks_update` | `false` | Set by provisioning: it can retarget a sink we wired deliberately |
| `nmos_enabled` | `false` | |
| `http_port` | 8080 | |

`GET /api/ptp/status` → `{"status":"locked","gmid":"6C-DF-FB-FF-FE-01-92-5C","jitter":9}` — **PTP is
locked**, so the device is properly clocked and could carry audio now.

`GET /api/sinks` and `/api/sources` are both empty, which is correct for a fresh install.
`GET /api/browse/sources/all` is the interesting one.

## What the network already offers

Two SAP-announced senders, both from `10.10.80.20`:

| Announcement | Multicast | Channels | Payload | Reference clock |
|---|---|---|---|---|
| `AES67-TX-1` | `233.254.57.0` | **8** — `L24/48000/8` | L24 | `6C-DF-FB-FF-FE-01-92-5C`, domain 0 |
| `AES67-TX-2-qsys` | `239.1.0.99` | 1 — `L24/48000/1` | L24 | same |

Three things fall out of that table:

**The eight-channel sender is exactly one of our blocks.** It is the ideal first thing to subscribe
to: it exercises the block premise end to end without needing 64 channels first, and `L24/48000/8`
with `a=ptime:1` is precisely the shape the specification assumes.

**Both senders reference the grandmaster we are locked to.** In other words this bench is a *single*
clock domain, which is the ideal case — and **not** what the WAN link will have. ADR 0003 exists
precisely because the two ends will be independent, so nothing measured here can validate or
invalidate the clock work.

**The announcements declare `a=recvonly`**, which looks wrong for a transmitter and is not: AES67's
convention is to write the sender's SDP from the receiver's perspective. Worth knowing before
somebody "fixes" it.

## Unresolved

- **The 64-channel mapping** — which AES67 stream carries which device channels — cannot be read from
  a running instance with no sinks or sources configured. It is the first task of ticket 09, where a
  sink is created for real; doing it as research first would duplicate that work.
- **The fake daemon's required surface for CI** — **done**, ticket 09 (`src/aes67/fake_daemon_client.*`).
  Written from the API list above, and its answers are this report's: the config table, the locked
  grandmaster, and the two senders below with the SDPs they announce. The one thing CI cannot check
  until the Pi session is that the *real* daemon accepts the sink and source documents we build.
- **Which fields a source document must carry** is the one thing the sibling could not lend us.
  `aes67-sip` sets `ttl`, `dscp`, `payload_type` and `refclk_ptp_traceable` from its own
  configuration; this appliance has no such settings and therefore sends none of them, on the
  reasoning that inventing a site's multicast policy is worse than omitting a field. **Whether the
  daemon defaults them or zeroes them is the first thing to measure on the Pi**, because a zeroed
  `payload_type` would produce silence with nothing reporting it. Ticket 09, the real-daemon half.
- **Whether 64 channels actually *stream*** — this opened the device and started recording. It did
  not verify continuity, underruns or that 64 channels of AES67 arrive. Tickets 09 and 10.
- **The daemon's version** is not in this report. Provisioning cloned `master`; the commit should be
  recorded, and the measurement script should read it from `/opt/aes67-linux-daemon`.

## What this changes

1. **The audio module's plan stands.** 64 channels of `S24_3LE` at 48 kHz is what the device takes,
   and the daemon's own frame size is the 1 ms period the specification specifies.
2. **A real end-to-end audio test can happen on this one Pi.** An 8-channel AES67 source is on the
   network, PTP is locked, and the device works — so the first audio through our code does not need a
   second appliance or a WAN link.
3. **Two provisioning settings are load-bearing**, not preferences: `interface_name` off `lo`, and
   `auto_sinks_update` disabled. Both are set by `scripts/install-daemon.sh` and both would look like
   mysterious breakage if they were not.
