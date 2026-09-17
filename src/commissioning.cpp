#include "commissioning.hpp"

#include <string>

#include "log.hpp"

namespace aes67_srt {
namespace {

bool fail(std::string* error, const std::string& message) {
  if (error != nullptr) {
    *error = message;
  }
  return false;
}

/**
 * The reason the daemon gave, or a placeholder.
 *
 * Read *before* `fail()` writes over it: `fail(error, "…" + *error)` is the
 * shape that looks right and reads a string the same call is about to replace.
 */
std::string reason_of(const std::string* error) {
  return (error != nullptr && !error->empty()) ? *error
                                               : std::string("no reason given");
}

}  // namespace

bool commission(daemon::DaemonClient* daemon, const Config& config, bool loopback,
                CommissioningResult* result, std::string* error) {
  if (daemon == nullptr) {
    return fail(error, "commissioning: no daemon client");
  }

  // Counted locally so the caller may pass nothing, and copied out at the end
  // rather than checked for null on every line.
  CommissioningResult counts;

  // Where the daemon is, and whether it is even answering, before anything is
  // written to it. A refusal here is the difference between "the appliance is
  // misconfigured" and "there is nothing to talk to".
  daemon::PtpStatus ptp;
  if (!daemon->get_ptp_status(&ptp, error)) {
    return fail(error, "commissioning: the daemon at " + daemon->endpoint() +
                           " did not answer: " + reason_of(error));
  }
  counts.ptp_status = ptp.status;
  counts.gmid = ptp.gmid;
  if (ptp.status != "locked") {
    // Not a refusal. Publishing streams on an unlocked slave is harmless and the
    // lock often arrives a few seconds after the daemon starts, but it is the
    // single commonest reason an appliance that looks healthy produces silence,
    // so it is said out loud rather than left in a status field.
    log().write(LogLevel::warn,
                "commissioning: PTP is \"" + ptp.status +
                    "\", not locked - no AES67 audio will flow until it locks "
                    "(grandmaster " +
                    (ptp.gmid.empty() ? std::string("unknown") : ptp.gmid) + ")");
  }

  for (size_t index = 0; index < config.blocks.size(); ++index) {
    const BlockConfig& block = config.blocks[index];
    const int stream_id = static_cast<int>(index);

    // The stream this appliance publishes: the block's device channels, on the
    // wire, as an AES67 source.
    if (!daemon->put_source(stream_id, daemon::make_block_source(config, block),
                            error)) {
      return fail(error, "commissioning: cannot publish source " +
                             std::to_string(stream_id) + " (block " +
                             std::to_string(block.index) +
                             "): " + reason_of(error));
    }
    ++counts.sources_published;

    if (!loopback) {
      continue;
    }

    // The commissioning loopback: subscribe to what we just published. The SDP
    // comes back from the daemon rather than being invented here, so this
    // exercises the same path a real site uses to wire a sink.
    std::string sdp;
    if (!daemon->get_source_sdp(stream_id, &sdp, error)) {
      return fail(error, "commissioning: cannot read the SDP of source " +
                             std::to_string(stream_id) + ": " + reason_of(error));
    }
    if (sdp.empty()) {
      return fail(error, "commissioning: source " + std::to_string(stream_id) +
                             " reports no SDP, so there is nothing to subscribe "
                             "a sink to");
    }

    daemon::json sink;
    if (!daemon::make_block_sink(block, sdp, &sink, error)) {
      return fail(error, "commissioning: " + reason_of(error));
    }
    if (!daemon->put_sink(stream_id, sink, error)) {
      return fail(error, "commissioning: cannot subscribe sink " +
                             std::to_string(stream_id) + " (block " +
                             std::to_string(block.index) +
                             "): " + reason_of(error));
    }
    ++counts.sinks_subscribed;

    daemon::SinkStatus status;
    if (!daemon->get_sink_status(stream_id, &status, error)) {
      return fail(error, "commissioning: cannot read sink " +
                             std::to_string(stream_id) +
                             " status: " + reason_of(error));
    }
    if (status.in_use && status.receiving_rtp_packet) {
      ++counts.sinks_receiving;
      continue;
    }
    // The one thing commissioning is here to find out, and per block rather than
    // as a count at the end: which block is silent is the whole question.
    log().write(
        LogLevel::warn,
        "commissioning: block " + std::to_string(block.index) + " sink " +
            std::to_string(stream_id) +
            (status.in_use ? " has a stream but is not receiving RTP packets yet"
                           : " has no stream at all"));
  }

  log().write(LogLevel::info,
              "commissioning: " + std::to_string(counts.sources_published) +
                  " sources published, " + std::to_string(counts.sinks_subscribed) +
                  " sinks subscribed, " + std::to_string(counts.sinks_receiving) +
                  " receiving, PTP " + counts.ptp_status);
  if (result != nullptr) {
    *result = counts;
  }
  return true;
}

}  // namespace aes67_srt
