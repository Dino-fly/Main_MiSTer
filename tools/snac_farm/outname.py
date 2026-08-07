#!/usr/bin/env python3
"""Name the .rbf a patched core tree will produce, for the build script.

The authoritative basename is the Quartus revision, because that is what
Quartus writes into output_files/ and upstream builds the same project file, so
upstream's released .rbf carries the same basename by construction. Deriving it
from the repository name instead is what produced three cores in wave 2 that no
.mra can name: repo Arcade-ATetris_MiSTer has revision Arcade-AtariTetris, repo
Arcade-RushnAttack_MiSTer has Arcade-RshnAtk, repo Arcade-GundamSD_MiSTer has
SDGundamPS.

The datecode comes from the upstream snapshot, so it is guaranteed to beat the
release it was built from rather than merely being today's date.

    outname.py <core-tree> [--work DIR] [--core NAME]
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import naming
import records
import stale


def revision(tree):
    """The .qpf the container script will pick: first non-Q13, sorted."""
    qpfs = sorted(f for f in os.listdir(tree)
                  if f.endswith(".qpf") and "Q13" not in f)
    return qpfs[0][:-4] if qpfs else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tree")
    ap.add_argument("--work", default=stale.DEFAULT_WORK)
    ap.add_argument("--core", default=None,
                    help="core name for the datecode lookup (default: the revision)")
    ap.add_argument("--print", dest="what", default="name",
                    choices=["name", "revision", "datecode"])
    a = ap.parse_args()

    rev = revision(a.tree)
    if not rev:
        print("no non-Q13 .qpf in " + a.tree, file=sys.stderr)
        return 4                      # same code the container uses for NO_QPF

    upstream = {stale.key(r["core"]): r for r in
                records.read_tsv(os.path.join(a.work, "upstream.tsv"),
                                 records.UPSTREAM_FIELDS)}
    row = upstream.get(stale.key(a.core or rev)) or {}
    dc = naming.build_datecode(row.get("release_datecode", ""))

    print({"name": f"{rev}_{dc}.rbf", "revision": rev, "datecode": dc}[a.what])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
