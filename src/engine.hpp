#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "audio/backend.hpp"
#include "clock/playout_buffer.hpp"
#include "clock/ratio_control.hpp"
#include "clock/resampler.hpp"
#include "config.hpp"
#include "transport/link.hpp"
#include "wire/frame.hpp"

namespace aes67_srt {

/**
 * The engine: the audio device to the wire and back.
 *
 * Everything before this module was a piece. This is the first thing that joins
 * them, and the first thing in the project that moves audio from a device into a
 * frame and onto a link.
 *
 * **Shape: one device-paced loop, per direction.** Each direction gets its own
 * thread, because each is paced by something different and neither may wait for
 * the other:
 *
 *   transmit  read a period from the device -> cut it into blocks -> one frame
 *             -> encode -> slice into SRT messages -> send
 *   receive   receive messages -> reassemble -> decode -> one frame -> join the
 *             blocks back into a period -> write it to the device
 *
 * The device is what paces each loop: `read()` blocks until the capture device
 * has a period, and `write()` blocks until the playback ring has room. That is
 * deliberate — a timer of our own would be a second timebase to reconcile, and
 * the whole design (ADR 0001) rests on there being as few of those as possible.
 * The cost is that the two directions are coupled *within* a direction's own
 * thread only; a stalled link slows its own direction and nothing else.
 *
 * What the clock does here, and what it costs the loopback. The receive path now
 * holds the sender's audio in a `PlayoutBuffer`, asks a `RatioControl` how fast to
 * consume it, and pulls each period for the device through a `Resampler` — the
 * three pieces of ticket 11 in the order that module was built in. The loop is
 * still paced by the device (`write` blocks until the ring has room), so the buffer
 * is what absorbs the difference between the sender's rate and ours.
 *
 * **The byte-exact loopback survives, with a caveat worth stating.** A resampler is
 * transparent only when the ratio is exactly 1, and in a loopback the two ends
 * share one clock, so the level sits exactly at its target, the control's
 * correction stays at zero and the ratio stays at 1 — measured bit-exact in
 * `tests/test_clock_resampler.cpp`. What is *not* preserved is the first couple of
 * periods: the converter fills its working room before it produces anything, so the
 * first periods the device gets are silence. The engine's loopback test allows for
 * exactly that and asserts bit-exactness after it.
 *
 * The delay figure ticket 12 asks for is `delay_ms()` — the buffer's level, which
 * is the only thing in the appliance that actually knows it. Its *trend* is a
 * difference between polls, so it belongs to whoever polls (the control surface),
 * not here.
 */
class Engine {
 public:
  Engine();
  ~Engine();

  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  /**
   * Build the device and the link from |config|. Opens nothing.
   *
   * The configuration has already been validated; this refuses only what a
   * validated configuration can still be wrong about — a block list that does
   * not cover the device's channels, which would silently drop audio rather
   * than fail.
   */
  bool prepare(const Config& config, std::string* error);

  /**
   * Open the device and the link.
   *
   * Separate from run() so a caller can drive the engine one turn at a time, and
   * so that a link that will not come up is reported before any thread exists.
   */
  bool open(std::string* error);

  /** Open, run both directions on their own threads, then close. */
  int run();

  /**
   * One turn of each direction, without starting a thread.
   *
   * These are the loops' bodies, and they are public for the same reason
   * `read()` and `write()` are: a loop that can only be exercised by starting a
   * thread and hoping is a loop nobody can prove. A test can drive one
   * iteration, look at what came out, and drive the next — which is what makes
   * the whole audio-to-wire path provable in CI, on a machine with no daemon and
   * no RAVENNA device, without a socket race.
   *
   * Each writes exactly one period to its direction, or refuses and says why.
   */
  bool step_transmit(std::string* error);
  bool step_receive(std::string* error);

  /** Ask both directions to finish. Safe to call from a signal handler thread. */
  void stop();

  /** True while both directions are running. */
  bool running() const;

  /**
   * One period of audio, cut into the frame's blocks.
   *
   * Pure: no device, no socket. `sample_position` is the caller's to advance,
   * and every block in a frame shares it — which is the cross-block sync
   * guarantee the wire format exists to make by construction (ADR 0001).
   */
  bool pack_period(const uint8_t* period, uint64_t sample_position,
                   wire::Frame* frame, std::string* error) const;

