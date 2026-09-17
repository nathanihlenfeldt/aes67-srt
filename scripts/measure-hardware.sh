#!/usr/bin/env bash
#
# Collects, on the appliance itself, the hardware measurements this project is
# blocked on. Writes one report file to send back.
#
#   bash measure-hardware.sh            # writes hardware-report-<host>-<date>.txt
#   curl -fsSL <url> -o /tmp/measure.sh && bash /tmp/measure.sh
#   bash measure-hardware.sh --commission [--subscribe <name|self|auto>]
#                                       # also wires the daemon and proves the whole
#                                       # path on one box (see the promise below)
#
# SAFE TO PIPE FROM A URL, and it says so rather than asking you to trust it:
#
#   * needs no root and asks for none
#   * installs nothing and changes no configuration
#   * writes exactly one file, the report, in the directory you run it from
#   * compiles two small probes in a temporary directory and deletes it
#   * reads the daemon's REST API on localhost. The default run does not write to
#     it. **--commission is the one exception, and it says so**: it asks our own
#     binary to publish one AES67 source per block and to subscribe at most one
#     sink — two documents per block for the sources, one for the sink. It is off
#     unless you ask for it, and it is the only writing this script does.
#
# Source of truth: scripts/measure-hardware.sh in the aes67-srt repository, which
# is private. This file is a mirror for running on an appliance that cannot clone
# it, and the repository copy wins if they ever differ.
#
# Why a script rather than a set of instructions: every value here decides a design
# question, and a value typed by hand into an email is a value nobody can check. It
# reports what it could not measure as clearly as what it could, because a missing
# measurement is the useful half.
#
# Deliberately NOT `set -e`: a missing tool should be reported, not abort the run.
set -uo pipefail

COMMISSION=false
SUBSCRIBE_TO=auto
SEEN_SUBSCRIBE=false
for argument in "$@"; do
  if [ "${SEEN_SUBSCRIBE}" = "true" ]; then
    SUBSCRIBE_TO="${argument}"
    SEEN_SUBSCRIBE=false
    continue
  fi
  case "${argument}" in
    --commission) COMMISSION=true ;;
    --subscribe) SEEN_SUBSCRIBE=true ;;
    *)
      echo "unknown argument: ${argument} (only --commission and --subscribe <name> are accepted)"
      exit 2
      ;;
  esac
done
if [ "${SEEN_SUBSCRIBE}" = "true" ]; then
  echo '--subscribe needs a value: an announcement name, or "self", or "auto"'
  exit 2
fi

REPORT="hardware-report-$(hostname -s 2>/dev/null || echo host)-$(date +%Y%m%d-%H%M).txt"
exec > >(tee "${REPORT}") 2>&1

section() { printf '\n========== %s ==========\n' "$1"; }
note() { printf '    %s\n' "$1"; }
have() { command -v "$1" >/dev/null 2>&1; }

# Runs something and shows its output, labelled. Failures are reported rather
# than swallowed: a command that did not run is a fact about the machine.
try() {
  local label="$1"
  shift
  printf '\n--- %s\n' "${label}"
  if ! "$@" 2>&1 | sed 's/^/    /'; then
    note "^^ that command failed (exit ${PIPESTATUS[0]})"
  fi
}

printf 'aes67-srt hardware report\n'
printf 'generated %s on %s\n' "$(date -Is)" "$(hostname -f 2>/dev/null || hostname)"

# ---------------------------------------------------------------------------
section "1. The machine (clock recovery, and everything's CPU budget)"
# ---------------------------------------------------------------------------
try "uname" uname -a
try "model" sh -c 'cat /proc/device-tree/model 2>/dev/null | tr -d "\\0"; echo'
if [ -f /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor ]; then
  try "cpu governor" cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
fi
if have nproc; then
  try "cpu count" nproc
else
  # nproc is GNU coreutils; macOS does not have it, and a report that says
  # "command not found" where a core count belongs is worse than one that adapts.
  try "cpu count" getconf _NPROCESSORS_ONLN
fi

