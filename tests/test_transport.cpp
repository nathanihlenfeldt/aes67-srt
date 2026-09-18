#include "transport/link.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "config.hpp"
#include "test_framework.hpp"
#include "wire/frame.hpp"

namespace {

using aes67_srt::transport::Link;
using aes67_srt::transport::LinkStats;

/**
 * Configurations that can actually come up on localhost.
 *
 * Note the name: these are the *socket* tests, which happen to use 127.0.0.1 as
 * the peer. `link.mode = "loopback"` is a different thing entirely — no socket,
 * no port, no library — and has its own helper and its own tests further down.
 *
 * Every test uses its own ports: two SRT connections cannot share one, and a
 * previous test's socket lingers long enough to matter.
 */
aes67_srt::Config localhost_config(const std::string& mode, int local_port,
                                   const std::string& peer,
                                   const std::string& passphrase = std::string()) {
  aes67_srt::Config config;
  config.link.mode = mode;
  config.link.role = "duplex";
  config.link.local_port = local_port;
  config.link.peer = peer;
  config.link.passphrase = passphrase;
  config.link.latency_ms = 120;
  config.audio.channels = 8;
  config.link.blocks = 1;
  return config;
}

/** A configuration for the in-process loopback: no socket, so no peer. */
aes67_srt::Config in_process_config(int local_port) {
  aes67_srt::Config config;
  config.link.mode = "loopback";
  config.link.role = "duplex";
  config.link.local_port = local_port;
  config.audio.channels = 8;
  config.link.blocks = 1;
  return config;
}

/** A frame big enough to need several SRT messages. */
std::vector<uint8_t> make_frame_bytes(size_t blocks, uint8_t seed) {
  aes67_srt::wire::Frame frame;
  frame.link_id = 1;
  frame.sequence = 1;
  frame.sample_position = 48000;
  for (size_t index = 0; index < blocks; ++index) {
    aes67_srt::wire::Block block;
    block.index = static_cast<uint8_t>(index);
    block.payload = aes67_srt::wire::PayloadType::pcm_l24;
    block.channels = 8;
    block.data.assign(48 * 8 * 3, static_cast<uint8_t>(seed + index));
    frame.blocks.push_back(std::move(block));
  }
  std::vector<uint8_t> bytes;
  std::string error;
  CHECK(aes67_srt::wire::encode(frame, &bytes, &error));
  return bytes;
}

/** Send one frame as the messages SRT will accept. */
bool send_frame_as_messages(Link* link, const std::vector<uint8_t>& frame,
                            std::string* error) {
  size_t offset = 0;
  const uint8_t* chunk = nullptr;
  size_t chunk_size = 0;
  while (aes67_srt::wire::next_fragment(frame.data(), frame.size(), &offset, &chunk,
                                        &chunk_size)) {
    if (!link->send_message(chunk, chunk_size, error)) {
      return false;
    }
  }
  return true;
}

/** Receive until |wanted| whole frames have been reassembled, or give up. */
std::vector<std::vector<uint8_t>> receive_frames(Link* link, size_t wanted,
                                                 int timeout_seconds) {
  std::vector<std::vector<uint8_t>> frames;
  aes67_srt::wire::Reassembler reassembler;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
  while (frames.size() < wanted && std::chrono::steady_clock::now() < deadline) {
    std::vector<uint8_t> message;
    bool timed_out = false;
    std::string error;
    if (!link->receive_message(&message, &timed_out, &error)) {
      if (timed_out) {
        continue;
      }
      test::report_failure("receive_message", __FILE__, __LINE__, error);
      break;
    }
    if (!reassembler.feed(message.data(), message.size(), &error)) {
      test::report_failure("reassembler.feed", __FILE__, __LINE__, error);
      break;
    }
    while (reassembler.frame_ready()) {
      std::vector<uint8_t> frame;
      if (!reassembler.take_frame(&frame, &error)) {
        test::report_failure("take_frame", __FILE__, __LINE__, error);
        break;
      }
      frames.push_back(std::move(frame));
    }
  }
  return frames;
}

bool contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

/** Skip the socket tests loudly when the build has no libsrt. */
bool skip_without_srt(const char* what) {
  if (Link::available()) {
    return false;
  }
  std::cout << "    (no libsrt in this build: skipping " << what << ")"
            << std::endl;
  return true;
}

}  // namespace

