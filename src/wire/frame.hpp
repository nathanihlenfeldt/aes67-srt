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
 * The largest message an SRT live-mode send will carry.
 *
 * Live mode caps a single send at `SRTO_PAYLOADSIZE`, which "can't be larger than
 * 1456 bytes (1316 default)" (Haivision SRT v1.5.7,
 * `docs/API/API-functions.md:1926`). See docs/research/libsrt.md.
 */
constexpr size_t k_max_message_bytes = 1456;

/**
 * Slice a frame into messages of at most k_max_message_bytes, without allocating.
 *
 * Start with `*offset = 0` and call until it returns false. The slices are views
 * into the caller's buffer: at a thousand frames a second, seven fresh vectors
 * per frame is churn for nothing.
 */
bool next_fragment(const uint8_t* frame, size_t size, size_t* offset,
                   const uint8_t** chunk, size_t* chunk_size);

/**
 * Reassembles frames from the messages an SRT receive returns.
 *
 * A frame arrives across several messages, and the boundaries fall wherever they
 * fall — mid-header, mid-payload. This owns that accumulation, and it is
 * deliberately free of I/O so it can be tested without a socket: the transport
 * moves bytes, this knows what the bytes mean.
 */
class Reassembler {
 public:
  /** Add one received message. Returns false on a stream that is not ours. */
  bool feed(const uint8_t* data, size_t size, std::string* error);

  /** True when a whole frame is waiting to be taken. */
  bool frame_ready() const;

  /** Bytes of the waiting frame, or 0. */
  size_t frame_size() const;

  /** Copy out the waiting frame and reset for the next. */
  bool take_frame(std::vector<uint8_t>* frame, std::string* error);

  /** Bytes buffered so far, waiting for the rest of a frame. */
  size_t buffered() const;

 private:
  std::vector<uint8_t> buffer_;
  size_t expected_ = 0;  // 0 until the header has told us how long this frame is
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
