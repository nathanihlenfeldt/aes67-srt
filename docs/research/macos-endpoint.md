# The macOS endpoint: what the numbers say

Roadmap step 1 — the engine running on macOS against a real CoreAudio device, no SRT and no appliance —
taken 2026-09-17 on the development Mac. The product and its order of work are in `docs/ROADMAP.md`;
the architecture is ADR 0005.

## What was built to get here

- **`audio::FloatRing`** — a lock-free single-producer/single-consumer ring of interleaved float frames.
  This is the boundary ADR 0005 describes: CoreAudio's render callback touches a ring and an atomic
  index, and the engine's ordinary thread on the other side may block. A full ring truncates and counts;
  a starved one returns short and counts. Tested for order, wraparound, flood, starvation and across two
  threads — provable with no device, which is why it was written first.
- **`audio::pcm`** — the float ↔ `s24_3le` seam, moved out of `clock` so a device backend does not depend
  on the clock. Same behaviour: exact for 24-bit, rounds and clips rather than wrapping, NaN to silence.
- **`CoreAudioBackend`** — an AudioUnit on a device named by `audio.device`, with the rings inside.
  `read()` returns what the device gave us; `write()` hands it audio to play. The conversion is on the
  engine's side of the ring; the callback only copies. `read()`/`write()` wait for a whole period, which
  is where the inversion ADR 0005 describes — *the device calls us* — is absorbed into the engine's
  existing blocking contract.

## The measured run

`aes67-srt -c mac.conf`, `audio.backend: coreaudio`, `audio.device: BlackHole 64ch`, daemon fake, link
loopback, 64 channels, ten seconds:

```
coreaudio: opened BlackHole 64ch 64ch @48000Hz, float32 rings (64 out / 64 in available)
engine: stopped after 9899 frames sent, 9898 received, 0 refused;
        playout delay 116.000000 ms, clock correction -0.077524 ppm, A/V offset 0.000000 ms,
        140 periods of silence
```

- **9,899 of 9,899 frames carried and nothing refused**, so the callbacks ran and the rings did not
  starve or flood over ten seconds.
- **The level held at 116 ms** against a target of 120: the four missing milliseconds are the
  resampler's working room, exactly what the appliance's own loopback reports. Two loops and one clock
  domain, so the correction sits at ~0, which is the right answer rather than a coincidence.
- **140 periods of silence** are priming, not underruns.
- A shorter 3-second run first read `delay 116 ms, -0.131 ppm, 139 periods of silence`, so the figures
  are stable rather than a lucky sample.

## Two notes for whoever repeats it

**`coreaudiod` must be restarted after BlackHole is installed.** The cask puts the driver in
`/Library/Audio/Plug-Ins/HAL/`, but the running daemon does not pick it up until `sudo killall
coreaudiod` (a reboot also does it). Until then the device is simply absent, and the way it was
diagnosed is this backend's own refusal message, which lists every CoreAudio device it can see:

```
audio.device: no CoreAudio device named "BlackHole 64ch"
  (found: Nathan's iPhone (3) Microphone, MacBook Air Microphone, MacBook Air Speakers,
   Multi-Output Device)
```

**Build axis.** `WITH_COREAUDIO` is `AUTO` like the others, so the backend joins the library only where
it can compile. CI shows it working: the macOS job builds `coreaudio_backend.cpp`, the Linux job builds
`ravenna_backend.cpp`, and each is absent on the other because the source is not in the target.

## What this does not prove

- **Not the link.** This crossed an in-process loopback, not `transport::Link` to an appliance.
- **Not duplex channel design.** One BlackHole sums what is played into it, so a duplex bridge needs two
  channel groups or two devices before a site's layout is designed (ADR 0005's consequence).
- **Not a real device's timing.** BlackHole is a virtual device; a physical interface's clock, and the
  resampler's behaviour against it, are untested.
