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
  **Partly measured 2026-09-17** — the *arrival spread* is now a number, on a routed path with 42 ms
  of ping jitter: 14.83 µs, typically. See "The arrival spread" at the end of this document. The
  crystal offset itself is still unmeasured, because the estimator that would give it needs
  robustness rather than the median the probe prints.
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

## Increment 2, the ratio control, measured 2026-09-17

`src/clock/ratio_control.{hpp,cpp}`, proved by `tests/test_clock_ratio.cpp` — 11 cases, six of them
simulations of an hour or more. The loop steers the **buffer level**, as decided above: it reads no
clock and needs no figure for `sigma`.

The plant is exact and worth stating once: **a ratio error of 1 ppm moves the level by one
thousandth of a millisecond per second** (a ppm of rate error moves 48 kHz audio by a microsecond a
second). So the plant is an integrator with gain 1/1000 ms/s per ppm — and that single number turns a
loop's period and damping into its two gains, `k_integral = 1000·ω²` and `k_proportional = 2000·ζω`.

### The first draft was wrong, and the trace is what showed it

The first version had **no proportional term**, on the reasoning that averaging the level would damp
the loop. It does not. Averaging is a lag, a lag is phase, and a double integrator (the plant, plus
the integral term a persistent error requires) with *any* extra lag is unstable at every gain. It
looked plausible; it was wrong; and nothing about the code said so.

What said so was a trace of `(level, average, correction)` every ten minutes, printed by the
simulation:

```
t=600 s   level=126 avg=122.95 ppm=3.08
t=1800 s  level=117 avg=120.97 ppm=30.49     <- past the true 10 ppm and climbing
t=3600 s  level=135 avg=123.65 ppm=-32.78
t=4800 s  level=127 avg=139.19 ppm=85.32     <- three times the previous swing
t=9000 s  level=3   avg=52.93  ppm=136.74
t=10200 s level=150 avg=93.37  ppm=-200      <- at the clamp
```

The correction grew by a factor of three an oscillation until it hit the ±200 ppm clamp, and the
level swung between **3 ms and 260 ms** over four hours, with 36 underruns on the way. The fix is the
classical one: the proportional term *is* the damping, because in a loop on a level it is the term
proportional to the error's derivative. The averaging is still there, for the one job it is good at —
turning the level's 1 ms staircase into something a proportional term can act on.

**A second thing the trace forced.** The rate limit (then 1 ppm/s) has to be generous enough not to
throttle the proportional term: answering a level that moved 20 ms means a correction of tens of ppm,
and at 1 ppm/s that takes tens of seconds while the loop's whole period is thirty minutes — the
rate limit, not the loop, would decide how fast a disturbance is damped. It is 50 ppm/s, and the
invariant that comes with it is absolute: **the ratio's rate of change is bounded, whatever is
asking.**

### What it measures, at the gains it ships with

Loop period 2000 s, damping 0.8, target 120 ms, from a buffer primed to 120 ms. **Every figure below is
against the stand-in plant** — a resampler model inside the test that consumes `frames x ratio` — which
is fast enough to run four hours of audio and is **not** the real library. The real library is lumpier (48
or 96 input frames per pull, with its own working room), so it gets its own measurement, for thirty
minutes rather than four hours, in "The join, measured" below. Two claims, two instruments:

| Run | Result |
|---|---|
| 4 h, sender **+10 ppm** | level **120 → 120 ms**, band **±2 ms**, correction **10.0 ppm**, inside 0.3 ppm by **23.6 min** |
| 2 h, receiver **+10 ppm** | level 120 → 120 ms, band ±2 ms, correction **−9.9999 ppm** |
| 2 h, sender **+1 ppm** | level 120 → 120 ms, band ±1 ms, correction **0.75 of 1 ppm** |
| 1 h, **matched** clocks | band **±0 ms**, correction **exactly zero** |
| 1 h, a **20 ms burst** at 1 h | excursion 112–140 ms, **back to 120**, correction 10.0004 ppm |
| 1 h at +10 ppm through **0–10 ms of arrival jitter** | level 111–129 ms (it floats with the spread), correction **9.69 ppm**, every period byte-exact |
| a target of 400 ms against a 120 ms buffer | correction held inside its clamp, level 120 → 401 ms, **nothing dropped** |

