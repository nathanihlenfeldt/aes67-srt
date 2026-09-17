# ADR 0006: The A/V delay line adjusts by crossfade, not by resampling

Status: **proposed** (2026-09-17). Built and measured for ticket 12 (issue #13) — figures in
`docs/research/av-delay.md` — and awaiting the owner's ratification of the mechanism.

## Context

Decision 8 makes the audio side of lipsync ours: an operator dials an offset, live, to line egress
audio up against someone else's vision. The offset sits `clock -> delay -> egress` and has to move
while audio is flowing, in both directions, without clicking.

Three mechanisms were real:

1. **Resample the offset away**, the way the clock reconciles the two crystals (ADR 0003). Continuous,
   no discontinuity, but the ratio change needed to move an offset of seconds is either far too slow
   (a pitch-safe 1% moves the delay 10 ms/s, so 2 seconds takes over three minutes) or fast enough to
   hear as a pitch bend.
2. **Duck to silence, jump the read head, duck back.** Provably continuous for any step size, no comb
   filtering, at the cost of a short dip — an audible hole on programme material.
3. **Crossfade between the old offset and the new one.** Continuous for any step size; the two copies
   of the programme overlap for the fade window, which is a short smear rather than a hole.

## Decision

**A sample-ring delay line whose read head changes by crossfade (10 ms, raised-cosine weights that
are exactly 0 and 1 at the window's ends), not by resampling.** The offset is quantised to whole
samples — 20.8 µs at 48 kHz, far below anything an A/V alignment can see.

## Reasoning

**ADR 0003's argument does not transfer, because the time constant does not.** It chose resampling
because at 10 ppm the clock corrects every two seconds, ~1700 times an hour, and no scheme that makes
1700 discontinuities an hour is guaranteed inaudible. An operator's offset change is a one-off, not a
drifting error, so there is no correction rate to drown in — one crossfade per action is the whole
cost. The delay line shares the buffer and the concept of delay with the clock; it does not share the
resampler.

**The crossfade's discontinuity is measurable and tiny.** On a 1 kHz, 0.9-full-scale programme whose
own largest sample step is 985,440 LSB, a change of 137.3 ms added **0 LSB** and one of 20 ms added
**64 LSB (-102 dBFS)**. A hard jump — the same code with the fade window set to zero — steps by up to
twice full scale and fails the test, which is what makes the assertion worth having.

**The dip was rejected rather than not considered.** Duck-and-jump is the safer mechanism in the
worst case and is one line simpler; it was rejected because a dip is plainly audible on programme
material whereas the crossfade's smear is short and contents. This is a judgement, not a measurement,
and it is the part of this ADR most worth overturning if a listening test disagrees.

## Consequences

- **Audibility is not proved.** The mechanism bounds the *discontinuity*; whether either the crossfade
  smear or the dip is heard needs ears and hardware, and that listening test remains ticket 03's open
  half.
- **An offset change is not a clock correction, and the two must not be conflated.** Drift stays the
  clock's business; this line only ever moves when a human moves it.
- **Whole-sample granularity.** An offset finer than a sample is not offered, because 20.8 µs is
  already two orders of magnitude below an A/V alignment's resolution and sub-sample work would buy
  nothing for the cost of interpolation.
- **Zero is transparent.** The steady state is a memcpy, so a link with no offset is bit-exact through
  this module; the clock's own byte-exactness result is untouched.
- **The fade window is a tuning constant, not a decision.** If the smear ever proves audible the window
  lengthens; if the latency of an update ever matters it shortens. 10 ms is where both are comfortable.
