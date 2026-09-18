#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace aes67_srt::codec {

/** Whether this build can encode and decode Opus at all. */
bool opus_available();

/** Why it cannot, when it cannot; empty otherwise. */
const char* opus_unavailable_reason();

/**
 * One 8-channel block's Opus codec.
 *
 * The single-stream API cannot carry eight channels, so this is the **multistream
 * API** with **eight mono streams and no coupling** — the correctness-first
 * mapping, because an AES67 block is eight arbitrary console channels rather than
 * a stereo pair (`docs/research/opus.md`). Coupling is an experiment to be
 * measured against this, not the starting point.
 *
 * PCM on both sides is interleaved signed 24-bit little-endian, which is the wire
 * format's PCM, so the codec sits behind the same seam the payload type already
 * describes. In-band FEC is off: SRT recovers loss by retransmission, and Opus
 * charges for FEC in quality by switching to a speech-optimised mode.
 */
class OpusBlock {
 public:
  OpusBlock();
  ~OpusBlock();
  OpusBlock(const OpusBlock&) = delete;
  OpusBlock& operator=(const OpusBlock&) = delete;

  /**
   * |frame_frames| is the samples-per-channel per Opus frame — 960 for 20 ms at
   * 48 kHz. |bitrate_bps_per_channel| is the fixed target: the project does not
   * adapt bitrate, quality changes only when a human changes it.
   */
  bool open(int channels, int sample_rate, int frame_frames,
            int bitrate_bps_per_channel, std::string* error);
  void close();
  bool is_open() const;

  /** Encode one frame of interleaved s24_3le PCM into |packet|. */
  bool encode(const uint8_t* pcm, size_t frames, std::vector<uint8_t>* packet,
              std::string* error);

  /** Decode |packet| into one frame of interleaved s24_3le PCM. */
  bool decode(const uint8_t* packet, size_t bytes, size_t frames, uint8_t* pcm,
              std::string* error);

  /**
   * The codec's algorithmic delay in milliseconds: `OPUS_GET_LOOKAHEAD`, which
   * is 6.50 ms at 48 kHz (measured; `docs/research/opus.md`). The A/V delay line
   * subtracts the frame duration plus this, and the operator is told it.
   */
  double lookahead_ms() const;

  /** Bytes one encoded frame may occupy — the packet buffer to budget. */
  size_t max_packet_bytes() const;

 private:
  struct Impl;
  Impl* impl_;
};

}  // namespace aes67_srt::codec