# The CPU sections below are only comparable if the cores are running flat out.
# This script will not change system configuration, so it warns instead.
GOVERNOR_FILE=/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
if [ -f "${GOVERNOR_FILE}" ] && [ "$(cat "${GOVERNOR_FILE}")" != "performance" ]; then
  printf '\n--- WARNING: cpu governor is %s, not performance\n' "$(cat "${GOVERNOR_FILE}")"
  note "Sections 6, 7 and 8 measure CPU, and under a power-saving governor the"
  note "numbers come out lower and vary between runs. For figures worth comparing:"
  note "  echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor"
  note "and put it back afterwards with 'ondemand'."
fi

# ---------------------------------------------------------------------------
section "2. Library versions (which libsrt, which libopus)"
# ---------------------------------------------------------------------------
# The version matters more than it sounds: libsrt 1.5.3 and 1.5.7 both pass our
# tests, but that is only known because both were tried, and libopus's own
# "latest release" API points at a two-year-old tag.
if have pkg-config; then
  for library in srt opus samplerate; do
    printf '\n--- pkg-config %s\n' "${library}"
    if pkg-config --exists "${library}"; then
      printf '    version %s\n' "$(pkg-config --modversion "${library}")"
    else
      note "not found via pkg-config"
    fi
  done
fi

printf '\n--- libsrt header macros\n'
for header in /usr/include/srt/version.h /usr/local/include/srt/version.h \
              /opt/homebrew/include/srt/version.h; do
  if [ -f "${header}" ]; then
    note "${header}"
    grep -E 'define SRT_VERSION_(MAJOR|MINOR|PATCH)|define SRT_VERSION_STR' \
      "${header}" | sed 's/^/        /'
  fi
done

printf '\n--- what is actually installed (in case pkg-config is not the whole story)\n'
if have dpkg; then
  dpkg -l 2>/dev/null | awk '/^ii/ && /srt|opus|samplerate|alsa/ {printf "    %s %s\n", $2, $3}'
fi
if have ldconfig; then
  ldconfig -p 2>/dev/null | grep -E 'libsrt|libopus|libsamplerate' | sed 's/^/    /'
fi

# ---------------------------------------------------------------------------
section "3. Our own build, if it is here (ticket 07's loopback on real hardware)"
# ---------------------------------------------------------------------------
if [ -x build/tests/aes67-srt-tests ]; then
  try "unit and loopback tests" ./build/tests/aes67-srt-tests
else
  note "no build/tests/aes67-srt-tests - build first: cmake -S . -B build && cmake --build build"
fi

# ---------------------------------------------------------------------------
section "4. ALSA and the RAVENNA device (ticket 03, and the audio module)"
# ---------------------------------------------------------------------------
try "playback devices" aplay -l
try "capture devices" arecord -l

printf '\n--- the RAVENNA device, opened at the shape we need\n'
if have arecord; then
  # Opening it at 64 channels of S24_3LE at 48 kHz is the question the audio
  # module needs answered: does the device take exactly that, and what does it
  # say if it does not? A refusal here is a real finding, not a failure.
  try "arecord --dump-hw-params (64ch S24_3LE 48k)" \
    arecord -D plughw:RAVENNA --dump-hw-params -f S24_3LE -r 48000 -c 64 -d 1 /dev/null
else
  note "arecord not installed"
fi

# Say what a missing card *means*. The difference between "the daemon is not
# installed" and "the hardware is broken" is the whole value of this section, and
# a raw ALSA error states neither.
if ! arecord -l 2>/dev/null | grep -qi ravenna; then
  printf '\n--- no RAVENNA card\n'
  note "Not an ALSA fault. It means the Merging RAVENNA kernel module and"
  note "aes67-daemon are not installed or not loaded, so sections 4 and 5 cannot be"
  note "measured until they are. Expected on a bare Pi; worth knowing before the"
  note "measurement is scheduled rather than during it."
fi

