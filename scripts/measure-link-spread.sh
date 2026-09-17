#!/usr/bin/env bash
#
# Measures the spread of frame arrival times at a receiver, which is the number the
# clock module's design turns on and which nobody had measured.
#
#   On one machine:  ./scripts/measure-link-spread.sh listen [seconds]
#   On the other:    ./scripts/measure-link-spread.sh send <host> [seconds]
#
# Then read the receiver's report. The sender mirrors src/transport/link.cpp's SRT
# options exactly — live mode, the message API, 120 ms latency both ways, a 1316-byte
# payload cap, TLPKTDROP off — and pushes one frame's worth of messages per
# millisecond (8 x 1164 = 9312 bytes, this appliance's real shape and rate, 74 Mbit/s).
#
# Why it matters, and what each figure decides:
#
#   - "robust sd" is the typical arrival spread, which is what a *direct rate
#     estimate* — fitting the sender's sample position against arrival times — has to
#     work against. With it, the window such an estimate needs is arithmetic
#     (error ~ sigma * sqrt(12/n) / span, with n = 1000 * span).
#   - "slips" is how often SRT's TSBPD holds a frame back and releases it, which the
#     naive standard deviation is dominated by. A least-squares fit sees those as
#     outliers, so the estimator has to be robust or it is fitting the slips.
#   - "sender schedule" separates the sender's own clock and scheduler from the link:
#     a figure here comparable to the arrival spread would mean the measurement says
#     more about the sending host than about the network.
#
# Measured baseline, Raspberry Pi 5 (sender) to a laptop (receiver) across two routed
# subnets, 2026-09-17 — a path with 43 ms of round trip and 42 ms of ping jitter, so
# harsher than a LAN and less harsh than the open internet. Over 120 s and 119,879
# frames: typical arrival 14.83 us, 17% of frames outside +/-60 us, 0.05% slipping a
# whole period, sender's own scheduler 2.5 us. The full analysis is in
# docs/research/clock-recovery.md.
#
set -euo pipefail

ROLE="${1:-}"
if [ -z "${ROLE}" ]; then
  echo "usage: $0 listen [seconds] | send <host> [seconds]" >&2
  exit 1
fi
shift

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "${WORK_DIR}"' EXIT

# The probe is C, with libsrt directly rather than this repository's transport: it
# measures the link, so it must not depend on the code being measured.
cat > "${WORK_DIR}/link_spread.c" <<'PROBE'
/*
 * Arrival-spread probe: listen (a receiver) or send (a sender).
 *
 * Both roles mirror src/transport/link.cpp's options so that what is measured is
 * what the appliance does. The sender paces from an absolute schedule on its own
 * clock, so its wake-up lateness is a constant offset rather than drift, and it
 * reports that lateness so the receiver's spread can be attributed.
 */
#include <srt/srt.h>

#include <arpa/inet.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PORT 9100
#define MESSAGES_PER_FRAME 8
#define MESSAGE_BYTES 1164
#define LATENCY_MS 120
#define PAYLOAD_CAP 1316
#define MAX_SAMPLES 4000000

static double now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int compare_doubles(const void* a, const void* b) {
  const double left = *(const double*)a;
  const double right = *(const double*)b;
  return left < right ? -1 : (left > right ? 1 : 0);
}

static void sleep_until(double target) {
  for (;;) {
    const double left = target - now_s();
    if (left <= 0.0) {
      return;
    }
    struct timespec ts;
    ts.tv_sec = (time_t)left;
    ts.tv_nsec = (long)((left - (double)ts.tv_sec) * 1e9);
    nanosleep(&ts, NULL);
  }
}

/* The options both roles share, and the reason each is here:
 *   livemode + messageapi  what the appliance runs
 *   latency both ways      120 ms, the configured figure
 *   payload cap            1316 bytes, what a live-mode send will accept
 *   TLPKTDROP off          a late frame is delivered late, not dropped, so the
 *                          spread shows lateness instead of hiding it
 */
static void configure(SRTSOCKET socket) {
  int live = SRTT_LIVE;
  int yes = 1;
  int latency = LATENCY_MS;
  int payload = PAYLOAD_CAP;
  int drop = 0;
  srt_setsockopt(socket, 0, SRTO_TRANSTYPE, &live, sizeof(live));
  srt_setsockopt(socket, 0, SRTO_MESSAGEAPI, &yes, sizeof(yes));
  srt_setsockopt(socket, 0, SRTO_RCVLATENCY, &latency, sizeof(latency));
  srt_setsockopt(socket, 0, SRTO_PEERLATENCY, &latency, sizeof(latency));
  srt_setsockopt(socket, 0, SRTO_PAYLOADSIZE, &payload, sizeof(payload));
  srt_setsockopt(socket, 0, SRTO_TLPKTDROP, &drop, sizeof(drop));
}

