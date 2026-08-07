#!/usr/bin/env python3
"""
Builds our MiSTer `downloader` database - the db.json.zip that keeps the files
Classic Home ships current on a card that runs update_all.

    python3 support/classicui/tools/mkdb.py --stage SD-CARD-DB --commit <sha>
    python3 support/classicui/tools/mkdb.py --check db.json.zip
    python3 support/classicui/tools/mkdb.py --selftest

--stage is a tree whose layout is the card's layout: classicui/disctitles.txt,
Scripts/*.sh, _Console/*.rbf and so on. Every file in it becomes one entry in
`files`, keyed on its path relative to the stage, hashed and sized here.

WHY THIS EXISTS AT ALL
----------------------
Anyone who runs update_all runs the official downloader, and the downloader only
knows about files some database claims. Ours were claimed by nobody, so a card
drifted: the disc title table stayed at whatever version was copied by hand, and
the SNAC cores never moved at all. Worse, update_all *reverted* the front-end,
because `MiSTer` belongs to the official distribution database and the file on
the card is whatever the last database to claim it said it should be.

A database of our own fixes the first half of that. It cannot fix the second
half, and this file will refuse to try - see below.

WHAT THIS DATABASE MAY NOT CONTAIN
----------------------------------
Downloader_MiSTer validates a database before it acts on any of it, and a single
bad path throws DbEntityValidationException for the *whole* database - not the
one entry. So the rules below are enforced here, at generation time, where the
failure is a message to us rather than a database that quietly does nothing on
every card that subscribed to it. Each is named after the constant in
src/downloader/db_entity.py that it mirrors:

  no_distribution_mister_invalid_paths   MiSTer, menu.rbf, Scripts/update.sh
  invalid_paths                          MiSTer.ini and the three alt inis,
                                         MiSTer.new, downloader.ini
  invalid_root_folders                   linux/, screenshots/, savestates/,
                                         downloader/
  folders_with_non_overridable_files     saves/ (only with overwrite = false)
  the path itself                        must be relative, no "..", must not
                                         start with "/", "." or "\\"
  a root-level downloader_*.ini          reserved for drop-in config files

The first line is the important one: **the firmware cannot be delivered this
way.** `MiSTer` and `menu.rbf` are reserved to the database whose id is
literally `distribution_mister`, and ours is not, so a database that lists them
is rejected entirely. That is a deliberate rule upstream, not an oversight, and
the way past it is a boot hook on the device - not this file.

WHERE THE FILES ARE SERVED FROM
-------------------------------
The `db` branch of the fork, addressed by commit rather than by branch name.
`base_files_url` is

    https://raw.githubusercontent.com/Dino-fly/Main_MiSTer/<commit>/

and every file's URL is that plus its card path, which is why the stage mirrors
the card. This is what the three databases already on Dinofly's card do -
distribution_mister, jtcores and Coin-OpCollection all pin base_files_url to a
40-character commit sha on a branch of their own repository - and it is the
right shape for a reason worth writing down: db.json.zip and the payload are
fetched at different moments, minutes apart on a slow card. A branch-name URL
lets a push in between serve different bytes than the ones we hashed, and the
user sees a hash mismatch on a file that is not corrupt. A commit URL cannot.

The db.json.zip itself is the opposite case and lives at the branch tip, because
its URL goes into a user's downloader.ini once and must keep working for ever.

Release assets were the alternative and lose on both counts: an asset URL
carries the tag, so every publication would need every user to edit their ini
again, and an asset name cannot contain "/", so the card path could not be
derived from it and each file would need a mangled name plus an explicit url.
The cost of the branch is git history carrying core binaries; keep it an orphan
branch so cloning the fork never fetches them.

TAGS
----
Users filter with them, so they follow the names the official database already
uses for the same folders (console/consolecores, arcade/arcadecores, cores,
scripts) - a card with `filter = !computer` in downloader.ini should mean the
same thing to us as it does to everyone else. Ours on top of those:

  classichome    every file in the database
  snac           every core, i.e. the whole reason to rebuild them
  disctitles     the serial-to-title table
  essential      classicui/ and Scripts/ - the files that are not optional

`essential` is not decoration. The downloader appends it to any filter that has
a positive term, so a card whose global filter reads `console` still gets the
disc title table instead of silently dropping it.
"""

import argparse, hashlib, ipaddress, json, os, re, subprocess, sys, tempfile, time, \
       urllib.parse, zipfile
from pathlib import Path

DB_ID = "Dino-fly/ClassicHome_MiSTer"
DB_URL = "https://raw.githubusercontent.com/Dino-fly/Main_MiSTer/db/db.json.zip"
BASE_URL = "https://raw.githubusercontent.com/Dino-fly/Main_MiSTer/%s/"

