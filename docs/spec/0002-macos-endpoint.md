# Spec: 0002 — the macOS endpoint

Status: **draft for review.** Written 2026-09-19 when the device decision was reversed (ADR 0007).
The capability map below is gated: module boundaries, dependency direction and build order are
reviewed before any module is implemented.

This spec is the second product's own. The appliance is spec 0001 and is not changed here; the
endpoint only ever talks to this project's own appliance (ADR 0004/0005), so there is no interop
surface and no second wire format.

## Objective

**A DAW or Fairlight session on a Mac works the site's audio live, in both directions, with nothing
installed on the Mac but this product.**

The endpoint receives 64 channels from the appliance and presents them to CoreAudio as a device a DAW
can select and record; it also carries 64 channels the other way, from the DAW to the site's AES67
fabric. It does this with its **own CoreAudio device** (ADR 0007), so the user does not install
BlackHole and the two directions do not hear each other.

**Success looks like**, on a Mac with only this product installed:

- The device appears in Audio MIDI Setup and in a DAW's device list, with 64 in and 64 out.
- A DAW records all 64 channels of the site's audio and plays 64 channels back to the site.
- Both directions run at once, and the endpoint does not hear its own playout.
- A fault in the driver does not take the Mac's audio down, and uninstalling restores the machine.

**The user.** A broadcast or audio engineer in a studio, working in Fairlight, Pro Tools, Reaper or
Logic, on the same Mac as the appliance's counterpart at the site. They are not a developer, and the
driver install is the one step that asks for an administrator password.

## Capability map

