# aes67-daemon at 64 channels, verified on a Pi 5

Status: **researched** 2026-09-16 on `rpi5-nathan` (Raspberry Pi 5 Model B Rev 1.1, kernel 6.18.34),
for ticket 03 (issue #4), and **extended 2026-09-17** by a second session that ran our own binary
against the daemon instead of measuring it from outside with `curl`. Every claim here comes from a
running daemon and from the ALSA device it
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

## Second session, 2026-09-17: our own binary in the path

The first session measured the daemon from outside. This one ran the appliance against it —
`bash scripts/measure-hardware.sh --commission-loopback` on the same Pi — and the report is attached to
issue #18. What it changed:

**Our source document is refused, and the daemon names the field:**

```
error commissioning: cannot publish source 0 (block 0):
  AES67 daemon returned HTTP 400: error parsing JSON: No such node (ttl)
```

That answers the question this document called "the first thing to measure on the Pi" — and the answer
is **neither** of the two it offered. The daemon does not default `ttl` and does not zero it: **it
rejects the document.** So the four fields `aes67-sip` sends from its own configuration (`ttl`, `dscp`,
`payload_type`, `refclk_ptp_traceable`) are required rather than optional, and the reasoning here that
omitting them was safer than inventing a site's multicast policy was **wrong in outcome**: omitting
gets a 400 naming a field, and carries no audio either way. The lesson is in the shape of the mistake —
"inventing a policy" and "omitting a required field" are not the two options; reading the daemon's own
schema was.

**`streamer_enabled: false` did not block the PUT.** The daemon parsed far enough to complain about the
document, so that setting is not what refused us. Whether it blocks *streaming* from a source we hand
it is still open.

**The device streams, and our backend drives it.** Ten seconds of
`arecord -f S24_3LE -r 48000 -c 64 -d 10` completed in ten seconds of wall clock with no overruns —
which closes the "did not verify continuity" gap below. And **our own binary opened the device and
reported `capture RUNNING, playback RUNNING`**: both substreams triggered, which is what
`RavennaBackend::start_stream()` exists to produce and which nothing had confirmed on hardware until
now.

**PTP is locked**, jitter 329 this time against 9 in the first session. Whether that is load or the
moment is unknown; it is a number to watch rather than a finding.

**Versions:** `aes67-daemon bondagit-4.0.1`, driver `ravenna-alsa-lkm/d4f77d4` (from `dkms status` —
the script's own `git rev-parse` was refused as root-owned and now overrides it one-shot rather than
editing git's config, which this script promises not to do).

**The SDPs are now copied exactly.** The real announcements carry payload type **96**, not 98, plus
`i=Channels 1-8` and a repeated session id in `o=`; the fake had 98 and the sibling's shape. It mirrors
the captured sender field for field now.

## Unresolved

- **What the daemon requires of a source document, beyond `ttl`.** `ttl` is the first field it named
  and there may be more behind it. **The authority is the daemon's own source on the Pi**
  (`/opt/aes67-linux-daemon`, bondagit-4.0.1): read the parser rather than discovering one field per
  round trip, which is what the omission above cost.
- **The 64-channel mapping** — which AES67 stream carries which device channels — is what our documents
  *declare* in `map`, and the daemon has not yet accepted a single one of ours. It stays open until a
  commissioning run gets past the document.
- **`streamer_enabled: false`** did not refuse the PUT, but whether a source handed to the daemon
  actually *streams* with it false is unmeasured. The next commissioning run answers it: a zero in
  "sinks receiving" with the document accepted points at this and nothing else.
- **Whether 64 channels of AES67 arrive.** The device streams; that audio arrives *from the network* is
  untested until a sink is subscribed for real. Tickets 09 and 10.
- **The fake daemon's required surface for CI** — **done**, ticket 09
  (`src/aes67/fake_daemon_client.*`), and now including the SDP shape above. What CI cannot check is
  whether the *real* daemon accepts what we build, which is the item at the top of this list.

## What this changes

1. **The audio module's plan stands.** 64 channels of `S24_3LE` at 48 kHz is what the device takes,
   and the daemon's own frame size is the 1 ms period the specification specifies.
2. **A real end-to-end audio test can happen on this one Pi.** An 8-channel AES67 source is on the
   network, PTP is locked, and the device works — so the first audio through our code does not need a
   second appliance or a WAN link.
3. **Two provisioning settings are load-bearing**, not preferences: `interface_name` off `lo`, and
   `auto_sinks_update` disabled. Both are set by `scripts/install-daemon.sh` and both would look like
   mysterious breakage if they were not.