# Junk that a staging tree picks up from the host rather than from us. Skipped
# with a note rather than refused, because a stage assembled on a Mac always has
# some, and a path like "classicui/.DS_Store" is legal as far as the downloader
# is concerned - it would just be published.
JUNK = ("__MACOSX",)
JUNK_NAMES = (".DS_Store", "Thumbs.db", "desktop.ini", ".gitignore", ".gitkeep")


# ------------------------------------------------- the rules the downloader has ---
#
# Mirrors src/downloader/db_entity.py of MiSTer-devel/Downloader_MiSTer. All
# comparisons there are on the lowercased path, so they are here too.

INVALID_PATHS = ("mister.ini", "mister_alt.ini", "mister_alt_1.ini",
                 "mister_alt_2.ini", "mister_alt_3.ini", "mister.new",
                 "downloader.ini")

NO_DIST_INVALID_PATHS = ("mister", "menu.rbf", "scripts/update.sh")

INVALID_ROOT_FOLDERS = ("linux", "screenshots", "savestates", "downloader")

NON_OVERRIDABLE_ROOTS = ("saves",)

# Paths the downloader lets through in spite of the rules above. Reproduced so
# this checker agrees with the real one rather than being merely stricter.
EXCEPTIONAL_PATHS = ("linux", "linux/gamecontrollerdb",
                     "linux/gamecontrollerdb/gamecontrollerdb.txt",
                     "linux/gamecontrollerdb/gamecontrollerdb_user.txt",
                     "yc.txt")

# Allowed to the official distribution database only, and reproduced for the same
# reason: --check has to agree with the real validator when it is pointed at
# somebody else's published database, or it is not a check of anything.
DIST_EXCEPTIONAL_PATHS = ("linux/pdfviewer", "linux/lesskey", "linux/glow")

DISTRIBUTION_MISTER_DB_ID = "distribution_mister"


class Refused(Exception):
    """A path or a URL this database is not allowed to carry. Carries the rule."""

    def __init__(self, subject, rule, detail):
        self.subject, self.rule, self.detail = subject, rule, detail
        super().__init__('refusing "%s": %s (%s)' % (subject, detail, rule))


def check_path(path, db_id=DB_ID, overwrite=True):
    """
    The path as the downloader would validate it, returning its lowercased parts.

    Raises Refused naming the upstream rule. The db_id argument exists because
    two of the rules turn on whether this is the official distribution database;
    ours never is, and passing it makes that testable rather than assumed.
    """
    if not isinstance(path, str):
        raise Refused(path, "path must be a string", "not a string")
    if path == "":
        raise Refused(path, "path must be valid", "empty")
    if path[0] in "/.\\":
        raise Refused(path, "path must be valid",
                      'starts with "%s"; paths are relative to the card root' % path[0])
    if "\\" in path or "\r" in path or "\n" in path:
        raise Refused(path, "path must be valid", "contains a backslash or a newline")

    lower = path.lower()
    parts = lower.split("/")

    if lower in EXCEPTIONAL_PATHS:
        return parts
    if db_id.lower() == DISTRIBUTION_MISTER_DB_ID and lower in DIST_EXCEPTIONAL_PATHS:
        return parts

    if lower in INVALID_PATHS:
        raise Refused(path, "invalid_paths",
                      "no database may supply this file; it is the user's own configuration")
    if db_id.lower() != DISTRIBUTION_MISTER_DB_ID and lower in NO_DIST_INVALID_PATHS:
        raise Refused(path, "no_distribution_mister_invalid_paths",
                      "reserved to the database whose id is literally %s; "
                      "the firmware cannot be delivered by a third-party database"
                      % DISTRIBUTION_MISTER_DB_ID)
    if len(parts) == 1 and lower.startswith("downloader_") and lower.endswith(".ini"):
        raise Refused(path, "invalid_paths",
                      "root-level downloader_*.ini is reserved for drop-in configuration")
    if ".." in parts:
        raise Refused(path, "path can't contain root folders", 'contains ".."')
    if "" in parts:
        raise Refused(path, "path must be valid", "empty path component")
    if parts[0] in INVALID_ROOT_FOLDERS:
        raise Refused(path, "invalid_root_folders",
                      '"%s" at the card root is not a database\'s to write' % parts[0])
    if parts[0] in NON_OVERRIDABLE_ROOTS and overwrite:
        raise Refused(path, "folders_with_non_overridable_files",
                      '"%s" holds the player\'s saves; only overwrite = false is allowed'
                      % parts[0])

    # Ours, not upstream's: a card gets read on a desktop, and Windows silently
    # drops a trailing space from a name, so the file the hash was taken of is not
    # the file that ends up there. A trailing *dot* is the same class of Windows
    # problem and is deliberately not refused - jotego publishes
    # _Arcade/_alternatives/_M.I.A./ and it works, so refusing it would be this
    # script inventing a rule the ecosystem does not have.
    for part in parts:
        if part != part.strip():
            raise Refused(path, "path must be valid",
                          'component "%s" has a leading or trailing space' % part)
    return parts