TEST_CASE(transport_reports_whether_it_has_libsrt) {
  // Both answers are correct somewhere: a development Mac without libsrt must
  // build and say so, and CI with libsrt must have it.
  if (Link::available()) {
    CHECK(Link::unavailable_reason().empty());
  } else {
    CHECK(!Link::unavailable_reason().empty());
    // Every operation refuses with the reason rather than pretending to work.
    Link link;
    std::string error;
    CHECK(!link.open(aes67_srt::Config(), &error));
    CHECK(contains(error, "libsrt"));
  }
}

TEST_CASE(transport_refuses_a_message_srt_could_not_carry) {
  Link link;
  std::string error;
  const std::vector<uint8_t> too_big(aes67_srt::wire::k_max_message_bytes + 1, 0);
  CHECK(!link.send_message(too_big.data(), too_big.size(), &error));
  CHECK(contains(error, "exceeds the"));
  CHECK(contains(error, "fragment the frame first"));
}

/**
 * Drain everything the loopback is holding, reassembled into frames.
 *
 * Terminates on the first quiet receive, which for an in-process pipe means
 * there is nothing left — unlike a socket, where a quiet moment means waiting.
 */
std::vector<std::vector<uint8_t>> drain_frames(Link* link) {
  std::vector<std::vector<uint8_t>> frames;
  aes67_srt::wire::Reassembler reassembler;
  std::string error;
  for (int guard = 0; guard < 100000; ++guard) {
    std::vector<uint8_t> message;
    bool timed_out = false;
    if (!link->receive_message(&message, &timed_out, &error)) {
      if (timed_out) {
        break;
      }
      test::report_failure("receive_message", __FILE__, __LINE__, error);
      break;
    }
    if (!reassembler.feed(message.data(), message.size(), &error)) {
      test::report_failure("reassembler.feed", __FILE__, __LINE__, error);
      break;
    }
  }
  while (reassembler.frame_ready()) {
    std::vector<uint8_t> frame;
    if (!reassembler.take_frame(&frame, &error)) {
      test::report_failure("take_frame", __FILE__, __LINE__, error);
      break;
    }
    frames.push_back(std::move(frame));
  }
  return frames;
}

// ---------------------------------------------------------------------------
// Loopback mode. Deliberately NOT guarded by skip_without_srt: the whole point
// of the mode is that the audio path can be exercised on a machine with nothing
// installed, so a build without libsrt skipping these would skip exactly the
// case they exist for.
// ---------------------------------------------------------------------------

TEST_CASE(transport_loops_back_with_no_socket_and_no_libsrt) {
  aes67_srt::Config config = in_process_config(19410);
  Link link;
  std::string error;

  // Refuses before opening, like every other mode.
  const std::vector<uint8_t> one_byte{0x00};
  CHECK(!link.send_message(one_byte.data(), one_byte.size(), &error));
  CHECK(!error.empty());

  CHECK(link.open(config, &error));
  CHECK(link.is_open());
  CHECK(contains(link.peer_description(), "loopback"));
  CHECK(link.uptime_seconds() >= 0.0);

  // One small frame first — one block fits in a single message — so the
  // queued-then-delivered property is asserted without a reassembler in the way.
  const std::vector<uint8_t> small = make_frame_bytes(1, 0x11);
  CHECK(small.size() < aes67_srt::wire::k_max_message_bytes);
  CHECK(send_frame_as_messages(&link, small, &error));

  std::vector<uint8_t> message;
  bool timed_out = false;
  CHECK(link.receive_message(&message, &timed_out, &error));
  CHECK(!timed_out);
  CHECK(message == small);  // queued, not delivered: both ends are here

  // Then one that needs eight messages, reassembled back to the same bytes. This
  // is the socket test's assertion, with no socket anywhere.
  const std::vector<uint8_t> large = make_frame_bytes(8, 0x5a);
  CHECK(large.size() > aes67_srt::wire::k_max_message_bytes * 6);
  CHECK(send_frame_as_messages(&link, large, &error));

  const std::vector<std::vector<uint8_t>> frames = drain_frames(&link);
  CHECK_EQ(frames.size(), static_cast<size_t>(1));
  CHECK(frames[0] == large);
}

