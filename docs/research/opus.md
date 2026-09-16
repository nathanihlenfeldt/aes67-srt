# Opus for an 8-channel block

Status: researched 2026-09-16 for ticket 04 (issue #5), against tag **`v1.6.1`** of
[`xiph/opus`](https://github.com/xiph/opus). This is roadmap phase 2 work: v1 carries PCM only, and
nothing here delays it. What it does is answer the questions `docs/ROADMAP.md` left open.

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

**5. The delay figure is 4 ms from the encoder, plus the frame duration — and it should be queried,
not hard-coded.** `src/opus_encoder.c:311-313`:

```c
    /* Delay compensation of 4 ms (2.5 ms for SILK's extra look-ahead
       + 1.5 ms for SILK resamplers and stereo prediction) */
    st->delay_compensation = st->Fs/250;
```

At 48 kHz that is 192 samples. So the algorithmic delay is **frame duration + 4 ms**: 24 ms at 20 ms
frames, 14 ms at 10 ms, 9 ms at 5 ms. The API's own advice is explicit — "Applications needing delay
compensation should call this CTL rather than hard-coding a value"
(`opus_defines.h:500-502`, `OPUS_GET_LOOKAHEAD`).

**One discrepancy to settle by measurement.** The widely quoted figure is 26.5 ms for 20 ms frames,
which implies a 6.5 ms lookahead, not 4 ms. The code says 4 ms and names its components; the folklore
says 6.5. Neither this document nor a blog post should settle it: **call `OPUS_GET_LOOKAHEAD` on real
hardware** and record what it returns. That is one line in the measurement runbook.

**6. The default bitrate is not what anyone would choose.** `st->bitrate_bps = 3000+Fs*channels`
(`src/opus_encoder.c:296`): at 48 kHz that is ~99 kbit/s for a stereo pair, i.e. ~396 kbit/s for an
8-channel block of four coupled streams, unless it is set explicitly. Per-channel targets from the
roadmap are what should be configured, and `OPUS_SET_BITRATE` is the control.

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

## Unresolved: what only hardware can answer

1. **CPU per channel on the target Pi** at 64 channels of 48 kHz — the roadmap's deciding number.
2. **`OPUS_GET_LOOKAHEAD`'s actual value at 48 kHz**, to settle 4 ms against the folklore's 6.5 ms.
3. **Whether coupling helps or hurts** on eight unrelated console channels, measured as bitrate
   against quality rather than argued.
4. **What 64 and 128 kbit/s per channel sound like on programme material.** The RFC's range runs from
   6 kbit/s mono speech to 510 kbit/s stereo music; where a *contribution* link should sit inside that
   is a listening decision, not a specification.
5. **The distribution's libopus version**, and whether the multistream API is present in it — it is
   old and ubiquitous, but that is a claim to check rather than assume.

Items 1, 2 and 5 belong in the same hardware session as the clock-recovery measurements; 3 and 4 need
ears.

## What this changes in the roadmap

`docs/ROADMAP.md` said "Opus first. Royalty-free, and it is native at 48 kHz… 2.5–60 ms frames".
All of that holds, and three things are now more specific than it was:

- **The mechanism is the multistream API**, with eight mono streams as the correctness-first default
  and coupling as an experiment rather than an assumption.
- **In-band FEC is off**, because SRT has already bought that resilience and Opus charges for it in
  quality.
- **The latency to budget for is frame + 4 ms**, pending a `OPUS_GET_LOOKAHEAD` measurement — so a
  20 ms frame costs the A/V budget ~24 ms, not the ~26.5 ms usually quoted. The delay line has to
  subtract this, and the operator should be told the codec's share of the delay rather than inheriting
  it silently.
