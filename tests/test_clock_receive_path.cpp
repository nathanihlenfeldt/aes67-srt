#include "clock/playout_buffer.hpp"
#include "clock/ratio_control.hpp"
#include "clock/resampler.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "audio/backend.hpp"
#include "clock_simulation.hpp"
#include "test_framework.hpp"

/**
 * The three pieces of the clock, together, for the first time.
 *
 * **Why this file exists.** Each piece was tested on its own, and each of those
 * tests used a *model* of the others: the loop's simulation drove a stand-in that
 * consumed `frames * control.ratio()` of input per output period — which is my
 * assumption about what a resampler does, written inside the test — while the
 * resampler's tests set a ratio by hand and never asked the control for one. So the
 * control's unit convention was only ever asserted against my model of a resampler,
 * and the resampler's reciprocal only against my assumption about the control. The
 * one relation this module cannot afford to get wrong — `src_ratio = 1 /
 * control.ratio()` — had never been exercised *across* the two.
 *
 * That is circular verification, and it is the failure this project keeps meeting.
 * Here the real resampler *is* the plant: the control's ratio goes into it, whole
 * periods come out of it, and the loop's stability is measured against the
 * library's own consumption — which increment 3 measured to be *lumpy* (48 or 96
 * input frames per pull, with 48–96 frames of working room) in a way the stand-in
 * never was.
 *
 * **What the stand-in runs still cover.** `test_clock_ratio.cpp` runs four
 * simulated hours at the shipped loop period, which costs nothing because its plant
 * does no filtering. This file runs thirty minutes with the real library: enough
 * for a loop whose period is 2000 s to converge, and as much as a unit test should
 * spend. The hours stay where the hours are affordable; the library is where the
 * library is real.
 */
