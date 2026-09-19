# aes67-srt

Extends a production site's AES67 fabric to a remote location over the public
internet: **64 channels — eight 8-channel blocks — of uncompressed L24 PCM at
48 kHz in each direction**, carried in a single SRT stream per link, with the
same binary acting as transmitter, receiver or both. Configured from a web UI,
because it ships to other people's sites.

**Status: v1 is built, tested and running in the field.** The appliance moves audio between an AES67
device and a single SRT stream in each direction, reconciles the two ends' clock domains by continuous
resampling (ADR 0003), carries up to 64 channels losslessly or with Opus, and can delay its egress
audio to match vision with an impulse test signal to align against. It is controlled from a web page
whose REST API and controls — including start/stop/restart of the engine, the process and the daemon —
are in service. The macOS endpoint presents the stream to CoreAudio through BlackHole, with a menu-bar
app. **What remains is human and hardware acceptance, not build:** the real-device clock session
(issue #18), an hours-long internet soak (#29), a signed Mac build (#30), and the studio return path
(#31). The plan is in the specification, the decisions in `docs/adr/`, and the measured facts in
`docs/research/`.

**The link it expects:** wired Ethernet at both ends and a low-jitter internet connection. The default
is **Opus**, at roughly **1–8 Mbit/s for all 64 channels**; lossless PCM is ~74 Mbit/s per direction.
Either way, the round trip plus its jitter has to fit inside the transport delay. Wi-Fi, 5G/LTE and
consumer satellite (Starlink) remain out of scope: their jitter exceeds what the delay window absorbs.
See `docs/spec/0001-aes67-srt.md` → *The link it expects*.

## Where things are

- **`docs/README.md`** — the map of all documentation, by who it is for.
- **`docs/manual/`** — **the user manual**, for installing and operating it: getting started,
  configuring, operating, use cases and troubleshooting.
- **`docs/spec/0001-aes67-srt.md`** — the specification. Start here to work on the code. The frozen
  decisions, the capability map, the wire format, the clock problem and the boundaries are all in it.
- **`docs/ROADMAP.md`** — what comes after v1, and what is deliberately not in it.
- **`docs/research/`** — what was verified against primary sources, with citations. Start with
  `libsrt.md`: it corrects an assumption the wire format's ADR was built on.
- **`docs/runbooks/hardware-session.md`** — the one session at the hardware the remaining measurements
  wait on. Run `scripts/measure-hardware.sh` on the appliance and send the report back.
- **`docs/agents/`** — how the engineering skills read this repository.
- **`docs/adr/`** — decisions of record. `0001` is the wire format: our own frame in the SRT
  stream rather than RTP-over-SRT. `0002` is the licence. The rest arrive as decisions land.
- **`CHANGELOG.md`** — what each version changed. **`docs/releasing.md`** — the version scheme and the
  release checklist. **`docs/third-party-licences.md`** — the dependency licence audit.
- **`docs/runbooks/`** — the field procedures: `commissioning.md` for a site and a studio,
  `macos-endpoint.md` for the studio endpoint, `hardware-session.md` for the measurements.
- **`CONTEXT.md`** — the glossary, created lazily when terms actually land. Not written
  speculatively.

The work is tracked as issues on
[`nathanihlenfeldt/aes67-srt`](https://github.com/nathanihlenfeldt/aes67-srt/issues):
the numbered chain is the build order, issues #2–#5 are the research, and #1 is the specification.

**Next, in one line:** the build is done, so the next work is acceptance — run
`scripts/measure-hardware.sh` on the Pi for the real-device clock figures (issue #18), then the
hours-long internet soak (#29); the signed Mac build (#30) and the studio return path (#31) follow.
See [`docs/README.md`](docs/README.md) for where everything lives.

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

Everything CI runs, in one command — including the formatting gate and the
refusal behaviour described below:

```sh
./scripts/check.sh
```

## Running

```sh
build/aes67-srt -c config/aes67-srt.dev.conf --validate   # check and exit
build/aes67-srt -c config/aes67-srt.dev.conf              # run (no audio yet)
build/aes67-srt -v                                        # version and build flags
```

`config/aes67-srt.conf` is the shipped sample; `config/aes67-srt.dev.conf` is the
development one, with a null audio backend and a fake daemon so nothing touches
hardware.

## Refusing rather than repairing

Configuration is validated **before** anything is written or applied, and a
refusal names the offending field:

```
$ build/aes67-srt -c bad.conf --validate
bad.conf: unknown key "link.latency"
$ echo $?
3
```

An unknown key is an error rather than something to ignore, because the commonest
way an appliance ends up running on defaults while looking configured is a typo
in a key name. `./scripts/check.sh` asserts this behaviour, so it cannot regress.

## Licence

**GPL-3.0** — see `LICENSE`, and `docs/adr/0002-licence-under-gpl-3-0.md` for the reasoning. Unlike
the sibling gateway, where PJSIP and `aes67-daemon` forced GPL-3.0, libsrt is **MPL-2.0**: file-level
copyleft that does not extend to the larger work. The licence was therefore a choice rather than a
constraint (`docs/research/libsrt.md`), and GPL-3.0 was chosen for consistency with the sibling
appliance and for GPL-compatibility later.

Distribution carries the source-availability obligation; building and testing do not.
