#!/bin/bash
# Runs the Classic Home UI on this machine, against a fake SD card, and serves it
# to a browser. The five chome_*.cpp files are the same ones that build for the
# DE10-Nano; only the platform is faked.
#
#   support/classicui/test/play.sh          1280x720 (HD profile)
#   support/classicui/test/play.sh --sd     640x480
#   support/classicui/test/play.sh --lo     320x240
#
# Then open http://localhost:8080
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
ARGS="${*:-}"

echo "Building and starting... first run pulls the toolchain image."

docker run --rm -it -p 8080:8080 -v "$REPO":/mister -w /mister ubuntu:20.04 bash -c '
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null
apt-get install -qq -y g++ libimlib2-dev >/dev/null

mkdir -p /tmp/harness
g++ -std=gnu++14 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation \
    -I. -o /tmp/harness/chome_play \
    support/classicui/chome_gfx.cpp \
    support/classicui/chome_theme.cpp \
    support/classicui/chome_lib.cpp \
    support/classicui/chome_art.cpp \
    support/classicui/chome_ui.cpp \
    support/classicui/chome_video.cpp \
    support/classicui/test/stubs.cpp \
    support/classicui/test/viewer.cpp \
    charrom.cpp \
    -lImlib2

exec /tmp/harness/chome_play '"$ARGS"'
'