# Whether 64 channels actually *stream*, which the earlier session did not settle:
# it opened the device and started recording for one second, which proves the shape
# is accepted and nothing about continuity. Ten seconds is long enough for a stalled
# engine, a starved clock or an xrun to show up, and arecord reports overruns on
# stderr where they land in the report.
if have arecord && arecord -l 2>/dev/null | grep -qi ravenna; then
  printf '\n--- ten seconds of 64-channel capture (continuity, not just opening)\n'
  note "Watch for: a non-zero exit, 'overrun' or 'underrun' in the output, and how"
  note "long the run actually took. A clean ten seconds at 64ch is the first half of"
  note "the audio module's proof; the second half is our own binary below."
  started=$(date +%s)
  if arecord -D plughw:RAVENNA -f S24_3LE -r 48000 -c 64 -d 10 /dev/null 2>&1 |
    sed 's/^/    /'; then
    note "capture completed"
  else
    note "^^ capture did NOT complete - that is the finding, not a script error"
  fi
  note "wall clock: $(( $(date +%s) - started )) s for 10 s of audio (more means it stalled)"

  # The device's own xrun counters, when the kernel exposes them. These are the
  # numbers nobody argues with.
  for status in /proc/asound/card*/pcm*c/sub*/status; do
    [ -r "${status}" ] || continue
    printf '\n--- %s\n' "${status}"
    sed 's/^/    /' "${status}"
  done
fi

# ---------------------------------------------------------------------------
section "5. aes67-daemon (ticket 03: the black box's real surface)"
# ---------------------------------------------------------------------------
if have curl; then
  for endpoint in /api/config /api/ptp/status /api/sinks /api/sources; do
    printf '\n--- GET %s\n' "${endpoint}"
    curl -fsS --max-time 5 "http://127.0.0.1:8080${endpoint}" 2>&1 | head -c 2000 | sed 's/^/    /'
    printf '\n'
  done
  printf '\n--- GET /api/browse/sources/all\n'
  curl -fsS --max-time 5 "http://127.0.0.1:8080/api/browse/sources/all" 2>&1 |
    head -c 2000 | sed 's/^/    /'
  printf '\n'
else
  note "curl not installed"
fi

printf '\n--- daemon binary\n'
try "which" sh -c 'command -v aes67-daemon || echo "aes67-daemon not on PATH"'

# Which daemon, and which driver commit. A version is the difference between a
# finding someone can reproduce and a story about a machine.
printf '\n--- daemon and driver versions\n'
if have aes67-daemon; then
  try "aes67-daemon --version" aes67-daemon --version
fi
if [ -d /opt/aes67-linux-daemon/.git ]; then
  # `-c safe.directory` rather than `git config --global`: these repositories are
  # root-owned, so git refuses them as "dubious ownership", and this script
  # promises to change no configuration on the machine. A one-shot override says
  # what it needs without leaving anything behind.
  try "daemon source commit" \
    git -c safe.directory='*' -C /opt/aes67-linux-daemon rev-parse --short HEAD
fi
if [ -d /usr/src/ravenna-alsa-lkm/.git ]; then
  try "RAVENNA driver commit" \
    git -c safe.directory='*' -C /usr/src/ravenna-alsa-lkm rev-parse --short HEAD
fi
if have dkms; then
  try "dkms status" dkms status
fi

if ! curl -fsS --max-time 2 http://127.0.0.1:8080/api/config >/dev/null 2>&1; then
  printf '\n--- the daemon is not answering\n'
  note "Nothing is listening on 127.0.0.1:8080, so there is no surface to measure."
  note "Worth checking 'systemctl status aes67-daemon' and 'command -v aes67-daemon'."
  note "The sibling project aes67-sip installs the RAVENNA module and the daemon if"
  note "a known-good provisioning path would help."
fi

# ---------------------------------------------------------------------------
section "6. Encryption CPU at the link rate (ticket 07: is a passphrase affordable?)"
# ---------------------------------------------------------------------------
# SRT encrypts with AES-CTR (docs/research/libsrt.md). 148 Mbit/s is 18.6 MB/s, so
# the number to compare against is MB/s of cipher throughput: 200 MB/s means the
# cipher costs ~9% of one core, 20 MB/s means it is the whole appliance.
if have openssl; then
  try "openssl speed aes-128-ctr" openssl speed -evp aes-128-ctr -seconds 3
  printf '\n'
  note "148 Mbit/s = 18.6 MB/s. Divide that by the MB/s above for the core fraction."
else
  note "openssl not installed"
fi

