#include "engine.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "audio/backend.hpp"
#include "config.hpp"
#include "test_framework.hpp"
#include "transport/link.hpp"
#include "wire/frame.hpp"

namespace {

using aes67_srt::Config;
using aes67_srt::Engine;
using aes67_srt::audio::AudioFormat;
using aes67_srt::wire::PayloadType;

namespace wire = aes67_srt::wire;

/**
 * A configuration with `blocks` 8-channel blocks, on the null device.
 *
 * The null device is not a compromise here: the engine's job is to move bytes
 * between a device and a link, and a device with defined behaviour proves that
 * as well as hardware does — better, because CI can run it.
 */
Config engine_config(size_t blocks, const std::string& mode = "listener",
                     int local_port = 0, const std::string& peer = std::string()) {
  Config config;
  config.audio.backend = "null";
  config.audio.channels = static_cast<int>(blocks * 8);
  config.link.blocks = static_cast<int>(blocks);
  config.link.mode = mode;
  config.link.role = "duplex";
  config.link.local_port = local_port;
  config.link.peer = peer;
  config.fill_default_blocks();
  return config;
}

/**
 * A period in which every frame and channel can be told apart.
 *
 * Three bytes per sample, each carrying what it is: the low byte is the channel,
 * the middle one is the frame, and the high one is a channel-dependent constant.
 * A mapping mistake therefore shows up as the wrong channel's data in the wrong
 * place rather than as silence, which is the failure that would otherwise pass.
 */
std::vector<uint8_t> make_period(const AudioFormat& format) {
  std::vector<uint8_t> period(format.period_bytes(), 0);
  for (size_t frame = 0; frame < format.period_frames; ++frame) {
    for (size_t channel = 0; channel < format.channels; ++channel) {
      const size_t at = (frame * format.channels + channel) * format.sample_bytes;
      period[at] = static_cast<uint8_t>(channel);
      period[at + 1] = static_cast<uint8_t>(frame);
      period[at + 2] = static_cast<uint8_t>(channel ^ 0x5a);
    }
  }
  return period;
}

bool contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE(engine_a_period_becomes_a_frame_and_comes_back_byte_for_byte) {
  const Config config = engine_config(8);
  Engine engine;
  std::string error;
  CHECK(engine.prepare(config, &error));

  const AudioFormat format = aes67_srt::audio::audio_format_from(config.audio);
  const std::vector<uint8_t> period = make_period(format);

  wire::Frame frame;
  CHECK(engine.pack_period(period.data(), 48000, &frame, &error));
  CHECK_EQ(frame.blocks.size(), static_cast<size_t>(8));
  CHECK_EQ(frame.sample_position, uint64_t{48000});
  for (const wire::Block& block : frame.blocks) {
    CHECK_EQ(static_cast<unsigned>(block.channels), 8u);
    CHECK(block.payload == PayloadType::pcm_l24);
  }

  // Out onto the wire and back. The frame size is the number the whole
  // bandwidth budget rests on, so it is asserted rather than described.
  std::vector<uint8_t> bytes;
  CHECK(wire::encode(frame, &bytes, &error));
  CHECK_EQ(bytes.size(), static_cast<size_t>(9312));

  wire::Frame arrived;
  CHECK(wire::decode(bytes.data(), bytes.size(), &arrived, &error));

  std::vector<uint8_t> rebuilt(format.period_bytes(), 0);
  CHECK(engine.unpack_frame(arrived, rebuilt.data(), &error));
  CHECK(rebuilt == period);
}

TEST_CASE(engine_the_mapping_is_the_one_the_configuration_declares) {
  Config config = engine_config(8);
  // Blocks 0 and 1 carry each other's device channels. Nothing about that is
  // conventional, which is the point: anything derived rather than read from the
  // configuration packs the wrong samples here. It is still a permutation of the
  // device, so the round trip below has to come back whole.
  config.blocks[0].channels = {8, 9, 10, 11, 12, 13, 14, 15};
  config.blocks[1].channels = {0, 1, 2, 3, 4, 5, 6, 7};

  Engine engine;
  std::string error;
  CHECK(engine.prepare(config, &error));

  const AudioFormat format = aes67_srt::audio::audio_format_from(config.audio);
  const std::vector<uint8_t> period = make_period(format);

  wire::Frame frame;
  CHECK(engine.pack_period(period.data(), 0, &frame, &error));
  CHECK_EQ(frame.blocks.size(), static_cast<size_t>(8));

  const wire::Block& first = frame.blocks[0];
  CHECK_EQ(first.data.size(),
           aes67_srt::wire::payload_bytes(PayloadType::pcm_l24, 8, 48));

  // The block's first sample is device channel 8's first sample...
  const size_t channel_8 = 8 * format.sample_bytes;
  CHECK_EQ(first.data[0], period[channel_8]);
  CHECK_EQ(first.data[1], period[channel_8 + 1]);
  CHECK_EQ(first.data[2], period[channel_8 + 2]);

  // ...and its eighth is device channel 15's.
  const size_t eighth = 7 * format.sample_bytes;
  const size_t channel_15 = 15 * format.sample_bytes;
  CHECK_EQ(first.data[eighth], period[channel_15]);

  // And it comes back to the same device channels: the mapping is applied on the
  // way in as well as on the way out, or a site would hear its blocks shuffled.
  std::vector<uint8_t> rebuilt(format.period_bytes(), 0);
  CHECK(engine.unpack_frame(frame, rebuilt.data(), &error));
  CHECK(rebuilt == period);
}

TEST_CASE(engine_a_one_block_link_is_the_dev_configuration_shape) {
  const Config config = engine_config(1);
  Engine engine;
  std::string error;
  CHECK(engine.prepare(config, &error));

  const AudioFormat format = aes67_srt::audio::audio_format_from(config.audio);
  const std::vector<uint8_t> period = make_period(format);

  wire::Frame frame;
  CHECK(engine.pack_period(period.data(), 0, &frame, &error));
  CHECK_EQ(frame.blocks.size(), static_cast<size_t>(1));

  std::vector<uint8_t> rebuilt(format.period_bytes(), 0);
  CHECK(engine.unpack_frame(frame, rebuilt.data(), &error));
  CHECK(rebuilt == period);
}

TEST_CASE(engine_refuses_a_frame_this_link_does_not_expect) {
  const Config config = engine_config(8);
  Engine engine;
  std::string error;
  CHECK(engine.prepare(config, &error));

  const AudioFormat format = aes67_srt::audio::audio_format_from(config.audio);
  const std::vector<uint8_t> period = make_period(format);
  wire::Frame frame;
  CHECK(engine.pack_period(period.data(), 0, &frame, &error));

  std::vector<uint8_t> rebuilt(format.period_bytes(), 0);

  // Another link's frame. The link id exists for exactly this, and on a site
  // with more than one link, playing the neighbours' audio is not a subtle bug.
  wire::Frame other_link = frame;
  other_link.link_id = 7;
  CHECK(!engine.unpack_frame(other_link, rebuilt.data(), &error));
  CHECK(contains(error, "link 7"));

  // A frame that cannot fill the device: audio would go missing silently.
  wire::Frame too_few = frame;
  too_few.blocks.pop_back();
  CHECK(!engine.unpack_frame(too_few, rebuilt.data(), &error));
  CHECK(contains(error, "blocks"));

  // The same block twice, which would leave another block's channels silent.
  // The count is unchanged, so this is also the check that a duplicate is caught
  // by the block indices rather than only by how many there are.
  wire::Frame doubled = frame;
  doubled.blocks[1] = doubled.blocks[0];
  CHECK(!engine.unpack_frame(doubled, rebuilt.data(), &error));
  CHECK(contains(error, "twice"));

  // A payload of the wrong width: playing 16-bit samples as 24-bit is not an
  // error anything downstream reports — it is just wrong audio.
  wire::Frame wrong_width = frame;
  wrong_width.blocks[0].payload = PayloadType::pcm_l16;
  CHECK(!engine.unpack_frame(wrong_width, rebuilt.data(), &error));
  CHECK(contains(error, "L16"));

  // A block shorter than its own header claims: the alternative is reading past
  // the end of the buffer.
  wire::Frame truncated = frame;
  truncated.blocks[0].data.resize(10);
  CHECK(!engine.unpack_frame(truncated, rebuilt.data(), &error));
  CHECK(contains(error, "shorter"));

  // And the frame all of that was built from still unpacks, so none of the
  // refusals above was a refusal of the good frame.
  CHECK(engine.unpack_frame(frame, rebuilt.data(), &error));
  CHECK(rebuilt == period);
}

TEST_CASE(engine_refuses_a_configuration_that_would_drop_audio) {
  Config config = engine_config(8);
  config.audio.channels = 48;  // six blocks' worth, while eight are declared
  Engine engine;
  std::string error;
  CHECK(!engine.prepare(config, &error));
  CHECK(contains(error, "48"));
}

TEST_CASE(engine_carries_a_period_from_one_box_to_another) {
  // The commissioning loopback, out of the pieces this project actually has: a
  // device with defined behaviour at each end, a real SRT link between them, and
  // the whole path — device, blocks, frame, messages, and back — driven one turn
  // at a time so that nothing is racing anything.
  if (!aes67_srt::transport::Link::available()) {
    std::cout << "  no libsrt in this build: skipping the engine loopback"
              << std::endl;
    return;
  }

  const Config site_config = engine_config(8, "listener", 19501);
  const Config remote_config = engine_config(8, "caller", 19502, "127.0.0.1:19501");

  Engine site;
  Engine remote;
  std::string error;
  CHECK(site.prepare(site_config, &error));
  CHECK(remote.prepare(remote_config, &error));

  // The listener blocks until a caller arrives, so it gets its own thread — the
  // same shape the transport's own loopback test uses.
  std::atomic<bool> site_up{false};
  std::string site_error;
  std::thread accepting([&] { site_up = site.open(&site_error); });
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  const bool remote_up = remote.open(&error);
  accepting.join();

  CHECK(remote_up);
  CHECK(site_up.load());
  if (!remote_up || !site_up.load()) {
    test::report_failure("the engine loopback did not come up", __FILE__, __LINE__,
                         error + " / " + site_error);
    return;
  }

  // The site's own AES67 source puts a period onto its device.
  const AudioFormat format = aes67_srt::audio::audio_format_from(site_config.audio);
  const std::vector<uint8_t> period = make_period(format);
  CHECK(site.backend()->write(period.data(), format.period_frames, &error));

  // One turn of the transmit loop: a period in, a frame's worth of messages out.
  CHECK(site.step_transmit(&error));
  CHECK_EQ(site.frames_sent(), uint64_t{1});

  // The far end turns until a whole frame has arrived. A frame is eight
  // messages, so one turn is not always enough — and a turn that produced no
  // frame is not a failure, which is what lets this loop be driven without a
  // timer and without a thread.
  for (int turn = 0; turn < 64 && remote.frames_received() == 0; ++turn) {
    CHECK(remote.step_receive(&error));
  }
  CHECK_EQ(remote.frames_received(), uint64_t{1});
  CHECK_EQ(remote.frames_refused(), uint64_t{0});

  // What comes out of the far end's device is what went into the near end's,
  // byte for byte: 64 channels, eight blocks, one frame, across an SRT link
  // between two processes' worth of engine.
  std::vector<uint8_t> played(format.period_bytes(), 0);
  CHECK(remote.backend()->read(played.data(), format.period_frames, &error));
  CHECK(played == period);
  CHECK_EQ(remote.backend()->overruns(), 0u);

  site.stop();
  remote.stop();
}
