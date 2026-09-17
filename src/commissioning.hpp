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
 * Hand this appliance's audio to the daemon, and optionally take it back.
 *
 * **Every block becomes one daemon source**, carrying that block's eight device
 * channels — the same `map` the engine packs its frames from, so the AES67 side
 * and the wire side cannot disagree about which channels are which. Stream id
 * `i` is block `i`, which is a convention rather than a coincidence: it is what
 * lets a human match a stream in a routing grid to a block in the configuration
 * without a lookup table.
 *
 * With `loopback`, each block's sink is also subscribed **to the source this
 * same appliance just published** — the commissioning loopback, transmitting to
 * ourselves. That is how a single box is proved end to end: the daemon publishes
 * our capture, subscribes to it, and reports whether packets are arriving. On a
 * device whose blocks cover every channel the audio returns to the channels it
 * came from, so the *verifiable* outcome is not the audio but the daemon's own
 * answer: a sink that reports `receiving_rtp_packet`. That answer needs PTP
 * locked and the whole AES67 path working, which is exactly what commissioning
 * is trying to establish.
 *
 * Returns false and fills `error` on the first refusal, having done whatever it
 * had already done — deliberately not rolled back, because a partially wired
 * daemon that says which part failed is more useful to whoever is standing in
 * front of it than a silent cleanup.
 */
bool commission(daemon::DaemonClient* daemon, const Config& config, bool loopback,
                CommissioningResult* result, std::string* error);

}  // namespace aes67_srt
