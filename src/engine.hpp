#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "audio/backend.hpp"
#include "clock/playout_buffer.hpp"
#include "clock/ratio_control.hpp"
#include "clock/resampler.hpp"
#include "codec/opus.hpp"
#include "config.hpp"
#include "delay/delay_line.hpp"
#include "delay/test_signal.hpp"
#include "parallel_for.hpp"
#include "transport/link.hpp"
#include "wire/frame.hpp"

namespace aes67_srt {

/**
 * What the engine looks like from another thread, as one plain value.
 *
 * The control surface polls the appliance from its own thread while the
 * device-paced loops run, so the figures it needs cannot be read off the live
 * objects — the playout buffer, the control and the resampler belong to the receive
 * thread, and a concurrent read of them is a data race, not a stale reading. The
 * loops publish the figures here as atomics each period instead, and
 * `Engine::status()` copies them out. It is a snapshot for a person, not a
 * synchronisation primitive: a value may move between two reads, and nothing in the
 * audio path ever waits on it.
 */
struct EngineStatus {
  bool running = false;
  bool link_open = false;
  uint64_t frames_sent = 0;
  uint64_t frames_received = 0;
  uint64_t frames_refused = 0;
  uint64_t silence_periods = 0;
  /** The clock's playout level, in milliseconds — the delay figure (ticket 12). */
  double delay_ms = 0.0;
  double delay_fraction = 0.0;
  /** The A/V offset the delay line is applying (decision 8). */
  double egress_delay_ms = 0.0;
  /** Transport latency + playout level + A/V offset + codec. */
  double av_delay_ms = 0.0;
  double clock_offset_ppm = 0.0;
  double clock_ratio = 1.0;
  int test_signal_channel = -1;
  /** False on a loopback, or before the link is up: there is nothing to report. */
  bool link_stats_available = false;
  transport::LinkStats link_stats;
};

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
                   wire::Frame* frame, std::string* error);

  /**
   * One frame, joined back into a period of interleaved samples.
   *
   * Refuses a frame this configuration does not expect — a block count that
   * cannot fill the device, or a link id that is not ours — rather than writing
   * a partly-filled period that would sound like the far end's fault.
   */
  bool unpack_frame(const wire::Frame& frame, uint8_t* period, std::string* error);

  /** Frames transmitted, received, and refused as not-ours. */
  uint64_t frames_sent() const;
  uint64_t frames_received() const;
  uint64_t frames_refused() const;

  /**
   * The playout delay the clock is holding, in milliseconds — ticket 12's figure,
   * and the number an operator trends while a link sags.
   *
   * It is the buffer's level, not a total: the transport's own latency is on top of
   * it, and the A/V delay line's offset is added to both (`egress_delay_ms()`, and
   * `av_delay_ms()` for the whole sum).
   */
  double delay_ms() const;

  /** The delay against what the buffer can hold, 0..1, for a meter or an alarm. */
  double delay_fraction() const;

  /**
   * The A/V offset the egress delay line is applying, in milliseconds — decision
   * 8's dial, and the audio side of lipsync (ticket 12).
   *
   * It is the value asked for; the line reaches it over the crossfade window,
   * which is 10 ms, so it is the number an operator sees and trusts.
   */
  double egress_delay_ms() const;

  /**
   * Ask the delay line for a new A/V offset while audio runs.
   *
   * **Safe to call from any thread, and that is why it posts rather than acts.**
   * The delay line belongs to the receive loop; a control surface mutating it
   * directly would be a data race with the loop reading it. This validates against
   * the same 0..5000 ms ceiling the configuration does, records the request, and
   * returns; the receive loop applies it on its next period. The requested value is
   * published immediately, so the operator sees the number they dialled.
   */
  bool set_egress_delay_ms(double offset_ms, std::string* error);

  /**
   * Fire the impulse test signal on the channel `egress.test_signal_channel`
   * selects, on the next period.
   *
   * Safe from any thread for the same reason as the offset: the request is
   * recorded and the receive loop places the impulse at the exact egress frame it
   * has reached when it gets there.
   */
  bool trigger_test_signal(std::string* error);

  /**
   * The codec's contribution to the A/V budget, in milliseconds: **zero in v1**,
   * because v1 encodes nothing (decision 9).
   *
   * It exists as a figure rather than a footnote because phase 2 changes it and
   * the operator's alignment number has to change with it: an Opus encoder adds
   * its frame plus `OPUS_GET_LOOKAHEAD`, measured at **312 samples = 6.50 ms** on
   * the appliance (`docs/research/opus.md`), which at 20 ms frames is 26.5 ms off
   * the top of the delay an operator has dialled. Subtracting it here is what
   * keeps a codec link alignable without re-learning the number.
   */
  double codec_delay_ms() const;

  /**
   * The whole audio delay an operator is aligning against vision, in
   * milliseconds: the transport's latency, plus the clock's playout delay, plus
   * the A/V offset, plus the codec's share.
   *
   * It is a sum of what actually knows its own number rather than a promise:
   * `link.latency_ms` is the buffering budget, `delay_ms()` is the level the
   * clock is holding, and the other two are configuration and the codec.
   */
  double av_delay_ms() const;

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

  /**
   * Everything the control surface shows, as one thread-safe snapshot.
   *
   * Safe to call from any thread at any time, including while both loops run; it
   * reads atomics the loops publish and copies the link statistics under a lock.
   */
  EngineStatus status();

  /** The device, for the caller that has to feed or read it. Not owned here. */
  audio::AudioBackend* backend();
  const audio::AudioBackend* backend() const;

 private:
  void transmit_loop();
  void receive_loop();
  /** Once a second, put the link's own statistics in the log (ticket 09). */
  void status_loop();
  void close();

  /** Move whatever complete frames have arrived into the playout buffer. */
  void receive_into_buffer(std::string* error);

  /** One period for the device: pulled through the resampler, or silence. */
  bool play_one_period(std::string* error);

  /**
   * The last step of the egress path: mix the test signal, delay the period by
   * the A/V offset, and hand it to the device.
   *
   * Every period the device hears goes through here, priming and silence
   * included, because the egress frame counter has to advance with what is
   * actually played for a test signal to land where it was asked to.
   */
  bool write_period_to_device(std::string* error);

  /**
   * The codec for one block, opened on first use, or null when this block does
   * not use Opus. One per block, because a block is the unit the codec works on
   * and eight block encoders can run on the appliance's cores.
   */
  codec::OpusBlock* codec_for_block(size_t block_index, bool opus,
                                    std::string* error);

  /**
   * Keep the link up: re-establish it whenever it goes down, without a restart.
   *
   * Runs on its own thread. When the link is down it calls reopen_link(), which
   * blocks in accept or connect; when that fails it waits, backing off, and tries
   * again. This is what makes a site-to-studio link unattended: a caller that
   * starts before its peer, a listener whose caller left, and a connection that
   * dropped all come back here rather than needing a human.
   */
  void link_supervisor_loop();

  /** Re-establish the link and reapply its options. The supervisor's one job. */
  bool reopen_link(std::string* error);

  Config config_;
  std::unique_ptr<audio::AudioBackend> backend_;
  std::unique_ptr<transport::Link> link_;
  wire::Reassembler reassembler_;
  audio::AudioFormat format_;
  /** One Opus codec per block that uses it, indexed by block index. */
  std::vector<std::unique_ptr<codec::OpusBlock>> codecs_;
  /** Per-block interleaved PCM on the way in to the transmit codecs. One buffer
   *  per block, because the encodes run in parallel. */
  std::vector<std::vector<uint8_t>> tx_block_pcm_;
  /** Scratch for a block's decoded PCM on the receive side (sequential). */
  std::vector<uint8_t> codec_pcm_;
  /** The block encoders' threads: eight Opus encodes on one thead is 43% of a
   *  Pi core (docs/research/opus.md), so they are spread across the cores. */
  ParallelFor encoders_;

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

  /**
   * The egress stage (ticket 12): the A/V delay line and the test signal, in the
   * order `clock -> delay -> egress` puts them.
   *
   * Both are the receive loop's to touch, like the clock above, and both are
   * built per open so a reopened link starts from a clean line rather than one
   * still holding the previous stream's audio.
   */
  delay::DelayLine delay_line_;
  delay::TestSignal test_signal_;
  /** Frames the device has been fed since open; the test signal's time base. */
  uint64_t egress_frames_ = 0;

  /** The level the clock holds, in periods, and how long it may take to get there.
   */
  uint64_t target_periods_ = 0;
  uint64_t prime_deadline_periods_ = 0;
  /** False until playout starts: a receiver has nothing to play before its level.
   */
  bool playing_ = false;
  uint64_t priming_periods_ = 0;
  bool prime_reported_ = false;
  std::atomic<uint64_t> silence_periods_{0};

  /** The link id every frame of this link carries; see the note in the .cpp. */
  uint16_t link_id_ = 0;
  uint64_t sequence_ = 0;
  uint64_t sample_position_ = 0;
  /** Counters, published for the control surface: the loops are the only writers.
   */
  std::atomic<uint64_t> frames_sent_{0};
  std::atomic<uint64_t> frames_received_{0};
  std::atomic<uint64_t> frames_refused_{0};

  /**
   * The figures the receive loop publishes for `status()`, one store each per
   * period. They are atomics rather than a locked struct because the loop must
   * never wait on a reader: a mutex here would let the control surface stall the
   * audio path.
   */
  std::atomic<double> published_delay_ms_{0.0};
  std::atomic<double> published_delay_fraction_{0.0};
  std::atomic<double> published_egress_delay_ms_{0.0};
  std::atomic<double> published_clock_offset_ppm_{0.0};
  std::atomic<double> published_clock_ratio_{1.0};

  /**
   * Requests posted by the control surface's thread, consumed by the receive loop.
   *
   * The pending flag is cleared with `exchange`, so a request is applied exactly
   * once; a second request arriving before the loop gets there overwrites the
   * value, which is the right answer for a dial.
   */
  std::atomic<double> requested_egress_delay_ms_{0.0};
  std::atomic<bool> egress_delay_pending_{false};
  std::atomic<bool> test_signal_pending_{false};

  /**
   * The link statistics, which only the status thread fills. They are a block
   * rather than a scalar, so this is the one field behind a lock; the lock is never
   * held by the audio path.
   */
  std::mutex stats_mutex_;
  bool link_stats_available_ = false;
  transport::LinkStats link_stats_;

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
  /** One outgoing message (fragment header + payload), reused every fragment. */
  std::vector<uint8_t> tx_message_;
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> running_{false};
  std::string failure_;
};

}  // namespace aes67_srt
