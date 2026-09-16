#include "engine.hpp"

#include <chrono>
#include <cstring>
#include <thread>

#include "log.hpp"
#include "util.hpp"

namespace aes67_srt {
namespace {

/** How long a drain waits for a message before letting the loop turn over. */
constexpr int k_receive_poll_ms = 2;

/** A direction that failed waits this long before trying again. */
constexpr int k_retry_delay_ms = 10;

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
  const auto refuse = [this](const std::string& reason) {
    ++frames_refused_;
    log().write(LogLevel::warn, "engine: refused " + reason);
  };

  // Drain whatever has arrived, and no more than a couple of frames' worth: the
  // device is waiting to be fed, and a burst that emptied a backlogged link in
  // one go would starve it.
  bool timed_out = false;
  for (int received = 0; received < k_max_messages_per_period; ++received) {
    if (!link_->receive_message(&message_, &timed_out, error)) {
      if (!timed_out) {
        return false;  // the link itself failed; a quiet one is not a failure
      }
      break;
    }
    if (!reassembler_.feed(message_.data(), message_.size(), error)) {
      // Something on this connection is not our format. The reassembler has
      // given up on it and will resynchronise on the next frame boundary.
      refuse(std::string("a stream that is not this format: ") +
             (error != nullptr ? *error : "unknown"));
    }
  }

  bool have_audio = false;
  if (reassembler_.frame_ready()) {
    if (!reassembler_.take_frame(&rx_bytes_, error)) {
      return false;
    }
  }
  if (!rx_bytes_.empty()) {
    wire::Frame frame;
    std::string reason;
    if (!wire::decode(rx_bytes_.data(), rx_bytes_.size(), &frame, &reason)) {
      // The CRC exists to catch corruption rather than play it (ADR 0001), so a
      // frame that will not decode is refused and counted, not guessed at.
      refuse("a frame that would not decode: " + reason);
    } else if (!unpack_frame(frame, rx_period_.data(), &reason)) {
      refuse("a frame this link does not expect: " + reason);
    } else {
      have_audio = true;
      ++frames_received_;
    }
    rx_bytes_.clear();
  }

  if (!have_audio) {
    // Nothing complete this turn of the loop. Play silence rather than leaving
    // the playback device unfed: a device that is not written to stops being
    // asked for audio, and the RAVENNA driver's engine then goes idle — which is
    // the silent stall this module already carries recovery for. The outage is
    // still visible, in the counters and in whether anything is being received.
    std::memset(rx_period_.data(), 0, rx_period_.size());
  }

  // This is what paces the receive loop: one period per iteration, and the
  // device blocks here until its ring has room for it.
  return backend_->write(rx_period_.data(), format_.period_frames, error);
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

bool Engine::open(std::string* error) {
  if (!backend_->open(format_, error)) {
    return false;
  }
  if (!link_->open(config_, error)) {
    backend_->close();
    return false;
  }
  // Short rather than blocking: this loop has a device to feed every
  // millisecond, and a receive that waited for the peer would turn a quiet link
  // into a stutter. What a timeout of *zero* means to libsrt is not recorded in
  // docs/research/libsrt.md and the installed header says only "recv()
  // timeout", so this stays a small positive number rather than resting on an
  // assumption about it.
  link_->set_receive_timeout_ms(k_receive_poll_ms);
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
  if (transmit) {
    transmit_thread = std::thread([this] { transmit_loop(); });
  }
  if (receive) {
    receive_thread = std::thread([this] { receive_loop(); });
  }
  if (transmit_thread.joinable()) {
    transmit_thread.join();
  }
  if (receive_thread.joinable()) {
    receive_thread.join();
  }
  running_ = false;

  log().write(LogLevel::info, "engine: stopped after " +
                                  std::to_string(frames_sent_) + " frames sent, " +
                                  std::to_string(frames_received_) + " received, " +
                                  std::to_string(frames_refused_) + " refused");
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
