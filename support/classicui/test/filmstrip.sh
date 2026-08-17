#!/bin/bash
# Builds and runs the riffle filmstrip (filmstrip.cpp) in Docker; PNG frames land
# in the directory given as $1 (default /tmp/chome_strip_out). The file list is
# run.sh's with filmstrip.cpp in place of harness.cpp - same modules, no suite.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
OUTDIR="${1:-/tmp/chome_strip_out}"
mkdir -p "$OUTDIR"

docker run --rm -v "$REPO":/mister -v "$OUTDIR":/out -w /mister ubuntu:20.04 bash -c '
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null
apt-get install -qq -y gcc g++ libimlib2-dev >/dev/null

mkdir -p /tmp/harness
gcc -std=gnu99 -O1 -g -I. -c sxmlc.c -o /tmp/harness/sxmlc.o

g++ -std=gnu++14 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation \
    -DCHOME_HOST_TEST \
    -DCLASSICUI_SS_DEVID=\"testdev\" \
    -DCLASSICUI_SS_DEVPASS=\"testpass\" \
    -DCLASSICUI_SS_SOFTNAME=\"classichome-test\" \
    -I. -I./lib/libchdr/include -o /tmp/harness/chome_strip \
    support/classicui/chome_gfx.cpp \
    support/classicui/chome_theme.cpp \
    support/classicui/chome_lib.cpp \
    support/classicui/chome_art.cpp \
    support/classicui/chome_gamelist.cpp \
    support/classicui/chome_ss.cpp \
    support/classicui/chome_disc.cpp \
    support/classicui/chome_rip.cpp \
    support/classicui/chome_proc.cpp \
    support/classicui/chome_titles.cpp \
    support/classicui/chome_ui.cpp \
    support/classicui/chome_osk.cpp \
    support/classicui/chome_net.cpp \
    support/classicui/chome_bt.cpp \
    support/classicui/chome_ini.cpp \
    support/classicui/chome_cfgrec.cpp \
    support/classicui/chome_cheats.cpp \
    support/classicui/chome_opt.cpp \
    support/classicui/chome_core.cpp \
    support/classicui/chome_video.cpp \
    snacpad.cpp \
    support/classicui/test/stubs.cpp \
    support/classicui/test/filmstrip.cpp \
    charrom.cpp \
    lib/miniz/miniz.c \
    /tmp/harness/sxmlc.o \
    -lImlib2

/tmp/harness/chome_strip
'
