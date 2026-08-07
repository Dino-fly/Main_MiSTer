#!/usr/bin/env python3
"""Record the datecode of every core we actually shipped.

This is the number upstream has to beat, and it is not in any of the farm's
records: builds.tsv stores the upstream commit but not the filename the core
was installed under, and the datecode was applied later while assembling
SD-CARD-ROOT by a step that was never scripted. So for wave 2 it is recovered
from the release archive's file listing - listing only, the archive is 322MB
and is never extracted.

From now on build_via_windows_snac.sh writes the same columns as it builds, so
this script is a one-off backfill and not part of the refresh loop.

    shipped_scan.py --zip .../snac-psx-pad-wave2.zip --wave wave2
    shipped_scan.py --dir  .../SD-CARD-ROOT          --wave wave2
"""

import argparse
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import naming
import records

SHIPPED_FIELDS = ["core", "wave", "folder", "out_rbf", "datecode"]
DEFAULT_WORK = "/Users/mister/workspace/snac-refresh"


def rows_from_paths(paths, wave):
    out = {}
    for p in paths:
        p = p.strip().replace("\\", "/")
        if not p.lower().endswith(".rbf"):
            continue
        name = os.path.basename(p)
        folder = os.path.dirname(p)
        # Drop the archive's own top-level directory, keep the card-relative part.
        parts = [x for x in folder.split("/") if x]
        if parts and parts[0].upper().startswith("SD-CARD-ROOT"):
            parts = parts[1:]
        folder = "/".join(parts)
        parsed = naming.parse_release_rbf(name)
        if parsed:
            base, date = parsed
        else:
            base, date = name[:-4], ""
        out[base] = {"core": base, "wave": wave, "folder": folder,
                     "out_rbf": name, "datecode": date}
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--zip")
    ap.add_argument("--dir")
    ap.add_argument("--wave", default="wave2")
    ap.add_argument("--work", default=DEFAULT_WORK)
    ap.add_argument("--state", default=None)
    a = ap.parse_args()
    if not a.zip and not a.dir:
        ap.error("pass --zip or --dir")

    paths = []
    if a.zip:
        p = subprocess.run(["unzip", "-Z1", a.zip], capture_output=True, text=True)
        if p.returncode != 0:
            print(p.stderr.strip(), file=sys.stderr)
            return 1
        paths += p.stdout.splitlines()
    if a.dir:
        for root, _, files in os.walk(a.dir):
            rel = os.path.relpath(root, os.path.dirname(a.dir.rstrip("/")))
            paths += [os.path.join(rel, f) for f in files]

    state_path = a.state or os.path.join(a.work, "shipped.tsv")
    have = {r["core"]: r for r in records.read_tsv(state_path, SHIPPED_FIELDS)}
    new = rows_from_paths(paths, a.wave)
    have.update(new)
    records.write_tsv(state_path, SHIPPED_FIELDS, [have[k] for k in sorted(have)])

    dated = sum(1 for r in new.values() if r["datecode"])
    print(f"{state_path}: {len(new)} entries this pass ({dated} datecoded), "
          f"{len(have)} total", file=sys.stderr)
    for r in sorted(new.values(), key=lambda x: x["core"]):
        if not r["datecode"]:
            print(f"  no datecode: {r['folder']}/{r['out_rbf']}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