def check_url(url):
    """is_url_valid() of db_entity.py: http(s) only, and never a private host."""
    try:
        parsed = urllib.parse.urlparse(url)
    except ValueError:
        return False
    if parsed.scheme not in ("http", "https"):
        return False
    if any(c in url for c in "\r\n"):
        return False
    host = parsed.hostname
    if not host or host.lower() == "localhost":
        return False
    try:
        ip = ipaddress.ip_address(host)
        if ip.is_private or ip.is_loopback:
            return False
    except ValueError:
        pass
    return True


def calculate_url(base_files_url, path):
    """other.py: plain concatenation onto a quoted path. Hence the trailing slash."""
    return base_files_url + urllib.parse.quote(path)


# ------------------------------------------------------------------------ tags ---
#
# Names verified against the official database as published: 'console' and
# 'consolecores' are two keys onto one tag index there, and so they are here.

ROOT_TAGS = {
    "_console":  ["console", "consolecores"],
    "_computer": ["computer", "computercores"],
    "_arcade":   ["arcade", "arcadecores"],
    "_other":    ["other", "othercores"],
    "_utility":  ["utility"],
    "scripts":   ["scripts"],
    "classicui": ["classicui"],
}

ESSENTIAL_ROOTS = ("classicui", "scripts")

COLLECTION_TAG = "classichome"
CORE_TAGS = ("cores", "snac")

# Filter terms are matched by "!?[a-z0-9]+[-_a-z0-9.]*$" with '-' and '_' stripped
# out first, so a tag is only ever lowercase letters and digits.
DATECODE = re.compile(r"^(.+?)[-_ ](\d{8}[a-z]?)$")


def tag_name(text):
    return re.sub(r"[^a-z0-9]", "", text.lower())


def core_identity(name):
    """
    ("snes", "20260603") for SNES_20260603.rbf, ("actfancer", None) for
    ActFancer.rbf.

    The datecode matters twice. It is what makes our cores sit beside the
    player's instead of over them, and it is what makes two of our own builds
    two different paths - so the pair has to be declared entangled, or a failed
    download of the new one takes the working old one with it.
    """
    stem = Path(name).stem
    m = DATECODE.match(stem)
    if m:
        return tag_name(m.group(1)), m.group(2)
    return tag_name(stem), None


def tags_for_file(path):
    parts = path.split("/")
    root = parts[0].lower()
    tags = [COLLECTION_TAG]
    tags += ROOT_TAGS.get(root, [tag_name(root)] if root else [])

    if root in ESSENTIAL_ROOTS:
        tags.append("essential")

    if path.lower().endswith(".rbf"):
        tags += list(CORE_TAGS)
        name, _ = core_identity(parts[-1])
        if name:
            tags.append(name)
    else:
        name = tag_name(Path(parts[-1]).stem)
        if name:
            tags.append(name)

    return sorted(set(t for t in tags if t))


def tangle_for_file(path):
    """
    None unless the name carries a datecode. A tangle id is opaque, but it is
    ours and not the official database's - two databases publishing SNES cores
    are not publishing the same core.
    """
    if not path.lower().endswith(".rbf"):
        return None
    name, datecode = core_identity(path.split("/")[-1])
    if not datecode or not name:
        return None
    return "classichome_%s_core" % name


def tags_for_folder(folder):
    """
    A folder is tagged for where it sits, and never for its own name past the
    root: the per-file name tags are what a filter picks a single core out with,
    and putting one on a folder would drop the folder along with the core.
    """
    root = folder.split("/")[0].lower()
    tags = [COLLECTION_TAG]
    tags += ROOT_TAGS.get(root, [tag_name(root)] if root else [])
    if root in ESSENTIAL_ROOTS:
        tags.append("essential")
    return set(t for t in tags if t)


def folders_for(paths):
    """
    Every ancestor directory of every file, tagged like its contents.

    'cores' lands on a folder only when a core sits directly in it, which is why
    the official database tags _Arcade/cores with it and bare _Arcade without -
    and _Arcade holds .mra files, not cores.
    """
    folders = {}
    for path in paths:
        parts = path.split("/")
        for depth in range(1, len(parts)):
            folder = "/".join(parts[:depth])
            tags = set(folders.get(folder, {}).get("tags", []))
            tags |= tags_for_folder(folder)
            if depth == len(parts) - 1 and path.lower().endswith(".rbf"):
                tags.update(CORE_TAGS)
            folders[folder] = {"tags": sorted(tags)}
    return folders