namespace {

using aes67_srt::audio::AudioFormat;
using aes67_srt::clock::PlayoutBuffer;
using aes67_srt::clock::PushStatus;
using aes67_srt::clock::RatioControl;
using aes67_srt::clock::Resampler;
using clock_sim::Pace;
using clock_sim::sim_format;

/** The 24-bit value of one sample of a period of bytes. */
int32_t s24_at(const uint8_t* bytes, size_t sample) {
  const uint8_t* at = bytes + sample * 3;
  int32_t value = static_cast<int32_t>(at[0]) | (static_cast<int32_t>(at[1]) << 8) |
                  (static_cast<int32_t>(at[2]) << 16);
  if ((value & 0x800000) != 0) {
    value |= ~0xFFFFFF;
  }
  return value;
}

/** A period whose every sample is |value|. */
std::vector<uint8_t> constant_period(const AudioFormat& format, int32_t value) {
  std::vector<uint8_t> period(format.period_bytes(), 0);
  for (size_t sample = 0; sample < format.period_frames * format.channels;
       ++sample) {
    uint8_t* at = period.data() + sample * 3;
    at[0] = static_cast<uint8_t>(value & 0xff);
    at[1] = static_cast<uint8_t>((value >> 8) & 0xff);
    at[2] = static_cast<uint8_t>((value >> 16) & 0xff);
  }
  return period;
}

/** What the joined path did. */
struct PathReport {
  bool opened = false;
  std::string error;
  uint64_t pulls = 0;
  uint64_t underruns = 0;
  uint64_t short_pulls = 0;
  uint64_t overruns = 0;
  uint64_t dropped_frames = 0;
  /** True when the reported position ever moved backwards, which would be audio
   *  played twice. */
  bool position_went_backwards = false;
  /** The audio's amplitude, as a fraction of what was pushed: the whole path has to
   *  put real audio out, not silence and not garbage. */
  double loudest = 0.0;
  double quietest = 1.0;
  /** The ledger, which has to balance to the frame across the module boundary. */
  uint64_t buffer_played = 0;
  uint64_t converter_took = 0;
  uint64_t still_pending = 0;
  uint64_t buffer_pushed = 0;
  uint64_t buffer_held = 0;
  double level_min_ms = 0.0;
  double level_max_ms = 0.0;
  double level_first_ms = 0.0;
  double level_last_ms = 0.0;
  /** The largest distance from the target after the settling window. */
  double band_ms = 0.0;
  double ppm_last = 0.0;
  uint64_t updates_at_limit = 0;
};

/**
 * The receive path as the engine will drive it: one millisecond of our clock per
 * turn, a period pushed for every millisecond of the sender's, and the control's
 * ratio handed to the resampler before every pull.
 *
 * The converter is a parameter only so the tests can use the cheapest one: the
 * loop's arithmetic does not know or care which filter is behind it, and increment
 * 3 measured that choice separately.
 */
PathReport run_path(size_t channels, uint64_t sender_ppm, uint64_t receiver_ppm,
                    uint64_t milliseconds, uint64_t prime_ms, double target_ms,
                    size_t capacity_periods, Resampler::Converter converter) {
  PathReport report;
  const AudioFormat format = sim_format(static_cast<unsigned>(channels));

  PlayoutBuffer buffer(capacity_periods, format);
  RatioControl::Config control_config;
  control_config.target_ms = target_ms;
  RatioControl control(control_config);
  Resampler resampler;
  Resampler::Config resampler_config;
  resampler_config.channels = static_cast<unsigned>(channels);
  resampler_config.period_frames = format.period_frames;
  resampler_config.converter = converter;
  if (!resampler.open(resampler_config, &report.error)) {
    return report;
  }
  report.opened = true;
  resampler.set_ratio(control.ratio());

  Pace sender;
  Pace receiver;
  sender.ppm = sender_ppm;
  receiver.ppm = receiver_ppm;

  std::vector<uint8_t> out(format.period_bytes(), 0);
  /** What the sender is carrying: a constant, so the whole path's output can be
   *  checked against it without a signal model. A resampler passes DC at unity. */
  const int32_t pushed_value = 1000000;
  const std::vector<uint8_t> constant = constant_period(format, pushed_value);
  uint64_t sender_position = 0;
  uint64_t last_position = 0;
  bool started = false;
  bool playing = false;
  const uint64_t settle_from_ms = 10 * 60 * 1000;

  for (uint64_t ms = 0; ms < milliseconds; ++ms) {
    for (uint64_t tick = 0, ticks = sender.advance(); tick < ticks; ++tick) {
      const PushStatus status =
          buffer.push(sender_position, constant.data(), constant.size());
      if (status == PushStatus::overrun) {
        ++report.overruns;
      }
      sender_position += format.period_frames;
    }

    for (uint64_t tick = 0, ticks = receiver.advance(); tick < ticks; ++tick) {
      if (!playing) {
        if (ms < prime_ms) {
          continue;
        }
        playing = true;
      }

      // The join, in one line, and the only place the two modules meet: what the
      // control asks for is what the resampler is told to do.
      resampler.set_ratio(control.ratio());

      uint64_t position = 0;
      if (resampler.pull(&buffer, out.data(), format.period_frames, &position,
                         &report.error)) {
        ++report.pulls;
        if (!started) {
          started = true;
          last_position = position;
        }
        if (position < last_position) {
          report.position_went_backwards = true;
        }
        last_position = position;
        // The audio itself, in the middle of the period where the filter has
        // settled: real audio at the value the sender is sending, not silence and
        // not garbage.
        if (report.pulls > 1000) {
          const double value =
              static_cast<double>(s24_at(out.data(), 24 * channels));
          const double relative = value / static_cast<double>(pushed_value);
          if (relative > report.loudest) {
            report.loudest = relative;
          }
          if (relative < report.quietest) {
            report.quietest = relative;
          }
        }
      } else {
        ++report.underruns;
      }

      const double level = buffer.level_ms();
      control.update(level, 1.0);
      if (control.at_limit()) {
        ++report.updates_at_limit;
      }
      if (report.pulls == 0) {
        continue;  // nothing has played yet, so there is no level to speak of
      }
      if (report.pulls == 1) {
        report.level_first_ms = level;
        report.level_min_ms = level;
        report.level_max_ms = level;
      }
      if (level < report.level_min_ms) {
        report.level_min_ms = level;
      }
      if (level > report.level_max_ms) {
        report.level_max_ms = level;
      }
      report.level_last_ms = level;
      report.ppm_last = control.offset_ppm();
      if (ms >= settle_from_ms) {
        const double away = std::fabs(level - target_ms);
        if (away > report.band_ms) {
          report.band_ms = away;
        }
      }
    }
  }

  report.short_pulls = resampler.short_pulls();
  report.buffer_played = buffer.frames_played();
  report.buffer_pushed = buffer.frames_pushed();
  report.buffer_held = buffer.held_frames();
  report.converter_took = resampler.input_frames();
  report.still_pending = resampler.pending_frames();
  report.dropped_frames = buffer.frames_dropped();
  return report;
}

/**
 * The claims the joined path makes, at either sign of the offset.
 *
 * **What is *not* asserted here, and why.** The first version of this test checked
 * that the played positions advanced by exactly one period, and that every output
 * period's bytes matched the sender's for that position. Both are false for a
 * *resampling* path, and the failures are what taught me the right metric: at a
 * ratio of 1.00001 the input position advances by the ratio's worth of frames —
 * sometimes 48, sometimes 96, occasionally less — and the samples have been through
 * an interpolating filter, so they are not the sender's bytes. Neither is a fault.
 * The correct loss metric across this path is the *ledger*, below, and the position
 * is only checked for monotonicity: audio played twice would show as a position
 * going backwards.
 */
void check_path(const PathReport& report, double truth_ppm) {
  CHECK(report.opened);
  if (!report.opened) {
    std::cout << "    " << report.error << std::endl;
    return;
  }
  // Nothing is lost, invented, or left unplayed by any of the three pieces.
  CHECK_EQ(report.underruns, 0u);
  CHECK_EQ(report.short_pulls, 0u);
  CHECK_EQ(report.overruns, 0u);
  CHECK_EQ(report.dropped_frames, 0u);
  CHECK_EQ(report.updates_at_limit, 0u);
  CHECK(!report.position_went_backwards);
  CHECK(report.pulls > 1000000u);  // half an hour of periods, less the priming
  // The buffer's own ledger: what the sender delivered is what was played plus what
  // is still held, with nothing dropped.
  CHECK_EQ(report.buffer_pushed,
           report.buffer_played + report.buffer_held + report.dropped_frames);
  // And the ledger across the module boundary: every frame the buffer handed to the
  // resampler was either consumed by the converter or is still in its carry. A
  // frame lost between the two would be audio lost with nothing in the log.
  CHECK_EQ(report.buffer_played, report.converter_took + report.still_pending);
  // The audio that came out is the audio that went in: the whole path carried a
  // constant at unity, so anything from silence to a broken buffer shows here.
  CHECK(report.quietest > 0.98);
  CHECK(report.loudest < 1.02);
  // The level is held, and the correction has found the offset. That second
  // assertion is the one that fails if the control's ratio and the resampler's
  // src_ratio disagree about which way round they go: the loop drives the level to
  // a rail and the correction to its clamp.
  CHECK(report.band_ms < 6.0);
  CHECK(std::fabs(report.level_last_ms - report.level_first_ms) < 4.0);
  CHECK_NEAR(report.ppm_last, truth_ppm, 1.5);
}

}  // namespace

