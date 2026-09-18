#include "clock/playout_buffer.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "audio/backend.hpp"
#include "clock_simulation.hpp"
#include "test_framework.hpp"

/**
 * The playout buffer, and the drift simulation that is the reason it exists
 * (ticket 11, issue #12; increment 1 of the clock module).
 *
 * **The simulation is the point.** ADR 0003 decided *what* reconciles the two
 * clock domains — continuous resampling — and left *how* to the module. The
 * order the module is built in came out of `docs/research/clock-recovery.md`:
 * the playout buffer first, because its level *is* the integral of the rate
 * error, so the ratio control steers on the level rather than on a timing
 * estimate that needs a jitter figure nobody has measured.
 *
 * So the test here has to prove the store first: two clocks offset by a few ppm,
 * hours of simulated audio, and no sample lost or duplicated — with the only
 * loss in the whole suite being a deliberate overrun, counted to the frame.
 *
 * Here the receiver plays one period per one of its own milliseconds, which is
 * the take rate a matched ratio would give. `test_clock_ratio.cpp` closes the
 * loop that produces that rate; the two-clock model itself is in
 * `clock_simulation.hpp`, shared so that a ppm means one thing.
 */
namespace {

using aes67_srt::audio::AudioFormat;
using aes67_srt::clock::PlayoutBuffer;
using aes67_srt::clock::PushStatus;
using clock_sim::check_clean;
using clock_sim::k_tag_mask;
using clock_sim::make_period;
using clock_sim::Pace;
using clock_sim::position_of;
using clock_sim::sim_format;

/**
 * What a simulated run did, so each test can assert on the numbers rather than
 * on a proxy for them.
 */
struct RunReport {
  uint64_t took = 0;
  uint64_t underruns = 0;
  /** Frames that appeared between two taken periods and were never played. */
  uint64_t lost_frames = 0;
  /** Frames taken twice, which is what a duplication would look like. */
  uint64_t repeated_frames = 0;
  /** Periods whose bytes did not name the position they were taken from. */
  uint64_t mismatched_periods = 0;
  /** Arrivals that cost audio or needed a decision, counted by event... */
  uint64_t overruns = 0;
  uint64_t duplicates = 0;
  uint64_t late = 0;
  uint64_t refused = 0;
  /** ...and the same conditions counted in frames. */
  uint64_t dropped_frames = 0;
  uint64_t late_frames = 0;
  uint64_t refused_frames = 0;
  uint64_t first_position = 0;
  uint64_t last_position = 0;
  /** The receiver's clock's own ticks, and how many passed before it played. */
  uint64_t receiver_ticks = 0;
  uint64_t ticks_before_play = 0;
  /** Simulated milliseconds, so a rate can be read off a run. */
  uint64_t milliseconds = 0;
  /** When the first overrun happened, in ms; 0 when there never was one. */
  uint64_t first_overrun_ms = 0;
  /** When the first underrun happened, in ms; 0 when there never was one. */
  uint64_t first_underrun_ms = 0;
  /** The level at the first take, the last, and its high-water mark, in ms. */
  double level_first_ms = 0.0;
  double level_last_ms = 0.0;
  double level_peak_ms = 0.0;
};

/**
 * Simulated audio across two clock domains.
 *
 * |sender_ppm| and |receiver_ppm| are the two crystals; |prime_ms| is how long
 * the receiver waits before it starts playing, which is the buffering a link's
 * latency buys — a receiver does not begin the instant a frame arrives. Those
 * skipped ticks are deliberately not underruns: nothing was being played.
 *
 * |verify_periods| byte-verifies every period taken. The long runs verify a
 * sample of them and every position, because a position that is consecutive and
 * unique through four hours is the loss-and-duplication claim itself, while the
 * bytes prove the audio followed the position; the short runs do both.
 */
RunReport run(const AudioFormat& format, size_t capacity_periods,
              uint64_t sender_ppm, uint64_t receiver_ppm, uint64_t milliseconds,
              uint64_t prime_ms, bool verify_periods) {
  PlayoutBuffer buffer(capacity_periods, format);
  Pace sender;
  Pace receiver;
  sender.ppm = sender_ppm;
  receiver.ppm = receiver_ppm;

  RunReport report;
  std::vector<uint8_t> arriving(format.period_bytes(), 0);
  std::vector<uint8_t> taken(format.period_bytes(), 0);
  std::vector<uint8_t> expected(format.period_bytes(), 0);
  uint64_t sender_position = 0;
  uint64_t next_expected = 0;
  bool playing = false;

  for (uint64_t ms = 0; ms < milliseconds; ++ms) {
    report.milliseconds = ms;
    for (uint64_t tick = 0, ticks = sender.advance(); tick < ticks; ++tick) {
      arriving = make_period(format, sender_position);
      const PushStatus status =
          buffer.push(sender_position, arriving.data(), arriving.size());
      switch (status) {
        case PushStatus::overrun:
          ++report.overruns;
          if (report.first_overrun_ms == 0) {
            report.first_overrun_ms = ms;
          }
          break;
        case PushStatus::duplicate:
          ++report.duplicates;
          break;
        case PushStatus::late:
          ++report.late;
          break;
        case PushStatus::misaligned:
        case PushStatus::invalid:
          ++report.refused;
          break;
        case PushStatus::stored:
          break;
      }
      sender_position += format.period_frames;
    }

    for (uint64_t tick = 0, ticks = receiver.advance(); tick < ticks; ++tick) {
      if (!playing && ms < prime_ms) {
        // Nothing is being played yet, so a tick with nothing to play is not an
        // underrun — it is a receiver waiting for the buffer to fill.
        ++report.ticks_before_play;
        continue;
      }
      playing = true;
      ++report.receiver_ticks;
      const double level = buffer.level_ms();
      if (report.took == 0) {
        report.level_first_ms = level;
      }
      if (level > report.level_peak_ms) {
        report.level_peak_ms = level;
      }
      report.level_last_ms = level;

      uint64_t position = 0;
      if (buffer.take(taken.data(), &position)) {
        if (report.took == 0) {
          next_expected = position;
          report.first_position = position;
        }
        if (position == next_expected) {
          next_expected = position + format.period_frames;
        } else if (position > next_expected) {
          report.lost_frames += position - next_expected;
          next_expected = position + format.period_frames;
        } else {
          report.repeated_frames += next_expected - position;
        }
        report.last_position = position;
        ++report.took;
        if (verify_periods || report.took % 1000 == 0) {
          // The cheap check first — the payload's own claim about where it
          // belongs — then the whole period against what the sender would have
          // produced for that position.
          if (position_of(taken.data()) != (position & k_tag_mask)) {
            ++report.mismatched_periods;
          }
          expected = make_period(format, position);
          if (expected != taken) {
            ++report.mismatched_periods;
          }
        }
      } else {
        if (report.first_underrun_ms == 0) {
          report.first_underrun_ms = ms;
        }
        ++report.underruns;
      }
    }
  }

  report.milliseconds = milliseconds;
  report.dropped_frames = buffer.frames_dropped();
  report.late_frames = buffer.frames_late();
  report.refused_frames = buffer.arrivals_refused();
  return report;
}

}  // namespace

