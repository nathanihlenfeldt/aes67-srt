# ADR 0003: Reconcile the clock domains by continuous resampling

Status: **accepted** (2026-09-16). Decided by measurement — `docs/research/clock-recovery.md`, on
hardware, for ticket 03 (issue #3).

## Context

Both ends follow their own PTP grandmaster, so their sample clocks differ by however many ppm their
crystals differ. SRT delivers reliable bytes, not a clock. Left alone the receiver's buffer drains or
fills against the sender, and something must reconcile them.

Two candidates were real:

1. **An adaptive playout buffer** that corrects by slipping a sample — adding or dropping one at a
   zero crossing or through a short crossfade. No continuous CPU cost.
2. **Continuous asynchronous sample rate conversion**, which absorbs the difference in every sample
   and never corrects anything.

The simulation said which question decided it. At an ordinary **10 ppm** offset the two clocks need a
one-sample correction **every 2.08 seconds** — about **1700 an hour**, not the "every few minutes"
the specification had assumed. The slipping approach therefore required 1700 separate proofs per
hour that a correction cannot be heard, on 64 channels at once, for the life of the link.

## Decision

**Continuous asynchronous sample rate conversion.**

Measured on the target hardware — Raspberry Pi 5, 64 channels of 48 kHz, a +10 ppm ratio,
`libsamplerate`'s `SINC_FASTEST`, governor at `performance` — at **11.27% of one core** (8.9×
realtime). Full figures in `docs/research/clock-recovery.md`.

## Reasoning

**The 11% is not the argument; the disappeared requirement is.** Slipping asked for 1700 audibility
proofs an hour. Resampling continuously means there are no corrections, so there is nothing to prove
inaudible. This project treats quality as non-negotiable, and the only way to guarantee that a
correction is inaudible is to not make one. That is worth 11% of a core several times over.

**11% leaves room for a better converter.** The measurement used the *cheapest* sinc available, and
this project intends to spend CPU on quality. `SINC_MEDIUM` and `SINC_BEST` cost proportionally more;
the decision has room for them, and choosing between them is an implementation question with its own
quality measurement rather than a reason to revisit this decision.

**The measured figure bounds the design from above.** The ppm offset between two crystals is
essentially constant, so a purpose-built fixed-ratio fractional resampler can be far cheaper than a
general converter that expects the ratio to move. Any reasonable implementation fits inside 11%.

**It also removes a failure mode.** A slipping buffer has a state machine — when to slip, which
channel, what to do under load — and every state is a chance to produce a click. Continuous
conversion has no such state.

## Rejected: adaptive playout buffer with sample slipping

Rejected on the frequency of corrections, not their CPU cost: at 10 ppm they arrive every two
seconds, and the design would have to make each one inaudible across 64 channels, every hour, for as
long as the link is up. If it were only ten times less frequent the tradeoff would be different —
this is a decision that could have gone the other way, and the measurement is what settled it.

## Consequences

- The clock module resamples all 64 channels continuously, which is where most of its CPU goes. The
  budget is ~11% of one Pi 5 core at the cheapest converter.
- **Latency is unaffected in kind but must be accounted for**: a resampler adds its own small delay,
  and the A/V delay line must know it alongside the codec's frame + 6.5 ms.
  **Amended 2026-09-17, by measurement: the delay is not what this assumed.** On this project's own
  signal path a libsamplerate sinc converter reproduces a 1 kHz tone **bit-exactly at zero lag** at
  ratio 1 — all three converters — so the A/V line has nothing to add for it. What the library *does*
  hold is working room: at ratio 1 it takes 48–96 frames (1–2 ms) more than it produces over the first
  pulls, and keeps them. That is audio received and not yet played, which is a real figure for the
  delay line to know, but it is bounded by two periods and it is not a filter delay. Figures:
  `docs/research/clock-recovery.md`, "Increment 3".
- The audibility question is **dissolved rather than answered**. If a future change reintroduces
  corrections — a cheaper design, a different host — that question returns with it.
- A quality-versus-CPU measurement of the available converters belongs to the clock module's
  implementation, not to this decision.