**The jitter run is the one that speaks to why this design was chosen** — the section above records
that a direct estimate needs an 11-second window to resolve 10 ppm at 5 ms of arrival jitter, and
0–10 ms is the spread whose `sigma` nobody has measured. What the run *shows* is narrower than that
argument: the conversion found the offset to within 0.5 ppm with every period byte-exact and in order
while the level moved with the spread. **It does not prove the loop is immune to jitter** — "a loop
that measures no time cannot be fooled by time" is an argument about the design, and a test cannot
confirm an argument. What it does show is the thing a control loop has to survive and often does not:
a *jittered measurement* — the level moving by ±9 ms as the link delivers in bursts — does not make
the correction hunt. Read it as that and no more. The jitter source is deterministic and
platform-independent, so at least the claim is repeatable.

### Two findings for the tickets that follow

**1. The level converges faster than the ppm figure, and that is structural.** At 1 ppm the loop held
the level to ±1 ms inside the run while its offset *estimate* was still 0.75 of 1 ppm after two hours
— because the integral only grows while a residual error persists, and holding the level means there
is almost none. So the ppm figure is **not** a clock measurement until it has been quiet for hours,
while the level is the number to trust immediately. Ticket 12's UI should trend the level and treat
the ppm as a slow diagnostic; anyone comparing it against the 10 ppm this project assumes should wait,
not read it early.

**2. A rate-limited loop settles a misconfigured target rather than swinging through it.** With the
rate limit and the anti-windup, the misconfigured case climbs monotonically to the unreachable target
and stops at the clamp. Without them it overshot to 436 ms and swung back to the other clamp — a
difference an operator *would* see in the delay figure, and which is now a test.

**Reproduce:** `./build/tests/aes67-srt-tests` (`clock_the_loop`, `clock_arrival_jitter`,
`clock_a_target`). The suite runs **115 tests**; the loop's six simulated runs cost about 10 seconds.
The trace that found the instability is `Plan::trace_ms` in that test file.

**What increment 2 does not include.** The resampler: the simulation's take rate stands in for it, as
it did in increment 1, and increment 3 replaces it with libsamplerate. Nothing here has touched a
device's clock, PTP, or a real link.

## Increment 3, the resampler, measured 2026-09-17

`src/clock/resampler.{hpp,cpp}`, proved by `tests/test_clock_resampler.cpp` and
`tests/test_clock_resampler_quality.cpp`. This is the last piece of the clock: the buffer holds the
sender's audio, the control decides how fast it must be consumed, and this consumes it.

**The API came from the library's documentation, not from memory**, and one thing there had to be got
exactly right: `src_ratio` is **output sample rate over input sample rate**
(`libsndfile.github.io/libsamplerate/api_misc.html`), while the control produces *input frames
consumed per output frame*. So `src_ratio = 1 / control.ratio()` — a reciprocal, and the one place in
this module where getting it wrong is silent rather than loud. `src_set_ratio()` is deliberately *not*
used: it bypasses the library's interpolation and gives a step in the ratio, which is a step in the
pitch of everything playing.

### Three things measured that the design had wrong or assumed

| Assumption | Measured |
|---|---|
| ADR 0003: "a resampler adds its own small delay" | **Zero.** At ratio 1 a 1 kHz tone comes back **bit-exactly at zero lag** on all three converters (residual 3.9e-7 at worst). The A/V line has nothing to add for it. |
| The converter needs look-ahead, so the carry holds it | The **carry** is empty after every pull. What the library holds is internal **working room: 48–96 frames (1–2 ms)** — measured as input consumed less output produced. Audio received, not yet played, and bounded by two periods. |
| What the library consumes per pull is 48 frames times the ratio | It is **quantised to whole periods**: one pull takes 48 input frames or 96, and the ratio lives in the average. A single pull cannot show 10 ppm — that is one frame per 4800 periods. |