TEST_CASE(clock_the_level_is_the_playout_delay_in_milliseconds) {
  const AudioFormat format = sim_format();
  PlayoutBuffer buffer(500, format);  // half a second of playout

  CHECK_EQ(buffer.capacity_periods(), static_cast<size_t>(500));
  CHECK_EQ(buffer.capacity_frames(), static_cast<size_t>(24000));
  CHECK_NEAR(buffer.capacity_ms(), 500.0, 1e-9);
  CHECK(!buffer.primed());
  CHECK_NEAR(buffer.level_ms(), 0.0, 1e-9);

  // Nothing has arrived, so there is nothing to play and no head to play it
  // from. This is also a buffer asked to take before anything arrived, which is
  // what start-up looks like.
  std::vector<uint8_t> period(format.period_bytes(), 0);
  CHECK(!buffer.take(period.data(), nullptr));
  CHECK_EQ(buffer.underruns(), 1u);
  CHECK(!buffer.primed());
}

TEST_CASE(
    clock_a_receiver_without_compensation_fills_a_120_ms_buffer_in_3_33_hours) {
  // The cliff edge from docs/research/clock-recovery.md, measured through the
  // buffer rather than in closed form: at 10 ppm the level grows by one frame
  // every 2.08 seconds, so a 120 ms buffer is full after 12000 seconds, which is
  // 3.33 hours — inside a four-hour show, and not inside a three-hour one.
  const AudioFormat format = sim_format();
  const RunReport report = run(format, 120, /*sender_ppm=*/10, /*receiver_ppm=*/0,
                               /*milliseconds=*/4ull * 3600 * 1000, /*prime_ms=*/0,
                               /*verify_periods=*/false);

  CHECK(report.overruns > 0);
  CHECK_NEAR(static_cast<double>(report.first_overrun_ms) / 3600000.0, 3.33, 0.05);

  // Nothing is lost except what was surrendered, and nothing is played twice.
  // This is the promise, stated as an equation: the frames missing from the
  // played stream are exactly the frames the buffer counted as dropped.
  CHECK_EQ(report.lost_frames, report.dropped_frames);
  CHECK_EQ(report.repeated_frames, 0u);
  CHECK_EQ(report.mismatched_periods, 0u);
  CHECK_EQ(report.late, 0u);
  CHECK_EQ(report.duplicates, 0u);
  CHECK_EQ(report.refused, 0u);
  CHECK_EQ(report.took, report.receiver_ticks);

  std::cout << "    clock drift: a 120 ms buffer filled after "
            << static_cast<double>(report.first_overrun_ms) / 3600000.0
            << " hours at 10 ppm and then surrendered "
            << report.dropped_frames / 48 << " ms of audio over four hours"
            << std::endl;
}

