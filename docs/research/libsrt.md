# libsrt, verified against v1.5.7

Status: researched 2026-09-16 for ticket 02. Every claim below is cited by file and line at the
pinned tag **`v1.5.7`** (released 2026-08-28, the latest at the time of writing).

## How to reproduce

## Verified against two library versions

Every claim here was checked against `v1.5.7` (the latest release at the time). CI runs against
whatever the distribution ships, and on `ubuntu-24.04` that is **1.5.3** — the transport's loopback
tests pass against both, which is the useful reassurance: the option names, the payload ceiling and
the statistics this project depends on have not moved between those versions.

```
gh api repos/Haivision/srt/releases/latest --jq .tag_name      # v1.5.7
curl -fsS https://raw.githubusercontent.com/Haivision/srt/v1.5.7/srtcore/srt.h
curl -fsS https://raw.githubusercontent.com/Haivision/srt/v1.5.7/docs/API/API-functions.md
curl -fsS https://raw.githubusercontent.com/Haivision/srt/v1.5.7/docs/API/API-socket-options.md
curl -fsS https://raw.githubusercontent.com/Haivision/srt/v1.5.7/docs/API/statistics.md
curl -fsS https://raw.githubusercontent.com/Haivision/srt/v1.5.7/docs/features/encryption.md
curl -fsS https://raw.githubusercontent.com/Haivision/srt/v1.5.7/docs/features/socket-groups.md
curl -fsS https://raw.githubusercontent.com/Haivision/srt/v1.5.7/CMakeLists.txt
```

Note the header path: at this tag the public headers are `srtcore/srt.h` and `srtcore/udt.h`, not
`srt/srt.h` as on `master`. `.clang-format` and CI are not affected, but anyone following
`master`-based directions will fetch 404s — which is why this document pins the tag rather than a
branch.

## What this changes

Five findings that alter decisions we had already made. Read this section first.

**1. A frame cannot be one SRT message, so ADR 0001 is wrong on this point.** Live mode caps a
single send at `SRTO_PAYLOADSIZE`, which "can't be larger than 1456 bytes (1316 default)"
(`docs/API/API-functions.md:1926`; the constants are `SRT_LIVE_DEF_PLSIZE = 1316` and
`SRT_LIVE_MAX_PLSIZE = 1456` at `srtcore/srt.h:295,299`). Our frame is **9312 bytes**, so one frame
spans roughly seven SRT messages. **The frame format itself is unaffected** — it is what we put
*inside* messages — but the transport must fragment and reassemble, and ADR 0001's "one frame per
SRT message" is hereby corrected (see the amendment on that ADR). This is precisely the assumption
ticket 02 existed to check.

**2. `TLPKTDROP` is ON by default in live mode — and turning it off stalls the link.** Its documented
default is "true in Live mode, false in File mode" (`docs/API/API-socket-options.md:1685`), and the
same option "is automatically enabled in sender if receiver supports it"
(`docs/API/API-socket-options.md:1682`). Decision 6's first reading was to force it off at both ends so
no audio is ever dropped. **That reading is wrong, and measured wrong.** A packet that arrives after
its play time cannot be delivered at all: with `TLPKTDROP` off the receiver head-of-line blocks waiting
for a retransmission that no longer helps, stops draining, and the link dies. At 64 channels over a
lossy link, receiving froze after ~2 s (2,126 packets, then flat) while the sender's buffer filled to
1,025 ms. `TLPKTDROP = 1` fixed it: 64 channels now run ~1,000 packets/s continuously. The option
stays **on**, and `pktRcvDrop` (`docs/API/statistics.md:95`, `:162`) is the count of packets too late
to play — zero on a healthy link, the number to watch when the link sags.

**3. Bonding is a build-time opt-in, and incompatible with rendezvous.** `ENABLE_BONDING` defaults
to **OFF** (`CMakeLists.txt:174`), so a distribution's libsrt may not contain the feature at all,
and `SRTO_GROUPCONNECT` is compiled out entirely (`srtcore/srt.h:19,227` region,
`srtcore/core.cpp:935`). Groups also "support only caller-listener mode"
(`docs/API/API-socket-options.md:273`) — which rules out the rendezvous mode decision 5 leans on.
Redundancy therefore stays out of v1, and when it arrives it will not be a drop-in: it changes the
connection model.