# ------------------------------------------------------------------ the database ---

def md5_and_size(path):
    h = hashlib.md5()
    size = 0
    with open(path, "rb") as f:
        while True:
            chunk = f.read(1 << 20)
            if not chunk:
                break
            h.update(chunk)
            size += len(chunk)
    return h.hexdigest(), size


def walk_stage(stage, log=print):
    """Every publishable file under the stage, as card paths, sorted."""
    paths = []
    for dirpath, dirnames, filenames in os.walk(stage):
        dirnames[:] = sorted(d for d in dirnames if d not in JUNK and d != ".git")
        for name in sorted(filenames):
            rel = os.path.relpath(os.path.join(dirpath, name), stage)
            rel = rel.replace(os.sep, "/")
            if name in JUNK_NAMES:
                log("  skipping host junk: %s" % rel)
                continue
            paths.append(rel)
    return sorted(paths)


def build(stage, base_files_url, db_id=DB_ID, db_url=DB_URL, timestamp=None,
          per_file_urls=False, default_filter=None, log=print):
    """
    The database, or Refused. Nothing is written until every path has passed.
    """
    if not base_files_url.endswith("/"):
        raise Refused(base_files_url, "base_files_url",
                      "must end in '/': the downloader concatenates it onto the "
                      "path with no separator")
    if not check_url(base_files_url):
        raise Refused(base_files_url, "is_url_valid",
                      "not an http(s) URL on a public host")

    paths = walk_stage(stage, log=log)
    if not paths:
        raise Refused(stage, "empty stage", "no files to publish")

    files, tag_dictionary = {}, {}

    def index(tag):
        return tag_dictionary.setdefault(tag, len(tag_dictionary))

    for path in paths:
        check_path(path, db_id=db_id)
        url = calculate_url(base_files_url, path)
        if not check_url(url):
            raise Refused(url, "is_url_valid", 'the URL for "%s" is not usable' % path)

        digest, size = md5_and_size(os.path.join(stage, path))
        entry = {"hash": digest, "size": size,
                 "tags": [index(t) for t in tags_for_file(path)]}
        tangle = tangle_for_file(path)
        if tangle:
            entry["tangle"] = [tangle]
        if per_file_urls:
            entry["url"] = url
        files[path] = entry

    folders = {}
    for folder, desc in folders_for(paths).items():
        check_path(folder, db_id=db_id)
        folders[folder] = {"tags": [index(t) for t in desc["tags"]]}

    db = {
        "v": 1,
        "db_id": db_id,
        "db_url": db_url,          # informational; the downloader reads it from the ini
        "timestamp": int(timestamp if timestamp is not None else time.time()),
        "files": files,
        "folders": folders,
        "tag_dictionary": tag_dictionary,
    }
    if not per_file_urls:
        db["base_files_url"] = base_files_url
    if default_filter:
        db["default_options"] = {"filter": default_filter}

    check(db, log=lambda *a: None)
    return db


def check(db, log=print):
    """
    Everything the downloader checks, against a database we already have.

    Runs over the generated database before it is written, and available on its
    own so a published db.json.zip - ours or anybody's - can be put through the
    same rules.
    """
    if not isinstance(db, dict):
        raise Refused("db", "improper format", "not an object")
    for field in ("db_id", "files", "folders", "timestamp"):
        if field not in db:
            raise Refused(field, "DbEntity", "mandatory field is missing")
    if db.get("v", 0) != 1:
        raise Refused("v", "DATABASE_LATEST_SUPPORTED_VERSION",
                      'must be 1, not %r' % db.get("v"))
    if not isinstance(db["timestamp"], int):
        raise Refused("timestamp", "DbEntity", "must be an integer unix epoch")
    for field in ("files", "folders", "tag_dictionary"):
        if field in db and not isinstance(db[field], dict):
            raise Refused(field, "DbEntity", "must be an object")

    db_id = str(db["db_id"])
    base = db.get("base_files_url", "")
    if base and not base.endswith("/"):
        raise Refused("base_files_url", "base_files_url", "must end in '/'")

    known = set(db.get("tag_dictionary", {}).values())

    for path, desc in db["files"].items():
        parts = check_path(path, db_id=db_id, overwrite=desc.get("overwrite", True))
        if not isinstance(desc.get("hash"), str):
            raise Refused(path, "check_file_pkg", "file needs a valid md5 hash")
        if not isinstance(desc.get("size"), int):
            raise Refused(path, "check_file_pkg", "file needs a valid size in bytes")
        url = desc.get("url") or (calculate_url(base, path) if base else None)
        if not url or not check_url(url):
            raise Refused(path, "check_file_pkg",
                          "no usable url, and no base_files_url to derive one from")
        for tag in desc.get("tags", []):
            if isinstance(tag, int) and tag not in known:
                raise Refused(path, "tag_dictionary", "tag index %d is not defined" % tag)
        del parts

    parents = set()
    for path in db["files"]:
        parts = path.split("/")
        for depth in range(1, len(parts)):
            parents.add("/".join(parts[:depth]))

    for folder, desc in db["folders"].items():
        check_path(folder.rstrip("/") or folder, db_id=db_id)
        for tag in (desc or {}).get("tags", []):
            if isinstance(tag, int) and tag not in known:
                raise Refused(folder, "tag_dictionary",
                              "tag index %d is not defined" % tag)

    missing = parents - set(f.rstrip("/") for f in db["folders"])
    if missing:
        raise Refused(sorted(missing)[0], "folders",
                      "%d parent folder(s) of listed files are not in folders; "
                      "the downloader creates only what is declared there"
                      % len(missing))

    log("check: ok - %d files, %d folders, %d tags, db_id %s"
        % (len(db["files"]), len(db["folders"]), len(db.get("tag_dictionary", {})), db_id))
    return db


