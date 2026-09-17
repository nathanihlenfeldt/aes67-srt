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
rejects the document.**

**And the rest of the schema was read from the daemon's own parser rather than discovered one round
trip at a time.** `daemon/json.cpp` at the installed commit (`bondagit-4.0.1`, `68bd278`) reads a source
with `pt.get<T>(...)` for *every* field, and `pt.get` throws when a node is missing — so nothing is
defaulted and nothing is zeroed: a document that omits a field is refused. The function carries its own
template as a comment, and those are the values we now send:

```json
"map": [ 0, 1, 2, 3, 4, 5, 6, 7 ],  "max_samples_per_packet": 48,
"codec": "L24",  "address": "",
"ttl": 15,  "payload_type": 98,  "dscp": 34,  "refclk_ptp_traceable": false
```

We were sending the first four and missing the last four. The sibling `aes67-sip` sends 15/98/34 for the
same reason — it read this template — so those numbers were never its invention, and our "inventing a
site's multicast policy" reasoning was aimed at the wrong danger. **Omitting a required field is not the
conservative choice; it is a 400.**

The sink document was already complete: `json_to_sink` requires `name`, `io`, `source`, `use_sdp`,
`sdp`, `delay`, `ignore_refclk_gmid`, `map`, and ours has all eight.

`payload_type` 98 pairs with `codec` L24 in the template. L16's payload type is **97 by AES67
convention and is not confirmed by the template**, which shows only L24 — v1 ships L24, so the L16 case
is unverified.

There is now a test for each document asserting every field the parser reads, so the next omission fails
in CI rather than on hardware. It was checked against a deliberate mutant: dropping `ttl` fails it with
`[json.exception.out_of_range.403] key 'ttl' not found`.

**Open, and now narrower:** TTL and DSCP are arguably *site* policy rather than daemon defaults —
multicast scope and QoS marking — and making them configurable is an open item, since the spec's
configuration schema has no such fields.

**Third run, with the corrected document: the source is accepted and the sink is not.**
`GET /api/sources` afterwards holds our stream, echoed back by the daemon:

```json
{ "id": 0, "enabled": true, "name": "aes67-srt block 0", "codec": "L24",
  "address": "239.1.0.1", "ttl": 15, "payload_type": 98, "dscp": 34,
  "refclk_ptp_traceable": false, "map": [ 0, 1, 2, 3, 4, 5, 6, 7 ] }
```

**That is the first thing of ours the real daemon has accepted**, and `address` is the tell: we send
`""` and it chose `239.1.0.1` from its own multicast base, which is what the empty string asks for.

The self-subscription failed one layer deeper than the document:

```
HTTP 400: failed to add sink 0 : (driver) command failed
```

Not a JSON error — `session_manager::add_sink` parses the SDP and then asks the kernel module to add
the stream, and the driver declined. So **the RAVENNA driver will not put a sink on this box's own
source's multicast group.** The mechanism that the spec calls the "commissioning loopback" does not
work as described on this driver. It is kept as a code path and asserted in CI against the fake, with
the refusal recorded where it is implemented; the way to prove the receive half is to subscribe to a
real announcement, which is also what a site does — and there is one on this network.

**The device streams, and our backend drives it.** Ten seconds of
`arecord -f S24_3LE -r 48000 -c 64 -d 10` completed in ten seconds of wall clock with no overruns —
which closes the "did not verify continuity" gap below. And **our own binary opened the device and
reported `capture RUNNING, playback RUNNING`**: both substreams triggered, which is what
`RavennaBackend::start_stream()` exists to produce and which nothing had confirmed on hardware until
now.

**PTP is locked**, and its jitter is *fleeting* rather than a finding: 9 in the first session, 329 in the
second, 13 in the third, with no change to anything in between. Treat it as a number to watch, not a
measurement of anything.

**Versions:** `aes67-daemon bondagit-4.0.1`, driver `ravenna-alsa-lkm/d4f77d4` (from `dkms status` —
the script's own `git rev-parse` was refused as root-owned and now overrides it one-shot rather than
editing git's config, which this script promises not to do).

**The SDPs are now copied exactly.** The real announcements carry payload type **96**, not 98, plus
`i=Channels 1-8` and a repeated session id in `o=`; the fake had 98 and the sibling's shape. It mirrors
the captured sender field for field now.

## Unresolved

- **Whether the daemon now accepts our documents, and whether the streams carry.** The schema is known
  and encoded in a test (above), so the next commissioning run is the one that says whether the
  corrected source document is accepted, whether the sinks subscribe, and how many report
  `receiving_rtp_packet`. Nothing has got past the document yet on this hardware.
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

## Third session, 2026-09-17: the whole appliance, on the Pi

Same day, later. Everything above that reads "refused" or "unproven" was resolved here, and the
resolution is what this section supersedes it with.

The full report is `hardware-report-rpi5-nathan-20260917-1022.txt` on issue #18. What it settled:

**Our documents are accepted.** Eight sources, each carrying its own block's eight device channels,
with the daemon assigning one multicast group each (`239.1.0.1` … `239.1.0.8`):

```json
{ "id": 1, "name": "aes67-srt block 1", "codec": "L24", "address": "239.1.0.2",
  "ttl": 15, "payload_type": 98, "dscp": 34, "map": [8,...,15] }
```

Why the first attempt was refused is at the top of the previous section: `pt.get<T>(...)` throws on a
missing node, nothing is defaulted, and four fields were missing.

**A sink is accepted and receives.** Not to our own source — that failed for a reason that turned out to
be unrelated — but to the real eight-channel `AES67-TX-1` sender on the network:

```
commissioning: chose the announcement "AES67-TX-1" (eight channels of L24)
commissioning: 8 sources published, 1 sinks subscribed, 1 receiving, PTP locked
```

and once settled, every flag clean:

```
rtp_seq_id_error false, rtp_ssrc_error false, rtp_payload_type_error false,
rtp_sac_error false, receiving_rtp_packet true
```

**The refusal that cost three runs was a playout delay of zero.** `failed to add sink 0 : (driver)
command failed` names nothing; bisecting the one value that differed from the daemon's own web UI gave
`delay 384 -> HTTP 200`, `delay 576 -> HTTP 200`, `delay 0 -> HTTP 400`. This document, and the code,
first reasoned that "the daemon should add no delay because the A/V delay is ours" — which mistook the
mechanism, because this is the sink's *receive buffer* rather than a competing delay line.

**The engine runs on this hardware.** Fifteen seconds of duplex at 64 channels against the real
device, with the transport looped:

```
RAVENNA audio device: plughw:RAVENNA 64ch @48000Hz s24_3le, capture RUNNING, playback RUNNING
engine: stopped after 14908 frames sent, 14906 received, 0 refused
```

14,908 frames in 15.008 s is realtime, and nothing was refused. The unit suite is 93/93 on the Pi
against 94/94 elsewhere, the difference being the platform branch of the ALSA tests.

**`streamer_enabled: false` did not refuse the PUT, the sources, or the sink.** Whether it stops a
source we publish from *transmitting* remains the one unmeasured half — and the self-loopback is the
way to see it, so the two questions are now the same question.

**Still open.** Whether our own sources actually stream: the self-loopback wired all eight sinks and
reported one receiving during the run, then none afterwards, on distinct multicast groups. The daemon's
sources read the ALSA device, which our own process holds open while it runs, so the next look should
watch the sinks *while* the appliance is running rather than after it stops.