That third one changed the test: the rate is verified *in the average*, over 200,000 periods at one
channel, where the effective ratio came out **10.0 ppm for a requested 10 ppm** (the quantisation's
residue is 5 ppm over that run, so the measurement has a 2:1 margin). And the carry is verified as a
**ledger that balances to the frame**: everything the buffer handed over was either consumed by the
converter or still pending — 95,568 frames in, 95,568 taken, 0 pending. A frame lost there would be
audio lost with nothing in the log, which is why it is an equation rather than a signal measurement.

### The converter choice, which ADR 0003 left to this increment

The documentation gives all three sinc converters **97 dB SNR** and says they differ in **bandwidth** —
97%, 90% and 80% of Nyquist. Measured on this project's own path (gain at 1 + 10 ppm, correlated
against the sender's position):

| Converter | 1 kHz | 10 kHz | 19 kHz | 20 kHz | 21 kHz | 22 kHz | Cost, 8 ch |
|---|---|---|---|---|---|---|---|
| `sinc_fastest` | 1.000 | 1.000 | **0.774** | 0.607 | 0.205 | 0.049 | **1.0×** |
| `sinc_medium` | 1.000 | 1.000 | 1.000 | 1.000 | 0.968 | 0.539 | 2.2× |
| `sinc_best` | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 | 6.3× |

So the cheapest converter is not "slightly worse": it is **2.2 dB down at 19 kHz and 14 dB down at
21 kHz**, which is the top of the audible band taken away, and it is exactly what an 80%-of-Nyquist
passband means. `sinc_best` is flat to 22 kHz and costs 6.3× the cheapest.

**Decision: `sinc_medium`, and it is the default — now measured on the appliance, not extrapolated.** The
first version of this section scaled the laptop's ratios by ticket 18's `SINC_FASTEST` figure and called
`sinc_medium` "~25% of one core". That was an extrapolation across two CPUs, so it was measured on the
Pi 5 itself, 64 channels, governor `performance`, best of three iterations per converter, two runs
agreeing to 0.05%:

| Converter | Time for 10 s of 64-channel audio | Versus realtime | **% of one Pi 5 core** |
|---|---|---|---|
| `sinc_fastest` | 1127 ms | 8.87× | **11.27%** |
| `sinc_medium` **← the default** | 2607 ms | 3.84× | **26.07%** |
| `sinc_best` | 8358 ms | **1.20×** | **83.58%** |

So the extrapolation was right within 4% — and `sinc_best` is *worse* than it guessed. The three things
that matter:

- **26% of one core for the audible band is affordable** on a four-core appliance, and `fastest`'s 11%
  would buy the wrong thing: a passband that loses the top octave. The decision stands.
- **`sinc_best` is out on measurement, not on taste**: at 1.20× realtime it would saturate a whole core
  to keep up, leaving nothing for the ALSA path, the daemon, the transport or the UI — and phase 2's
  codec may want 40–70% of a core by itself.
- **The ratios travel, the absolute figures do not.** On this laptop the same probe gave 1.00 : 2.17 :
  6.35; on the Pi it is 1.00 : 2.31 : 7.41. That is close enough that the laptop's *shape* predicted the
  Pi's decision — which is worth knowing for the next quality-versus-CPU question, and not worth
  pretending is an exact transfer.

**The clock module's tests run on the appliance too.** The whole suite — 126 cases on Linux, including the
resampler and the receive-path join — passes on the Pi 5 in **1m50s**, with no skips, and every number in
these three increments reproduces there **bit for bit**: the 10 ppm convergence (9.94305), the zero-lag
delay with its 3.94584e-07 residual, the 48-frame working room, the 15 ppm measured rate. The library's
behaviour is not an x86 artefact, which is what an ARM with NEON paths could have made it.

