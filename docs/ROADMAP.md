# Roadmap

Status: agreed with the project owner (2026-09-16). The current phase is specified in
`docs/spec/0001-aes67-srt.md`; this file holds what comes **after** it, so nothing here delays v1
and nothing in v1 forecloses it.

## Phase 1 — PCM transport (current)

64 channels (eight 8-channel blocks) of uncompressed L24 PCM at 48 kHz in each direction, in one
SRT stream per link, bidirectional, with a web UI. Spec: `docs/spec/0001-aes67-srt.md`.

## Phase 2 — bandwidth-constrained operation (lossy encoding)

**Why.** PCM has no compression lever at all, so 64 channels cost ~74 Mbit/s per direction and a
link that cannot carry that simply cannot use the tool. Encoding to Opus or AAC turns "does not
fit" into "fits, at a quality cost the operator chose".

**What v1 must do so phase 2 is possible.** These are v1 obligations, not phase 2 work:

1. **Every block carries an explicit payload type in the wire frame.** Without it, adding a codec
   means a breaking format change, and every appliance already sitting in another building has to
   be re-flashed in lockstep — a migration that never happens cleanly.
2. **The pipeline has a codec seam** between block assembly and framing, so an encoder is a stage
   rather than a redesign.

**Codec choice.**

- **Opus first.** Royalty-free (BSD-3-Clause), and it is **native at 48 kHz**, which is the AES67
  rate — so it needs no resampling, unlike AAC at some rates. 2.5–60 ms frames, in-band FEC, DTX.
  Encoder: `libopus`. **Researched: `docs/research/opus.md`** — the mechanism is the multistream API
  (the single-stream API cannot carry 8 channels), coupling is an experiment rather than an
  assumption, in-band FEC is **off** because SRT has already bought that resilience, and the latency
  to budget is frame + 4 ms.
- **AAC-LC second.** Not a schema problem but a **licensing** one: a patent pool applies to
  encoders shipped in a product, and FDK-AAC's licence is not GPL-compatible. It means ffmpeg's
  native AAC encoder or a licensed one, and an ADR when the time comes.
- **Opus's channel mapping needs deciding**: an 8-channel block as one multistream Opus stream
  (coupled stereo pairs) versus 8 independent mono streams. Coupled pairs are the sensible default;
  the block boundary is already there.

**Density targets.**

| Mode | Per channel | 64 channels | Versus PCM |
|---|---|---|---|
| PCM L24 | 1.152 Mbit/s | 74 Mbit/s | — |
| Opus | 128 kbit/s | 8.2 Mbit/s | ~9× less |
| Opus | 64 kbit/s | 4 Mbit/s | ~18× less |

**But bandwidth may not be what sets the limit.** Measuring Opus on the first available machine put
encoding 64 channels at **~39% of a fast desktop core** (`docs/research/opus.md`). A Pi is several
times slower at that work, so phase 2 may be **CPU-bound rather than bandwidth-bound**, in which case
the table above describes a promise this appliance cannot keep at 64 channels. The measurement that
decides it is in `scripts/measure-hardware.sh` (section 7) and should be taken before any phase 2
work is planned, because the answer changes what phase 2 *is*.

**Latency cost is real and must be counted against the A/V budget.** An Opus encoder adds the frame
duration **plus its algorithmic delay** to the transport floor: at 48 kHz that is **frame + 4 ms**,
so **~24 ms** with 20 ms frames, ~14 ms at 10 ms, ~9 ms at 5 ms (`docs/research/opus.md`, which
cites `opus_encoder.c:311-313`). Every millisecond of it is headroom the operator loses when lining
audio up with vision, and it comes out of the same budget the clock module needs for drift.

**Settle the figure on hardware before designing the delay line around it.** The widely quoted 26.5 ms
for 20 ms frames implies a 6.5 ms lookahead, which the encoder's own code contradicts. The authority
is `OPUS_GET_LOOKAHEAD`, queried at runtime (`opus_defines.h:500-502`) — not this document, not a blog
post, and not the number above. `docs/research/opus.md` lists measuring it as an open item.

**Policy — decided, and identical to PCM.** Codec mode keeps the same rule: **never drop audio,
let delay grow, show the delay, alarm past the threshold. Quality changes only when a human
changes it, never automatically.** No adaptive bitrate. This means the third lever an encoder
makes possible (bitrate) is deliberately left alone.

**Mixed-mode links are the prize.** Because the transport unit is an 8-channel block *and* the
payload type is per block, a link can run **PCM on the blocks that matter and Opus on the rest** —
the operator keeps the critical channels pristine and compresses only what they can spare. This
falls out of the v1 obligations above at no extra cost, and it is the main reason they exist.

**Open questions for phase 2.**

1. Is Opus the default for a constrained link, or always an explicit choice?
2. Per-block bitrate policy: one bitrate for the whole link, or per block?
3. Does a codec link still get the impulse/clap test signal for A/V alignment? (It should, but
   the encoder's latency has to be part of what the operator is aligning.)
4. Transcode on ingest, or accept already-compressed sources? (AES67 is PCM in practice, so the
   question is really about future inputs.)
5. Does the delay-grows policy need a harder ceiling in codec mode, where the whole point was to
   fit a constrained link?

**Out of scope even for phase 2.** Lossy *ingest* codecs other than Opus/AAC, per-channel adaptive
bitrate, and any automatic quality reduction. Phase 2 widens the quality dial; it does not make
the appliance change it by itself.
