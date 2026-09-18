# Roadmap

Status: agreed with the project owner (2026-09-16; extended the same day with the second product).
The current phase is specified in `docs/spec/0001-aes67-srt.md`; this file holds what comes **after**
it, so nothing here delays v1 and nothing in v1 forecloses it.

Two different kinds of thing live here, and the difference matters:

- **Phases 1 and 2 are capability steps of the appliance** — PCM, then lossy encoding. Same product,
  same plumbing, and phase 2 is the reason v1 carries a per-block payload type at all.
- **The second product is a different program that extends this one** — a macOS application that
  bridges SRT to CoreAudio, at the end of this file. It shares the core and none of the plumbing
  around it, which is what ADR 0004 is about. **It does not start until the appliance is complete** —
  see the sequencing note under it.

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
  to budget is frame + 6.5 ms, measured.
- **AAC-LC second.** Not a schema problem but a **licensing** one: a patent pool applies to
  encoders shipped in a product, and FDK-AAC's licence is not GPL-compatible. It means ffmpeg's
  native AAC encoder or a licensed one, and an ADR when the time comes.
- **Opus's channel mapping: eight mono streams per block, decided.** The research's default is
  **eight independent mono streams** (family 255, explicit mapping), because our channels are
  arbitrary console channels rather than stereo pairs and coupling is an assumption about
  correlation that may not hold. Coupled pairs are an experiment to be measured against it, not the
  starting point.

**Density targets.**

| Mode | Per channel | 64 channels | Versus PCM |
|---|---|---|---|
| PCM L24 | 1.152 Mbit/s | 74 Mbit/s | — |
| Opus | 128 kbit/s | 8.2 Mbit/s | ~9× less |
| Opus | 64 kbit/s | 4 Mbit/s | ~18× less |

**But bandwidth may not be what sets the limit.** Measuring Opus put encoding 64 channels at
**~14% of a fast desktop core** and **43.5% of one Pi 5 core** (`docs/research/opus.md` — best-of-three
figures, governor at `performance`; a single sample of the same probe read 39%, which is how easily
this number lies). Decoding is 15% of a Pi core. The probe runs the eight block encoders
**sequentially on one thread**; eight threads, or four, divide that across the Pi's four cores, so
the machine cost is closer to 11–15%. **The Pi number is in, and it fits — but the codec is the
entire CPU budget of phase 2**: a phase 2 implementation that does not thread per block will be
CPU-bound on a single core and will fail under load. That is a design requirement, not an
optimisation. If it still does not fit at 64, phase 2 becomes fewer encoded channels, a faster
appliance class, or encoding only some blocks — which the per-block payload type already makes
possible.

**Latency cost is real and must be counted against the A/V budget.** An Opus encoder adds the frame
duration **plus its algorithmic delay** to the transport floor. At 48 kHz that delay is
**6.50 ms** — `OPUS_GET_LOOKAHEAD` returns **312 samples**, measured on the Pi and on the laptop, and
the encoder's internal `4 ms` constant is measuring something else (`docs/research/opus.md`). So the
cost is **frame + 6.5 ms**: **~26.5 ms** with 20 ms frames, ~16.5 ms at 10 ms, ~11.5 ms at 5 ms.
Every millisecond of it is headroom the operator loses when lining audio up with vision, and it
comes out of the same budget the clock module needs for drift. The delay line subtracts it, and the
codec's share is shown to the operator rather than inherited silently.

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

### Sequencing: the appliance is finished first

> **Overridden 2026-09-17, by the owner.** With **one** appliance rather than two, the Mac endpoint is
> the product that can actually be built and proved from here, so it starts now (ADR 0005 carries the
> decision and the consequence). The paragraph below is kept because its reasoning is still true: the
> core is being extended before its hardware acceptance is complete, and a change made for the Mac
> product lands on the appliance too. The appliance's remaining hardware items (#10, #11, #16's
> clean-Pi run) are deferred, not closed.

**Decided 2026-09-16: none of this is started until the appliance's v1 is complete** — phase 1's
tickets closed, including its hardware acceptance rather than deferring it.

The reason is the one this project keeps rediscovering: the Mac endpoint is built on the same core, so
starting it early takes time from the thing that proves that core against reality, and it invites
changing `wire`, `transport` or `engine` *before* they have been through real hardware once. The
appliance is the only artefact that can tell us the core is right; two products growing on an
unproven core would mean changing it under both.

This was written on this page as "it is not blocked on v1" and the owner overruled that. The earlier
wording was true about *dependencies* and wrong about *priority*, which is the distinction that
matters here.