TEST_CASE(transport_a_closed_loopback_hands_back_nothing) {
  aes67_srt::Config config = in_process_config(19411);
  Link link;
  std::string error;

  CHECK(link.open(config, &error));

  const std::vector<uint8_t> frame = make_frame_bytes(1, 0x22);
  const std::vector<uint8_t> one_byte{0x00};
  CHECK(send_frame_as_messages(&link, frame, &error));
  link.close();
  CHECK(!link.is_open());

  // Sending on a closed link refuses, whatever mode it was last in.
  CHECK(!link.send_message(one_byte.data(), one_byte.size(), &error));
  CHECK(!error.empty());

  // And reopening must not hand back the previous session's messages: that would
  // be a baffling thing to debug from the audio it produced.
  CHECK(link.open(config, &error));
  std::vector<uint8_t> message;
  bool timed_out = false;
  error.clear();
  CHECK(!link.receive_message(&message, &timed_out, &error));
  CHECK(timed_out);
  CHECK(error.empty());  // a quiet pipe is not a failure
  CHECK(drain_frames(&link).empty());
}

TEST_CASE(transport_a_loopback_refuses_rather_than_growing_for_ever) {
  aes67_srt::Config config = in_process_config(19412);
  Link link;
  std::string error;
  CHECK(link.open(config, &error));

  // Nothing is receiving, so the queue fills. The property that matters is not
  // the ceiling but the *behaviour*: it refuses, with a reason, rather than
  // consuming the machine's memory to hide a wiring mistake.
  const std::vector<uint8_t> message(1000, 0x33);
  bool refused = false;
  for (int sent = 0; sent < 100000; ++sent) {
    if (!link.send_message(message.data(), message.size(), &error)) {
      refused = true;
      CHECK(contains(error, "loopback"));
      CHECK(contains(error, "nothing is receiving"));
      // A quarter of a second of audio, so a correctly wired loopback never
      // meets the ceiling and this bound is not the point.
      CHECK(sent < 8192);
      break;
    }
  }
  CHECK(refused);
}

TEST_CASE(transport_a_loopback_has_no_link_statistics) {
  aes67_srt::Config config = in_process_config(19413);
  Link link;
  std::string error;
  CHECK(link.open(config, &error));

  // Refused rather than answered with zeroes: this class's own rule is that it
  // fails rather than returning plausible numbers, and a pipe that is not a
  // network has no RTT, no bandwidth and no loss to report.
  aes67_srt::transport::LinkStats stats;
  stats.rtt_ms = -1.0;
  CHECK(!link.stats(&stats, &error));
  CHECK(contains(error, "loopback"));
  CHECK_EQ(stats.rtt_ms, -1.0);  // untouched, not quietly filled with zeroes
}

TEST_CASE(transport_the_statistics_render_as_one_line) {
  // The formatter is where the diagnostic lives, and it is pure, so it is tested
  // with values rather than a socket: a run that delivered a fraction of what it
  // offered has to be readable from the log, and a mislabelled figure is worse
  // than a missing one.
  LinkStats stats;
  stats.rtt_ms = 12.34;
  stats.bandwidth_mbps = 74.5;
  stats.receive_rate_mbps = 3.1;
  stats.receive_buffer_ms = 120;
  stats.negotiated_latency_ms = 118;
  stats.send_buffer_ms = 120;
  stats.packets_received = 4801;
  stats.packets_lost = 2;
  stats.packets_retransmitted = 7;
  stats.packets_dropped = 0;

  const std::string line = aes67_srt::transport::to_string(stats);
  const auto has = [&line](const std::string& needle) {
    return line.find(needle) != std::string::npos;
  };
  CHECK(has("rtt 12.3 ms"));  // one decimal, not the whole double
  CHECK(has("bandwidth 74.5 Mbps"));
  CHECK(has("receive 3.1 Mbps"));  // the figure that names a starved link
  CHECK(has("receive buffer 120 ms"));
  CHECK(has("latency 118 ms"));
  CHECK(has("packets received 4801"));
  CHECK(has("lost 2"));
  CHECK(has("retransmitted 7"));
  CHECK(has("dropped 0"));
}

