#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "audio/backend.hpp"

namespace aes67_srt::clock {

/**
 * What a push did, and what it cost.
 *
 * A refusal here is not an error path: a late arrival and a full buffer are both
 * things the clock expects to see and has to report, so they are returned rather
 * than thrown, and every one of them is counted as well as named.
 */
enum class PushStatus {
  /** Held at its position. */
  stored,
  /**
   * Held at its position, and audio that was already held was surrendered to
   * make room: the buffer was at capacity, which means the playout head had
   * fallen a whole buffer behind the sender.
   *
   * The ordinary correction for a rate error is the resampler (ADR 0003), and
   * this is what happens when nothing has corrected it. It is the last resort
   * past the alarm threshold and not the correction mechanism, so it is counted
   * and returned rather than absorbed.
   */
  overrun,
  /** That position is already held. Nothing was stored and nothing was lost. */
  duplicate,
  /** Behind the playout head: its moment has passed, or it was surrendered. */
  late,
  /** Not on a period boundary; positions in one period are period_frames apart. */
  misaligned,
  /** A null pointer, or a byte count that is not one period of the format. */
  invalid,
};

/** The name as it appears in logs, for a status the caller has to report. */
const char* to_string(PushStatus status);

/**
 * The playout buffer: audio held by sample position, and its level in time.
 *
 * This is the first increment of the clock module (ticket 11, issue #12), and it
 * is what both later increments stand on: the ratio control steers on the level
 * this reports, and the resampler sits behind that ratio. See
 * `docs/research/clock-recovery.md`, "How the ratio is obtained, and why the
 * first answer was wrong" — the buffer level *is* the integral of the rate
 * error, so steering the ratio to hold the level constant is self-correcting and
 * needs no timing estimate at all.
 *
 * **The unit is one period at one sample position.** A wire frame carries all
 * eight blocks under a single sample position (ADR 0001) and the engine unpacks
 * a frame into one interleaved period, so holding whole periods is what makes
 * "all eight blocks hold one sample position" true here by construction rather
 * than by reconciliation.
 *
 * **The level is the playout delay, in milliseconds.** It is the *contiguous*
 * run of periods from the playout head, not the highest position held, because
 * it answers the question the operator and the ratio control both ask: how much
 * audio can be played without inventing any? A hole ahead of the head occupies
 * capacity and contributes nothing to the level. This figure is also the delay
 * ticket 12 asks for, and the machinery ticket 13's A/V offset is the same thing
 * seen from the other side.
 *
 * **What it promises.** It never overwrites audio it holds, it never invents
 * audio it does not have, and every arrival it cannot place is counted and named
 * rather than silently absorbed. The one behaviour that costs audio is the
 * overrun: at capacity the oldest periods are surrendered, because the
 * alternative — refusing the arrival until the head reaches it — leaves the head
 * stalled with a full buffer, which reads as silence with nothing in the log.
 *
 * **Where the playout head starts is decided by the first period to arrive.** A
 * receiver has no idea what position 0 was, and playing from zero would be
 * inventing audio, so the head is set by the first push and takes before that
 * refuse. The head advances by a period for each period played; the resampler's
 * ratio is what will later make the sender's positions and our playout come out
 * even.
 *
 * **One thread at a time.** The engine will drive push and take from its receive
 * loop. The control path that polls the level from another thread arrives with
 * the module that serves it, and this class will want a snapshot then rather
 * than a lock-free redesign — it is not made safe by an atomic here.
 *
 * **A hole in the sender's stream, and how the head crosses it.** A gap that a
 * retransmit may yet close must be waited for, not guessed at. But once a *later*
 * period is held, the gap can never be filled: SRT delivers in order, so a period
 * after one that arrived will not arrive later. The head then crosses the hole at
 * real time — one period per `take`, each reported as an underrun so the caller
 * plays silence for it, and counted as concealed — rather than stalling at it for
 * ever. That is what a single lost frame used to do: silence the receiver
 * permanently (issue #20).
 */
class PlayoutBuffer {
 public:
  /**
   * A buffer holding |capacity_periods| periods of |format|.
   *
   * Periods rather than frames or milliseconds, deliberately: the ring is
   * addressed by period, so a capacity that is not a whole number of periods is
   * unrepresentable, and that is the one geometry that would let a push land on
   * audio it does not own. The caller derives it from the configured latency and
   * the alarm threshold above that.
   */
  PlayoutBuffer(size_t capacity_periods, const audio::AudioFormat& format);