**Reproduce:** `./build/tests/aes67-srt-tests` (`clock_` cases). 125 tests; the resampler's cases cost
about 6 seconds, most of it the 200,000-period rate measurement and the converter sweep.

**What increment 3 does not include.** The engine: nothing yet carries a frame from a real device
through this buffer, so the clock is complete as a module and not yet joined to the receive path. That
join is what makes the delay figure continuous for ticket 12's UI, and it is the next slice.

## The join, and the circularity it exposed, measured 2026-09-17

`tests/test_clock_receive_path.cpp`, written because a review of the three increments above found the
hole: **the pieces had never met.** Each had been tested against a *model* of the others. The loop's
simulation drove a stand-in that consumed `frames x ratio` of input per output period — which is my
assumption about what a resampler does, written *inside the test* — and the resampler's tests set a
ratio by hand and never asked the control for one. So the control's unit convention was asserted only
against my model of a resampler, and the resampler's reciprocal only against my assumption about the
control. The relation that matters, `src_ratio = 1 / control.ratio()`, had never been exercised *across*
the two modules — and the mutant I ran in increment 3 was inside `resampler.cpp`, i.e. one file, so it
did not cover the handoff either.

**That is circular verification: N tests, one assumption.** With the real library as the plant, thirty
minutes of simulated audio, four channels:

| Run | Result |
|---|---|
| 30 min, sender **+10 ppm** | level **119 → 121 ms**, band **±2 ms**, correction **9.94 ppm** |
| 30 min, receiver **+10 ppm** | level **119 → 119 ms**, band ±2 ms, correction **−9.94 ppm** |

with the ledger balancing to the frame across the boundary (86,395,056 frames handed over = 86,395,056
consumed + 0 pending), no underruns, no short pulls, nothing dropped, and the audio coming out at unity.
So the loop holds the level against the thing it will actually drive, and the correction converges to
within 0.06 ppm of the truth in half an hour.

**Two assertions the first version of that test got wrong, and what they taught.** It checked that the
played *positions* advanced by exactly one period, and that every output period's *bytes* matched the
sender's for that position. Both are false in a resampling path: the input position advances by the
ratio's worth of frames (48, 96, occasionally less), and the samples have been through an interpolating
filter. Neither is a fault. **The correct loss metric across this path is the ledger**, and the position
is only worth checking for monotonicity — audio played twice would show as a position going backwards.
That distinction is written into the test because the engine will need it.

**What six of these modules' tests cannot reach.** The physics: two real crystals, a real PTP lock, a
real link's arrival pattern. Everything above runs on integer-exact simulated clocks with a distribution
I chose. That is the hardware session's job (ticket 09's two appliances, #18), and it is why no ticket
here is "done" on CI alone.

## The clock in the engine's receive path, 2026-09-17

`src/engine.cpp`. The three pieces now sit in the appliance's own receive loop, in the shape the
module-level join test modelled: messages in, frames reassembled and decoded, each period pushed into the
buffer by its sample position, then exactly one period pulled through the resampler at the control's ratio
and written to the device. The loop is still paced by the device, so the buffer is what absorbs the
difference between the sender's rate and ours.

What the engine's loopback — one process, one clock, 64 channels, two seconds — now reports:

```
engine loopback through the clock: 1880 periods of audio, 119 of silence,
                                   delay 118 ms, correction -0.087 ppm
```

- **119 periods of silence** before the first audio: the receiver waits until its level is what
  `link.latency_ms` bought (120 periods, since one 48-frame period is one millisecond). That is the
  priming policy, and it is the difference between "the first frame arrives and the device gets whatever
  happened to be there" and "the device gets audio at a known delay".
- **delay 118 ms** against a target of 120: the two periods the converter holds as its working room. The
  control reads that deficit as a small rate error, consumes slightly less for a while, and the level
  returns to the target — a **transient, not a bias**, because the converter's hold is a one-time
  transference rather than a permanent loss. (Worth checking rather than assuming: a permanent deficit
  would wind the integral up at 0.6 ppm a minute and put a standing bias in the ratio.)