TEST_CASE(clock_the_receive_path_holds_the_level_with_the_real_resampler) {
  if (!Resampler::available()) {
    std::cout
        << "    no libsamplerate in this build: skipping the joined receive path"
        << std::endl;
    return;
  }

  // Half an hour of simulated audio, which is what a loop with a
  // two-thousand-second period needs to have found the offset and settled. Inside
  // that time the real library consumes in lumps and holds a working room; the
  // stand-in in test_clock_ratio.cpp does neither, so this is the run that says the
  // loop survives contact with the thing it will actually drive.
  const PathReport faster = run_path(4, /*sender_ppm=*/10, /*receiver_ppm=*/0,
                                     30ull * 60 * 1000, /*prime_ms=*/120,
                                     /*target_ms=*/120.0, /*capacity_periods=*/500,
                                     Resampler::Converter::sinc_fastest);
  check_path(faster, 10.0);

  std::cout << "    receive path: 30 min at +10 ppm — level "
            << faster.level_first_ms << " -> " << faster.level_last_ms
            << " ms, band +/-" << faster.band_ms << " ms, correction "
            << faster.ppm_last << " ppm, " << faster.pulls
            << " periods played, ledger " << faster.buffer_played << " = "
            << faster.converter_took << " + " << faster.still_pending << std::endl;
}

TEST_CASE(clock_the_receive_path_holds_the_level_from_the_other_side_too) {
  if (!Resampler::available()) {
    std::cout
        << "    no libsamplerate in this build: skipping the joined receive path"
        << std::endl;
    return;
  }

  // The same run with the faster clock at *our* end, so the correction has to go
  // the other way and the level is falling rather than filling. A sign error
  // anywhere in the join passes one of these two runs and fails the other, which is
  // why both are here.
  const PathReport slower = run_path(4, /*sender_ppm=*/0, /*receiver_ppm=*/10,
                                     30ull * 60 * 1000, /*prime_ms=*/120,
                                     /*target_ms=*/120.0, /*capacity_periods=*/500,
                                     Resampler::Converter::sinc_fastest);
  check_path(slower, -10.0);

  std::cout << "    receive path: 30 min at -10 ppm — level "
            << slower.level_first_ms << " -> " << slower.level_last_ms
            << " ms, band +/-" << slower.band_ms << " ms, correction "
            << slower.ppm_last << " ppm" << std::endl;
}