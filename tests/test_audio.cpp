#include "audio/backend.hpp"
#include "audio/null_backend.hpp"

#include <memory>
#include <string>
#include <vector>

#include "config.hpp"
#include "test_framework.hpp"
#include "wire/frame.hpp"

namespace {

using aes67_srt::audio::AudioFormat;
using aes67_srt::audio::create_audio_backend;
using aes67_srt::audio::NullBackend;
using aes67_srt::audio::ravenna_backend_available;
using aes67_srt::wire::PayloadType;

/** Audio that is not silence, so "it arrived" is distinguishable from "it was
 *  zeroed by the padding". */
std::vector<uint8_t> make_audio(size_t bytes, uint8_t seed) {
  std::vector<uint8_t> audio(bytes);
  for (size_t index = 0; index < bytes; ++index) {
    audio[index] = static_cast<uint8_t>((index * 7 + seed) & 0xff);
  }
  return audio;
}

bool contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE(audio_a_period_is_one_millisecond_and_agrees_with_the_wire_format) {
  const AudioFormat format;  // the specification: 48 kHz, 64 channels, L24, 1 ms
  CHECK_EQ(format.period_frames, 48u);
  CHECK_EQ(format.period_samples(), static_cast<size_t>(3072));  // 64 x 48
  CHECK_EQ(format.period_bytes(), static_cast<size_t>(9216));    // 64 x 48 x 3

  // A period holds eight 8-channel blocks, and **an 8-channel block at 1 ms is
  // 1152 bytes** - exactly the payload the wire format reserves for one block.
  // Neither module was told about the other; if this assertion ever fails, one of
  // them has drifted and every frame on the link is the wrong length.
  const size_t block_bytes = format.frames_to_bytes(format.period_frames) / 8;
  CHECK_EQ(block_bytes,
           aes67_srt::wire::payload_bytes(PayloadType::pcm_l24, 8, 48));
  CHECK_EQ(block_bytes, static_cast<size_t>(1152));
}

TEST_CASE(audio_the_null_backend_refuses_a_format_it_cannot_carry) {
  NullBackend backend;
  std::string error;

  AudioFormat no_channels;
  no_channels.channels = 0;
  CHECK(!backend.open(no_channels, &error));
  CHECK(contains(error, "audio.channels"));

  AudioFormat weird_sample;
  weird_sample.sample_bytes = 4;  // the device carries 16- or 24-bit, not 32
  CHECK(!backend.open(weird_sample, &error));
  CHECK(contains(error, "audio.format"));

  AudioFormat no_period;
  no_period.period_frames = 0;
  CHECK(!backend.open(no_period, &error));
  CHECK(contains(error, "audio.period_frames"));

  CHECK(!backend.is_open());
}

TEST_CASE(audio_the_null_backend_loops_back_what_is_written) {
  NullBackend backend;
  std::string error;
  CHECK(backend.open(AudioFormat(), &error));
  CHECK(backend.is_open());
  CHECK_EQ(backend.kind(), std::string("null"));

  const size_t period = backend.format().period_bytes();
  const std::vector<uint8_t> audio = make_audio(period, 0x40);
  CHECK(backend.write(audio.data(), backend.format().period_frames, &error));
  CHECK_EQ(backend.pending_bytes(), period);

  std::vector<uint8_t> captured(period, 0);
  CHECK(backend.read(captured.data(), backend.format().period_frames, &error));
  CHECK(captured == audio);
  CHECK_EQ(backend.pending_bytes(), static_cast<size_t>(0));
  // Nothing was starved and nothing was refused.
  CHECK_EQ(backend.overruns(), 0u);
  CHECK_EQ(backend.underruns(), 0u);
  backend.close();
  CHECK(!backend.is_open());
}

TEST_CASE(audio_a_read_with_nothing_written_is_silence_and_is_counted) {
  NullBackend backend;
  std::string error;
  CHECK(backend.open(AudioFormat(), &error));

  // A device that has to starve does so deliberately and says so, rather than
  // returning a short period that every caller would then have to reason about.
  std::vector<uint8_t> captured(backend.format().period_bytes(), 0xff);
  CHECK(backend.read(captured.data(), backend.format().period_frames, &error));
  CHECK_EQ(backend.overruns(), 1u);
  for (uint8_t byte : captured) {
    CHECK_EQ(byte, 0);  // silence, not whatever happened to be in the buffer
  }
}

TEST_CASE(audio_a_write_the_device_cannot_take_is_counted) {
  NullBackend backend;
  std::string error;
  CHECK(backend.open(AudioFormat(), &error));

  const AudioFormat& format = backend.format();
  const std::vector<uint8_t> too_much = make_audio(format.period_bytes() * 2, 0x11);
  CHECK(backend.write(too_much.data(), format.period_frames * 2, &error));
  CHECK_EQ(backend.underruns(), 1u);
  // It kept a period's worth rather than losing everything.
  CHECK_EQ(backend.pending_bytes(), format.period_bytes());
}

TEST_CASE(audio_the_factory_refuses_a_backend_this_build_does_not_have) {
  // A typo in the configuration, and the reason names the string that was typed
  // rather than reporting a null pointer somewhere further along.
  aes67_srt::AudioConfig config;
  config.backend = "coreaudio";
  std::unique_ptr<aes67_srt::audio::AudioBackend> backend =
      create_audio_backend(config);
  CHECK_EQ(backend->kind(), std::string("unavailable"));
  std::string error;
  CHECK(!backend->open(AudioFormat{}, &error));
  CHECK(contains(error, "coreaudio"));
  CHECK(contains(error, "audio.backend"));

  // And the one backend every build has, so the fake mode always has somewhere
  // to put its audio.
  CHECK(aes67_srt::audio::null_backend_available());
  aes67_srt::AudioConfig as_null;
  as_null.backend = "null";
  CHECK_EQ(create_audio_backend(as_null)->kind(), std::string("null"));
}

#if AES67_SRT_WITH_ALSA
TEST_CASE(audio_with_alsa_the_factory_hands_back_the_ravenna_backend) {
  CHECK(ravenna_backend_available());

  aes67_srt::AudioConfig config;  // the specification: plughw:RAVENNA, 64 channels
  std::unique_ptr<aes67_srt::audio::AudioBackend> backend =
      create_audio_backend(config);
  CHECK_EQ(backend->kind(), std::string("ravenna"));

  // No CI runner has the RAVENNA kernel module, so opening the device must fail
  // — and fail naming the device, because that message is what a commissioning
  // engineer reads on a machine that is missing the module rather than broken.
  std::string error;
  const AudioFormat format;
  CHECK(!backend->open(format, &error));
  CHECK(contains(error, config.device));
  CHECK(!backend->is_open());
  CHECK_EQ(backend->overruns(), 0u);

  // A device that could not be opened refuses politely rather than crashing:
  // this is the state the audio thread sits in while somebody fixes it.
  std::vector<uint8_t> period(format.period_bytes(), 0);
  CHECK(!backend->read(period.data(), format.period_frames, &error));
  CHECK(!error.empty());
  CHECK(!backend->write(period.data(), format.period_frames, &error));
  CHECK(!error.empty());

  backend->close();
  CHECK(!backend->is_open());
  // Closing twice is not an error: the destructor does it again on every exit
  // path, including after a failed open.
  backend->close();
}
#else
TEST_CASE(audio_without_alsa_the_ravenna_backend_refuses_and_says_why) {
  // macOS, and any Linux build configured with -DWITH_ALSA=OFF. The module still
  // compiles and still answers: a reason rather than a null pointer, and the
  // reason names both the problem and the way out.
  CHECK(!ravenna_backend_available());

  aes67_srt::AudioConfig config;  // asks for ravenna, as production does
  std::unique_ptr<aes67_srt::audio::AudioBackend> backend =
      create_audio_backend(config);
  std::string error;
  CHECK(!backend->open(AudioFormat{}, &error));
  CHECK(contains(error, "ALSA"));
  CHECK(contains(error, "null"));
  CHECK(!backend->is_open());

  std::vector<uint8_t> period(AudioFormat{}.period_bytes(), 0);
  CHECK(!backend->read(period.data(), AudioFormat{}.period_frames, &error));
  CHECK(!backend->write(period.data(), AudioFormat{}.period_frames, &error));
}

TEST_CASE(audio_the_ravenna_backend_is_absent_rather_than_faked) {
  // The trap this guards: a backend that pretends to open so that "the audio
  // path" looks alive on a developer machine is worse than no backend at all,
  // because the illusion survives all the way to a site.
  CHECK(!ravenna_backend_available());
  aes67_srt::AudioConfig config;
  std::unique_ptr<aes67_srt::audio::AudioBackend> backend =
      create_audio_backend(config);
  CHECK_EQ(backend->kind(), std::string("unavailable"));
  CHECK(backend->detail().find("no ALSA") != std::string::npos);
}
#endif

TEST_CASE(audio_carries_audio_end_to_end_without_hardware) {
  // Ticket 09's first criterion, with no ALSA device, no daemon and no socket:
  // audio written to a site-side device is captured, framed, sent as bytes,
  // reassembled, decoded and written to a far-side device, and comes out
  // unchanged. The shape of the whole product, proved before any of it touches a
  // Pi.
  NullBackend site;
  NullBackend remote;
  std::string error;
  CHECK(site.open(AudioFormat(), &error));
  CHECK(remote.open(AudioFormat(), &error));

  // The site's local AES67 source puts a period of audio onto its device.
  const std::vector<uint8_t> source =
      make_audio(site.format().period_bytes(), 0xa5);
  CHECK(site.write(source.data(), site.format().period_frames, &error));

  // Capture it, and cut it into the eight 8-channel blocks the transport carries.
  std::vector<uint8_t> captured(site.format().period_bytes(), 0);
  CHECK(site.read(captured.data(), site.format().period_frames, &error));

  aes67_srt::wire::Frame frame;
  frame.link_id = 1;
  frame.sequence = 1;
  frame.sample_position = 48000;
  const size_t block_bytes =
      aes67_srt::wire::payload_bytes(PayloadType::pcm_l24, 8, 48);
  for (size_t block = 0; block < 8; ++block) {
    aes67_srt::wire::Block wire_block;
    wire_block.index = static_cast<uint8_t>(block);
    wire_block.payload = PayloadType::pcm_l24;
    wire_block.channels = 8;
    wire_block.data.assign(
        captured.begin() + static_cast<long>(block * block_bytes),
        captured.begin() + static_cast<long>((block + 1) * block_bytes));
    frame.blocks.push_back(std::move(wire_block));
  }

  // Out it goes and back it comes. The transport is a byte pipe, so this exercises
  // the real encoder and decoder without needing a socket.
  std::vector<uint8_t> on_the_wire;
  CHECK(aes67_srt::wire::encode(frame, &on_the_wire, &error));
  aes67_srt::wire::Frame arrived;
  CHECK(aes67_srt::wire::decode(on_the_wire.data(), on_the_wire.size(), &arrived,
                                &error));
  CHECK_EQ(arrived.sample_position, frame.sample_position);

  // Reassemble the blocks into the far end's period and play it out.
  std::vector<uint8_t> rebuilt;
  for (const aes67_srt::wire::Block& block : arrived.blocks) {
    rebuilt.insert(rebuilt.end(), block.data.begin(), block.data.end());
  }
  CHECK(rebuilt == captured);
  CHECK(remote.write(rebuilt.data(), remote.format().period_frames, &error));

  std::vector<uint8_t> played(remote.format().period_bytes(), 0);
  CHECK(remote.read(played.data(), remote.format().period_frames, &error));

  // Byte for byte, all 9216 of them, through eight blocks and a frame.
  CHECK(played == source);
  CHECK_EQ(remote.overruns(), 0u);
  CHECK_EQ(remote.underruns(), 0u);
}
