# Roadmap

Status: agreed with the project owner (2026-09-16; extended the same day with the second product).
The current phase is specified in `docs/spec/0001-aes67-srt.md`; this file holds what comes **after**
it, so nothing here delays v1 and nothing in v1 forecloses it.

Two different kinds of thing live here, and the difference matters:

- **Phases 1 and 2 are capability steps of the appliance** — PCM, then lossy encoding. Same product,
  same plumbing, and phase 2 is the reason v1 carries a per-block payload type at all.
- **The second product is a different program that extends this one** — a macOS application that
  bridges SRT to CoreAudio, at the end of this file. It shares the core and none of the plumbing
  around it, which is what ADR 0004 is about.

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

**But bandwidth may not be what sets the limit.** Measuring Opus put encoding 64 channels at
**~14% of a fast desktop core** (`docs/research/opus.md` — a best-of-three figure; a single sample of
the same probe read 39%, which is how easily this number lies). A Pi 5 core is perhaps three to five
times slower at that work, so 64 channels sit somewhere between 40% and 70% of one Pi core: tight on
a four-core appliance, but not impossible. **The Pi number decides**, and it is section 7 of
`scripts/measure-hardware.sh`, taken with the CPU governor at `performance`. If it does not fit,
phase 2 becomes fewer encoded channels, a faster appliance class, or encoding only some blocks —
which the per-block payload type already makes possible.

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

## Second product — SRT to CoreAudio on macOS

**Why.** Everything in phases 1 and 2 assumes the far end is equipment: the appliance exists so that
a console or a DAW *somewhere else on the network* can be fed, and nobody listens in the path. But
the commonest thing an operator wants at the *near* end is to **mix or process the audio live** — in
a DAW, or in DaVinci Resolve's Fairlight page — and today that means a hardware codec or a bespoke
arrangement per site.

**What it is.** A standalone macOS application: SRT in or out, presented to CoreAudio as an audio
device, so a DAW can select it and work live. The macOS counterpart of what `AES67-VSC` is on
Windows — with the notable difference that Windows needed a kernel driver and macOS does not.

**It is not the appliance with a different audio backend.** It carries no AES67, no PTP, no RAVENNA
and no daemon: one side is SRT and the other is CoreAudio. That makes it a *smaller* program than the
appliance, assembled from parts that already exist.

**What it reuses is the point of putting it on this roadmap.** All three hard pieces are done and are
platform-neutral C++17:

| Module | State | What it gives the Mac application |
|---|---|---|
| `wire` | built, tested | blocks, sample position, per-block payload type, CRC |
| `transport` | built, tested | SRT: latency, passphrase, never-drop policy, statistics |
| `engine` | built, tested | the device ↔ frame ↔ link loops, **already running on macOS** |

The one missing piece is a `CoreAudioBackend` — the same seam `NullBackend` and `RavennaBackend`
already prove holds for two very different devices. **That is why ADR 0004 exists**: the moment a
second product is real, "keep the core free of ALSA, libsrt-specific, daemon and Linux assumptions"
stops being a style preference and becomes a constraint on every future commit.

**It is not blocked on v1.** The core already builds and runs its whole test suite on macOS, so this
can start before the appliance's own tickets finish without waiting for anything. It is the only
thing on this roadmap for which that is true.

### Three questions decide whether it is even the product described above

1. **Whose audio does it carry — ours, or anybody's?** If it only ever talks to this project's
   appliance, it is *the family's macOS endpoint* and the wire format is the whole story. If it must
   also talk to ffmpeg, vMix or a third-party SRT sender, then plain SRT audio has to be accepted and
   emitted too, the wire format becomes one of two payload conventions, and the product is a general
   SRT audio bridge. **Most of this page changes shape depending on the answer**, so it is the first
   thing to settle.
2. **A virtual device, or a bridge to one that already exists?** A virtual device means an
   `AudioServerPlugin` bundle installed under `/Library/Audio/Plug-Ins/HAL` — the kext-free mechanism
   BlackHole uses, and *to be confirmed as research rather than taken from this page*. It also means
   a second artefact to build, sign and notarise, and the first time this project has shipped a macOS
   bundle of any kind. Bridging to an existing or aggregate CoreAudio device is much less work and
   much less useful.
3. **Where does Float32 meet bytes?** CoreAudio's native format is Float32 and a DAW works in float;
   this project's audio path is deliberately bytes (ADR 0001). The conversion has to happen exactly
   once and be named somewhere. It is the first genuine tension with the byte-verbatim principle, and
   it is a decision rather than a detail.

### One question this product does *not* get to re-argue

The Mac's CoreAudio device runs on the host clock while the SRT stream runs on the sender's — two
independent 48 kHz domains, which is exactly the problem **ADR 0003** settled by measurement, with
the machinery already specified (continuous resampling at 11.27% of one Pi 5 core). What *is* fresh
is whether the virtual device owns the host clock or slaves to the stream: `AES67-VSC` ADR 0002
answered that for Windows as "the virtual sound card owns the clock", and that reasoning does not
transfer to CoreAudio unchanged.

### Non-goals for this product

No video. No mixing, EQ, metering or plugins — the entire point is to hand the audio to something
that already has them. No Windows or Linux build: if it is not macOS it is not this product, and the
appliance covers the headless case.