static int listen_role(double seconds);
static int send_role(const char* host, double seconds);

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s listen [seconds] | send <host> [seconds]\n", argv[0]);
    return 1;
  }
  srt_startup();
  int status;
  if (strcmp(argv[1], "listen") == 0) {
    status = listen_role(argc > 2 ? atof(argv[2]) : 0.0);
  } else if (strcmp(argv[1], "send") == 0 && argc > 2) {
    status = send_role(argv[2], argc > 3 ? atof(argv[3]) : 120.0);
  } else {
    fprintf(stderr, "usage: %s listen [seconds] | send <host> [seconds]\n", argv[0]);
    status = 1;
  }
  srt_cleanup();
  return status;
}

static int listen_role(double seconds) {
  SRTSOCKET listener = srt_create_socket();
  configure(listener);
  struct sockaddr_in address;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons(PORT);
  address.sin_addr.s_addr = INADDR_ANY;
  if (srt_bind(listener, (struct sockaddr*)&address, sizeof(address)) == SRT_ERROR) {
    fprintf(stderr, "bind: %s\n", srt_getlasterror_str());
    return 1;
  }
  if (srt_listen(listener, 1) == SRT_ERROR) {
    fprintf(stderr, "listen: %s\n", srt_getlasterror_str());
    return 1;
  }
  printf("listening on %d, waiting for a sender...\n", PORT);
  fflush(stdout);
  SRTSOCKET connection = srt_accept(listener, NULL, NULL);
  if (connection == SRT_INVALID_SOCK) {
    fprintf(stderr, "accept: %s\n", srt_getlasterror_str());
    return 1;
  }
  printf("connected; the report comes when the sender stops\n");
  fflush(stdout);

  double* intervals = malloc(sizeof(double) * MAX_SAMPLES);
  char buffer[2048];
  long messages = 0;
  size_t samples = 0;
  double first = 0.0;
  double previous = 0.0;
  const double deadline = seconds > 0.0 ? now_s() + seconds : 0.0;

  for (;;) {
    if (deadline > 0.0 && now_s() > deadline) {
      break;
    }
    const int received = srt_recvmsg(connection, buffer, sizeof(buffer));
    if (received == SRT_ERROR) {
      break; /* the sender stopped, which is the normal end of a run */
    }
    ++messages;
    if (messages % MESSAGES_PER_FRAME != 0) {
      continue; /* only frame boundaries are timed */
    }
    const double arrival = now_s();
    if (first == 0.0) {
      first = arrival;
      previous = arrival;
      continue;
    }
    if (samples < MAX_SAMPLES) {
      intervals[samples++] = arrival - previous;
    }
    previous = arrival;
  }
  if (samples < 100) {
    printf("not enough frames to say anything (%zu)\n", samples);
    return 1;
  }

  double sum = 0.0;
  double sum_of_squares = 0.0;
  double lowest = 1e9;
  double highest = 0.0;
  for (size_t i = 0; i < samples; ++i) {
    const double value = intervals[i];
    sum += value;
    sum_of_squares += value * value;
    if (value < lowest) lowest = value;
    if (value > highest) highest = value;
  }
  const double mean = sum / (double)samples;
  const double variance = sum_of_squares / (double)samples - mean * mean;
  const double deviation = variance > 0.0 ? sqrt(variance) : 0.0;

  double* sorted = malloc(sizeof(double) * samples);
  memcpy(sorted, intervals, sizeof(double) * samples);
  qsort(sorted, samples, sizeof(double), compare_doubles);
  const double median = sorted[samples / 2];
  double* deviations = malloc(sizeof(double) * samples);
  for (size_t i = 0; i < samples; ++i) {
    deviations[i] = fabs(intervals[i] - median);
  }
  qsort(deviations, samples, sizeof(double), compare_doubles);
  /* 1.4826 x the median absolute deviation: a standard deviation for a Gaussian, and
   * a description of the core for anything else — which this distribution is. */
  const double robust_sd = 1.4826 * deviations[samples / 2];

  size_t near_zero = 0;
  size_t slips = 0;
  size_t outside = 0;
  for (size_t i = 0; i < samples; ++i) {
    if (intervals[i] < 0.0005) ++near_zero;
    if (intervals[i] > 0.0015) ++slips;
    if (fabs(intervals[i] - median) > 60e-6) ++outside;
  }

  printf("\n--- arrival spread over %.1f s, %zu frames (%ld messages)\n",
         previous - first, samples, messages);
  printf("interval     mean %.1f us   median %.1f us   min %.1f   max %.1f\n",
         mean * 1e6, median * 1e6, lowest * 1e6, highest * 1e6);
  printf("typical      robust sd %.2f us\n", robust_sd * 1e6);
  printf("not typical  sd %.1f us; %zu frames (%.1f%%) outside +/-60 us\n",
         deviation * 1e6, outside, 100.0 * (double)outside / (double)samples);
  printf("slips        %zu near-zero, %zu longer than 1.5 ms  (%.2f%% of frames)\n",
         near_zero, slips, 100.0 * (double)(near_zero + slips) / (double)samples);
  printf("offset       %+.2f ppm from the median, +/- %.1f ppm over this run\n",
         (0.001 / median - 1.0) * 1e6, robust_sd / sqrt((double)samples) / median * 1e6);
  printf("             a median of intervals is a weak estimator; a slope over the\n");
  printf("             whole span is orders better. The robust sd is the figure to\n");
  printf("             put into the window arithmetic, not this precision.\n");

  const int buckets = 24;
  const double width = 5e-6;
  size_t counts[24];
  memset(counts, 0, sizeof(counts));
  size_t far = 0;
  for (size_t i = 0; i < samples; ++i) {
    const int index = (int)((intervals[i] - (0.001 - 12 * width)) / width);
    if (index < 0 || index >= buckets) {
      ++far;
    } else {
      ++counts[index];
    }
  }
  printf("histogram, 5 us a column, centred on 1 ms (%zu frames outside):\n", far);
  for (int i = 0; i < buckets; ++i) {
    printf("  %8.1f us %7zu  ", (0.001 - 12 * width + width * i) * 1e6, counts[i]);
    for (size_t mark = 0; mark < counts[i] * 60 / samples; ++mark) {
      printf("*");
    }
    printf("\n");
  }
  free(sorted);
  free(deviations);
  free(intervals);
  srt_close(connection);
  srt_close(listener);
  return 0;
}