TEST_CASE(
    clock_four_hours_of_two_clocks_a_few_ppm_apart_loses_and_duplicates_nothing) {
  // The claim ticket 11 is judged on, and the reason this module is built
  // buffer-first. Ten ppm the wrong way, four hours, 500 ms of buffer: the
  // receiver's take rate is the rate periods arrive at, which is what the ratio
  // control will arrange — with the resampler behind it in increment 3 turning 48
  // output frames into a slightly different number of input ones. Here the take
  // rate stands in for it, because this test is about the store losing nothing.
  const AudioFormat format = sim_format();
  const uint64_t four_hours = 4ull * 3600 * 1000;
  const RunReport report =
      run(format, 500, /*sender_ppm=*/10, /*receiver_ppm=*/10, four_hours,
          /*prime_ms=*/120, /*verify_periods=*/false);

  check_clean("a take found nothing contiguous", report.underruns);
  check_clean("an arrival had to be refused",
              report.late + report.duplicates + report.refused);
  check_clean("frames arrived too late to play", report.late_frames);
  check_clean("frames were refused outright", report.refused_frames);
  check_clean("a period was played at the wrong position", report.lost_frames);
  check_clean("a period was played twice", report.repeated_frames);
  check_clean("a period did not match its position", report.mismatched_periods);
  CHECK_EQ(report.overruns, 0u);
  CHECK_EQ(report.dropped_frames, 0u);

  // Four hours is 14.4 million periods, in order, every one of them 48 frames
  // after the last: one identity rather than a hand-computed position, so the
  // assertion cannot be satisfied by the arithmetic being wrong in the same way
  // twice.
  CHECK_EQ(report.milliseconds, four_hours);
  CHECK_EQ(report.took, report.receiver_ticks);
  CHECK_EQ(report.first_position, 0u);
  CHECK_EQ(report.last_position, report.first_position + 48 * (report.took - 1));
  CHECK_EQ(report.lost_frames, 0u);

  // And the level held: with the rates matched it does not creep, which is the
  // property the ratio control steers on.
  CHECK_NEAR(report.level_last_ms, report.level_first_ms, 1e-9);

  std::cout << "    clock drift: four simulated hours, 10 ppm apart, "
            << report.took << " periods played, level "
            << (report.level_last_ms / 1000.0) << " s" << std::endl;
}

