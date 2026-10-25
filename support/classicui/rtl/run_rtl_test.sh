#!/bin/bash
# Prove the substrate stays inside the picture, without needing the hardware.
#
# Applies shadowmask-substrate.patch to a pristine sys/shadowmask.sv, runs the
# testbench against it, then runs the SAME testbench against a copy with the border
# gate removed. The second run must FAIL: a test that cannot catch the bug it exists
# for is worth nothing, and this one had two vacuous-pass bugs of its own before it
# worked (an unwritten LUT and an uninitialised row counter, both of which read as X
# and were counted as "lit").
#
# Needs iverilog and a Template_MiSTer checkout for the pristine file.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${1:-/Users/derek/workspace/Template_MiSTer/sys/shadowmask.sv}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$WORK/sys"
cp "$SRC" "$WORK/sys/shadowmask.sv"
patch -p1 -s -d "$WORK" < "$HERE/shadowmask-substrate.patch"

run() {   # run <dir> ; echoes the verdict line
	( cd "$1" && iverilog -g2012 -o sim.out shadowmask.sv tb.sv && vvp sim.out ) \
		| grep -E "^(PASS|FAIL|lit )"
}

mkdir -p "$WORK/gated" "$WORK/ungated"
cp "$WORK/sys/shadowmask.sv" "$WORK/gated/shadowmask.sv"
sed 's/(sub4 && !brd_d\[3\])/sub4/' "$WORK/sys/shadowmask.sv" > "$WORK/ungated/shadowmask.sv"
cp "$HERE/shadowmask_border_tb.sv" "$WORK/gated/tb.sv"
cp "$HERE/shadowmask_border_tb.sv" "$WORK/ungated/tb.sv"

echo "== with the border gate (the shipping patch) =="
G=$(run "$WORK/gated");   echo "$G"
echo "== with the gate removed (the control - must fail) =="
U=$(run "$WORK/ungated"); echo "$U"

echo
if echo "$G" | grep -q '^PASS' && echo "$U" | grep -q '^FAIL'; then
	echo "RTL TEST OK - the gate fixes the leak and the test can see the leak"
	exit 0
fi
echo "RTL TEST FAILED"
exit 1
