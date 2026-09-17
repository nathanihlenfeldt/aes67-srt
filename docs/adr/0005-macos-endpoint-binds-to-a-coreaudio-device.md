# ADR 0005: The macOS endpoint binds to a CoreAudio device, and ships no driver

Status: **accepted** (2026-09-17). Adopted by the owner when this product was started; the reasoning was
written on 2026-09-16 as a proposal and is unchanged.

> **Adoption, and the sequencing override (2026-09-17, the owner's decision).** Two things were decided
> together.
>
> 1. **The sequencing is overridden.** ADR 0004 and `docs/ROADMAP.md` recorded that this product would
>    not start until the appliance's v1 was complete, including its hardware acceptance. With **one**
>    appliance rather than two, the Mac endpoint is the product that can be built and proved from here,
>    so it starts now. The consequence is accepted knowingly: **the core is extended before it has been
>    through real hardware a second time**, and any change made for this product lands on the appliance
>    too. The appliance's remaining hardware items (#10, #11, #16's clean-Pi run) are deferred, not
>    closed.
> 2. **v1 binds to an existing CoreAudio device — BlackHole — and ships no driver**, exactly as the
>    Decision below says. The owner considered the alternative (our own `AudioServerPlugin`) and chose
>    to reach it **later**: a device of our own is what the product ultimately wants, and the deferred
>    option below is not rejected, but it costs a privileged install, a signed and notarised bundle, and
>    a realtime path across the `coreaudiod` boundary — none of which is worth paying before the bridge
>    carries audio at all.
>
> The consequences section below is where the BlackHole dependency and the duplex-channel question are
> already written down.

## Context

The roadmap's second product carries this project's SRT audio to and from a DAW or DaVinci Resolve,
and it only ever talks to this project's own appliance. ADR 0004 established that it shares `wire`,
`transport` and `engine`, and that the audio device is the only extension seam.

That leaves one question with real consequences: **what, on the Mac, is "the device"?** The answer
decides whether this project writes a system audio driver — the first privileged, signed, notarised
component it has ever shipped — or an application.

Two properties of CoreAudio constrain the answer:

- **The device calls the application**, at its own I/O cycle, through a render callback. This inverts
  the appliance, where the engine paces itself by blocking in `read()`.
- **That callback is realtime**: no allocation, no locks that can block, no logging, no syscalls.

## Decision

**The application binds to an existing CoreAudio device, and ships no driver.**

`CoreAudioBackend` implements the existing `audio::AudioBackend` seam as an AudioUnit on a device the
user chooses — a virtual loopback device such as BlackHole, an aggregate, or any other. Between the
realtime render callback and the engine's ordinary blocking `read()`/`write()` sit two lock-free
single-producer/single-consumer rings of 32-bit float, one per direction.

- The **render callback** does nothing but copy to or from a ring and update an atomic index.
- **Float32 ↔ `s24_3le` conversion** happens on the engine's side of the ring, beside the wire
  format, in ordinary testable code.
- The **resampler** (ADR 0003) is instantiated on the receive path only.

A virtual device of this project's own — a thin `AudioServerPlugin` with shared-memory rings — is
deferred, not rejected. It would be a second implementation of the same seam, and the owner's decision
is to reach it later (see the adoption note above).

## Reasoning

- **The seam already exists, and this is the cheapest thing that could work.** The engine needs an
  `AudioBackend`. It does not need a driver, and it does not need to know what CoreAudio is.
- **No privileged component means no signing, no notarisation, no installer, and no way for a bug in
  this product to take down audio for every application on the machine.** That last point is decisive:
  an `AudioServerPlugin` runs inside `coreaudiod`, so a fault there silences the whole Mac. The
  appliance's own never-drop instinct argues against buying convenience with that.
- **The conversion is provably harmless.** CoreAudio is natively 32-bit float, and BlackHole documents
  its float as "lossless for up to 24-bit integer", so the byte-verbatim principle (ADR 0001) holds
  where it matters. Converting beside the wire format rather than inside a realtime callback also
  keeps it testable.
- **The appliance is the peer, so there is nothing to interoperate with.** One wire format, one block
  structure, one period, one policy — nothing here has to serve a general SRT world.
- **It is reversible.** If the dependency is later judged unacceptable, the driver is added behind the
  same seam and no decision below this one changes.

## Rejected: our own AudioServerPlugin first

The self-contained product, and the right one to reach *second*. It moves the realtime path into
another process, adds shared-memory synchronisation across a process boundary, requires codesigning
and notarisation this project has never done, and — because the plugin is loaded by `coreaudiod` —
turns a fault in our code into a fault in every application's audio. All of that before the first
version has proved it can carry audio at all. (The owner's later step: reach it once the bridge works.)

## Rejected: no virtual device, playing to the built-in output

The smallest possible thing, and it does not do the job: a DAW cannot select a program as an input.
The product exists so that something else can mix the audio.

## Consequences

- **A CoreAudio backend, and a realtime boundary inside it.** Because the rings are in-process they
  are testable: a test can starve and flood them with no driver, no signature and no second process.
- **A dependency on a virtual loopback device for v1**, which belongs in the product's requirements
  rather than being discovered by a user. BlackHole is GPL-3.0 — licence-compatible with ADR 0002 —
  and offers 64-channel builds.
- **Duplex routing needs care.** A loopback device sums what is played into it, so both directions on
  one device means two disjoint channel groups, and on two devices means two devices. To be measured
  before the channel layout is designed.
- **The ring is float whichever payload the link carries.** The L16/L24 choice belongs to the frame,
  where it already lives, and does not reach the ring.
- **The resampler is needed in one direction only** — a smaller clock story than the appliance's.
- **A user-facing wrinkle worth writing down early:** macOS gates input from a virtual device behind
  microphone permission, so the DAW will ask for it and a site will ask why.
