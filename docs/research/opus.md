# Opus for an 8-channel block

Status: researched 2026-09-16 for ticket 04 (issue #5), against tag **`v1.6.1`** of
[`xiph/opus`](https://github.com/xiph/opus); the open items were closed **on the target Pi 5 on
2026-09-18** against the distribution's **libopus 1.5.2**. This is roadmap phase 2 work: v1 carries
PCM only, and nothing here delays it. What it does is answer the questions `docs/ROADMAP.md` left
open.

## How to reproduce

```
gh api repos/xiph/opus/releases/latest --jq .tag_name   # v1.5.2  <- misleading, see below
gh api repos/xiph/opus/tags --jq '.[0:5][].name'        # v1.6.1 is the newest tag
curl -fsS https://raw.githubusercontent.com/xiph/opus/v1.6.1/include/opus.h
curl -fsS https://raw.githubusercontent.com/xiph/opus/v1.6.1/include/opus_defines.h
curl -fsS https://raw.githubusercontent.com/xiph/opus/v1.6.1/include/opus_multistream.h
curl -fsS https://raw.githubusercontent.com/xiph/opus/v1.6.1/src/opus_encoder.c
```

**The same trap as libsrt, and worth remembering**: the "latest release" API returns `v1.5.2`, while
the newest *tag* is `v1.6.1`. Pinning by release name would have researched a two-year-old version.
The distribution's version will be older again; record which one is actually installed when this is
implemented, exactly as the libsrt work had to.

## The findings that matter

**1. The single-stream API cannot carry 8 channels.** `opus_encoder_create` takes a channel count
that "must be 1 or 2" (`include/opus.h:203`). An 8-channel block therefore **requires the multistream
API** — that is not an optimisation, it is the only way to do it. `opus_multistream.h:106` describes
it: individual streams combined into one, "up to 255 elementary Opus streams" (`:122`), with coupled
streams ordered first and a mapping array relating I/O channels to streams (`:125-139`).

**2. Coupling is an assumption about correlation, and our channels may violate it.** A coupled stream
codes two channels jointly, which is a win when they are a stereo pair and a loss when they are not.
Our eight channels are *arbitrary console channels*, not a surround layout, so the standard surround
mappings do not describe them and family 255 with an explicit mapping is the honest choice. Whether
to couple at all is a measurement, not a preference: **default to eight mono streams (streams=8,
coupled=0) for correctness**, and treat coupling as an experiment with a bitrate and quality
comparison behind it. `OPUS_SET_FORCE_CHANNELS` (`opus_defines.h:376`) is the control.

**3. In-band FEC is the wrong tool here, and it costs quality.** `OPUS_SET_INBAND_FEC`
(`opus_defines.h:506-515`) is "only applicable to the LPC layer", and enabling it means Opus "will
automatically switch to SILK even at high rates to enable use of that FEC" — a real quality cost on
music. Meanwhile **SRT is already recovering loss by retransmission**: this is a reliable transport.
Paying Opus's FEC cost would buy back loss recovery we already have, at the price of the codec
choosing a speech-optimised mode for programme audio. **Recommendation: FEC off.**

**4. 48 kHz is the right rate and it is the top of the set.** Opus accepts 8000, 12000, 16000, 24000
or 48000 (`opus.h:201-202`). We need 48000, which needs no input resampling and is the rate the
codec's own delay accounting is written against (`src/opus_encoder.c:311-313`).

**5. The delay is frame + 6.5 ms, and measuring it was the only way to know.** `src/opus_encoder.c:311-313`
sets an internal delay compensation of 4 ms:

```c
    /* Delay compensation of 4 ms (2.5 ms for SILK's extra look-ahead
       + 1.5 ms for SILK resamplers and stereo prediction) */
    st->delay_compensation = st->Fs/250;
```

At 48 kHz that is 192 samples, which implies 24 ms for a 20 ms frame. **The API disagrees.**
`OPUS_GET_LOOKAHEAD` returns **312 samples = 6.50 ms at 48 kHz** — measured, not read, by
`scripts/measure-hardware.sh` on the first machine it ran on, and again on the target Pi 5 on
2026-09-18 (libopus 1.5.2, governor `performance`), which returned the same 312. The total
algorithmic delay is therefore **frame + 6.5 ms**: **26.5 ms** at 20 ms frames, exactly the folklore
figure this document had started to doubt.

The internal constant and the reported lookahead are measuring different things, and the reported one
is the one an application must compensate for. Which is what the API says in as many words
(`opus_defines.h:500-502`):

> Applications needing delay compensation should call this CTL rather than hard-coding a value.

**This is the clearest case in the project so far of reading being insufficient.** The source
comment, the header documentation and the measurement pointed at three different numbers, and only
the third is usable. The A/V delay line must subtract **frame + 6.5 ms**, and the codec's share
should be shown to the operator rather than inherited silently.

**6. The default bitrate is not what anyone would choose.** `st->bitrate_bps = 3000+Fs*channels`
(`src/opus_encoder.c:296`): at 48 kHz that is ~99 kbit/s for a stereo pair, i.e. ~396 kbit/s for an
8-channel block of four coupled streams, unless it is set explicitly. Per-channel targets from the
roadmap are what should be configured, and `OPUS_SET_BITRATE` is the control. Measured with 128 kbit/s
per channel configured, the encoder achieved 129 kbit/s per channel and 8.27 Mbit/s for all 64.

**7. The frame format needs no change, which was the point.** `PayloadType::opus = 2` is already
reserved in the wire format (`src/wire/frame.hpp`) with a codec seam behind it, and a PCM-only build
refuses it with a message that says so rather than misreading it as PCM. Phase 2 is an addition to
this format, not a version of it.

## The AAC question, which is about licences rather than code

AAC-LC is a codec with a **patent pool**, administered by Via LA, covering "MPEG-4 AAC Profile
(including MPEG-2 AAC LC)" among others, and a product that encodes AAC needs a licence agreement
with the pool (`via-la.com/licensing-2/aac/`). Their FAQ also defines what counts as a *multichannel*
product, which 64 channels plainly does. The page does not publish the royalty rates, and no rate
should be assumed from anywhere else.

**This document does not answer the AAC licensing question, and it should not pretend to.** What it
establishes is that the question is real, that it is commercial rather than technical, and that it
must be settled before an AAC encoder ships inside a product. Two things still need checking, both
outside the code:

- The **actual royalty terms** for a 64-channel encoder in an appliance, which means engaging Via LA
  rather than reading a summary.
- The **implementation's own licence**: whatever encoder is chosen must be compatible with this
  project's GPL-3.0 (ADR 0002). Some common AAC encoders are not, and which ones can only be judged
  by reading the licence file of the specific candidate — a five-minute task in the implementation
  ticket, not a guess here.

Opus has none of this: it is royalty-free by design, and its `COPYING` is a permissive BSD-style
licence from Xiph, Skype and others. That is the strongest argument for Opus being the default rather
than the alternative.

## The CPU number, and how easily a single sample of it lies

The harness measures encoding and decoding. The first run on an Apple-silicon MacBook Air (not the
target Pi) reported:

```
lookahead        : 312 samples = 6.50 ms at 48 kHz
encode           : 3883 ms = 2.6x realtime, 38.83% of one core (0.607% per channel)
decode           : 890 ms  = 11.2x realtime, 8.90% of one core
achieved bitrate : 8.27 Mbit/s total (129292 bit/s per channel)
```

**That 38.83% was wrong, and this document said so before it was corrected** — it was a single sample
taken while the machine was busy with something else. Running the same probe three times and keeping
the best (the run least interfered with) gives:

```
encode           : 1442 ms = 6.9x realtime, 14.42% of one core (0.225% per channel)  [best of 3]
decode           : 382 ms  = 26.2x realtime, 3.82% of one core  [best of 3]
achieved bitrate : 8.27 Mbit/s total (129164 bit/s per channel)
```

**A 2.7× swing on identical work, from machine load alone.** The harness now takes the best of three
runs and reports the CPU governor, because under `ondemand` the numbers are lower and less
repeatable still. Anyone measuring this on the Pi should set the governor to `performance` first —
the script prints the command.

**So what is true?** Encoding 64 channels of Opus costs **~14% of a fast desktop core**, decoding
~4%. **And on the target hardware it is 43.32% of one Pi 5 core** (2.3× realtime), with decode at
14.90% — measured 2026-09-16, governor at `performance`, best of three, and **reproduced on
2026-09-18 at 43.48% / 14.97%** against the distribution's libopus 1.5.2. The Pi is therefore **3×
slower than the laptop** at this work, which is inside the three-to-five-times band predicted above.

**The verdict, which the earlier version of this document got wrong in both directions:**

- **It fits.** 43% of one core to encode 64 channels, and my probe runs the eight block encoders
  sequentially on one thread. Eight threads, or four, would divide that across the Pi's four cores —
  so the *machine* cost is closer to 11–15% when threaded, which a Pi can afford.
- **But the codec is the entire CPU budget of phase 2.** PCM costs copies; Opus costs 43% of a core
  single-threaded. A phase 2 implementation that does not thread per block will be CPU-bound on a
  single core and will fail under load. **That is now a design requirement, not an optimisation.**
- **The trade is a good one where it is needed**: nine times less bandwidth for 43% of one core
  (unthreaded). For a link that cannot carry 74 Mbit/s, that is the whole point of phase 2.

The honest position is that phase 2 is affordable on this appliance *if* the encoders are threaded
per block, and that this must be designed in rather than discovered.

**The correction is the lesson.** This project has now been wrong three times by reading rather than
measuring (the 1456-byte payload ceiling, the 4 ms lookahead, and this), and once by measuring too
little. A CPU figure that will decide a product's channel count is not evidence until it is taken on
the target with the governor fixed and taken more than once.

## Unresolved: what only hardware can answer

**Closed on the target, 2026-09-18.** The probe in section 7 of `scripts/measure-hardware.sh` was run
on the Pi 5 with the governor at `performance`, best of three:

```
lookahead        : 312 samples = 6.50 ms at 48 kHz
encode           : 4348 ms = 2.3x realtime, 43.48% of one core (0.679% per channel)  [best of 3]
decode           : 1497 ms = 6.7x realtime, 14.97% of one core  [best of 3]
achieved bitrate : 8.27 Mbit/s total (129164 bit/s per channel)
```

1. **CPU per channel on the target Pi — answered: 0.679% of a core per channel to encode, 14.97% to
   decode all 64.** This reproduces the 2026-09-16 figure (43.32% / 14.90%) within noise, so it is a
   number to build on. It is single-threaded across the eight blocks; see "What this changes".
2. **`OPUS_GET_LOOKAHEAD` — answered: 312 samples = 6.50 ms at 48 kHz**, on the Pi and on the
   laptop. The encoder's `4 ms` internal constant is not the application-facing delay; the 26.5 ms
   folklore for a 20 ms frame is right after all.
5. **The distribution's libopus — answered: 1.5.2** (`libopus-dev`/`libopus0` 1.5.2-2 on Raspberry Pi
   OS), and **the multistream API is present** — `opus_multistream.h` is installed and the probe
   above compiles and runs against it. The research was written against upstream `v1.6.1`; nothing
   the probe uses differs in 1.5.2, but the version actually linked is recorded here so an API
   change is a thing to check rather than to assume.

Still open, and they need ears rather than a bench:

3. **Whether coupling helps or hurts** on eight unrelated console channels, measured as bitrate
   against quality. The default stays eight mono streams until this says otherwise.
4. **What 64 and 128 kbit/s per channel sound like on programme material.** A listening decision, not
   a specification.

## Phase 2, first slice: one block, built and proved on the Pi (2026-09-18)

The seam is in (`src/codec/opus.cpp`, commit `40e7511`): `OpusBlock` uses the multistream API with
**eight mono streams and no coupling**, s24_3le in and out, in-band FEC off, and reports
`OPUS_GET_LOOKAHEAD`. The wire format's per-block payload type was already reserved for it, so the
format did not change. Per-block `codec` and `bitrate_bps_per_channel` are configuration, and **in
codec mode the transport period *is* the Opus frame** (2.5–60 ms): a 1 ms period with an Opus block is
refused, because Opus cannot take one.

Proved on the target, Raspberry Pi 5 with libopus 1.5.2:

- the whole test suite passes on the Pi (175 tests, codec tests included), and a `WITH_OPUS=OFF`
  build also builds and passes with the codec tests skipping cleanly;
- a one-block Opus loopback on the Pi: 502 frames sent, 500 received, 0 refused;
- **one block over the real Pi → Mac link**: 20 ms frames at the expected 50/s, **0 refused**, delay
  100 ms. The measured rate was ~49 kbit/s because both ends ran the *null* backend, so Opus was
  encoding silence — a real-audio bitrate needs the RAVENNA device and is still to be taken.

Not done, and they are the rest of phase 2: a real-audio bitrate, and the coupling and bitrate
listening tests below.

## Eight blocks, and the encoders threaded (2026-09-18)

The ROADMAP makes threading a *requirement*, not an optimisation: 43% of a Pi core to encode 64
channels on one thread is a CPU-bound appliance under load. `ParallelFor` (`src/parallel_for.cpp`)
runs the block encodes across `hardware_concurrency() - 1` workers plus the calling thread, started
once at `prepare` and reused per period; a single-block link runs inline and never starts a thread.
The receive-side decodes stay sequential for now (14.97% of a core, not the bottleneck).

Proved on the Pi: eight Opus blocks on the null backend ran 1,201 frames sent / 1,200 received with
**0 refused** and 14 threads in the process (the pool's included). A `WITH_OPUS=` build with and
without libopus both pass (179 tests on the Mac; the same suite passes on the Pi).

**The CPU saving is not measured, and the bench cannot measure it.** The null backend emits digital
silence, and Opus does almost no work on silence — the process sat at ~0.1% of a core either way.
The 43% figure comes from the probe with a **tone**, and until the appliance's own transmit path runs
on the RAVENNA device with programme material, the threaded number is unmeasured. That is the same
hardware-session gap the clock module has; recorded here rather than claimed.

## The real-audio numbers, on the RAVENNA device (2026-09-18)

Taken on the Pi 5 with live programme material on `plughw:RAVENNA` (8 active channels at -15 to -27
dBFS, tonal, ~390 Hz), all eight blocks Opus, 20 ms frames, Pi transmit → Mac receive:

| | value |
|---|---|
| Pi CPU, 64 channels encode + decode | **22.6% of one core (0.23 cores)**, threaded; busiest threads 157/74/71/70 ticks, so the pool is really sharing the work |
| wire rate | **1.22 Mbit/s** for this material |
| frames | 0 refused, delay 80 ms |

Two things this settles, and one it corrects:

- **The appliance can do 64-channel Opus on a Pi with room to spare.** 0.23 of the 4 cores, on live
  audio, without the encoders saturating any single core. Threading is what keeps it off one core.
- **The rate is the *content's*, not the setting's.** The material is tonal, and Opus spent ~19
  kbit/s per channel on it. White noise through the same codec path at the same setting reached
  **129 kbit/s per channel**, so the encoder is not silently capped at a low rate.
- **Corrected: `OPUS_SET_BITRATE` is clamped for a multistream block.** Asking for 128 kbit/s per
  channel (1024 kbit/s for the block) and reading it back with `OPUS_GET_BITRATE` gives **576
  kbit/s**, i.e. ~72 kbit/s per channel. The configuration's 6..510 kbit/s per-channel range is
  therefore wider than the multistream encoder will honour, and VBR can still exceed the clamped
  target on hard material. **The configuration validation should be narrowed to what Opus will take,
  or the ceiling documented** — a follow-up, not done here.

## What this changes in the roadmap

`docs/ROADMAP.md` said "Opus first. Royalty-free, and it is native at 48 kHz… 2.5–60 ms frames".
All of that holds, and three things are now more specific than it was:

- **The mechanism is the multistream API**, with eight mono streams as the correctness-first default
  and coupling as an experiment rather than an assumption.
- **In-band FEC is off**, because SRT has already bought that resilience and Opus charges for it in
  quality.
- **The latency to budget for is frame + 6.5 ms** — settled by `OPUS_GET_LOOKAHEAD`, measured at 312
  samples on both the laptop and the Pi. A 20 ms frame costs the A/V budget **~26.5 ms**. The delay
  line subtracts this, and the operator is told the codec's share rather than inheriting it silently.
- **Phase 2 must thread per block.** 43.5% of a Pi core to encode 64 channels is affordable only
  because the eight block encoders can run on the Pi's four cores; done on one thread it is a
  CPU-bound appliance waiting for load. Design it in.
