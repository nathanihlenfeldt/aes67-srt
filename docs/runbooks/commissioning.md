# Runbook: commissioning a site and a studio

Putting the product on two real ends: an appliance on the production site's AES67 fabric, an endpoint
in the studio, and an SRT link between them. This is the procedure a commissioning run walks, and the
decisions each site has to make. It is the field companion to the spec's *Deployment* and *What it
expects* sections.

## Before you go

- **The link**, per the spec: **wired Ethernet at both ends**, a low-jitter internet connection
  (~74 Mbit/s per direction for PCM, ~1–8 Mbit/s for Opus), and no 5G/LTE or consumer satellite. If
  the link cannot carry PCM, encode with Opus.
- **A shared passphrase** for the SRT link. It is the one part of the product that crosses the public
  internet, and it is encrypted even behind firewalls. Both ends use the same secret.
- **Both ends behind routers/firewalls**, never taking a public WAN IP. The API is LAN-only (the spec's
  *Security, and what it assumes*).
- **The model**: one end is the **listener**, the other the **caller**. The listener is normally the
  site (its address is the stable one); the studio calls it. Either way works, and `rendezvous` is for
  when neither end can accept a connection.

## Decide these before you touch the gear

These are the spec's open items, and each site answers them rather than the product:

1. **PTP at each end.** Is there a grandmaster on the AES67 VLAN, and which **domain** does each end
   use? Without lock there is no audio. Check it, do not assume it — the daemon reports lock at
   `GET /api/ptp/status`, and the preflight page leads with it.
2. **Addressing.** A **static IP or DHCP reservation** for the appliance on the AES67 VLAN, and the
   same for the studio Mac. The SRT address each end dials (`link.peer`) is fixed by this.
3. **How the far end subscribes to our streams.** SAP/RAVENNA discovery, or a hand-configured
   multicast address and port? If discovery is off, the far end needs the addresses below written in
   by hand.
4. **The block-to-stream mapping.** See the next section — the convention is usually all a site needs,
   but confirm it.

## The block-to-stream mapping, and the convention

An AES67 block is **eight channels on one multicast stream**. The convention, which is what
`fill_default_blocks` and the daemon's sources produce, is:

| Block | Device channels | AES67 multicast |
|---|---|---|
| 0 | 0–7 | 239.1.0.1 |
| 1 | 8–15 | 239.1.0.2 |
| … | … | … |
| 7 | 56–63 | 239.1.0.8 |

Each stream is **L24, 48 kHz, payload type 98, DSCP 34, TTL 15, 48 samples per packet**, with the
channel map `[0..7]` (stream channel *n* carries block channel *n*). A site that wants a different
mapping configures `blocks[].channels` on the appliance; the daemon's source document is generated to
match, so the mapping is declared once. The far-end device must subscribe to the streams it needs, and
this is the mapping to give it.

## Commissioning the appliance (the site)

On a freshly flashed Pi, run the installer (`scripts/install.sh`, or the `curl | sudo bash` line in the
spec). It is idempotent and its **preflight report** is the first check: PTP lock, daemon reachable, the
64-channel device present, the SRT port reachable, the config valid. Then:

1. **Set the link in `/etc/aes67-srt.conf`**: `role` (tx to feed the studio, rx to take audio from it,
   duplex for both), `mode` (listener or caller), `peer` (the other end, if caller), the **passphrase**,
   and `codec` per block (`pcm_l24` by default, or `opus` for a link that cannot carry PCM).
2. **Confirm PTP is locked** and the RAVENNA device is present — `aplay -l` names Merging RAVENNA, and
   the preflight page shows the lock.
3. **Confirm the capture carries audio**, not silence: `arecord -D plughw:RAVENNA -c 64 -f S24_3LE`
   and check a channel's level set. (Capture the *full* channel count; a smaller `-c` can read silence.)
4. **Restart the service** and read the log: it should say the link is up and the direction it runs.

## Commissioning the endpoint (the studio)

On the Mac, `scripts/install-mac.sh --peer <appliance>:<port> --passphrase <secret> --role rx`. Install
**BlackHole 64ch** and `sudo killall coreaudiod` (the one sudo step). The runbook
`docs/runbooks/macos-endpoint.md` has the detail, including the Gatekeeper step for the unsigned build.
In the DAW, record from BlackHole 64ch: **stream channel 1 is DAW input 1**.

## The check, end to end

- The preflight page on the appliance is green, PTP locked.
- The studio end receives: `frames_received` climbing, `frames_refused` 0, and a delay near the
  configured latency.
- **Audio, not silence**, on the expected channels: play a known source at the site and confirm it in
  the DAW on the matching input.
- **Watch it under load** with `scripts/soak-link.sh <endpoint> <seconds>`: it reports the receive rate,
  loss, retransmissions, drops and delay, and fails if the link stops delivering or the delay trips the
  alarm.
- **Pull the cable and put it back.** The link should re-establish itself with no restart (issue #21);
  audio resumes. If it does not, that is a bug worth a ticket, not a workaround.

## When something is wrong

- **Silence, but the link is up**: PTP not locked (the commonest), the far end not subscribed to our
  streams, or a block mapped to channels the source does not carry. `scripts/collect-diagnostics.sh` on
  the appliance gathers this in one file, with the passphrase redacted.
- **Audio with gaps**: the link is losing packets faster than the delay window can recover — a Wi-Fi or
  congested path. The soak script names it; the fix is a wired/low-jitter link or Opus.
- **The studio hears the wrong channels**: check the block-to-stream mapping in both places.