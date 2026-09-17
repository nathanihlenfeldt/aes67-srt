#include "transport/link.hpp"

#include <chrono>
#include <cstring>
#include <deque>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <vector>

#include "log.hpp"
#include "util.hpp"
#include "wire/frame.hpp"

#if AES67_SRT_WITH_SRT
#include <arpa/inet.h>
#include <netinet/in.h>
#include <srt/srt.h>
#endif

namespace aes67_srt::transport {
namespace {

#if AES67_SRT_WITH_SRT

/** SRT has process-wide state: start it once and let the process own it. */
void ensure_started() {
  static const bool started = [] {
    srt_startup();
    return true;
  }();
  (void)started;
}

std::string last_error() {
  return srt_getlasterror_str();
}

bool set_int(SRTSOCKET socket, SRT_SOCKOPT option, int value) {
  return srt_setsockopt(socket, 0, option, &value, sizeof(value)) != SRT_ERROR;
}

bool set_bool(SRTSOCKET socket, SRT_SOCKOPT option, bool value) {
  return srt_setsockopt(socket, 0, option, &value, sizeof(value)) != SRT_ERROR;
}

/** One MTU, which is the most a live-mode receive can hand up. */
constexpr size_t k_receive_buffer_bytes = 1500;

#endif

bool fail(std::string* error, const std::string& message) {
  if (error != nullptr) {
    *error = message;
  }
  return false;
}

/**
 * How many messages the loopback will hold before it refuses to take more.
 *
 * A frame is eight messages, so this is a quarter-second of audio at 1 ms
 * frames — far more than any correctly wired loopback ever holds, and small
 * enough that a loopback nothing is reading from fails loudly instead of
 * quietly consuming the machine.
 */
constexpr size_t k_loopback_max_messages = 2048;

}  // namespace

struct Link::Impl {
#if AES67_SRT_WITH_SRT
  SRTSOCKET socket = SRT_INVALID_SOCK;
#endif
  bool opened = false;
  int receive_timeout_ms = 500;
  std::chrono::steady_clock::time_point opened_at;
  std::string peer;
  std::string mode;

