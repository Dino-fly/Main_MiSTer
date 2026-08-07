"""Pull the authoritative file list and every .mra out of Distribution_MiSTer.

The wave-2 archive shipped five arcade cores that no .mra could ever name, and
the audit that missed them worked from a guessed .mra <rbf> value instead of a
real one. So nothing here infers: db.json gives the exact set of filenames the
distribution installs on a card, and every .mra it lists is downloaded so the
<rbf> element can be read rather than reconstructed from a repository name.

The alternatives archive is fetched too. An alternative .mra names an <rbf> just
like a top-level one does, so a core that only ever appears under _alternatives
is still reachable and must not be reported dead.

Usage: fetch_official.py <workdir>
"""

import concurrent.futures
import hashlib
import json
import os
import sys
import urllib.parse
import urllib.request
import zipfile

DB_URL = ("https://raw.githubusercontent.com/MiSTer-devel/"
          "Distribution_MiSTer/main/db.json.zip")


def get(url):
    with urllib.request.urlopen(url, timeout=120) as r:
        return r.read()


def fetch_db(workdir):
    blob = get(DB_URL)
    zpath = os.path.join(workdir, "db.json.zip")
    with open(zpath, "wb") as fh:
        fh.write(blob)
    with zipfile.ZipFile(zpath) as z:
        z.extractall(workdir)
    with open(os.path.join(workdir, "db.json")) as fh:
        return json.load(fh)


def fetch_mras(db, workdir):
    base = db["base_files_url"]
    out = os.path.join(workdir, "mra")
    os.makedirs(out, exist_ok=True)
    todo = []
    for path, meta in db["files"].items():
        if not path.endswith(".mra"):
            continue
        dest = os.path.join(out, path.split("/", 1)[1])
        todo.append((base + urllib.parse.quote(path), dest, meta["hash"]))

    def one(job):
        url, dest, want = job
        if os.path.exists(dest):
            with open(dest, "rb") as fh:
                if hashlib.md5(fh.read()).hexdigest() == want:
                    return None
        blob = get(url)
        got = hashlib.md5(blob).hexdigest()
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        with open(dest, "wb") as fh:
            fh.write(blob)
        return None if got == want else f"{dest}: md5 {got} != {want}"

    bad = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
        for err in pool.map(one, todo):
            if err:
                bad.append(err)
    return len(todo), bad


def fetch_alternatives(db, workdir):
    spec = db["archives"]["mra_alternatives"]["archive_file"]
    zpath = os.path.join(workdir, "mra_alternatives.zip")
    if not os.path.exists(zpath):
        with open(zpath, "wb") as fh:
            fh.write(get(spec["url"]))
    with open(zpath, "rb") as fh:
        got = hashlib.md5(fh.read()).hexdigest()
    if got != spec["hash"]:
        raise SystemExit(f"mra_alternatives.zip md5 {got} != {spec['hash']}")
    dest = os.path.join(workdir, "alt")
    with zipfile.ZipFile(zpath) as z:
        z.extractall(dest)
    return sum(1 for _, _, fs in os.walk(dest)
               for f in fs if f.endswith(".mra"))


def main():
    workdir = sys.argv[1]
    os.makedirs(workdir, exist_ok=True)
    db = fetch_db(workdir)
    print(f"db.json: {len(db['files'])} files, "
          f"{sum(1 for k in db['files'] if k.endswith('.rbf'))} rbf, "
          f"timestamp {db['timestamp']}")
    n, bad = fetch_mras(db, workdir)
    print(f"top-level .mra: {n} fetched/verified, {len(bad)} bad")
    for b in bad:
        print("  " + b)
    print(f"alternative .mra: {fetch_alternatives(db, workdir)}")


if __name__ == "__main__":
    main()