def load(path):
    """A db.json or a db.json.zip, dispatched on the suffix as the downloader does."""
    if path.lower().endswith(".zip"):
        with zipfile.ZipFile(path) as z:
            names = z.namelist()
            if len(names) != 1:
                raise Refused(path, "load_json_from_zip",
                              "a zipped database must hold exactly one member, not %d"
                              % len(names))
            return json.loads(z.read(names[0]))
    with open(path, "rb") as f:
        return json.loads(f.read())


def write(db, out, fmt):
    """
    db.json, db.json.zip, or both.

    The zip is deflated and holds one member named after the outer file's stem,
    which is what the downloader's own save_json_on_zip() produces and what all
    three published databases look like. Its timestamp comes from the database's
    own, so rebuilding an unchanged database produces an unchanged file rather
    than a pointless commit.
    """
    body = json.dumps(db, sort_keys=True).encode()
    written = []
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)

    if fmt in ("json", "both"):
        with open(out, "wb") as f:
            f.write(body)
        written.append((out, len(body)))

    if fmt in ("zip", "both"):
        zip_path = out + ".zip"
        info = zipfile.ZipInfo(Path(out).name,
                              date_time=time.gmtime(db["timestamp"])[:6])
        info.compress_type = zipfile.ZIP_DEFLATED
        info.external_attr = 0o644 << 16
        with zipfile.ZipFile(zip_path, "w") as z:
            z.writestr(info, body, compresslevel=9)
        written.append((zip_path, os.path.getsize(zip_path)))

    return written


# -------------------------------------------------------------------- selftest ---

# path, the rule that must refuse it. Two of these are the whole reason this
# checker exists: MiSTer and menu.rbf look like the most natural things in the
# world for us to ship, and shipping either kills the entire database.
FORBIDDEN = [
    ("MiSTer",                    "no_distribution_mister_invalid_paths"),
    ("mister",                    "no_distribution_mister_invalid_paths"),
    ("menu.rbf",                  "no_distribution_mister_invalid_paths"),
    ("Scripts/update.sh",         "no_distribution_mister_invalid_paths"),
    ("MiSTer.ini",                "invalid_paths"),
    ("MiSTer_alt_2.ini",          "invalid_paths"),
    ("MiSTer.new",                "invalid_paths"),
    ("downloader.ini",            "invalid_paths"),
    ("downloader_classichome.ini", "invalid_paths"),
    ("linux/anything.txt",        "invalid_root_folders"),
    ("savestates/SNES/x.ss",      "invalid_root_folders"),
    ("screenshots/x.png",         "invalid_root_folders"),
    ("downloader/x.ini",          "invalid_root_folders"),
    ("saves/SNES/x.sav",          "folders_with_non_overridable_files"),
    ("../outside.txt",            "path must be valid"),
    ("classicui/../../etc/passwd", "path can't contain root folders"),
    ("/media/fat/x",              "path must be valid"),
    (".hidden",                   "path must be valid"),
    ("classicui\\x.txt",          "path must be valid"),
    ("",                          "path must be valid"),
    ("classicui//x.txt",          "path must be valid"),
    ("classicui/trailing.txt ",   "path must be valid"),
]

