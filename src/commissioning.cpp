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

/**
 * Subscribe one block's sink to `sdp`, and report whether it is receiving.
 *
 * Shared by both subscription paths, because "wire a sink to an SDP and read the
 * daemon's answer" is the same operation whether the SDP came from our own source
 * or from a discovery announcement.
 */
bool subscribe_sink(daemon::DaemonClient* daemon, const BlockConfig& block,
                    int stream_id, const std::string& sdp,
                    CommissioningResult* counts, std::string* error) {
  daemon::json sink;
  if (!daemon::make_block_sink(block, sdp, &sink, error)) {
    return fail(error, "commissioning: " + reason_of(error));
  }
  if (!daemon->put_sink(stream_id, sink, error)) {
    return fail(error, "commissioning: cannot subscribe sink " +
                           std::to_string(stream_id) + " (block " +
                           std::to_string(block.index) + "): " + reason_of(error));
  }
  ++counts->sinks_subscribed;

  daemon::SinkStatus status;
  if (!daemon->get_sink_status(stream_id, &status, error)) {
    return fail(error, "commissioning: cannot read sink " +
                           std::to_string(stream_id) +
                           " status: " + reason_of(error));
  }
  if (status.in_use && status.receiving_rtp_packet) {
    ++counts->sinks_receiving;
    return true;
  }
  // The one thing commissioning is here to find out, per stream rather than as a
  // count at the end: which one is silent is the whole question.
  log().write(
      LogLevel::warn,
      "commissioning: block " + std::to_string(block.index) + " sink " +
          std::to_string(stream_id) +
          (status.in_use ? " has a stream but is not receiving RTP packets yet"
                         : " has no stream at all"));
  return true;
}

/**
 * The SDP of the discovery announcement an operator named.
 *
 * The refusals list what *was* discovered, because a name that does not match is
 * the likeliest mistake here and "no such source" without the alternatives sends
 * whoever typed it back to a shell to run `curl` by hand.
 */
bool announcement_sdp(daemon::DaemonClient* daemon, const std::string& name,
                      std::string* sdp, std::string* error) {
  // `auto` means the first announcement that looks like one of our blocks: eight
  // channels of L24 at 48 kHz, which is what an eight-channel AES67 sender
  // declares and exactly one block's worth. It exists so commissioning a bench is
  // one command instead of a shell pipeline parsing the daemon's JSON — and it is
  // not silent about the choice, because a machine that picks for you has to say
  // what it picked.
  const bool automatic = name == "auto";
  daemon::json discovered;
  if (!daemon->browse_sources("all", &discovered, error)) {
    return fail(error,
                "commissioning: cannot browse for sources: " + reason_of(error));
  }
  if (!discovered.contains("remote_sources") ||
      !discovered["remote_sources"].is_array()) {
    return fail(error, "commissioning: the daemon returned no remote_sources list");
  }

  std::string seen;
  for (const daemon::json& entry : discovered["remote_sources"]) {
    const std::string entry_name = entry.value("name", std::string());
    if (!seen.empty()) {
      seen += ", ";
    }
    seen += entry_name.empty() ? std::string("(unnamed)") : entry_name;

    const std::string entry_sdp = entry.value("sdp", std::string());
    const bool matches = automatic
                             ? entry_sdp.find("L24/48000/8") != std::string::npos
                             : entry_name == name;
    if (!matches) {
      continue;
    }
    if (entry_sdp.empty()) {
      return fail(error, "commissioning: \"" + entry_name + "\" announces no SDP");
    }
    if (automatic) {
      log().write(LogLevel::info, "commissioning: chose the announcement \"" +
                                      entry_name + "\" (eight channels of L24)");
    }
    *sdp = entry_sdp;
    return true;
  }

  const std::string nothing =
      seen.empty() ? std::string() : " (discovered: " + seen + ")";
  if (automatic) {
    return fail(error,
                "commissioning: nothing discovered carries eight channels of L24 "
                "at 48 kHz" +
                    nothing);
  }
  return fail(error, "commissioning: nothing discovered is named \"" + name + "\"" +
                         nothing);
}

}  // namespace

bool commission(daemon::DaemonClient* daemon, const Config& config,
                Subscription subscription, const std::string& subscribe_to,
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

  // Publishing comes first and always: it is what makes this appliance a
  // transmitter, and a site's sinks are wired to the far end by hand or by
  // discovery rather than by us.
  for (size_t index = 0; index < config.blocks.size(); ++index) {
    const BlockConfig& block = config.blocks[index];
    const int stream_id = static_cast<int>(index);

    if (!daemon->put_source(stream_id, daemon::make_block_source(config, block),
                            error)) {
      return fail(error, "commissioning: cannot publish source " +
                             std::to_string(stream_id) + " (block " +
                             std::to_string(block.index) +
                             "): " + reason_of(error));
    }
    ++counts.sources_published;
  }

  // The receive side, if it was asked for.
  if (subscription == Subscription::self) {
    // Transmitting to ourselves. The SDP comes back from the daemon rather than
    // being invented here, so this exercises the same path a real site uses to
    // wire a sink.
    for (size_t index = 0; index < config.blocks.size(); ++index) {
      const int stream_id = static_cast<int>(index);
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
      if (!subscribe_sink(daemon, config.blocks[index], stream_id, sdp, &counts,
                          error)) {
        return false;
      }
    }
  } else if (subscription == Subscription::discovered) {
    if (config.blocks.empty()) {
      return fail(error, "commissioning: no blocks to subscribe a sink for");
    }
    std::string sdp;
    if (!announcement_sdp(daemon, subscribe_to, &sdp, error)) {
      return false;
    }
    log().write(LogLevel::info, "commissioning: subscribing block " +
                                    std::to_string(config.blocks.front().index) +
                                    " to \"" + subscribe_to + "\"");
    if (!subscribe_sink(daemon, config.blocks.front(), 0, sdp, &counts, error)) {
      return false;
    }
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
