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
 */
class Link {
 public:
  Link();
  ~Link();

  Link(const Link&) = delete;
  Link& operator=(const Link&) = delete;

  /** True when this build contains libsrt at all. */
  static bool available();

  /** Why not, when available() is false. Empty when it is true. */
  static std::string unavailable_reason();

  /**
   * Bring the link up. Blocking: a listener waits for a caller, a caller
   * connects, rendezvous does both sides.
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

  /** Statistics for the UI. Fails rather than returning plausible zeroes. */
  bool stats(LinkStats* out, std::string* error) const;

  /** The resolved peer, once the link is up, for logging. */
  std::string peer_description() const;

 private:
  struct Impl;
  Impl* impl_;
};

}  // namespace aes67_srt::transport