ALLOWED = [
    "classicui/disctitles.txt",
    "Scripts/classichome.sh",
    "_Console/SNES_20260731.rbf",
    "_Arcade/cores/ActFancer.rbf",
    "yc.txt",
    "linux/gamecontrollerdb/gamecontrollerdb.txt",
    # jotego publishes this one, so it is legitimate however Windows feels about it.
    "_Arcade/_alternatives/_M.I.A./M.I.A. - Missing in Action (version S).mra",
]

STAGE = {
    "classicui/disctitles.txt": b"#classicui-disctitles 1\nSLES01506\tMetal Gear Solid\n",
    "Scripts/classichome.sh": b"#!/bin/bash\nexit 0\n",
    "_Console/SNES_20260731.rbf": b"\x00rbf snes" * 41,
    "_Console/PSX_20260731.rbf": b"\x00rbf psx" * 37,
    "_Computer/AO486_20260731.rbf": b"\x00rbf 486" * 13,
    "_Arcade/cores/ActFancer.rbf": b"\x00rbf arcade" * 7,
    "classicui/.DS_Store": b"junk",
}


def _fail(msg):
    print("selftest: FAILED - %s" % msg)
    return 1


def selftest(downloader_src=None):
    """
    The forbidden paths one by one, then a real stage generated and taken apart
    again. The hashes are re-checked with the system md5 binary rather than with
    hashlib a second time, because "hashlib agrees with hashlib" would pass even
    if this read the wrong file.

    With --downloader-src pointing at a checkout of MiSTer-devel/Downloader_MiSTer
    the generated database is additionally put through the real DbEntity, which
    is the only check here that cannot drift from upstream.
    """
    failures = 0

    for path, rule in FORBIDDEN:
        try:
            check_path(path)
        except Refused as e:
            if e.rule != rule:
                failures += _fail('"%s" refused by %s, expected %s'
                                  % (path, e.rule, rule))
        else:
            failures += _fail('"%s" was ACCEPTED; expected %s to refuse it'
                              % (path, rule))
    print("  %d forbidden paths, each refused by the rule that owns it" % len(FORBIDDEN))

    for path in ALLOWED:
        try:
            check_path(path)
        except Refused as e:
            failures += _fail('"%s" is legitimate but was refused: %s' % (path, e))
    print("  %d legitimate paths, all accepted" % len(ALLOWED))

    # saves/ is the one rule that turns on a field rather than on the path.
    try:
        check_path("saves/SNES/x.sav", overwrite=False)
    except Refused as e:
        failures += _fail("saves/ with overwrite = false should be allowed: %s" % e)

    # And two that turn on the db_id, which is why check_path takes one.
    try:
        check_path("menu.rbf", db_id="distribution_mister")
    except Refused as e:
        failures += _fail("menu.rbf is distribution_mister's to ship: %s" % e)
    try:
        check_path("linux/glow", db_id="distribution_mister")
    except Refused as e:
        failures += _fail("linux/glow is distribution_mister's to ship: %s" % e)
    try:
        check_path("MiSTer.ini", db_id="distribution_mister")
    except Refused:
        pass
    else:
        failures += _fail("MiSTer.ini is nobody's to ship, not even distribution_mister's")
    try:
        check_path("linux/glow")
    except Refused:
        pass
    else:
        failures += _fail("linux/glow is not ours to ship")

    with tempfile.TemporaryDirectory() as tmp:
        stage = os.path.join(tmp, "stage")
        for path, body in STAGE.items():
            full = os.path.join(stage, path)
            os.makedirs(os.path.dirname(full), exist_ok=True)
            with open(full, "wb") as f:
                f.write(body)

        base = BASE_URL % ("a" * 40)
        db = build(stage, base, timestamp=1786000000, log=lambda *a: None)

        # Schema, field by field.
        if db["v"] != 1:
            failures += _fail("v is %r" % db["v"])
        if db["db_id"] != DB_ID:
            failures += _fail("db_id is %r" % db["db_id"])
        if not isinstance(db["timestamp"], int):
            failures += _fail("timestamp is %r" % db["timestamp"])
        if db["base_files_url"] != base:
            failures += _fail("base_files_url is %r" % db["base_files_url"])
        if ".DS_Store" in json.dumps(db):
            failures += _fail("host junk reached the database")
        if len(db["files"]) != len(STAGE) - 1:
            failures += _fail("%d files, expected %d" % (len(db["files"]), len(STAGE) - 1))

        # Hashes and sizes against the system md5, and against the bytes we wrote.
        md5sum = None
        for candidate in (["md5sum"], ["md5", "-q"]):
            try:
                subprocess.run(candidate + [os.devnull], capture_output=True, check=True)
                md5sum = candidate
                break
            except (OSError, subprocess.CalledProcessError):
                continue

        for path, desc in db["files"].items():
            if desc["size"] != len(STAGE[path]):
                failures += _fail("%s size %d, file is %d bytes"
                                  % (path, desc["size"], len(STAGE[path])))
            if md5sum:
                out = subprocess.run(md5sum + [os.path.join(stage, path)],
                                     capture_output=True, check=True).stdout.decode()
                want = re.search(r"[0-9a-f]{32}", out).group(0)
                if desc["hash"] != want:
                    failures += _fail("%s hash %s, %s says %s"
                                      % (path, desc["hash"], md5sum[0], want))
        print("  %d files hashed and sized, cross-checked with %s"
              % (len(db["files"]), md5sum[0] if md5sum else "hashlib only"))

        # URLs.
        for path, desc in db["files"].items():
            url = calculate_url(db["base_files_url"], path)
            if not check_url(url):
                failures += _fail("unusable url for %s: %s" % (path, url))
            if urllib.parse.urlparse(url).path.count(" "):
                failures += _fail("unquoted space in %s" % url)
        print("  %d urls well formed under %s" % (len(db["files"]), base))

        # Tags: indexes resolve, and the ones we promised are on the right files.
        names = {v: k for k, v in db["tag_dictionary"].items()}
        got = {p: set(names[i] for i in d["tags"]) for p, d in db["files"].items()}
        want = {
            "classicui/disctitles.txt": {"classichome", "classicui", "essential",
                                         "disctitles"},
            "Scripts/classichome.sh": {"classichome", "scripts", "essential"},
            "_Console/SNES_20260731.rbf": {"classichome", "console", "consolecores",
                                           "cores", "snac", "snes"},
            "_Arcade/cores/ActFancer.rbf": {"classichome", "arcade", "arcadecores",
                                            "cores", "snac", "actfancer"},
        }
        for path, expected in want.items():
            if not expected <= got[path]:
                failures += _fail("%s tags %s, missing %s"
                                  % (path, sorted(got[path]), sorted(expected - got[path])))
        for tag in db["tag_dictionary"]:
            if not re.match(r"^[a-z0-9]+[-_a-z0-9.]*$", tag):
                failures += _fail("tag %r cannot be typed in a filter" % tag)
        # The whole dictionary, exactly. A tag nobody meant to publish is not a
        # cosmetic problem: folders were once tagged by handing tags_for_file() a
        # made-up filename, which put a tag called "x" on every folder in the
        # database, and `filter = ... !x` would then have deleted the lot.
        if set(db["tag_dictionary"]) != {
                "classichome", "essential", "cores", "snac",
                "classicui", "disctitles", "scripts",
                "console", "consolecores", "computer", "computercores",
                "arcade", "arcadecores", "snes", "psx", "ao486", "actfancer"}:
            failures += _fail("tag dictionary is %s" % sorted(db["tag_dictionary"]))
        print("  %d tags, all filterable, expected tags on the expected files"
              % len(db["tag_dictionary"]))

        # Datecoded cores are entangled, plain-named ones are not.
        if db["files"]["_Console/SNES_20260731.rbf"].get("tangle") != \
                ["classichome_snes_core"]:
            failures += _fail("datecoded core is not entangled")
        if "tangle" in db["files"]["_Arcade/cores/ActFancer.rbf"]:
            failures += _fail("plain-named arcade core should not be entangled")

        # Folders: exactly the parents, and _Arcade is not tagged as holding cores.
        if set(db["folders"]) != {"classicui", "Scripts", "_Console", "_Computer",
                                  "_Arcade", "_Arcade/cores"}:
            failures += _fail("folders are %s" % sorted(db["folders"]))
        arcade = set(names[i] for i in db["folders"]["_Arcade"]["tags"])
        if "cores" in arcade:
            failures += _fail("_Arcade holds .mra files, not cores")
        if "cores" not in set(names[i] for i in db["folders"]["_Arcade/cores"]["tags"]):
            failures += _fail("_Arcade/cores holds cores")

        # The round trip through both output formats.
        out = os.path.join(tmp, "db.json")
        for path, size in write(db, out, "both"):
            if size <= 0:
                failures += _fail("%s is empty" % path)
        if load(out) != db:
            failures += _fail("db.json does not read back identical")
        if load(out + ".zip") != db:
            failures += _fail("db.json.zip does not read back identical")
        with zipfile.ZipFile(out + ".zip") as z:
            if z.namelist() != ["db.json"]:
                failures += _fail("zip members are %s" % z.namelist())
            if z.infolist()[0].compress_type != zipfile.ZIP_DEFLATED:
                failures += _fail("zip member is not deflated")
        print("  db.json and db.json.zip both read back identical")

        # A forbidden file in a real stage must stop the whole build, not be dropped.
        for name in ("MiSTer", "menu.rbf", "MiSTer.ini"):
            with open(os.path.join(stage, name), "wb") as f:
                f.write(b"x")
            try:
                build(stage, base, timestamp=1786000000, log=lambda *a: None)
            except Refused as e:
                if e.subject != name:
                    failures += _fail("stage with %s refused over %s instead"
                                      % (name, e.subject))
            else:
                failures += _fail("a stage containing %s was published" % name)
            os.unlink(os.path.join(stage, name))
        print("  a stage containing MiSTer, menu.rbf or MiSTer.ini is refused whole")

        if downloader_src:
            failures += _upstream(db, downloader_src)

    if failures:
        return 1
    print("selftest: ok")
    return 0


