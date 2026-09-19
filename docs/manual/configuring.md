# Configuring it

Everything is configured on the appliance's or the endpoint's own **web page**
(`http://<address>:<port>/`), in the **Configuration** card. Settings are saved to a file and either
apply immediately or need a **Restart**, which the page tells you and offers.

For automation, the same settings are a JSON file (`/etc/aes67-srt.conf` on the appliance, and beside
the Mac binary), and a REST API — see `docs/api.md`.

## The link

How the two ends find each other and how much delay the network gets.

| Setting | What it is | Notes |
|---|---|---|
| **Mode** | `listener`, `caller` or `rendezvous` | The **site is normally the listener** (its address is the stable one) and the **studio the caller**. `rendezvous` is for when neither end can accept a connection. |
| **Role** | `tx`, `rx` or `duplex` | Which direction **this** end runs. The site is usually `duplex`; the studio is `rx` to record, `tx` to send. One direction at a time (see *Known limits*). |
| **Peer** | `host:port` | The other end's address. Required unless this end is a listener. An IPv4 address, not a name. |
| **Local port** | the port this end listens on | Default **9000** on the appliance. Must be free, and open on any firewall between, though SRT is designed to work behind NAT without a fixed rule. |
| **Latency** | the delay budget, ms | Default **120 ms**. The jitter (not just the round trip) must fit inside it. **Opus needs a value at least a couple of frames larger than PCM** — 120 is fine for both. |
| **Alarm threshold** | when the page alarms about growing delay, ms | Default **500 ms**, and it must be well above the latency. |
| **Passphrase** | the shared secret for the link | **Set it.** It encrypts the link, which is the one part of the product that crosses the public internet. 10–79 characters, the same at both ends. |

**The link is encrypted even behind firewalls.** The cost is about 1% of the appliance's CPU at full
rate.

## The codec: lossless or compressed

The single most important setting — how much bandwidth the audio costs.

| Mode | What it sends | Bandwidth (64 ch) | When to choose it |
|---|---|---|---|
| **Compressed (Opus)** — *default* | L24, compressed | **~1–8 Mbit/s** | The default, and the right choice for most internet links. |
| **Lossless (PCM L24)** | uncompressed | **~74 Mbit/s** | A link that can carry it and wants no codec at all — a dark-fibre or a strong dedicated connection. |

The **Codec** control on the page switches the whole link at once, and sets the frame size for you. The
**Bitrate per channel** field applies to Opus only (**6000–256000 bit/s**; 128000 is a good default).
The per-block codec dropdowns underneath are for the advanced case — **mixed links**, where some blocks
are compressed and some are not, which the wire format supports by design.

**Opus quality does not change by itself.** It is a fixed bitrate the operator chooses, never an
automatic response to a link that is struggling. If the link cannot keep up, the *delay* grows and the
page alarms; it never quietly reduces quality.

> **The frame size follows the codec.** Lossless uses a 1 ms frame (48 samples); Opus uses a frame the
> codec allows (960 samples = 20 ms is the default). You do not normally set this by hand, and the
> appliance refuses an Opus block with a 1 ms frame rather than failing later.

## Channels: blocks and mapping

There are up to **eight blocks of eight channels** — 64 channels. The page lists each block and lets you
set:

- **Channels** — which device channels (on the appliance, the RAVENNA device's channels) that block
  carries, comma-separated. By convention block *i* carries channels `8i..8i+7`, and the site's AES67
  streams are published to match (block 0 → multicast `239.1.0.1`, and so on). Change it when the site's
  wiring differs from the convention.
- **Gain** and **Mute** — per block, −60 to +24 dB.
- **Codec** and **bitrate** — per block, for mixed links.

The **block count** and the mapping must agree with the channel count: the page regenerates the block
list when you change the count, so a mismatch is not something you can save by accident.

## The A/V delay

To line audio up with a picture that arrives later, add delay to the audio. This one is **live** — it
changes while audio runs, no restart.

- **Delay (ms)** — 0 to 5000. Use the DAW or a meter to find the figure.
- **Trigger test signal** — fires a short **impulse** on a chosen channel, so you can measure the offset
  against the picture (a clap on screen). Which channel is `egress.test_signal_channel`.

The total delay the page shows combines the transport latency, the playout level, this offset, and the
codec's own delay (Opus adds its frame plus ~6.5 ms).

## What applies live, and what needs a restart

- **Live:** the A/V delay and the test signal.
- **Needs a restart:** the link, the codec, the frame size, the blocks, the mapping, the levels. The page
  saves them and says *"restart to apply"*; the **Restart** button in the **Service** card applies them.
  The choice is deliberate: those settings reopen sockets and rebuild the audio path, so reapplying them
  in place is how a half-applied configuration happens.

## Where the settings live

The configuration is a JSON file, written by the page and validated before anything is applied — an
impossible setting is refused with the **name of the field**, and nothing is written:

```
audio.period_frames: an Opus block needs an Opus frame — 120, 240, 480, 960, 1920 or 2880
  samples at 48 kHz — got 48
```

An unknown key is an error rather than something ignored, because a typo in a key name is the commonest
way an appliance runs on defaults while looking configured. If you edit the file by hand, check it
before applying: `aes67-srt -c /etc/aes67-srt.conf --validate`.