  /**
   * Loopback mode's pipe: what `send_message` has written and `receive_message`
   * has not read yet.
   *
   * A mutex is enough here where a real transport would need a lock-free queue.
   * The two ends are this process's own threads, nothing realtime is on either
   * side of it, and a message copy is microseconds — so the simplest correct
   * thing is the right thing, and the comment says so rather than leaving the
   * next reader to wonder whether it was an oversight.
   *
   * Bounded, and it *refuses* when full instead of growing: a loopback nobody is
   * receiving from is a bug in whoever wired it up, and eating the machine's
   * memory to hide it would be the wrong failure.
   */
  std::mutex loopback_mutex;
  std::deque<std::vector<uint8_t>> loopback_queue;
};

Link::Link() : impl_(new Impl()) {}

Link::~Link() {
  close();
  delete impl_;
}

bool Link::available() {
#if AES67_SRT_WITH_SRT
  return true;
#else
  return false;
#endif
}

std::string Link::unavailable_reason() {
#if AES67_SRT_WITH_SRT
  return std::string();
#else
  return "this build has no libsrt: install it and configure with "
         "-DWITH_SRT=ON (see docs/research/libsrt.md)";
#endif
}

void Link::close() {
  if (impl_ == nullptr) {
    return;
  }
#if AES67_SRT_WITH_SRT
  if (impl_->socket != SRT_INVALID_SOCK) {
    // Give the peer the courtesy of knowing, and bound how long that may take.
    srt_close(impl_->socket);
    impl_->socket = SRT_INVALID_SOCK;
  }
#endif
  impl_->opened = false;
  impl_->peer.clear();
  impl_->mode.clear();
  {
    // Drop whatever the loopback was holding: a closed link that hands back the
    // previous session's messages on reopening would be a baffling bug to chase.
    std::lock_guard<std::mutex> lock(impl_->loopback_mutex);
    impl_->loopback_queue.clear();
  }
}

bool Link::is_open() const {
  return impl_ != nullptr && impl_->opened;
}

double Link::uptime_seconds() const {
  if (!is_open()) {
    return 0.0;
  }
  const auto elapsed = std::chrono::steady_clock::now() - impl_->opened_at;
  return std::chrono::duration<double>(elapsed).count();
}

void Link::set_receive_timeout_ms(int timeout_ms) {
  impl_->receive_timeout_ms = timeout_ms;
#if AES67_SRT_WITH_SRT
  if (impl_->socket != SRT_INVALID_SOCK) {
    set_int(impl_->socket, SRTO_RCVTIMEO, timeout_ms);
  }
#endif
}

std::string Link::peer_description() const {
  if (!is_open()) {
    return "(down)";
  }
  return impl_->mode + " " + impl_->peer;
}

bool Link::open(const Config& config, std::string* error) {
  if (impl_->opened) {
    return fail(error, "the link is already open");
  }

  const std::string mode = to_lower(config.link.mode);
  if (mode != "caller" && mode != "listener" && mode != "rendezvous" &&
      mode != "loopback") {
    return fail(error,
                "link.mode: expected caller, listener, rendezvous or loopback; "
                "got \"" +
                    config.link.mode + "\"");
  }

  // Loopback is answered before the build-without-libsrt refusal below: it is
  // our own byte pipe in our own process, so a build with no libsrt can still
  // run the whole audio path in fake mode. Making that a property of this class
  // rather than of the library is what lets a laptop with nothing installed
  // exercise the appliance end to end.
  if (mode == "loopback") {
    {
      std::lock_guard<std::mutex> lock(impl_->loopback_mutex);
      // A link that was closed and reopened must not hand back the old
      // session's messages.
      impl_->loopback_queue.clear();
    }
    impl_->mode = mode;
    impl_->peer = "(in-process)";
    impl_->opened = true;
    impl_->opened_at = std::chrono::steady_clock::now();
    log().write(LogLevel::warn,
                "the transport is a loopback: nothing leaves this machine");
    return true;
  }

#if !AES67_SRT_WITH_SRT
  return fail(error, unavailable_reason());
#else
  ensure_started();

  SRTSOCKET socket = srt_create_socket();
  if (socket == SRT_INVALID_SOCK) {
    return fail(error, "srt_socket: " + last_error());
  }
  // Every refusal below closes the socket, so a link that fails to come up
  // leaves nothing half-open behind it.
  const auto refuse = [&socket, error](const std::string& option,
                                       const std::string& detail) {
    const std::string reason = last_error();
    srt_close(socket);
    return fail(error, option + ": " + (detail.empty() ? reason : detail));
  };

  // Live mode and the message API. Live is what gives us boundaries and a
  // latency-controlled playout; the message API is the only one it offers.
  if (!set_int(socket, SRTO_TRANSTYPE, SRTT_LIVE)) {
    return refuse("SRTO_TRANSTYPE", "");
  }
  if (!set_bool(socket, SRTO_MESSAGEAPI, true)) {
    return refuse("SRTO_MESSAGEAPI", "");
  }
  if (!set_int(socket, SRTO_RCVLATENCY, config.link.latency_ms)) {
    return refuse("SRTO_RCVLATENCY", "");
  }
  if (!set_int(socket, SRTO_PEERLATENCY, config.link.latency_ms)) {
    return refuse("SRTO_PEERLATENCY", "");
  }
  // Set explicitly rather than trusting the default: the library enforces
  // SRTO_PAYLOADSIZE on every send, and our fragment size has to agree with it.
  if (!set_int(socket, SRTO_PAYLOADSIZE,
               static_cast<int>(wire::k_max_message_bytes))) {
    return refuse("SRTO_PAYLOADSIZE", "");
  }
  // SRT's default for this is TRUE in live mode, and it discards exactly the
  // audio this design exists to preserve. Failing to turn it off is fatal rather
  // than a warning: a link that silently drops audio is worse than no link.
  if (!set_bool(socket, SRTO_TLPKTDROP, false)) {
    return refuse("SRTO_TLPKTDROP", "could not disable too-late packet drop");
  }
  if (mode == "rendezvous" && !set_bool(socket, SRTO_RENDEZVOUS, true)) {
    return refuse("SRTO_RENDEZVOUS", "");
  }
  // Buffer tuning, when the configuration asks for it. Zero means "leave the
  // library's default", which is the right answer until these have been tuned
  // against a real link (docs/research/libsrt.md).
  if (config.link.receive_buffer_bytes > 0 &&
      !set_int(socket, SRTO_RCVBUF, config.link.receive_buffer_bytes)) {
    return refuse("SRTO_RCVBUF", "");
  }
  if (config.link.flow_control_packets > 0 &&
      !set_int(socket, SRTO_FC, config.link.flow_control_packets)) {
    return refuse("SRTO_FC", "");
  }
  if (!config.link.passphrase.empty()) {
    const int length = static_cast<int>(config.link.passphrase.size());
    if (srt_setsockopt(socket, 0, SRTO_PASSPHRASE, config.link.passphrase.data(),
                       length) == SRT_ERROR) {
      return refuse("SRTO_PASSPHRASE", "");
    }
  }
  if (!set_bool(socket, SRTO_RCVSYN, true)) {
    return refuse("SRTO_RCVSYN", "");
  }
  if (!set_bool(socket, SRTO_SNDSYN, true)) {
    return refuse("SRTO_SNDSYN", "");
  }
  if (!set_int(socket, SRTO_RCVTIMEO, impl_->receive_timeout_ms)) {
    return refuse("SRTO_RCVTIMEO", "");
  }

  sockaddr_in local{};
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = htonl(INADDR_ANY);
  local.sin_port = htons(static_cast<uint16_t>(config.link.local_port));
  if (srt_bind(socket, reinterpret_cast<sockaddr*>(&local), sizeof(local)) ==
      SRT_ERROR) {
    return refuse("srt_bind port " + std::to_string(config.link.local_port), "");
  }

  sockaddr_in peer{};
  if (mode != "listener") {
    std::string host;
    int port = 0;
    if (!split_host_port(config.link.peer, &host, &port)) {
      return refuse("link.peer",
                    "expected host:port, got \"" + config.link.peer + "\"");
    }
    peer.sin_family = AF_INET;
    peer.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, host.c_str(), &peer.sin_addr) != 1) {
      // Deliberate for v1: a name would need resolution that can fail at a site
      // we are not standing in, and the address is configured once anyway.
      return refuse("link.peer", "names are not resolved yet: \"" + host +
                                     "\" must be an IPv4 address");
    }
  }

  if (mode == "listener") {
    if (srt_listen(socket, 1) == SRT_ERROR) {
      return refuse("srt_listen", "");
    }
    const SRTSOCKET accepted = srt_accept(socket, nullptr, nullptr);
    if (accepted == SRT_INVALID_SOCK) {
      return refuse("srt_accept", "");
    }
    srt_close(socket);  // the listener has done its job
    socket = accepted;
  } else if (srt_connect(socket, reinterpret_cast<sockaddr*>(&peer),
                         sizeof(peer)) == SRT_ERROR) {
    return refuse("srt_connect", "");
  }

  impl_->socket = socket;
  impl_->opened = true;
  impl_->opened_at = std::chrono::steady_clock::now();
  impl_->mode = mode;
  impl_->peer = (mode == "listener")
                    ? ("accepted on port " + std::to_string(config.link.local_port))
                    : config.link.peer;
  return true;
