#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/**
 * The on-wire frame.
 *
 * Pure: no sockets, no ALSA, no daemon, no clock.  Everything the transport
 * carries is one of these, in one direction, and nothing else appears on the
 * wire — so this is the narrowest part of the system and the one that is fully
 * testable on a development machine.
 *
 * Byte order is big-endian throughout (network order), with one deliberate
 * exception: block payload bytes are the AES67 payload verbatim, which for
 * L24 is little-endian 3-byte samples (ALSA's s24_3le).  We never reorder
 * samples, because doing so would mean touching every byte of every channel
 * twice for nothing.
 *
 * See docs/adr/0001-own-wire-format-over-srt.md for why this format exists
 * rather than RTP-over-SRT.
 */
namespace aes67_srt::wire {

/** "A67S". Detects a desynchronised stream before anything else can go wrong. */
constexpr uint32_t k_magic = 0x41363753;
constexpr uint16_t k_version = 1;

/**
 * A sanity ceiling on a frame's length, for reassembly rather than for the
 * format.
 *
 * A reassembler learns a frame's length from its own header and waits for that
 * many bytes, so a desynchronised stream carrying a plausible-looking magic and
 * a corrupted length field would otherwise have us buffering until the machine
 * dies. 256 KB is far above any real frame — a full 64-channel L24 frame is
 * 9312 bytes, and even a full second of 64-channel audio is ~74 KB — so anything
 * larger means the stream is not ours and reassembly should give up.
 */
constexpr size_t k_max_frame_bytes = 256 * 1024;

/** Eight, because AES67 allows no more than eight channels per stream. */
constexpr uint8_t k_max_blocks = 8;
constexpr uint8_t k_block_channels = 8;

constexpr size_t k_frame_header_bytes = 28;
constexpr size_t k_block_header_bytes = 8;
constexpr size_t k_checksum_bytes = 4;

/**
 * Payload types.  Per block, not per link: that is what lets a link carry PCM
 * on the blocks that matter and a codec on the rest (docs/ROADMAP.md, phase 2).
 */
enum class PayloadType : uint8_t { pcm_l24 = 0, pcm_l16 = 1, opus = 2, aac_lc = 3 };

/** The name as it appears in logs and refusals. */
const char* to_string(PayloadType type);

/**
 * Recognised by this format, whoever is asking.  False for a byte that is not a
 * payload type at all, which is a different failure from one this build cannot
 * decode yet: see supported_here().
 */
bool parse_payload_type(uint8_t raw, PayloadType* type);

/**
 * Decodable by this build.  v1 is PCM only, so Opus and AAC-LC are recognised
 * and refused rather than misread as PCM.
 */
bool supported_here(PayloadType type);

/** Bytes per sample for a PCM payload type; 0 for anything else. */
size_t sample_bytes(PayloadType type);

/** Payload bytes one block occupies at |frames| 48 kHz frames per block. */
size_t payload_bytes(PayloadType type, uint8_t channels, size_t frames);

struct Block {
  uint8_t index = 0;
  PayloadType payload = PayloadType::pcm_l24;
  uint8_t channels = k_block_channels;
  /** Interleaved sample data, exactly as it came off the AES67 payload. */
  std::vector<uint8_t> data;
};

struct Frame {
  uint16_t link_id = 0;
  uint64_t sequence = 0;
  /**
   * 48 kHz frames since the stream started — the sync anchor.  Every block in a
   * frame shares it, so blocks are sample-aligned by construction rather than
   * by reconciliation.
   */
  uint64_t sample_position = 0;
  std::vector<Block> blocks;
};

/** CRC-32 (IEEE 802.3), which is what the frame's trailing four bytes are. */
uint32_t checksum(const uint8_t* data, size_t size);

/** The exact byte count encode() will produce for this frame's blocks. */
size_t encoded_size(const Frame& frame);

/**
 * How long the frame at |data| is, reading only its header.
 *
 * A frame arrives across several SRT messages, because live mode caps a single
 * send at 1456 bytes and a full 64-channel frame is 9312
 * (docs/research/libsrt.md). A reassembler therefore has to know how much it is
 * waiting for, and it must be able to say so from a *prefix* — there is always a
 * moment when a message boundary lands mid-header.
 *
 * The knowledge belongs here rather than in the transport, because "how long is
 * a frame" is a property of the format.
 */
enum class LengthStatus {
  /**
   * The length is now known: |total| holds it. Note this is *not* "the whole
   * frame has arrived" — the length becomes knowable as soon as the last block
   * header is readable, which is usually several payloads earlier. Whether the
   * bytes are all present is the reassembler's question, not this one's.
   */
  known,
  /** Not enough bytes yet to say. Wait for more; this is not an error. */
  incomplete,
  /** Not one of our frames, or a version we cannot read. Do not wait for more. */
  invalid
};

LengthStatus frame_length(const uint8_t* data, size_t size, size_t* total,
                          std::string* error);

/**
 * The largest message an SRT live-mode send will carry by default.
 *
 * The documentation says `SRTO_PAYLOADSIZE` "can't be larger than 1456 bytes
 * (1316 default)" (Haivision SRT v1.5.7, `docs/API/API-functions.md:1926`), and
 * the library *means* the default: sending 1456 bytes without first raising the
 * option fails with "payload size: 1456 exceeds maximum allowed 1316". So this is
 * 1316, not 1456, and the transport sets the option to match rather than relying
 * on a default that could move under us.
 *
 * The cost of the smaller message is negligible: eight messages per frame rather
 * than seven, which is 16 extra bytes of SRT header per 9312-byte frame, or
 * 0.2%. See docs/research/libsrt.md.
 */
constexpr size_t k_max_message_bytes = 1316;

/**
 * Bytes of the fragment header every SRT message begins with.
 *
 *   0  magic[2]         'A','F'
 *   2  frame_sequence   uint32 little-endian: which frame this fragment is from
 *   6  index            0-based fragment position within the frame
 *   7  count            fragments in this frame, 1..255
 *
 * It exists because SRT message mode does not promise a gapless stream: a packet
 * that arrives too late is dropped (`TLPKTDROP`), and the message carrying it
 * goes with it. Counting bytes alone cannot see that, so the reassembler read a
 * bogus frame length and collapsed — measured at 64 channels as 277 frames/s and
 * a receive buffer climbing to 993 ms. With a sequence and an index per message,
 * a missing fragment is detectable and only its own frame is lost.
 */
constexpr size_t k_fragment_header_bytes = 8;

/** What a fragment header says. */
struct FragmentHeader {
  uint32_t frame_sequence = 0;
  uint8_t index = 0;
  uint8_t count = 1;
};

/** Write a fragment header into |out| (at least k_fragment_header_bytes). */
void write_fragment_header(uint8_t* out, const FragmentHeader& header);

/** Read a fragment header from |data|. False if it is not one of ours. */
bool read_fragment_header(const uint8_t* data, size_t size, FragmentHeader* header);

/** How many fragments a frame of |size| bytes becomes (0 if it cannot be sent). */
size_t fragment_count(size_t size);

/**
 * Build the next message of a frame: a fragment header then up to the payload cap.
 *
 * Start with `*offset = 0` and call until it returns false. The message is
 * written into |message|, which the caller reuses: at a thousand frames a second,
 * eight fresh vectors per frame is churn for nothing. |frame_sequence| is the
 * frame's own sequence, carried so the receiver can tell frames apart.
 */
bool next_fragment(const uint8_t* frame, size_t size, uint32_t frame_sequence,
                   size_t* offset, std::vector<uint8_t>* message);

/**
 * Reassembles frames from the messages an SRT receive returns.
 *
 * A frame arrives across several messages, each with a fragment header. This owns
 * that accumulation and, unlike a byte counter, it can see a missing message:
 * when a fragment is absent the frame it belonged to is abandoned and counted,
 * and assembly resumes on the next frame's first fragment. It is deliberately
 * free of I/O so it can be tested without a socket: the transport moves bytes,
 * this knows what the bytes mean.
 */
class Reassembler {
 public:
  /** Add one received message. Returns false on a message that is not ours. */
  bool feed(const uint8_t* data, size_t size, std::string* error);

  /** True when a whole frame is waiting to be taken. */
  bool frame_ready() const;

  /** Bytes of the waiting frame, or 0. */
  size_t frame_size() const;

  /** Copy out the waiting frame and reset for the next. */
  bool take_frame(std::vector<uint8_t>* frame, std::string* error);

  /** Bytes buffered so far, waiting for the rest of a frame. */
  size_t buffered() const;

  /** Frames given up because a fragment never arrived. */
  size_t dropped() const;

 private:
  void abandon();

  std::vector<uint8_t> buffer_;
  uint32_t frame_sequence_ = 0;  // the frame being assembled
  uint8_t count_ = 0;            // fragments that frame has (0: none in progress)
  uint8_t next_index_ = 0;       // the fragment we expect next
  bool assembling_ = false;
  bool ready_ = false;
  size_t dropped_ = 0;
};

/**
 * Serialise |frame| into |out|, header first and checksum last.
 *
 * Refuses rather than repairs: a frame that cannot be represented — an
 * unsupported payload type, a payload that is not a whole number of sample
 * frames, more than eight blocks — is an error and produces no bytes at all,
 * so a half-written frame can never reach the socket.
 */
bool encode(const Frame& frame, std::vector<uint8_t>* out, std::string* error);

/**
 * Parse one frame.  |size| must be exactly the frame's length: a frame arrives
 * as one SRT message, so trailing bytes mean the writer and the reader disagree
 * about the format, which is worth failing loudly on.
 */
bool decode(const uint8_t* data, size_t size, Frame* frame, std::string* error);

}  // namespace aes67_srt::wire
