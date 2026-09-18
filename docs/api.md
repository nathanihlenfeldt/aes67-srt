# API

The appliance serves its own control surface on `http_addr`:`http_port` (default `0.0.0.0:8082`).
There is **one polling status endpoint plus a configuration document**, as the specification asks — no
websockets, no SSE — so the appliance stays debuggable with `curl`.

> **Authentication is not implemented.** Spec open item 1 (shared password, per-user accounts, TLS, or
> tunnel-only) is still unanswered, and until it is, this API should be reachable only from a trusted
> network. That is a known gap, not an oversight.

## Conventions

- Every response is `application/json` except errors, which are `text/plain` with a message that names
  the offending field (`egress.delay_ms: expected 0..5000.000000 ms, got -1.000000 ...`).
- Status codes: `400` a request that cannot be honoured (validation), `404` unknown path, `500` local
  failure (a file that cannot be written), `502` the daemon refused, `503` the engine is not running.
- The daemon (`aes67-daemon`) is a black box reached over REST by the appliance, not by the browser
  (decision 11). The `/api/aes67/*` routes below are the appliance's own window onto it.

## `GET /api/version`

```json
{ "name": "aes67-srt", "version": "0.1.0", "build": "srt=1 alsa=1 assert=0" }
```

## `GET /api/status`

Everything the page shows, in one document. Polled once a second.

```json
{
  "name": "aes67-srt", "version": "0.1.0", "build": "srt=1 alsa=1 assert=0",
  "role": "duplex", "mode": "caller", "peer": "remote.example.com:9000",
  "blocks": 8, "channels": 64, "config_path": "/etc/aes67-srt.conf",
  "pending_restart": ["link"],
  "preflight": {
    "ok": false,
    "delay_ms": 120.0,
    "checks": [
      { "name": "ptp",    "ok": false, "detail": "not locked: locking" },
      { "name": "daemon", "ok": true,  "detail": "127.0.0.1:8080" },
      { "name": "device", "ok": true,  "detail": "plughw:RAVENNA 64ch @48000Hz ..." },
      { "name": "link",   "ok": true,  "detail": "caller duplex" }
    ]
  },
  "engine": {
    "running": true, "link_open": true,
    "frames_sent": 1236, "frames_received": 1234, "frames_refused": 0,
    "silence_periods": 121,
    "delay_ms": 120.0, "delay_fraction": 0.12,
    "egress_delay_ms": 0.0, "av_delay_ms": 240.0,
    "clock_offset_ppm": -0.05, "clock_ratio": 0.99999994,
    "test_signal_channel": -1
  },
  "link": {
    "available": true, "rtt_ms": 12.4, "bandwidth_mbps": 74.5,
    "receive_rate_mbps": 74.3, "receive_buffer_ms": 118,
    "negotiated_latency_ms": 120, "send_buffer_ms": 120,
    "packets_received": 4801, "packets_lost": 0,
    "packets_retransmitted": 6, "packets_dropped": 0
  }
}
```

**Preflight leads with PTP**, and an unlocked slave is `ok: false` for the check and for the whole
preflight: it is the commonest way the appliance looks healthy and produces silence. A daemon that does
not answer reports PTP as *unknown* rather than guessing "unlocked", because those are different fixes.
`link.available` is false on a loopback, which has no statistics to report.

**Reading the link fields.** `packets_retransmitted` is the receiver-side count of packets that
arrived as retransmissions — loss **recovered**, the companion to `packets_lost` (loss detected).
SRT has no cumulative `pktRcvRetransTotal`; this is the interval field `pktRcvRetrans`, which
accumulates because we call `srt_bstats` without clearing (`srt.h:313` is the sender's
`pktRetransTotal`, and reading *that* on a receiver always reports zero — it did, before this was
fixed). `bandwidth_mbps` is SRT's **estimate** of link capacity, which live mode only roughs in and
which is meaningless on a receiver we measured sitting at 6,165 "Mb/s" over a Wi-Fi link. The number
to trust is `receive_rate_mbps`.

