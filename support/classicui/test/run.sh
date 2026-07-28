#!/bin/bash
# Builds and runs the Classic Home host harness in Docker.
# The five chome_*.cpp files are compiled unmodified; only the platform is faked.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
OUTDIR="${1:-/tmp/chome_out}"

docker run --rm -v "$REPO":/mister -w /mister ubuntu:20.04 bash -c '
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null
apt-get install -qq -y g++ libimlib2-dev >/dev/null

mkdir -p /tmp/harness
g++ -std=gnu++14 -O1 -g -Wall -Wextra -Wno-unused-parameter \
    -I. -o /tmp/harness/chome_test \
    support/classicui/chome_gfx.cpp \
    support/classicui/chome_theme.cpp \
    support/classicui/chome_lib.cpp \
    support/classicui/chome_art.cpp \
    support/classicui/chome_ui.cpp \
    support/classicui/chome_video.cpp \
    support/classicui/test/stubs.cpp \
    support/classicui/test/harness.cpp \
    charrom.cpp \
    -lImlib2

echo "--- running ---"
/tmp/harness/chome_test
rc=$?

mkdir -p /mister/support/classicui/test/out
cp /tmp/chome_out/*.png /mister/support/classicui/test/out/ 2>/dev/null || true
exit $rc
'
echo "PNGs copied to $REPO/support/classicui/test/out/"