static int send_role(const char* host, double seconds) {
  SRTSOCKET socket = srt_create_socket();
  configure(socket);
  struct sockaddr_in address;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons(PORT);
  if (inet_pton(AF_INET, host, &address.sin_addr) != 1) {
    fprintf(stderr, "bad address %s\n", host);
    return 1;
  }
  if (srt_connect(socket, (struct sockaddr*)&address, sizeof(address)) == SRT_ERROR) {
    fprintf(stderr, "connect: %s\n", srt_getlasterror_str());
    return 1;
  }
  printf("connected to %s:%d, sending for %.0f s\n", host, PORT, seconds);

  char message[MESSAGE_BYTES];
  memset(message, 0x5a, sizeof(message));
  const long frames = (long)(seconds * 1000.0);
  const double started = now_s();
  /* This host's own scheduling lateness, so the receiver's spread can be attributed:
   * a figure here comparable to the arrival spread would mean the measurement says
   * more about the sender than about the link. */
  double lateness_sum = 0.0;
  double lateness_square_sum = 0.0;
  double worst = 0.0;
  long sent = 0;
  long failed = 0;

  for (long frame = 0; frame < frames; ++frame) {
    for (int part = 0; part < MESSAGES_PER_FRAME; ++part) {
      /* Live mode: what is sent once must not be sent again, so a failure is counted
       * and the run carries on. */
      if (srt_sendmsg(socket, message, sizeof(message), -1, 0) == SRT_ERROR) {
        ++failed;
      }
    }
    ++sent;
    const double target = started + (double)(frame + 1) / 1000.0;
    sleep_until(target);
    const double late = now_s() - target;
    lateness_sum += late;
    lateness_square_sum += late * late;
    if (late > worst) {
      worst = late;
    }
  }

  const double elapsed = now_s() - started;
  const double mean_late = lateness_sum / (double)frames;
  const double late_variance =
      lateness_square_sum / (double)frames - mean_late * mean_late;
  printf("sent %ld frames (%ld messages, %ld failed) in %.1f s = %.4f frames/s\n", sent,
         sent * MESSAGES_PER_FRAME, failed, elapsed, (double)sent / elapsed);
  printf("sender schedule: mean lateness %.1f us, sd %.1f us, worst %.1f us\n",
         mean_late * 1e6, sqrt(late_variance > 0.0 ? late_variance : 0.0) * 1e6,
         worst * 1e6);
  srt_close(socket);
  return 0;
}
PROBE

# Link it. pkg-config knows where libsrt is on both platforms this repository builds
# on; the Homebrew hints are the fallback for a machine without its .pc file.
if pkg-config --exists srt 2>/dev/null; then
  SRT_CFLAGS="$(pkg-config --cflags srt)"
  SRT_LIBS="$(pkg-config --libs srt)"
else
  SRT_CFLAGS="-I/opt/homebrew/include -I/usr/local/include"
  SRT_LIBS="-L/opt/homebrew/lib -L/usr/local/lib -lsrt"
fi
if ! cc -O2 -o "${WORK_DIR}/link_spread" "${WORK_DIR}/link_spread.c" \
     ${SRT_CFLAGS} ${SRT_LIBS} -lm 2> "${WORK_DIR}/cc.log"; then
  echo "could not build the probe; the compiler said:" >&2
  sed 's/^/    /' "${WORK_DIR}/cc.log" >&2
  echo "libsrt is needed: brew install srt, or apt install libsrt-openssl-dev" >&2
  exit 1
fi

case "${ROLE}" in
  listen)
    echo "==> listening: the sender runs on the other machine, and the report comes"
    echo "    when it stops. Ctrl-C to abandon."
    exec "${WORK_DIR}/link_spread" listen "${1:-0}"
    ;;
  send)
    if [ -z "${1:-}" ]; then
      echo "usage: $0 send <host> [seconds]" >&2
      exit 1
    fi
    exec "${WORK_DIR}/link_spread" send "${1}" "${2:-120}"
    ;;
  *)
    echo "usage: $0 listen [seconds] | send <host> [seconds]" >&2
    exit 1
    ;;
esac