# A/V delay: what the numbers say

The delay module (ticket 12, issue #13): where the audio side of lipsync sits, why a change of
offset is a crossfade rather than a jump, and what the A/V budget is made of. Every figure here was
measured on this machine by `tests/test_delay_line.cpp` unless it says otherwise.

## The problem

Two sites, and vision has a latency of its own. Audio is the only side of the picture that can be
delayed to match it — an operator cannot advance video to match audio — so the offset is ours
(decision 8). It sits on **egress**: after the clock has decided what to play and before the device
plays it, `clock -> delay -> egress`. The clock's playout level is the delay the link forced on us
(decision 6); this is the offset dialled on top of it.

## The mechanism, and the one decision in it

The line is a sample ring. The read head trails the write head by the requested offset, and the
steady state does no arithmetic at all — **an offset of zero is a memcpy**, so a link with no A/V
offset is bit-exact through this module and the clock's byte-exactness result is untouched.

An operator's dial is a jump in that read head, and a jump is a click. So a change of offset is a
**crossfade**: for the fade window — 10 ms by default — the old offset and the new one are read at
the same time and blended with raised-cosine weights that are exactly 0 and exactly 1 at the
window's ends. The output is therefore continuous with what came before it and what follows it, for
any step size. A request that arrives mid-fade is remembered and applied when the current fade
lands, so a dial turned quickly settles on its final value without ever stepping.

The alternative is fade-through-silence: duck out, jump, duck in. It is provably continuous for any
step size and has no comb filtering, at the cost of a short dip. The crossfade was chosen because
the ticket names it and a blend is less audible than a dip on programme material. **If that trade is
the wrong way round, the fade is the one place to change.**

## Measured: the discontinuity an adjustment introduces

Audibility is a listening test and cannot run in CI — it is still ticket 03's open half. What *is*
measurable is the size of the discontinuity the adjustment introduces, and that is what the test
asserts. The programme is a 1 kHz sine at 0.9 x full scale on eight channels, whose own largest
sample-to-sample step is **985,440 LSB**.

| Change | Largest step during the change | Added by the adjustment |
|---|---|---|
| 0 -> 137.3 ms | 985,440 LSB | **0 LSB** |
| 0 -> 20.0 ms | 985,504 LSB | **64 LSB (-102 dBFS)** |

Zero and 64 LSB against a bound of 1,048,575: the crossfade's added step is two to four orders of
magnitude below the programme's own slew, because the blend term is `|old - new| x d(weight)`, and a
10 ms window makes the weight change 0.0033 per sample.

**The mutant that proves the test can fail.** With the fade window set to zero — a hard jump — the
same test fails on both changes, because a jump between two samples of opposite polarity steps by up
to twice full scale (16.7 million LSB). The test was rebuilt for that run, and the failure is loud.

## The A/V budget: what the operator's number is made of

`Engine::av_delay_ms()` sums these. It is a sum of what actually knows its own number, not a promise.

| Term | Value | Source |
|---|---|---|
| transport latency | `link.latency_ms` (120 ms nominal) | a buffering budget, decision 7 — not a measured end-to-end figure |
| playout level | `Engine::delay_ms()`, live | the clock's buffer level, measured every run (ticket 11) |
| the resampler | **0** | measured, increment 3: bit-exact at zero lag on all three converters |
| the A/V offset | `egress.delay_ms` | the operator's dial, 0..5000 ms |
| the codec | **0 in v1** | nothing is encoded (decision 9); phase 2 is the codec's frame + 6.50 ms |

The codec term is a figure rather than a footnote for the reason the ticket gives: an Opus link must
not silently move the operator's alignment number. The measured `OPUS_GET_LOOKAHEAD` is **312 samples
= 6.50 ms** at 48 kHz on the Pi (`docs/research/opus.md`), so a 20 ms frame costs the budget 26.5 ms
that the operator has to subtract from the offset they dialled. That subtraction is what phase 2
inherits; in v1 the term is zero because there is no codec to pay.

## Range

The module holds 0..5000 ms, matching `egress.delay_ms`'s existing validation. **Spec open item 7 —
0-2 s or 0-5 s — is still a human answer about the vision path**, not about this module; when it lands
the capacity line moves and nothing else does.

## Relationship to ADR 0003, stated rather than left to look like a contradiction

ADR 0003 chose continuous resampling for the **clock**, because at 10 ppm there is a correction every
two seconds and a mechanism that slips a sample cannot make ~1700 corrections an hour inaudible. An
operator's A/V offset change is not that: it is a one-off of up to seconds, and resampling a
two-second change would either take minutes (at a pitch-safe ratio) or bend the pitch audibly (at a
fast one). The delay line shares the buffer and the *concept* of delay with the clock — the same
machinery seen from the other side, as `clock-recovery.md` puts it — but not the resampler. The same
observation, a different time constant, a different mechanism.

## What it does not prove

- **Nothing about hearing.** The 10 ms window and the crossfade-versus-dip choice are engineering
  judgements, not measurements of audibility; that test needs ears and hardware.
- **Nothing about hardware.** No real device, no vision path, no operator. The egress stage has run
  in the engine's loopback and in CI, and nowhere else yet.
- **Nothing about a codec.** The impulse is proved sample-accurate through a PCM path; whether it
  survives an Opus encode is roadmap open question 3.