  /**
   * One frame, joined back into a period of interleaved samples.
   *
   * Refuses a frame this configuration does not expect — a block count that
   * cannot fill the device, or a link id that is not ours — rather than writing
   * a partly-filled period that would sound like the far end's fault.
   */
  bool unpack_frame(const wire::Frame& frame, uint8_t* period,
                    std::string* error) const;

  /** Frames transmitted, received, and refused as not-ours. */
  uint64_t frames_sent() const;
  uint64_t frames_received() const;
  uint64_t frames_refused() const;

  /**
   * The playout delay the clock is holding, in milliseconds — ticket 12's figure,
   * and the number an operator trends while a link sags.
   *
   * It is the buffer's level, not a total: the transport's own latency is on top of
   * it, and the A/V delay line's offset (ticket 13) is added to both.
   */
  double delay_ms() const;

  /** The delay against what the buffer can hold, 0..1, for a meter or an alarm. */
  double delay_fraction() const;

  /**
   * The clock's correction, in ppm: positive when the sender's clock is the faster.
   *
   * At convergence this is not a state but a *measurement* — the offset between the
   * two crystals — and it converges far more slowly than the level does, so it is a
   * diagnostic rather than a control reading. See the clock's own documentation.
   */
  double clock_offset_ppm() const;

  /** The ratio the resampler is applying: input frames consumed per output frame.
   */
  double clock_ratio() const;

  /** Periods the device was fed silence for because nothing could be played. */
  uint64_t silence_periods() const;

  /** The device, for the caller that has to feed or read it. Not owned here. */
  audio::AudioBackend* backend();
  const audio::AudioBackend* backend() const;

 private:
  void transmit_loop();
  void receive_loop();
  void close();

  /** Move whatever complete frames have arrived into the playout buffer. */
  void receive_into_buffer(std::string* error);

  /** One period for the device: pulled through the resampler, or silence. */
  bool play_one_period(std::string* error);

  Config config_;
  std::unique_ptr<audio::AudioBackend> backend_;
  std::unique_ptr<transport::Link> link_;
  wire::Reassembler reassembler_;
  audio::AudioFormat format_;

  /**
   * The clock, in the order it was built: a buffer that holds the sender's audio by
   * sample position, a control that steers the ratio from the buffer's level, and a
   * resampler that consumes the buffer at that ratio.
   *
   * The buffer is built in `prepare` because its geometry comes from the format;
   * the control and the resampler are members so their state survives a link that
   * is reopened. Nothing here is thread-safe, and nothing needs to be: the receive
   * loop is the only thread that touches them.
   */
  std::unique_ptr<clock::PlayoutBuffer> playout_;
  clock::RatioControl control_;
  clock::Resampler resampler_;

  /** The level the clock holds, in periods, and how long it may take to get there.
   */
  uint64_t target_periods_ = 0;
  uint64_t prime_deadline_periods_ = 0;
  /** False until playout starts: a receiver has nothing to play before its level.
   */
  bool playing_ = false;
  uint64_t priming_periods_ = 0;
  bool prime_reported_ = false;
  uint64_t silence_periods_ = 0;

  /** The link id every frame of this link carries; see the note in the .cpp. */
  uint16_t link_id_ = 0;
  uint64_t sequence_ = 0;
  uint64_t sample_position_ = 0;
  uint64_t frames_sent_ = 0;
  uint64_t frames_received_ = 0;
  uint64_t frames_refused_ = 0;

  /**
   * One buffer per direction, deliberately.
   *
   * The two directions run on two threads, so a single period buffer would be
   * the transmit thread reading a device into the same memory the receive thread
   * is writing to the device from — the kind of bug that shows up as a click
   * every few hours on a site and never on a laptop.
   */
  std::vector<uint8_t> tx_period_;
  std::vector<uint8_t> rx_period_;
  std::vector<uint8_t> message_;
  /** One encode target and one decode source: two threads, two buffers. */
  std::vector<uint8_t> tx_bytes_;
  std::vector<uint8_t> rx_bytes_;
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> running_{false};
  std::string failure_;
};

}  // namespace aes67_srt
