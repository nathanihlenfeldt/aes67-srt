#include "clock/playout_buffer.hpp"

#include <cstring>

namespace aes67_srt::clock {

const char* to_string(PushStatus status) {
  switch (status) {
    case PushStatus::stored:
      return "stored";
    case PushStatus::overrun:
      return "overrun";
    case PushStatus::duplicate:
      return "duplicate";
    case PushStatus::late:
      return "late";
    case PushStatus::misaligned:
      return "misaligned";
    case PushStatus::invalid:
      return "invalid";
  }
  return "unknown";
}

PlayoutBuffer::PlayoutBuffer(size_t capacity_periods,
                             const audio::AudioFormat& format)
    : format_(format),
      slots_(capacity_periods),
      period_frames_(format.period_frames),
      period_bytes_(format.period_bytes()) {
  positions_.assign(slots_, k_empty);
  audio_.assign(slots_ * period_bytes_, 0);
}

size_t PlayoutBuffer::slot_of(uint64_t sample_position) const {
  return static_cast<size_t>((sample_position / period_frames_) % slots_);
}

uint64_t PlayoutBuffer::held_frames() const {
  return contiguous_end_ - head_;
}

double PlayoutBuffer::level_ms() const {
  if (format_.sample_rate == 0) {
    return 0.0;
  }
  return static_cast<double>(held_frames()) * 1000.0 / format_.sample_rate;
}

double PlayoutBuffer::capacity_ms() const {
  if (format_.sample_rate == 0) {
    return 0.0;
  }
  return static_cast<double>(capacity_frames()) * 1000.0 / format_.sample_rate;
}

double PlayoutBuffer::level_fraction() const {
  const size_t capacity = capacity_frames();
  if (capacity == 0) {
    return 0.0;
  }
  return static_cast<double>(held_frames()) / static_cast<double>(capacity);
}

uint64_t PlayoutBuffer::frames_held_below(uint64_t position) const {
  // A scan rather than arithmetic, because only the slots know: the frames below
  // the head are the ones already surrendered, and the caller is entitled to a
  // true count of what it lost. This runs once per overrun, not once per push.
  uint64_t held = 0;
  for (size_t slot = 0; slot < slots_; ++slot) {
    const uint64_t stored = positions_[slot];
    if (stored != k_empty && stored >= head_ && stored < position) {
      held += period_frames_;
    }
  }
  return held;
}

PushStatus PlayoutBuffer::push(uint64_t sample_position, const uint8_t* period,
                               size_t bytes) {
  if (period == nullptr || period_bytes_ == 0 || period_frames_ == 0 ||
      bytes != period_bytes_) {
    // A short period is refused rather than read past the end of, and a long one
    // means the caller and this buffer disagree about what a period is.
    ++refused_;
    return PushStatus::invalid;
  }
  if (sample_position % period_frames_ != 0) {
    ++refused_;
    return PushStatus::misaligned;
  }
  if (slots_ == 0) {
    // A buffer with no periods has nothing to hold an arrival and nothing to
    // surrender. The frames are still counted, like every other arrival that
    // cannot be placed.
    dropped_ += period_frames_;
    return PushStatus::overrun;
  }

  if (!primed_) {
    // The first period decides where the playout head is. Before it arrives
    // there is no such thing as late, and playing from position zero would be
    // inventing audio this receiver never had.
    head_ = sample_position;
    contiguous_end_ = sample_position;
    primed_ = true;
  } else if (sample_position < head_) {
    // Its moment has passed: the head is already beyond it, so it cannot be
    // played without going back, and seeking back would be a correction our
    // quality rule does not allow (ADR 0003).
    late_ += period_frames_;
    return PushStatus::late;
  }

  PushStatus status = PushStatus::stored;
  const uint64_t capacity = capacity_frames();
  if (sample_position >= head_ + capacity) {
    // At capacity, which means the playout head has fallen a whole buffer behind
    // the sender. The oldest periods are the ones whose delay is least
    // affordable, so the head moves forward to make room — by one period in the
    // ordinary case, where it is one period past a full buffer.
    uint64_t new_head = head_ + period_frames_;
    if (sample_position - head_ > capacity) {
      // Further than that: the sender's timeline has moved more than the buffer
      // could follow — a link that went away and came back, or a far end that
      // restarted. Seating the head at the arrival is the only choice that
      // leaves the stream playable; anywhere between leaves an unfillable hole
      // in front of the head, and the receiver never plays another sample.
      new_head = sample_position;
    }
    // Counted before the head moves, because what it costs is what the old
    // window held below where it is going.
    const uint64_t lost = frames_held_below(new_head);
    if (lost > 0) {
      status = PushStatus::overrun;
    }
    dropped_ += lost;
    head_ = new_head;
    if (contiguous_end_ < head_) {
      // Nothing held reaches the new head, so nothing is contiguous from it.
      contiguous_end_ = head_;
    }
  }

  const size_t slot = slot_of(sample_position);
  if (positions_[slot] == sample_position) {
    // Already here. A retransmit of something we hold is not audio to play
    // again, and overwriting our copy with the same bytes would be churn.
    ++refused_;
    return PushStatus::duplicate;
  }
  // The slot is free: a position is inside the window when it is stored, two
  // aligned positions cannot share a window, and the head never passes a period
  // that is held. So this stores and never overwrites.
  std::memcpy(&audio_[slot * period_bytes_], period, period_bytes_);
  positions_[slot] = sample_position;
  if (sample_position == contiguous_end_) {
    advance_contiguous();
  }
  pushed_ += period_frames_;
  return status;
}

bool PlayoutBuffer::take(uint8_t* period, uint64_t* sample_position) {
  if (period == nullptr) {
    return false;  // a caller bug rather than an underrun; see the header
  }
  if (!primed_) {
    ++underruns_;
    return false;
  }

  if (head_ >= contiguous_end_) {
    // The head has reached a hole. If a period is held *beyond* it, the hole can
    // never be filled — SRT delivers in order, so a period after one that arrived
    // will not arrive later. Cross one period of it and report an underrun, so
    // the caller plays silence for it and calls again: the gap is concealed at
    // real time rather than skipped, which keeps A/V alignment. Without this a
    // single lost frame stalled the head at the hole for ever (issue #20).
    if (next_held_at_or_after(head_) == k_empty) {
      // Nothing beyond: a genuine underrun, waiting for audio that may still come.
      ++underruns_;
      return false;
    }
    head_ += period_frames_;
    concealed_ += period_frames_;
    contiguous_end_ = head_;
    advance_contiguous();
    // Always report this take as an underrun: the period crossed was silence,
    // and the caller must play exactly one period for it before the held audio
    // is reached. Falling through to play would advance the head by two periods
    // in one take and skip the gap in time rather than conceal it.
    ++underruns_;
    return false;
  }

  const size_t slot = slot_of(head_);
  // The head is inside the contiguous run and every position in that run is
  // held, so this is the period the sender put at this position.
  std::memcpy(period, &audio_[slot * period_bytes_], period_bytes_);
  positions_[slot] = k_empty;
  if (sample_position != nullptr) {
    *sample_position = head_;
  }
  head_ += period_frames_;
  played_ += period_frames_;
  return true;
}

uint64_t PlayoutBuffer::discard_to_level_ms(double keep_ms) {
  if (keep_ms < 0.0) {
    keep_ms = 0.0;
  }
  uint64_t keep = static_cast<uint64_t>(
      keep_ms * static_cast<double>(format_.sample_rate) / 1000.0);
  keep -= keep % period_frames_;
  const uint64_t held = held_frames();
  if (held <= keep) {
    return 0;
  }
  const uint64_t drop = held - keep;
  head_ += drop;
  if (contiguous_end_ < head_) {
    contiguous_end_ = head_;
  }
  // Counted as dropped: it is audio the sender delivered and we chose not to
  // play, because playing it would only add latency.
  dropped_ += drop;
  advance_contiguous();
  return drop;
}

uint64_t PlayoutBuffer::next_held_at_or_after(uint64_t position) const {
  const uint64_t end = head_ + capacity_frames();
  for (uint64_t candidate = position; candidate < end;
       candidate += period_frames_) {
    if (positions_[slot_of(candidate)] == candidate) {
      return candidate;
    }
  }
  return k_empty;
}

void PlayoutBuffer::advance_contiguous() {
  // Amortised: every period this walks past was stored once and is played once.
  // The window bound is what keeps a stale slot outside the window from being
  // read as held.
  while (contiguous_end_ < head_ + capacity_frames() &&
         positions_[slot_of(contiguous_end_)] == contiguous_end_) {
    contiguous_end_ += period_frames_;
  }
}

}  // namespace aes67_srt::clock
