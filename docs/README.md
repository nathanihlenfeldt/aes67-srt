# Documentation

Where each kind of document lives, and who it is for. If you are not sure where to start, the
**manual** is for people running the product, the **specification** is for people building it.

## If you are installing or running it — the manual

| Document | What it is |
|---|---|
| [`manual/index.md`](manual/index.md) | the overview: what it is, the two ends, the words it uses |
| [`manual/getting-started.md`](manual/getting-started.md) | install both ends and get first audio |
| [`manual/configuring.md`](manual/configuring.md) | the link, the codec, the channels, the A/V delay |
| [`manual/operating.md`](manual/operating.md) | start/stop at each level, monitoring, updating, removing |
| [`manual/use-cases.md`](manual/use-cases.md) | remote production into a DAW, a Fairlight-style studio, thin links |
| [`manual/troubleshooting.md`](manual/troubleshooting.md) | the failures you will actually meet |

## If you are building or extending it

| Document | What it is |
|---|---|
| [`spec/0001-aes67-srt.md`](spec/0001-aes67-srt.md) | **the specification.** The frozen decisions, the wire format, the clock problem, the boundaries. Start here for engineering. |
| [`ROADMAP.md`](ROADMAP.md) | what follows v1, and what is deliberately not in it |
| [`adr/`](adr/) | decisions of record. `0001` the wire format; `0002` the licence; `0003` clock reconciliation; `0004` the shared core; `0005` the macOS endpoint; `0006` the A/V delay. |
| [`research/`](research/) | what was verified against primary sources, with citations — `libsrt`, `clock-recovery`, `av-delay`, `opus`, `aes67-daemon-64ch`, `macos-endpoint` |
| [`api.md`](api.md) | the REST API and every route |

## If you are in the field

| Document | What it is |
|---|---|
| [`runbooks/commissioning.md`](runbooks/commissioning.md) | bringing up a site and a studio |
| [`runbooks/macos-endpoint.md`](runbooks/macos-endpoint.md) | the studio endpoint, including the unsigned-build step |
| [`runbooks/hardware-session.md`](runbooks/hardware-session.md) | the one hardware session the project is waiting on |

## Project housekeeping

| Document | What it is |
|---|---|
| [`releasing.md`](releasing.md) | the version scheme and the release checklist |
| [`third-party-licences.md`](third-party-licences.md) | the dependency licence audit |
| [`agents/`](agents/) | how the engineering skills read this repository |

Root files: [`README.md`](../README.md) (the front page), [`CHANGELOG.md`](../CHANGELOG.md) (what each
version changed), [`AGENTS.md`](../AGENTS.md) (how to work in the repo), [`CONTEXT.md`](../CONTEXT.md)
(the glossary).