TEST_CASE(
    clock_a_buffer_drained_by_a_faster_receiver_runs_dry_when_the_table_says) {
  // The same cliff edge from the other side: with the sender slower the level
  // falls instead of rising, and an empty buffer means the device is asked for
  // audio that nothing has arrived to fill. The research document's table says a
  // full buffer lasts 0.67 hours at 50 ppm, and running the buffer's own
  // arithmetic at 50 ppm says 0.67 hours — 120 periods at one period every 20
  // seconds. Direction does not change the arithmetic.
  const AudioFormat format = sim_format();
  const RunReport report = run(format, /*capacity_periods=*/150,
                               /*sender_ppm=*/0, /*receiver_ppm=*/50,
                               /*milliseconds=*/3600ull * 1000,
                               /*prime_ms=*/120, /*verify_periods=*/false);

  CHECK(report.underruns > 0);
  CHECK_NEAR(static_cast<double>(report.first_underrun_ms) / 3600000.0, 0.667,
             0.02);

  // An underrun invents nothing: the head does not move, so no sample is skipped
  // by it and none is repeated. The audio that arrives later is played late
  // rather than being pretended into place.
  CHECK_EQ(report.repeated_frames, 0u);
  CHECK_EQ(report.mismatched_periods, 0u);
  CHECK_EQ(report.lost_frames, 0u);
  CHECK_EQ(report.overruns, 0u);
}

TEST_CASE(clock_only_the_difference_between_two_clocks_matters) {
  // The third finding in docs/research/clock-recovery.md, measured: two crystals
  // that are both wrong by nearly the same amount drift far more slowly than two
  // that are far apart. It is the difference that costs, which is worth knowing
  // when specifying what goes in a rack at each end.
  //
  // The resolution is one period, and that is a property of the buffer rather
  // than of this simulation: the level moves in whole milliseconds because the
  // buffer holds whole periods, so at 1 ppm it moves one period per 1000 seconds
  // and an hour's run carries up to a period of rounding. The scale is the
  // finding; the bit is not.
  const AudioFormat format = sim_format();
  const uint64_t one_hour = 3600ull * 1000;

  const RunReport far_apart = run(format, 500, 10, 0, one_hour, 0, false);
  const RunReport both_wrong = run(format, 500, 10, 9, one_hour, 0, false);
  const RunReport nearly_right = run(format, 500, 1, 0, one_hour, 0, false);

  // The closed form: 0.48 samples a second at 10 ppm is 1728 frames an hour,
  // which is 36 ms of level; at 1 ppm it is 36 ms over ten hours.
  CHECK_NEAR(far_apart.level_last_ms, 36.0, 1.0);
  CHECK_NEAR(nearly_right.level_last_ms, 3.6, 1.0);
  CHECK_NEAR(both_wrong.level_last_ms, nearly_right.level_last_ms, 1.0);
  CHECK(far_apart.level_last_ms > 5.0 * both_wrong.level_last_ms);

  CHECK_EQ(far_apart.overruns + both_wrong.overruns + nearly_right.overruns, 0u);
  CHECK_EQ(
      far_apart.lost_frames + both_wrong.lost_frames + nearly_right.lost_frames,
      0u);

  std::cout << "    clock drift, one hour: 10 ppm against 0 -> level +"
            << far_apart.level_last_ms << " ms; 10 ppm against 9 -> +"
            << both_wrong.level_last_ms << " ms; 1 ppm against 0 -> +"
            << nearly_right.level_last_ms << " ms" << std::endl;
}

TEST_CASE(clock_every_period_comes_back_byte_for_byte) {
  // The bytes, not just the positions, over twenty seconds at each sign of the
  // offset: a period must come out exactly as the sender's period did, or the
  // audio that was correct in position was wrong in content. Every period is
  // verified here, where the run is short enough for it, and a sample of them is
  // verified in the multi-hour runs.
  const AudioFormat format = sim_format();
  const uint64_t twenty_seconds = 20ull * 1000;

  for (const uint64_t ppm : {1ull, 10ull, 50ull}) {
    const RunReport faster = run(format, 500, ppm, ppm, twenty_seconds, 120, true);
    const RunReport slower = run(format, 500, 0, ppm, twenty_seconds, 120, true);
    for (const RunReport* report : {&faster, &slower}) {
      check_clean("a period came back with different bytes",
                  report->mismatched_periods);
      check_clean("a period came back at the wrong position", report->lost_frames);
      check_clean("a period came back twice", report->repeated_frames);
      CHECK_EQ(report->took, report->receiver_ticks);
      CHECK_EQ(report->last_position,
               report->first_position + 48 * (report->took - 1));
      CHECK_EQ(report->dropped_frames, 0u);
    }
  }
}

