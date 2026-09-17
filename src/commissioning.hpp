#pragma once

#include <cstddef>
#include <string>

#include "aes67/daemon_client.hpp"
#include "config.hpp"

namespace aes67_srt {

/**
 * What the daemon ended up holding, for the log and the status page.
 *
 * Counts rather than a bare success flag: the interesting failure is a stream
 * that was asked for and refused, and "7 of 8" is a sentence an operator can act
 * on where "failed" is not.
 */
struct CommissioningResult {
  size_t sources_published = 0;
  size_t sinks_subscribed = 0;
  size_t sinks_receiving = 0;
  std::string ptp_status{"unknown"};
  std::string gmid;
};

/**
 * What the sinks should subscribe to, if anything.
 *
 * The sources are always published — that is what makes this appliance a
 * transmitter — so this is only about the receive side, and it is the difference
 * between commissioning and operating: a site wires its sinks to the far end,
 * while a bench wires them to something it can hear.
 */
enum class Subscription {
  /** Publish our sources and subscribe nothing. */
  none,
  /**
   * Subscribe the **first block's** sink to a named discovery announcement.
   *
   * One block, not eight, because an AES67 stream carries at most eight channels:
   * eight sinks subscribing to one eight-channel sender would be the same audio
   * eight times over. A 64-channel site subscribes eight *different* streams, one
   * per block, which is the per-site mapping ticket 09 hands to the operator.
   */
  discovered,
  /**
   * Subscribe every block's sink to this appliance's own source: transmitting to
   * ourselves.
   *
   * **Measured 2026-09-17: the RAVENNA driver refuses this.**
   * `HTTP 400: failed to add sink 0 : (driver) command failed` — the source
   * document is accepted and the sink document parses, and then the kernel module
   * declines a sink pointed at this box's own multicast group. Kept because it is
   * the behaviour the spec names, and because a driver update could change it; not
   * the path to expect to work on a first commissioning.
   */
  self,
};

/**
 * Hand this appliance's audio to the daemon, and optionally take something back.
 */
bool commission(daemon::DaemonClient* daemon, const Config& config,
                Subscription subscription, const std::string& subscribe_to,
                CommissioningResult* result, std::string* error);

}  // namespace aes67_srt
