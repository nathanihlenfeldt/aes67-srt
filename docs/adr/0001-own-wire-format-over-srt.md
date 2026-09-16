# ADR 0001: Our own wire format over SRT, not RTP-over-SRT

Status: accepted (2026-09-16). Implemented by ticket 06 (`wire`).

## Context

The link carries 64 channels — eight blocks of eight — of uncompressed L24 PCM at
48 kHz in each direction, in a single SRT stream per link. Something has to define
what a message on that stream contains. Two candidates were real:

1. **RTP over SRT.** Reuse the AES67 packetisation: an RTP header per 8-channel
   stream, timestamps, sequence numbers, and SRT underneath as a reliable pipe.
2. **Our own frame.** One header per *frame* covering all eight blocks, with a
   single sample position for the whole frame.

SRT is reliable and ordered. It retransmits, it preserves message boundaries, and
it delivers a byte stream whose ordering is already guaranteed — which is the
property RTP's sequence numbers exist to detect the absence of.

## Decision

Carry our own frame format. One frame per SRT message:

- a fixed 28-byte frame header: magic, version, flags, link id, block count, and
  the two 64-bit integers that matter — `sequence` and `sample_position`;
- eight bytes of block header per block, carrying `index`, `payload_type`,
  `channels` and `payload_bytes`;
- the block payloads verbatim from the AES67 side;
- a trailing CRC-32 over everything before it.

`sample_position` is **48 kHz frames since the stream started**, and it is shared
by every block in the frame. That single field is the cross-block synchronisation
guarantee: the requirement is that all 64 channels arrive sample-aligned, and with
one position per frame they are aligned *by construction* rather than by
reconciliation after the fact.

`payload_type` is **per block, not per link**. v1 carries PCM only, so this field
is dead weight today. It is there because phase 2 adds Opus and AAC-LC
(`docs/ROADMAP.md`), and adding a payload type to a frozen format later means
every appliance already installed in another building has to be re-flashed in
lockstep. One extra byte per 1152-byte block buys the ability to add a codec
without a format break, and it is what makes mixed-mode links possible: PCM on the
blocks that matter, Opus on the rest.

Payload bytes are the AES67 payload verbatim, so L24 stays little-endian 3-byte
samples. Reordering samples on the wire would mean touching every byte twice in
each direction to satisfy a convention nobody reads.

## Consequences

- **The format is ours to version and ours to break.** Magic and version are in
  the header, so a future change is detectable rather than silently misread.
  Decode refuses an unknown version instead of guessing.
- **Unknown payload types are refused, and recognised-but-unsupported ones are
  refused differently**: Opus arriving at a PCM-only build says "not supported by
  this build", not "corrupt". Those are different problems for the operator.
- **We own framing bugs.** Nothing upstream will catch a length error; the size
  check and the CRC are the whole defence.
- **No RTP means no interop with anything that speaks RTP.** That was already
  true — the AES67 side is interop, because `aes67-daemon` handles RTP and SDP,
  and this link is a private hop between two of our own appliances.
- One CRC-32 per ~9.3 KB frame costs almost nothing at 1000 frames/s, and it
  turns a corrupted payload into a refusal rather than into noise in the mix.

## Rejected: RTP-over-SRT

- Sequence numbers and timestamps duplicate what SRT already guarantees. Two
  timebases to reconcile where one sample position will do.
- One RTP header per 8-channel stream, eight times per frame, for no benefit.
- RTP's timestamp is tied to its own clock model; our `sample_position` is
  directly the number of frames since the stream started, which is exactly what
  the clock module and the A/V delay line need.
- It would invite the question "so is this AES67?" — and it is not. This is a
  private transport between two appliances, not a contribution to a fabric.

## Rejected: no checksum

The link is SRT, which already guarantees delivery. But a CRC also catches
*corruption* that is not loss — a miswritten length, a half-filled buffer, a bug
in our own encoder — and a click in the programme audio is far more expensive
than four bytes per frame.
