# The macOS endpoint's clock: what the numbers say

Investigation for issue #41, 2026-09-19. This is a measurement, not a decision: it records what the
endpoint's clock correction does on the bench and what that implies. The device seam itself is in
`docs/spec/0002-macos-endpoint.md`; this note is about the clock problem it exposed.

## Why this was looked at

With the endpoint's own device (`audio.backend: "hal"`) and a live stream from the appliance, the
playout buffer filled to its maximum and stayed there (`delay 1000 ms`, frames surrendered). The same
stream on BlackHole did not fill, which was read at first as a `hal` bug. The owner then noticed that
**BlackHole also leaks** — its playout delay is not held — which moved the search from the backend to
the clock.

## How the offset was measured

The clock loop clamps its correction at ±200 ppm. That clamp is documented as "a safety net rather
than a control: real crystals are tens of ppm apart at worst, so a correction beyond ±200 ppm means a
measurement that has gone wrong" (`src/clock/ratio_control.hpp`). Because it was *saturated* on both
backends, it was hiding the number.

To read the number, the loop was temporarily sped up (5 s natural period, 0.2 s averaging, damping 1.0,
slew 1000 ppm/s) and the clamp widened to ±5000 ppm. A correction the loop settles at *with the level on
target* is a real offset; one that keeps moving is not. The experiment was reverted afterwards;
`src/engine.cpp` is unchanged in the tree.

## The numbers

| Backend | correction it settles at | level |
|---|---|---|
| `coreaudio` / BlackHole | **−2500 ppm** | reaches and holds the 120 ms target |
| `hal` (this project's device) | **+5000 ppm**, still pinned | stays at maximum, still filling |

**The two device clocks are at least 7500 ppm — 0.75 % — apart.** No pair of crystals is within two
orders of magnitude of that. One device clock is grossly wrong, and the plug-in's is the prime
suspect: a separate loopback run read ~1005 wire frames/s where 1000 is nominal (~0.5 % fast).

The appliance's own clock may also be off. If BlackHole is taken as accurate, the numbers imply the Pi
is ~2500 ppm slow.

## What this explains

- The ±200 ppm clamp cannot hold a 2500–7500 ppm offset, so it pins and the level drifts: `hal` fills,
  BlackHole empties. The direction differs because the offsets differ.
- It looked like a `hal` bug only because `hal`'s offset is the larger one.
- No container fixes this. MPEG-TS would carry the sender's clock (as PCR) and leave the receiver to
  synchronise to it, which on a Mac — where CoreAudio owns the output clock — still means resampling.
  A 0.75 % clock error survives any framing.

## What this does not prove

- **Which clock is wrong.** The Pi's PTP clock, BlackHole's clock, and the plug-in's device clock are
  all candidates. The hardware session (issue #18) exists to measure the Pi against a known reference.
- **That the offset is stable.** A fixed offset can be corrected by widening the clamp; a drifting one
  cannot. Only a longer measurement says which this is.
- **That the plug-in's clock is the fault**, though it is the leading suspect: libASPL's
  `ZeroTimeStampPeriod` defaults to the sample rate, and the timestamp scheme is the first thing to
  check against `mach_absolute_time` with a client attached.

## Trap recorded

Setting `ZeroTimeStampPeriod` to an I/O-sized value (512) **wedged `coreaudiod` at ~100 % CPU** and
hung `system_profiler` until a reboot. It is the timestamp ring's wrap length, not the HAL's I/O
buffer size. Recorded in `src/audio/hal_shared.hpp` and `src/audio/hal_driver/Driver.cpp`; the
install/uninstall scripts now bound `system_profiler` so a wedged CoreAudio cannot hang them.

## Where it stands

The device works: a real CoreAudio client reading `AES67-SRT` gets I/O at ~1005 frames/s with delay
stable at 116 ms on loopback. What does not work is holding the playout level against a *live* stream,
because the clocks are far enough apart that the correction saturates. Next, in order:

1. Check the plug-in's device clock against `mach_absolute_time` (offline).
2. Measure the appliance's clock (issue #18).
3. Widen the clamp to a value the evidence supports — not to 7500 ppm, which would mask the fault and
   pitch-shift the programme.
