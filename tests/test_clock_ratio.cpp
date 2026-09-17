#include "clock/ratio_control.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "audio/backend.hpp"
#include "clock/playout_buffer.hpp"
#include "clock_simulation.hpp"
#include "test_framework.hpp"

/**
 * The ratio control, driven against the playout buffer (ticket 11, issue #12;
 * increment 2 of the clock module).
 *
 * The loop steers the **buffer level**, which is the decision
 * `docs/research/clock-recovery.md` records: the level *is* the integral of the
 * rate error, so it needs no timing estimate and no figure for the arrival
 * jitter that nobody has measured. This test is therefore the whole of the
 * loop's evidence: two clocks a few ppm apart, hours of simulated audio, and the
 * level held while nothing is lost or duplicated.
 *
 * **What stands in for the resampler.** The device consumes one period per one of
 * its own milliseconds, and the input the resampler must consume to fill it is
 * `period_frames x ratio` — so the loop's ratio is what sets how fast periods
 * leave the buffer, exactly as it will when libsamplerate sits behind it in
 * increment 3. The consumption is in whole periods because the buffer's API is
 * (increment 1's design), and that is what puts the loop's measurement on a
 * 1 ms grid.
 */
namespace {

using aes67_srt::audio::AudioFormat;
using aes67_srt::clock::PlayoutBuffer;
using aes67_srt::clock::PushStatus;
using aes67_srt::clock::RatioControl;
using clock_sim::check_clean;
using clock_sim::Jitter;
using clock_sim::k_tag_mask;
using clock_sim::make_period;
using clock_sim::Pace;
using clock_sim::position_of;
using clock_sim::sim_format;

/**
 * A simulated run: the two clocks, the buffer, the loop, and a link event to
 * survive.
 */
struct Plan {
  uint64_t sender_ppm = 0;
  uint64_t receiver_ppm = 0;
  size_t capacity_periods = 500;
  uint64_t milliseconds = 3600ull * 1000;
  /** How long the receiver waits before playing, which is what primes the level. */
  uint64_t prime_ms = 120;
  /** A burst: the sender's clock runs this much faster for |burst_ms|, which
   *  moves the level the way a link hiccup does. */
  uint64_t burst_ppm = 0;
  uint64_t burst_at_ms = 0;
  uint64_t burst_ms = 0;
  bool verify_periods = false;
  /** A diagnostic trace of the loop, every |trace_ms|, when non-zero. */
  uint64_t trace_ms = 0;
  /** The largest arrival delay to add, in milliseconds — the link's spread. Zero
   *  is a link that delivers on a metronome, which no link does. */
  uint64_t jitter_ms = 0;
  RatioControl::Config control;
  /** How close to the true offset counts as converged, for |off_truth_last_ms|. */
  double truth_tolerance_ppm = 0.3;
  /** The run's band is measured from this point, so that a loop still settling —
   *  or still answering a deliberate disturbance — is not mistaken for one that
   *  cannot hold a level. */
  double settle_from_ms = 0.0;

  /** The offset the two clocks really have, as the loop's correction reads it:
   *  positive when the sender's clock is the faster. */
  double truth_ppm() const {
    return static_cast<double>(sender_ppm) - static_cast<double>(receiver_ppm);
  }
};

/**
 * What a run did, so each test asserts on numbers rather than on a proxy for
 * them.
 */
struct LoopReport {
  uint64_t took = 0;
  uint64_t underruns = 0;
  uint64_t overruns = 0;
  uint64_t lost_frames = 0;
  uint64_t repeated_frames = 0;
  uint64_t mismatched_periods = 0;
  uint64_t late = 0;
  uint64_t duplicates = 0;
  uint64_t refused = 0;
  uint64_t frames_pushed = 0;
  uint64_t frames_played = 0;
  uint64_t frames_held = 0;
  uint64_t frames_dropped = 0;
  uint64_t milliseconds = 0;

