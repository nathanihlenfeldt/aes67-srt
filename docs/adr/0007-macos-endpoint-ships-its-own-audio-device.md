# ADR 0007: The macOS endpoint ships its own CoreAudio device

Status: **accepted** (2026-09-19). The owner's decision, taken when the Mac product was made
installable. It supersedes ADR 0005's *"binds to an existing CoreAudio device, and ships no driver"*
decision; the rest of ADR 0005 — the rings, the realtime discipline, the conversion beside the wire
format, the one-direction resampler — carries over unchanged.

## Context

ADR 0005 chose to bind the endpoint to an existing virtual device, BlackHole, and to defer a device
of our own, so the bridge could carry audio before paying for a privileged, signed driver. That
worked: audio crosses the seam today (`docs/research/macos-endpoint.md`).

Three things have since moved the decision.

1. **BlackHole is a loopback, and that is the whole of the duplex problem.** A loopback sums what is
   played into it and presents the sum at its input. So if received audio is written to the device
   *and* a DAW also plays to it, the endpoint hears itself. The product's headline use — a DAW mixing
   a remote contribution — is one-directional under a loopback, which is what issue #31 records.
2. **The installability ADR 0005 deferred is being paid anyway.** The endpoint is being made
   installable for someone else (issue #27) and distributed signed (issue #30). A privileged install,
   a bundle and a notarised identity are now in the plan regardless.
3. **A loopback is a dependency on the user's machine.** BlackHole is a third-party system driver the
   user must install, and one under GPL-3.0. Our own device removes it.

## Decision

**The endpoint ships an Audio Server Plug-in of its own: one device with 64 input and 64 output
channels, replacing BlackHole.**

The plug-in is a pure copy shim. It presents the device to the HAL and moves audio between the HAL's
I/O callbacks and a **shared-memory ring**, one per direction. It performs no SRT, no resampling, no
format conversion, no allocation and no logging. Everything that decides anything stays in our
application process, which maps the same shared memory through a new `AudioBackend`. This is the
realtime discipline ADR 0005 set out, moved across a process boundary rather than invented again.

One device with independent input and output streams is **not** a loopback: what a DAW plays to the
output is ours to send, and what we put on the input is the DAW's to read, with no summing and no
self-hearing. **Duplex is therefore a property of owning the device, not a separate feature.**

## Reasoning

- **The custom device is the cheapest way to get duplex, not the expensive one.** On BlackHole, duplex
  costs a second device (or a split channel map) *and* a channel-map feature *and* a device layout the
  user must understand. Owning the device makes both directions fall out of two independent rings.
- **The seam already exists and nothing outside `audio` changes.** ADR 0004 reserved the audio device
  as the only extension point; `wire`, `transport` and `engine` are untouched, and the engine is
  already duplex-capable — the documented run reaches "64 out / 64 in available".
- **The realtime risk is bounded by keeping the plug-in trivial.** ADR 0005 feared a fault in our code
  silencing the Mac. That is answered by structure rather than by care: the plug-in copies and nothing
  else, so there is no logic in the realtime path to get wrong. It runs in a sandboxed host process of
  its own, not inside `coreaudiod`, so the worst case is our device disappearing, not the system audio
  server failing.
- **libASPL is the scaffolding.** `gavv/libASPL` is MIT (GPL-compatible), C++17, under the same
  licence family, and the same shape as BlackHole; it is what the Roc virtual audio device ships on.
  Hand-writing the HAL property dispatch is exactly the cost ADR 0005 was avoiding, and the library
  removes it. Adding it is an "ask first" dependency, so it is recorded here.
- **It is still reversible in the small.** The AudioUnit backend is removed only once the device is
  proven, so a working path remains until then; and because the shared-memory layout is the contract
  between the two halves, either half can be tested against a fake of the other.

## Consequences

- **Three new modules and a prerequisite install**, specified in `docs/spec/0002-macos-endpoint.md`:
  `hal-driver`, `endpoint-bridge` and `mac-app`. The driver installs to
  `/Library/Audio/Plug-Ins/HAL/` with authorisation, followed by a `coreaudiod` restart.
- **Signing and notarisation move from optional to required** (issue #30). A HAL driver is not
  distributed unsigned.
- **The BlackHole dependency is retired.** The AudioUnit backend, the installer's BlackHole check and
  the `audio.device` binding to an external device are removed once `endpoint-bridge` carries audio
  both ways. (BlackHole is a user-installed program, never linked or redistributed, so it is not in
  `docs/third-party-licences.md`; what changes is the manual's install steps, not the audit entry.)
- **`libASPL` is added to the licence audit.** Unlike BlackHole it *is* linked and distributed inside
  the plug-in, so MIT attribution joins `docs/third-party-licences.md`.
- **Shared memory and a Mach service are the new boundary surface**, declared in the plug-in's
  `Info.plist`, with the in-process `audio::FloatRing` as the design model.
- **The appliance is unaffected.** No wire-format, transport or engine change, so a Mac running the
  new device still talks to an appliance that was never reflashed for it.
- **The device name is a user-facing decision.** It is what an operator selects in a DAW and what
  appears in Audio MIDI Setup; it is recorded as an open item in the spec rather than chosen here.