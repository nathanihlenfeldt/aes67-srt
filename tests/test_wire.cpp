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
  // AAC-LC is a payload type this format defines and no build decodes yet. A
  // build must say so, rather than decode it as if it were PCM. (Opus was this
  // test's subject until phase 2 gave it a codec seam; it is carried now, below.)
  std::vector<uint8_t> bytes = encoded(make_frame(1, PayloadType::pcm_l24, 0));
  bytes[block_offset(0, PayloadType::pcm_l24) + 1] = 3;
  fix_checksum(&bytes);
  const std::string error = decode_error(bytes);
  CHECK(contains(error, "aac-lc"));
  CHECK(contains(error, "not supported by this build"));
}

TEST_CASE(wire_carries_an_opus_block_as_opaque_bytes) {
  // The format does not decode Opus, it *carries* it: the block header's length
  // is the packet's length, and the bytes come back exactly. The codec seam
  // decides what they mean.
  Frame frame = make_frame(1, PayloadType::pcm_l24, 0);
  frame.blocks[0].payload = PayloadType::opus;
  frame.blocks[0].data.assign(137, 0x5a);  // a stand-in packet, not PCM-sized

  std::vector<uint8_t> out;
  std::string error;
  CHECK(aes67_srt::wire::encode(frame, &out, &error));

  Frame parsed;
  CHECK(aes67_srt::wire::decode(out.data(), out.size(), &parsed, &error));
  CHECK_EQ(parsed.blocks.size(), static_cast<size_t>(1));
  CHECK(parsed.blocks[0].payload == PayloadType::opus);
  CHECK(parsed.blocks[0].data == frame.blocks[0].data);
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
  codec.blocks[0].payload = PayloadType::aac_lc;
  error.clear();
  CHECK(!aes67_srt::wire::encode(codec, &out, &error));
  CHECK(contains(error, "not supported by this build"));

  // Nothing was written, on any of those paths: a half-written frame must never
  // reach the socket.
  CHECK_EQ(out.size(), static_cast<size_t>(3));
  CHECK_EQ(out[0], static_cast<uint8_t>(0xaa));
}

// ---------------------------------------------------------------------------
// frame_length: what a reassembler needs, because a frame arrives across several
// SRT messages and a message boundary can land anywhere.
// ---------------------------------------------------------------------------

namespace {

using aes67_srt::wire::LengthStatus;

LengthStatus length_of(const std::vector<uint8_t>& bytes, size_t length,
                       size_t* total) {
  std::string error;
  return aes67_srt::wire::frame_length(bytes.data(), length, total, &error);
}

/** A prefix must ask for more bytes, never report a failure. */
void check_incomplete(const std::vector<uint8_t>& bytes, size_t length) {
  size_t total = 0;
  if (length_of(bytes, length, &total) != LengthStatus::incomplete) {
    test::report_failure("prefix should be incomplete", __FILE__, __LINE__,
                         "length " + std::to_string(length));
  }
}

}  // namespace

TEST_CASE(wire_measures_a_frame_from_its_header_alone) {
  const std::vector<uint8_t> bytes =
      encoded(make_frame(8, PayloadType::pcm_l24, 0));
  size_t total = 0;
  CHECK(length_of(bytes, bytes.size(), &total) == LengthStatus::known);
  CHECK_EQ(total, static_cast<size_t>(9312));

  const std::vector<uint8_t> single =
      encoded(make_frame(1, PayloadType::pcm_l16, 0));
  CHECK(length_of(single, single.size(), &total) == LengthStatus::known);
  CHECK_EQ(total, static_cast<size_t>(808));
}

TEST_CASE(wire_knows_a_frame_length_before_all_of_it_has_arrived) {
  // Two L24 blocks: 28 header + (8 + 1152) + (8 + 1152) + 4 checksum.
  const std::vector<uint8_t> bytes =
      encoded(make_frame(2, PayloadType::pcm_l24, 0));
  const size_t last_block_header_ends = 28 + (8 + 1152) + 8;

  // Before the last block header is readable, the length cannot be known.
  for (size_t length = 1; length < last_block_header_ends; ++length) {
    check_incomplete(bytes, length);
  }

  // From that byte onwards the length is known, even though most of the frame is
  // still in flight. This is what lets the transport know how much it is waiting
  // for instead of buffering blind until a message happens to arrive.
  size_t total = 0;
  CHECK(length_of(bytes, last_block_header_ends, &total) == LengthStatus::known);
  CHECK_EQ(total, bytes.size());
  CHECK(total > last_block_header_ends);  // i.e. the payloads are still to come
}

