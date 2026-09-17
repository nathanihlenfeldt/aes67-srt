#pragma once

#include <cstdint>

namespace aes67_srt::clock {

/**
 * The ratio control: steer the resampler to hold the playout buffer's level.
 *
 * This is increment 2 of the clock module (ticket 11, issue #12). ADR 0003
 * decided *what* reconciles the two clock domains — continuous asynchronous
 * resampling — and `docs/research/clock-recovery.md` decided *how*: **close the
 * loop on the buffer level**, because the level *is* the integral of the rate
 * error.
 *
 * That choice is the whole design. The alternative — estimate the offset by
 * fitting the wire's sample position against each frame's arrival time — needs a
 * window of tens of seconds and depends on the arrival jitter `sigma`, which
 * nobody has measured; the research document works the arithmetic and records
 * why the first draft of this module was wrong. A loop on the level measures no
 * time at all: it measures how full the buffer is, which is robust to any jitter
 * because jitter cannot accumulate in an integral. So **nothing here reads a
 * clock**, and a direct estimate stays an optional accelerator for when ticket 09
 * has measured `sigma` on two appliances.
 *
 * **The plant, and what the loop has to be.** A ratio error of one part per
 * million moves the level by **one thousandth of a millisecond per second** (ten
 * ppm is the 0.48 samples a second the research document measured), so the plant
 * is an integrator whose gain is exactly 1/1000 ms/s per ppm. Closing a loop
 * around an integrator with an integrator of its own — and the correction is what
 * removes a *persistent* error, so an integral term is unavoidable — gives a
 * double integrator, which is undamped: it oscillates and grows unless something
 * supplies a term proportional to the error. **That term is the proportional
 * gain.** Its two coefficients are not magnitudes chosen by taste: a loop with
 * natural frequency `omega` and damping `zeta` needs
 * `k_integral = 1000 * omega^2` and `k_proportional = 2000 * zeta * omega`, which
 * is why the configuration carries a period and a damping ratio rather than two
 * gains.
 *
 * The first draft of this loop had no proportional term, on the reasoning that
 * the averaging below would damp it. It does not: a lag adds phase, and 180
 * degrees plus any lag is instability. Measured, it grew by a factor of three an
 * oscillation until it reached its clamp and the level swung from 3 ms to 260 ms
 * — which is what the trace in `tests/test_clock_ratio.cpp` prints. The averaging
 * is still here, but only for the reason it is good at: the measurement above.
 *
 * **The measurement is quantised.** The buffer holds whole periods, so the level
 * moves in whole milliseconds: at 1 ppm it moves one period per **1000 seconds**.
 * The loop therefore cannot correct a 1 ppm offset in seconds — it corrects it in
 * the time the level takes to show it — and its period is minutes rather than
 * seconds. Anything faster chases the 1 ms steps of the loop's own quantisation.
 *
 * **One thread.** Driven from the receive/playout loop, one call per period
 * played.
 */
class RatioControl {
 public:
  /**
   * The loop's gains, and the level it holds.
   *
   * Every one of these is a policy the operator would eventually want to see;
   * they are in a struct rather than member constants because the engine will
   * fill them from the configuration, and the tests drive them directly.
   */
  struct Config {
    /**
     * The level to hold, in milliseconds — the playout delay the receiver primed
     * the buffer to. This is the figure ticket 12's UI shows, so the loop and the
     * operator are looking at the same number.
     *
     * It is not a hard floor or ceiling: the loop steers toward it, the
     * quantisation means it lands within a millisecond or two, and decision 6
     * (never drop audio, let the delay grow) means a link that sags shows up as
     * the level being *away* from target rather than as a refusal.
     */
    double target_ms = 120.0;

    /**
     * The window the level is averaged over before the loop sees it, in
     * milliseconds.
     *
     * One job, and it is not damping: the level arrives as a staircase of whole
     * milliseconds, and this turns that into something a proportional term can act
     * on without a tremor on every period boundary. It has to be *shorter* than
     * the loop's own period, because its lag is phase and this loop has no phase
     * to spare — see the note in the class comment.
     */
    double average_ms = 100000.0;

    /**
     * How long one oscillation of the loop takes, in milliseconds — the loop's
     * natural period, `2 * pi / omega`.
     *
     * Thirty-three minutes, and that is the drift budget's number rather than a
     * taste: a loop has to be slower than the noise it measures and faster than
     * the error it is chasing. The level cannot show a 1 ppm offset in less than
     * a thousand seconds, and a 10 ppm offset reaches a millisecond in a hundred —
     * so the loop sits between them, and its own settling time is a few of these.
     */
    double loop_period_ms = 2000000.0;