| Module id | Responsibility | Depends on |
|---|---|---|
| `hal-driver` | The Audio Server Plug-in: one 64-in/64-out device, shared-memory audio transport, `Info.plist` (sandbox, mach service), and install/uninstall into `/Library/Audio/Plug-Ins/HAL/` | — |
| `endpoint-bridge` | The application-side `AudioBackend`: maps the driver's shared memory, presents the engine's ordinary blocking `read()`/`write()` contract, applies levels and the test signal as it already does | `hal-driver` |
| `duplex-endpoint` | Both directions through the one device without self-hearing; role/config wiring; the return path (issue #31) | `endpoint-bridge` |
| `mac-app` | `.app` bundle (the menu bar promoted), `.pkg` installer and uninstaller, launchd, signing and notarisation | `hal-driver`, `endpoint-bridge` |

**Build order:** `hal-driver` → `endpoint-bridge` → `duplex-endpoint`. `mac-app` packaging starts
once `hal-driver` installs, in parallel with `endpoint-bridge`; signing (issue #30) attaches to
`mac-app` last.

Dependencies point one way, no cycles. Contracts between modules belong to the provider's spec.

## The device

**One device, 64 input channels and 64 output channels, 48 kHz, 32-bit float to the HAL.** Not a
loopback: the input and output streams are independent, which is what makes duplex possible.

| Property | Decision | Why |
|---|---|---|
| Channels | 64 in, 64 out | The link's ceiling; a DAW sees the full fabric |
| Sample rate | 48 kHz | The AES67 rate and the wire format's; no conversion in the plug-in |
| HAL format | 32-bit float | CoreAudio's native; ADR 0005's conversion stays on our side of the ring |
| Device name | **`AES67-SRT`** | It is what an operator selects in a DAW and what Audio MIDI Setup shows; decided by the owner 2026-09-19 |
| Ring depth | at least four engine periods, rounded up to a power of two | It absorbs cadence, not drift — the playout buffer is still the jitter buffer |

**Built and confirmed by the spike (#35, 2026-09-19).** A libASPL v3.1.2 plug-in presenting this exact
shape installs in **2.6 s** including the `coreaudiod` restart, and appears in `system_profiler` as
**64 in, 64 out, 48 kHz, Virtual**; it uninstalls to a byte-identical device list. libASPL supplied
every piece of HAL dispatch — device, streams, formats, callbacks — and nothing was hand-written, so
the unknown this ticket carried is retired. Two libASPL defaults were the product's to decide, and both
are now decided: **`CanBeDefault` is set false** — this is a bridge a DAW selects, not a sound device,
and letting macOS route system alerts into it would be silently confusing — and **`EnableMixing` stays
true**, because the send path wants the mix of every client.

The plug-in does exactly three things: report the device's properties to the HAL (from libASPL's
defaults plus our stream layout), and in the two I/O callbacks copy to or from a shared-memory ring.
**No allocation, no locks that can block, no logging, no syscalls, no logic.** Format conversion,
resampling, levels and the test signal all stay in the application, which is where ADR 0005 put them.

**The shared-memory layout is the contract** between the two halves. It is a versioned struct — ring
capacities, channel count, per-direction read and write indices as atomics — in the spirit of
`audio::FloatRing`, and it is what both halves are tested against a fake of.

### Ring behaviour at an edge

**In steady state neither ring should reach an edge.** The receive path is rate-adapted to the Mac's
device by the resampler, and the transmit path is a single clock domain, so the rings absorb only the
mismatch between the engine's period and CoreAudio's I/O cycle — a *cadence* difference, not *drift*.
An edge is therefore a fault, and the two rules that apply are the project's usual ones: never hide
it, and never let it grow latency.

| Side | Edge | What it does | Why |
|---|---|---|---|
| us → host (`OnReadClientInput`) | **starved** | pads with **silence**, bumps an underrun counter | matches the engine's existing `silence_periods`; on a monitor or mix path a hole is better than a repeat, which buzzes on tonal material |
| host → us (`OnWriteMixedOutput`) | **full** | drops the **oldest** audio, keeps the newest, bumps an overrun counter | bounds latency instead of growing it — the same argument that turned `TLPKTDROP` on (ADR 0006's sibling decision) |
| either | the other edge | the non-realtime producer blocks, or the consumer waits, bounded and interruptible by stop | `read()`/`write()` is already blocking; the bound matters because an unbounded block is a stop that never lands |

**The drop-oldest rule deliberately diverges from `FloatRing`.** `FloatRing` truncates on a full ring,
which drops the *newest*; on the realtime producer that keeps stale audio and lets latency climb. The
lock-free single-producer/single-consumer shape, the atomics and the flood/starve tests are still the
model; only the full-ring policy differs here. Both counters surface on the page, because the plug-in
cannot log and a lost period that nothing reports is the operator's least diagnosable failure.

## Audio path, and what is reused

The endpoint reuses the core unchanged: `wire`, `transport` and `engine` are shared with the
appliance (ADR 0004). What is new is the device seam only.

- **Receive:** SRT frames → blocks → engine's clock and delay → `write()` → output ring → the DAW
  reads the device's input. The **resampler is on this path only** (ADR 0005): the Mac does not own
  its device clock, CoreAudio and the DAW do.
- **Transmit:** the DAW plays to the device → input ring → `read()` → engine → blocks → SRT frames.
  **No resampling** — the appliance's own clock module already reconciles what arrives.
- **Levels and the test signal** are applied where they already are, in the engine's ordinary code,
  not in the plug-in.
- **Channel mapping** is the existing per-block `blocks[].channels`; the return path needs no new
  engine feature, which is what issue #31's body predates.

## Commands

House commands, the same shape as spec 0001.

```
Configure : cmake -S . -B build                        # Apple Silicon + macOS; -DWITH_HALDRIVER=ON
Build     : cmake --build build
Driver    : cmake --build build --target aes67-srt-hal # the Audio Server Plug-in bundle
App       : cmake --build build --target aes67-srt-mac # the application (menu bar + endpoint)
Test      : ctest --test-dir build --output-on-failure
Install-drv: sudo scripts/install-hal.sh               # into /Library/Audio/Plug-Ins/HAL/, then coreaudiod
Uninstall-drv: sudo scripts/uninstall-hal.sh
Package   : scripts/build-pkg.sh                       # a signed .pkg (issue #30 attaches here)
Gate      : scripts/check.sh                            # unchanged; the HAL target is macOS-only
```

The HAL target is `AUTO` like the other platform pieces: on Linux the bundle is not built and the
bridge's tests skip, exactly as the transport tests skip without libsrt.

**libASPL must be a full clone, not `--depth 1`.** Its CMake derives its version from `git describe`
and fails on a shallow clone with no reachable tag (the spike hit exactly this). A full clone, or
`git fetch --unshallow`, is required — worth encoding in whatever fetches it.

## Project structure

```
src/audio/shared_ring.{hpp,cpp}   the shared-memory contract (#36) — built
src/audio/hal_driver/             the Audio Server Plug-in sources -> the bundle
src/audio/hal_backend.{hpp,cpp}   the application-side shared-memory AudioBackend
src/mac/                  the app: endpoint main, menu bar, app bundle resources
scripts/install-hal.sh    install the driver and restart coreaudiod
scripts/uninstall-hal.sh  remove it and restart coreaudiod
scripts/build-pkg.sh      the signed installer
tests/test_shared_ring.cpp        the ring contract (order, wrap, edge, two processes) — built
tests/test_hal_*.cpp      the plug-in's realtime path and the bridge contract
```

The plug-in is the only code in this repository that runs in another process and in a realtime
context. It is isolated in its own directory for that reason, so review can see all of it at once.

## Code style

Unchanged from spec 0001 — C++17, 2-space indent, `#pragma once`, `snake_case`/`PascalCase`,
comments that explain *why*. The plug-in follows libASPL's C++ shape; the application side follows the
existing `AudioBackend` seam. One rule is added for the plug-in specifically:

**The plug-in's I/O callbacks contain no calls that can block or allocate.** Not `new`, not
`std::vector` growth, not `printf`, not a mutex, not a `shared_ptr` copy. The shared-memory ring is
raw pointers and atomics. A test asserts the plug-in's realtime path has no heap allocation.

## Testing strategy

The house framework, as spec 0001. The point of the split is that **the hard parts are testable
without a signed driver**: the shared-memory contract, the ring discipline and the bridge are
ordinary code, exercised against a fake of the other half.

| Module | What the tests must prove |
|---|---|
| `hal-driver` | The shared-memory layout round-trips between two processes; a full ring truncates and counts, a starved one returns short and counts (the same assertions `FloatRing` already passes); the realtime callbacks allocate nothing |
| `endpoint-bridge` | The engine's blocking `read()`/`write()` contract holds across the shared memory; a period written by the engine appears intact on the other side, both directions; a missing or mismatched driver is refused by name |
| `duplex-endpoint` | Both directions run at once; playout does not appear in the received stream (the self-hearing assertion); return audio reaches the wire as the configured blocks |

**What CI can and cannot do.** CI builds and tests the shared-memory and bridge logic on both Linux
and macOS without a driver, and builds the bundle on macOS. **No CI runner can install a HAL driver
and restart `coreaudiod`**, so the plug-in's behaviour in a real DAW is a manual acceptance check:
the device appears, records and plays, both ways, on real hardware. No module is "done" on CI alone.

## Boundaries

**Always**
- Run `scripts/check.sh` before committing.
- Keep the plug-in's realtime path free of allocation, blocking and logging.
- Version the shared-memory layout, and refuse a mismatched version by name.
- Record a decision in `docs/adr/` when it changes, and update the spec before the code.
- Test the shared-memory contract against a fake before a driver exists.

**Ask first**
- Adding a dependency — `libASPL` is the one this spec needs, and ADR 0007 records it.
- Changing the device's shape: channel counts, sample rate, or the input/output split.
- Any install that does not come with an uninstall that restores the machine.
- Making the driver install non-optional.

**Never**
- Put logic, SRT, resampling or format conversion in the plug-in.
- Install the driver without authorisation, or write outside its documented paths.
- Load the plug-in into `coreaudiod` in a way that can take the system's audio down with it.
- Ship a driver unsigned.

## Success criteria

- [ ] The device appears in Audio MIDI Setup with 64 in and 64 out, and in a DAW's device list
- [ ] A DAW records 64 channels of the site's audio through it
- [ ] A DAW plays 64 channels through it and they reach the site's AES67 fabric
- [ ] Both directions run at once without self-hearing
- [ ] The plug-in's realtime callbacks allocate nothing, asserted by a test
- [ ] Uninstall removes the device, restarts `coreaudiod`, and leaves no trace
- [ ] A `.pkg` installs the app and driver signed and notarised (issue #30)

## Open questions

1. **Whether the ring behaviour above is right, against the machine.** The edge rules are decided and
   reasoned, but the ring depth and the audibility of a padded or dropped period are not measurable
   off a CI runner; the spike and the first hardware run are where they get tested.
2. **One plug-in hosting both streams, or two streams in one device?** libASPL supports both; the DAW
   experience is the tie-breaker and needs a hand-test.
3. **Does the return path need any UI?** The gains, mutes and channel maps exist; whether the page
   needs a "send to site" view is a product question, not a device one.
4. **What the app is called, and whether the menu bar stays the whole UI.** The app exists for the
   install and the driver; whether it grows a window is open.