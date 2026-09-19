# Use cases

The product exists to get a production site's audio into a studio, over an ordinary internet
connection, without a VPN or a public IP. These are the shapes that takes in practice. Each says how it
is wired, what to set, and what to know.

In every case: the **appliance sits at the site**, takes audio off the site's **AES67** network, and
sends it as one encrypted **SRT** stream; the **Mac sits in the studio**, receives it, and presents it
on **BlackHole 64ch**, where a DAW records or monitors it.

---

## 1. Remote production: record a live event in a DAW

**The situation.** An event — a concert, a service, a sports fixture — is mixed on a console at the
venue. An engineer in a studio elsewhere records all 64 channels for a later mix, or monitors them
live and takes notes.

**How it is wired.**

```
venue console ──AES67──▶ appliance (Pi) ──SRT over internet──▶ Mac endpoint ──CoreAudio──▶ DAW
   (Q-SYS / Yamaha / Calrec / DiGiCo)        (compressed)         BlackHole 64ch        (record)
```

**What you set.** Nothing unusual: the shipped defaults. **Compressed (Opus)** keeps the venue's uplink
requirement under ~8 Mbit/s, which almost any venue has. The studio is the **caller** and the site the
**listener**; both use the **same passphrase**.

**What to know.**

- **Arm all 64 inputs in the DAW** and record BlackHole. Map the channels to the DAW's tracks once and
  save the session as a template.
- **This is a monitoring and recording path, not foldback.** The end-to-end delay is around a tenth of a
  second; that is invisible to a recordist and too long for a performer's in-ear monitor. Nothing here
  sends audio *back* to the venue (see *Return path*, below).
- **Record a safety copy at the venue too** if the take matters. The internet is unpredictable, and the
  product is designed to *survive* a bad link rather than to deny that one exists.

---

## 2. Fairlight (or any DAW) mixing a remote feed for broadcast

**The situation.** A studio running **Fairlight in DaVinci Resolve** — or Pro Tools, Reaper, Logic —
takes a remote contribution and mixes it into a broadcast or a stream. Because the same Mac also handles
vision, **the audio and the picture have to line up**.

**How it is wired.** As above, but the DAW is the mix point rather than a recorder. BlackHole 64ch is
selected as the DAW's input; the remote channels appear as inputs and are routed to the desk.

**What you set.** The **A/V delay** is the setting that matters here — this use case is why it exists.
The remote audio arrives ahead of a picture that has travelled a slower path (satellite, a cloud
contribution, a vision mixer further downstream). Add delay to the audio until it matches.

**What to know.**

- **Find the figure by measurement, not by ear.** Use the **impulse test signal**: fire it on a known
  channel, and measure how far it leads the picture. That number is the delay to set. It adjusts **live**,
  while audio runs.
- **The page shows the total delay** — transport + playout + your offset + the codec's own (Opus adds its
  frame plus ~6.5 ms). That total is what you compare against the picture.
- **Compressed audio costs latency, not sync.** Opus adds ~26.5 ms at 20 ms frames, which is part of the
  number above. It is a fixed, known cost, not a drift.
- **`tx`/`rx` is per direction and one at a time.** To *mix back and send return audio* to the venue is
  the return path, below.

---

## 3. A venue with a thin internet link

**The situation.** The site's uplink is a few megabit — a village venue, a temporary rig on a shared
connection, a location with only a 4G/5G modem. Lossless PCM at 74 Mbit/s is out of the question.

**What you set.** **Compressed (Opus)** — the default — at 64–128 kbit/s per channel. 64 channels then
cost roughly **1–8 Mbit/s** depending on the material, an order of magnitude less.

**What to know.**

- **This is what Opus is for.** The bitrate is fixed and chosen by you; the codec never lowers quality by
  itself. If the thin link still cannot keep up, the **delay grows and the page alarms** — that is the
  product telling you the truth, not hiding it.
- **Jitter matters more than bandwidth on thin links.** A connection whose latency swings (mobile,
  satellite) will still defeat the delay window even when it is "fast enough" on average.

---

## 4. Sync audio to vision for a live stream

**The situation.** A stream carries vision from the site and audio from the product. The two arrive out
of step.

**What you set.** The **A/V delay**, as in use case 2. The audio almost always arrives *first* (video
encoding and distribution are slower), so you add delay to the audio.

**What to know.** The delay line only ever **adds** delay — it can never advance the audio, because that
would mean playing audio it has not received. If the audio is *late* relative to vision, the correction
belongs on the vision side.

---

## 5. Monitoring a site from the studio (no recording)

**The situation.** An engineer wants to *hear* what is happening at a site — confidence monitoring —
without recording.

**What you set.** As use case 1, but route BlackHole to a monitoring output in the DAW rather than
arming tracks. Or run any app that can play BlackHole.

**What to know.** BlackHole is a device, not a speaker; something must play it. The DAW is the usual
thing.

---

## Return path (not yet)

Sending audio **from the studio back to the site** — a talkback, a return mix, a comms feed — is **not
built**. It is deliberately future work, and the reason is a CoreAudio detail worth knowing: **one
BlackHole sums what is played into it**, so a single device cannot carry both directions without the
endpoint hearing itself. Doing it properly needs a second BlackHole or two disjoint channel groups, and
a channel-map feature the engine does not have yet (issue #31).

**One direction at a time is the current rule.** The appliance itself is `duplex`-capable over SRT; it is
the studio's single audio device that limits it.

## Two appliances, no studio Mac

The two ends need not be a Pi and a Mac. The **same binary runs at both ends of an appliance-to-appliance
link** — useful where there is no DAW, e.g. moving an AES67 fabric between two sites, or a permanent link
between a venue and a broadcast centre's own AES67 network. Configure one as `caller`/`tx` and the other
`listener`/`rx`; the codec, passphrase and block mapping are the same as everywhere else.

## What this product is *not* for

- **In-ear monitoring or foldback to performers.** The delay is too high; that is a different tool.
- **Wi-Fi or mobile/satellite links as a first choice.** They are outside what the delay window can
  absorb; the spec's *The link it expects* says why.
- **A permanent licensed radio-link replacement.** SRT over the public internet is best-effort; where the
  audio is contractual, treat it as a resilient contribution path with a local safety recording, not as
  your only copy.