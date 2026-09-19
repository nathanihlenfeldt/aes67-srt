# Troubleshooting

Work from the page first: the **Preflight** tells you which layer is at fault, so you do not hunt in the
wrong one. This is ordered by what people actually hit.

## "Everything looks fine but there is no audio"

The most common failure, and almost always one of these:

1. **PTP is not locked.** The page's preflight leads with it. No PTP, no audio, however healthy the rest
   looks. The site's AES67 network is not reaching a grandmaster — a network problem, not an appliance
   one. The PTP state is on the **AES67 daemon** card, and `GET /api/aes67/status` reports it directly.
2. **The far end is not subscribed to the site's streams.** The appliance publishes eight AES67 streams
   (block *i* → `239.1.0.i+1`); the site's console has to send to the appliance. The **AES67 daemon** card
   shows the sources and whether anything is being received.
3. **The blocks map to channels that carry nothing.** The live audio is on the device channels the block
   lists. If the block carries channels 0–7 but the console sends on 8–15, you get silence. Use **Impulse
   test signal** or a meter to find which channel actually has signal.

If the link is up and audio flows but you hear nothing: **the Mac still needs something to play
BlackHole.** BlackHole is a device, not a speaker. Arm the DAW's inputs, or route BlackHole to an output.

## The link will not come up

The page shows `link_open: false`, or the state sits at **starting**.

- **A listener shows `starting` until a caller connects.** That is not stuck.
- **One end is a listener, the other a caller.** Two listeners, or two callers, never meet. (Use
  `rendezvous` if neither can accept.)
- **`link.peer` is an IPv4 address, not a name.** Names are not resolved.
- **The passphrase must match exactly at both ends.** A mismatch fails the connection.
- **A firewall between them** must allow the SRT traffic; SRT works through NAT, but an outright block
  does not.
- **The port is already in use** at this end (another instance). The page's config refuses a bad port;
  check nothing else holds it.

## Audio with gaps or dropouts

The link is losing packets faster than the delay can recover. The link line names it:

| On the page | What it means |
|---|---|
| **packets lost** rising, **retransmitted** keeping pace | The link is lossy but recovering. If `dropped` stays 0, nobody heard it. |
| **dropped** rising | Packets arrived too late to play. This is audible — a tick or a gap. |
| **receive buffer** climbing toward the latency | The receiver cannot drain as fast as the sender fills; a stall is coming. |
| **delay** alarm | The link has been behind long enough that latency grew past the threshold. |

The fixes, in order: **wire it** (Wi-Fi is the usual cause); give the link **more latency** (it buys
retransmission time); or **compress** it (Opus), which lowers the bitrate the link must carry.

`scripts/soak-link.sh http://<address>:<port> <seconds>` watches exactly these numbers over time.

## The delay is high, or climbing

- **A fresh Opus link takes a minute to settle.** The level converges one 20 ms frame at a time; a link
  that looks idle for a little while is normal.
- **Delay growing** is the design working: when the link sags, delay grows and the page alarms rather
  than dropping audio. If it alarms often, the link is the problem (see above).
- **`clock correction` pinned at ±200 ppm** means the two ends' clocks differ by more than the correction
  can absorb. On a real link this settles; if it does not, note it as a bug.
- **Total delay includes the codec.** Opus adds its frame + ~6.5 ms; that is expected and goes into the
  A/V number, not the network's.

## The channels are wrong, or in the wrong order

- **Stream channel 1 is DAW input 1.** The first block is channels 0–7; the second 8–15, and so on.
- **Check the block mapping** on both ends. On the appliance, `blocks[].channels` says which device
  channels a block carries, and the daemon's published streams are generated to match. A remap is a
  restart-only change.

## Studio Mac problems

- **No BlackHole device.** `sudo killall coreaudiod` after installing BlackHole.
- **The DAW hears itself / feedback.** One BlackHole **sums what is played into it**, so a single device
  cannot carry both directions. Run one direction at a time (see [Use cases](use-cases.md)).
- **The menu-bar icon is missing.** It is installed with the endpoint (`--no-menubar` skips it, and a Mac
  without a Swift compiler gets no icon). Re-run `scripts/install-mac.sh`.
- **The endpoint is blocked by Gatekeeper.** It is unsigned; see
  `docs/runbooks/macos-endpoint.md` for the step to allow it.

## The service will not start

- **A bad configuration.** Validate it: `aes67-srt -c /etc/aes67-srt.conf --validate`. The reason names
  the offending field.
- **The port is taken**, or the device is not present. The preflight says which.
- **PTP or the RAVENNA device missing** — the daemon half. Check `sudo systemctl status aes67-daemon`.

## Asking for help

1. **`scripts/collect-diagnostics.sh`** on the appliance — one file, read-only, passphrase redacted. Send
   that.
2. **The logs:** `sudo journalctl -u aes67-srt` on the appliance; `~/Library/Logs/aes67-srt/endpoint.log`
   on the Mac.
3. **Say which layer:** PTP, device, link, or the DAW. The preflight tells you — and a report that names
   the layer gets a faster answer than one that says "no audio".