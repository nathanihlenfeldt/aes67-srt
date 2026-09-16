#!/usr/bin/env bash
#
# Runs everything CI runs, locally: the formatting gate, the build, the unit
# tests, the config validation, and the refusal behaviour that the specification
# treats as a feature rather than an error path.  Run this before committing.
#
# Usage:  ./scripts/check.sh
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${REPO_ROOT}"

BUILD_DIR="${BUILD_DIR:-build}"
# .clang-format targets clang-format 18, which is what ubuntu-24.04 ships and
# what CI enforces.  A newer clang-format can break lines differently, so prefer
# the pinned version when it is present and fall back to whatever is installed
# rather than skipping the gate.  On macOS:
#
#   python3 -m pip install --user 'clang-format==18.1.8'
#
if [ -z "${CLANG_FORMAT:-}" ]; then
  if command -v clang-format-18 >/dev/null 2>&1; then
    CLANG_FORMAT="clang-format-18"
  else
    CLANG_FORMAT="clang-format"
  fi
fi

echo "==> source files"
SOURCES="$(find src tests -name '*.hpp' -o -name '*.cpp' | sort)"
echo "${SOURCES}" | wc -l | tr -d ' '
echo " files"

echo "==> clang-format"
if ! command -v "${CLANG_FORMAT}" >/dev/null 2>&1; then
  echo "    ${CLANG_FORMAT} not found: skipping (CI checks it on Linux)"
else
  "${CLANG_FORMAT}" --version
  # shellcheck disable=SC2086
  if ! "${CLANG_FORMAT}" --dry-run --Werror ${SOURCES}; then
    echo "    run '${CLANG_FORMAT} -i \$(find src tests -name '*.hpp' -o -name '*.cpp')'"
    exit 1
  fi
fi

echo "==> build (${BUILD_DIR})"
cmake -S . -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null
cmake --build "${BUILD_DIR}" --parallel

echo "==> tests"
ctest --test-dir "${BUILD_DIR}" --output-on-failure

# Run the suite directly as well. ctest hides the output of every test that
# passes, and this is where the conditional tests announce themselves ("no libsrt
# in this build: skipping ..."). A CI log that cannot show whether a test ran is
# not evidence that it did — and the difference between "the loopback passed" and
# "the loopback never ran" is the whole of ticket 07's proof.
echo "==> tests, directly, for the summary and any skips"
"${BUILD_DIR}/tests/aes67-srt-tests" | tail -6

echo "==> config validation accepts the sample configurations"
"${BUILD_DIR}/aes67-srt" -c config/aes67-srt.conf --validate
"${BUILD_DIR}/aes67-srt" -c config/aes67-srt.dev.conf --validate

echo "==> config validation refuses an impossible configuration"
BAD_CONFIG="$(mktemp)"
trap 'rm -f "${BAD_CONFIG}"' EXIT
# A typo in a key name: accepted by JSON, refused by us, and the reason must
# name the key.  This is the behaviour the appliance is built around.
python3 - <<'PY' > "${BAD_CONFIG}"
import json
with open("config/aes67-srt.dev.conf") as handle:
    document = json.load(handle)
document["link"]["latency"] = 120
print(json.dumps(document))
PY
if "${BUILD_DIR}/aes67-srt" -c "${BAD_CONFIG}" --validate > /tmp/aes67-srt-bad.log 2>&1; then
  echo "    FAILED: an unknown key was accepted"
  cat /tmp/aes67-srt-bad.log
  exit 1
fi
if ! grep -q 'unknown key "link.latency"' /tmp/aes67-srt-bad.log; then
  echo "    FAILED: the refusal did not name the offending key"
  cat /tmp/aes67-srt-bad.log
  exit 1
fi
echo "    refused, and named the key"

echo "==> shell scripts"
for script in scripts/*.sh; do
  bash -n "${script}"
done
echo "    syntax OK"

echo
echo "all checks passed"
