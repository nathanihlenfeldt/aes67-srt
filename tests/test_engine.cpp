#include "engine.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "audio/backend.hpp"
#include "audio/pcm.hpp"
#include "audio_bytes.hpp"
#include "clock/resampler.hpp"
#include "config.hpp"
#include "log.hpp"
#include "test_framework.hpp"
#include "transport/link.hpp"
#include "wire/frame.hpp"

namespace {

using aes67_srt::Config;
using aes67_srt::Engine;
using aes67_srt::EngineStatus;
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
  CHECK(engine.unpack_frame(frame, rebuilt.data(), &error));
  CHECK(rebuilt == period);
}

TEST_CASE(engine_carries_an_opus_block_through_the_codec_seam) {
  if (!aes67_srt::codec::opus_available()) {
    std::cout << "    no libopus in this build: skipping the engine codec seam"
              << std::endl;
    return;
  }

  Config config = engine_config(1);
  config.audio.period_frames =
      960;  // 20 ms: in codec mode the period *is* the frame
  config.blocks[0].codec = "opus";
  config.blocks[0].bitrate_bps_per_channel = 128000;

  Engine engine;
  std::string error;
  CHECK(engine.prepare(config, &error));

  const AudioFormat format = aes67_srt::audio::audio_format_from(config.audio);
  // A tone, so the codec has something real to carry rather than zeros.
  std::vector<float> samples(static_cast<size_t>(format.period_frames) *
                             format.channels);
  for (size_t frame = 0; frame < format.period_frames; ++frame) {
    const float value =
        0.25f * std::sin(2.0 * 3.14159265358979 * 997.0 * frame / 48000.0);
    for (size_t channel = 0; channel < format.channels; ++channel) {
      samples[frame * format.channels + channel] = value;
    }
  }
  std::vector<uint8_t> period(format.period_bytes(), 0);
  aes67_srt::audio::float_to_s24_3le(samples.data(), format.period_frames,
                                     format.channels, period.data());

  wire::Frame frame;
  CHECK(engine.pack_period(period.data(), 0, &frame, &error));
  CHECK_EQ(frame.blocks.size(), static_cast<size_t>(1));
  // The seam did its job: the block says Opus, and it is much smaller than PCM.
  CHECK(frame.blocks[0].payload == PayloadType::opus);
  CHECK(!frame.blocks[0].data.empty());
  CHECK(frame.blocks[0].data.size() < period.size() / 4);

  std::vector<uint8_t> rebuilt(format.period_bytes(), 0);
  CHECK(engine.unpack_frame(frame, rebuilt.data(), &error));

  // Lossy and lookahead-compensated, so this is a level check, not a byte compare.
  std::vector<float> original(samples.size());
  std::vector<float> returned(samples.size());
  aes67_srt::audio::s24_3le_to_float(period.data(), format.period_frames,
                                     format.channels, original.data());
  aes67_srt::audio::s24_3le_to_float(rebuilt.data(), format.period_frames,
                                     format.channels, returned.data());
  double energy_in = 0.0;
  double energy_out = 0.0;
  // The middle of the frame, away from the codec's edges, and a loose tolerance:
  // this proves audio came through the seam, not that Opus is transparent.
  for (size_t frame = 240; frame < 720; ++frame) {
    for (size_t channel = 0; channel < format.channels; ++channel) {
      const size_t index = frame * format.channels + channel;
      energy_in += original[index] * original[index];
      energy_out += returned[index] * returned[index];
    }
  }
  // Lossy and delayed: decoding one frame in isolation yields the input shifted by
  // the codec's lookahead, so the tail of the frame has not arrived yet and the
  // energy is lower by that fraction. In a stream the shift is a constant latency,
  // which is exactly what codec_delay_ms() reports and the A/V budget subtracts.
  // So this proves audio came through the seam at the right order of magnitude;
  // test_codec.cpp proves the codec itself is transparent (correlation > 0.99).
  CHECK(energy_in > 0.0);
  CHECK(energy_out > 0.5 * energy_in);
  CHECK(energy_out < 1.5 * energy_in);

  // And the codec's share of the A/V budget is the frame plus the lookahead.
  CHECK(engine.codec_delay_ms() > 20.0);
  CHECK(engine.codec_delay_ms() < 30.0);
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

TEST_CASE(engine_carries_eight_opus_blocks_through_the_pool) {
  if (!aes67_srt::codec::opus_available()) {
    std::cout << "    no libopus in this build: skipping the eight-block codec seam"
              << std::endl;
    return;
  }

  Config config = engine_config(8);
  config.audio.period_frames = 960;  // 20 ms
  for (aes67_srt::BlockConfig& block : config.blocks) {
    block.codec = "opus";
    block.bitrate_bps_per_channel = 128000;
  }

  Engine engine;
  std::string error;
  CHECK(engine.prepare(config, &error));

  const AudioFormat format = aes67_srt::audio::audio_format_from(config.audio);
  std::vector<float> samples(static_cast<size_t>(format.period_frames) *
                             format.channels);
  for (size_t frame = 0; frame < format.period_frames; ++frame) {
    const float value =
        0.25f * std::sin(2.0 * 3.14159265358979 * 997.0 * frame / 48000.0);
    for (size_t channel = 0; channel < format.channels; ++channel) {
      samples[frame * format.channels + channel] = value;
    }
  }
  std::vector<uint8_t> period(format.period_bytes(), 0);
  aes67_srt::audio::float_to_s24_3le(samples.data(), format.period_frames,
                                     format.channels, period.data());

  wire::Frame frame;
  CHECK(engine.pack_period(period.data(), 0, &frame, &error));
  CHECK_EQ(frame.blocks.size(), static_cast<size_t>(8));
  for (const wire::Block& block : frame.blocks) {
    // Every block went through the pool and came back as an Opus packet.
    CHECK(block.payload == PayloadType::opus);
    CHECK(!block.data.empty());
    CHECK(block.data.size() < period.size() / 8 / 4);
  }

  std::vector<uint8_t> rebuilt(format.period_bytes(), 0);
  CHECK(engine.unpack_frame(frame, rebuilt.data(), &error));
  // Not silence: the eight encodes and decodes carried audio.
  bool any = false;
  for (uint8_t byte : rebuilt) {
    any = any || byte != 0;
  }
  CHECK(any);
  CHECK(engine.codec_delay_ms() > 20.0);
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

TEST_CASE(engine_carries_audio_from_one_box_to_another_through_the_clock) {
  // The commissioning loopback, out of the pieces this project actually has: a
  // device with defined behaviour at each end, a real SRT link between them, the
  // clock in between, and the whole path — device, blocks, frame, messages, clock,
  // device — driven one turn at a time so that nothing is racing anything.
  //
  // **What changed when the clock landed here: this cannot assert byte-exactness
  // any more, and that is a consequence of ADR 0003 rather than a regression.**
  // Through a resampler the output equals the input only while the ratio is exactly
  // 1, and the converter's two periods of working room are a level deficit the
  // control reads as a rate error, so it corrects — by resampling, which is the
  // point. Byte-exactness is proved where it still belongs: the transport's own
  // loopback (`test_transport.cpp`) and the resampler at ratio 1
  // (`test_clock_resampler.cpp`). What this test proves is that the clock carries
  // the audio and holds its level, with nothing lost.
  if (!aes67_srt::transport::Link::available()) {
    std::cout << "  no libsrt in this build: skipping the engine loopback"
              << std::endl;
    return;
  }
  if (!aes67_srt::clock::Resampler::available()) {
    std::cout << "  no libsamplerate in this build: skipping the engine loopback"
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

  // The site's own AES67 source puts audio onto its device: a constant, because a
  // resampler passes DC at unity and a period that encoded its own position would
  // come back filtered — the wrong signal for a path that resamples by design.
  const AudioFormat format = aes67_srt::audio::audio_format_from(site_config.audio);
  const std::vector<uint8_t> period = audio_bytes::constant_period(format, 1000000);

  // The clock plays nothing until its level is what the link's latency bought, so a
  // loopback that sends one frame now proves nothing: the receiver would still be
  // priming. Drive both ends for a while instead — the sender emits a frame per
  // turn, the receiver moves what arrived and plays one period.
  //
  // With 48-frame periods at 48 kHz one period *is* one millisecond, so the target
  // in periods is the latency in milliseconds: 120.
  const int target_periods = site_config.link.latency_ms;
  int periods_with_audio = 0;
  int turns = 0;
  std::vector<uint8_t> played(format.period_bytes(), 0);
  // Enough turns to prime the clock and then prove the audio flows: the claim does
  // not need minutes of it, and every turn costs a period of real time through the
  // device.
  const int total_turns = target_periods + 300;
  for (; turns < total_turns; ++turns) {
    CHECK(site.backend()->write(period.data(), format.period_frames, &error));
    CHECK(site.step_transmit(&error));
    CHECK(remote.step_receive(&error));
    CHECK(remote.backend()->read(played.data(), format.period_frames, &error));
    if (audio_bytes::s24_at(played.data(), 0) == 1000000) {
      ++periods_with_audio;
    }
  }
  // The audio arrived, at unity — not silence, and not something the clock mangled.
  // A non-blocking receive trails the sender by the link's latency window, so the
  // count is short by that window rather than exact -- the assertion below allows
  // for it, and does *not* drain: draining plays while nothing new arrives and
  // would starve the level the delay assertions just below are checking.
  CHECK(periods_with_audio > 150);
  // Nothing was lost and nothing was refused: every frame that arrived is in the
  // buffer, the converter or the device. The last `latency` frames sit in SRT's
  // buffer until the connection ticks past them, so this allows the window -- the
  // regression it guards against read one frame in twenty-five, not nine in ten.
  CHECK(remote.frames_received() > static_cast<uint64_t>(total_turns) - 200);
  CHECK_EQ(remote.frames_refused(), uint64_t{0});
  CHECK_EQ(remote.backend()->overruns(), 0u);
  // The clock did not stay silent: playout started once the level was up. The
  // count includes the link's latency window, where a non-blocking receiver has
  // nothing to play yet, so the bound is "not silent for the whole run" rather
  // than "silent only while priming".
  CHECK(remote.silence_periods() > 0u);
  CHECK(remote.silence_periods() < static_cast<uint64_t>(total_turns));
  // And the delay figure ticket 12 asks for is exposed, as a level the clock is
  // holding rather than one that collapsed or ran away.
  //
  // **The band is deliberately wide, and that is the test being honest about
  // load.** On a busy runner the level sits wherever the device pacing and the link
  // left it this run — decision 6 grows it when the device sags, and a non-blocking
  // receive starts low while the link's latency window fills — so a tight band here
  // asserts that CI is idle, which is not this test's business. What it *must*
  // catch is a buffer that emptied (0) or hit its ceiling, and that is what these
  // do; the exact figure is asserted where it is deterministic, in the clock's own
  // tests.
  CHECK(remote.delay_ms() > 1.0);
  CHECK(remote.delay_ms() < 2.0 * target_periods);
  CHECK(remote.delay_fraction() > 0.0);
  CHECK(remote.delay_fraction() < 1.0);
  // Two ends sharing one clock, so the true offset is zero and the correction has
  // no reason to be anywhere else.
  CHECK(std::fabs(remote.clock_offset_ppm()) < 10.0);
  CHECK_NEAR(remote.clock_ratio(), 1.0, 1e-4);

  std::cout << "    engine loopback through the clock: " << periods_with_audio
            << " periods of audio, " << remote.silence_periods()
            << " of silence, delay " << remote.delay_ms() << " ms, correction "
            << remote.clock_offset_ppm() << " ppm" << std::endl;

  site.stop();
  remote.stop();
}

TEST_CASE(engine_the_link_reconnects_after_the_caller_drops) {
  // The failure this ticket exists for: on the bench, a dropped connection left
  // the listener holding a dead socket for ever, and the only way back was to
  // restart both ends in the right order. Here the caller goes away and a *new*
  // caller connects, and the receiver's frame count must advance across the gap
  // with no restart of the site.
  if (!aes67_srt::transport::Link::available()) {
    std::cout << "  no libsrt in this build: skipping the reconnect test"
              << std::endl;
    return;
  }

  Config site_config = engine_config(1, "listener", 19601);
  site_config.link.role = "rx";
  Config remote_config = engine_config(1, "caller", 19602, "127.0.0.1:19601");
  remote_config.link.role = "tx";
  Config second_config = engine_config(1, "caller", 19603, "127.0.0.1:19601");
  second_config.link.role = "tx";

  Engine site;
  Engine first;
  Engine second;
  std::string error;
  CHECK(site.prepare(site_config, &error));
  CHECK(first.prepare(remote_config, &error));
  CHECK(second.prepare(second_config, &error));

  const auto wait_for_frames = [](Engine* engine, uint64_t wanted, int timeout_ms) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
      if (engine->frames_received() >= wanted) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return engine->frames_received() >= wanted;
  };

  // The listener blocks in accept, so run() goes on its own thread; the caller
  // connects a moment later, as the two-machine tests do.
  std::thread site_thread([&] { site.run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  std::thread first_thread([&] { first.run(); });
  CHECK(wait_for_frames(&site, 50, 15000));

  // The caller leaves. Nothing restarts the site: the supervisor must notice the
  // dead link and go back to accepting.
  first.stop();
  first_thread.join();
  std::this_thread::sleep_for(std::chrono::milliseconds(2000));

  const uint64_t before = site.frames_received();
  std::thread second_thread([&] { second.run(); });
  const bool recovered = wait_for_frames(&site, before + 50, 30000);

  second.stop();
  site.stop();
  second_thread.join();
  site_thread.join();

  CHECK(recovered);
  CHECK(site.frames_received() > before);
  std::cout << "    reconnect: " << before << " frames before the drop, "
            << site.frames_received() << " after reconnect" << std::endl;
}

TEST_CASE(engine_the_link_reconnects_under_full_load) {
  // Issue #32: the light 1-block reconnect test passed, but on the bench a
  // reconnect under full 64-channel load left the receiver draining at about 640
  // of the sender's 1000 frames a second, so the sender's buffer climbed and the
  // link flapped. The cause was a stale error string: the first hard failure after
  // a drop was never cleared, so every later step_receive returned false, slept the
  // retry delay, and skipped play_one_period. This measures the *rate*, which the
  // light test never did.
  if (!aes67_srt::transport::Link::available()) {
    std::cout << "  no libsrt in this build: skipping the loaded reconnect test"
              << std::endl;
    return;
  }

  Config site_config = engine_config(8, "listener", 19701);
  site_config.link.role = "rx";
  Config first_config = engine_config(8, "caller", 19702, "127.0.0.1:19701");
  first_config.link.role = "tx";
  Config second_config = engine_config(8, "caller", 19703, "127.0.0.1:19701");
  second_config.link.role = "tx";

  Engine site;
  Engine first;
  Engine second;
  std::string error;
  CHECK(site.prepare(site_config, &error));
  CHECK(first.prepare(first_config, &error));
  CHECK(second.prepare(second_config, &error));

  const auto wait_for_frames = [](Engine* engine, uint64_t wanted, int timeout_ms) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
      if (engine->frames_received() >= wanted) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return engine->frames_received() >= wanted;
  };
  const auto drain_rate = [](Engine* engine, int ms) {
    const uint64_t before = engine->frames_received();
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    return 1000.0 * static_cast<double>(engine->frames_received() - before) / ms;
  };

  std::thread site_thread([&] { site.run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  std::thread first_thread([&] { first.run(); });
  CHECK(wait_for_frames(&site, 500, 15000));

  const double rate_before = drain_rate(&site, 2000);

  // Drop the caller and connect a fresh one, exactly as a reboot does.
  first.stop();
  first_thread.join();
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));

  const uint64_t before = site.frames_received();
  std::thread second_thread([&] { second.run(); });
  CHECK(wait_for_frames(&site, before + 500, 30000));
  const double rate_after = drain_rate(&site, 2000);

  std::cout << "    loaded reconnect: " << rate_before << " frames/s before, "
            << rate_after << " after" << std::endl;

  second.stop();
  site.stop();
  second_thread.join();
  site_thread.join();

  // The sender offers ~1000 frames a second (one 1 ms period). The receiver must
  // keep up before *and after* a reconnect; the bench failure was ~640/s after.
  CHECK(rate_before > 700.0);
  CHECK(rate_after > 700.0);
}

TEST_CASE(engine_counts_the_silence_it_feeds_a_link_that_cannot_fill_the_clock) {
  // The case the two-machine run found: a link delivering far less than the
  // appliance offers. The clock primes past its deadline, plays what little there
  // is, and the device is fed silence for the rest — and the *counting* is the
  // point, because a level sitting high while the device gets silence and nothing
  // is counted is exactly the situation an operator cannot diagnose.
  //
  // One engine in loopback mode, so the whole receive path is exercised without a
  // second process or a thread.
  Config config = engine_config(8, "loopback", 0);
  Engine engine;
  std::string error;
  CHECK(engine.prepare(config, &error));
  CHECK(engine.open(&error));

  // One frame, and no more: less than the converter's working room needs to fill a
  // period, which is what makes this a *short* pull rather than a refusal.
  const AudioFormat format = aes67_srt::audio::audio_format_from(config.audio);
  const std::vector<uint8_t> period = audio_bytes::constant_period(format, 1000000);
  CHECK(engine.backend()->write(period.data(), format.period_frames, &error));
  CHECK(engine.step_transmit(&error));
  CHECK_EQ(engine.frames_sent(), uint64_t{1});

  // Turn the receive side past the priming deadline: the level never reaches its
  // target, so playout starts when the deadline expires rather than never.
  for (int turn = 0; turn < config.link.alarm_delay_ms + 20; ++turn) {
    CHECK(engine.step_receive(&error));
  }

  CHECK(engine.silence_periods() >
        static_cast<uint64_t>(config.link.alarm_delay_ms));
  CHECK(engine.delay_ms() <= 2.0);  // one frame of audio, and no more
  CHECK(engine.delay_fraction() < 0.01);
  std::cout << "    engine starved link: " << engine.silence_periods()
            << " periods of silence counted, delay " << engine.delay_ms() << " ms"
            << std::endl;

  engine.stop();
}

TEST_CASE(engine_applies_the_egress_offset_and_refuses_one_it_cannot_hold) {
  // Ticket 12's dial, through the engine rather than the module: the value the
  // control surface will set and the operator will read.
  Config config = engine_config(1, "loopback", 0);
  Engine engine;
  std::string error;
  CHECK(engine.prepare(config, &error));
  CHECK(engine.open(&error));

  CHECK(engine.set_egress_delay_ms(37.5, &error));
  CHECK_NEAR(engine.egress_delay_ms(), 37.5, 0.05);
  // Audio can be delayed, never advanced, and the line has a ceiling.
  CHECK(!engine.set_egress_delay_ms(-1.0, &error));
  CHECK(!error.empty());
  CHECK(!engine.set_egress_delay_ms(5001.0, &error));
  CHECK_NEAR(engine.egress_delay_ms(), 37.5, 0.05);

  engine.stop();
}

TEST_CASE(engine_the_impulse_lands_in_the_device_stream_where_it_was_asked) {
  // The test signal is nothing until it reaches the device, so this drives a whole
  // egress period and looks at what the device was handed. With an offset of zero
  // the delay line is a memcpy, so the impulse is exactly where it was fired.
  Config config = engine_config(1, "loopback", 0);
  config.egress.test_signal_channel = 2;
  Engine engine;
  std::string error;
  CHECK(engine.prepare(config, &error));
  CHECK(engine.open(&error));

  const AudioFormat format = aes67_srt::audio::audio_format_from(config.audio);
  CHECK(engine.trigger_test_signal(&error));
  CHECK(engine.step_receive(&error));

  std::vector<uint8_t> played(format.period_bytes(), 0);
  CHECK(engine.backend()->read(played.data(), format.period_frames, &error));
  CHECK_EQ(audio_bytes::s24_at(played.data(), 2), 0x7FFFFF);
  CHECK_EQ(audio_bytes::s24_at(played.data(), 0), 0);
  CHECK_EQ(audio_bytes::s24_at(played.data(), 1), 0);

  engine.stop();
}

TEST_CASE(engine_the_codec_contributes_nothing_to_the_av_budget_in_v1) {
  // Decision 9: v1 encodes nothing, so the codec's term is zero. It exists as a
  // figure because phase 2 changes it and the operator's alignment number has to
  // change with it. The A/V total is the sum of what each stage knows.
  Config config = engine_config(1, "loopback", 0);
  Engine engine;
  std::string error;
  CHECK(engine.prepare(config, &error));
  CHECK(engine.open(&error));

  CHECK_NEAR(engine.codec_delay_ms(), 0.0, 1e-12);
  CHECK_NEAR(engine.av_delay_ms(),
             static_cast<double>(config.link.latency_ms) + engine.delay_ms() +
                 engine.egress_delay_ms(),
             1e-9);

  CHECK(engine.set_egress_delay_ms(100.0, &error));
  CHECK_NEAR(engine.av_delay_ms(),
             static_cast<double>(config.link.latency_ms) + 100.0, 0.05);

  engine.stop();
}

TEST_CASE(engine_refuses_a_clock_it_could_not_fill) {
  // The clock's geometry comes from the link's own numbers, so a configuration
  // whose alarm is not comfortably above the latency it alarms about has no playout
  // buffer to build: a capacity at or below the target is a buffer that overruns
  // the first time a link sags, and that is a configuration error rather than a
  // runtime one.
  Config config = engine_config(8);
  config.link.latency_ms = 500;
  config.link.alarm_delay_ms =
      400;  // below the latency it is supposed to alarm about

  Engine engine;
  std::string error;
  CHECK(!engine.prepare(config, &error));
  CHECK(contains(error, "alarm_delay_ms"));
  CHECK(contains(error, "playout buffer"));
}

TEST_CASE(engine_delivers_the_full_rate_over_a_real_srt_link) {
  // The regression the in-process loopback cannot see. The receive loop is paced by
  // the device and owes it one period a millisecond; a *blocking* receive
  // multiplies its timeout across the drain loop and stalls that cadence, so a
  // socket delivers a few percent of what the sender offers while the loopback,
  // which never blocks, stays perfect. Measured on the routed path: 42 frames a
  // second with a 2 ms timeout, 999 with 0 (docs/research/libsrt.md).
  if (!aes67_srt::transport::Link::available() ||
      !aes67_srt::clock::Resampler::available()) {
    std::cout << "  no libsrt or no libsamplerate: skipping the full-rate link"
              << std::endl;
    return;
  }

  const Config site_config = engine_config(1, "listener", 19701);
  const Config remote_config = engine_config(1, "caller", 19702, "127.0.0.1:19701");
  Engine site;
  Engine remote;
  std::string error;
  CHECK(site.prepare(site_config, &error));
  CHECK(remote.prepare(remote_config, &error));

  std::atomic<int> site_result{-1};
  std::atomic<int> remote_result{-1};
  std::thread site_thread([&site, &site_result] { site_result = site.run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  std::thread remote_thread(
      [&remote, &remote_result] { remote_result = remote.run(); });
  // Wait for the sender to have run long enough that the ratio means something,
  // rather than for a fixed wall-clock window: a loaded runner can spend most of a
  // fixed sleep on link start-up, and a test that fails for being scheduled late
  // teaches nothing. The clock's own slow loop does not matter here -- this is
  // throughput, not the crystal.
  for (int attempt = 0; attempt < 500 && site.status().frames_sent < 1000;
       ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  site.stop();
  remote.stop();
  site_thread.join();
  remote_thread.join();

  CHECK_EQ(site_result.load(), 0);
  CHECK_EQ(remote_result.load(), 0);
  // Both ends are duplex and share this machine's clock, so the receiver should get
  // close to everything the sender sent. Half is a floor with room for start-up; a
  // blocking receive puts this near 4%.
  CHECK(site.frames_sent() > 500);
  CHECK(remote.frames_received() > site.frames_sent() / 2);
  CHECK(site.frames_received() > remote.frames_sent() / 2);

  std::cout << "    engine full-rate link: site sent " << site.frames_sent()
            << ", remote received " << remote.frames_received() << std::endl;
}

TEST_CASE(engine_the_status_snapshot_is_readable_while_the_loops_run) {
  // The control surface polls from its own thread while the device-paced loops run,
  // so the snapshot is read concurrently on purpose: this exercises the race it
  // exists to prevent rather than describing it.
  Config config = engine_config(1, "loopback", 0);
  Engine engine;
  std::string error;
  CHECK(engine.prepare(config, &error));

  int result = -1;
  std::thread running([&engine, &result] { result = engine.run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  uint64_t previous_sent = 0;
  bool saw_running = false;
  for (int i = 0; i < 50; ++i) {
    EngineStatus status = engine.status();
    saw_running = saw_running || status.running;
    CHECK(status.frames_sent >= previous_sent);
    previous_sent = status.frames_sent;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  CHECK(saw_running);

  engine.stop();
  running.join();
  CHECK_EQ(result, 0);

  const EngineStatus final_status = engine.status();
  CHECK(!final_status.running);
  CHECK(final_status.frames_sent > 0);
  // A loopback has no link statistics, and the snapshot says so rather than zeroes.
  CHECK(!final_status.link_stats_available);
}

TEST_CASE(engine_says_once_that_a_loopback_has_no_link_statistics) {
  // The status thread (ticket 09's missing diagnostic) runs whichever directions
  // this end has, and it must report the absence of statistics rather than stay
  // silent — a loopback is a legitimate configuration, so the log says why there
  // is no RTT rather than leaving an operator to wonder.
  Config config = engine_config(1, "loopback", 0);
  Engine engine;
  std::string error;
  CHECK(engine.prepare(config, &error));

  int result = -1;
  std::thread running([&engine, &result] { result = engine.run(); });
  // Poll rather than sleep a fixed interval: a loaded CI runner can push the
  // status thread's first turn past any constant, and a test that fails for being
  // scheduled late teaches nothing. The marker is the loopback reason, which is
  // unique to this path.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
  bool found = false;
  while (!found && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    for (const std::string& line : aes67_srt::log().tail(200)) {
      if (line.find("link statistics:") != std::string::npos &&
          line.find("loopback") != std::string::npos) {
        found = true;
      }
    }
  }
  engine.stop();
  running.join();

  CHECK_EQ(result, 0);
  CHECK(found);
}

TEST_CASE(engine_applies_the_block_gain_to_what_it_sends) {
  // The pure function is tested where it lives; this proves the *engine* calls it
  // on the transmit path, across a real SRT link, which is the wiring a unit test
  // cannot see.
  if (!aes67_srt::transport::Link::available() ||
      !aes67_srt::clock::Resampler::available()) {
    std::cout << "  no libsrt or no libsamplerate: skipping the gain link"
              << std::endl;
    return;
  }
  Config site_config = engine_config(1, "listener", 19711);
  site_config.blocks[0].gain_db = -6.0206;  // half, to within rounding
  Config remote_config = engine_config(1, "caller", 19712, "127.0.0.1:19711");
  Engine site;
  Engine remote;
  std::string error;
  CHECK(site.prepare(site_config, &error));
  CHECK(remote.prepare(remote_config, &error));

  std::atomic<bool> site_up{false};
  std::string site_error;
  std::thread accepting(
      [&site, &site_up, &site_error] { site_up = site.open(&site_error); });
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  const bool remote_up = remote.open(&error);
  accepting.join();
  CHECK(remote_up);
  CHECK(site_up.load());
  if (!remote_up || !site_up.load()) {
    test::report_failure("the gain link did not come up", __FILE__, __LINE__,
                         error + " / " + site_error);
    return;
  }

  const AudioFormat format = aes67_srt::audio::audio_format_from(site_config.audio);
  const std::vector<uint8_t> period = audio_bytes::constant_period(format, 1000000);
  std::vector<uint8_t> played(format.period_bytes(), 0);
  int halved = 0;
  const int turns = site_config.link.latency_ms + 200;
  for (int turn = 0; turn < turns; ++turn) {
    CHECK(site.backend()->write(period.data(), format.period_frames, &error));
    CHECK(site.step_transmit(&error));
    CHECK(remote.step_receive(&error));
    CHECK(remote.backend()->read(played.data(), format.period_frames, &error));
    const int32_t value = audio_bytes::s24_at(played.data(), 0);
    if (value > 480000 && value < 520000) {
      ++halved;
    }
  }
  // Drain the latency window (a non-blocking receive trails the sender by it) so
  // the played frames can be counted, then check the trim reached the wire.
  for (int extra = 0;
       extra < 400 && remote.frames_received() + 10 < site.frames_sent(); ++extra) {
    CHECK(remote.step_receive(&error));
    CHECK(remote.backend()->read(played.data(), format.period_frames, &error));
    const int32_t value = audio_bytes::s24_at(played.data(), 0);
    if (value > 480000 && value < 520000) {
      ++halved;
    }
  }
  // A gain of -6.02 dB is a factor of 0.5, so 1,000,000 arrives as ~500,000 --
  // which is the whole point: the operator's trim reaches the wire.
  CHECK(halved > 100);

  site.stop();
  remote.stop();
}
