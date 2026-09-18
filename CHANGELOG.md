# Changelog

Notable changes, newest first. A paragraph per release beats a rigid schema nobody fills in, but every
entry says what changed and, where it matters, why.

Versions are `MAJOR.MINOR.PATCH`. The project is pre-1.0 and **the wire format may still change between
minor versions** — it did, when the fragment header landed (ADR 0001's second amendment), which is why
both ends of a link must run the same version. The format does not negotiate.

## [Unreleased]

Nothing since 0.1.0.

## [0.1.0] — the appliance and the macOS endpoint

The first version with the whole audio path in place.

### The appliance (Linux)

- **64 channels** — eight 8-channel blocks — of L24 PCM at 48 kHz over one SRT stream per link, one
  direction at a time, with roles chosen by configuration.
- The **RAVENNA backend** captures and plays through `plughw:RAVENNA`, with the AES67 block mapping.
- **Clock reconciliation by continuous resampling** (ADR 0003): a playout buffer holds the delay and a
  ratio control steers it.
- On top: the **A/V delay line** and the impulse **test signal**.
- The **link reconnects itself** — a dropped connection, a listener whose caller left, and a caller
  that started first all come back with no restart (issue #21).
- The **playout crosses a lost frame** instead of silencing for ever (issue #20).
- **Opus (phase 2)** for a link that cannot carry 74 Mbit/s: per-block codec, eight mono streams,
  encoders threaded across the cores (~0.23 of a Pi 5 core for 64 channels on live audio).
- The wire format's **fragment header** makes a lost SRT message cost one frame rather than the stream
  (ADR 0001, second amendment).

### The macOS endpoint

- **SRT to CoreAudio**, recording into BlackHole 64ch, sharing the same engine (ADR 0004).
- An **installer, a LaunchAgent and a runbook** for putting it in a studio (issue #27).

### Known limits

- **One direction per link.** Duplex with a single BlackHole is not supported, because it sums its own
  playback (issue #31).
- The playout **conceals a provably-permanent gap as silence**; it does not repair loss.
- **No authentication or TLS.** The API is LAN-only by assumption, and exposing it is not supported —
  see *Security, and what it assumes* in the spec.
- The macOS build is **unsigned** (issue #30).
- A **real-internet soak** has not been run (issue #29), and the clock's on-device figures await the
  hardware session (issue #18).