def _upstream(db, src):
    """
    The generated database through the real Downloader_MiSTer validator.

    Worth the awkwardness of importing somebody else's package: everything else
    in this file is our reading of their rules, and a reading can go stale.
    """
    src = os.path.join(src, "src") if os.path.isdir(os.path.join(src, "src")) else src
    sys.path.insert(0, src)
    try:
        from downloader.db_entity import DbEntity, check_folder_paths, check_file_pkg
        from downloader.path_package import PathPackage
    except ImportError as e:
        print("  upstream check skipped: %s" % e)
        return 0

    try:
        entity = DbEntity(db, db["db_id"])
        check_folder_paths(list(entity.folders), entity.db_id)
        for path, desc in entity.files.items():
            pkg = PathPackage.from_full_path(path, {"description": desc}) \
                if hasattr(PathPackage, "from_full_path") else None
            if pkg is None:
                pkg = type("P", (), {"rel_path": path, "description": desc})()
            check_file_pkg(pkg, entity.db_id, calculate_url(entity.base_files_url, path))
    except Exception as e:
        return _fail("upstream %s: %s" % (type(e).__name__, e))

    print("  upstream DbEntity accepts it: db_id %s, %d files, %d folders"
          % (entity.db_id, len(entity.files), len(entity.folders)))
    return 0


