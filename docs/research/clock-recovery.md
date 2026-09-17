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
  while they do. **Increment 1 narrowed this**: the level is quantised to a whole period, so a 1 ppm
  offset takes 1000 s to become visible in it. See "Increment 1, the playout buffer, measured" below.

## How the ratio is obtained, and why the first answer was wrong

ADR 0003 decides *what* to do — continuous resampling — and leaves *how* to the module. The first
draft of that "how" was a rate estimator: fit the wire's sample position against each frame's local
arrival time, take the slope, and resample by it. Writing it out exposed two problems, and the second
is the one that matters.

**1. Do not filter with the thing you are measuring.** The sample position advances by exactly 48 for
every frame because frames are sent once per millisecond of the *sender's* clock. The sender's ppm
offset is therefore invisible in the position alone: it appears only when the position is set against
**our** clock. That is fine, and it is the whole trick — but it means the instrument's noise is the
**arrival jitter**, not the sample position.

**2. How much jitter can be tolerated is a hard arithmetic bound.** A least-squares slope over a
window of span `T` seconds with `n` points and arrival jitter `sigma` has an error of about
`sigma * sqrt(12/n) / T`. Frames arrive every millisecond, so `n = 1000*T` and the error is about
`1.1e-4 * sigma / T^1.5` with `sigma` in seconds:

| Arrival jitter | Window for a 10 ppm estimate | Window for 1 ppm |
|---|---|---|
| 1 ms | ~5 s | ~23 s |
| 5 ms | ~11 s | ~51 s |

So a direct estimate is **workable but not free**: it needs a window of tens of seconds, and what it
needs exactly is set by `sigma`, which **nobody has measured**. SRT delivers on a TSBPD schedule rather
than at the mercy of the network, so the true figure may be far below a millisecond — but "may be" is
not a number, and this project has been burned by exactly that kind of assumption.

**The alternative needs no measurement at all: close the loop on the buffer level.** The playout
buffer level *is* the integral of the rate error, so steering the ratio to hold the level constant is
self-correcting and needs no direct estimate. That is the standard shape for an asynchronous playout
path, and it is robust to any jitter because it does not measure time at all — it measures how full
the buffer is.

**What this changes about the build order, not the design.** Neither instrument exists yet, and both
need the same thing underneath them: the playout buffer, its level, and the sample-position alignment
that `wire` already guarantees by construction. So the increments became:

1. the **playout buffer** — holds frames by sample position, never drops audio, reports its level in
   milliseconds (which is also the delay figure ticket 12 asks for, and the machinery ticket 13's A/V
   offset is the same thing seen from the other side);
2. the **ratio control** — buffer-level feedback, with a direct estimate as an optional accelerator
   once `sigma` is known;
3. the **resampler** behind that ratio, choosing among libsamplerate's converters by measuring quality
   against CPU, as ADR 0003 says belongs here.

**And the next hardware session gains a cheap, decisive measurement:** log the spread of frame arrival
times at the receiver. That single number decides whether the direct estimate is viable as an
accelerator, and it costs a line of logging rather than a session.

## Increment 1, the playout buffer, measured 2026-09-17

`src/clock/playout_buffer.{hpp,cpp}`, proved by `tests/test_clock_playout.cpp` on every commit. No
hardware: this is arithmetic and a discrete simulation, and the simulation is the point — two clocks
offset by a few ppm, hours of simulated audio, and no sample lost or duplicated. The clock module's
test claims are met here or not at all.

The simulation is integer-exact. Each end's clock advances in millionths of a millisecond, so a ppm
offset is a whole number of units and four simulated hours accumulate no floating-point error; the
sender's periods arrive at positions 48 frames apart, as the wire format says they do.

| Run | Result |
|---|---|
| 120 ms buffer, sender +10 ppm, no compensation | full after **3.3333 hours**, then one 1 ms period surrendered every 100 s |
| 150 periods of buffer, receiver +50 ppm, no compensation | dry after **0.667 hours** (the table above says 0.67) |
| 4 hours, rates matched, 500 ms buffer | **14,400,024 periods played, 0 frames lost, 0 repeated, 0 underruns** |
| One hour at 10 ppm against 0 / 10 against 9 / 1 against 0 | level grows **+37 ms / +5 ms / +4 ms** |

**The level is the delay figure, and it is quantised to a period.** The buffer holds whole periods
because a wire frame's eight blocks share one sample position, so the level moves in whole
milliseconds: at 1 ppm it moves one period per **1000 seconds**. The drift is therefore invisible to
the level over short windows and only integrates in over hundreds of seconds — which is exactly what
a control loop on the level is, and part of why the increment order puts the ratio control after the
buffer rather than before it. **A loop on the level cannot correct a 1 ppm offset in seconds; it
corrects it in the time it takes the level to show it.** Where that lands relative to the 500 ms
alarm threshold is the ratio control's question, and it now has a number to answer it with.

**What the four-hour run does *not* include.** The take rate in the compensated run stands in for the
ratio control and the resampler: increment 3 replaces that stand-in with libsamplerate, and this test
becomes its regression test. Nothing here proves the resampler's quality or its CPU cost, and nothing
here has touched a real device's clock — a Pi and two appliances are still what turns any of this into
a field claim.

**The one behaviour that costs audio, and its conservation law.** An overrun surrenders the oldest
periods when the buffer is at capacity. The test asserts the equation rather than the intent: the
frames missing from the played stream are *exactly* the frames the buffer counted as dropped, so no
loss can hide in a miscount. A sender that jumps forwards — a link that went away and came back — is
counted in the same way, and the head is seated at the arrival rather than part way, because a head
that lands before an unfillable hole never plays again.

**Reproduce:** `./build/tests/aes67-srt-tests` (`clock_` cases). The suite runs **104 tests**; the
clock's 10 simulated runs cover a little over ten hours of simulated audio in about 18 seconds.
