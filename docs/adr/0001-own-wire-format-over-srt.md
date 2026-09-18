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

## Amendment, 2026-09-16: a frame does not fit in one SRT message

Ticket 02's research found that this ADR's opening claim — "one frame per SRT message" — is
**wrong**. Correcting a decision of record with an amendment rather than by quietly editing the
text above, because the wrong version is what informed the work between the two.

Live mode caps a single send at `SRTO_PAYLOADSIZE`, "which can't be larger than 1456 bytes (1316
default)" (`Haivision/srt` `v1.5.7`, `docs/API/API-functions.md:1926`; the constants are at
`srtcore/srt.h:295,299`). A frame of eight L24 blocks is **9312 bytes**. **A frame therefore spans
roughly seven SRT messages**, and fragmenting and reassembling is the transport's job (ticket 07),
not the format's.

What is *not* affected: the format. The ceiling applies to what one `srt_sendmsg` call may carry,
not to what a frame may contain, so no field, size or version changes. Reassembly needs no new
header field either, because a frame is self-describing — the frame header gives the block count
and each block header gives the length of the payload after it, so the total is computable as bytes
arrive, and SRT guarantees ordering.

See `docs/research/libsrt.md` for the citations, and for the rest of what that research changed.

## Amendment, 2026-09-18: a frame needs a header per message, after all

The amendment above concluded that "reassembly needs no new header field either", on the grounds
that a frame is self-describing and "SRT guarantees ordering". **The ordering half is true and the
conclusion is still wrong.** SRT guarantees order but not *completeness*: in live mode a packet that
arrives after its play time is dropped (`TLPKTDROP`), and the message it belonged to is dropped with
it. The byte stream then has a hole in it, and a receiver counting bytes cannot see one — it reads a
frame length from the wrong offset and desynchronises.

**Measured, at 64 channels.** Every message after the missing one was misread: the log filled with
`checksum ... stored 0` and `magic: not an aes67-srt frame`, the reassembler never resynchronised,
the playout overran, and the receiver collapsed from ~1000 frames/s to **277** with the SRT buffer
climbing to 993 ms. An 8-channel frame fits in one message, so this path had never been exercised —
which is why 8 channels worked and 64 did not.

**Decision: each SRT message begins with an 8-byte fragment header** (`src/wire/frame.hpp`):

```
0  magic[2]        'A','F'
2  frame_sequence  uint32 little-endian
6  index           0-based fragment position
7  count           fragments in this frame, 1..255
```

A receiver now sees that a fragment is missing the moment the next one arrives out of sequence: it
abandons that frame, counts it, and resynchronises on the next frame's first fragment. Loss costs one
frame, not the stream. This is a **breaking wire-format change** — both ends must run the same
version — which is acceptable because both ends are ours. The frame's own header is unchanged; the
fragment header is a transport concern that happens to live in the format, and the format's checksum
still catches corruption within a frame.

Consequence for the layering: `wire::Reassembler` is now fragment-aware, and the frame length in the
frame header is no longer how the receiver decides a frame is complete.

