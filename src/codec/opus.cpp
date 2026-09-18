#include "codec/opus.hpp"

#include <cstring>

#include "audio/pcm.hpp"

#if AES67_SRT_WITH_OPUS
#include <opus/opus_multistream.h>
#endif

namespace aes67_srt::codec {
namespace {

// One 20 ms Opus frame of 8 channels at 128 kbit/s each is ~2500 bytes; the
// probe in docs/research/opus.md used 4000 and that is the budget here too.
constexpr size_t k_max_packet_bytes = 4000;

}  // namespace

bool opus_available() {
#if AES67_SRT_WITH_OPUS
  return true;
#else
  return false;
#endif
}

const char* opus_unavailable_reason() {
#if AES67_SRT_WITH_OPUS
  return "";
#else
  return "this build has no libopus: rebuild with WITH_OPUS=ON and libopus "
         "development files installed";
#endif
}

#if AES67_SRT_WITH_OPUS

struct OpusBlock::Impl {
  OpusMSEncoder* encoder = nullptr;
  OpusMSDecoder* decoder = nullptr;
  int channels = 0;
  int sample_rate = 48000;
  int frame_frames = 0;
  double lookahead_ms = 0.0;
  std::vector<float> floats;    // encode input / decode output
  std::vector<uint8_t> packet;  // encode output
};

OpusBlock::OpusBlock() : impl_(new Impl()) {}

OpusBlock::~OpusBlock() {
  close();
  delete impl_;
}

bool OpusBlock::open(int channels, int sample_rate, int frame_frames,
                     int bitrate_bps_per_channel, std::string* error) {
  close();
  if (channels < 1 || channels > 255) {
    if (error != nullptr) {
      *error = "opus: channels must be 1..255, got " + std::to_string(channels);
    }
    return false;
  }
  if (frame_frames < 120) {  // Opus's shortest frame is 2.5 ms = 120 at 48 kHz
    if (error != nullptr) {
      *error = "opus: frame of " + std::to_string(frame_frames) +
               " samples is shorter than Opus's 2.5 ms minimum";
    }
    return false;
  }

  unsigned char mapping[255];
  for (int channel = 0; channel < channels; ++channel) {
    mapping[channel] = static_cast<unsigned char>(channel);
  }

  int status = OPUS_OK;
  // streams = channels, coupled = 0: eight mono streams, family 255 with an
  // explicit mapping. See the header for why coupling is not assumed.
  impl_->encoder = opus_multistream_encoder_create(
      sample_rate, channels, channels, 0, mapping, OPUS_APPLICATION_AUDIO, &status);
  if (impl_->encoder == nullptr || status != OPUS_OK) {
    if (error != nullptr) {
      *error = std::string("opus: encoder: ") + opus_strerror(status);
    }
    close();
    return false;
  }
  impl_->decoder = opus_multistream_decoder_create(sample_rate, channels, channels,
                                                   0, mapping, &status);
  if (impl_->decoder == nullptr || status != OPUS_OK) {
    if (error != nullptr) {
      *error = std::string("opus: decoder: ") + opus_strerror(status);
    }
    close();
    return false;
  }

  const int bitrate = bitrate_bps_per_channel * channels;
  opus_multistream_encoder_ctl(impl_->encoder, OPUS_SET_BITRATE(bitrate));
  // In-band FEC off, explicitly: SRT already recovers loss, and FEC costs quality
  // on music by steering the codec to SILK. Documented rather than defaulted.
  opus_multistream_encoder_ctl(impl_->encoder, OPUS_SET_INBAND_FEC(0));

  opus_int32 lookahead = 0;
  if (opus_multistream_encoder_ctl(impl_->encoder,
                                   OPUS_GET_LOOKAHEAD(&lookahead)) == OPUS_OK) {
    impl_->lookahead_ms = 1000.0 * static_cast<double>(lookahead) / sample_rate;
  }

  impl_->channels = channels;
  impl_->sample_rate = sample_rate;
  impl_->frame_frames = frame_frames;
  impl_->floats.assign(static_cast<size_t>(channels) * frame_frames, 0.0f);
  impl_->packet.assign(k_max_packet_bytes, 0);
  return true;
}

void OpusBlock::close() {
  if (impl_ == nullptr) {
    return;
  }
  if (impl_->encoder != nullptr) {
    opus_multistream_encoder_destroy(impl_->encoder);
    impl_->encoder = nullptr;
  }
  if (impl_->decoder != nullptr) {
    opus_multistream_decoder_destroy(impl_->decoder);
    impl_->decoder = nullptr;
  }
  impl_->channels = 0;
  impl_->frame_frames = 0;
}

bool OpusBlock::is_open() const {
  return impl_ != nullptr && impl_->encoder != nullptr && impl_->decoder != nullptr;
}

bool OpusBlock::encode(const uint8_t* pcm, size_t frames,
                       std::vector<uint8_t>* packet, std::string* error) {
  if (!is_open()) {
    if (error != nullptr) {
      *error = "opus: the encoder is not open";
    }
    return false;
  }
  if (pcm == nullptr || packet == nullptr) {
    if (error != nullptr) {
      *error = "opus: no PCM to encode or no packet to fill";
    }
    return false;
  }
  if (frames != static_cast<size_t>(impl_->frame_frames)) {
    if (error != nullptr) {
      *error = "opus: a frame must be exactly " +
               std::to_string(impl_->frame_frames) + " samples, got " +
               std::to_string(frames);
    }
    return false;
  }

  audio::s24_3le_to_float(pcm, frames, static_cast<unsigned>(impl_->channels),
                          impl_->floats.data());
  const int bytes = opus_multistream_encode_float(
      impl_->encoder, impl_->floats.data(), impl_->frame_frames,
      impl_->packet.data(), static_cast<opus_int32>(impl_->packet.size()));
  if (bytes < 0) {
    if (error != nullptr) {
      *error = std::string("opus: encode: ") + opus_strerror(bytes);
    }
    return false;
  }
  packet->assign(impl_->packet.begin(), impl_->packet.begin() + bytes);
  return true;
}

bool OpusBlock::decode(const uint8_t* packet, size_t bytes, size_t frames,
                       uint8_t* pcm, std::string* error) {
  if (!is_open()) {
    if (error != nullptr) {
      *error = "opus: the decoder is not open";
    }
    return false;
  }
  if (packet == nullptr || pcm == nullptr || bytes == 0) {
    if (error != nullptr) {
      *error = "opus: no packet to decode or no PCM to fill";
    }
    return false;
  }
  if (frames != static_cast<size_t>(impl_->frame_frames)) {
    if (error != nullptr) {
      *error = "opus: a frame must be exactly " +
               std::to_string(impl_->frame_frames) + " samples, got " +
               std::to_string(frames);
    }
    return false;
  }

  const int decoded = opus_multistream_decode_float(
      impl_->decoder, packet, static_cast<opus_int32>(bytes), impl_->floats.data(),
      impl_->frame_frames, 0);
  if (decoded < 0) {
    if (error != nullptr) {
      *error = std::string("opus: decode: ") + opus_strerror(decoded);
    }
    return false;
  }
  audio::float_to_s24_3le(impl_->floats.data(), frames,
                          static_cast<unsigned>(impl_->channels), pcm);
  return true;
}

double OpusBlock::lookahead_ms() const {
  return impl_ != nullptr ? impl_->lookahead_ms : 0.0;
}

size_t OpusBlock::max_packet_bytes() const {
  return k_max_packet_bytes;
}

#else  // !AES67_SRT_WITH_OPUS

struct OpusBlock::Impl {};

OpusBlock::OpusBlock() : impl_(nullptr) {}
OpusBlock::~OpusBlock() = default;

bool OpusBlock::open(int, int, int, int, std::string* error) {
  if (error != nullptr) {
    *error = opus_unavailable_reason();
  }
  return false;
}
void OpusBlock::close() {}
bool OpusBlock::is_open() const {
  return false;
}
bool OpusBlock::encode(const uint8_t*, size_t, std::vector<uint8_t>*,
                       std::string* error) {
  if (error != nullptr) {
    *error = opus_unavailable_reason();
  }
  return false;
}
bool OpusBlock::decode(const uint8_t*, size_t, size_t, uint8_t*,
                       std::string* error) {
  if (error != nullptr) {
    *error = opus_unavailable_reason();
  }
  return false;
}
double OpusBlock::lookahead_ms() const {
  return 0.0;
}
size_t OpusBlock::max_packet_bytes() const {
  return k_max_packet_bytes;
}

#endif  // AES67_SRT_WITH_OPUS

}  // namespace aes67_srt::codec