#endif
}

bool Link::send_message(const uint8_t* data, size_t size, std::string* error) {
  if (data == nullptr || size == 0) {
    return fail(error, "nothing to send");
  }
  if (size > wire::k_max_message_bytes) {
    // Refused here rather than at the socket, so the message says what is wrong
    // instead of leaving the caller to decode SRT_EINVALMSGAPI.
    return fail(error, "message of " + std::to_string(size) +
                           " bytes exceeds the " +
                           std::to_string(wire::k_max_message_bytes) +
                           "-byte ceiling SRT live mode allows (fragment the "
                           "frame first)");
  }
  if (impl_->mode == "loopback") {
    std::lock_guard<std::mutex> lock(impl_->loopback_mutex);
    if (impl_->loopback_queue.size() >= k_loopback_max_messages) {
      // Refused rather than queued: a loopback nobody is receiving from is a
      // wiring mistake, and the honest report is a message the caller can act
      // on rather than memory that grows until the appliance dies.
      return fail(error, "the loopback is holding " +
                             std::to_string(impl_->loopback_queue.size()) +
                             " messages already: nothing is receiving on it");
    }
    impl_->loopback_queue.emplace_back(data, data + size);
    return true;
  }
#if !AES67_SRT_WITH_SRT
  return fail(error, unavailable_reason());
#else
  if (!impl_->opened) {
    return fail(error, "the link is not open");
  }
  const int sent = srt_sendmsg(impl_->socket, reinterpret_cast<const char*>(data),
                               static_cast<int>(size), -1, false);
  if (sent == SRT_ERROR) {
    return fail(error, std::string("srt_sendmsg: ") + last_error());
  }
  if (sent != static_cast<int>(size)) {
    // Documented as impossible in live and file/message mode, which is exactly
    // why it is worth checking rather than assuming.
    return fail(error, "srt_sendmsg sent " + std::to_string(sent) + " of " +
                           std::to_string(size) + " bytes");
  }
  return true;
#endif
}