    /**
     * The loop's damping ratio: 1.0 is critically damped, and this is slightly
     * under so that it settles a little faster than it would without overshoot.
     *
     * Below about a third the level rings visibly, and the correction is what
     * turns a rate error into a level error — so the ringing appears in the delay
     * figure ticket 12's UI trends, not just in the audio.
     */
    double damping = 0.8;

    /**
     * The furthest the correction may go, either way, in ppm.
     *
     * A safety net rather than a control: real crystals are tens of ppm apart at
     * worst, so a correction beyond ±200 ppm means a measurement that has gone
     * wrong rather than a clock that is far out — and a ratio left to run away
     * would pitch-shift the programme, which is the one thing this module's whole
     * approach exists to prevent.
     */
    double limit_ppm = 200.0;

    /**
     * The fastest the correction may move, in ppm per second.
     *
     * This bounds the ratio's rate of change, absolutely: a level step, a first
     * update against a level far from target, a sender that burst — none of them
     * may step the ratio, because a step in the ratio is a step in the audio.
     *
     * Fifty rather than one, and the number matters. The proportional term has to
     * be able to answer a level that moved in tens of milliseconds, which is a
     * correction of tens of ppm — and it has to do so in about a second, or the
     * rate limit, not the loop, is what decides how fast a disturbance is damped.
     * At a loop period of minutes, a second is nothing. (The first draft of this
     * loop used one ppm per second, which is a bound of a different kind: it
     * throttled the proportional term to a thousandth of what it needed, and the
     * loop's damping went with it.)
     */
    double slew_ppm_second = 50.0;
  };

  /**
   * The loop with the module's defaults, and with |config|.
   *
   * Two constructors rather than one with a default argument: `Config` carries
   * its own defaults, and a compiler is entitled to refuse `= Config()` inside
   * the enclosing class definition, where `Config` is not yet complete.
   */
  RatioControl();
  explicit RatioControl(const Config& config);

  /**
   * One update, from what the buffer holds now and how long since the last one.
   *
   * |elapsed_ms| is time on *our* clock — the period cadence, so one millisecond
   * per period played. A non-positive or non-finite |elapsed_ms| is ignored
   * rather than integrated as zero time: a caller that cannot say how long it has
   * been should not move the ratio at all.
   */
  void update(double level_ms, double elapsed_ms);

  /**
   * The ratio the resampler should apply: **input frames consumed per output
   * frame**, so slightly above one when the sender's clock is the faster.
   *
   * That is the mapping increment 3 needs, and it is worth being exact about,
   * because a sign error here is a loop that runs away rather than one that
   * corrects: this ratio *is* `f_sender / f_receiver` by construction, since
   * consuming at the sender's rate is exactly what holds the level still.
   * Whether the resampler calls that `src_ratio` or its reciprocal is its
   * documentation's question, to be taken there rather than from memory.
   */
  double ratio() const { return 1.0 + correction_ppm_ * 1e-6; }

  /**
   * The correction, in ppm: positive when the sender's clock is the faster.
   *
   * At convergence this is a measurement rather than a state — the offset between
   * the two crystals, which is a number the operator can be shown and the next
   * hardware session can check against the 10 ppm this project assumes.
   */
  double offset_ppm() const { return correction_ppm_; }

  /** The averaged level the loop is steering, in milliseconds. */
  double average_level_ms() const { return average_level_ms_; }

  /** The error the loop is acting on: the averaged level less the target. */
  double level_error_ms() const { return average_level_ms_ - config_.target_ms; }

  /**
   * The two gains the configuration's period and damping come to, exposed so that
   * the derivation in the implementation can be asserted rather than trusted.
   *
   * The integral gain is in ppm of correction per millisecond of error per second;
   * the proportional gain is in ppm per millisecond of error.
   */
  double integral_ppm_per_ms_second() const;
  double proportional_ppm_per_ms() const;

  /** True when the correction is at its clamp: something is wrong, and the engine
   *  should say so rather than let the ratio pretend otherwise. */
  bool at_limit() const;

  /** True once the loop has seen a level and has something to steer. */
  bool primed() const { return primed_; }

  /** Updates applied, so a caller can tell a running loop from an idle one. */
  uint64_t updates() const { return updates_; }

 private:
  Config config_;
  double average_level_ms_ = 0.0;
  /** The integral half of the correction, in ppm. The reported correction is this
   *  plus the proportional term, which is algebraic in the level. */
  double integral_ppm_ = 0.0;
  double correction_ppm_ = 0.0;
  uint64_t updates_ = 0;
  bool primed_ = false;
};

}  // namespace aes67_srt::clock