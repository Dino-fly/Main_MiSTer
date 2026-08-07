"""Thin wrapper over the `gh` CLI.

Everything the farm needs from GitHub is a plain GET, so shelling out to
`gh api` is enough and keeps credential handling out of this codebase. Calls
are retried because a scan makes several hundred of them in a row and one
transient 502 must not cost a whole pass.
"""

import json
import subprocess
import time


class GhError(Exception):
    pass


class NotFound(GhError):
    pass


def api(path, jq=None, paginate=False, attempts=4):
    cmd = ["gh", "api", path]
    if paginate:
        cmd.append("--paginate")
    if jq:
        cmd += ["--jq", jq]

    last = None
    for n in range(attempts):
        p = subprocess.run(cmd, capture_output=True, text=True)
        if p.returncode == 0:
            return p.stdout
        err = (p.stderr or "").strip()
        if "HTTP 404" in err or "Not Found" in err:
            raise NotFound(err.splitlines()[0] if err else "404")
        last = err
        # Secondary rate limits want a real pause, not a tight retry.
        time.sleep(2 * (n + 1))
    raise GhError(last or "unknown gh failure")


def api_json(path, paginate=False, attempts=4):
    out = api(path, paginate=paginate, attempts=attempts)
    if not paginate:
        return json.loads(out)

    # --paginate concatenates one JSON document per page, and each page of a
    # list endpoint is its own array, so splice them back together.
    merged = []
    dec = json.JSONDecoder()
    i = 0
    while i < len(out):
        while i < len(out) and out[i].isspace():
            i += 1
        if i >= len(out):
            break
        doc, i = dec.raw_decode(out, i)
        if not isinstance(doc, list):
            return doc
        merged.extend(doc)
    return merged


def dir_listing(repo, path=""):
    """Directory listing, or None when the path does not exist upstream."""
    p = f"repos/{repo}/contents/{path}" if path else f"repos/{repo}/contents"
    try:
        d = api_json(p)
    except NotFound:
        return None
    return d if isinstance(d, list) else None


def head_commit(repo):
    d = api_json(f"repos/{repo}/commits?per_page=1")
    if not d:
        return None, None
    return d[0]["sha"], d[0]["commit"]["committer"]["date"]