**4. `120 ms` is right, but latency is negotiated, not set.** Live mode's default
`SRTO_RCVLATENCY` is **120 ms** (`docs/API/API-socket-options.md:1348`), matching the
specification's `latency_ms: 120`. The *actual* delay is decided during the handshake as "the
maximum of the `SRTO_RCVLATENCY` value and the value of `SRTO_PEERLATENCY` set by the peer"
(`docs/API/API-socket-options.md:1354`). So our 100–200 ms target is a property of the *pair*, not
of one appliance: a peer configured at 400 ms gives a 400 ms link regardless of what we set. The UI
must show the negotiated value, not the configured one.

**5. The UI's numbers already exist as statistics.** `srt_bstats` and `srt_bistats`
(`srtcore/srt.h:868,870`) expose RTT, bandwidth, receive-buffer depth, the TsbPd delay and drop
counters. See the table below.

## Licence

**MPL-2.0**, and it obliges us to nothing.

- `LICENSE:1` reads "Mozilla Public License Version 2.0"; the file is the unmodified MPL 2.0 text
  and the GitHub API reports `MPL-2.0` for the repository.
- MPL-2.0 is **file-level** copyleft: its obligations attach to modified MPL-covered *files* that
  are distributed. We do not modify libsrt, we link it, and MPL-2.0 expressly does not extend to
  the larger work (`LICENSE:373` region, the "Incompatible With Secondary Licenses" notice and the
  Exhibit A notices at `:359`). **Our project may carry any licence**, including a permissive one,
  and including GPL-3.0.

This is a factual finding only. The *choice* of our own licence is the project owner's, and is
recorded separately as ADR 0002 — which is written as a **proposal**, not a decision.

## Message boundaries and the live-mode ceiling

SRT preserves message boundaries, but live mode caps how big a message may be, and that cap is far
smaller than one of our frames.

- **Sending**: "In **live mode**, you are only allowed to send up to the length of
  `SRTO_PAYLOADSIZE`, which can't be larger than 1456 bytes (1316 default)"
  (`docs/API/API-functions.md:1926`). Exceeding it is `SRT_EINVALMSGAPI`. In live and file/message
  mode a successful send always returns `len` — it does not partially send
  (`docs/API/API-functions.md:1937`).
- **Receiving**: live mode "behaves as in **file/message mode**, although the number of bytes
  retrieved will be at most the maximum payload of one MTU" (`docs/API/API-functions.md:1997`). One
  call returns one message's worth of bytes, never a partial message
  (`docs/API/API-functions.md:1990`).
- **Only the message API exists in live mode** (`docs/API/API-socket-options.md:923-924`), which is
  what makes the boundary-preserving behaviour above automatic rather than something we implement.
- **The receiver cannot know our fragment size.** "The `SRTO_PAYLOADSIZE` value configured by the
  sender is not negotiated, and not known to the receiver. The `SRTO_PAYLOADSIZE` value set on the
  SRT receiver is mainly used for heuristics. However, the receiver is prepared to receive the whole
  MTU as configured with `SRTO_MSS`" (`docs/API/API-functions.md:1999-2002`). So a reassembler must
  accept any fragment up to the MSS, not the size we happen to send.

**One ambiguity, resolved by measurement rather than by reading.** The socket-options reference says
of `SRTO_PAYLOADSIZE`: "When set to 0, there's no limit for a single sending call"
(`docs/API/API-socket-options.md:1143`) and gives the range as `0..*`, while the API-functions
reference states the 1456 ceiling for live mode (`:1926`) and the header defines
`SRT_LIVE_MAX_PLSIZE = 1456` (`srtcore/srt.h:299`). The documentation suggests 1316 is merely a
*default* and 1456 is available. **The library disagrees**: sending a 1456-byte message without first
raising the option fails, and this is what it says, verbatim —

```
SRT.cc: LiveCC: payload size: 1456 exceeds maximum allowed 1316
srt_sendmsg: Operation not supported: Incorrect use of Message API (sendmsg/recvmsg)
```

So 1316 is *enforced*, not merely suggested. **`wire::k_max_message_bytes` is therefore 1316 rather
than 1456**, and the transport sets `SRTO_PAYLOADSIZE` to that value explicitly rather than trusting
a default that could move. The cost is one extra message per frame: 16 bytes of SRT header in 9312,
about 0.2%. Had this been met in the field it would have looked like a mystery — a link that works
with one channel and fails with eight.