- **correction −0.087 ppm** where the truth is zero, both ends sharing one clock: that transient, on its
  way back.

**And byte-exactness through the engine is gone, deliberately.** ADR 0003's resampler is transparent only
at a ratio of exactly 1, and the working room takes even a loopback briefly through a non-zero ratio — so
the engine's old headline test, *"a period written to one box's device comes out of another's byte for
byte"*, now asserts that the audio arrives at unity, the level holds, and nothing is refused. Byte-exactness
is still proved where it belongs: the transport's own loopback, and the resampler at ratio 1, where it is
measured bit-exact. **That is the cost of the clock, stated rather than discovered.**

**What the engine exposes for ticket 12**: `delay_ms()` (the buffer's level), `delay_fraction()`,
`clock_offset_ppm()`, `clock_ratio()`, `silence_periods()`. The *trend* the criterion asks for is a
difference between polls, so it belongs to whoever polls — the control surface — and not here.

**Still not measured: any of this on hardware.** Everything above is a process-local loopback, and the Pi
has run the engine but never with the clock in it.

## The arrival spread, measured on a real path, 2026-09-17

`scripts/measure-link-spread.sh`, new: two machines, one running `listen` and one running `send`, both
using libsrt directly rather than this repository's transport, with the same options
`src/transport/link.cpp` sets (live mode, message API, 120 ms latency both ways, 1316-byte payload cap,
TLPKTDROP off) and one frame's worth of messages per millisecond — this appliance's real shape and rate.

**This is the figure the section above twice calls decisive and never had**: what SRT delivers to a
receiver, which is what a direct rate estimate would have to work against. Raspberry Pi 5 as the sender,
a laptop as the receiver, **two routed subnets** with 43 ms of round trip and **42 ms of ping jitter** —
harsher than a LAN and less harsh than the open internet. 120 s, 119,879 frames:

| Figure | Measured |
|---|---|
| median interval | 1000.0 µs |
| **typical arrival spread** (robust sd, 1.4826 x MAD) | **14.83 µs** |
| naive sd | 71.2 µs — dominated by the outliers, and the wrong figure to use |
| frames more than 60 µs off the schedule | **17%** |
| whole-period slips | **0.05%** (42 near-zero and 20 longer than 1.5 ms) |
| the sender's own scheduler | sd 2.5 µs, worst 553 µs — so this is the link, not the Pi |

**SRT's TSBPD does what this document hoped it might.** 42 ms of network round-trip jitter arrives at the
application as a 15 µs spread, in a tight core around one millisecond. The network's jitter is *not* what
the clock module has to tolerate, and that is now measured rather than assumed.

**What it changes about the direct estimate.** The window arithmetic above needs `sigma`, and both figures
that matter are different from what this document assumed:

- with a typical spread of 15 µs rather than the 1 ms it called "the optimistic case", a slope fit
  resolves **10 ppm in about a third of a second and 1 ppm in about 1.4 seconds** — against the level
  loop's measured 24 minutes for 10 ppm. As an *accelerator*, that is worth having.
- **but a plain least-squares fit would be fitting the slips.** 17% of frames sit between 60 µs and half a
  millisecond off the schedule, and one in two thousand slips a whole period; those dominate a
  squared-error fit, which is exactly why this probe's naive sd (71 µs) is five times its robust one. The
  work in a direct estimate is the robust estimator, not the arithmetic — and nobody has built one.

**Correction to this document's own formula.** The section above writes the slope error as
`1.1e-4 * sigma / T^1.5` with `sigma` in seconds, which cannot be right: that coefficient already
contains `sigma = 1 ms`, and the formula with sigma left in reads `0.11 * sigma / T^1.5`. Its table was
computed correctly; its text was not.

**What it does not prove.** One path, and a routed one; both ends are general-purpose machines and the
sender is a C loop rather than the engine, so the sender-side figures belong to that loop. A *real* WAN
pair — ticket #10, two appliances — is what turns this into a field figure, and the script exists so that
session can repeat it in two minutes.
