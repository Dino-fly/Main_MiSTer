#!/usr/bin/env python3
"""Decide which cores upstream has overtaken and must be rebuilt.

The failure this exists to prevent has nothing to do with source control: a
core is chosen by the datecode in its filename, so the moment upstream commits
a release newer than the datecode we shipped, the firmware picks upstream's
build and SNAC support disappears with no error anywhere. That makes the
primary test a comparison of two datecodes, not of two commits:

    upstream release datecode >= the datecode we shipped   ->  SHADOWED

Greater-or-equal, not greater: an equal datecode produces an identical
filename, so our file overwrites upstream's rather than sitting beside it, and
where both survive the tie-break in bootcore.cpp and mra_loader.cpp is
undefined. See naming.py for the full derivation.

A second, weaker signal is that master moved since we compiled
(SOURCE_MOVED). An unreleased upstream commit cannot shadow anything, so it is
reported but does not force a rebuild unless --include-moved is given.

Cores whose last recorded result was a compile failure are held back as
KNOWN_FAIL: the same source fails the same way, and the farm must not burn
someone else's PC retrying thirteen hopeless cores every sweep. Failures where
the harness never reached a verdict (overran, stalled, transfer errors) are
RETRYABLE.

    stale.py                      full report
    stale.py --list               repo names to feed the farm, one per line
    stale.py --list --include-moved
"""

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import naming
import records
from shipped_scan import SHIPPED_FIELDS

DEFAULT_FARM = "/Users/mister/workspace/snac-build"
DEFAULT_WORK = "/Users/mister/workspace/snac-refresh"

BUILD_LOGS = ("builds.tsv", "builds-win.tsv", "builds-rebuild.tsv")
# Written by build_via_windows_snac.sh in the work directory. It carries the
# out_rbf column, so a core rebuilt by this pipeline updates its own datecode
# and stops being reported stale without waiting for an archive to be assembled.
SNAC_LOG = "builds-snac.tsv"
WAVE1_MANIFEST = "MANIFEST_wave1.txt"
REPO_INDEX = "allrepos.txt"
ORG = "MiSTer-devel"

REPORT_FIELDS = ["core", "repo", "status", "shipped_datecode",
                 "upstream_release_datecode", "upstream_release_rbf",
                 "built_sha", "upstream_head_sha", "next_out_rbf", "detail"]

# MISNAMED is a live failure, not a future one: the file is on the card and
# unreachable today, so it goes in the rebuild list alongside the shadowed ones.
REBUILD = ("SHADOWED", "MISNAMED", "RETRYABLE")


def key(name):
    """Join key across the three disagreeing name spaces.

    The repository is Arcade-ATetris_MiSTer, the farm calls the core
    Arcade-ATetris, and the file we shipped is ATetris_20260731.rbf, so the key
    has to ignore the prefix, the suffix and the case. The suffix matters: three
    rows of MANIFEST_wave1.txt carry the repository name in the core column
    (Arcade-Arkanoid_MISTer, Arcade-Deco16_Mister, Arcade-TimePilot84_MISTer),
    and without stripping it they become phantom cores with no upstream.
    """
    n = re.sub(r"^arcade[-_]", "", (name or "").strip(), flags=re.I)
    n = re.sub(r"_MiSTer$", "", n, flags=re.I)
    return n.lower()


def load_wave1(path):
    """core -> repo from the wave-1 manifest.

    For the 141 cores built before builds.tsv recorded anything, this is the
    only surviving record of which repository they came from. Its SHAs are
    master HEAD at manifest time, not proof of what was compiled, so they are
    marked approximate.
    """
    out = {}
    if not os.path.exists(path):
        return out
    with open(path) as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            parts = line.split()
            if len(parts) < 3 or "/" not in parts[2]:
                continue
            core, sha, repo = parts[0], parts[1], parts[2]
            out[key(core)] = {
                "repo": repo,
                "sha": sha if re.fullmatch(r"[0-9a-f]{40}", sha) else "",
            }
    return out


