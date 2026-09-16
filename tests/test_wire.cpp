#include "wire/frame.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include "test_framework.hpp"

namespace {

using aes67_srt::wire::Block;
using aes67_srt::wire::Frame;
using aes67_srt::wire::PayloadType;

/** One millisecond at 48 kHz: the frame rate the link is designed around. */
constexpr size_t k_frames = 48;

std::vector<uint8_t> make_payload(PayloadType type, size_t frames, uint8_t seed) {
  const size_t bytes = aes67_srt::wire::payload_bytes(type, 8, frames);
  std::vector<uint8_t> data(bytes);
  for (size_t index = 0; index < bytes; ++index) {
    data[index] = static_cast<uint8_t>((index + seed) & 0xff);
  }
  return data;
}

Frame make_frame(size_t block_count, PayloadType type, uint64_t sample_position) {
  Frame frame;
  frame.link_id = 1;
  frame.sequence = 42;
  frame.sample_position = sample_position;
  for (size_t index = 0; index < block_count; ++index) {
    Block block;
    block.index = static_cast<uint8_t>(index);
    block.payload = type;
    block.channels = 8;
    block.data = make_payload(type, k_frames, static_cast<uint8_t>(index));
    frame.blocks.push_back(std::move(block));
  }
  return frame;
}

std::vector<uint8_t> encoded(const Frame& frame) {
  std::vector<uint8_t> bytes;
  std::string error;
  CHECK(aes67_srt::wire::encode(frame, &bytes, &error));
  return bytes;
}

/** Rewrite the trailing checksum, so a deliberately damaged field still gets past
 * it. */
void fix_checksum(std::vector<uint8_t>* bytes) {
  const size_t end = bytes->size() - aes67_srt::wire::k_checksum_bytes;
  const uint32_t crc = aes67_srt::wire::checksum(bytes->data(), end);
  for (int index = 0; index < 4; ++index) {
    (*bytes)[end + index] = static_cast<uint8_t>((crc >> (24 - index * 8)) & 0xff);
  }
}

/** Decode expecting refusal, and return the reason. */
std::string decode_error(const std::vector<uint8_t>& bytes) {
  Frame frame;
  std::string error;
  CHECK(!aes67_srt::wire::decode(bytes.data(), bytes.size(), &frame, &error));
  return error;
}

/** Byte offset of a block's header within the frame. */
size_t block_offset(size_t position, PayloadType type) {
  return aes67_srt::wire::k_frame_header_bytes +
         position * (aes67_srt::wire::k_block_header_bytes +
                     aes67_srt::wire::payload_bytes(type, 8, k_frames));
}

bool contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE(wire_round_trips_every_block_count_and_pcm_type) {
  const PayloadType types[] = {PayloadType::pcm_l24, PayloadType::pcm_l16};
  for (size_t blocks = 1; blocks <= aes67_srt::wire::k_max_blocks; ++blocks) {
    for (PayloadType type : types) {
      const Frame frame = make_frame(blocks, type, 48000 * 60 + 7);
      const std::vector<uint8_t> bytes = encoded(frame);
      CHECK_EQ(bytes.size(), aes67_srt::wire::encoded_size(frame));

      Frame decoded;
      std::string error;
      CHECK(aes67_srt::wire::decode(bytes.data(), bytes.size(), &decoded, &error));
      CHECK_EQ(decoded.blocks.size(), blocks);
      CHECK_EQ(decoded.link_id, frame.link_id);
      CHECK_EQ(decoded.sequence, frame.sequence);
      CHECK_EQ(decoded.sample_position, frame.sample_position);
      CHECK(decoded.blocks.front().payload == type);
      CHECK_EQ(decoded.blocks.front().data, frame.blocks.front().data);
      CHECK_EQ(decoded.blocks.back().data, frame.blocks.back().data);
      CHECK_EQ(decoded.blocks.back().index, frame.blocks.back().index);
    }
  }
}

TEST_CASE(wire_sizes_are_exact_for_the_specified_shape) {
  // 64 channels at one millisecond per frame is the shape the link is sized for.
  CHECK_EQ(aes67_srt::wire::payload_bytes(PayloadType::pcm_l24, 8, k_frames),
           static_cast<size_t>(1152));

  const std::vector<uint8_t> bytes =
      encoded(make_frame(8, PayloadType::pcm_l24, 0));
  // 28 header + 8 x (8 block header + 1152 payload) + 4 checksum.
  CHECK_EQ(bytes.size(), static_cast<size_t>(9312));

  // 9312 bytes a thousand times a second is the ~74 Mbit/s the specification
  // quotes for one direction.  If this ever drifts, the bandwidth budget in the
  // spec has been invalidated by a format change.
  const size_t bits_per_second = bytes.size() * 1000 * 8;
  CHECK_EQ(bits_per_second / 1000000, static_cast<size_t>(74));

  const std::vector<uint8_t> single_l16 =
      encoded(make_frame(1, PayloadType::pcm_l16, 0));
  // 28 header + (8 block header + 8 x 48 x 2 payload) + 4 checksum.
  CHECK_EQ(single_l16.size(), static_cast<size_t>(808));

  const std::vector<uint8_t> l16_link =
      encoded(make_frame(8, PayloadType::pcm_l16, 0));
  CHECK_EQ(l16_link.size(), static_cast<size_t>(6240));
}

TEST_CASE(wire_writes_the_header_first_and_the_checksum_last) {
  const std::vector<uint8_t> bytes =
      encoded(make_frame(2, PayloadType::pcm_l24, 12345));

  // Identity at the very front, so a desynchronised stream is caught first.
  CHECK_EQ(bytes[0], 'A');
  CHECK_EQ(bytes[1], '6');
  CHECK_EQ(bytes[2], '7');
  CHECK_EQ(bytes[3], 'S');
  CHECK_EQ(bytes[5], 1);   // version
  CHECK_EQ(bytes[10], 2);  // block count

  // The sample position lives in the header, before any payload byte: that is
  // what makes every block in the frame share one position.
  const uint64_t position = 12345;
  for (int index = 0; index < 8; ++index) {
    CHECK_EQ(bytes[20 + index],
             static_cast<uint8_t>((position >> ((7 - index) * 8)) & 0xff));
  }

  // The first payload byte sits exactly after the frame header and the first
  // block header, and it is the payload's own first byte.
  const size_t first_payload =
      aes67_srt::wire::k_frame_header_bytes + aes67_srt::wire::k_block_header_bytes;
  CHECK_EQ(bytes[first_payload], 0);
  CHECK_EQ(bytes[first_payload + 1], 1);

  // Checksum last, over everything before it.
  const size_t end = bytes.size() - aes67_srt::wire::k_checksum_bytes;
  const uint32_t crc = aes67_srt::wire::checksum(bytes.data(), end);
  for (int index = 0; index < 4; ++index) {
    CHECK_EQ(bytes[end + index],
             static_cast<uint8_t>((crc >> (24 - index * 8)) & 0xff));
  }
}

TEST_CASE(wire_refuses_a_corrupted_payload) {
  std::vector<uint8_t> bytes = encoded(make_frame(2, PayloadType::pcm_l24, 0));
  const size_t first_payload =
      aes67_srt::wire::k_frame_header_bytes + aes67_srt::wire::k_block_header_bytes;
  bytes[first_payload + 5] ^= 0x40;
  CHECK(contains(decode_error(bytes), "checksum"));
}

TEST_CASE(wire_refuses_a_buffer_shorter_than_a_header) {
  const std::vector<uint8_t> tiny(10, 0);
  CHECK(contains(decode_error(tiny), "shorter than a frame header"));
}

TEST_CASE(wire_refuses_a_truncated_frame) {
  std::vector<uint8_t> bytes = encoded(make_frame(1, PayloadType::pcm_l24, 0));
  bytes.resize(bytes.size() - 8);
  CHECK(!decode_error(bytes).empty());
}

TEST_CASE(wire_refuses_an_unknown_version) {
  std::vector<uint8_t> bytes = encoded(make_frame(1, PayloadType::pcm_l24, 0));
  bytes[5] = 2;  // a format this build cannot read
  fix_checksum(&bytes);
  CHECK(contains(decode_error(bytes), "version: expected 1, got 2"));
}

TEST_CASE(wire_refuses_reserved_flag_bits) {
  std::vector<uint8_t> bytes = encoded(make_frame(1, PayloadType::pcm_l24, 0));
  bytes[7] = 1;
  fix_checksum(&bytes);
  CHECK(contains(decode_error(bytes), "reserved bits are set"));
}

TEST_CASE(wire_refuses_an_unknown_payload_type) {
  std::vector<uint8_t> bytes = encoded(make_frame(1, PayloadType::pcm_l24, 0));
  bytes[block_offset(0, PayloadType::pcm_l24) + 1] = 9;
  fix_checksum(&bytes);
  CHECK(contains(decode_error(bytes), "unknown payload type 9"));
}

TEST_CASE(wire_refuses_a_payload_type_this_build_cannot_decode) {
  // Opus is a payload type this format defines, and phase 2 will use.  A PCM
  // build must say so, rather than decode it as if it were PCM.
  std::vector<uint8_t> bytes = encoded(make_frame(1, PayloadType::pcm_l24, 0));
  bytes[block_offset(0, PayloadType::pcm_l24) + 1] = 2;
  fix_checksum(&bytes);
  const std::string error = decode_error(bytes);
  CHECK(contains(error, "opus"));
  CHECK(contains(error, "not supported by this build"));
}

TEST_CASE(wire_refuses_a_duplicate_block_index) {
  std::vector<uint8_t> bytes = encoded(make_frame(2, PayloadType::pcm_l24, 0));
  bytes[block_offset(1, PayloadType::pcm_l24)] = 0;  // also block 0
  fix_checksum(&bytes);
  CHECK(contains(decode_error(bytes), "duplicate block index"));
}

TEST_CASE(wire_refuses_trailing_bytes_after_the_last_block) {
  std::vector<uint8_t> bytes = encoded(make_frame(1, PayloadType::pcm_l24, 0));
  bytes.insert(bytes.end() - aes67_srt::wire::k_checksum_bytes, 4, 0);
  fix_checksum(&bytes);
  CHECK(contains(decode_error(bytes), "trailing bytes after the last block"));
}

TEST_CASE(wire_refuses_a_frame_it_cannot_encode_and_writes_nothing) {
  std::vector<uint8_t> out = {0xaa, 0xbb, 0xcc};
  std::string error;

  Frame empty;
  CHECK(!aes67_srt::wire::encode(empty, &out, &error));
  CHECK(contains(error, "at least one block"));

  Frame too_many = make_frame(8, PayloadType::pcm_l24, 0);
  too_many.blocks.push_back(too_many.blocks.front());
  error.clear();
  CHECK(!aes67_srt::wire::encode(too_many, &out, &error));
  CHECK(contains(error, "exceeds the format's maximum"));

  Frame wrong_channels = make_frame(1, PayloadType::pcm_l24, 0);
  wrong_channels.blocks[0].channels = 7;
  error.clear();
  CHECK(!aes67_srt::wire::encode(wrong_channels, &out, &error));
  CHECK(contains(error, "expected 8"));

  Frame partial_frame = make_frame(1, PayloadType::pcm_l24, 0);
  partial_frame.blocks[0].data.resize(1151);
  error.clear();
  CHECK(!aes67_srt::wire::encode(partial_frame, &out, &error));
  CHECK(contains(error, "whole number of"));

  Frame codec = make_frame(1, PayloadType::pcm_l24, 0);
  codec.blocks[0].payload = PayloadType::opus;
  error.clear();
  CHECK(!aes67_srt::wire::encode(codec, &out, &error));
  CHECK(contains(error, "not supported by this build"));

  // Nothing was written, on any of those paths: a half-written frame must never
  // reach the socket.
  CHECK_EQ(out.size(), static_cast<size_t>(3));
  CHECK_EQ(out[0], static_cast<uint8_t>(0xaa));
}
