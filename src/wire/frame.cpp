#include "wire/frame.hpp"

#include <array>
#include <cstring>

namespace aes67_srt::wire {
namespace {

void write_u16(uint8_t* out, uint16_t value) {
  out[0] = static_cast<uint8_t>((value >> 8) & 0xff);
  out[1] = static_cast<uint8_t>(value & 0xff);
}

void write_u32(uint8_t* out, uint32_t value) {
  out[0] = static_cast<uint8_t>((value >> 24) & 0xff);
  out[1] = static_cast<uint8_t>((value >> 16) & 0xff);
  out[2] = static_cast<uint8_t>((value >> 8) & 0xff);
  out[3] = static_cast<uint8_t>(value & 0xff);
}

void write_u64(uint8_t* out, uint64_t value) {
  for (int index = 7; index >= 0; --index) {
    out[7 - index] = static_cast<uint8_t>((value >> (index * 8)) & 0xff);
  }
}

uint16_t read_u16(const uint8_t* in) {
  return static_cast<uint16_t>((static_cast<uint16_t>(in[0]) << 8) | in[1]);
}

uint32_t read_u32(const uint8_t* in) {
  return (static_cast<uint32_t>(in[0]) << 24) |
         (static_cast<uint32_t>(in[1]) << 16) |
         (static_cast<uint32_t>(in[2]) << 8) | static_cast<uint32_t>(in[3]);
}

uint64_t read_u64(const uint8_t* in) {
  uint64_t value = 0;
  for (int index = 0; index < 8; ++index) {
    value = (value << 8) | in[index];
  }
  return value;
}

bool fail(std::string* error, const std::string& message) {
  if (error != nullptr) {
    *error = message;
  }
  return false;
}

std::string count(size_t value) {
  return std::to_string(value);
}

}  // namespace

const char* to_string(PayloadType type) {
  switch (type) {
    case PayloadType::pcm_l24:
      return "L24";
    case PayloadType::pcm_l16:
      return "L16";
    case PayloadType::opus:
      return "opus";
    case PayloadType::aac_lc:
      return "aac-lc";
  }
  return "unknown";
}

bool parse_payload_type(uint8_t raw, PayloadType* type) {
  if (raw > static_cast<uint8_t>(PayloadType::aac_lc)) {
    return false;
  }
  *type = static_cast<PayloadType>(raw);
  return true;
}

bool supported_here(PayloadType type) {
  return type == PayloadType::pcm_l24 || type == PayloadType::pcm_l16;
}

size_t sample_bytes(PayloadType type) {
  switch (type) {
    case PayloadType::pcm_l24:
      return 3;
    case PayloadType::pcm_l16:
      return 2;
    default:
      return 0;
  }
}

size_t payload_bytes(PayloadType type, uint8_t channels, size_t frames) {
  return frames * static_cast<size_t>(channels) * sample_bytes(type);
}

uint32_t checksum(const uint8_t* data, size_t size) {
  // CRC-32 (IEEE 802.3), table built once.  Table-driven because the bitwise
  // form would be ~8 operations per bit, and this runs over 9.3 KB per frame a
  // thousand times a second on a Pi.
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> built{};
    for (uint32_t index = 0; index < 256; ++index) {
      uint32_t value = index;
      for (int bit = 0; bit < 8; ++bit) {
        value = (value & 1u) ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
      }
      built[index] = value;
    }
    return built;
  }();

