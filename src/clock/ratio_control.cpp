#include "clock/ratio_control.hpp"

#include <cmath>

namespace aes67_srt::clock {
namespace {

/**
 * The plant's gain: milliseconds of level per second, per ppm of ratio error.
 *
 * A ppm of ratio error moves 48 kHz audio by one microsecond per second, so it
 * moves a level measured in milliseconds by a thousandth of one — and that single
 * number is what turns the loop's period and damping into gains.
 */
constexpr double k_plant_ms_per_second_per_ppm = 1.0 / 1000.0;

constexpr double k_two_pi = 6.283185307179586;

}  // namespace

RatioControl::RatioControl() = default;

RatioControl::RatioControl(const Config& config) : config_(config) {}

double RatioControl::integral_ppm_per_ms_second() const {
  const double omega = k_two_pi * 1000.0 / config_.loop_period_ms;
  return omega * omega / k_plant_ms_per_second_per_ppm;
}

double RatioControl::proportional_ppm_per_ms() const {
  const double omega = k_two_pi * 1000.0 / config_.loop_period_ms;
  return 2.0 * config_.damping * omega / k_plant_ms_per_second_per_ppm;
}

bool RatioControl::at_limit() const {
  return std::fabs(correction_ppm_) >= config_.limit_ppm;
}

void RatioControl::update(double level_ms, double elapsed_ms) {
  if (!std::isfinite(level_ms) || !std::isfinite(elapsed_ms) || elapsed_ms <= 0.0) {
    // A caller with no level or no interval has nothing to say to the loop, and
    // integrating a NaN as zero time would silently freeze the ratio rather than
    // report that something upstream is broken.
    return;
  }

  if (!primed_) {
    // The first level is where the buffer is, so the average starts there: a loop
    // that averaged up from zero would see the whole target as an error on its
    // first update and move the ratio by a limit's worth of correction at once.
    average_level_ms_ = level_ms;
    primed_ = true;
  } else if (config_.average_ms <= 0.0) {
    average_level_ms_ = level_ms;
  } else {
    // A first-order average: its time constant turns the measurement's staircase
    // into a ramp. It is deliberately shorter than the loop's period — a lag is
    // phase, and the damping comes from the proportional term below.
    const double alpha =
        elapsed_ms >= config_.average_ms ? 1.0 : elapsed_ms / config_.average_ms;
    average_level_ms_ += alpha * (level_ms - average_level_ms_);
  }

  const double seconds = elapsed_ms / 1000.0;
  const double error = average_level_ms_ - config_.target_ms;
  const double proportional = proportional_ppm_per_ms() * error;

  // The integral accumulates: it is what removes a persistent error, and what
  // ends up holding the offset between the two crystals.
  integral_ppm_ += integral_ppm_per_ms_second() * error * seconds;

  // Then the answer: proportional on the (averaged) level, plus the integral. The
  // proportional term is what damps a loop whose plant is an integrator.
  const double wanted = integral_ppm_ + proportional;

  // Rate limit and clamp the *reported* correction, so that neither a level step
  // nor a first update against a level far from target can step the ratio. The
  // slew is per second and the update is a millisecond, so a term that wants to
  // move tens of ppm still gets there in about a second — fast enough that the
  // damping is untouched on a loop whose period is minutes, slow enough that
  // nothing about the ratio is instantaneous.
  double delta = wanted - correction_ppm_;
  const double max_delta = config_.slew_ppm_second * seconds;
  if (delta > max_delta) {
    delta = max_delta;
  } else if (delta < -max_delta) {
    delta = -max_delta;
  }
  correction_ppm_ += delta;
  if (correction_ppm_ > config_.limit_ppm) {
    correction_ppm_ = config_.limit_ppm;
  } else if (correction_ppm_ < -config_.limit_ppm) {
    correction_ppm_ = -config_.limit_ppm;
  }

  // Whatever the rate limit and the clamp withheld is taken back out of the
  // integral, so the loop cannot wind up against its own limits: absorbing it
  // here would leave a correction the plant never saw, and the loop would then
  // overshoot by exactly that much on the way back.
  integral_ppm_ = correction_ppm_ - proportional;
  if (integral_ppm_ > config_.limit_ppm) {
    integral_ppm_ = config_.limit_ppm;
  } else if (integral_ppm_ < -config_.limit_ppm) {
    integral_ppm_ = -config_.limit_ppm;
  }
  ++updates_;
}

}  // namespace aes67_srt::clock