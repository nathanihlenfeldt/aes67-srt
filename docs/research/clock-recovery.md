# Clock recovery: what the numbers say

Status: **measured** 2026-09-16, simulation only (ticket 03, issue #3). The hardware half of that
ticket — the CPU cost of the alternatives and whether a slip is audible — is still outstanding, and
this document says what it has to answer.

Reproduce: `tests/test_clock_drift.cpp`, run by `ctest` on every commit, on both platforms.

## The problem, in one paragraph

Both ends follow **their own** PTP grandmaster, so their sample clocks differ by however many ppm
their crystals differ. SRT delivers reliable bytes, not a clock. Left alone, the receiver's buffer
drains or fills against the sender, and something must reconcile the two. Everything below follows
from that one sentence.

## The measurement

| Clock offset | Drift | One-sample correction every | A 120 ms buffer lasts |
|---|---|---|---|
| 1 ppm | 0.048 samples/s | 20.8 s | 33.3 hours |
| **10 ppm** | **0.48 samples/s** | **2.08 s** | **3.33 hours** |
| 50 ppm | 2.4 samples/s | 0.42 s | 0.67 hours |

Over a four-hour show at 10 ppm: **6912 one-sample corrections**, or about 1700 an hour.

## What this changes

**1. The specification was wrong by two orders of magnitude.** It said a receiver left alone would
click "every few minutes" and that a slip a few minutes apart would be inaudible. At an ordinary
10 ppm it is **every two seconds**. Whatever the clock module does, it does it ~1700 times an hour,
not ~20. A design that slips a sample and justifies it as "rare enough" is **not available** — each
correction has to be individually inaudible (sub-sample interpolation, or a short crossfade), which
is a different and harder piece of work than the spec assumed.

**2. A show is a cliff edge, not a safety margin.** 3.33 hours at 10 ppm means a three-hour show
passes and a four-hour one comes apart in its last hour. That is the worst failure shape available:
it never appears during commissioning, it appears in front of an audience, and at a different time
depending on whose crystal is worse.

**3. Only the difference matters.** 10 ppm against 9 ppm drifts as slowly as 1 ppm against 0, on
20.8-second intervals and a 33-hour buffer. Matching hardware is worth more than good hardware, which
is worth knowing when specifying what goes in a rack at each end.

**4. The correction belongs with the delay figure, not beside it.** A drift correction *is* a delay
adjustment of a fraction of a sample. The clock module and the A/V delay line are the same machinery
seen from two directions, which is why they are adjacent in the capability map and why neither
should be built in ignorance of the other.

## The two candidates, and what each still needs

| Approach | What it costs | What is still unknown |
|---|---|---|
| **Adaptive playout buffer with corrections** | No continuous processing. ~1700 corrections/hour/link, each on all 64 channels at once, so the *size* of each correction is what matters and the count is a property of the clock pair alone | Whether a correction is audible on programme material, and with what crossfade — a listening test on hardware |
| **Continuous asynchronous sample rate conversion** | Every sample of all 64 channels processed continually | The CPU cost on the target Pi. This is the number that decides the question, and it is not yet measured |

## Measured on the target hardware, 2026-09-16

Raspberry Pi 5 Model B Rev 1.1, four cores, kernel 6.18.34, **governor `performance`** (which
matters: see the note below). Full numbers in the report attached to issue #18.

| Approach | Cost on the Pi, 64 channels of 48 kHz | Cost of the alternative |
|---|---|---|
| **Continuous ASRC** (`libsamplerate`, `SINC_FASTEST`) | **11.27% of one core** — 8.9× realtime, 0.176% per channel | — |
| Slipping the playout buffer | no continuous CPU at all | ~1700 corrections an hour at 10 ppm, each of which must be **individually inaudible** |

At a +10 ppm ratio, which is the correction the clock module would make continuously.

## The decision

**Continuous asynchronous sample rate conversion, not sample slipping.**

The reasoning is not that 11% of a core is cheap — it is that it **dissolves the hardest requirement
in the design**. The drift measurement said corrections come every ~2 seconds, so the slipping
approach asked for 1700 proofs an hour that a correction cannot be heard. Resampling continuously
means there are no corrections at all, and no audibility question to answer. Trading 11% of a core
for the removal of an unverifiable quality claim is the best value in this project so far.

Recorded as **ADR 0003**.

## Two caveats on that number, both of which matter

**It is the cheapest converter measured, not the best.** `SRC_SINC_FASTEST` is libsamplerate's
lowest-quality sinc; `SINC_MEDIUM` and `SINC_BEST` cost proportionally more and this project values
audio quality above CPU. The *shape* of the decision does not change — 11% has room — but the
converter choice is a quality-versus-CPU question that needs its own measurement, and it belongs
with the clock module's implementation rather than before it.

**The ratio is constant, and a general ASRC does not exploit that.** The offset between two crystals
is fixed at a few ppm, so a purpose-built fixed-ratio fractional resampler could be substantially
cheaper than a general converter that expects the ratio to move. The measurement therefore bounds
the design from above: any reasonable implementation will fit inside 11%.

**On the governor, which is not a footnote:** every figure above was taken with the governor at
`performance`, and the first Pi run — under `ondemand`, before the libraries were installed — could
not take them at all. The lesson comes from the laptop, where repeated runs of the Opus probe varied
by **2.7×** (39% against 14% of a core for identical work, from load alone). CPU numbers taken once,
or under a power-saving governor, are not evidence. The script now warns about the governor and takes
the best of three runs.


## Unresolved, and who owns it

- **ASRC CPU at 64 channels of 48 kHz on the target Pi.** Ticket 03, on hardware.
- **Whether a slipped or interpolated sample is audible**, and with what crossfade. Ticket 03.
- **What the ppm offset actually is on the target hardware.** 10 ppm here is a plausible crystal
  figure, not a measurement: if the real pair is worse, every interval above halves, and if the
  site's PTP holds both ends to a common reference the whole problem shrinks. Measuring the real
  offset on two appliances is a five-minute job that would sharpen all of this. Ticket 09, where
  two appliances exist on a real link.
- **Where the drift budget sits relative to the 120 ms latency**: whether corrections happen
  continuously or inside a bounded window, and what happens to the figure the operator is shown
  while they do.
