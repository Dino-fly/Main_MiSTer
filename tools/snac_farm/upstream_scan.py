#!/usr/bin/env python3
"""Snapshot what each upstream core repository is currently shipping.

Every MiSTer core repository checked publishes the same way: no git tags, no
GitHub releases, just dated .rbf files committed under releases/. The datecode
in that filename is the thing that shadows our rebuild, so it is what gets
recorded here, alongside master's HEAD so we can also tell whether the source
moved at all.

The snapshot is a TSV that is rewritten atomically and re-read on the next run,
so a scan that dies at core 200 of 285 resumes instead of restarting, and
running it twice in a row does no work the second time. Two GitHub calls per
core; 285 cores is well inside the authenticated hourly budget.

    upstream_scan.py --repos <file>            one repo name per line
    upstream_scan.py --from-builds             every core in the build logs
    upstream_scan.py --core NES_MiSTer ...     named repos only
"""

import argparse
import concurrent.futures
import datetime
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gh
import naming
import records

DEFAULT_FARM = "/Users/derek/workspace/snac-build"
DEFAULT_WORK = "/Users/derek/workspace/snac-refresh"
ORG = "MiSTer-devel"


def core_name(repo):
    """The farm's core name for a repo, matching build_via_windows*.sh."""
    n = repo.split("/")[-1]
    for suffix in ("_MiSTer", "_MISTer", "_Mister"):
        if n.endswith(suffix):
            return n[: -len(suffix)]
    return n


def scan_repo(repo):
    full = repo if "/" in repo else f"{ORG}/{repo}"
    row = {f: "" for f in records.UPSTREAM_FIELDS}
    row["core"] = core_name(full)
    row["repo"] = full
    row["scanned_at"] = datetime.datetime.now(datetime.timezone.utc)\
        .strftime("%Y-%m-%dT%H:%M:%SZ")
    try:
        listing = gh.dir_listing(full, "releases")
        if listing is None:
            row["note"] = "no releases/ folder"
        else:
            base, date, count = naming.newest_release(
                [e["name"] for e in listing if e.get("type") == "file"])
            if base is None:
                row["note"] = "releases/ holds no dated .rbf"
            row["release_rbf"] = base or ""
            row["release_datecode"] = date or ""
            row["release_count"] = str(count)
        sha, when = gh.head_commit(full)
        row["head_sha"] = sha or ""
        row["head_date"] = when or ""
    except gh.GhError as e:
        row["note"] = f"scan failed: {e}".replace("\t", " ")[:200]
    return row


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--farm", default=DEFAULT_FARM,
                    help="wave-2 farm directory, read only")
    ap.add_argument("--work", default=DEFAULT_WORK,
                    help="writable directory for the snapshot")
    ap.add_argument("--state", default=None,
                    help="upstream snapshot TSV (default <work>/upstream.tsv)")
    ap.add_argument("--repos", help="file of repo names, one per line")
    ap.add_argument("--from-builds", action="store_true",
                    help="scan every core named in the build logs")
    ap.add_argument("--core", action="append", default=[],
                    help="scan this repo only (repeatable)")
    ap.add_argument("--max-age", type=float, default=6.0,
                    help="hours before a snapshot row is refreshed (0 = always)")
    ap.add_argument("--jobs", type=int, default=6)
    a = ap.parse_args()

    state_path = a.state or os.path.join(a.work, "upstream.tsv")

    wanted = []
    if a.repos:
        with open(a.repos) as f:
            wanted += [l.strip() for l in f if l.strip()
                       and not l.startswith("#")]
    wanted += a.core
    if a.from_builds:
        import stale
        builds = records.load_builds(
            [os.path.join(a.farm, n) for n in stale.BUILD_LOGS])
        # builds.tsv only names the repo for the 144 cores compiled after
        # provenance recording started; MANIFEST_wave1.txt is the only record of
        # the repo for the rest, so both are needed to cover the shipped set.
        wave1 = stale.load_wave1(os.path.join(a.farm, stale.WAVE1_MANIFEST))
        index = stale.load_repo_index(os.path.join(a.farm, stale.REPO_INDEX))
        missing = []
        for core, rec in sorted(builds.items()):
            k = stale.key(core)
            repo = rec["repo"] or (wave1.get(k) or {}).get("repo") or index.get(k)
            if repo:
                wanted.append(repo)
            else:
                missing.append(core)
        for repo in sorted(w["repo"] for w in wave1.values()):
            wanted.append(repo)
        if missing:
            print(f"{len(missing)} cores have no repo in any record: "
                  f"{', '.join(missing)}", file=sys.stderr)
    if not wanted:
        ap.error("nothing to scan: pass --repos, --from-builds or --core")

    seen, order = set(), []
    for r in wanted:
        full = r if "/" in r else f"{ORG}/{r}"
        if full not in seen:
            seen.add(full)
            order.append(full)

    have = {r["repo"]: r for r in records.read_tsv(state_path,
                                                  records.UPSTREAM_FIELDS)}
    cutoff = None
    if a.max_age > 0:
        cutoff = (datetime.datetime.now(datetime.timezone.utc)
                  - datetime.timedelta(hours=a.max_age))\
            .strftime("%Y-%m-%dT%H:%M:%SZ")

    todo = []
    for full in order:
        old = have.get(full)
        fresh = (old and cutoff and old.get("scanned_at", "") >= cutoff
                 and not old.get("note", "").startswith("scan failed"))
        if fresh:
            continue
        todo.append(full)

    print(f"{len(order)} repos requested, {len(order) - len(todo)} already fresh,"
          f" {len(todo)} to scan", file=sys.stderr)

    done = 0
    if todo:
        with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as ex:
            for row in ex.map(scan_repo, todo):
                have[row["repo"]] = row
                done += 1
                if done % 25 == 0 or done == len(todo):
                    # Checkpoint so an interrupted scan keeps its progress.
                    records.write_tsv(state_path, records.UPSTREAM_FIELDS,
                                      [have[k] for k in sorted(have)])
                    print(f"  {done}/{len(todo)}", file=sys.stderr)

    records.write_tsv(state_path, records.UPSTREAM_FIELDS,
                      [have[k] for k in sorted(have)])
    bad = [r for r in have.values() if r.get("note")]
    print(f"snapshot: {state_path}  rows={len(have)}  with-notes={len(bad)}",
          file=sys.stderr)
    for r in bad[:10]:
        print(f"  {r['repo']}: {r['note']}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
