# ADR 0002: Licence under GPL-3.0

Status: **accepted** (2026-09-16). Raised by ticket 02. Decided on the owner's instruction — the
owner delegated this choice to the agent rather than reserving it, so the reasoning below is the
rationale of record, not a proposal awaiting a signature.

## Context

We link libsrt (Haivision SRT). What that does and does not require of our own licence is a factual
question, and ticket 02 answered it: libsrt is **MPL-2.0**, which is file-level copyleft attaching to
modified MPL-covered *files* that are distributed. We do not modify libsrt, and MPL-2.0 does not
extend to the larger work. **We may license this project however we choose.**

That is unusual here. The sibling `aes67-sip` had no choice at all: PJSIP (GPLv2+) and
`aes67-daemon` (GPLv3) forced GPL-3.0 on it. In this project the licence is a decision rather than a
constraint, which is exactly why it deserves an ADR instead of a copied `LICENSE` file.

## Decision

**GPL-3.0**, matching the sibling appliance. `LICENSE` holds the canonical text as published by the
Free Software Foundation, and nothing else in the repository carries a per-file notice.

## Reasoning

- **One licence across the suite matters operationally.** These appliances are installed and
  supported together at the same sites. A single licence keeps the documentation, the source
  availability obligation and the installer honest, rather than "it depends which component you are
  asking about".
- **The obligations are already understood and already accepted** in `aes67-sip`: source
  availability on distribution, and no relicensing of the combined work. Adopting GPL-3.0 takes on
  nothing new.
- **It keeps a door open at no cost.** If anything GPL is ever linked in — a GPL ALSA plugin, a GPL
  codec implementation from the roadmap's phase 2, a GPL measurement tool — GPL-3.0 is already
  compatible. A permissive choice would have to be revisited under time pressure, at the least
  convenient moment.
- **The value of this project is not in its source being secret.** It is an appliance for sites the
  owner runs and supports; a licence does not protect a moat that does not exist.

## Rejected: MIT or Apache-2.0

Not rejected because they are worse licences, but on the two grounds above: consistency across the
suite, and GPL-compatibility later. The concrete cost of permissive-now is that linking any GPL
component later forces relicensing the whole project — and the phase 2 codec work is precisely where
that could bite, since Opus is royalty-free but AAC-LC means an encoder whose provenance is a
licensing question (see `docs/ROADMAP.md`).

## Consequences

- `LICENSE` is the GPL-3.0 text, with no per-file notices beyond it, matching `aes67-sip`.
- Distribution (ticket 15's installer) must make source available — already the sibling's
  requirement, and the reason ticket 15 asked for this to be settled first. **It is now settled, so
  ticket 15 is unblocked on this point.**
- **Distribution carries the GPL-3.0 source-availability obligation**, settled here so that ticket
  15 can ship an installer without reopening it. The repository may also be built, tested and
  published as source freely — publishing source is not distribution of a binary, and either way the
  obligation is satisfiable and now known.