**What the transport therefore has to do** (ticket 07): fragment a frame across several messages,
and reassemble. The reassembler needs no new format field, because a frame is self-describing — the
28-byte header gives the block count, and each 8-byte block header gives the length of the payload
that follows, so the total length is computable as bytes arrive. SRT guarantees ordering, so the
parser can be a simple accumulator. **The frame format itself is unchanged by all of this**: what a
message contains was never part of ADR 0001, only what a *frame* contains.

## Latency

- Live mode's default `SRTO_RCVLATENCY` is **120 ms**; file mode's is 0
  (`docs/API/API-socket-options.md:1348`). Our spec's `latency_ms: 120` default is therefore
  exactly the library default, which is a good sign rather than a coincidence.
- **Latency is negotiated between the two ends**, as "the maximum of the `SRTO_RCVLATENCY` value and
  the value of `SRTO_PEERLATENCY` set by the peer" (`docs/API/API-socket-options.md:1354`). A peer
  configured at 400 ms produces a 400 ms link no matter what we set. *The UI must display the
  negotiated value, not the configured one* — obtainable as `msRcvTsbPdDelay` (below).
- Latency is not the send-to-receive time. It is "only used to add an extra delay (at the receiver
  side) to the time when the packet 'should' arrive", and that delay "is used to compensate for two
  things: an extra network delay …, or a packet retransmission" (`docs/features/latency.md:41-49`).
  That is precisely the buffer our decision 7 spends: enough delay to absorb retransmits at
  ~148 Mbit/s.
- Time-based delivery is `SRTO_TSBPDMODE`, default **true in live mode, false in file mode**
  (`docs/API/API-socket-options.md:1729`).

**Measured, and the answer is "discarded".** A packet that arrives *after* its play time is gone:
with `TLPKTDROP` off the receiver does not deliver it late, it blocks — and at 64 channels the link
stops delivering altogether (see item 2 above). There is no "delay grows, audio is never lost" mode to
lean on; a packet past its play time is unplayable, and the honest policy is to count it and move on.
**Ticket 07 now owns the opposite assertion**: starve a link deliberately and assert that (a) frames
still flow, (b) the reported delay increases, and (c) any loss shows up in `pktRcvDrop` rather than as a
stall.

## Encryption

- `SRTO_PASSPHRASE`: "Crypto PBKDF2 Passphrase (must be 10..79 characters, or empty to disable
  encryption)" (`srtcore/srt.h:200`). **Our configuration validation already enforces exactly
  10..79** — that rule was written as a guess in ticket 05 and is now verified against the header.
- The cipher is "AES in counter mode (AES-CTR) … with a short lived key", the key being "randomly
  generated by the sender and transmitted within the stream … wrapped with another longer-term key,
  the Key Encrypting Key (KEK)" (`docs/features/encryption.md:95,97`). Key derivation is PBKDF2
  (`docs/features/encryption.md:91`); rotation is governed by `SRTO_KMREFRESHRATE` and
  `SRTO_KMPREANNOUNCE` (`srtcore/srt.h:224-225`).
- **The CPU cost is now measured — on a Pi 5, which is the machine that matters.** `openssl speed -evp
  aes-128-ctr` reports **~3.2 GB/s** in 16 KB blocks and **~2.9 GB/s** in 1 KB blocks, i.e. the ARMv8
  crypto extensions are in use (`OPENSSL_armcap=0xbd`). Our link needs 18.6 MB/s in one direction and
  ~37 MB/s duplex, so **encryption costs about 1% of one core.** A passphrase link is affordable, and
  it should not be made optional for performance reasons. Measured 2026-09-16 on a Raspberry Pi 5
  Model B (kernel 6.18.34, governor `ondemand` — which does not matter here, since the cipher is
  fast enough by a factor of eighty).

## Statistics for the UI

`srt_bstats(sock, &stats, 0)` and `srt_bistats(sock, &stats, 0, 0)` fill an `SRT_TRACEBSTATS`
(`srtcore/srt.h:868,870`). Statistics come in three flavours — accumulated, interval-based, and
instantaneous (`docs/API/statistics.md:34-36`) — so gauges want the instantaneous ones and rates
want the interval-based ones.

