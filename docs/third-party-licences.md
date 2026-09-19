# Third-party licences

This project is **GPL-3.0** (ADR 0002). Everything it links must be GPL-compatible, and everything it
distributes must be accounted for. Verified against the installed packages on 2026-09-18.

| Component | Licence | How it is used | Obligation |
|---|---|---|---|
| **libsrt** | MPL-2.0 | linked — the transport | File-level copyleft. We do not modify it; upstream source is available. Compatible with GPL-3.0. |
| **libopus** | BSD-3-Clause | linked — the phase-2 codec | Attribution. |
| **libsamplerate** | BSD-2-Clause | linked — the clock's resampler | Attribution. |
| **nlohmann/json** | MIT | header-only, fetched at build (v3.12.0) | Attribution. |
| **cpp-httplib** | MIT | header-only, fetched at build (v0.56.0) | Attribution. |
| **alsa-lib** | LGPL-2.1 | linked on Linux — the RAVENNA backend | Dynamic linking, and the ability to relink against a modified library (satisfied by dynamic linking). |
| **CoreAudio** | Apple SDK | linked on macOS | A system framework; not redistributed. |
| **libASPL** | MIT | linked into the macOS endpoint's HAL driver (ADR 0007) | Attribution. Static library, distributed inside the plug-in. |

**Not linked — a separate program:**

- **`aes67-daemon`** is reached **over REST** and never forked, vendored or linked (spec decision 11).
  Its licence is its own; it is a dependency a site installs, not a library this project contains.

**Attribution** is satisfied by this file and the upstream notices, which the fetched headers keep.
Nothing here is a patent-encumbered codec: Opus is royalty-free by design, and AAC-LC — the roadmap's
second codec, not built — is deliberately not shipped because its patent pool makes that a commercial
decision rather than a technical one (`docs/research/opus.md`).