def name_reachable(rec):
    """Can any .mra actually name the file we shipped?

    mra_loader.cpp get_rbf() accepts a candidate starting with the .mra's <rbf>
    value or with "Arcade-" + that value. We do not have the .mra here, but
    upstream's own released file must satisfy it, so <rbf> is upstream's
    basename either as-is or with the Arcade- prefix removed. If our basename is
    neither, nothing can reach our file. Checked for arcade cores only: outside
    _Arcade the user picks the file in the browser and the name is cosmetic.
    """
    if not rec["folder"].startswith("_Arcade"):
        return True
    theirs, ours_file = rec["upstream_release_rbf"], rec["shipped_rbf"]
    if not theirs or not ours_file:
        return True
    parsed = naming.parse_release_rbf(ours_file)
    ours = parsed[0] if parsed else ours_file[:-4]
    stripped = re.sub(r"^arcade[-_]", "", theirs, flags=re.I)
    return ours.lower() in (theirs.lower(), stripped.lower())


def load_repo_index(path):
    """key -> repo, from the cached org listing.

    Last resort for cores that predate both records: ten shipped arcade cores
    from the end of the alphabet appear in no manifest, and their repository is
    only recoverable by name. The suffix is not fixed - the org contains
    _MiSTer, _MISTer and _Mister - so the key ignores it.
    """
    out = {}
    if not os.path.exists(path):
        return out
    with open(path) as f:
        for line in f:
            name = line.strip()
            if not name or name.startswith("#"):
                continue
            out.setdefault(key(re.sub(r"_MiSTer$", "", name, flags=re.I)),
                           f"{ORG}/{name}")
    return out


def classify(rec):
    """Order matters, and holding a shipped artifact outranks the build log.

    builds-rebuild.tsv is not a newer attempt at the same thing - it was a
    separate wave that recompiled already-shipped arcade cores purely to record
    their provenance, and it was abandoned part-way. Eight of its rows say
    'overran'. Letting the newest log row decide would mark eight cores that
    are on the card and working as needing a rebuild, so a core we shipped is
    judged on its datecode and only an unshipped core is judged on its last
    result.
    """
    ship = rec["shipped_datecode"]
    rel = rec["upstream_release_datecode"]
    result = rec["result"]

    if not rec["repo"]:
        return "NO_PROVENANCE", "no upstream repo recorded for this core"
    if not rec["scanned"]:
        return "UNKNOWN_UPSTREAM", "upstream not scanned yet"
    if rec["upstream_note"]:
        return "UNKNOWN_UPSTREAM", rec["upstream_note"]

    if not ship:
        if result in records.COMPILE_FAILURES:
            return "KNOWN_FAIL", f"last result {result}; needs a fix, not a retry"
        if result in records.HARNESS_FAILURES:
            return "RETRYABLE", f"last result {result}; no verdict was reached"
        return "NOT_SHIPPED", "built but not present in any shipped archive"

    if not rel:
        return "NO_UPSTREAM_RELEASE", "upstream ships no dated .rbf to compare"
    if not name_reachable(rec):
        return "MISNAMED", (f"we ship {rec['shipped_rbf']} but upstream ships "
                            f"{rec['upstream_release_rbf']}_*.rbf; no .mra can "
                            f"name ours, so upstream's loads whatever the date")
    if rel >= ship:
        how = "ties" if rel == ship else "beats"
        return "SHADOWED", f"upstream {rel} {how} our {ship}"
    if rec["built_sha"] and rec["upstream_head_sha"] \
            and rec["built_sha"] != rec["upstream_head_sha"]:
        extra = " (our recorded sha is approximate)" if rec["sha_approx"] else ""
        return "SOURCE_MOVED", f"master moved since we built{extra}"
    return "UP_TO_DATE", f"our {ship} still beats upstream {rel}"


def next_out_rbf(rec, today=None):
    base = rec["upstream_release_rbf"]
    if not base and rec["shipped_rbf"]:
        parsed = naming.parse_release_rbf(rec["shipped_rbf"])
        base = parsed[0] if parsed else rec["shipped_rbf"][:-4]
    return naming.output_name(base or rec["core"],
                              rec["upstream_release_datecode"], today)