# ---------------------------------------------------------------------------
section "7. Opus on this machine (ticket 04: does 64 channels fit?)"
# ---------------------------------------------------------------------------
# Two answers: the encoder's own lookahead, which settles 4 ms against the 6.5 ms
# usually quoted, and the CPU cost of 64 channels at 128 kbit/s each.
#
# The model is the real one: eight 8-channel blocks, each its own multistream
# encoder with eight MONO streams - the correctness-first mapping in
# docs/research/opus.md, because our channels are arbitrary console channels
# rather than stereo pairs.
# Name exactly what is missing rather than "cc or libopus". The first run of this
# on a real Pi could not say which, and the remedy is different for each.
opus_missing=""
have cc || opus_missing="${opus_missing} a C compiler (apt install build-essential)"
have pkg-config || opus_missing="${opus_missing} pkg-config"
if [ -z "${opus_missing}" ] && ! pkg-config --exists opus; then
  opus_missing=" libopus development files (apt install libopus-dev)"
fi
if [ -z "${opus_missing}" ]; then
  workdir="$(mktemp -d)"
  cat > "${workdir}/opus_probe.c" <<'PROBE'
#include <opus_multistream.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define BLOCKS 8
#define BLOCK_CHANNELS 8
#define CHANNELS (BLOCKS * BLOCK_CHANNELS)
#define FRAME 960               /* 20 ms at 48 kHz */
#define FRAMES 500              /* ten seconds of audio */
#define BITRATE_PER_CHANNEL 128000

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

int main(void) {
  OpusMSEncoder *encoders[BLOCKS];
  OpusMSDecoder *decoders[BLOCKS];
  unsigned char mapping[BLOCK_CHANNELS];
  int error = OPUS_OK;
  int b;

  for (b = 0; b < BLOCK_CHANNELS; ++b) mapping[b] = (unsigned char)b;
  for (b = 0; b < BLOCKS; ++b) {
    encoders[b] = opus_multistream_encoder_create(
        48000, BLOCK_CHANNELS, BLOCK_CHANNELS, 0, mapping,
        OPUS_APPLICATION_AUDIO, &error);
    if (encoders[b] == NULL || error != OPUS_OK) {
      fprintf(stderr, "encoder %d: %s\n", b, opus_strerror(error));
      return 1;
    }
    opus_multistream_encoder_ctl(
        encoders[b], OPUS_SET_BITRATE(BITRATE_PER_CHANNEL * BLOCK_CHANNELS));
    decoders[b] = opus_multistream_decoder_create(
        48000, BLOCK_CHANNELS, BLOCK_CHANNELS, 0, mapping, &error);
    if (decoders[b] == NULL || error != OPUS_OK) {
      fprintf(stderr, "decoder %d: %s\n", b, opus_strerror(error));
      return 1;
    }
  }

  {
    opus_int32 lookahead = 0;
    opus_multistream_encoder_ctl(encoders[0], OPUS_GET_LOOKAHEAD(&lookahead));
    printf("lookahead        : %d samples = %.2f ms at 48 kHz\n",
           (int)lookahead, (double)lookahead / 48.0);
  }
  printf("frame            : %d samples = %.0f ms, %d channels in %d blocks\n",
         FRAME, (double)FRAME / 48.0, CHANNELS, BLOCKS);
  printf("target bitrate   : %d bit/s per channel\n", BITRATE_PER_CHANNEL);

  {
    float *input = malloc(sizeof(float) * FRAME * BLOCK_CHANNELS);
    float *output = malloc(sizeof(float) * FRAME * BLOCK_CHANNELS);
    unsigned char packet[4000];
    int last_bytes = 0;
    long long payload_bytes = 0;
    double start, encode_ms, decode_ms;
    const double audio_ms = (double)FRAMES * FRAME / 48.0;
    int f;

    if (input == NULL || output == NULL) {
      fprintf(stderr, "out of memory\n");
      return 1;
    }
    for (f = 0; f < FRAME * BLOCK_CHANNELS; ++f) {
      input[f] = 0.01f * (float)((f % 97) - 48);
    }

    /* Three runs, best kept. A CPU measurement on a shared machine measures the
       machine's mood as much as the codec: two runs of this probe on the same
       laptop gave 39% and 15% of a core for identical work, a 2.6x swing. Since
       this number decides whether phase 2 fits on a Pi at all, a single sample
       would not be evidence. The minimum is the fairest figure - it is the run
       least interfered with. The first run also warms the caches. */
    double best_encode = 1e18;
    double best_decode = 1e18;
    int run;
    for (run = 0; run < 3; ++run) {
      start = now_ms();
      for (f = 0; f < FRAMES; ++f) {
        for (b = 0; b < BLOCKS; ++b) {
          int bytes = opus_multistream_encode_float(
              encoders[b], input, FRAME, packet, sizeof(packet));
          if (bytes < 0) {
            fprintf(stderr, "encode: %s\n", opus_strerror(bytes));
            return 1;
          }
          last_bytes = bytes;
          payload_bytes += bytes;
        }
      }
      encode_ms = now_ms() - start;
      if (encode_ms < best_encode) best_encode = encode_ms;

    /* The same packet repeatedly: a cost measurement rather than a correctness
       one, so the payload length is simply the last one encoded. */
    start = now_ms();
    for (f = 0; f < FRAMES; ++f) {
      for (b = 0; b < BLOCKS; ++b) {
        int samples = opus_multistream_decode_float(decoders[b], packet,
                                                   last_bytes, output, FRAME, 0);
        if (samples < 0) {
          fprintf(stderr, "decode: %s\n", opus_strerror(samples));
          return 1;
        }
      }
    }
      decode_ms = now_ms() - start;
      if (decode_ms < best_decode) best_decode = decode_ms;
    }
    /* Best of three, for the reasons in the comment above. */
    encode_ms = best_encode;
    decode_ms = best_decode;

    printf("audio measured   : %.1f s of %d channels\n", audio_ms / 1000.0,
           CHANNELS);
    printf("encode           : %.0f ms = %.1fx realtime, %.2f%% of one core "
           "(%.3f%% per channel)  [best of 3]\n",
           encode_ms, audio_ms / encode_ms, 100.0 * encode_ms / audio_ms,
           100.0 * encode_ms / audio_ms / CHANNELS);
    printf("decode           : %.0f ms = %.1fx realtime, %.2f%% of one core  "
           "[best of 3]\n",
           decode_ms, audio_ms / decode_ms, 100.0 * decode_ms / audio_ms);
    printf("achieved bitrate : %.2f Mbit/s total (%.0f bit/s per channel)\n",
           (double)payload_bytes * 8.0 / (3.0 * audio_ms / 1000.0) / 1e6,
           (double)payload_bytes * 8.0 / (3.0 * audio_ms / 1000.0) / CHANNELS);
    free(input);
    free(output);
  }
  return 0;
}
PROBE
  printf '\n--- compiling the Opus probe\n'
  if cc -O2 -o "${workdir}/opus_probe" "${workdir}/opus_probe.c" \
       $(pkg-config --cflags --libs opus) 2> "${workdir}/cc.log"; then
    try "running the Opus probe" "${workdir}/opus_probe"
  else
    note "could not compile the probe; the compiler said:"
    sed 's/^/    /' "${workdir}/cc.log"
  fi
  rm -rf "${workdir}"
