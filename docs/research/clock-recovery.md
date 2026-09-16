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

## Recommendation

**Do not choose between them yet** — the deciding fact is a listening test, not an argument. But the
numbers already rule something out, and that is worth having before any code is written: a design
that relies on corrections being *infrequent* will fail, because at 10 ppm they are not infrequent.
If corrections are used, each one must be inaudible on its own merits.

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
