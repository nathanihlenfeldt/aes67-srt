#include <cstdint>
#include <iostream>

#include "test_framework.hpp"

/**
 * The measurement half of the clock-recovery prototype (ticket 03, issue #3).
 *
 * No hardware, deliberately: this is arithmetic and a discrete simulation, and a
 * Pi cannot improve on either. What it produces is the number the whole design
 * turns on — how often two clock domains force a sample to be slipped — and what
 * it leaves to hardware is the CPU cost of the alternatives and whether a slip is
 * audible.
 *
 * The two ends follow their own PTP grandmasters, so their sample clocks differ by
 * however many ppm their crystals differ. Everything here follows from that one
 * sentence.
 */
namespace {

constexpr double k_sample_rate = 48000.0;

/** Samples per second the receiver gains on (positive) or loses to (negative) the
 * sender. */
double drift_samples_per_second(double sender_ppm, double receiver_ppm) {
  return k_sample_rate * (sender_ppm - receiver_ppm) / 1e6;
}

/** Seconds between one-sample corrections, or 0 when the clocks agree. */
double slip_interval_seconds(double sender_ppm, double receiver_ppm) {
  const double drift = drift_samples_per_second(sender_ppm, receiver_ppm);
  if (drift == 0.0) {
    return 0.0;
  }
  return 1.0 / (drift < 0.0 ? -drift : drift);
}

/** How long a buffer of |buffer_ms| lasts before it underruns or overruns. */
double buffer_exhaustion_seconds(double buffer_ms, double sender_ppm,
                                 double receiver_ppm) {
  const double drift = drift_samples_per_second(sender_ppm, receiver_ppm);
  if (drift == 0.0) {
    return 0.0;
  }
  const double samples = buffer_ms / 1000.0 * k_sample_rate;
  return samples / (drift < 0.0 ? -drift : drift);
}

/**
 * Discrete simulation rather than the closed form: each millisecond the sender
 * produces what its own clock says and the receiver consumes what its own clock
 * says. Returns the whole number of samples of surplus that must be corrected,
 * which is what a slipping buffer would actually have to do.
 */
long long simulated_surplus_samples(double sender_ppm, double receiver_ppm,
                                    long long seconds) {
  double produced = 0.0;
  double consumed = 0.0;
  const double per_ms = k_sample_rate / 1000.0;
  for (long long millisecond = 0; millisecond < seconds * 1000; ++millisecond) {
    produced += per_ms * (1.0 + sender_ppm / 1e6);
    consumed += per_ms * (1.0 + receiver_ppm / 1e6);
  }
  const double surplus = produced - consumed;
  return static_cast<long long>(surplus < 0.0 ? -surplus : surplus);
}

}  // namespace

TEST_CASE(clock_drift_is_one_sample_every_few_seconds_not_every_few_minutes) {
  // 10 ppm is an ordinary crystal offset, not a fault.
  CHECK_NEAR(drift_samples_per_second(10.0, 0.0), 0.48, 1e-9);
  CHECK_NEAR(slip_interval_seconds(10.0, 0.0), 2.0833, 1e-3);

  // The specification guessed this would be "a click every few minutes". It is
  // every couple of seconds, and that is the finding that changes the design.
  CHECK(slip_interval_seconds(10.0, 0.0) < 3.0);

  std::cout << "    clock drift: 1 ppm -> a slip every "
            << slip_interval_seconds(1.0, 0.0) << " s; 10 ppm -> every "
            << slip_interval_seconds(10.0, 0.0) << " s; 50 ppm -> every "
            << slip_interval_seconds(50.0, 0.0) << " s" << std::endl;
}

TEST_CASE(clock_drift_at_the_offsets_a_site_might_actually_have) {
  CHECK_NEAR(slip_interval_seconds(1.0, 0.0), 20.833, 1e-2);
  CHECK_NEAR(slip_interval_seconds(50.0, 0.0), 0.4167, 1e-3);
  // Two clocks that are both wrong, but by nearly the same amount, drift slowly:
  // only the difference matters, which is why a site with matched hardware is
  // kinder than one with a good clock at one end and a poor one at the other.
  CHECK_NEAR(drift_samples_per_second(10.0, 9.0), 0.048, 1e-9);
  CHECK_NEAR(slip_interval_seconds(10.0, 9.0), 20.833, 1e-2);
}

TEST_CASE(clock_an_uncompensated_buffer_runs_out_in_hours) {
  // Why this cannot be ignored rather than fixed: a 120 ms buffer at 10 ppm
  // survives a three-hour show and comes apart during a four-hour one. That is the
  // worst kind of failure — one that a commissioning test never sees.
  const double exhaustion = buffer_exhaustion_seconds(120.0, 10.0, 0.0);
  CHECK(exhaustion > 3.0 * 3600.0);
  CHECK(exhaustion < 4.0 * 3600.0);

  std::cout << "    clock drift: a 120 ms buffer lasts " << (exhaustion / 3600.0)
            << " hours at 10 ppm, "
            << (buffer_exhaustion_seconds(120.0, 50.0, 0.0) / 3600.0)
            << " hours at 50 ppm" << std::endl;
}

TEST_CASE(clock_the_simulation_agrees_with_the_arithmetic) {
  for (const double ppm : {1.0, 10.0, 50.0}) {
    const long long simulated = simulated_surplus_samples(ppm, 0.0, 3600);
    const double closed_form = drift_samples_per_second(ppm, 0.0) * 3600.0;
    // Within two samples an hour of accumulated rounding, which is the check that
    // the simulation measures what the arithmetic says it does.
    CHECK_NEAR(static_cast<double>(simulated), closed_form, 2.0);
  }
}

TEST_CASE(clock_a_show_is_a_stated_number_of_corrections) {
  // Four hours at 10 ppm. The number matters because 64 channels correct at the
  // same instant: the count is per link, not per channel, so it is the size of
  // each correction that is in question and not how often they come.
  const long long corrections = simulated_surplus_samples(10.0, 0.0, 4 * 3600);
  CHECK_NEAR(static_cast<double>(corrections), 6912.0, 2.0);

  std::cout << "    clock drift: " << corrections
            << " one-sample corrections over a four-hour show at 10 ppm"
            << std::endl;
}