`pending_restart` lists the configuration sections that were saved but need a restart to take effect.

## `GET /api/log?lines=N`

```json
{ "lines": ["12:00:00.000 info aerospace ...", "..."] }
```

The in-memory tail (default 200, capped at 500). The UI tails it, because the person diagnosing the
appliance is on the other end of a browser.

## `GET /api/config` · `POST /api/config`

`GET` returns the running configuration document, in the same shape as the file on disk.

`POST` **replaces the whole document**. It is validated first by the same parser the command line uses,
so an unknown key is an error that names it (`unknown key "link.typo"`), and nothing is written or
applied on a refusal. On success the file is written by replace (temp + rename) and the response says
what happened:

```json
{ "ok": true, "applied": ["egress.delay_ms"], "restart_required": ["link"],
  "path": "/etc/aes67-srt.conf" }
```

- `applied` — fields that took effect immediately. Today that is only `egress.delay_ms`.
- `restart_required` — sections that changed and only take effect when the appliance restarts. The
  running state does **not** change, and `pending_restart` in `/api/status` keeps saying so.

An empty `config_path` (a run that was not given `-c`) means the document is validated and applied but
not persisted; the response's `path` is empty.

## `POST /api/egress/delay`

```json
{ "delay_ms": 250.0 }   ->   { "ok": true, "delay_ms": 250.0 }
```

The A/V offset, adjustable while audio runs. Validated `0..5000`; audio can be delayed, never advanced.
It moves through the delay line's crossfade, so it does not click. Applied by the receive loop on its
next period; the value shown in the response and in `/api/status` is the one requested.

## `POST /api/egress/test-signal`

Fires the impulse on `egress.test_signal_channel`, sample-accurate. `400` naming the field when no
channel is selected (`-1`).

```json
{ "ok": true }
```

## `GET /api/aes67/status`

The daemon window: PTP, what we publish, what we subscribed, and what was discovered.

```json
{
  "endpoint": "127.0.0.1:8080", "reachable": true,
  "ptp": { "status": "locked", "gmid": "6C-DF-FB-FF-FE-01-92-5C", "jitter": 9 },
  "sources": [ { "id": 0, "name": "aes67-srt block 0", "codec": "L24" } ],
  "sinks": [ { "id": 0, "in_use": true, "receiving": true } ],
  "discovered": [
    { "name": "AES67-TX-1", "address": "10.10.80.20", "id": "sap:...", "sdp": "v=0\r\n..." }
  ]
}
```

`sources` is one per block, published by commissioning. `sinks` are the ones we subscribed, each with
whether the daemon has a stream there (`in_use`) and whether it is receiving RTP (`receiving`).
`discovered` is everything the daemon heard (SAP/mDNS), with the SDP a sink needs. If the daemon is
unreachable, `reachable` is false and the lists are empty.

## `POST /api/aes67/subscribe`

```json
{ "block": 0, "source": "AES67-TX-1" }   ->   { "ok": true, "block": 0, "source": "AES67-TX-1" }
```

Wires one block's sink to a discovered announcement by name, or to our own source with `"self"`. Uses
the same sink document the commissioning path builds, so the two cannot drift. An unknown name
(`source: nothing discovered is named "..."`) or a block out of range is refused by name. The sink's
`receiving` state then appears in `/api/aes67/status`.

## `POST /api/aes67/unsubscribe`

```json
{ "block": 0 }   ->   { "ok": true, "block": 0 }
```

## `POST /api/aes67/publish`

Re-publishes one source per block. Commissioning does this at start; the button is for a daemon that was
restarted underneath a running appliance.

```json
{ "ok": true, "sources": 8 }
```

## Not yet in this document

- **Authentication** (spec open item 1) — see the warning above.
- **Per-channel gain/mute**: only per-block `gain_db` and `mute` exist today, and they are restart-only.
- **Live meters**: the page shows counters, not per-block levels.
