#!/usr/bin/env bash
#
# Collects, on the appliance itself, the hardware measurements this project is
# blocked on. Writes one report file to send back.
#
#   bash scripts/measure-hardware.sh              # writes hardware-report-<host>-<date>.txt
#
# Why a script rather than a set of instructions: every one of these values
# decides a design question, and a value typed by hand into an email is a value
# nobody can check. It reports what it could not measure as clearly as what it
# could, because a missing measurement is the useful half.
#
# Deliberately NOT `set -e`: a missing tool should be reported, not abort the run.
set -uo pipefail

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
try "cpu count" nproc

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

printf '\n--- libopus header\n'
for header in /usr/include/opus/opus.h /usr/local/include/opus/opus.h \
              /opt/homebrew/include/opus/opus.h; do
  if [ -f "${header}" ]; then
    note "${header} present"
  fi
done

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
if have cc && have pkg-config && pkg-config --exists opus; then
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

    start = now_ms();
    for (f = 0; f < FRAMES; ++f) {
      for (b = 0; b < BLOCKS; ++b) {
        int bytes = opus_multistream_encode_float(encoders[b], input, FRAME,
                                                  packet, sizeof(packet));
        if (bytes < 0) {
          fprintf(stderr, "encode: %s\n", opus_strerror(bytes));
          return 1;
        }
        last_bytes = bytes;
        payload_bytes += bytes;
      }
    }
    encode_ms = now_ms() - start;