def build_report(farm, work):
    builds = records.load_builds([os.path.join(farm, n) for n in BUILD_LOGS])
    wave1 = load_wave1(os.path.join(farm, WAVE1_MANIFEST))
    index = load_repo_index(os.path.join(farm, REPO_INDEX))
    shipped = {key(r["core"]): r for r in
               records.read_tsv(os.path.join(work, "shipped.tsv"), SHIPPED_FIELDS)}
    # A core this pipeline has already rebuilt is held to its new filename, so
    # rerunning the loop after a build round reports it up to date instead of
    # queueing it again.
    for r in records.read_tsv(os.path.join(work, SNAC_LOG),
                              records.BUILD_FIELDS + ["out_rbf"]):
        out = (r.get("out_rbf") or "").strip()
        if r.get("result") != "OK" or not out or out == records.UNKNOWN:
            continue
        parsed = naming.parse_release_rbf(out)
        if not parsed:
            continue
        k = key(r["core"])
        prev = shipped.get(k, {})
        if prev.get("datecode", "") > parsed[1]:
            continue
        shipped[k] = {"core": parsed[0], "wave": "refresh",
                      "folder": prev.get("folder") or
                      naming.card_folder(parsed[0], r.get("repo", "")),
                      "out_rbf": out, "datecode": parsed[1]}
    upstream = {key(r["core"]): r for r in
                records.read_tsv(os.path.join(work, "upstream.tsv"),
                                 records.UPSTREAM_FIELDS)}
    by_key = {}
    for core, rec in builds.items():
        by_key[key(core)] = rec

    keys = (set(shipped) | set(by_key)) - {"", key("menu")}

    rows = []
    for k in sorted(keys):
        b = by_key.get(k)
        s = shipped.get(k)
        w1 = wave1.get(k)
        core = (b or {}).get("core") or (s or {}).get("core") or k

        repo = (b or {}).get("repo") or ""
        built_sha = (b or {}).get("upstream_sha") or ""
        sha_approx = False
        if not repo and w1:
            repo, built_sha, sha_approx = w1["repo"], w1["sha"], True
        elif not built_sha and w1:
            built_sha, sha_approx = w1["sha"], True
        if not repo:
            repo = index.get(k, "")

        u = upstream.get(k)
        if not u and repo:
            u = upstream.get(key(repo.split("/")[-1]))

        rec = {
            "core": core,
            "repo": repo,
            "result": (b or {}).get("result", ""),
            "shipped_datecode": (s or {}).get("datecode", ""),
            "shipped_rbf": (s or {}).get("out_rbf", ""),
            "folder": (s or {}).get("folder", ""),
            "built_sha": built_sha,
            "sha_approx": sha_approx,
            "scanned": bool(u),
            "upstream_note": (u or {}).get("note", ""),
            "upstream_release_rbf": (u or {}).get("release_rbf", ""),
            "upstream_release_datecode": (u or {}).get("release_datecode", ""),
            "upstream_head_sha": (u or {}).get("head_sha", ""),
        }
        rec["status"], rec["detail"] = classify(rec)
        rec["next_out_rbf"] = next_out_rbf(rec) \
            if rec["status"] in REBUILD + ("SOURCE_MOVED",) else ""
        rows.append(rec)
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--farm", default=DEFAULT_FARM,
                    help="wave-2 farm directory, read only")
    ap.add_argument("--work", default=DEFAULT_WORK)
    ap.add_argument("--list", action="store_true",
                    help="print only the repo names needing a rebuild")
    ap.add_argument("--include-moved", action="store_true",
                    help="also rebuild cores whose master moved without a release")
    ap.add_argument("--status", action="append", default=[],
                    help="detail only these statuses (repeatable)")
    ap.add_argument("--report", default=None)
    a = ap.parse_args()

    rows = build_report(a.farm, a.work)
    wanted = set(REBUILD) | ({"SOURCE_MOVED"} if a.include_moved else set())

    report_path = a.report or os.path.join(a.work, "stale-report.tsv")
    records.write_tsv(report_path, REPORT_FIELDS, rows)

    if a.list:
        for r in rows:
            if r["status"] in wanted and r["repo"]:
                print(r["repo"].split("/")[-1])
        return 0

    counts = {}
    for r in rows:
        counts[r["status"]] = counts.get(r["status"], 0) + 1
    print(f"{len(rows)} cores known")
    for st in sorted(counts, key=lambda s: (-counts[s], s)):
        print(f"  {st:22}{counts[st]:4}")
    print(f"\nrebuild list: {sum(counts.get(s, 0) for s in wanted)} cores "
          f"({', '.join(sorted(wanted))})")
    print(f"full table:   {report_path}\n")

    show = set(a.status) if a.status else wanted
    for r in rows:
        if r["status"] not in show:
            continue
        print(f"{r['status']:20} {r['core']:26} "
              f"ours={r['shipped_datecode'] or '-':8} "
              f"upstream={r['upstream_release_datecode'] or '-':8} "
              f"-> {r['next_out_rbf'] or '-':26} {r['detail']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
