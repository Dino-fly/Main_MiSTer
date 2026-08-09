#!/usr/bin/env python3
"""Name the .rbf we install, for the build script.

The authoritative basename is the basename of upstream's newest released .rbf,
copied verbatim. Deriving it from the repository name is what produced three
cores in wave 2 that no .mra can name (repo Arcade-ATetris_MiSTer releases
Arcade-AtariTetris, Arcade-RushnAttack_MiSTer releases Arcade-RshnAtk,
Arcade-GundamSD_MiSTer releases SDGundamPS).

Deriving it from the Quartus revision is *also* wrong, which this file
previously did on the theory that upstream compiles the same project file so
the released name matches by construction. Measured against the org, it does
not: upstream renames the artifact after compiling it, and 5 of the 11 cores in
the 2026-08-07 rebuild set disagree.

    repo                        revision              upstream releases
    Arcadia_MiSTer              arcadia               Arcadia
    Arcade-DECOCassette_MiSTer  Arcade-DECOCassette   DECOCassette
    Arcade-Qix_MiSTer           Arcade-Qix            Qix
    Arcade-Salamander_MiSTer    Salamander            Arcade-Salamander
    Arcade-SNK6502_MiSTer       Arcade-SNK6502        SNK6502

Three of those are not cosmetic. mra_loader.cpp get_rbf() keeps the
lexicographic maximum over the whole filename, so where our prefix sorts below
upstream's the datecode never gets consulted: "Arcade-Qix_20260807.rbf" loses
to upstream's "Qix_20260804.rbf" because 'A' < 'Q', and the same for
DECOCassette and SNK6502. Shipping the revision name would have reproduced the
exact bug this rebuild exists to fix. Arcadia is a console core, where
bootcore.cpp matchesCore_yyyyMMdd_rbf() does a case-SENSITIVE strncmp, so
"arcadia_*.rbf" is not reachable as "Arcadia" either.

The revision is still needed - it is what Quartus writes into output_files/ -
but only as a fallback for a core upstream has never released a dated .rbf for,
where there is nothing to copy and nothing to lose a sort against.

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


DUAL_SUFFIXES = ("_dualsdram", "_dualsdr", "_ds")


def _norm(name):
    return "".join(c for c in name.lower() if c.isalnum())


def revision(tree, core=None):
    """The .qpf the container script will pick.

    This has to agree with win_container_build.sh exactly. It used to say "first
    non-Q13, sorted", which was true and was the bug: sorting picked
    PSX_DualSDRAM.qpf over PSX.qpf inside the container's UTF-8 locale, and picked
    Atari5200.qpf over Atari800.qpf under every locale. Five released cores were
    built from the wrong project, two of them a different machine entirely.

    Same rules as the shell now: never a dual-SDRAM revision, then the one whose
    name matches the core, and only fall back to the single remaining candidate
    when there is nothing to choose between. Returns None where the shell would
    exit AMBIGUOUS_QPF, so the caller stops rather than naming a guess.
    """
    qpfs = sorted(f for f in os.listdir(tree)
                  if f.endswith(".qpf") and "Q13" not in f)

    cands = [f for f in qpfs
             if not any(f[:-4].lower().endswith(sfx) for sfx in DUAL_SUFFIXES)]
    if not cands:
        cands = qpfs
    if not cands:
        return None

    if core:
        want = _norm(core)
        for f in cands:
            if _norm(f[:-4]) == want:
                return f[:-4]

    return cands[0][:-4] if len(cands) == 1 else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tree")
    ap.add_argument("--work", default=stale.DEFAULT_WORK)
    ap.add_argument("--core", default=None,
                    help="core name for the datecode lookup (default: the revision)")
    ap.add_argument("--print", dest="what", default="name",
                    choices=["name", "revision", "datecode", "basename"])
    a = ap.parse_args()

    rev = revision(a.tree, a.core)
    if not rev:
        print("no non-Q13 .qpf in " + a.tree, file=sys.stderr)
        return 4                      # same code the container uses for NO_QPF

    upstream = {stale.key(r["core"]): r for r in
                records.read_tsv(os.path.join(a.work, "upstream.tsv"),
                                 records.UPSTREAM_FIELDS)}
    row = upstream.get(stale.key(a.core or rev)) or {}
    dc = naming.build_datecode(row.get("release_datecode", ""))

    # Upstream's released basename wins; the revision is only the fallback for a
    # core with no dated release to copy from. Announced on stderr when they
    # differ, because that difference silently broke three cores in wave 2 and
    # the build log is the only place anyone would notice it.
    base = (row.get("release_rbf") or "").strip()
    if base and base != rev:
        print(f"basename from upstream release: {base!r} "
              f"(quartus revision is {rev!r})", file=sys.stderr)
    if not base:
        base = rev
        print(f"no upstream dated release; falling back to quartus revision "
              f"{rev!r}", file=sys.stderr)

    print({"name": f"{base}_{dc}.rbf", "revision": rev,
           "basename": base, "datecode": dc}[a.what])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
