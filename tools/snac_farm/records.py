"""Read and write the farm's TSV records.

The build log written by build_via_windows*.sh is the only durable record of
what was compiled from where, so it is the state this layer builds on rather
than a new format of its own. Its five columns are treated as fixed:

    core  repo  upstream_sha  upstream_date  result

Extra columns may be appended (see UPSTREAM_FIELDS and the extended manifest
written by build_via_windows_snac.sh); anything reading $1..$5 with awk keeps
working. Rows are append-only, so several rows may exist for one core; the
last row wins, except that a row with '-' placeholders never overrides a row
that actually recorded a commit.
"""

import csv
import os
import tempfile

BUILD_FIELDS = ["core", "repo", "upstream_sha", "upstream_date", "result"]

UPSTREAM_FIELDS = [
    "core",             # our core name, matching the build log
    "repo",             # MiSTer-devel/<repo>
    "release_rbf",      # basename upstream ships, e.g. ActFancer or Arcade-Gauntlet
    "release_datecode", # newest YYYYMMDD in releases/
    "release_count",    # how many dated .rbf upstream keeps
    "head_sha",
    "head_date",
    "scanned_at",
    "note",
]

# Results that mean "the compile itself was attempted and failed", as opposed
# to the harness giving up. A core that fails to compile will fail again from
# the same source, so it must not be retried on every sweep.
COMPILE_FAILURES = {"BUILD_FAIL", "NO_QPF", "NO_QSF", "NO_RBF", "PATCH_FAIL"}
# Results that mean "we never got a verdict" - safe and correct to retry.
HARNESS_FAILURES = {"overran", "stalled", "CLONE_FAIL", "COPYIN_FAIL", "COPYOUT_FAIL"}

UNKNOWN = "-"


def _clean(v):
    v = (v or "").strip()
    return "" if v == UNKNOWN else v


def read_tsv(path, fields=None):
    if not os.path.exists(path):
        return []
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f, delimiter="\t"))
    if fields:
        for r in rows:
            for k in fields:
                r.setdefault(k, "")
    return rows


def write_tsv(path, fields, rows):
    """Atomic write - a scan interrupted halfway must not truncate the state."""
    d = os.path.dirname(os.path.abspath(path)) or "."
    os.makedirs(d, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=d, prefix=".tsv-")
    try:
        with os.fdopen(fd, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=fields, delimiter="\t",
                               extrasaction="ignore")
            w.writeheader()
            for r in rows:
                w.writerow(r)
        os.replace(tmp, path)
    except BaseException:
        if os.path.exists(tmp):
            os.unlink(tmp)
        raise


def load_builds(paths):
    """Collapse one or more append-only build logs to one row per core."""
    best = {}
    for p in paths:
        for row in read_tsv(p, BUILD_FIELDS):
            core = (row.get("core") or "").strip()
            if not core or core == "core":
                continue
            rec = {
                "core": core,
                "repo": _clean(row.get("repo")),
                "upstream_sha": _clean(row.get("upstream_sha")),
                "upstream_date": _clean(row.get("upstream_date")),
                "result": (row.get("result") or "").strip(),
                "upstream_release": _clean(row.get("upstream_release")),
                "source": p,
            }
            prev = best.get(core)
            # A backfilled row with no commit must not erase real provenance.
            if prev and prev["upstream_sha"] and not rec["upstream_sha"]:
                prev["result"] = rec["result"] or prev["result"]
                continue
            best[core] = rec
    return best