# ------------------------------------------------------------------------ main ---

def main():
    ap = argparse.ArgumentParser(description="build the Classic Home downloader database")
    ap.add_argument("--stage", help="tree laid out like the card root")
    ap.add_argument("-o", "--out", default="db.json",
                    help="output path; the zip is this plus .zip (default: db.json)")
    ap.add_argument("--format", choices=("zip", "json", "both"), default="zip",
                    help="zip is what the ecosystem serves (default)")
    ap.add_argument("--commit", help="sha on the db branch that serves the files")
    ap.add_argument("--base-url", help="override the whole base_files_url; must end in /")
    ap.add_argument("--per-file-urls", action="store_true",
                    help="write url into every entry instead of one base_files_url")
    ap.add_argument("--db-id", default=DB_ID, help="do not change this once published")
    ap.add_argument("--db-url", default=DB_URL, help="where the database itself is served")
    ap.add_argument("--default-filter", help='e.g. "[MiSTer] !snac"')
    ap.add_argument("--timestamp", type=int, help="unix epoch; defaults to now")
    ap.add_argument("--check", metavar="DB", help="validate a db.json or db.json.zip")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--downloader-src", metavar="DIR",
                    help="checkout of MiSTer-devel/Downloader_MiSTer, for --selftest")
    args = ap.parse_args()

    if args.selftest:
        return selftest(args.downloader_src)

    if args.check:
        try:
            check(load(args.check))
        except Refused as e:
            print("%s: %s" % (args.check, e), file=sys.stderr)
            return 1
        return 0

    if not args.stage:
        ap.error("nothing to do - pass --stage, --check or --selftest")

    base_url = args.base_url
    if not base_url:
        if not args.commit:
            ap.error("--commit or --base-url is required: the files must be addressed "
                     "by commit, not by branch, so the bytes cannot move under a hash "
                     "we already published")
        base_url = BASE_URL % args.commit

    try:
        db = build(args.stage, base_url, db_id=args.db_id, db_url=args.db_url,
                   timestamp=args.timestamp, per_file_urls=args.per_file_urls,
                   default_filter=args.default_filter)
    except Refused as e:
        print("%s\n\nNothing was written. Take that path out of the stage: the "
              "downloader rejects the whole database over one bad path, so this "
              "would have broken every card subscribed to us." % e, file=sys.stderr)
        return 1

    for path, size in write(db, args.out, args.format):
        print("%s: %d bytes" % (path, size))
    print("%d files, %d folders, %d tags, timestamp %d"
          % (len(db["files"]), len(db["folders"]), len(db["tag_dictionary"]),
             db["timestamp"]))
    print("served from %s" % base_url)
    return 0


if __name__ == "__main__":
    sys.exit(main())