TEST_CASE(
    transport_a_send_to_a_peer_that_stops_reading_returns_rather_than_blocking) {
  // The other half of the non-blocking trap (issue #16): a peer that accepts the
  // connection and then reads nothing fills the sender's flow-control window, and
  // an unbounded srt_sendmsg parks the device-paced transmit loop inside a network
  // call — so the appliance never sees SIGTERM and systemd cannot stop it. Bounded
  // (SRTO_SNDTIMEO = 0, documented as a zero-millisecond limit), the send returns
  // and the loop's own retry-and-check cycle runs.
  if (skip_without_srt("the stalled-peer send test")) {
    return;
  }
  const int listener_port = 19811;
  const int caller_port = 19812;

  Link listener;
  Link caller;
  std::string listener_error;
  std::atomic<bool> listener_up{false};
  std::thread accepting([&] {
    listener.set_receive_timeout_ms(300);
    listener_up = listener.open(localhost_config("listener", listener_port, ""),
                                &listener_error);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(250));

  std::string caller_error;
  const bool caller_up =
      caller.open(localhost_config("caller", caller_port,
                                   "127.0.0.1:" + std::to_string(listener_port)),
                  &caller_error);
  accepting.join();
  CHECK(listener_up.load());
  CHECK(caller_up);
  if (!caller_up) {
    test::report_failure("the stalled-peer link did not come up", __FILE__,
                         __LINE__, caller_error + " / " + listener_error);
    return;
  }

  caller.set_send_timeout_ms(0);
  const std::vector<uint8_t> message(1200, 0x5a);
  bool refused = false;
  const auto start = std::chrono::steady_clock::now();
  // The window fills after a few thousand messages; without the bound this loop
  // would never reach the check.
  for (int sent = 0; sent < 200000 && !refused; ++sent) {
    if (!caller.send_message(message.data(), message.size(), &caller_error)) {
      refused = true;
    }
  }
  const double elapsed =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
          .count();

  CHECK(refused);
  CHECK(elapsed < 5.0);

  caller.close();
  listener.close();
}

TEST_CASE(transport_carries_frames_both_ways_on_one_connection) {
  if (skip_without_srt("the loopback test")) {
    return;
  }
  const int listener_port = 19401;
  const int caller_port = 19402;

  Link listener;
  Link caller;
  std::string listener_error;
  std::atomic<bool> listener_up{false};

  // The listener blocks until a caller arrives, so it gets its own thread.
  std::thread accepting([&] {
    listener.set_receive_timeout_ms(300);
    listener_up = listener.open(localhost_config("listener", listener_port, ""),
                                &listener_error);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(250));

  std::string caller_error;
  const bool caller_connected =
      caller.open(localhost_config("caller", caller_port,
                                   "127.0.0.1:" + std::to_string(listener_port)),
                  &caller_error);
  accepting.join();

  CHECK(caller_connected);
  CHECK(listener_up.load());
  if (!caller_connected || !listener_up.load()) {
    test::report_failure("loopback did not come up", __FILE__, __LINE__,
                         caller_error + " / " + listener_error);
    return;
  }

  // Caller to listener: three frames, each needing eight messages.
  const std::vector<uint8_t> frame = make_frame_bytes(8, 0x40);
  CHECK(frame.size() > aes67_srt::wire::k_max_message_bytes * 6);
  for (int index = 0; index < 3; ++index) {
    std::string error;
    if (!send_frame_as_messages(&caller, frame, &error)) {
      // Report SRT's own words: guessing at them has cost enough already.
      test::report_failure("send_frame_as_messages", __FILE__, __LINE__,
                           "frame " + std::to_string(index) + ": " + error);
    }
  }
  const std::vector<std::vector<uint8_t>> forward = receive_frames(&listener, 3, 5);
  CHECK_EQ(forward.size(), static_cast<size_t>(3));
  for (const std::vector<uint8_t>& received : forward) {
    CHECK(received == frame);
  }

  // And back the other way on the same connection: the link is duplex, which is
  // what lets one appliance be transmitter and receiver at once.
  const std::vector<uint8_t> back = make_frame_bytes(1, 0x90);
  {
    std::string error;
    CHECK(send_frame_as_messages(&listener, back, &error));
  }
  const std::vector<std::vector<uint8_t>> reverse = receive_frames(&caller, 1, 5);
  CHECK_EQ(reverse.size(), static_cast<size_t>(1));
  if (!reverse.empty()) {
    CHECK(reverse.front() == back);
  }

  // TLPKTDROP is on, so a packet that missed its play time would be discarded
  // rather than stalling the receiver. A healthy round trip drops none of them,
  // so anything non-zero here is a link that is already too late.
  LinkStats stats;
  std::string stats_error;
  CHECK(listener.stats(&stats, &stats_error));
  CHECK_EQ(stats.packets_dropped, static_cast<int64_t>(0));
  CHECK(stats.packets_received > 0);
  // The negotiated delay, which is the number the UI will label "delay".
  CHECK(stats.negotiated_latency_ms > 0);
  CHECK(listener.uptime_seconds() > 0.0);
  // And it renders: the log line an operator reads has to come from the same
  // structure the UI polls, not from a second path that can disagree with it.
  const std::string rendered = aes67_srt::transport::to_string(stats);
  CHECK(rendered.find("rtt ") != std::string::npos);
  CHECK(rendered.find("packets received ") != std::string::npos);
  CHECK(rendered.find("dropped 0") != std::string::npos);

  caller.close();
  listener.close();
  CHECK(!listener.is_open());
}

TEST_CASE(transport_carries_a_passphrase_link) {
  if (skip_without_srt("the passphrase test")) {
    return;
  }
  const int listener_port = 19411;
  const int caller_port = 19412;
  const std::string passphrase = "a-shared-secret-for-the-link";

  Link listener;
  Link caller;
  std::string listener_error;
  std::atomic<bool> listener_up{false};
  std::thread accepting([&] {
    listener.set_receive_timeout_ms(300);
    listener_up =
        listener.open(localhost_config("listener", listener_port, "", passphrase),
                      &listener_error);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(250));

  std::string caller_error;
  const bool caller_connected = caller.open(
      localhost_config("caller", caller_port,
                       "127.0.0.1:" + std::to_string(listener_port), passphrase),
      &caller_error);
  accepting.join();

  CHECK(listener_up.load());
  CHECK(caller_connected);
  if (!listener_up.load() || !caller_connected) {
    test::report_failure("encrypted loopback did not come up", __FILE__, __LINE__,
                         caller_error + " / " + listener_error);
    return;
  }

  const std::vector<uint8_t> frame = make_frame_bytes(1, 0x11);
  std::string error;
  CHECK(send_frame_as_messages(&caller, frame, &error));
  const std::vector<std::vector<uint8_t>> received =
      receive_frames(&listener, 1, 5);
  CHECK_EQ(received.size(), static_cast<size_t>(1));
  if (!received.empty()) {
    CHECK(received.front() == frame);
  }

  caller.close();
  listener.close();
}

TEST_CASE(transport_carries_a_rendezvous_link) {
  if (skip_without_srt("the rendezvous test")) {
    return;
  }
  const int first_port = 19421;
  const int second_port = 19422;

  // Both ends bind their own port and connect to the other's: a coordinated
  // configuration rather than a discovery mechanism, which is what an install at
  // a fixed site can provide and a roaming peer cannot.
  Link first;
  Link second;
  std::string first_error;
  std::string second_error;
  std::atomic<bool> first_up{false};
  std::atomic<bool> second_up{false};

  std::thread first_thread([&] {
    first.set_receive_timeout_ms(300);
    first_up =
        first.open(localhost_config("rendezvous", first_port,
                                    "127.0.0.1:" + std::to_string(second_port)),
                   &first_error);
  });
  std::thread second_thread([&] {
    second.set_receive_timeout_ms(300);
    second_up =
        second.open(localhost_config("rendezvous", second_port,
                                     "127.0.0.1:" + std::to_string(first_port)),
                    &second_error);
  });
  first_thread.join();
  second_thread.join();

  CHECK(first_up.load());
  CHECK(second_up.load());
  if (!first_up.load() || !second_up.load()) {
    test::report_failure("rendezvous did not come up", __FILE__, __LINE__,
                         first_error + " / " + second_error);
    return;
  }

  const std::vector<uint8_t> frame = make_frame_bytes(1, 0x22);

  // Rendezvous completes its handshake from both ends at once, so srt_connect can
  // return before the peer has finished, and a frame sent inside that window is
  // discarded — the statistics showed the peer receiving nothing at all. A real
  // appliance sends a thousand frames a second and would never notice; a test
  // that sends exactly one tests the first twenty milliseconds, not the link.
  std::vector<std::vector<uint8_t>> received;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
  int attempts = 0;
  while (received.empty() && std::chrono::steady_clock::now() < deadline) {
    ++attempts;
    std::string send_error;
    send_frame_as_messages(&first, frame, &send_error);
    received = receive_frames(&second, 1, 1);
  }

  if (received.empty()) {
    // Say what both ends saw rather than merely "expected 1, got 0".
    LinkStats first_stats;
    LinkStats second_stats;
    std::string ignored;
    const bool first_ok = first.stats(&first_stats, &ignored);
    const bool second_ok = second.stats(&second_stats, &ignored);
    test::report_failure(
        "rendezvous delivery", __FILE__, __LINE__,
        "no frame after " + std::to_string(attempts) + " attempts; first " +
            (first_ok
                 ? ("received=" + std::to_string(first_stats.packets_received) +
                    " dropped=" + std::to_string(first_stats.packets_dropped))
                 : std::string("stats unavailable")) +
            ", second " +
            (second_ok
                 ? ("received=" + std::to_string(second_stats.packets_received) +
                    " dropped=" + std::to_string(second_stats.packets_dropped))
                 : std::string("stats unavailable")));
  } else {
    CHECK(received.front() == frame);
  }
  CHECK_EQ(received.size(), static_cast<size_t>(1));

  first.close();
  second.close();
}

TEST_CASE(transport_takes_strain_as_delay_and_never_drops_audio) {
  if (skip_without_srt("the starved-link test")) {
    return;
  }
  const int listener_port = 19441;
  const int caller_port = 19442;
  const int frame_count = 20;

  // Starve the link deliberately, and without root so it runs in CI on both
  // platforms. The mechanism: a receive buffer far too small to hold what the
  // flow-control window permits...
  //
  // ...and the sizes have to be *right*, not merely small. The first attempt at
  // this set a receive buffer smaller than the flow-control window can fill
  // (8 KB against a 32-packet window), and SRT does not check that: the receiver
  // quietly overflowed, two frames of five never arrived, and the library
  // reported neither a retransmission nor a drop. That is a way to lose audio,
  // not a way to starve a link — and this project's whole promise is that it
  // never loses audio. The buffer must be able to hold the whole window
  // (32 packets x 1456 bytes is 46 KB, so 64 KB does), and then what forces the
  // sender to stop is the *reader* being slow, not the buffer being too small.
  aes67_srt::Config listener_config =
      localhost_config("listener", listener_port, "");
  listener_config.link.receive_buffer_bytes = 65536;
  listener_config.link.flow_control_packets = 32;

  aes67_srt::Config caller_config = localhost_config(
      "caller", caller_port, "127.0.0.1:" + std::to_string(listener_port));
  caller_config.link.receive_buffer_bytes = 65536;
  caller_config.link.flow_control_packets = 32;

  Link listener;
  Link caller;
  std::string listener_error;
  std::atomic<bool> listener_up{false};
  std::thread accepting([&] {
    listener.set_receive_timeout_ms(100);
    listener_up = listener.open(listener_config, &listener_error);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(250));

  std::string caller_error;
  const bool caller_connected = caller.open(caller_config, &caller_error);
  accepting.join();

  CHECK(listener_up.load());
  CHECK(caller_connected);
  if (!listener_up.load() || !caller_connected) {
    test::report_failure("starved link did not come up", __FILE__, __LINE__,
                         caller_error + " / " + listener_error);
    return;
  }

  // The sender behaves like a real stream: forty frames, eight messages each, with
  // no pause to see how the far end is coping.
  const std::vector<uint8_t> frame = make_frame_bytes(8, 0x33);
  std::thread sending([&] {
    for (int index = 0; index < frame_count; ++index) {
      std::string send_error;
      if (!send_frame_as_messages(&caller, frame, &send_error)) {
        test::report_failure("starved send", __FILE__, __LINE__,
                             "frame " + std::to_string(index) + ": " + send_error);
        return;
      }
    }
  });

  // The receiver drains deliberately slower than the sender fills, so the link has
  // to hold what it cannot deliver yet.
  std::vector<std::vector<uint8_t>> received;
  aes67_srt::wire::Reassembler reassembler;
  int peak_buffer_ms = 0;
  int64_t peak_retransmitted = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (received.size() < static_cast<size_t>(frame_count) &&
         std::chrono::steady_clock::now() < deadline) {
    std::vector<uint8_t> message;
    bool timed_out = false;
    std::string receive_error;
    if (listener.receive_message(&message, &timed_out, &receive_error)) {
      if (!reassembler.feed(message.data(), message.size(), &receive_error)) {
        test::report_failure("starved reassembly", __FILE__, __LINE__,
                             receive_error);
        break;
      }
      while (reassembler.frame_ready()) {
        std::vector<uint8_t> taken;
        if (!reassembler.take_frame(&taken, &receive_error)) {
          break;
        }
        received.push_back(std::move(taken));
      }
      // The consumer is deliberately slow: 20 ms per message is far below the
      // rate the sender fills at, so the receive buffer fills, the flow-control
      // window closes and the *sender* has to stop. That is the strain: it shows
      // up as a stalled sender and a deeper buffer, not as loss.
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } else if (!timed_out) {
      test::report_failure("starved receive", __FILE__, __LINE__, receive_error);
      break;
    }

    // Watch the delay while the strain is on. This is the measurement the ticket
    // asks for, and why it is not a yes/no test.
    LinkStats sampled;
    std::string stats_error;
    if (listener.stats(&sampled, &stats_error)) {
      if (sampled.receive_buffer_ms > peak_buffer_ms) {
        peak_buffer_ms = sampled.receive_buffer_ms;
      }
      if (sampled.packets_retransmitted > peak_retransmitted) {
        peak_retransmitted = sampled.packets_retransmitted;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  sending.join();

  LinkStats stats;
  std::string stats_error;
  CHECK(listener.stats(&stats, &stats_error));

  std::cout << "    starved link: " << received.size() << "/" << frame_count
            << " frames, peak receive buffer " << peak_buffer_ms
            << " ms, retransmitted " << peak_retransmitted << " peak ("
            << stats.packets_retransmitted << " total), dropped "
            << stats.packets_dropped << std::endl;

  // The promise: everything arrives, and nothing was thrown away. TLPKTDROP is
  // on, so a packet too late to play would be counted here rather than stalling
  // the link; a loopback round trip delivers them all in time.
  CHECK_EQ(received.size(), static_cast<size_t>(frame_count));
  for (const std::vector<uint8_t>& taken : received) {
    CHECK(taken == frame);
  }
  CHECK_EQ(stats.packets_dropped, static_cast<int64_t>(0));

  // NO assertion here that the strain was visible, and that is now a *measured*
  // finding rather than an omission. The history is worth keeping because it cost
  // two configurations to learn:
  //
  //   buffer 8 KB, window 32 packets, 5 frames (the ticket's own mechanism, i.e.
  //   a buffer smaller than the window can fill):
  //     -> 3 of 5 frames arrived, retransmitted 0, dropped 0
  //     8 KB cannot hold the 32-packet window, and SRT does not check that. The
  //     receiver silently overflowed. Losing audio is not starving a link, and
  //     nothing in the statistics said so.
  //
  //   buffer 64 KB, window 32 packets, 20 frames, reader pausing 20 ms/message:
  //     -> 20 of 20 frames arrived intact, peak buffer 1 ms, retransmitted 0,
  //        dropped 0
  //     The buffer holds the whole window, so nothing overflows and the never-drop
  //     promise holds under real backpressure. But the strain never became
  //     *visible*: `msRcvBuf` is "undelivered timespan (msec) of UDT receiver"
  //     (srt.h:382), and in live mode packets go straight into the TSBPD playout
  //     buffer, so the UDT receiver's own buffer stays empty however hard the
  //     reader is squeezed.
  //
  // So the delay the operator will actually see is not in this module's
  // statistics: it is the sender's sample position against the receiver's
  // playout, which needs the clock module (ticket 11). **Criterion 3 of ticket 16
  // is not reachable from here**, and saying so with two measurements behind it is
  // the useful result; asserting a rise that cannot happen made a red build out of
  // something the test does not claim.
  //
  // What this test *does* claim, it proves, and it is the promise the whole design
  // rests on: under backpressure, every frame arrives intact and none is dropped.

  // Deliberately NOT asserted, because it is not what this proves: this is
  // backpressure and induced overflow on loopback, not packet loss across a WAN.
  // The real thing belongs to tickets 09 and 10, on real links.

  caller.close();
  listener.close();
}