TEST_CASE(clock_a_hole_with_nothing_beyond_it_is_waited_for) {
  // A gap that may still close is waited for: nothing is held beyond it, so a
  // retransmit could yet fill it, and playing through would be inventing audio.
  const AudioFormat format = sim_format();
  PlayoutBuffer buffer(500, format);
  std::vector<uint8_t> period = make_period(format, 0);
  uint64_t position = 0;

  CHECK(buffer.push(0, period.data(), period.size()) == PushStatus::stored);
  CHECK(buffer.take(period.data(), &position));
  CHECK_EQ(position, 0u);
  CHECK_EQ(buffer.head_position(), 48u);

  // Nothing at the head and nothing beyond it: wait, and do not move the head.
  CHECK(!buffer.take(period.data(), &position));
  CHECK_EQ(buffer.underruns(), 1u);
  CHECK_EQ(buffer.head_position(), 48u);
  CHECK_EQ(buffer.frames_concealed(), 0u);

  // It arrives, and it plays in order with nothing concealed.
  period = make_period(format, 48);
  CHECK(buffer.push(48, period.data(), period.size()) == PushStatus::stored);
  CHECK(buffer.take(period.data(), &position));
  CHECK_EQ(position, 48u);
  CHECK_EQ(buffer.frames_concealed(), 0u);
  CHECK_EQ(buffer.frames_played(), 96u);
}

TEST_CASE(clock_a_hole_with_audio_beyond_it_is_crossed_as_silence) {
  // Once a later period is held the gap can never be filled — SRT delivers in
  // order — so the head crosses it at real time, one period of silence per take,
  // rather than stalling at it for ever. This is the failure that used to silence
  // the receiver permanently on one lost frame (issue #20).
  const AudioFormat format = sim_format();
  PlayoutBuffer buffer(500, format);
  std::vector<uint8_t> period = make_period(format, 0);
  uint64_t position = 0;

  CHECK(buffer.push(0, period.data(), period.size()) == PushStatus::stored);
  CHECK(buffer.push(96, period.data(), period.size()) == PushStatus::stored);

  CHECK(buffer.take(period.data(), &position));
  CHECK_EQ(position, 0u);

  // At the hole with 96 held beyond it: one period of silence, and the head
  // steps over exactly one period — not a jump that skips the gap in time.
  CHECK(!buffer.take(period.data(), &position));
  CHECK_EQ(buffer.frames_concealed(), 48u);
  CHECK_EQ(buffer.head_position(), 96u);

  // The held period then plays, in its proper place.
  CHECK(buffer.take(period.data(), &position));
  CHECK_EQ(position, 96u);
  CHECK_EQ(buffer.frames_played(), 96u);
  CHECK_EQ(buffer.frames_dropped(), 0u);
  CHECK_EQ(buffer.frames_late(), 0u);
}

TEST_CASE(clock_discards_a_startup_backlog_forward_to_the_target) {
  // Issue #34: a link that comes up after playout started floods the buffer, and
  // the ratio control steers rate, not level, so the excess is discarded forward
  // to the target rather than held for ever.
  const AudioFormat format = sim_format();
  PlayoutBuffer buffer(500, format);
  std::vector<uint8_t> period = make_period(format, 0);
  uint64_t position = 0;

  // A hundred contiguous periods (100 ms at this format).
  for (int i = 0; i < 100; ++i) {
    CHECK(buffer.push(static_cast<uint64_t>(i) * 48, period.data(),
                      period.size()) == PushStatus::stored);
  }
  CHECK_EQ(buffer.held_frames(), 100u * 48u);

  // Keep 20 ms: 80 periods are discarded, the head moves forward, and what
  // remains plays in order.
  const uint64_t dropped = buffer.discard_to_level_ms(20.0);
  CHECK_EQ(dropped, 80u * 48u);
  CHECK_NEAR(buffer.level_ms(), 20.0, 1e-9);
  CHECK_EQ(buffer.frames_dropped(), 80u * 48u);
  CHECK(buffer.take(period.data(), &position));
  CHECK_EQ(position, 80u * 48u);

  // Keeping more than is held discards nothing.
  CHECK_EQ(buffer.discard_to_level_ms(1000.0), 0u);
}

