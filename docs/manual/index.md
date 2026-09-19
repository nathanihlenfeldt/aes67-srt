# aes67-srt — user manual

This is the manual for the people who **install and operate** the product: a broadcast or audio
engineer at a production site, and the engineer in the studio receiving from it. It assumes no
knowledge of the source code.

- **[Getting started](getting-started.md)** — install both ends and get audio flowing.
- **[Configuring it](configuring.md)** — the link, the codec, the channels, the delay.
- **[Operating it](operating.md)** — running, starting, stopping, monitoring, updating, removing.
- **[Use cases](use-cases.md)** — remote production into a DAW, a Fairlight-style studio, and more.
- **[Troubleshooting](troubleshooting.md)** — the failures you will actually meet.

## What it is, in one paragraph

It takes **up to 64 channels of audio** from one place and delivers them to another over an ordinary
internet connection. At the sending end an **appliance** (a small computer, the Raspberry Pi) takes
audio from an **AES67** network — the audio-over-IP standard a production console or a Q-SYS/Dante
system publishes — and sends it as a single encrypted **SRT** stream. At the receiving end a **Mac
endpoint** turns that stream back into audio a DAW can record or monitor, by presenting it as a
**BlackHole** audio device. It does not need a VPN, it does not need a public IP at either end, and it
carries 64 channels in **either direction**, one direction at a time.

**The default is compressed (Opus), around 1–8 Mbit/s for all 64 channels.** A link that can carry it
can also run **lossless PCM** at ~74 Mbit/s.

## The two ends

| | Production site | Studio |
|---|---|---|
| Hardware | Raspberry Pi 5 (or similar), wired Ethernet | a Mac, wired Ethernet |
| Software | the **appliance** (`aes67-srt`), a background service | the **endpoint** (`aes67-srt-mac`), a background service |
| Connects to | the site's AES67 network (via `aes67-daemon`) | a CoreAudio device — **BlackHole 64ch** |
| Controlled by | its **web page** | its **web page** and a **menu-bar icon** |

Same code underneath, two different jobs. The appliance has no screen and is reached over the network;
the Mac is on your desk and shows an icon.

## What you need before you start

- **Wired Ethernet at both ends.** Not Wi-Fi — 64 channels is more than Wi-Fi carries reliably, and the
  the loss it causes is what an operator hears as dropouts.
- **A low-jitter internet connection.** Not 5G/LTE or consumer satellite (Starlink): their latency moves
  too much for the transport delay to absorb. See the spec's *The link it expects*.
- **A shared passphrase** for the link — any secret, same at both ends.
- **At the site:** a running AES67 network and `aes67-daemon` (the installer sets this up).
- **In the studio:** a Mac, and **BlackHole 64ch** installed (one step needing your password).
- **Something to hear or record it:** a DAW, or a monitoring app, set to BlackHole.

## Words this manual uses

- **Block** — eight channels, the unit AES67 works in. 64 channels is eight blocks.
- **AES67** — the audio-over-IP standard the site's console publishes on.
- **SRT** — the secure, reliable transport this product sends over the internet.
- **Stream** — one block on the network. Eight blocks, eight streams.
- **The link** — the two ends' connection, and the settings that define it.
- **PCM / L24** — uncompressed, lossless audio. ~74 Mbit/s for 64 channels.
- **Opus** — compressed audio. ~1–8 Mbit/s for 64 channels. The default.
- **Delay** — how far behind real time the audio is played, to absorb network jitter and to line up
  with vision.