#include "engine.hpp"

#include <chrono>
#include <cstring>
#include <thread>

#include "log.hpp"
#include "util.hpp"

namespace aes67_srt {
namespace {

/**
 * How long a receive waits for a message before letting the loop turn over.
 *
 * **Zero, and that is non-blocking — verified, not assumed.** `SRTO_RCVTIMEO`
 * "limits the time up to which the receiving operation will block ... The -1 value
 * means no time limit" (Haivision SRT, `docs/API/API-socket-options.md`), so 0
 * returns immediately with `SRT_ETIMEOUT` when nothing has arrived. It has to: this
 * loop is paced by the device and owes it exactly one period every millisecond, and
 * a positive timeout here is multiplied by the drain loop — up to 24 messages per
 * frame, 8 frames per turn — which stalls the loop for tens of milliseconds. The
 * device is then starved, the playout buffer floods and overruns, and the receiver
 * delivers a few percent of what the sender offers. Measured on a real SRT link:
 * **42 frames a second with a 2 ms timeout, 1000 with 0**, and the app-level
 * loopback never blocks, so only a socket test can see the difference.
 */
constexpr int k_receive_poll_ms = 0;

/** A direction that failed waits this long before trying again. */
constexpr int k_retry_delay_ms = 10;

/**
 * How often the link's statistics are logged, and the sleep slice that divides it.
 *
 * A second is what the two-ended run needed and could not get: the transport
 * exposes RTT, loss, retransmits, bandwidth and buffer delay, and nothing printed
 * them, so a link delivering a fraction of what it offered had no answer *inside
 * the run*. The slice exists so a stop request is answered promptly rather than
 * after a whole interval.
 */
constexpr int k_status_interval_ms = 1000;
constexpr int k_status_slice_ms = 100;

/**
 * The most messages one iteration will consume.
 *
 * A frame is eight messages of `k_max_message_bytes`, so this is a couple of
 * frames with room to spare. The cap is what stops a link that has fallen
 * behind from being drained in one unbounded burst: the device has to be fed
 * every millisecond, and a burst would starve it, which is the failure this loop
 * exists to prevent.
 */
constexpr int k_max_messages_per_period = 24;

/**
 * How many complete frames one turn of the receive loop will move into the buffer.
 *
 * The cap is not about the device, which is fed every turn regardless — it is about
 * the control loop. A buffer that absorbed a whole backlog in one turn would show
 * the control a level spike that is already over, and the correction would chase
 * it; eight milliseconds is enough for a burst to be *visible* in the level, which
 * is where delay growth belongs (decision 6), without the loop mistaking a burst
 * for a rate error.
 */
constexpr int k_max_frames_per_period = 8;

/** Frames in one period, as a double, for the control's time base. */
double period_ms(const audio::AudioFormat& format) {
  if (format.sample_rate == 0) {
    return 0.0;
  }
  return static_cast<double>(format.period_frames) * 1000.0 / format.sample_rate;
}

/**
 * The A/V delay line's ceiling, in milliseconds.
 *
 * It matches the 0..5000 the configuration validator accepts, and it is spec open
 * item 7 that will move it: whether the product's range is 0-2 s or 0-5 s is a
 * decision about the vision path, not about this module.
 */
constexpr double k_max_egress_delay_ms = 5000.0;

/**
 * A duration in milliseconds as a number of periods.
 *
 * The clock counts in periods because that is what it holds, and configuration is
 * in milliseconds because that is what an operator sets. Rounded up, so a level is
 * never targeted below what was asked for.
 */
uint64_t periods_for_ms(int ms, const audio::AudioFormat& format) {
  if (ms <= 0 || format.sample_rate == 0 || format.period_frames == 0) {
    return 0;
  }
  const double periods = static_cast<double>(ms) * format.sample_rate /
                         (1000.0 * static_cast<double>(format.period_frames));
  return static_cast<uint64_t>(periods + 0.5);
}

bool fail(std::string* error, const std::string& message) {
  if (error != nullptr) {
    *error = message;
  }
  return false;
}

}  // namespace

Engine::Engine() = default;

Engine::~Engine() {
  stop();
  close();
}

bool Engine::prepare(const Config& config, std::string* error) {
  config_ = config;

  if (config.blocks.size() * wire::k_block_channels !=
      static_cast<size_t>(config.audio.channels)) {
    // Validated configurations cannot be here; one that is would drop audio on
    // the floor, because a block list that does not cover the device leaves
    // those channels out of every frame with nothing reporting it.
    return fail(error, "engine: " + std::to_string(config.blocks.size()) +
                           " blocks of 8 channels do not cover audio.channels " +
                           std::to_string(config.audio.channels));
  }

  format_ = audio::audio_format_from(config.audio);
  backend_ = audio::create_audio_backend(config.audio);
  link_.reset(new transport::Link());

  // The clock's geometry, checked here rather than at open: a capacity that is not
  // comfortably above the level's target is a buffer that will overrun the first
  // time a link sags, and that is a configuration error, not a runtime one.
  target_periods_ = periods_for_ms(config.link.latency_ms, format_);
  const uint64_t alarm_periods =
      periods_for_ms(config.link.alarm_delay_ms, format_);
  const uint64_t capacity_periods = 2 * alarm_periods + 1;
  if (target_periods_ == 0 || capacity_periods <= alarm_periods + target_periods_) {
    return fail(
        error, "engine: link.latency_ms " + std::to_string(config.link.latency_ms) +
                   " and link.alarm_delay_ms " +
                   std::to_string(config.link.alarm_delay_ms) +
                   " leave no room for a playout buffer: the alarm has to be "
                   "well above the latency it alarms about");
  }

  tx_period_.assign(format_.period_bytes(), 0);
  rx_period_.assign(format_.period_bytes(), 0);
  return true;
}

bool Engine::pack_period(const uint8_t* period, uint64_t sample_position,
                         wire::Frame* frame, std::string* error) const {
  if (frame == nullptr) {
    return fail(error, "engine: no frame to fill");
  }
  if (period == nullptr) {
    return fail(error, "engine: no period to pack");
  }

  wire::Frame packed;
  packed.link_id = link_id_;
  packed.sample_position = sample_position;
  // The payload type follows the bytes actually in hand, not the configuration
  // string, so a disagreement between the two cannot produce a frame that claims
  // to be L24 while carrying 16-bit samples.
  const wire::PayloadType payload = format_.sample_bytes == 2
                                        ? wire::PayloadType::pcm_l16
                                        : wire::PayloadType::pcm_l24;

  for (const BlockConfig& block : config_.blocks) {
    if (block.channels.size() != wire::k_block_channels) {
      return fail(error, "engine: block " + std::to_string(block.index) + " maps " +
                             std::to_string(block.channels.size()) +
                             " channels; a block is exactly 8, because AES67 "
                             "allows no more in one stream");
    }
    wire::Block wire_block;
    wire_block.index = static_cast<uint8_t>(block.index);
    wire_block.payload = payload;
    wire_block.channels = wire::k_block_channels;
    wire_block.data.assign(
        wire::payload_bytes(payload, wire::k_block_channels, format_.period_frames),
        0);

    for (size_t frame_index = 0; frame_index < format_.period_frames;
         ++frame_index) {
      for (size_t slot = 0; slot < block.channels.size(); ++slot) {
        const int device_channel = block.channels[slot];
        if (device_channel < 0 ||
            device_channel >= static_cast<int>(format_.channels)) {
          return fail(error, "engine: block " + std::to_string(block.index) +
                                 " maps device channel " +
                                 std::to_string(device_channel) +
                                 ", beyond audio.channels " +
                                 std::to_string(format_.channels));
        }
        // The only place the device-channel-to-block mapping is applied on the
        // way out, and it is the same mapping the daemon's source document
        // declares — one configuration, used twice.
        const size_t from =
            (frame_index * format_.channels + static_cast<size_t>(device_channel)) *
            format_.sample_bytes;
        const size_t to =
            (frame_index * block.channels.size() + slot) * format_.sample_bytes;
        std::memcpy(&wire_block.data[to], period + from, format_.sample_bytes);
      }
    }
    packed.blocks.push_back(std::move(wire_block));
  }

  *frame = std::move(packed);
  return true;
}

bool Engine::unpack_frame(const wire::Frame& frame, uint8_t* period,
                          std::string* error) const {
  if (period == nullptr) {
    return fail(error, "engine: no period to fill");
  }
  if (frame.link_id != link_id_) {
    return fail(error, "engine: frame from link " + std::to_string(frame.link_id) +
                           ", not ours (" + std::to_string(link_id_) + ")");
  }
  if (frame.blocks.size() != config_.blocks.size()) {
    return fail(error, "engine: frame carries " +
                           std::to_string(frame.blocks.size()) +
                           " blocks, this link expects " +
                           std::to_string(config_.blocks.size()));
  }

  // Silence first: a channel this frame does not carry is left quiet rather than
  // left holding whatever the buffer had in it a millisecond ago.
  std::memset(period, 0, format_.period_bytes());

  std::vector<bool> filled(config_.blocks.size(), false);
  for (const wire::Block& block : frame.blocks) {
    if (block.index >= config_.blocks.size()) {
      return fail(error, "engine: frame carries block " +
                             std::to_string(block.index) + ", and this link has " +
                             std::to_string(config_.blocks.size()));
    }
    if (filled[block.index]) {
      return fail(error, "engine: frame carries block " +
                             std::to_string(block.index) + " twice");
    }
    filled[block.index] = true;

    const size_t block_sample_bytes = wire::sample_bytes(block.payload);
    if (block_sample_bytes != format_.sample_bytes) {
      // Refused rather than reinterpreted: playing 16-bit samples as 24-bit is
      // not an error any mixer downstream would report, it is just wrong audio.
      return fail(error, "engine: block " + std::to_string(block.index) +
                             " carries " + wire::to_string(block.payload) +
                             ", and this device is " +
                             (format_.sample_bytes == 2 ? "s16_le" : "s24_3le"));
    }

    const BlockConfig& destination = config_.blocks[block.index];
    for (size_t frame_index = 0; frame_index < format_.period_frames;
         ++frame_index) {
      for (size_t slot = 0; slot < destination.channels.size(); ++slot) {
        const int device_channel = destination.channels[slot];
        if (device_channel < 0 ||
            device_channel >= static_cast<int>(format_.channels)) {
          return fail(error, "engine: block " + std::to_string(block.index) +
                                 " maps device channel " +
                                 std::to_string(device_channel) +
                                 ", beyond audio.channels " +
                                 std::to_string(format_.channels));
        }
        const size_t from =
            (frame_index * block.channels + slot) * block_sample_bytes;
        if (from + block_sample_bytes > block.data.size()) {
          return fail(error, "engine: block " + std::to_string(block.index) +
                                 " is shorter than its own header claims");
        }
        const size_t to =
            (frame_index * format_.channels + static_cast<size_t>(device_channel)) *
            format_.sample_bytes;
        std::memcpy(period + to, &block.data[from], block_sample_bytes);
      }
    }
  }
  return true;
}

bool Engine::step_transmit(std::string* error) {
  // A read that fails has already filled the period with silence, so this
  // returns before anything is built rather than sending a frame full of
  // whatever was in the buffer.
  if (!backend_->read(tx_period_.data(), format_.period_frames, error)) {
    return false;
  }

  wire::Frame frame;
  if (!pack_period(tx_period_.data(), sample_position_, &frame, error)) {
    return false;
  }
  // Same thread as pack_period, so the sequence and the sample position advance
  // together and neither can be seen by the receive thread mid-update.
  frame.sequence = sequence_;
  if (!wire::encode(frame, &tx_bytes_, error)) {
    return false;
  }

  size_t offset = 0;
  const uint8_t* chunk = nullptr;
  size_t chunk_size = 0;
  while (wire::next_fragment(tx_bytes_.data(), tx_bytes_.size(), &offset, &chunk,
                             &chunk_size)) {
    if (!link_->send_message(chunk, chunk_size, error)) {
      return false;
    }
  }

  ++sequence_;
  sample_position_ += format_.period_frames;
  ++frames_sent_;
  return true;
}

bool Engine::step_receive(std::string* error) {
  if (playout_ == nullptr) {
    return fail(error, "engine: the clock is not built; call prepare and open");
  }

  // Whatever has arrived goes into the buffer, and then exactly one period comes
  // out of it for the device. That one-way-in, one-way-out shape is what makes the
  // level a real measurement: it moves only when the sender's rate and ours differ,
  // which is the thing the control steers.
  receive_into_buffer(error);
  if (error != nullptr && !error->empty()) {
    return false;
  }
  return play_one_period(error);
}

/**
 * Move every complete frame that has arrived into the playout buffer.
 *
 * A frame this link does not expect — or one whose CRC does not match — is refused
 * and counted rather than guessed at, exactly as it was before the clock landed
 * here.
 */
void Engine::receive_into_buffer(std::string* error) {
  const auto refuse = [this](const std::string& reason) {
    ++frames_refused_;
    log().write(LogLevel::warn, "engine: refused " + reason);
  };

  for (int frame_count = 0; frame_count < k_max_frames_per_period; ++frame_count) {
    // Drain whatever has arrived, and no more than a couple of frames' worth. What
    // the drain does *not* do is decide how much audio is played: that is the
    // buffer's, one period per turn, for ever, whatever the link is doing.
    bool timed_out = false;
    for (int received = 0; received < k_max_messages_per_period; ++received) {
      if (!link_->receive_message(&message_, &timed_out, error)) {
        if (!timed_out) {
          return;  // the link itself failed; a quiet one is not a failure
        }
        break;
      }
      if (!reassembler_.feed(message_.data(), message_.size(), error)) {
        // Something on this connection is not our format. The reassembler has given
        // up on it and will resynchronise on the next frame boundary.
        refuse(std::string("a stream that is not this format: ") +
               (error != nullptr ? *error : "unknown"));
      }
    }

    if (!reassembler_.frame_ready()) {
      return;  // nothing complete this turn; the rest of the turn plays what we
               // have
    }
    if (!reassembler_.take_frame(&rx_bytes_, error)) {
      return;
    }
    if (rx_bytes_.empty()) {
      return;
    }

    wire::Frame frame;
    std::string reason;
    if (!wire::decode(rx_bytes_.data(), rx_bytes_.size(), &frame, &reason)) {
      // The CRC exists to catch corruption rather than play it (ADR 0001), so a
      // frame that will not decode is refused and counted, not guessed at.
      refuse("a frame that would not decode: " + reason);
    } else if (!unpack_frame(frame, rx_period_.data(), &reason)) {
      refuse("a frame this link does not expect: " + reason);
    } else {
      // Held by the *sender's* sample position, which is the anchor the wire format
      // carries for exactly this. A position already played, or further ahead than
      // the buffer reaches, is refused by the buffer and counted there.
      const clock::PushStatus status = playout_->push(
          frame.sample_position, rx_period_.data(), format_.period_bytes());
      if (status == clock::PushStatus::stored) {
        ++frames_received_;
      } else if (status == clock::PushStatus::overrun) {
        ++frames_received_;
        // The one case where audio is lost, and it is the last resort rather than a
        // correction: the sender is more than a whole buffer ahead of the playout
        // head, so the oldest periods go. The level is the figure that says so.
        log().write(
            LogLevel::warn,
            "playout buffer full: " + std::to_string(playout_->frames_dropped()) +
                " frames surrendered in total; the delay is " +
                std::to_string(playout_->level_ms()) + " ms");
      } else {
        refuse(std::string("a frame the buffer would not hold (") +
               clock::to_string(status) + ") at position " +
               std::to_string(frame.sample_position));
      }
    }
    rx_bytes_.clear();
  }
}

/**
 * One period for the device, through the clock.
 *
 * The pull is what consumes the sender's audio at the ratio the control asks for;
 * the write is what paces the loop. Silence when there is nothing playable, because
 * a device that is not written to stops being asked for audio and the RAVENNA
 * driver's engine then goes idle — the silent stall this module already carries
 * recovery for.
 */
bool Engine::play_one_period(std::string* error) {
  if (!playing_) {
    // The receiver waits for the level the latency setting bought, and starts
    // anyway once the deadline has passed: a link that never fills must still be
    // audible, and the level will be whatever it is.
    ++priming_periods_;
    const bool reached =
        playout_->held_frames() >= target_periods_ * format_.period_frames;
    const bool late = priming_periods_ >= prime_deadline_periods_;
    if (!reached && !late) {
      std::memset(rx_period_.data(), 0, rx_period_.size());
      ++silence_periods_;
      return write_period_to_device(error);
    }
    playing_ = true;
    if (!reached && !prime_reported_) {
      prime_reported_ = true;
      log().write(
          LogLevel::warn,
          "the playout level never reached its target; playing anyway, and the "
          "clock will pull it up");
    }
  }

  // The ratio first, so the pull uses this turn's correction rather than the last
  // turn's: the control steers from the level, and the level is what this pull
  // changes.
  resampler_.set_ratio(control_.ratio());

  uint64_t position = 0;
  std::string reason;
  const uint64_t short_before = resampler_.short_pulls();
  const bool played = resampler_.pull(playout_.get(), rx_period_.data(),
                                      format_.period_frames, &position, &reason);
  // A pull that could not be filled is *success* to the resampler — the device is
  // owed a period and the rest is silence — so the only way to know it happened is
  // the resampler's own count. Without this the engine reports a level sitting high
  // while the device is fed silence and counts none of it, which is precisely the
  // situation worth seeing: a link that cannot deliver what it offers.
  const bool short_pull = resampler_.short_pulls() != short_before;
  if (!played || short_pull) {
    // Either the buffer has nothing playable, the resampler could not fill the
    // period, or the resampler refused; all three mean the device is owed a period
    // it cannot have. Silence, counted, and named in the log the first time and
    // then every thousand periods — a fault that repeats every millisecond must not
    // flood the log it appears in.
    ++silence_periods_;
    if (silence_periods_ == 1 || silence_periods_ % 1000 == 0) {
      log().write(LogLevel::warn,
                  "no audio to play (" + std::to_string(silence_periods_) +
                      " periods of silence so far, delay " +
                      std::to_string(delay_ms()) + " ms): " + reason);
    }
    std::memset(rx_period_.data(), 0, rx_period_.size());
  }

  // The control's time base is the device's: one period per turn, because that is
  // what paces this loop. It reads the level after the pull, which is the level the
  // device is about to be short of if the sender is behind.
  control_.update(playout_->level_ms(), period_ms(format_));

  return write_period_to_device(error);
}

/**
 * The egress stage: mix the test signal, delay the period, hand it to the device.
 *
 * In-place: `DelayLine::process` writes the input into its ring before it reads
 * anything back out, so a period can be its own source and destination.
 * `egress_frames_` advances with what is *played*, not with what is pulled, so a
 * test signal scheduled against it lands where an operator watching the device
 * would say it did.
 */
bool Engine::write_period_to_device(std::string* error) {
  test_signal_.mix(rx_period_.data(), format_.period_frames, egress_frames_);
  if (!delay_line_.process(rx_period_.data(), rx_period_.data(),
                           format_.period_frames, error)) {
    return false;
  }
  egress_frames_ += format_.period_frames;
  return backend_->write(rx_period_.data(), format_.period_frames, error);
}

double Engine::delay_ms() const {
  return playout_ != nullptr ? playout_->level_ms() : 0.0;
}

double Engine::delay_fraction() const {
  return playout_ != nullptr ? playout_->level_fraction() : 0.0;
}

double Engine::egress_delay_ms() const {
  return delay_line_.offset_ms();
}

bool Engine::set_egress_delay_ms(double offset_ms, std::string* error) {
  return delay_line_.set_offset_ms(offset_ms, error);
}

bool Engine::trigger_test_signal(std::string* error) {
  // The next period, which is the one about to be written: a mark that landed in
  // the past would be dropped by the queue rather than heard.
  return test_signal_.trigger(egress_frames_, error);
}

double Engine::codec_delay_ms() const {
  // v1 encodes nothing, so the codec's share of the A/V budget is zero. Phase 2
  // puts an Opus frame + lookahead here; docs/research/opus.md has the measured
  // 6.50 ms and the roadmap has the question of whether the test signal survives
  // the codec at all.
  return 0.0;
}

double Engine::av_delay_ms() const {
  return static_cast<double>(config_.link.latency_ms) + delay_ms() +
         egress_delay_ms() + codec_delay_ms();
}

double Engine::clock_offset_ppm() const {
  return control_.offset_ppm();
}

double Engine::clock_ratio() const {
  return control_.ratio();
}

uint64_t Engine::silence_periods() const {
  return silence_periods_;
}

void Engine::transmit_loop() {
  unsigned consecutive_failures = 0;
  std::string error;
  while (!stop_requested_.load()) {
    if (step_transmit(&error)) {
      consecutive_failures = 0;
      continue;
    }
    // A period that could not be produced is not a reason to stop the appliance.
    // The device may be starved, unlocked PTP produces nothing, or the link may
    // have blipped — all conditions that recover on their own and are counted
    // where they happen. It *is* a reason not to spin: a failing loop with no
    // wait takes the whole core.
    ++consecutive_failures;
    if (consecutive_failures == 1 || consecutive_failures % 500 == 0) {
      log().write(LogLevel::warn, "engine: transmit: " + error + " (" +
                                      std::to_string(consecutive_failures) +
                                      " consecutive)");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(k_retry_delay_ms));
  }
}

void Engine::receive_loop() {
  unsigned consecutive_failures = 0;
  std::string error;
  while (!stop_requested_.load()) {
    if (step_receive(&error)) {
      consecutive_failures = 0;
      continue;
    }
    ++consecutive_failures;
    if (consecutive_failures == 1 || consecutive_failures % 500 == 0) {
      log().write(LogLevel::warn, "engine: receive: " + error + " (" +
                                      std::to_string(consecutive_failures) +
                                      " consecutive)");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(k_retry_delay_ms));
  }
}

void Engine::status_loop() {
  bool reported_reason = false;
  while (!stop_requested_.load()) {
    for (int slice = 0; slice < k_status_interval_ms / k_status_slice_ms &&
                        !stop_requested_.load();
         ++slice) {
      std::this_thread::sleep_for(std::chrono::milliseconds(k_status_slice_ms));
    }
    if (stop_requested_.load()) {
      break;
    }

    transport::LinkStats stats;
    std::string error;
    if (link_->stats(&stats, &error)) {
      reported_reason = false;
      log().write(LogLevel::info, "link: " + transport::to_string(stats));
    } else if (!reported_reason) {
      // A loopback has no statistics and a link that is not up has none either.
      // Say why once, then again only after it has worked: a line every second
      // saying nothing is how a real fault hides in the log.
      reported_reason = true;
      log().write(LogLevel::info, "link statistics: " + error);
    }
  }
}

bool Engine::open(std::string* error) {
  if (!backend_->open(format_, error)) {
    return false;
  }
  if (!link_->open(config_, error)) {
    backend_->close();
    return false;
  }
  // Non-blocking rather than short: this loop has a device to feed every
  // millisecond, and any wait in the receive path is a wait the device pays for.
  // See k_receive_poll_ms for why the 2 ms this used to be was not short enough,
  // and why 0 is documented to be non-blocking rather than guessed.
  link_->set_receive_timeout_ms(k_receive_poll_ms);

  // The clock, built per open rather than per process: a link that is reopened is a
  // stream whose sample positions may well start somewhere else, and a buffer still
  // holding the previous one's audio would play it as if it were this one's.
  const uint64_t capacity_periods =
      2 * periods_for_ms(config_.link.alarm_delay_ms, format_) + 1;
  playout_.reset(new clock::PlayoutBuffer(capacity_periods, format_));

  clock::RatioControl::Config control_config;
  control_config.target_ms =
      static_cast<double>(target_periods_) * period_ms(format_);
  control_ = clock::RatioControl(control_config);

  clock::Resampler::Config resampler_config;
  resampler_config.channels = format_.channels;
  resampler_config.period_frames = format_.period_frames;
  // The converter is the resampler's own default, which is the measured choice:
  // libsamplerate's three sinc converters are all 97 dB and differ in bandwidth,
  // and the cheapest loses the top of the audible band.
  // docs/research/clock-recovery.md has the table, measured on the Pi.
  std::string reason;
  if (!resampler_.open(resampler_config, &reason)) {
    link_->close();
    backend_->close();
    return fail(error, "engine: the clock's resampler will not open: " + reason);
  }
  resampler_.set_ratio(1.0);

  // The egress stage (ticket 12), fresh per open for the same reason the clock is:
  // a reopened link must not start with the previous stream's audio still in the
  // delay line. The line is sized from the same 5000 ms the validator accepts, and
  // the offset it is opened with is applied immediately rather than crossfaded from
  // zero — there is nothing to fade from before the first period.
  delay::DelayLine::Config delay_config;
  delay_config.channels = format_.channels;
  delay_config.sample_rate = format_.sample_rate;
  delay_config.period_frames = format_.period_frames;
  delay_config.sample_bytes = format_.sample_bytes;
  delay_config.capacity_ms = k_max_egress_delay_ms;
  if (!delay_line_.open(delay_config, &reason)) {
    resampler_.close();
    link_->close();
    backend_->close();
    return fail(error, "engine: the A/V delay line will not open: " + reason);
  }
  if (!delay_line_.set_offset_ms(config_.egress.delay_ms, &reason)) {
    delay_line_.close();
    resampler_.close();
    link_->close();
    backend_->close();
    return fail(error, "engine: egress.delay_ms: " + reason);
  }

  delay::TestSignal::Config signal_config;
  signal_config.channels = format_.channels;
  signal_config.sample_rate = format_.sample_rate;
  signal_config.period_frames = format_.period_frames;
  signal_config.sample_bytes = format_.sample_bytes;
  signal_config.channel = config_.egress.test_signal_channel;
  if (!test_signal_.open(signal_config, &reason)) {
    delay_line_.close();
    resampler_.close();
    link_->close();
    backend_->close();
    return fail(error, "engine: the test signal will not open: " + reason);
  }

  playing_ = false;
  priming_periods_ = 0;
  prime_reported_ = false;
  silence_periods_ = 0;
  egress_frames_ = 0;
  // A receiver does not play the instant the first period arrives: it waits until
  // the level is what the latency setting bought. The deadline is what keeps a link
  // that never fills from being silence for ever — after it, playout starts with
  // whatever there is, and the control pulls the level up toward the target.
  prime_deadline_periods_ = periods_for_ms(config_.link.alarm_delay_ms, format_);
  return true;
}

int Engine::run() {
  std::string error;
  if (!open(&error)) {
    log().write(LogLevel::error, "engine: cannot start: " + error);
    return 1;
  }

  const std::string role = to_lower(config_.link.role);
  const bool transmit = role == "tx" || role == "duplex";
  const bool receive = role == "rx" || role == "duplex";

  log().write(LogLevel::info, "engine: running " + role + " via " +
                                  link_->peer_description() + ", " +
                                  backend_->detail());

  stop_requested_ = false;
  running_ = true;
  std::thread transmit_thread;
  std::thread receive_thread;
  std::thread status_thread;
  if (transmit) {
    transmit_thread = std::thread([this] { transmit_loop(); });
  }
  if (receive) {
    receive_thread = std::thread([this] { receive_loop(); });
  }
  // The link's own statistics go to the log once a second, whichever directions
  // this end runs: they are the only thing that can say why a link delivered what
  // it did, and a run without them answers that question a day later, in a
  // comment, from memory (ticket 09).
  status_thread = std::thread([this] { status_loop(); });
  if (transmit_thread.joinable()) {
    transmit_thread.join();
  }
  if (receive_thread.joinable()) {
    receive_thread.join();
  }
  if (status_thread.joinable()) {
    status_thread.join();
  }
  running_ = false;

  log().write(LogLevel::info,
              "engine: stopped after " + std::to_string(frames_sent_) +
                  " frames sent, " + std::to_string(frames_received_) +
                  " received, " + std::to_string(frames_refused_) + " refused" +
                  // What the clock ended up holding, and what it decided the offset
                  // between the two clocks was: the two numbers an operator or a
                  // hardware session wants out of a run, and the ones the control
                  // surface will show continuously (ticket 12).
                  "; playout delay " + std::to_string(delay_ms()) +
                  " ms, clock correction " + std::to_string(clock_offset_ppm()) +
                  " ppm, A/V offset " + std::to_string(egress_delay_ms()) +
                  " ms, " + std::to_string(silence_periods_) +
                  " periods of silence");
  close();
  return 0;
}

void Engine::stop() {
  stop_requested_ = true;
}

bool Engine::running() const {
  return running_.load();
}

void Engine::close() {
  test_signal_.close();
  delay_line_.close();
  resampler_.close();
  if (link_) {
    link_->close();
  }
  if (backend_) {
    backend_->close();
  }
}

audio::AudioBackend* Engine::backend() {
  return backend_.get();
}

const audio::AudioBackend* Engine::backend() const {
  return backend_.get();
}

uint64_t Engine::frames_sent() const {
  return frames_sent_;
}

uint64_t Engine::frames_received() const {
  return frames_received_;
}

uint64_t Engine::frames_refused() const {
  return frames_refused_;
}

}  // namespace aes67_srt