TEST_CASE(clock_an_arrival_it_cannot_place_is_refused_and_counted) {
  const AudioFormat format = sim_format();
  PlayoutBuffer buffer(4, format);  // four periods, so capacity is easy to reach
  std::vector<uint8_t> period = make_period(format, 0);
  uint64_t position = 0;

  // Malformed before any of it can be audio: a null pointer, a period that is
  // short, a period that is long, and a position that is not on a period
  // boundary. None of them may be stored, and none of them may be ignored
  // either — a caller whose byte count is wrong has a bug, and it shows up here.
  CHECK(buffer.push(0, nullptr, period.size()) == PushStatus::invalid);
  CHECK(buffer.push(0, period.data(), period.size() - 1) == PushStatus::invalid);
  CHECK(buffer.push(0, period.data(), period.size() + 1) == PushStatus::invalid);
  CHECK(buffer.push(49, period.data(), period.size()) == PushStatus::misaligned);
  CHECK_EQ(buffer.arrivals_refused(), 4u);
  CHECK_EQ(buffer.frames_pushed(), 0u);
  CHECK(!buffer.primed());

  // The first good arrival sets the head; a second copy of the same period is a
  // duplicate rather than audio to play again.
  CHECK(buffer.push(0, period.data(), period.size()) == PushStatus::stored);
  CHECK(buffer.push(0, period.data(), period.size()) == PushStatus::duplicate);
  CHECK_EQ(buffer.frames_pushed(), 48u);
  CHECK_EQ(buffer.arrivals_refused(), 5u);

  // Fill it: four periods is the whole buffer.
  for (uint64_t at = 48; at < 192; at += 48) {
    period = make_period(format, at);
    CHECK(buffer.push(at, period.data(), period.size()) == PushStatus::stored);
  }
  CHECK_NEAR(buffer.level_ms(), 4.0, 1e-9);

  // The fifth position cannot be held without giving something up, and the
  // oldest period is what goes. It is counted, because it is audio that will
  // never be played, and the head moves with it so the stream carries on.
  period = make_period(format, 192);
  CHECK(buffer.push(192, period.data(), period.size()) == PushStatus::overrun);
  CHECK_EQ(buffer.frames_dropped(), 48u);
  CHECK_EQ(buffer.head_position(), 48u);
  CHECK_NEAR(buffer.level_ms(), 4.0, 1e-9);

  CHECK(buffer.take(period.data(), &position));
  CHECK_EQ(position, 48u);

  // An arrival behind the head is late: it arrived, and its moment has gone.
  period = make_period(format, 0);
  CHECK(buffer.push(0, period.data(), period.size()) == PushStatus::late);
  CHECK_EQ(buffer.frames_late(), 48u);
  CHECK_EQ(buffer.frames_dropped(), 48u);

  // A buffer with no periods at all holds nothing and says so rather than
  // pretending: every arrival is counted as audio that will never play.
  PlayoutBuffer none(0, format);
  CHECK(none.push(0, period.data(), period.size()) == PushStatus::overrun);
  CHECK_EQ(none.frames_dropped(), 48u);
  CHECK(!none.take(period.data(), nullptr));
  CHECK_EQ(none.underruns(), 1u);
}

