#!/bin/bash
# Handheld display looks, checked as numbers: the firmware's own generated files
# (via the harness's CHOME_GEN_LOOKS mode) run through the bit-exact scaler model,
# then tools/look_metrics.py. Exits non-zero when a check fails, so a change to a
# filter, a mask or the preset table that breaks a look shows up here and not on a
# television.
#
# Separate from run.sh on purpose: the suite there stays pure C++ in one container,
# and this needs python3 on the host besides. The test frames are the committed ones
# in test/frames and the GBA captures in docs/img, so the result does not depend on
# whatever tools/corpus holds that day. Pass --render to also write the 1080p
# renders and review crops to test/out/looks (render_looks.py).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOLS="$HERE/../tools"

CHOME_GEN_LOOKS=1 bash "$HERE/run.sh" >/dev/null
echo "generated files: $HERE/out/looks-gen"

if [ "${1:-}" = "--render" ]; then
    python3 "$TOOLS/render_looks.py" --out "$HERE/out/looks" --corpus "$HERE/frames" \
        --looks-dir "$HERE/out/looks-gen"
else
    python3 "$TOOLS/look_metrics.py" --corpus "$HERE/frames" --looks-dir "$HERE/out/looks-gen"
fi
