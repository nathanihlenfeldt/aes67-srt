# aes67-srt

Extends a production site's AES67 fabric to a remote location over the public
internet: **64 channels — eight 8-channel blocks — of uncompressed L24 PCM at
48 kHz in each direction**, carried in a single SRT stream per link, with the
same binary acting as transmitter, receiver or both. Configured from a web UI,
because it ships to other people's sites.

**Status: skeleton.** The repository configures, builds, tests and validates its
configuration. **It carries no audio and no network.** The plan, the decisions and
the open questions are in the specification.

## Where things are

- **`docs/spec/0001-aes67-srt.md`** — the specification. Start here. The frozen
  decisions, the capability map, the wire format, the clock problem and the
  boundaries are all in it.
- **`docs/ROADMAP.md`** — what comes after v1: Opus and AAC-LC encoding for links
  that cannot carry 74 Mbit/s of PCM.
- **`docs/research/`** — what was verified against primary sources, with citations. Start with
  `libsrt.md`: it corrects an assumption the wire format's ADR was built on.
- **`docs/agents/`** — how the engineering skills read this repository.
- **`docs/adr/`** — decisions of record. `0001` is the wire format: our own frame in the SRT
  stream rather than RTP-over-SRT. The rest arrive as decisions land.
- **`CONTEXT.md`** — the glossary, created lazily when terms actually land. Not written
  speculatively.

The work is tracked as issues on
[`nathanihlenfeldt/aes67-srt`](https://github.com/nathanihlenfeldt/aes67-srt/issues).
Ticket 05 is this skeleton; 01–04 are the research that must precede the modules
that need it; the numbered chain 06→15 is the build order.

## Building

Requires CMake 3.16 or later and a C++17 compiler. Nothing else is required on a
development machine: the platform pieces are `AUTO`, so a Mac with no ALSA and no
daemon builds and tests the whole skeleton.

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

**Proposed: GPL-3.0**, awaiting the owner's decision — `docs/adr/0002-licence-under-gpl-3-0.md`.
Unlike the sibling gateway, where PJSIP and `aes67-daemon` forced GPL-3.0, libsrt is **MPL-2.0**:
file-level copyleft that does not extend to the larger work. Our licence is therefore a choice
rather than a constraint (`docs/research/libsrt.md`). Nothing is distributed until it is settled.