| Field | Unit | Direction | What it gives us |
|---|---|---|---|
| `msRcvTsbPdDelay` | ms | receiver | **The negotiated latency — the number the UI must label "delay"** (`statistics.md:123`) |
| `msRcvBuf` | ms | receiver | Buffer depth now: the "delay is growing" signal (`statistics.md:122`) |
| `msRTT` | ms | both | Round-trip time (`statistics.md:110`) |
| `mbpsBandwidth` | Mbps | both | Estimated link capacity (`statistics.md:111`) |
| `mbpsRecvRate` | Mbps | receiver | Actual throughput, to compare against the 74.5 Mbit/s we expect (`statistics.md:90`) |
| `pktRcvDrop` | packets | receiver | **Packets `TLPKTDROP` discarded: must stay 0** (`statistics.md:95`, defined at `:162`) |
| `pktRcvRetrans` | packets | receiver | Retransmissions received — "the link is working for it" (`statistics.md:80`) |
| `pktRcvLoss` | packets | receiver | Presently missing packets (`statistics.md:78`) |
| `msSndBuf` | ms | sender | Send buffer depth (`statistics.md:118`) |

Accumulated totals exist alongside the interval figures (`pktRecvTotal`, `pktRcvLossTotal`,
`pktRetransTotal`, `pktRcvDropTotal`), as do the decryption counters (`pktRcvUndecrypt`,
`pktRcvUndecryptTotal`). **Note `pktRetransTotal` in particular**: the accumulated
receiver-side retransmit counter does *not* follow the `pktRcv…` pattern that its own
interval figure (`pktRcvRetrans`) suggests, so inferring the name from the documentation
produces code that does not compile. The header is the authority (`srtcore/srt.h:313`
against `:338`, verified against the installed 1.5.7). **The whole "show me
the delay" requirement needs no invention: `msRcvTsbPdDelay` is the number, and `msRcvBuf` is its
trend.**

## The receive and send timeouts must be zero, and 0 *is* non-blocking

`SRTO_RCVTIMEO` **"limits the time up to which the receiving operation will block ... The -1 value
means no time limit"** (`docs/API/API-socket-options.md`; default `-1`). The range is `-1, 0..`, so
**0 means a zero-millisecond limit — the call returns immediately with `SRT_ETIMEOUT`.** This is
documented, not inferred, and it matters because the engine's receive loop is paced by the device and
owes it one period every millisecond.

**What a positive timeout cost, measured on a real SRT link on 2026-09-17.** With the original
`SRTO_RCVTIMEO = 2 ms`, a two-ended run over the routed path delivered the full rate at the SRT layer
(`mbpsRecvRate` 9.9 Mbit/s, `pktRcvDrop` 0) while the engine accepted only **42 of ~1000 frames a
second** and then flooded its playout buffer to capacity and overran. The 2 ms is not the problem on
its own; the drain loop multiplies it — up to 24 messages per frame, 8 frames per turn — so a turn
could block for tens of milliseconds, starving the device that the whole design is paced by. Setting
`SRTO_RCVTIMEO = 0` restored **999 of ~1000 frames a second**, the delay settled at 33 ms and there
were zero overruns. The same run on the in-process loopback is unchanged, because a loopback receive
never blocks — which is exactly why **only a socket test can catch this**, and why the engine now has
one.

The earlier note in this document's spirit ("what a timeout of zero means is not recorded") is now
closed: it is recorded here, from the primary source and confirmed by the measurement.

**Confirmed on the routed path, 2026-09-17, with the fix in place.** The Pi as caller and the laptop
as listener, two routed subnets, 43 ms round trip, one 8-channel block, fake daemon and null audio,
50 seconds. Both directions delivered the whole rate: listener **49,838 sent / 50,004 received**,
caller **50,004 sent / 49,803 received**, `0 refused`, `pktRcvDropTotal 0`, and **zero playout
overruns** at either end. `mbpsRecvRate` was 9.9 Mbit/s both ways and the SRT receive buffer sat at
0–1 ms, where before the fix it had climbed to 119 ms and stayed there. The same run before the fix
had delivered 2,091 of 49,875 frames and overrun the playout buffer to its 1,001 ms capacity.

The playout level had not settled when the run ended — 447 ms at the listener with its correction at
the +200 ppm clamp, 88 ms at the caller with −90.8 ppm — which is expected rather than a fault: the
level loop's period is 2,000 s, and the clock research records that a 10 ppm offset takes ~24 minutes
to converge. A short run measures throughput, not the clock; the ppm figure needs the ~20 minute run.