TEST_CASE(wire_never_measures_a_prefix_shorter_than_a_header) {
  const std::vector<uint8_t> bytes =
      encoded(make_frame(1, PayloadType::pcm_l24, 0));
  // One byte short of a block header is the interesting case: the length of the
  // payload that follows is not readable yet.
  check_incomplete(bytes, aes67_srt::wire::k_frame_header_bytes + 7);
  check_incomplete(bytes, aes67_srt::wire::k_frame_header_bytes + 1);
  check_incomplete(bytes, aes67_srt::wire::k_frame_header_bytes);
  check_incomplete(bytes, 5);
  check_incomplete(bytes, 3);
  check_incomplete(bytes, 1);
}

TEST_CASE(wire_refuses_to_measure_a_stream_that_is_not_ours) {
  size_t total = 0;
  std::vector<uint8_t> bytes = encoded(make_frame(1, PayloadType::pcm_l24, 0));

  std::vector<uint8_t> alien = bytes;
  alien[0] = 'X';
  CHECK(length_of(alien, alien.size(), &total) == LengthStatus::invalid);

  std::vector<uint8_t> future = bytes;
  future[5] = 9;  // a version this build cannot read
  CHECK(length_of(future, future.size(), &total) == LengthStatus::invalid);

  std::vector<uint8_t> no_blocks = bytes;
  no_blocks[10] = 0;
  CHECK(length_of(no_blocks, no_blocks.size(), &total) == LengthStatus::invalid);

  std::vector<uint8_t> too_many_blocks = bytes;
  too_many_blocks[10] = 9;
  CHECK(length_of(too_many_blocks, too_many_blocks.size(), &total) ==
        LengthStatus::invalid);
}

TEST_CASE(wire_gives_up_on_an_absurd_length_rather_than_buffering_forever) {
  // A corrupted payload length on a stream that has plausible framing would
  // otherwise have the reassembler waiting for gigabytes that will never come.
  std::vector<uint8_t> bytes = encoded(make_frame(1, PayloadType::pcm_l24, 0));
  const size_t length_field =
      aes67_srt::wire::k_frame_header_bytes + 4;  // the block's payload_bytes
  bytes[length_field + 0] = 0x00;
  bytes[length_field + 1] = 0xFF;
  bytes[length_field + 2] = 0xFF;
  bytes[length_field + 3] = 0xFF;

  size_t total = 0;
  std::string error;
  CHECK(aes67_srt::wire::frame_length(bytes.data(), bytes.size(), &total, &error) ==
        LengthStatus::invalid);
  CHECK(contains(error, "sanity ceiling"));
}

// ---------------------------------------------------------------------------
// Fragmenting and reassembling: a frame is bigger than an SRT message, so this
// is where the two halves of that problem are proved — without a socket, which
// is exactly why it lives here rather than in the transport.
// ---------------------------------------------------------------------------

namespace {

/** Every message a frame is sent as, headers and all. */
std::vector<std::vector<uint8_t>> fragments_of(const std::vector<uint8_t>& frame,
                                               uint32_t sequence = 0) {
  std::vector<std::vector<uint8_t>> messages;
  size_t offset = 0;
  std::vector<uint8_t> message;
  while (aes67_srt::wire::next_fragment(frame.data(), frame.size(), sequence,
                                        &offset, &message)) {
    messages.push_back(message);
  }
  return messages;
}

}  // namespace

TEST_CASE(wire_slices_a_frame_into_messages_srt_will_accept) {
  const std::vector<uint8_t> bytes =
      encoded(make_frame(8, PayloadType::pcm_l24, 0));
  const std::vector<std::vector<uint8_t>> messages = fragments_of(bytes, 7);

  // 9312 bytes at 1308 payload bytes per message (1316 less the header) is still
  // eight messages.
  CHECK_EQ(messages.size(), static_cast<size_t>(8));

  std::vector<uint8_t> joined;
  for (size_t index = 0; index < messages.size(); ++index) {
    CHECK(messages[index].size() > 0);
    CHECK(messages[index].size() <= aes67_srt::wire::k_max_message_bytes);
    // Every message starts with a fragment header naming its frame and its place
    // in it, which is what lets the far end see a message go missing.
    aes67_srt::wire::FragmentHeader header;
    CHECK(aes67_srt::wire::read_fragment_header(messages[index].data(),
                                                messages[index].size(), &header));
    CHECK_EQ(header.frame_sequence, static_cast<uint32_t>(7));
    CHECK_EQ(static_cast<int>(header.index), static_cast<int>(index));
    CHECK_EQ(static_cast<int>(header.count), static_cast<int>(messages.size()));
    joined.insert(
        joined.end(),
        messages[index].begin() + aes67_srt::wire::k_fragment_header_bytes,
        messages[index].end());
  }
  // Strip the headers and the frame is reproduced exactly.
  CHECK(joined == bytes);
}