  uint32_t crc = 0xFFFFFFFFu;
  for (size_t index = 0; index < size; ++index) {
    crc = table[(crc ^ data[index]) & 0xFFu] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

size_t encoded_size(const Frame& frame) {
  size_t size = k_frame_header_bytes + k_checksum_bytes;
  for (const Block& block : frame.blocks) {
    size += k_block_header_bytes + block.data.size();
  }
  return size;
}

bool encode(const Frame& frame, std::vector<uint8_t>* out, std::string* error) {
  if (out == nullptr) {
    return fail(error, "no output buffer");
  }

  // Everything is validated before a single byte is written.  A half-written
  // frame must never reach the socket, so this either produces a whole frame or
  // produces none.
  if (frame.blocks.empty()) {
    return fail(error, "blocks: a frame carries at least one block");
  }
  if (frame.blocks.size() > k_max_blocks) {
    return fail(error, "blocks: " + count(frame.blocks.size()) +
                           " exceeds the format's maximum of " +
                           count(k_max_blocks));
  }

  std::array<int, k_max_blocks> index_seen{};
  for (const Block& block : frame.blocks) {
    const std::string path = "blocks[" + std::to_string(block.index) + "].";
    if (block.index >= k_max_blocks) {
      return fail(error, path + "index: expected 0.." + count(k_max_blocks - 1) +
                             ", got " + count(block.index));
    }
    if (index_seen[block.index]++ != 0) {
      return fail(error,
                  path + "index: duplicate block index " + count(block.index));
    }
    if (!supported_here(block.payload)) {
      return fail(error,
                  path + "payload_type: " + std::string(to_string(block.payload)) +
                      " is not supported by this build");
    }
    if (block.channels != k_block_channels) {
      return fail(error, path + "channels: expected " + count(k_block_channels) +
                             ", the AES67 stream size; got " +
                             count(block.channels));
    }
    const size_t stride =
        static_cast<size_t>(block.channels) * sample_bytes(block.payload);
    if (block.data.empty() || block.data.size() % stride != 0) {
      return fail(error, path + "data: " + count(block.data.size()) +
                             " bytes is not a whole number of " +
                             count(block.channels) + "-channel frames");
    }
    if (block.data.size() > 0xFFFFFFFFu) {
      return fail(error, path + "data: too large for the format's 32-bit length");
    }
  }

  std::vector<uint8_t> buffer(encoded_size(frame));
  uint8_t* cursor = buffer.data();

  // Header first, in the order the format describes.
  write_u32(cursor, k_magic);
  cursor += 4;
  write_u16(cursor, k_version);
  cursor += 2;
  write_u16(cursor, 0);  // flags, reserved
  cursor += 2;
  write_u16(cursor, frame.link_id);
  cursor += 2;
  *cursor++ = static_cast<uint8_t>(frame.blocks.size());
  *cursor++ = 0;  // reserved
  write_u64(cursor, frame.sequence);
  cursor += 8;
  write_u64(cursor, frame.sample_position);
  cursor += 8;

  for (const Block& block : frame.blocks) {
    *cursor++ = block.index;
    *cursor++ = static_cast<uint8_t>(block.payload);
    *cursor++ = block.channels;
    *cursor++ = 0;  // reserved
    write_u32(cursor, static_cast<uint32_t>(block.data.size()));
    cursor += 4;
    std::memcpy(cursor, block.data.data(), block.data.size());
    cursor += block.data.size();
  }

  // Checksum last, over everything before it.
  write_u32(cursor, checksum(buffer.data(), buffer.size() - k_checksum_bytes));

  *out = std::move(buffer);
  return true;
}

bool decode(const uint8_t* data, size_t size, Frame* frame, std::string* error) {
  if (data == nullptr || frame == nullptr) {
    return fail(error, "no frame to fill");
  }
  if (size < k_frame_header_bytes + k_checksum_bytes) {
    return fail(error,
                "frame: " + count(size) + " bytes is shorter than a frame header");
  }

  // Identify before interpreting: a desynchronised stream should be reported as
  // such, not decoded into plausible-looking audio.
  if (read_u32(data) != k_magic) {
    return fail(error, "magic: not an aes67-srt frame (expected \"A67S\")");
  }
  const uint16_t version = read_u16(data + 4);
  if (version != k_version) {
    return fail(error, "version: expected " + count(k_version) + ", got " +
                           count(version) +
                           " - this build cannot read that format");
  }
  const uint16_t flags = read_u16(data + 6);
  if (flags != 0) {
    return fail(error, "flags: reserved bits are set (" + count(flags) + ")");
  }

  // Integrity before structure: a corrupted block length that happens to fall
  // inside the buffer would otherwise be believed.
  const uint32_t stored = read_u32(data + size - k_checksum_bytes);
  const uint32_t computed = checksum(data, size - k_checksum_bytes);
  if (stored != computed) {
    return fail(error, "checksum: the frame is corrupted (stored " + count(stored) +
                           ", computed " + count(computed) + ")");
  }

  const uint8_t block_count = data[10];
  if (block_count == 0 || block_count > k_max_blocks) {
    return fail(error, "block_count: expected 1.." + count(k_max_blocks) +
                           ", got " + count(block_count));
  }
  if (data[11] != 0) {
    return fail(error, "reserved: expected 0");
  }

  Frame parsed;
  parsed.link_id = read_u16(data + 8);
  parsed.sequence = read_u64(data + 12);
  parsed.sample_position = read_u64(data + 20);

  const size_t payload_end = size - k_checksum_bytes;
  size_t offset = k_frame_header_bytes;
  std::array<int, k_max_blocks> index_seen{};

  for (uint8_t position = 0; position < block_count; ++position) {
    if (offset + k_block_header_bytes > payload_end) {
      return fail(error, "blocks: the frame ends inside a block header");
    }
    Block block;
    block.index = data[offset];
    const std::string path = "blocks[" + count(block.index) + "].";

    const uint8_t raw_type = data[offset + 1];
    if (!parse_payload_type(raw_type, &block.payload)) {
      return fail(error,
                  path + "payload_type: unknown payload type " + count(raw_type));
    }
    // Recognised but not decodable here is a different problem from corrupt, and
    // saying so is the difference between "upgrade the appliance" and "find the
    // broken cable".
    if (!supported_here(block.payload)) {
      return fail(error, path + "payload_type: " + to_string(block.payload) +
                             " is not supported by this build");
    }
    block.channels = data[offset + 2];
    if (data[offset + 3] != 0) {
      return fail(error, path + "reserved: expected 0");
    }
    const uint32_t bytes = read_u32(data + offset + 4);
    offset += k_block_header_bytes;

    if (block.index >= k_max_blocks) {
      return fail(error, path + "index: expected 0.." + count(k_max_blocks - 1) +
                             ", got " + count(block.index));
    }
    if (index_seen[block.index]++ != 0) {
      return fail(error,
                  path + "index: duplicate block index " + count(block.index));
    }
    if (block.channels != k_block_channels) {
      return fail(error, path + "channels: expected " + count(k_block_channels) +
                             ", got " + count(block.channels));
    }
    const size_t stride =
        static_cast<size_t>(block.channels) * sample_bytes(block.payload);
    if (bytes == 0 || bytes % stride != 0) {
      return fail(error, path + "data: " + count(bytes) +
                             " bytes is not a whole number of " +
                             count(block.channels) + "-channel frames");
    }
    if (offset + bytes > payload_end) {
      return fail(error, path + "data: " + count(bytes) +
                             " bytes runs past the end of the frame");
    }

    block.data.assign(data + offset, data + offset + bytes);
    offset += bytes;
    parsed.blocks.push_back(std::move(block));
  }

  if (offset != payload_end) {
    return fail(error, "frame: " + count(payload_end - offset) +
                           " trailing bytes after the last block - the writer "
                           "and the reader disagree about the format");
  }

  *frame = std::move(parsed);
  return true;
}

}  // namespace aes67_srt::wire