  double level_min_ms = 0.0;
  double level_max_ms = 0.0;
  double level_first_ms = 0.0;
  double level_last_ms = 0.0;
  /** The largest distance from the target after |settle_from_ms|, and from the
   *  moment playout began. */
  double band_ms = 0.0;
  double band_from_start_ms = 0.0;
  double settle_from_ms = 0.0;
  double ppm_min = 0.0;
  double ppm_max = 0.0;
  double ppm_last = 0.0;
  /** The fastest the ratio moved, in ppm per second. */
  double max_slew_ppm_second = 0.0;
  /** The last update at which the correction was further than |tolerance| from
   *  the true offset; 0 when it never was. */
  double off_truth_last_ms = 0.0;
  uint64_t updates_at_limit = 0;
};

/**
 * The clocks, the buffer and the loop, for |plan|'s milliseconds of audio.
 *
 * The receiver's take rate is the loop's ratio: the device wants one period per
 * period (48 output frames), and producing them consumes `48 x ratio` input
 * frames, so a ratio above one drains the buffer — which is exactly what stops
 * the level rising when the sender's clock is the faster. A ratio below one
 * leaves the level rising, which is how the loop refills a buffer that started
 * below its target.
 */
LoopReport run_loop(const AudioFormat& format, const Plan& plan) {
  PlayoutBuffer buffer(plan.capacity_periods, format);
  RatioControl control(plan.control);
  Pace sender;
  Pace receiver;
  sender.ppm = plan.sender_ppm;
  receiver.ppm = plan.receiver_ppm;

  LoopReport report;
  report.settle_from_ms = plan.settle_from_ms;
  std::vector<uint8_t> arriving(format.period_bytes(), 0);
  std::vector<uint8_t> taken(format.period_bytes(), 0);
  std::vector<uint8_t> expected(format.period_bytes(), 0);
  uint64_t sender_position = 0;
  uint64_t next_expected = 0;
  /** Input frames the resampler owes the device for the periods already played. */
  double input_need = 0.0;
  /** Periods the link has not delivered yet: (the millisecond it is due, its
   *  position). Empty when there is no jitter. */
  std::vector<std::pair<uint64_t, uint64_t>> in_flight;
  uint64_t last_due = 0;
  Jitter jitter;
  bool playing = false;
  bool tracking = false;
  bool started = false;
  const double frames = static_cast<double>(format.period_frames);

  /** One period reaches the buffer: the whole of what a link does with it. */
  const auto deliver = [&](uint64_t position) {
    arriving = make_period(format, position);
    const PushStatus status =
        buffer.push(position, arriving.data(), arriving.size());
    if (status == PushStatus::overrun) {
      ++report.overruns;
    } else if (status == PushStatus::late) {
      ++report.late;
    } else if (status == PushStatus::duplicate) {
      ++report.duplicates;
    } else if (status != PushStatus::stored) {
      ++report.refused;
    }
  };

  for (uint64_t ms = 0; ms < plan.milliseconds; ++ms) {
    report.milliseconds = ms;
    if (plan.burst_ppm != 0 && ms == plan.burst_at_ms) {
      sender.ppm = plan.burst_ppm;
    }
    if (plan.burst_ppm != 0 && ms == plan.burst_at_ms + plan.burst_ms) {
      sender.ppm = plan.sender_ppm;
    }

    for (uint64_t tick = 0, ticks = sender.advance(); tick < ticks; ++tick) {
      // The transport delivers in order — SRT is a reliable ordered stream — so a
      // jittered arrival is clamped to be no earlier than the one before it, and
      // the spread shows up as a delay rather than as a reordering.
      uint64_t due = ms + jitter.next(plan.jitter_ms);
      if (due < last_due) {
        due = last_due;
      }
      last_due = due;
      if (plan.jitter_ms == 0) {
        deliver(sender_position);
      } else {
        in_flight.push_back({due, sender_position});
      }
      sender_position += format.period_frames;
    }
    if (!in_flight.empty()) {
      for (size_t index = 0; index < in_flight.size();) {
        if (in_flight[index].first <= ms) {
          deliver(in_flight[index].second);
          in_flight.erase(in_flight.begin() + static_cast<long>(index));
        } else {
          ++index;
        }
      }
    }

    for (uint64_t tick = 0, ticks = receiver.advance(); tick < ticks; ++tick) {
      if (!playing) {
        if (ms < plan.prime_ms) {
          continue;  // a receiver waiting for its buffer to fill
        }
        playing = true;
      }

      input_need += frames * control.ratio();
      while (input_need >= frames) {
        input_need -= frames;
        uint64_t position = 0;
        if (!buffer.take(taken.data(), &position)) {
          // The device is fed either way and nothing was consumed, so the
          // fraction already in use is kept but the whole periods owed are
          // forgiven: a debt accumulated through a starvation would have the
          // loop correcting a deficit that no longer exists.
          ++report.underruns;
          input_need = std::fmod(input_need, frames);
          continue;
        }
        if (!started) {
          started = true;
          next_expected = position;
        }
        if (position == next_expected) {
          next_expected = position + format.period_frames;
        } else if (position > next_expected) {
          report.lost_frames += position - next_expected;
          next_expected = position + format.period_frames;
        } else {
          report.repeated_frames += next_expected - position;
        }
        ++report.took;
        if (plan.verify_periods || report.took % 1000 == 0) {
          if (position_of(taken.data()) != (position & k_tag_mask)) {
            ++report.mismatched_periods;
          }
          expected = make_period(format, position);
          if (expected != taken) {
            ++report.mismatched_periods;
          }
        }
      }

      const double before = control.offset_ppm();
      const double level = buffer.level_ms();
      control.update(level, 1.0);
      const double slew = std::fabs(control.offset_ppm() - before) * 1000.0;
      if (slew > report.max_slew_ppm_second) {
        report.max_slew_ppm_second = slew;
      }
      if (control.at_limit()) {
        ++report.updates_at_limit;
      }
      if (std::fabs(control.offset_ppm() - plan.truth_ppm()) >
          plan.truth_tolerance_ppm) {
        report.off_truth_last_ms = static_cast<double>(ms);
      }

      if (!tracking) {
        tracking = true;
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
      if (control.offset_ppm() < report.ppm_min) {
        report.ppm_min = control.offset_ppm();
      }
      if (control.offset_ppm() > report.ppm_max) {
        report.ppm_max = control.offset_ppm();
      }
      report.ppm_last = control.offset_ppm();

      const double away = std::fabs(level - plan.control.target_ms);
      if (away > report.band_from_start_ms) {
        report.band_from_start_ms = away;
      }
      if (static_cast<double>(ms) >= report.settle_from_ms &&
          away > report.band_ms) {
        report.band_ms = away;
      }
      if (plan.trace_ms != 0 && ms % plan.trace_ms == 0) {
        std::cerr << "      t=" << (ms / 1000) << " s level=" << level
                  << " avg=" << control.average_level_ms()
                  << " ppm=" << control.offset_ppm() << std::endl;
      }
    }
  }

  report.frames_pushed = buffer.frames_pushed();
  report.frames_played = buffer.frames_played();
  report.frames_held = buffer.held_frames();
  report.frames_dropped = buffer.frames_dropped();
  return report;
}

}  // namespace

TEST_CASE(clock_the_loop_holds_the_level_with_nothing_to_steer_from) {
  // Before it has seen a level the loop has no opinion, and an update it cannot
  // time is ignored rather than integrated as zero: a caller that cannot say how
  // long it has been must not move the ratio.
  RatioControl control;
  CHECK(!control.primed());
  CHECK_NEAR(control.offset_ppm(), 0.0, 1e-12);
  CHECK_NEAR(control.ratio(), 1.0, 1e-12);

  // The first update the loop can time is the one it believes, and the average
  // starts where the buffer is rather than at zero — so a receiver that starts at
  // its target sees no error at all and the ratio does not move.
  control.update(120.0, 1.0);
  CHECK(control.primed());
  CHECK_EQ(control.updates(), 1u);
  CHECK_NEAR(control.average_level_ms(), 120.0, 1e-12);
  CHECK_NEAR(control.level_error_ms(), 0.0, 1e-12);
  CHECK_NEAR(control.offset_ppm(), 0.0, 1e-12);

  // An update with no interval, or none that is a number, is not a level reading:
  // the ratio does not move and the window does not forget.
  control.update(400.0, 0.0);
  control.update(400.0, -1.0);
  control.update(std::nan(""), 1.0);
  CHECK_EQ(control.updates(), 1u);
  CHECK_NEAR(control.average_level_ms(), 120.0, 1e-12);
  CHECK_NEAR(control.offset_ppm(), 0.0, 1e-12);

  // And neither can a single stray reading, however far off it is: the update is a
  // millisecond and the window is a hundred seconds, so the level the loop acts on
  // moves by 280/100000 ms — and the proportional term turns that into
  // `proportional_ppm_per_ms() * 280 / 100000`, which is 0.014 ppm: a hundredth of
  // a thousandth of a cent of pitch, and a level drift of a ten thousandth of a
  // microsecond a second.
  control.update(400.0, 1.0);
  CHECK(control.average_level_ms() < 120.01);
  CHECK_NEAR(control.offset_ppm(),
             control.proportional_ppm_per_ms() * 280.0 / 100000.0, 1e-6);
  CHECK(control.offset_ppm() < 0.02);
}

TEST_CASE(clock_a_level_above_the_target_speeds_the_resampler_up) {
  // The sign, which is the one thing a loop cannot get wrong quietly: a full
  // buffer means the sender is delivering faster than we consume, so the
  // correction has to raise the ratio — consuming more input per output frame is
  // what drains it. Get this backwards and the loop runs away from the target
  // instead of toward it.
  RatioControl::Config config;
  config.average_ms = 1000.0;  // fast enough to see a step inside one test
  RatioControl control(config);

  control.update(120.0, 1.0);
  CHECK_NEAR(control.offset_ppm(), 0.0, 1e-12);

  // A level above the target, held for long enough to be averaged in.
  for (int update = 0; update < 100000; ++update) {
    control.update(220.0, 1.0);
  }
  CHECK(control.offset_ppm() > 0.0);
  CHECK(control.ratio() > 1.0);
  CHECK_NEAR(control.ratio(), 1.0 + control.offset_ppm() * 1e-6, 1e-15);
  CHECK(control.level_error_ms() > 0.0);

  // And the other way: a level below the target slows the resampler down, which
  // lets the buffer refill.
  RatioControl below(config);
  below.update(120.0, 1.0);
  for (int update = 0; update < 100000; ++update) {
    below.update(20.0, 1.0);
  }
  CHECK(below.offset_ppm() < 0.0);
  CHECK(below.ratio() < 1.0);
}

TEST_CASE(clock_the_loop_stops_at_its_limits_rather_than_running_away) {
  // Two safety nets, both of them things an operator would see rather than an
  // audio fault: the correction is clamped, and its integral cannot step. A ratio
  // left to run away is a pitch shift, and a step in the ratio is a step in the
  // audio.
  RatioControl::Config config;
  config.average_ms = 1.0;  // the measurement is the level itself, for this test
  config.loop_period_ms = 1000.0;
  config.limit_ppm = 50.0;
  config.slew_ppm_second = 2.0;
  RatioControl control(config);

  control.update(120.0, 1.0);
  CHECK(!control.at_limit());

  // A level step of a whole target, held: the loop wants far more than its clamp.
  for (int update = 0; update < 200000; ++update) {
    control.update(400.0, 1.0);
  }
  CHECK_EQ(control.offset_ppm(), 50.0);
  CHECK(control.at_limit());
  CHECK_NEAR(control.ratio(), 1.0 + 50e-6, 1e-12);

  // Unclamped, the *integral's* step is bounded by the slew rather than by the size
  // of the error: one second of a 580 ms error would move it by the gain's own
  // worth, and it moves by two ppm instead. Damping is taken out here because the
  // proportional term is algebraic and would swamp the thing being measured.
  RatioControl::Config loose = config;
  loose.limit_ppm = 100000.0;
  loose.slew_ppm_second = 2.0;
  loose.damping = 0.0;
  RatioControl slewed(loose);
  slewed.update(120.0, 1.0);
  for (int update = 0; update < 3; ++update) {
    slewed.update(700.0, 1000.0);  // one second of a 580 ms error at once
  }
  CHECK_NEAR(slewed.offset_ppm(), 3.0 * loose.slew_ppm_second, 1e-9);
}

TEST_CASE(clock_the_gains_are_the_loop_period_and_damping_it_was_given) {
  // The derivation, asserted rather than trusted. The plant is an integrator with
  // a gain of exactly 1/1000 ms/s per ppm, so a decade above and below the
  // defaults the two gains have to follow: quadruple the loop's frequency and the
  // integral gain goes up sixteen-fold while the proportional gain only doubles.
  // And damping scales the proportional gain alone, which is why a loop that rings
  // is fixed by one number and not by two.
  RatioControl::Config config;
  config.loop_period_ms = 2000000.0;
  config.damping = 0.8;
  const RatioControl base(config);

  // omega = 2*pi/2000 s = 0.0031416 rad/s.
  CHECK_NEAR(base.integral_ppm_per_ms_second(), 0.0098696, 1e-6);
  CHECK_NEAR(base.proportional_ppm_per_ms(), 5.0265, 1e-3);

  RatioControl::Config faster = config;
  faster.loop_period_ms = 1000000.0;  // twice the frequency
  const RatioControl twice(faster);
  CHECK_NEAR(twice.integral_ppm_per_ms_second(),
             4.0 * base.integral_ppm_per_ms_second(), 1e-9);
  CHECK_NEAR(twice.proportional_ppm_per_ms(), 2.0 * base.proportional_ppm_per_ms(),
             1e-9);

  RatioControl::Config critical = config;
  critical.damping = 1.6;  // twice the damping, same frequency
  const RatioControl damped(critical);
  CHECK_NEAR(damped.integral_ppm_per_ms_second(), base.integral_ppm_per_ms_second(),
             1e-12);
  CHECK_NEAR(damped.proportional_ppm_per_ms(), 2.0 * base.proportional_ppm_per_ms(),
             1e-9);
}

/** Every claim the healthy runs share, so a failure names the one that broke. */
void check_healthy(const LoopReport& report, const RatioControl::Config& config) {
  check_clean("a take found nothing contiguous", report.underruns);
  check_clean("an arrival had to be dropped at capacity", report.overruns);
  check_clean("a period was played at the wrong position", report.lost_frames);
  check_clean("a period was played twice", report.repeated_frames);
  check_clean("a period did not match its position", report.mismatched_periods);
  check_clean("an arrival was late", report.late);
  check_clean("an arrival was a duplicate", report.duplicates);
  check_clean("an arrival was refused", report.refused);
  CHECK_EQ(report.updates_at_limit, 0u);
  // Nothing is lost to the loop either: what was stored is what was played, what
  // is still held, and what was dropped — and here nothing was dropped.
  CHECK_EQ(report.frames_pushed,
           report.frames_played + report.frames_held + report.frames_dropped);
  CHECK_EQ(report.frames_dropped, 0u);
  // The ratio's rate of change is bounded, absolutely: not by the size of the
  // error, not by the level's step, and not by which term is asking.
  CHECK(report.max_slew_ppm_second <= config.slew_ppm_second + 1e-9);
}

TEST_CASE(clock_the_loop_holds_the_level_against_a_ten_ppm_sender) {
  // The claim increment 2 exists for. Ten ppm the wrong way, four hours, and the
  // level held where it was primed to rather than running to the buffer's floor or
  // ceiling — with the correction converging on the offset the two clocks
  // actually have, because that is what holding the level requires.
  const AudioFormat format = sim_format();
  Plan plan;
  plan.sender_ppm = 10;
  plan.milliseconds = 4ull * 3600 * 1000;
  plan.settle_from_ms = 10 * 60 * 1000.0;
  const LoopReport report = run_loop(format, plan);

  check_healthy(report, plan.control);
  CHECK(report.took > 14000000u);  // four hours, less the priming
  CHECK_NEAR(report.ppm_last, plan.truth_ppm(), 0.2);
  CHECK(report.band_ms < 3.0);
  CHECK(std::fabs(report.level_last_ms - report.level_first_ms) < 2.0);
  // It had found the offset, to within a third of a ppm, inside the first
  // thousand seconds of the loop's own period — and stayed there for the rest of
  // the four hours.
  CHECK(report.off_truth_last_ms < 45.0 * 60 * 1000.0);

  std::cout << "    ratio control: 4 h at +10 ppm — level " << report.level_first_ms
            << " ms -> " << report.level_last_ms << " ms, band +/-"
            << report.band_ms << " ms, correction " << report.ppm_last
            << " ppm, settled by " << (report.off_truth_last_ms / 60000.0) << " min"
            << std::endl;
}

TEST_CASE(clock_the_loop_holds_the_level_against_a_faster_receiver) {
  // The other sign, and it is not symmetric in the plant: here the level falls,
  // so the loop has to slow the resampler below the rate the periods arrive at.
  // A correction of -10 ppm is the same measurement from the other side, and a
  // loop that got the sign wrong would drive the level into the floor instead.
  const AudioFormat format = sim_format();
  Plan plan;
  plan.receiver_ppm = 10;
  plan.milliseconds = 2ull * 3600 * 1000;
  plan.settle_from_ms = 10 * 60 * 1000.0;
  const LoopReport report = run_loop(format, plan);

  check_healthy(report, plan.control);
  CHECK_NEAR(report.ppm_last, plan.truth_ppm(), 0.2);
  CHECK(report.band_ms < 3.0);
  CHECK(report.level_min_ms > 100.0);

  std::cout << "    ratio control: 2 h at -10 ppm — level " << report.level_first_ms
            << " ms -> " << report.level_last_ms << " ms, band +/-"
            << report.band_ms << " ms, correction " << report.ppm_last << " ppm"
            << std::endl;
}

TEST_CASE(clock_the_loop_finds_one_ppm_eventually_and_says_how_long_it_took) {
  // The offset the level can barely see. One ppm is one period of level per
  // **1000 seconds**, so this is the case that decides how slow the loop has to
  // be: fast enough to catch it, slow enough not to chase the 1 ms steps it is
  // measured in. The number this prints is the honest limit of a loop closed on a
  // quantised level, and it is what the research document records.
  const AudioFormat format = sim_format();
  Plan plan;
  plan.sender_ppm = 1;
  plan.milliseconds = 2ull * 3600 * 1000;
  plan.settle_from_ms = 20 * 60 * 1000.0;
  plan.truth_tolerance_ppm = 0.2;
  const LoopReport report = run_loop(format, plan);

  check_healthy(report, plan.control);
  // Held just as tightly as the ten ppm case — the level is what the loop
  // measures, and a 1 ms drift in a thousand seconds is still a drift.
  CHECK(report.band_ms < 5.0);
  // The *offset estimate*, though, is the integral, and the integral only grows
  // while there is a residual error. Holding the level means the residual error is
  // small, so the estimate converges more slowly than the level does: after two
  // hours it is most of the way to 1 ppm and still climbing. That is a property of
  // a loop closed on the level, not a bug, and the research document records it —
  // it is also why the ppm figure is not a clock measurement until it has been
  // quiet for hours, while the level is the number to trust immediately.
  CHECK(report.ppm_last > 0.5 * plan.truth_ppm());
  CHECK(report.ppm_last < 1.05 * plan.truth_ppm());

  std::cout << "    ratio control: 2 h at +1 ppm — level " << report.level_first_ms
            << " ms -> " << report.level_last_ms << " ms, band +/-"
            << report.band_ms << " ms, correction " << report.ppm_last
            << " of 1 ppm" << std::endl;
}

TEST_CASE(clock_the_loop_does_not_hunt_on_a_link_with_no_offset_at_all) {
  // Two matched crystals, so there is nothing to correct and anything the loop
  // does is its own noise. What is left is the quantisation floor: the level
  // moves in whole periods because the buffer holds whole periods, so the loop
  // cannot hold it flatter than a millisecond or two — and the ratio must not
  // wander just because the level did.
  const AudioFormat format = sim_format();
  Plan plan;
  plan.milliseconds = 3600ull * 1000;
  plan.settle_from_ms = 10 * 60 * 1000.0;
  const LoopReport report = run_loop(format, plan);

  check_healthy(report, plan.control);
  CHECK(std::fabs(report.ppm_last) < 0.2);
  CHECK(report.ppm_max - report.ppm_min < 0.5);
  CHECK(report.band_ms < 2.0);

  std::cout << "    ratio control: 1 h at 0 ppm — level band +/-" << report.band_ms
            << " ms, correction within "
            << std::max(std::fabs(report.ppm_min), std::fabs(report.ppm_max))
            << " ppm of zero" << std::endl;
}

TEST_CASE(clock_the_loop_brings_the_level_back_after_a_link_hiccup) {
  // A sender that bursts: two thousand ppm for ten seconds, which is 20 ms of
  // extra audio and moves the level the way a link that hiccuped does. The loop
  // has to bring it back without stepping the ratio — the slew is what stops a
  // level step becoming an audio step — and without losing anything while it
  // does.
  const AudioFormat format = sim_format();
  Plan plan;
  plan.sender_ppm = 10;
  plan.milliseconds = 2ull * 3600 * 1000;
  plan.settle_from_ms = 20 * 60 * 1000.0;
  plan.burst_ppm = 2000;
  plan.burst_at_ms = 60ull * 60 * 1000;  // an hour in
  plan.burst_ms = 10000;                 // ten seconds of it
  // The band is measured after the loop has had half an hour to answer the
  // hiccup, so that what it reports is the steady behaviour rather than the
  // disturbance itself — which the printed excursion shows instead.
  plan.settle_from_ms = 90 * 60 * 1000.0;
  const LoopReport report = run_loop(format, plan);

  check_healthy(report, plan.control);
  CHECK_NEAR(report.ppm_last, plan.truth_ppm(), 0.5);
  CHECK(report.band_ms < 3.0);
  // Back where it started: the hiccup is history, not a new operating point.
  CHECK_NEAR(report.level_last_ms, plan.control.target_ms, 1.0);
  // The excursion itself, which the band deliberately excludes.
  CHECK(report.level_max_ms > plan.control.target_ms + 10.0);

  std::cout << "    ratio control: a 20 ms burst at 1 h — level "
            << report.level_min_ms << "-" << report.level_max_ms
            << " ms over the run, back to " << report.level_last_ms
            << " ms, correction " << report.ppm_last << " ppm" << std::endl;
}

TEST_CASE(clock_arrival_jitter_does_not_move_a_loop_that_measures_no_time) {
  // The claim that chose this design. The alternative — estimating the offset
  // from arrival times — needs the jitter's spread measured before it can be
  // trusted, and `docs/research/clock-recovery.md` records that nobody has
  // measured it. A loop on the level is supposed to be immune, because it measures
  // no time and jitter cannot accumulate in an integral. So here is a link that
  // delivers every period up to ten milliseconds late, in order, at random — ten
  // times the spread the research document's table calls "1 ms, the optimistic
  // case" — and every period is still byte-exact, in the right order, and the loop
  // does not notice.
  const AudioFormat format = sim_format();
  Plan plan;
  plan.sender_ppm = 10;
  plan.milliseconds = 3600ull * 1000;
  plan.settle_from_ms = 10 * 60 * 1000.0;
  plan.jitter_ms = 10;
  plan.verify_periods = true;  // every period, because jitter is where audio hides
  const LoopReport report = run_loop(format, plan);

  check_healthy(report, plan.control);
  // Slower than the unjittered run, because the level the loop averages now moves
  // with the spread as well as with the drift — and it is still tracking the
  // offset rather than being fooled by it.
  CHECK_NEAR(report.ppm_last, plan.truth_ppm(), 0.5);
  // The *level's* band is the jitter itself: the buffer holds what the link has
  // delivered, so a delay of up to ten milliseconds is ten milliseconds of level.
  // That is not the loop failing; it is the level being what it is.
  CHECK(report.band_ms < 15.0);

  std::cout
      << "    ratio control: 1 h at +10 ppm through 0-10 ms of jitter — level "
      << report.level_min_ms << "-" << report.level_max_ms << " ms, band +/-"
      << report.band_ms << " ms, correction " << report.ppm_last << " ppm"
      << std::endl;
}

TEST_CASE(clock_a_target_the_buffer_cannot_reach_is_clamped_rather_than_chased) {
  // A misconfiguration, and the safety net that keeps it a misconfiguration: a
  // target the buffer was never primed to. The loop asks for more correction than
  // its clamp allows, so it stops there and the engine can see that it has stopped.
  //
  // What this does *not* promise is a tidy settling: measured, the level climbs
  // toward the unreachable target, passes it, and the correction swings to the
  // other clamp on the way back. That is honest behaviour for a target that is
  // wrong — the point is that nothing is dropped and the correction never leaves
  // its clamp, which is what the assertions below are.
  const AudioFormat format = sim_format();
  Plan plan;
  plan.sender_ppm = 10;
  plan.milliseconds = 30ull * 60 * 1000;
  plan.control.target_ms = 400.0;  // primed to 120 ms, and never told otherwise
  const LoopReport report = run_loop(format, plan);

  CHECK(report.updates_at_limit > 0);
  CHECK(report.level_last_ms > report.level_first_ms);
  // The invariant: the ratio never leaves its clamp, at any point in the run.
  CHECK(report.ppm_min >= -plan.control.limit_ppm - 1e-9);
  CHECK(report.ppm_max <= plan.control.limit_ppm + 1e-9);
  // And it is still not dropping anything: a ratio at its limit is a slow clock,
  // not a broken one.
  check_clean("a take found nothing contiguous", report.underruns);
  check_clean("an arrival had to be dropped at capacity", report.overruns);
  CHECK_EQ(report.frames_dropped, 0u);

  std::cout << "    ratio control: a target of 400 ms against a 120 ms buffer — "
            << "correction held within " << report.ppm_min << "/" << report.ppm_max
            << " ppm, at the clamp for " << report.updates_at_limit
            << " updates, level " << report.level_first_ms << " -> "
            << report.level_last_ms << " ms" << std::endl;
}
