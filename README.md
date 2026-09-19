# aes67-srt

**64 channels of AES67 audio, from a production site to a studio Mac, over the public internet — in
one encrypted SRT stream, with no VPN and no public IP.**

[![build](https://github.com/nathanihlenfeldt/aes67-srt/actions/workflows/build.yml/badge.svg)](https://github.com/nathanihlenfeldt/aes67-srt/actions/workflows/build.yml)
[![licence: GPL-3.0](https://img.shields.io/badge/licence-GPL--3.0-blue.svg)](LICENSE)

```
   PRODUCTION SITE                                     STUDIO
 ┌────────────────────────┐                      ┌────────────────────────┐
 │ console / Q-SYS /      │                      │ DAW: Fairlight,        │
 │ Dante / AES67 fabric   │                      │ Pro Tools, Reaper      │
 └───────────┬────────────┘                      └───────────▲────────────┘
             │ 8 × AES67 streams, 64 channels               │ CoreAudio
 ┌───────────▼────────────┐   one SRT stream     ┌───────────┴────────────┐
 │ appliance (Pi 5)       │   over the internet   │ macOS endpoint         │
 │ AES67 → blocks → SRT   │══════════════════════▶│ SRT → blocks →         │
 │ web UI :8082           │   encrypted, ~1–8 Mbps│ BlackHole 64ch         │
 └────────────────────────┘                       └────────────────────────┘
```

## What it does

- **64 channels** — eight 8-channel blocks, 48 kHz, the AES67 stream size. One SRT stream per link,
  in either direction; the same binary is transmitter, receiver or both.
- **Opus by default**, at roughly **1–8 Mbit/s for all 64 channels**, so an ordinary site uplink
  carries it. Lossless PCM L24 is the other mode, at ~74 Mbit/s per direction. Mixed links are
  allowed: PCM on the blocks that matter, Opus on the rest.
- **Reconciles the two ends' clock domains** by continuous resampling (ADR 0003), so the audio does
  not drift when the site's PTP clock and the Mac's device clock disagree.
- **Never silently degrades.** If the link sags, delay grows, the page shows it and alarms; quality
  changes only when a human changes it.
- **Aligns audio to vision** with an A/V delay line and an impulse test signal to measure against.
- **Configured from a web page**, because it ships to other people's sites — with a menu-bar app on
  the studio Mac.

## Quick start

**The site** (a Pi 5 on the AES67 network, wired Ethernet) — one command, idempotent, safe to re-run:

```sh
curl -fsSL https://raw.githubusercontent.com/nathanihlenfeldt/aes67-srt/main/scripts/install.sh \
  | sudo bash
```

It installs the RAVENNA kernel module and `aes67-daemon`, builds the appliance, sets it up as a
service, and prints a preflight report. **PTP locked** and **device present** are the two lines that
matter. The page is then at `http://<its address>:8082/`.

**The studio** (a Mac, wired Ethernet, [BlackHole 64ch](https://existential.audio/blackhole/)
installed) — build, then install the endpoint and its menu-bar app at login:

```sh
cmake -S . -B build && cmake --build build --target aes67-srt-mac
./scripts/install-mac.sh --peer <appliance-ip>:9000 --passphrase <shared-secret> --role rx
```

Then record **BlackHole 64ch** in your DAW. **Stream channel 1 is DAW input 1.**

Full instructions, settings and use cases are in the [user manual](docs/manual/index.md).

## Status

**Built, tested, and running in the field.** The wire format, SRT transport, the AES67 and CoreAudio
backends, the daemon client, the engine, the clock, the A/V delay, the REST API and web UI, the
control surface and the macOS endpoint all exist and are tested on Linux CI and macOS. **What
remains is acceptance, not construction:** a real-device clock session (#18), an hours-long internet
soak (#29), a signed Mac build (#30), and the studio return path (#31).

**The link it expects:** wired Ethernet at both ends and a low-jitter connection — the round trip
plus its jitter has to fit inside the transport delay. Wi-Fi, 5G/LTE and consumer satellite are out
of scope. See the spec → *The link it expects*.

## Documentation

[`docs/README.md`](docs/README.md) is the map, by audience. The short version:

- **[User manual](docs/manual/index.md)** — installing and running it: getting started, configuring,
  use cases, troubleshooting.
- **[Specification](docs/spec/0001-aes67-srt.md)** — the frozen decisions, the wire format, the
  clock problem, the boundaries. Start here to work on the code.
- **[ADRs](docs/adr/)** — decisions of record, starting with the wire format (`0001`) and the
  licence (`0002`).
- **[Research](docs/research/)** — what was verified against primary sources, with citations.
- **[Runbooks](docs/runbooks/)** — the field procedures, including the [one hardware session the
  remaining measurements wait on](docs/runbooks/hardware-session.md).
- **[Roadmap](docs/ROADMAP.md)** — what comes after v1, and what is deliberately not in it.
- **[API](docs/api.md)** · **[Changelog](CHANGELOG.md)** · **[Releasing](docs/releasing.md)** ·
  **[Third-party licences](docs/third-party-licences.md)**.

The work is tracked as issues on
[`nathanihlenfeldt/aes67-srt`](https://github.com/nathanihlenfeldt/aes67-srt/issues): the numbered
chain is the build order, issues #2–#5 are the research, and #1 is the specification.

## Building

Requires CMake 3.16 or later and a C++17 compiler.

**libsrt is needed for the transport and is optional for everything else.** The platform pieces are
`AUTO`, so on a machine without libsrt the project still builds, tests and reports honestly — every
transport test skips itself and `Link::open` refuses with the reason rather than pretending. To build
the transport:

```sh
brew install srt                     # macOS
sudo apt install libsrt-openssl-dev  # Debian/Ubuntu
cmake -S . -B build                  # libsrt is detected automatically
```

**libsamplerate is what the clock module resamples with** (ADR 0003), and it is detected the same
way:

```sh
brew install libsamplerate             # macOS
sudo apt install libsamplerate0-dev    # Debian/Ubuntu
```

ALSA is Linux-only and detected automatically: where it is absent the RAVENNA backend declines to
open and says why, rather than the build failing.

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Everything CI runs, in one command — including the formatting gate and the refusal behaviour
described below:

```sh
./scripts/check.sh
```

## Running

```sh
build/aes67-srt -c config/aes67-srt.dev.conf --validate   # check and exit
build/aes67-srt -c config/aes67-srt.dev.conf              # run (no audio yet)
build/aes67-srt -v                                        # version and build flags
```

`config/aes67-srt.conf` is the shipped sample; `config/aes67-srt.dev.conf` is the development one,
with a null audio backend and a fake daemon so nothing touches hardware.

## Refusing rather than repairing

Configuration is validated **before** anything is written or applied, and a refusal names the
offending field:

```
$ build/aes67-srt -c bad.conf --validate
bad.conf: unknown key "link.latency"
$ echo $?
3
```

An unknown key is an error rather than something to ignore, because the commonest way an appliance
ends up running on defaults while looking configured is a typo in a key name. `./scripts/check.sh`
asserts this behaviour, so it cannot regress.

## Licence

**GPL-3.0** — see `LICENSE`, and `docs/adr/0002-licence-under-gpl-3-0.md` for the reasoning. Unlike
the sibling gateway, where PJSIP and `aes67-daemon` forced GPL-3.0, libsrt is **MPL-2.0**: file-level
copyleft that does not extend to the larger work. The licence was therefore a choice rather than a
constraint (`docs/research/libsrt.md`), and GPL-3.0 was chosen for consistency with the sibling
appliance and for GPL-compatibility later.

Distribution carries the source-availability obligation; building and testing do not.