  /**
   * Hold one period of audio at |sample_position|.
   *
   * |bytes| must be exactly period_bytes(): a short period is refused rather
   * than read past the end of, and a long one means the caller and the buffer
   * disagree about what a period is. |sample_position| must be a whole number of
   * periods from the stream's start, which is what the sender's own packing and
   * the wire format's one-position-per-frame guarantee it to be.
   */
  PushStatus push(uint64_t sample_position, const uint8_t* period, size_t bytes);

  /**
   * Take the period at the playout head, and advance it.
   *
   * False when nothing contiguous is held — an underrun, which is a state rather
   * than an error: the head does not move and nothing is invented, so the caller
   * (a device that has to be fed either way) decides what to play instead.
   * |period| must have room for period_bytes(); |sample_position| may be null.
   */
  bool take(uint8_t* period, uint64_t* sample_position);

  /** True once the first period has decided where the playout head is. */
  bool primed() const { return primed_; }

  /** The position the next period played will come from. */
  uint64_t head_position() const { return head_; }

  /** Contiguous frames held from the head: what can be played out. */
  uint64_t held_frames() const;

  /** The level, in milliseconds — the playout delay, and ticket 12's figure. */
  double level_ms() const;

  /** The level this buffer can hold, in milliseconds. */
  double capacity_ms() const;

  /** The level as a fraction of capacity, for a UI's bar or a test's threshold. */
  double level_fraction() const;

  size_t capacity_periods() const { return slots_; }
  size_t capacity_frames() const { return slots_ * period_frames_; }
  size_t period_frames() const { return period_frames_; }
  size_t period_bytes() const { return period_bytes_; }

  /** Frames stored: frames the sender delivered and this held. */
  uint64_t frames_pushed() const { return pushed_; }

  /** Frames taken, which is frames played. */
  uint64_t frames_played() const { return played_; }

  /**
   * Frames that will never be played because the buffer was at capacity. This is
   * the size of the overrun, and the number an operator is entitled to see when
   * the delay alarm fires.
   */
  uint64_t frames_dropped() const { return dropped_; }

  /** Frames refused because the head had already passed their position. */
  uint64_t frames_late() const { return late_; }

  /**
   * Frames the head crossed as a proven-permanent hole, played as silence.
   *
   * Distinct from `frames_dropped`: an overrun surrenders audio the buffer held,
   * while this is audio that never arrived and could never arrive once a later
   * period did. Both are loss; only one was ever in our hands.
   */
  uint64_t frames_concealed() const { return concealed_; }

  /** Arrivals refused as duplicates, off a period boundary, or malformed. */
  uint64_t arrivals_refused() const { return refused_; }

  /** Takes that found nothing contiguous, including before the first arrival. */
  uint64_t underruns() const { return underruns_; }

 private:
  /** A slot holding this is holding nothing: it is outside every window. */
  static constexpr uint64_t k_empty = ~static_cast<uint64_t>(0);

  /** The ring slot a position lives in. */
  size_t slot_of(uint64_t sample_position) const;

  /** Extend the contiguous run through every period now held in sequence. */
  void advance_contiguous();

  /**
   * The first held position at or after |position|, or k_empty if none is held
   * within the window. Used to prove a hole permanent: a held period beyond it
   * means the gap can never be filled.
   */
  uint64_t next_held_at_or_after(uint64_t position) const;

  /** Frames held anywhere in the window below |position|: what moving the head
   *  to |position| would strand. */
  uint64_t frames_held_below(uint64_t position) const;

  audio::AudioFormat format_;
  size_t slots_ = 0;
  size_t period_frames_ = 0;
  size_t period_bytes_ = 0;

  /**
   * The position held in each ring slot, and the audio itself.
   *
   * A slot holds audio exactly when its position lies inside the window
   * [head_, head_ + capacity_frames()): a push only ever stores a position that
   * is in the window, two aligned positions cannot both lie in one window
   * without being the same position, and the head never passes a period that is
   * held. So there is no separate "filled" flag that could disagree with the
   * data, and an arrival that cannot be placed is refused rather than
   * overwriting audio that is already here.
   */
  std::vector<uint64_t> positions_;
  std::vector<uint8_t> audio_;

  uint64_t head_ = 0;
  uint64_t contiguous_end_ = 0;
  bool primed_ = false;

  uint64_t pushed_ = 0;
  uint64_t played_ = 0;
  uint64_t dropped_ = 0;
  uint64_t late_ = 0;
  uint64_t refused_ = 0;
  uint64_t underruns_ = 0;
  uint64_t concealed_ = 0;
};

}  // namespace aes67_srt::clock