### Settled: it is the family's endpoint, not an SRT bridge

**Decided 2026-09-16: this application only ever talks to this project's appliance** — appliance in
the field, Mac in the studio. That collapses the work rather than expanding it: one wire format, one
block structure, one 1 ms period, one never-drop policy, and a configuration in the same shape as the
appliance's. No MPEG-TS, no SDP, no second payload convention, no interop matrix.

### Architecture (recommended, and ADR 0005 records it)

Two facts about CoreAudio invert the appliance's model, and both come from the platform rather than
from taste:

- **The device calls us; on the Pi, we called the device.** There, the engine paces itself by blocking
  in `read()`. Here, CoreAudio drives a *render callback* at its own I/O cycle and expects the data to
  be there. The two sides never meet directly.
- **That callback is realtime.** No allocation, no locks that can block, no logging, no syscalls.

So the boundary between them is a **lock-free single-producer/single-consumer ring per direction** —
and the useful part is that this boundary is already `audio::AudioBackend`:

| Piece | Where | New? |
|---|---|---|
| `wire`, `transport`, `engine` | the shared core, unchanged | no |
| `CoreAudioBackend`: an AudioUnit on a CoreAudio device, rings inside it; `read()`/`write()` on the engine side, the render callback on the device side | the application | **yes, one file** |
| Float32 ↔ `s24_3le` conversion | the application's side of the ring, beside the wire format | part of the above |
| Resampler (ADR 0003) | **the receive path only** | reuse |
| UI: device picker, link configuration, status | the application | yes |

**Where Float32 meets bytes is no longer a worry, and that is verified rather than assumed.** CoreAudio
is natively 32-bit float, and BlackHole's documentation states its 32-bit float is "lossless for up to
24-bit integer" — so the conversion is lossless in the direction that matters, and the byte-verbatim
principle (ADR 0001) survives meeting a float-native platform. The conversion happens once, on the
application's side of the ring, as ordinary testable C++ rather than code in a realtime callback.

**The clock answer is asymmetric, and that halves the work.** We do not own the Mac's device clock —
CoreAudio and the DAW do — so the incoming stream must be rate-adapted to it, while the outgoing
direction needs nothing at all, because the appliance's own clock module already reconciles what
arrives. Note where this differs from `AES67-VSC` ADR 0002 ("the virtual sound card owns the clock"):
that reasoning holds when you *are* the device, and here we are a client of one. One resampler, one
direction.

### Two ways to be a device, and the first is much cheaper

1. **Bind to a virtual device that already exists — recommended for v1.** An AudioUnit on a CoreAudio
   device gives the DAW something to select, and needs no driver, no privileged install and no
   notarised bundle. The obvious companion is **BlackHole**, verified 2026-09-16: **GPL-3.0**, which
   is exactly this project's licence (ADR 0002), **64 channels** available, installed by `.pkg`.
   *This corrects an earlier note on this page that called bridging to an existing device "much less
   useful" — with a virtual loopback device it is no less useful and far less work.*
2. **Ship our own virtual device — only if that dependency is unacceptable.** An `AudioServerPlugin`
   loaded by `coreaudiod`, with shared-memory rings in place of in-process ones. Same realtime
   discipline, now across a process boundary, and the first signed and notarised bundle this project
   has shipped. Note also that a loopback device sums what is played into it, so a *duplex* bridge
   needs either two devices or two disjoint channel groups on one — worth measuring before designing
   the channel layout.

The second is a second implementation of the same seam, which is exactly what ADR 0004 exists to keep
possible. Doing the first does not foreclose it.

### The order of work, cheapest risk first

**After the appliance is complete** — the order below is the order *within* this product, not a queue
running alongside phase 1. Step 1 is deliberately the smallest possible thing that proves the seam:
no driver, no signing, no appliance, no network.

1. `CoreAudioBackend` against a real CoreAudio device, with the engine running end to end on the Mac
   and **no appliance and no SRT involved**: the loopback test we already have, on real hardware.
2. The rings and their realtime discipline, exercised by a test that starves and floods them — before
   any UI exists to hide a mistake.
3. Link the real `transport::Link` and point it at an appliance.
4. The UI: device picker, link configuration, status.
5. Only then, if ever, the virtual device and the installer.

### Non-goals for this product

No video. No mixing, EQ, metering or plugins — the entire point is to hand the audio to something
that already has them. No Windows or Linux build: if it is not macOS it is not this product, and the
appliance covers the headless case.