else
  note "cannot take this measurement - missing:${opus_missing}"
fi

# ---------------------------------------------------------------------------
section "8. Resampling CPU (ticket 03: the alternative to slipping samples)"
# ---------------------------------------------------------------------------
# The clock module must reconcile two clock domains. One candidate is continuous
# asynchronous sample rate conversion, and its cost is the number that decides
# between the approaches. libsamplerate is one implementation and SRC_SINC_FASTEST
# its cheapest sinc, so read this as a fair-shape measurement rather than a
# universal answer: a heavier converter costs more, a lighter one less.
asrc_missing=""
have cc || asrc_missing="${asrc_missing} a C compiler (apt install build-essential)"
have pkg-config || asrc_missing="${asrc_missing} pkg-config"
if [ -z "${asrc_missing}" ] && ! pkg-config --exists samplerate; then
  asrc_missing=" libsamplerate development files (apt install libsamplerate0-dev)"
fi
if [ -z "${asrc_missing}" ]; then
  workdir="$(mktemp -d)"
  cat > "${workdir}/asrc_probe.c" <<'PROBE'
#include <samplerate.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHANNELS 64
#define FRAME 480               /* 10 ms at 48 kHz */
#define FRAMES 1000             /* ten seconds of audio */

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

int main(void) {
  int error = 0;
  int f;
  float *input = malloc(sizeof(float) * FRAME * CHANNELS);
  float *output = malloc(sizeof(float) * (FRAME + 64) * CHANNELS);
  SRC_STATE *state = src_new(SRC_SINC_FASTEST, CHANNELS, &error);
  const double audio_ms = (double)FRAMES * FRAME / 48.0;
  double start, elapsed_ms;
  SRC_DATA data;

  if (state == NULL) {
    fprintf(stderr, "src_new: %s\n", src_strerror(error));
    return 1;
  }
  if (input == NULL || output == NULL) {
    fprintf(stderr, "out of memory\n");
    return 1;
  }
  for (f = 0; f < FRAME * CHANNELS; ++f) {
    input[f] = 0.01f * (float)((f % 97) - 48);
  }

  memset(&data, 0, sizeof(data));
  data.data_in = input;
  data.data_out = output;
  data.input_frames = FRAME;
  data.output_frames = FRAME + 64;
  /* 10 ppm: the receiver's clock runs slightly fast, so it has to consume
     slightly more samples than arrive. This is the correction the clock module
     would make continuously, and the reason the ratio is not exactly 1. */
  data.src_ratio = 1.0 + 10e-6;
  data.end_of_input = 0;

  start = now_ms();
  for (f = 0; f < FRAMES; ++f) {
    if (src_process(state, &data) != 0) {
      fprintf(stderr, "src_process: %s\n", src_strerror(error));
      return 1;
    }
  }
  elapsed_ms = now_ms() - start;

  printf("converter        : libsamplerate SRC_SINC_FASTEST, %d channels\n",
         CHANNELS);
  printf("ratio            : 1 + 10 ppm\n");
  printf("audio measured   : %.1f s of %d channels\n", audio_ms / 1000.0,
         CHANNELS);
  printf("resample         : %.0f ms = %.1fx realtime, %.2f%% of one core "
         "(%.3f%% per channel)\n",
         elapsed_ms, audio_ms / elapsed_ms, 100.0 * elapsed_ms / audio_ms,
         100.0 * elapsed_ms / audio_ms / CHANNELS);

  src_delete(state);
  free(input);
  free(output);
  return 0;
}
PROBE
  printf '\n--- compiling the resampler probe\n'
  if cc -O2 -o "${workdir}/asrc_probe" "${workdir}/asrc_probe.c" \
       $(pkg-config --cflags --libs samplerate) -lm 2> "${workdir}/cc.log"; then
    try "running the resampler probe" "${workdir}/asrc_probe"
  else
    note "could not compile the probe; the compiler said:"
    sed 's/^/    /' "${workdir}/cc.log"
  fi
  rm -rf "${workdir}"
