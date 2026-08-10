#!/bin/bash
# Runs the Classic Home UI on this machine, against a fake SD card of placeholder
# games, and serves it to a browser. The five chome_*.cpp files are the same ones
# that build for the DE10-Nano; only the platform is faked.
#
#   support/classicui/test/play.sh              1280x720 (HD profile)
#   support/classicui/test/play.sh --sd         640x480
#   support/classicui/test/play.sh --lo         320x240
#   support/classicui/test/play.sh --no-fetch   do not download cover art
#   PORT=9000 support/classicui/test/play.sh    if 8099 is taken
#
# Then open the URL it prints.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
PORT="${PORT:-8099}"
ARGS="${*:-}"

echo "Building... the first run pulls the toolchain image."
echo "  Once it is up:  http://localhost:$PORT"
echo

# -t only when there is a terminal, so this works when piped too.
TTY=""
[ -t 0 ] && TTY="-t"

docker run --rm -i $TTY -p "$PORT":8099 -v "$REPO":/mister -w /mister ubuntu:20.04 bash -c '
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null
# curl is what the art fetcher forks: without it covers stay on the fallback card.
apt-get install -qq -y g++ libimlib2-dev curl ca-certificates >/dev/null

mkdir -p /tmp/harness
g++ -std=gnu++14 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation \
    -I. -o /tmp/harness/chome_play \
    support/classicui/chome_gfx.cpp \
    support/classicui/chome_theme.cpp \
    support/classicui/chome_lib.cpp \
    support/classicui/chome_art.cpp \
    support/classicui/chome_bt.cpp \
    support/classicui/chome_proc.cpp \
    support/classicui/chome_ini.cpp \
    support/classicui/chome_opt.cpp \
    support/classicui/chome_ui.cpp \
    support/classicui/chome_video.cpp \
    support/classicui/test/stubs.cpp \
    support/classicui/test/viewer.cpp \
    charrom.cpp \
    lib/miniz/miniz.c \
    -lImlib2

exec /tmp/harness/chome_play '"$ARGS"'
'