TEST_CASE(wire_reassembles_every_fragmented_frame_byte_for_byte) {
  for (size_t blocks = 1; blocks <= 8; ++blocks) {
    const std::vector<uint8_t> bytes =
        encoded(make_frame(blocks, PayloadType::pcm_l24, 48000 + blocks));
    aes67_srt::wire::Reassembler reassembler;
    std::string error;
    for (const std::vector<uint8_t>& message : fragments_of(bytes)) {
      CHECK(reassembler.feed(message.data(), message.size(), &error));
    }
    CHECK(reassembler.frame_ready());
    CHECK_EQ(reassembler.frame_size(), bytes.size());

    std::vector<uint8_t> taken;
    CHECK(reassembler.take_frame(&taken, &error));
    CHECK(taken == bytes);
    CHECK(!reassembler.frame_ready());
    CHECK_EQ(reassembler.buffered(), static_cast<size_t>(0));
  }
}

TEST_CASE(wire_reassembly_waits_for_the_last_fragment) {
  const std::vector<uint8_t> bytes =
      encoded(make_frame(8, PayloadType::pcm_l24, 0));
  const std::vector<std::vector<uint8_t>> messages = fragments_of(bytes);

  aes67_srt::wire::Reassembler reassembler;
  std::string error;
  for (size_t index = 0; index + 1 < messages.size(); ++index) {
    CHECK(reassembler.feed(messages[index].data(), messages[index].size(), &error));
    // Most of the frame is buffered and none of it is usable: a partial frame is
    // not a quiet frame, it is not a frame.
    CHECK(!reassembler.frame_ready());
    CHECK_EQ(reassembler.frame_size(), static_cast<size_t>(0));
  }
  CHECK(reassembler.feed(messages.back().data(), messages.back().size(), &error));
  CHECK(reassembler.frame_ready());
}

TEST_CASE(wire_a_lost_fragment_loses_only_its_own_frame) {
  const std::vector<uint8_t> first =
      encoded(make_frame(8, PayloadType::pcm_l24, 11));
  const std::vector<uint8_t> second =
      encoded(make_frame(1, PayloadType::pcm_l16, 22));

  // What TLPKTDROP does on a lossy link: the message carrying one fragment never
  // arrives. Counting bytes cannot see that; the fragment header can.
  const std::vector<std::vector<uint8_t>> messages = fragments_of(first, 1);
  CHECK_EQ(messages.size(), static_cast<size_t>(8));

  aes67_srt::wire::Reassembler reassembler;
  std::string error;
  for (size_t index = 0; index < messages.size(); ++index) {
    if (index == 3) {
      continue;  // dropped
    }
    CHECK(reassembler.feed(messages[index].data(), messages[index].size(), &error));
  }
  CHECK(!reassembler.frame_ready());
  CHECK_EQ(reassembler.dropped(), static_cast<size_t>(1));

  // The next frame is untouched by the loss and comes out whole.
  for (const std::vector<uint8_t>& message : fragments_of(second, 2)) {
    CHECK(reassembler.feed(message.data(), message.size(), &error));
  }
  CHECK(reassembler.frame_ready());
  std::vector<uint8_t> taken;
  CHECK(reassembler.take_frame(&taken, &error));
  CHECK(taken == second);
}

TEST_CASE(wire_a_lost_fragment_at_the_end_of_a_frame_is_noticed) {
  const std::vector<uint8_t> first =
      encoded(make_frame(8, PayloadType::pcm_l24, 11));
  const std::vector<uint8_t> second =
      encoded(make_frame(8, PayloadType::pcm_l24, 22));

  const std::vector<std::vector<uint8_t>> first_messages = fragments_of(first, 1);
  aes67_srt::wire::Reassembler reassembler;
  std::string error;
  for (size_t index = 0; index + 1 < first_messages.size(); ++index) {
    CHECK(reassembler.feed(first_messages[index].data(),
                           first_messages[index].size(), &error));
  }
  CHECK(!reassembler.frame_ready());

  // The last fragment never arrives; the next frame's first fragment is the
  // signal that the old frame is dead.
  for (const std::vector<uint8_t>& message : fragments_of(second, 2)) {
    CHECK(reassembler.feed(message.data(), message.size(), &error));
  }
  CHECK_EQ(reassembler.dropped(), static_cast<size_t>(1));
  CHECK(reassembler.frame_ready());
  std::vector<uint8_t> taken;
  CHECK(reassembler.take_frame(&taken, &error));
  CHECK(taken == second);
}

TEST_CASE(wire_reassembly_gives_up_on_a_stream_that_is_not_ours) {
  aes67_srt::wire::Reassembler reassembler;
  std::string error;
  const std::vector<uint8_t> garbage(200, 0x5a);
  CHECK(!reassembler.feed(garbage.data(), garbage.size(), &error));
  CHECK(contains(error, "fragment"));
  // The desync is dropped rather than carried into every later frame.
  CHECK_EQ(reassembler.buffered(), static_cast<size_t>(0));
  CHECK(!reassembler.frame_ready());
}
