"""Resolve the naming fallback for cores the distribution does not install.

db.json is the authority for what lands on a card, but it does not list every
core that exists - two arcade cores we ship, AsteroidsDeluxe and LunarLander,
have .mra files in the distribution and no .rbf, and eleven console/computer
cores are installed by upstream under a different name than the repository uses.
For those thirteen there is no layer 3 to copy, so the rule falls back to the
repository's newest released .rbf basename with any "Arcade-" prefix stripped,
which is exactly the transformation the distribution applies when it does ship a
core.

This matters most for the two arcade cores. They are sole candidates today, so
any case resolves, but if upstream ever ships them the way it ships freeze -
lowercase - our capitalised name would lose the strcmp permanently. Recording
the repository's own capitalisation now is what makes that check possible later.

Usage: layer2_names.py <repo> [<repo> ...]   (needs gh)
"""

import json
import subprocess
import sys


def releases(repo):
    try:
        out = subprocess.run(
            ["gh", "api", f"repos/MiSTer-devel/{repo}/contents/releases",
             "--jq", ".[].name"],
            capture_output=True, text=True, timeout=60, check=True).stdout
    except subprocess.CalledProcessError as e:
        return None, e.stderr.strip()[:80]
    return [l for l in out.splitlines() if l.endswith(".rbf")], None


def main():
    for repo in sys.argv[1:]:
        rbfs, err = releases(repo)
        if rbfs is None:
            print(json.dumps({"repo": repo, "error": err}))
            continue
        newest = max(rbfs) if rbfs else None
        base = None
        if newest:
            base = newest.rsplit("_", 1)[0]
            if base.lower().startswith("arcade-"):
                base = base[7:]
        print(json.dumps({"repo": repo, "layer2_newest": newest,
                          "layer3_would_be": base, "count": len(rbfs)}))


if __name__ == "__main__":
    main()