**The send has the same trap, and it is the one that made the appliance unkillable.** `SRTO_SNDTIMEO`
"limit[s] the time up to which the sending operation will block ... The -1 value means no time limit"
(default `-1`), so 0 is non-blocking there too. Left unbounded, a peer that accepts a connection and
then reads nothing fills the sender's flow-control window and `srt_sendmsg` blocks for ever — and
because the transmit loop only checks `stop()` between turns, the process ignores SIGTERM and systemd
has to SIGKILL it. Reproduced on 2026-09-17 with a `tx`-only peer that never reads: the sender parked
at `send buffer 8426 ms` and survived SIGTERM; with `SRTO_SNDTIMEO = 0` it exits cleanly. This is the
SIGTERM hang filed as issue #16, and it was a network call with no time limit, not a logic bug.

## Bonding and groups: not a v1 path, and not a drop-in later

- **It is a build-time feature.** `option(ENABLE_BONDING "Should the bonding functionality be
  enabled?" OFF)` (`CMakeLists.txt:174`), with a deprecated alias still handled
  (`CMakeLists.txt:877-881`). Code is compiled conditionally: `SRTO_GROUPCONNECT` appears only under
  `#if ENABLE_BONDING` (`srtcore/core.cpp:935`). **A distribution's libsrt may therefore contain no
  bonding support at all**, and we cannot assume the option exists at runtime.
- **It is incompatible with our connection strategy.** Groups "support only caller-listener mode"
  and `SRTO_TRANSTYPE` "live mode is the only supported for groups"
  (`docs/API/API-socket-options.md:273,275`). Decision 5 leans on **rendezvous** to get both ends
  through NAT unattended, so bonding and that strategy are mutually exclusive as things stand.
- In main/backup mode "only one link at a time delivers any useful data"
  (`docs/features/socket-groups.md:53`).

Redundancy is already a non-goal for v1, so nothing changes today. What changes is the *cost* of
revisiting it: it is a connection-model change plus a build requirement on the shipped library, not
a configuration flag. Better to know that before someone assumes a `bonded: true`.

## NAT and unattended operation

- `SRTO_RENDEZVOUS` defaults to `false`, and "both sides must set this and both must use the
  procedure of `srt_bind` and then `srt_connect` (or `srt_rendezvous`) to one another"
  (`docs/API/API-socket-options.md:1444-1446`).
- Rendezvous is therefore a **coordinated configuration, not a discovery mechanism**: both
  appliances must be told about each other, and there is no server or NAT-assist role. That is
  acceptable for an appliance configured once at each end, but it is an operational constraint worth
  writing down: **the peer address must be known at both ends in rendezvous mode**.
- `docs/features/handshake.md` (1565 lines) covers the handshake in depth and has not been read for
  this ticket; it is the place to look if rendezvous misbehaves in the field.
- **Unverifiable from documentation**: whether rendezvous succeeds through the specific NAT
  implementations at the two sites. That is a test on the real links, and it belongs to ticket 09.
- **Measured here, and worth knowing before a site visit**: in rendezvous mode a frame sent
  immediately after `srt_connect` returns is **discarded**. Both ends connect simultaneously, so
  data sent before the handshake settles never arrives at all — the peer's `packets_received` stays
  at zero while the sender's `srt_sendmsg` reports success. Sending repeatedly works normally, which
  is what an appliance carrying a thousand frames a second does without thinking about it. But an
  engineer who sends one test frame and sees nothing will reasonably conclude the link is broken.
  In caller/listener this does not happen: `srt_connect` returning means the peer accepted.

## What this confirms, and what it corrects

| Our decision or assumption | Status after this research |
|---|---|
| Own frame format rather than RTP-over-SRT (ADR 0001) | **Holds.** The ceiling applies to *messages*, not frames; the format is untouched |
| "One frame per SRT message" (ADR 0001) | **Wrong, and corrected** by an amendment to that ADR: a frame spans about seven messages |
| `latency_ms: 120` as the default | Confirmed — it is the library's live-mode default |
| 100–200 ms transport target (decision 7) | Holds, but it is *negotiated* as the maximum of both ends' settings; the UI must show the negotiated value, not ours |
| Never drop audio, `TLPKTDROP` unused (decision 6) | **Reversed by measurement.** Disabling it head-of-line blocks and kills a 64-channel link after ~2 s. It stays **on**; `pktRcvDrop` counts packets too late to play — zero on a healthy link |
| Passphrase validation of 10..79 characters | Confirmed exactly against `srtcore/srt.h:200` |
| Redundancy out of v1 | Confirmed, and the cost of revisiting it is now known: build-time gated, rendezvous-incompatible |
| Public internet, both ends NAT'd, unattended (decision 5) | Rendezvous works but is a coordinated configuration; bonding would force one end to be reachable |
| Pi-class CPU budget | Encryption cost at 148 Mbit/s is **unmeasured** |