TEST_CASE(clock_a_sender_that_jumps_forwards_moves_the_head_and_says_what_it_lost) {
  // A link that went away and came back, or a far end that restarted: the
  // sender's timeline moves further than the buffer could follow. Seating the
  // audio at its new position is the only way to keep playing, so the head moves
  // there and the audio that was held is counted as gone — rather than left
  // behind a head that can never reach it, which reads as silence with a full
  // buffer and nothing in the log to say why.
  const AudioFormat format = sim_format();
  PlayoutBuffer buffer(10, format);  // a ten-period window
  std::vector<uint8_t> period = make_period(format, 0);
  uint64_t position = 0;

  CHECK(buffer.push(0, period.data(), period.size()) == PushStatus::stored);
  period = make_period(format, 48);
  CHECK(buffer.push(48, period.data(), period.size()) == PushStatus::stored);
  CHECK_NEAR(buffer.level_ms(), 2.0, 1e-9);

  const uint64_t far = 48 * 100;
  period = make_period(format, far);
  CHECK(buffer.push(far, period.data(), period.size()) == PushStatus::overrun);
  CHECK_EQ(buffer.head_position(), far);
  CHECK_EQ(buffer.frames_dropped(), 96u);  // both held periods, counted as gone
  CHECK_NEAR(buffer.level_ms(), 1.0, 1e-9);

  CHECK(buffer.take(period.data(), &position));
  CHECK_EQ(position, far);

  // And the run carries on at the sender's timeline rather than stalling.
  period = make_period(format, far + 48);
  CHECK(buffer.push(far + 48, period.data(), period.size()) == PushStatus::stored);
  CHECK(buffer.take(period.data(), &position));
  CHECK_EQ(position, far + 48);
  CHECK_EQ(buffer.underruns(), 0u);
}

TEST_CASE(clock_all_eight_blocks_hold_one_sample_position) {
  // Ticket 12's cross-block criterion, at the shape the appliance runs: 64
  // channels of L24 in eight blocks of eight. The wire format gives every block
  // in a frame one sample position (ADR 0001) and the engine unpacks a frame into
  // one interleaved period, so a whole frame's audio is one period here — which
  // is what makes "all eight blocks at one sample position" structural rather
  // than something to reconcile.
  //
  // The payload says what it is: three bytes per sample whose low byte is the
  // frame, middle byte the channel *within its block*, high byte the block. Any
  // channel that moves between blocks, or any block seated at another frame, is
  // therefore caught rather than passing as audio.
  const AudioFormat format = sim_format(64);
  PlayoutBuffer buffer(4, format);

  std::vector<uint8_t> period(format.period_bytes(), 0);
  for (size_t block = 0; block < 8; ++block) {
    for (size_t frame = 0; frame < format.period_frames; ++frame) {
      for (size_t channel = 0; channel < 8; ++channel) {
        const size_t global_channel = block * 8 + channel;
        const size_t at =
            (frame * format.channels + global_channel) * format.sample_bytes;
        period[at] = static_cast<uint8_t>(frame);
        period[at + 1] = static_cast<uint8_t>(channel);
        period[at + 2] = static_cast<uint8_t>(block);
      }
    }
  }

  // A position that is not zero, so a household default cannot pass this.
  const uint64_t position = 4800;
  CHECK(buffer.push(position, period.data(), period.size()) == PushStatus::stored);

  std::vector<uint8_t> played(format.period_bytes(), 0);
  uint64_t taken_at = 0;
  CHECK(buffer.take(played.data(), &taken_at));
  CHECK_EQ(taken_at, position);

  size_t wrong_block = 0;
  size_t wrong_channel = 0;
  size_t wrong_frame = 0;
  for (size_t block = 0; block < 8; ++block) {
    for (size_t frame = 0; frame < format.period_frames; ++frame) {
      for (size_t channel = 0; channel < 8; ++channel) {
        const size_t global_channel = block * 8 + channel;
        const size_t at =
            (frame * format.channels + global_channel) * format.sample_bytes;
        if (played[at + 2] != static_cast<uint8_t>(block)) {
          ++wrong_block;
        }
        if (played[at + 1] != static_cast<uint8_t>(channel)) {
          ++wrong_channel;
        }
        if (played[at] != static_cast<uint8_t>(frame)) {
          ++wrong_frame;
        }
      }
    }
  }
  check_clean("a sample came back in the wrong block", wrong_block);
  check_clean("a sample came back on the wrong channel", wrong_channel);
  check_clean("a sample came back in the wrong frame", wrong_frame);

  // Which is the whole period, unaltered: the eight blocks are one period of the
  // device, and that is what makes them one sample position.
  CHECK_EQ(format.period_bytes(), static_cast<size_t>(9216));
  CHECK(played == period);
  CHECK_EQ(buffer.held_frames(), 0u);
  CHECK_EQ(buffer.frames_played(), 48u);
}
