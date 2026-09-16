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
 * Configs that can actually come up on loopback.
 *
 * Every test uses its own ports: two SRT connections cannot share one, and a
 * previous test's socket lingers long enough to matter.
 */
aes67_srt::Config loopback_config(const std::string& mode, int local_port,
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
    listener_up = listener.open(loopback_config("listener", listener_port, ""),
                                &listener_error);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(250));

  std::string caller_error;
  const bool caller_connected =
      caller.open(loopback_config("caller", caller_port,
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

  // The assertion the whole design rests on: TLPKTDROP is off, so nothing was
  // discarded. If this ever fails, audio is being thrown away silently.
  LinkStats stats;
  std::string stats_error;
  CHECK(listener.stats(&stats, &stats_error));
  CHECK_EQ(stats.packets_dropped, static_cast<int64_t>(0));
  CHECK(stats.packets_received > 0);
  // The negotiated delay, which is the number the UI will label "delay".
  CHECK(stats.negotiated_latency_ms > 0);
  CHECK(listener.uptime_seconds() > 0.0);

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
        listener.open(loopback_config("listener", listener_port, "", passphrase),
                      &listener_error);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(250));

  std::string caller_error;
  const bool caller_connected = caller.open(
      loopback_config("caller", caller_port,
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
        first.open(loopback_config("rendezvous", first_port,
                                   "127.0.0.1:" + std::to_string(second_port)),
                   &first_error);
  });
  std::thread second_thread([&] {
    second.set_receive_timeout_ms(300);
    second_up =
        second.open(loopback_config("rendezvous", second_port,
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
  // flow-control window permits. The receiver overflows, SRT notices packets
  // missing and retransmits into the space that frees up — the same recovery path
  // a lossy WAN exercises.
  aes67_srt::Config listener_config =
      loopback_config("listener", listener_port, "");
  listener_config.link.receive_buffer_bytes = 262144;
  listener_config.link.flow_control_packets = 256;

  aes67_srt::Config caller_config = loopback_config(
      "caller", caller_port, "127.0.0.1:" + std::to_string(listener_port));
  caller_config.link.receive_buffer_bytes = 262144;
  caller_config.link.flow_control_packets = 256;

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
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
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
    } else if (!timed_out) {
      test::report_failure("starved receive", __FILE__, __LINE__, receive_error);
      break;
    }

    // Watch the delay while the strain is on. This is the measurement the ticket
    // asks for, and why it is not a yes/no test.
    LinkStats sampled;
    std::string stats_error;
    if (listener.stats(&sampled, &stats_error) &&
        sampled.receive_buffer_ms > peak_buffer_ms) {
      peak_buffer_ms = sampled.receive_buffer_ms;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  sending.join();

  LinkStats stats;
  std::string stats_error;
  CHECK(listener.stats(&stats, &stats_error));

  std::cout << "    starved link: " << received.size() << "/" << frame_count
            << " frames, peak receive buffer " << peak_buffer_ms
            << " ms, retransmitted " << stats.packets_retransmitted << ", dropped "
            << stats.packets_dropped << std::endl;

  // The promise: everything arrives, and nothing was thrown away.
  CHECK_EQ(received.size(), static_cast<size_t>(frame_count));
  for (const std::vector<uint8_t>& taken : received) {
    CHECK(taken == frame);
  }
  CHECK_EQ(stats.packets_dropped, static_cast<int64_t>(0));

  // And the strain has to have been real, or this test proved nothing: either
  // packets were retransmitted or the buffer held a queue.
  CHECK(stats.packets_retransmitted > 0 || peak_buffer_ms > 0);

  // Deliberately NOT asserted, because it is not what this proves: this is
  // backpressure and induced overflow on loopback, not packet loss across a WAN.
  // The real thing belongs to tickets 09 and 10, on real links.

  caller.close();
  listener.close();
}