bool Link::receive_message(std::vector<uint8_t>* buffer, bool* timed_out,
                           std::string* error) {
  if (buffer == nullptr) {
    return fail(error, "no buffer to fill");
  }
  if (timed_out != nullptr) {
    *timed_out = false;
  }
  if (impl_->mode == "loopback") {
    std::lock_guard<std::mutex> lock(impl_->loopback_mutex);
    if (impl_->loopback_queue.empty()) {
      // Nothing waiting is what `timed_out` is for, and the caller's loop has to
      // be able to tell a quiet loopback from a broken one. There is no waiting
      // here: an in-process pipe either has a message or it does not, and
      // sleeping for the configured timeout would be pretending to be a network.
      if (timed_out != nullptr) {
        *timed_out = true;
      }
      return false;
    }
    *buffer = std::move(impl_->loopback_queue.front());
    impl_->loopback_queue.pop_front();
    return true;
  }
#if !AES67_SRT_WITH_SRT
  return fail(error, unavailable_reason());
#else
  if (!impl_->opened) {
    return fail(error, "the link is not open");
  }
  char chunk[k_receive_buffer_bytes];
  const int received = srt_recvmsg(impl_->socket, chunk, sizeof(chunk));
  if (received == SRT_ERROR) {
    if (srt_getlasterror(nullptr) == SRT_ETIMEOUT) {
      // A quiet link is not a broken one, and the caller's loop has to be able to
      // tell the difference.
      if (timed_out != nullptr) {
        *timed_out = true;
      }
      return false;
    }
    return fail(error, std::string("srt_recvmsg: ") + last_error());
  }
  buffer->assign(reinterpret_cast<uint8_t*>(chunk),
                 reinterpret_cast<uint8_t*>(chunk) + received);
  return true;
#endif
}

bool Link::stats(LinkStats* out, std::string* error) const {
  if (out == nullptr) {
    return fail(error, "no statistics to fill");
  }
  if (impl_->mode == "loopback") {
    // Refused rather than filled with zeroes. The rule this class states is that
    // it fails rather than returning plausible numbers, and a loopback has no
    // RTT, no bandwidth and no loss to report — a status page showing 0.0 ms RTT
    // for a link that does not exist is worse than one showing nothing.
    return fail(error,
                "there are no link statistics: the transport is a loopback, not "
                "a network");
  }
#if !AES67_SRT_WITH_SRT
  return fail(error, unavailable_reason());
#else
  if (!impl_->opened) {
    return fail(error, "the link is not open");
  }
  SRT_TRACEBSTATS performance{};
  if (srt_bstats(impl_->socket, &performance, 0) == SRT_ERROR) {
    return fail(error, std::string("srt_bstats: ") + last_error());
  }
  out->rtt_ms = performance.msRTT;
  out->bandwidth_mbps = performance.mbpsBandwidth;
  out->receive_rate_mbps = performance.mbpsRecvRate;
  out->receive_buffer_ms = performance.msRcvBuf;
  // The negotiated value, not the configured one: the delay belongs to the pair.
  out->negotiated_latency_ms = performance.msRcvTsbPdDelay;
  out->send_buffer_ms = performance.msSndBuf;
  out->packets_received = performance.pktRecvTotal;
  out->packets_lost = performance.pktRcvLossTotal;
  // NOT pktRcvRetransTotal: the accumulated receiver-side retransmit counter
  // breaks the pktRcv* pattern the interval figure follows (srt.h:313 against
  // :338). Guessing cost a compile error; the header is the authority.
  out->packets_retransmitted = performance.pktRetransTotal;
  // Counts what TLPKTDROP threw away. With it disabled this must stay zero, and
  // a test asserts exactly that.
  out->packets_dropped = performance.pktRcvDropTotal;
  return true;
#endif
}

std::string to_string(const LinkStats& stats) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(1) << "rtt " << stats.rtt_ms
      << " ms, bandwidth " << stats.bandwidth_mbps << " Mbps, receive "
      << stats.receive_rate_mbps << " Mbps, receive buffer "
      << stats.receive_buffer_ms << " ms, latency " << stats.negotiated_latency_ms
      << " ms, send buffer " << stats.send_buffer_ms << " ms, packets received "
      << stats.packets_received << ", lost " << stats.packets_lost
      << ", retransmitted " << stats.packets_retransmitted << ", dropped "
      << stats.packets_dropped;
  return out.str();
}

}  // namespace aes67_srt::transport
