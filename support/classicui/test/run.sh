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
apt-get install -qq -y gcc g++ libimlib2-dev >/dev/null

mkdir -p /tmp/harness

# sxmlc is C and does not compile as C++ - it assigns void* and drops const, which
# the firmware build gets away with by compiling it with the C compiler. The
# gamelist reader parses with it rather than with a hand-rolled parser, so the
# harness compiles it the same way and links the object.
gcc -std=gnu99 -O1 -g -I. -c sxmlc.c -o /tmp/harness/sxmlc.o

g++ -std=gnu++14 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation \
    -I. -o /tmp/harness/chome_test \
    support/classicui/chome_gfx.cpp \
    support/classicui/chome_theme.cpp \
    support/classicui/chome_lib.cpp \
    support/classicui/chome_art.cpp \
    support/classicui/chome_gamelist.cpp \
    support/classicui/chome_ui.cpp \
    support/classicui/chome_osk.cpp \
    support/classicui/chome_net.cpp \
    support/classicui/chome_bt.cpp \
    support/classicui/chome_ini.cpp \
    support/classicui/chome_opt.cpp \
    support/classicui/chome_core.cpp \
    support/classicui/chome_video.cpp \
    support/classicui/test/stubs.cpp \
    support/classicui/test/harness.cpp \
    charrom.cpp \
    lib/miniz/miniz.c \
    /tmp/harness/sxmlc.o \
    -lImlib2

echo "--- running ---"
/tmp/harness/chome_test
rc=$?

mkdir -p /mister/support/classicui/test/out
cp /tmp/chome_out/*.png /mister/support/classicui/test/out/ 2>/dev/null || true
exit $rc
'
echo "PNGs copied to $REPO/support/classicui/test/out/"