## Unresolved: what must be measured or read next

1. ~~**Late-arrival behaviour with `TLPKTDROP` disabled.**~~ **Answered, and it killed the premise.**
   A late packet is not delivered later — with `TLPKTDROP` off the receiver blocks, and a 64-channel
   link stalls within ~2 s. `TLPKTDROP` stays on; `pktRcvDrop` is the loss counter to watch. Ticket 07
   now measures the opposite: frames keep flowing under strain, and loss shows up in `pktRcvDrop`.
2. ~~**AES-CTR cost at 148 Mbit/s on the target Pi.**~~ **Taken** — it is measured in the section
   above, and was measured again on 2026-09-17 with the same ~3.2 GB/s in 16 KB blocks, so a
   passphrase costs well under 1% of a core. The entry was left here after the measurement landed,
   which is the kind of stale to-do this document should not carry.
3. **Rendezvous through the actual site NATs.** Ticket 09, on the real links.
4. ~~**Whether the shipped libsrt has bonding compiled in.**~~ **Answered on the target, 2026-09-17,
   and the answer is no.** Not by hunting symbols — the symbol `srt_connect_group` *is* exported,
   which is exactly why symbol-hunting would have got this wrong — but by asking the library to do
   it:

   ```
   libsrt 1.5.4 (packed 0x00010504)
   SRT.ac: OPTION: #57 UNKNOWN
   SRTO_GROUPCONNECT accepted: no  (Operation not supported: Bad parameters)
   srt_connect_group symbol present: yes
   ```

   `SRTO_GROUPCONNECT` is unknown to this build, so bonding cannot be enabled even by a caller who
   tries. That is *this* package (Debian's `libsrt-gnutls` 1.5.4 on the Pi); the conclusion for
   ticket 15's installer is that whichever package it selects decides this, and the check above is
   the five-line way to know rather than assume.
5. **Buffer sizing at 148 Mbit/s.** `docs/API/configuration-guidelines.md:17` (the default receiver
   buffer is 8192 packets) and `SRTO_SNDBUF`/`SRTO_RCVBUF` are the next sources. Not researched
   here, and ticket 07 will likely need them.
6. **The `SRTO_PAYLOADSIZE = 0` tension.** Sidestepped rather than resolved: we choose our own
   fragment size below 1456 and do not depend on either reading being correct.

## Sources

All at tag `v1.5.7`. Nothing upstream was copied into this repository — the files were read in a
temporary directory and are cited, not vendored, in keeping with `AES67-VSC` ADR-0005.

| File | Used for |
|---|---|
| `LICENSE` | MPL-2.0, and what it does and does not oblige |
| `srtcore/srt.h` | Payload-size constants, passphrase rule, option enums, statistics API |
| `srtcore/core.cpp` | Conditional compilation of bonding |
| `CMakeLists.txt` | `ENABLE_BONDING` default |
| `docs/API/API-functions.md` | Send/receive semantics, message boundaries, the live-mode ceiling |
| `docs/API/API-socket-options.md` | Defaults for `SRTO_RCVLATENCY`, `SRTO_TLPKTDROP`, `SRTO_TSBPDMODE`, `SRTO_PAYLOADSIZE`, `SRTO_RENDEZVOUS`; group restrictions |
| `docs/API/statistics.md` | The statistics surface available to the UI |
| `docs/API/configuration-guidelines.md` | Receiver buffer sizing — the next source ticket 07 needs |
| `docs/features/latency.md` | What the latency buffer actually compensates for |
| `docs/features/encryption.md` | AES-CTR, KEK, PBKDF2 |
| `docs/features/socket-groups.md`, `bonding-intro.md`, `bonding-main-backup.md` | Bonding behaviour and its restrictions |
| `docs/features/handshake.md` | Not read for this ticket; the reference if rendezvous misbehaves |
