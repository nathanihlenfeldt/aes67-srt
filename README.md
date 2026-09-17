# aes67-srt

Extends a production site's AES67 fabric to a remote location over the public
internet: **64 channels — eight 8-channel blocks — of uncompressed L24 PCM at
48 kHz in each direction**, carried in a single SRT stream per link, with the
same binary acting as transmitter, receiver or both. Configured from a web UI,
because it ships to other people's sites.

**Status: the wire format, the SRT transport, the audio device, the daemon client, the engine, the
clock and the A/V delay line are built and tested.** The appliance moves audio between an AES67
device and a single SRT stream in each direction, reconciles the two ends' clock domains by
continuous resampling (ADR 0003), and can delay its egress audio to match vision with an impulse test
signal to align against (ticket 12). **The control surface — the REST API and the web UI — is still
to come**, and so is a two-appliance WAN figure. The plan, the decisions and the open questions are
in the specification, and the measured facts are in `docs/research/`.

## Where things are

- **`docs/spec/0001-aes67-srt.md`** — the specification. Start here. The frozen
  decisions, the capability map, the wire format, the clock problem and the
  boundaries are all in it.
- **`docs/ROADMAP.md`** — what comes after v1: Opus and AAC-LC encoding for links
  that cannot carry 74 Mbit/s of PCM.
- **`docs/research/`** — what was verified against primary sources, with citations. Start with
  `libsrt.md`: it corrects an assumption the wire format's ADR was built on.
- **`docs/runbooks/hardware-session.md`** — **where the project is blocked**: one session at the
  hardware closes the measurements three tickets are waiting on. Run
  `scripts/measure-hardware.sh` on the appliance and send the report back.
- **`docs/agents/`** — how the engineering skills read this repository.
- **`docs/adr/`** — decisions of record. `0001` is the wire format: our own frame in the SRT
  stream rather than RTP-over-SRT. `0002` is the licence. The rest arrive as decisions land.
- **`CONTEXT.md`** — the glossary, created lazily when terms actually land. Not written
  speculatively.

The work is tracked as issues on
[`nathanihlenfeldt/aes67-srt`](https://github.com/nathanihlenfeldt/aes67-srt/issues):
the numbered chain is the build order, issues #2–#5 are the research, and #1 is the specification.

**Next, in one line:** run `scripts/measure-hardware.sh` on the Pi (issue #18). The clock module's
approach, phase 2's shape and whether 64 channels of audio fit at all all wait on numbers that only
the appliance can produce — see the runbook for what each figure decides.

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
