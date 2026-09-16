# ADR 0004: A second product over the same core

Status: **accepted** (2026-09-16), on the owner's product decision. The decision recorded *here* is
the architectural constraint that decision forces on code which already exists; the product's own
questions are open and listed in `docs/ROADMAP.md`.

## Context

The roadmap gains a second product: a standalone macOS application that bridges SRT to CoreAudio so a
DAW or DaVinci Resolve can mix live. It is not a phase of the appliance — it carries no AES67, no
PTP, no RAVENNA and no daemon — and it is not the appliance with a different audio backend either.
Its lifecycle differs (a window a person opens and closes, not a service that must survive being left
alone), its distribution differs (a signed and notarised macOS bundle, not an installer script for a
Pi), and its user differs (someone who wants to record or process audio, not someone who wants a
routing grid).

What it *is*, though, is built from parts that already exist. At the moment this decision was made,
`wire`, `transport` and `engine` were complete and tested, with the whole suite running on macOS as
well as on Linux. The only missing piece is a `CoreAudioBackend`.

So one core is about to be linked into two products. That is the decision worth recording, because it
is nearly free while there is one product and expensive to retrofit once there are two.

## Decision

**The core stays platform-neutral, and the audio device is its only extension seam.**

`wire`, `transport` and `engine` may not acquire a dependency on ALSA, on Linux, on `aes67-daemon`,
on PTP, or on the appliance's configuration shape. Everything platform-specific stays behind
`audio::AudioBackend` or in a front-end, and the appliance's `app` and the macOS application are two
front-ends over one core.

This is roughly what `audio/backend.hpp` was already shaped for — two very different devices
(`NullBackend`, `RavennaBackend`) behind one byte-based interface — but nothing enforced it until
now, because there was only ever one product to link it into.

## Reasoning

- **The alternative is a second copy of the wire format.** A forked Mac application would drift, and
  the drift would be undetectable until two products at a site disagreed about the bytes on a link.
  The wire format's amendment (ADR 0001) is a document that would then have to exist twice.
- **The expensive half is already tested on the target platform.** The core builds and runs its suite
  on macOS today, so the Mac application adds a backend and a user interface — not a second
  implementation of the hard parts.
- **The constraint costs a code review now and a migration later.** Keeping `engine` free of the
  daemon is a sentence in a review while there is one product; untangling it when two depend on it is
  a release.
- **It preserves the option not to build it.** Nothing here commits the project to the Mac
  application. It only says the core must not be welded to the appliance while that question is
  still open.

## Rejected: fork the repository for the Mac application

The fastest way to start and the worst way to continue: two copies of the wire format, two copies of
the transport's policy decisions, and the family's own products unable to talk to each other with any
confidence. Rejected on the wire format alone.

## Rejected: make the Mac application a mode of the appliance binary

One binary, one more flag, and a GUI bolted on beside a systemd unit. The lifecycles differ in ways
that matter: the appliance is a service that has to keep running unattended, and the Mac application
is a window somebody opens. Distribution differs too. Rejected as one program pretending to be two.

## Rejected: build the Mac application first and back-port the core

It would be built against the appliance's assumptions anyway — that is where the wire format and the
transport decisions came from — and the appliance is the nearer product. No advantage over adding a
backend to the core that exists.

## Consequences

- **A third platform axis.** `WITH_ALSA` and `WITH_SRT` gain a sibling (`WITH_COREAUDIO`), and the
  `AUTO` detection pattern is what makes that a configuration rather than a fork.
- **Float32 becomes a real question.** The byte-verbatim principle (ADR 0001) meets CoreAudio's
  native format. The conversion has to happen once and be named; this ADR does not decide where, and
  it is the first genuine tension with that principle.
- **The clock question returns in a new form.** ADR 0003 answered two independent 48 kHz domains by
  continuous resampling, and that machinery applies. Which side owns the host clock — whether the
  virtual device slaves to the stream — is a fresh decision.
- **The appliance's non-goal is scoped rather than deleted.** `docs/spec/0001-aes67-srt.md` said "no
  macOS or Windows runtime target"; it now says no such target *for the appliance*, which still has
  none.
- **No effect on v1, and no work pulled forward.** Nothing here adds work to the appliance's tickets,
  the constraint above is already satisfied by the code as it stands, and **the product itself waits
  until the appliance's v1 is complete** — the owner's sequencing decision, recorded in
  `docs/ROADMAP.md`. The appliance is what proves this core against real hardware, and it goes first.
