#pragma once

#include <string>
#include <vector>

namespace aes67_srt {

/**
 * One 8-channel block.
 *
 * Eight is not a preference: AES67 allows no more than eight channels per
 * stream, so the block is the unit the whole system already thinks in.  See
 * docs/spec/0001-aes67-srt.md.
 */
struct BlockConfig {
  int index = 0;
  /** Device channels this block carries. Exactly 8, per the AES67 stream size. */
  std::vector<int> channels;
  double gain_db = 0.0;
  bool mute = false;
};

struct AudioConfig {
  std::string backend = "ravenna";  // ravenna | null
  std::string device = "plughw:RAVENNA";
  int channels = 64;
  int sample_rate = 48000;
  std::string format = "s24_3le";  // s24_3le | s16_le
  int period_frames = 48;          // 1 ms at 48 kHz, matching AES67's ptime
  int periods = 8;
};

struct LinkConfig {
  std::string role = "duplex";      // tx | rx | duplex
  std::string mode = "rendezvous";  // caller | listener | rendezvous
  std::string peer;                 // host:port; required unless mode=listener
  int local_port = 9000;
  int latency_ms = 120;
  int alarm_delay_ms = 500;
  std::string passphrase;
  int blocks = 8;
};

struct DaemonConfig {
  std::string address = "127.0.0.1";
  int port = 8080;
  bool fake = false;
};

/** The A/V alignment side of the design; delay can only ever be added. */
struct EgressConfig {
  double delay_ms = 0.0;
  int test_signal_channel = -1;  // -1 = off
};

struct Config {
  int version = 1;
  AudioConfig audio;
  LinkConfig link;
  DaemonConfig daemon;
  EgressConfig egress;
  std::vector<BlockConfig> blocks;
  std::string http_addr = "0.0.0.0";
  int http_port = 8082;

  /**
   * Check everything that could stop audio, before anything is written or
   * applied.  Returns false and fills |reason| with a plain-text message naming
   * the offending field, so the operator can see which line to fix.
   */
  bool validate(std::string* reason) const;

  /** Block i carries device channels 8i..8i+7 — the conventional mapping. */
  void fill_default_blocks();

  /** Serialise back to a JSON document, in the same shape as the file. */
  std::string to_json() const;
};

/**
 * Parse and validate a configuration document.
 *
 * Refuses rather than repairs.  An unknown key is an error, not something to
 * ignore: the commonest way an appliance appears broken is a typo in a key name
 * that is silently defaulted past.
 */
bool parse_config(const std::string& json_text, Config* config,
                  std::string* reason);

/** Read a file and parse it. Fills |reason| on failure, including on I/O. */
bool load_config(const std::string& path, Config* config, std::string* reason);

}  // namespace aes67_srt
