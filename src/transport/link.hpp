#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "config.hpp"

namespace aes67_srt::transport {

/**
 * What the link can tell the UI or the log.
 *
 * Field names follow SRT's own statistics so that a value on screen can be
 * traced to the header that produces it. See docs/research/libsrt.md.
 */
struct LinkStats {
  double rtt_ms = 0.0;                // msRTT
  double bandwidth_mbps = 0.0;        // mbpsBandwidth
  double receive_rate_mbps = 0.0;     // mbpsRecvRate
  int receive_buffer_ms = 0;          // msRcvBuf
  int negotiated_latency_ms = 0;      // msRcvTsbPdDelay: the pair's actual delay
  int send_buffer_ms = 0;             // msSndBuf
  int64_t packets_received = 0;       // pktRecvTotal
  int64_t packets_lost = 0;           // pktRcvLossTotal
  int64_t packets_retransmitted = 0;  // pktRcvRetransTotal
  int64_t packets_dropped = 0;        // pktRcvDropTotal: must stay 0
};

/**
 * One SRT connection, carrying messages in both directions.
 *
 * Deliberately a *message pipe*: it knows nothing about frames. Slicing a frame
 * into messages and reassembling them belongs to `wire`, which keeps this class
 * about the socket and lets both halves be tested separately — the format half
 * without a socket, this half without the format.
 *
 * It also has a **loopback mode**, in which there is no socket at all: a message
 * sent is a message waiting to be received by the same object, in this process.
 * That is what `-f` means by "loopback transport" — the appliance can run its
 * whole audio path on a laptop with no network, no second appliance and no
 * libsrt. It is a development and commissioning facility, not a transport, and
 * the class says so wherever it can be observed (see `stats` and
 * `peer_description`).
 */
class Link {
 public:
  Link();
  ~Link();

  Link(const Link&) = delete;
  Link& operator=(const Link&) = delete;

  /**
   * True when this build contains libsrt at all.
   *
   * Loopback mode needs no libsrt: it is our own byte pipe, so a build without
   * the library can still run the whole audio path in fake mode, which is how it
   * stays honest about having no network.
   */
  static bool available();

  /** Why not, when available() is false. Empty when it is true. */
  static std::string unavailable_reason();

  /**
   * Bring the link up. Blocking for the three network modes: a listener waits
   * for a caller, a caller connects, rendezvous does both sides. `loopback`
   * returns immediately, because there is no peer to wait for.
   */
  bool open(const Config& config, std::string* error);

  void close();
  bool is_open() const;

  /** How long the link has been up, in seconds. 0 when it is down. */
  double uptime_seconds() const;

  /**
   * Send one message.
   *
   * Rejects anything larger than wire::k_max_message_bytes before it reaches the
   * socket, so a caller that forgot to slice a frame gets a refusal naming the
   * size rather than an SRT_EINVALMSGAPI that means the same thing less clearly.
   */
  bool send_message(const uint8_t* data, size_t size, std::string* error);

  /**
   * Receive one message into |buffer|, which is resized to fit.
   *
   * Returns false on error or on timeout, with |timed_out| distinguishing them:
   * a quiet link is not a broken one, and the caller's loop must be able to tell.
   */
  bool receive_message(std::vector<uint8_t>* buffer, bool* timed_out,
                       std::string* error);

  void set_receive_timeout_ms(int timeout_ms);

  /**
   * Bound how long a send waits when the peer has stopped reading. **0 is
   * non-blocking**, by the same documentation the receive timeout rests on
   * (`SRTO_SNDTIMEO` limits how long the send blocks; only -1 is no limit).
   *
   * Without this the send blocks for ever once the peer's flow-control window
   * fills — the transmit loop is device-paced, so a network call that never
   * returns is a loop that never sees the stop flag, and the process will not
   * exit on SIGTERM. It is the same trap the receive timeout had.
   */
  void set_send_timeout_ms(int timeout_ms);

  /** Statistics for the UI. Fails rather than returning plausible zeroes. */
  bool stats(LinkStats* out, std::string* error) const;

  /** The resolved peer, once the link is up, for logging. */
  std::string peer_description() const;

 private:
  struct Impl;
  Impl* impl_;
};

/**
 * The statistics as one line, for the log and the UI.
 *
 * The class exposes these and, until this existed, nothing printed them — so a
 * run that delivered a fraction of what it offered could not say why from inside
 * the run. One line a second is the diagnostic (ticket 09's two-ended run).
 */
std::string to_string(const LinkStats& stats);

}  // namespace aes67_srt::transport
