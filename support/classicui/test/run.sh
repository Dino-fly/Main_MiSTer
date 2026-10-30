#!/bin/bash
# Builds and runs the Classic Home host harness in Docker.
# The chome_*.cpp files are compiled unmodified; only the platform is faked.
#
# The container comes from the prebuilt image in harness-image.sh (gcc, g++, imlib2
# already installed), the sources compile in parallel, and the container is named so
# a run stopped from outside is removed rather than left running.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
OUTDIR="${1:-/tmp/chome_out}"
. "$REPO/support/classicui/test/harness-image.sh"
ensure_harness_image
harness_container "chome-harness-$$"

docker run --rm --name "$HARNESS_CONTAINER" -e CHOME_GEN_LOOKS="${CHOME_GEN_LOOKS:-}" -v "$REPO":/mister -w /mister "$HARNESS_IMAGE" bash -c '
set -euo pipefail

mkdir -p /tmp/harness

# sxmlc is C and does not compile as C++ - it assigns void* and drops const, which
# the firmware build gets away with by compiling it with the C compiler. The
# gamelist reader parses with it rather than with a hand-rolled parser, so the
# harness compiles it the same way and links the object.
gcc -std=gnu99 -O1 -g -I. -c sxmlc.c -o /tmp/harness/sxmlc.o

# The ScreenScraper module is compiled with a dummy devid defined.
#
# In every shipped build CLASSICUI_SS_DEVID is undefined, which makes ss_available()
# compile-time false and ss_build_url() refuse - so the URL builder would be dead
# code that no test could reach. Defining a fake pair here is what makes it
# testable, and the value being obviously fake is the point: a test that needed a
# real credential would be a test nobody but Dinofly could run.
# -I./lib/libchdr/include because the disc launch path includes physical_disc.h, which
# reaches cd.h, which includes libchdr/chd.h.
#
# Each translation unit is compiled on its own, in parallel (xargs -P), with exactly
# the flags one g++ invocation would have given it; only the link is serial.
CXXFLAGS="-std=gnu++14 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation \
    -DCHOME_HOST_TEST \
    -DCLASSICUI_SS_DEVID=\"testdev\" \
    -DCLASSICUI_SS_DEVPASS=\"testpass\" \
    -DCLASSICUI_SS_SOFTNAME=\"classichome-test\" \
    -I. -I./lib/libchdr/include"
SOURCES="support/classicui/chome_gfx.cpp
support/classicui/chome_theme.cpp
support/classicui/chome_lib.cpp
support/classicui/chome_art.cpp
support/classicui/chome_gamelist.cpp
support/classicui/chome_ss.cpp
support/classicui/chome_disc.cpp
support/classicui/chome_rip.cpp
support/classicui/chome_proc.cpp
support/classicui/chome_titles.cpp
support/classicui/chome_ui.cpp
support/classicui/chome_osk.cpp
support/classicui/chome_net.cpp
support/classicui/chome_bt.cpp
support/classicui/chome_ini.cpp
support/classicui/chome_cfgrec.cpp
support/classicui/chome_cheats.cpp
support/classicui/chome_opt.cpp
support/classicui/chome_core.cpp
support/classicui/chome_video.cpp
snacpad.cpp
support/classicui/test/stubs.cpp
support/classicui/test/harness.cpp
charrom.cpp
lib/miniz/miniz.c"
export CXXFLAGS
# one .o per source, named after the source path with / and . turned into _
echo "$SOURCES" | xargs -P "$(nproc)" -I{} sh -c "g++ \$CXXFLAGS -c {} -o /tmp/harness/\$(echo {} | tr /. __).o"
g++ -o /tmp/harness/chome_test /tmp/harness/*.o -lImlib2

# CHOME_GEN_LOOKS=1 bash run.sh: do not run the suite, write the handheld looks generated
# files (every scale, plus a manifest) to test/out/looks-gen for tools/render_looks.py.
# The harness has a mode for it; see gen_looks() in harness.cpp. No apostrophes here.
if [ -n "${CHOME_GEN_LOOKS:-}" ]; then
    rm -rf /mister/support/classicui/test/out/looks-gen
    mkdir -p /mister/support/classicui/test/out
    CHOME_GEN_LOOKS=/mister/support/classicui/test/out/looks-gen /tmp/harness/chome_test
    exit $?
fi

# The same chome_ss.cpp compiled the way it actually ships - no devid - to prove that
# configuration cannot reach the network. See support/classicui/test/gate.cpp.
#
# -DCLASSICUI_SS_NO_CREDS says so out loud, because chome_ss.cpp will otherwise include
# bin/ss_credentials.h when real credentials exist on this machine - and then this binary
# would be testing that local build rather than the one we ship, and would fail exactly
# when its reassurance matters most.
#
# No apostrophes in here. This whole script is one single-quoted argument to bash -c, so
# one apostrophe ends the quote and the rest of it runs on the host instead of in the
# container - which shows up as clang++ complaining that it cannot find these files.
g++ -std=gnu++14 -O1 -g -Wall -Wextra -DCLASSICUI_SS_NO_CREDS -I. -o /tmp/harness/chome_gate \
    support/classicui/chome_ss.cpp \
    support/classicui/test/gate.cpp \
    /tmp/harness/sxmlc.o

echo "--- screenscraper gate ---"
/tmp/harness/chome_gate

echo "--- running ---"
/tmp/harness/chome_test
rc=$?

mkdir -p /mister/support/classicui/test/out
cp /tmp/chome_out/*.png /mister/support/classicui/test/out/ 2>/dev/null || true
exit $rc
'
echo "PNGs copied to $REPO/support/classicui/test/out/"
