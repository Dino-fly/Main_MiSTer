"""What to call a rebuilt core so the firmware picks it and not upstream's.

Three separate pieces of firmware resolve a core name to a file, and the rule
below is the only one that satisfies all three at once. Line references are to
this repository.

1. bootcore.cpp matchesCore_yyyyMMdd_rbf() / findCore(): matches exactly
   "<core>_YYYYMMDD.rbf" - fixed length, '_' separator, eight digits, and a
   case SENSITIVE strncmp on the prefix. Among matches the highest datecode
   wins numerically, and the comparison is `candidate.date > best.date`, so a
   tie leaves whichever readdir() happened to return first.

2. support/arcade/mra_loader.cpp get_rbf(): for an .mra it scans
   _Arcade/cores/ and accepts a file whose name starts, case-insensitively,
   with either the .mra's <rbf> value or "Arcade-" + that value, provided the
   next character is '.' or '_'. Among matches it keeps the lexicographic
   maximum of the whole filename.

3. file_io.cpp get_display_name(): splits the browser label at "_20" and shows
   the tail as a datecode, which MiSTer.ini rbf_hide_datecode can hide.

Consequences that dictate the rule:

* Only "<name>_YYYYMMDD.rbf" works everywhere. Any extra suffix such as
  "_snac" still satisfies the arcade prefix scan but fails
  matchesCore_yyyyMMdd_rbf outright, so bootcore and .mgl lookups would stop
  seeing the core while .mra lookups preferred it. Mixed behaviour is worse
  than either, so the datecode has to carry the distinction alone.

* <name> must be upstream's own released basename, copied verbatim, never
  re-derived from the repository name. Upstream is not consistent: repo
  Arcade-ActFancer_MiSTer releases ActFancer_20260708.rbf while repo
  Arcade-Gauntlet_MiSTer releases Arcade-Gauntlet_20250425.rbf. Because the
  arcade scan takes a lexicographic maximum over the full filename, the choice
  of prefix can beat the datecode: "Gauntlet_20260807.rbf" sorts above
  "Arcade-Gauntlet_20250425.rbf" (G > A) and wins, but the mirror image loses -
  ship "Arcade-Bagman_20260807.rbf" next to an upstream "Bagman_20260101.rbf"
  and upstream wins forever, because B > A, no matter how new we are. Matching
  upstream's basename makes the prefixes identical, which reduces the
  comparison to the datecode, which is the only thing we control.

* The datecode must be strictly greater than the datecode of the upstream
  release we built from. Equal is not enough: it produces the identical
  filename, so our file overwrites upstream's instead of sitting beside it,
  and where both survive the tie-break is undefined. Because the format has
  only day resolution there is no way to say "same day but later", so when we
  build on the same day upstream released we date one day ahead. That is a
  deliberate lie of up to 24 hours, and it is the price of the mechanism.
"""

import datetime
import re

RBF_RE = re.compile(r"^(?P<base>.+)_(?P<date>\d{8})\.rbf$", re.I)


def parse_release_rbf(filename):
    """('ActFancer', '20260708') from 'ActFancer_20260708.rbf', else None."""
    m = RBF_RE.match(filename)
    if not m:
        return None
    return m.group("base"), m.group("date")


def newest_release(filenames):
    """Pick upstream's current release out of a releases/ listing.

    Returns (basename, datecode, count) or (None, None, 0). Grouping is by
    basename because releases/ also holds .mra files, ROMs and alternatives
    folders, and a few repos have shipped more than one core from one repo.
    """
    groups = {}
    for name in filenames:
        p = parse_release_rbf(name)
        if not p:
            continue
        base, date = p
        groups.setdefault(base, []).append(date)
    if not groups:
        return None, None, 0
    # The core of the repo is the basename with the most releases; ties go to
    # the one with the newest datecode.
    base = max(groups, key=lambda b: (len(groups[b]), max(groups[b])))
    return base, max(groups[base]), len(groups[base])


def build_datecode(upstream_datecode, today=None):
    """Our datecode: today, bumped past upstream's if it would not beat it."""
    today = today or datetime.date.today()
    ours = today.strftime("%Y%m%d")
    if not upstream_datecode:
        return ours
    if ours > upstream_datecode:
        return ours
    d = datetime.datetime.strptime(upstream_datecode, "%Y%m%d").date()
    return (d + datetime.timedelta(days=1)).strftime("%Y%m%d")


def output_name(release_rbf, upstream_datecode, today=None):
    """The filename to install, e.g. 'ActFancer_20260807.rbf'."""
    return f"{release_rbf}_{build_datecode(upstream_datecode, today)}.rbf"


def card_folder(release_rbf, repo, is_arcade=None):
    """Where the file goes under the SD card root.

    Arcade cores live in _Arcade/cores because that is what mra_loader.cpp
    get_arcade_root() scans; everything else is browsed directly by the user,
    so the folder is presentation only.
    """
    if is_arcade is None:
        is_arcade = release_rbf.lower().startswith("arcade") or \
            repo.split("/")[-1].lower().startswith("arcade")
    return "_Arcade/cores" if is_arcade else "_Console"


if __name__ == "__main__":
    import sys
    day = datetime.date.fromisoformat(sys.argv[1]) if len(sys.argv) > 1 else None
    cases = [
        ("ActFancer", "20260708"),
        ("Arcade-Gauntlet", "20250425"),
        ("NES", "20260603"),
        ("PSX", "20260807"),   # upstream released today
        ("Foo", "20260808"),   # upstream release dated ahead of us
        ("Bar", ""),           # upstream has no dated release at all
    ]
    for base, dc in cases:
        print(f"{base:20} upstream={dc or '(none)':10} -> "
              f"{output_name(base, dc, day)}")