else
  note "cannot take this measurement - missing:${asrc_missing}"
  note "this is the clock module's deciding number, so it is worth the install"
fi

# ---------------------------------------------------------------------------
section "9. The whole path on one box (ticket 08: our binary, the daemon, the device)"
# ---------------------------------------------------------------------------
# The only section that writes to the daemon: it asks our binary to publish one
# AES67 source per block and subscribe a sink to each, so the daemon carries this
# box's audio back to itself. What comes out is the answer to the question the
# project has carried since ticket 03 — does the daemon accept the documents we
# build, and does it report packets arriving — and it is the criterion of ticket 08
# that no runner can reach.
if [ ! -x build/aes67-srt ]; then
  note "no build/aes67-srt. Build it first:"
  note "  cmake -S . -B build && cmake --build build --parallel"
  note "Without a clone there is no binary and this section cannot run at all, which"
  note "is why it says so rather than measuring something else instead."
elif ! curl -fsS --max-time 3 http://127.0.0.1:8080/api/config >/dev/null 2>&1; then
  note "the daemon is not answering on 127.0.0.1:8080, so there is nothing to wire up"
elif [ "${COMMISSION}" != "true" ]; then
  note "not run. This section writes to the daemon, so it is off unless you ask:"
  note "  bash $0 --commission                    # publish, and subscribe to an"
  note "                                          # eight-channel sender it finds"
  note "  bash $0 --commission --subscribe NAME   # a specific announcement"
  note "  bash $0 --commission --subscribe self   # transmit to ourselves"
  note "It publishes one source per block and subscribes at most one sink."
else
  printf '\n--- the configuration, before anything is wired\n'
  # A commissioning configuration rather than the production one: the daemon and the
  # RAVENNA device are real, because the AES67 half is what is being tested, but the
  # *link* is a loopback. The production config points at a WAN peer, so with no peer
  # present the appliance would fail to open the link seconds after commissioning had
  # already succeeded — and a non-zero exit there reads as a commissioning failure
  # when it is nothing of the kind. The AES67 wiring does not involve the link at all.
  try "validate" ./build/aes67-srt -c config/aes67-srt.commissioning.conf --validate

  # The settings that decide whether any of this can work. `streamer_enabled` is
  # the one nobody has measured: provisioning sets it false because it would
  # capture the RAVENNA device, and whether a source handed over REST needs it true
  # is unknown. A zero in "receiving" below, with everything else healthy, points
  # at exactly that and nothing else.
  if have curl; then
    printf '\n--- daemon settings this depends on\n'
    note "interface_name must not be lo, and auto_sinks_update should be false:"
    note "the first never sees PTP or RTP, the second can retarget a sink we wired."
    note "streamer_enabled is the open question (see docs/research/aes67-daemon-64ch.md)."
    curl -fsS --max-time 5 http://127.0.0.1:8080/api/config 2>/dev/null |
      grep -oE '"(interface_name|streamer_enabled|auto_sinks_update|tic_frame_size_at_1fs|rtp_mcast_base|rtp_port|ptp_domain)"[^,}]*' |
      sed 's/^/    /'
  fi

  printf '\n--- our binary, commissioning for 15 seconds\n'
  note "subscribe target: ${SUBSCRIBE_TO} (\"self\" means our own source, \"auto\" means"
  note "the first eight-channel L24 sender discovered - the binary names its choice)"
  appliance_log="$(mktemp)"
  ./build/aes67-srt -c config/aes67-srt.commissioning.conf \
    --subscribe "${SUBSCRIBE_TO}" >"${appliance_log}" 2>&1 &
  appliance=$!
  sleep 15
  # SIGTERM, because that is what systemd sends and the appliance is built to stop
  # cleanly on it. A non-zero exit means commissioning failed and said why.
  kill -TERM "${appliance}" 2>/dev/null
  wait "${appliance}"
  appliance_status=$?
  sed 's/^/    /' "${appliance_log}"
  note "exit ${appliance_status} (commissioning failure exits non-zero on purpose)"
  rm -f "${appliance_log}"

  printf '\n--- what the daemon holds now\n'
  for endpoint in /api/sinks /api/sources; do
    printf '\n--- GET %s\n' "${endpoint}"
    curl -fsS --max-time 5 "http://127.0.0.1:8080${endpoint}" 2>&1 |
      head -c 8000 | sed 's/^/    /'
    printf '\n'
  done

  # The per-sink flags, one by one: which block is silent is the whole question,
  # and a count at the end averages that away.
  printf '\n--- per-sink reception (receiving_rtp_packet is the answer that matters)\n'
  for id in 0 1 2 3 4 5 6 7; do
    printf '    sink %s: ' "${id}"
    curl -fsS --max-time 5 "http://127.0.0.1:8080/api/sink/status/${id}" 2>&1 |
      head -c 400
    printf '\n'
  done
  note "a 400 or 404 here means the daemon holds no stream for that sink, which is"
  note "normal for a block that was never wired - not a failure of anything."
fi

# ---------------------------------------------------------------------------
section "10. What each measurement above answers"
# ---------------------------------------------------------------------------
cat <<'SUMMARY'
    1  The machine        cpu budget everything else is measured against
    2  Library versions   which libsrt and which libopus are really installed
    3  Our build          the loopback tests passing on real hardware
    4  ALSA / RAVENNA     does the device take 64ch S24_3LE at 48 kHz
                          (the audio module, ticket 08)
    5  aes67-daemon       the black box's real surface and its PTP state
                          (the daemon questions, ticket 03)
    6  Encryption CPU     whether a passphrase link is affordable at 148 Mbit/s
                          (ticket 07)
    7  Opus               the encoder's lookahead, and whether 64 channels fit
                          at 128 kbit/s each (ticket 04)
    8  Resampling CPU     the clock module's deciding number (ticket 03)
    9  The whole path     whether the daemon accepts the documents we build, and
                          whether its sinks receive our own sources
                          (ticket 08's hardware criterion; needs --commission)

    Send the whole report file back. A measurement that could not be taken is as
    useful as one that could: it says what is missing from the machine.
SUMMARY

printf '\nreport written to %s\n' "$(pwd)/${REPORT}"
printf 'send the whole file back, including the sections that failed\n'

