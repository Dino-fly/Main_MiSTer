"""Decide the wave-3 filename for every core we ship, and prove the firmware
will actually load it.

Wave 2 shipped five cores that could never run. Three of them - ATetris,
GundamSD, RushnAttack - were named after their repository, and no .mra on the
card names those, so mra_loader.cpp never had a candidate to find. The other
two, Freeze and StarForce, were named in the case the repository used rather
than the case the distribution installs, and get_rbf() picks its winner with a
case SENSITIVE strcmp: 'F' is 70 and 'f' is 102, so Freeze_20260731.rbf sorts
below upstream's freeze_20240526.rbf and loses forever no matter how new it is.
StarForce_20260731.rbf loses to Starforce_20260418.rbf the same way.

So the name we ship is not derived from anything we own. It is upstream's
installed basename, byte for byte, and only the datecode is ours. The
distribution's db.json is the authority for that basename because it lists
exactly what lands on a card - and nothing in it is prefixed "Arcade-", which
is why matching a repository release name instead would reintroduce the bug:
"Arcade-AtariTetris_..." loses to "AtariTetris_..." on the very first differing
character, before the datecode is ever reached.

An earlier audit grouped candidates case-sensitively and therefore declared
StarForce healthy. This one groups case-insensitively, exactly as strncasecmp
does, and only then compares with strcmp. Both halves matter.

Usage: wave3_plan.py <workdir> <wave2.zip> <refreshdir> > plan.json
"""

import datetime
import json
import os
import re
import sys
import xml.etree.ElementTree as ET
import zipfile

RBF_RE = re.compile(r"^(?P<base>.+)_(?P<date>\d{8})\.rbf$")

# The twelve fresh rebuilds and the wave-2 file each one replaces. Nine are
# rebuilds because upstream released a newer core than the one we forked and we
# must stay above its datecode; three are rebuilds because the wave-2 file was
# named after the repository and was unloadable.
SUPERSEDES = {
    "AtariTetris_20260807.rbf":  "_Arcade/cores/ATetris_20260731.rbf",
    "Gaplus_20260807.rbf":       "_Arcade/cores/Gaplus_20260731.rbf",
    "RshnAtk_20260807.rbf":      "_Arcade/cores/RushnAttack_20260731.rbf",
    "Salamander_20260807.rbf":   "_Arcade/cores/Salamander_20260731.rbf",
    "Arcadia_20260807.rbf":      "_Console/Arcadia_20260731.rbf",
    "Asteroids_20260807.rbf":    "_Arcade/cores/Asteroids_20260731.rbf",
    "DECOCassette_20260807.rbf": "_Arcade/cores/DECOCassette_20260731.rbf",
    "Qix_20260807.rbf":          "_Arcade/cores/Qix_20260731.rbf",
    "SDGundamPS_20260807.rbf":   "_Arcade/cores/GundamSD_20260731.rbf",
    "SGB_20260807.rbf":          "_Console/SGB_20260731.rbf",
    "SNK6502_20260807.rbf":      "_Arcade/cores/SNK6502_20260731.rbf",
    "UK101_20260807.rbf":        "_Computer/UK101_20260731.rbf",
}


# Thirteen cores have no layer-3 entry: db.json does not install a .rbf under
# any spelling of their name. For those the rule falls back to layer 2, the
# repository's newest released .rbf with "Arcade-" stripped, measured by
# layer2_names.py against MiSTer-devel rather than guessed. Where layer 2
# disagrees with the name wave 2 shipped, the name is left alone on purpose:
# these are all console/computer cores, nothing resolves them by name the way
# .mra does, so the only symptom is a second entry in the core list - and
# renaming Genesis to MegaDrive would land on a file we already ship.
LAYER2 = {
    "AsteroidsDeluxe":      ("Arcade-AsteroidsDeluxe_MiSTer", "AsteroidsDeluxe", "20240525"),
    "LunarLander":          ("Arcade-LunarLander_MiSTer", "LunarLander", "20240529"),
    "Intv":                 ("Intv_MiSTer", "Intellivision", "20250903"),
    "Life":                 ("Life_MiSTer", "GameOfLife", "20260702"),
    "PCFX":                 ("PCFX_MiSTer", "PCFX", "20260515"),
    "SuperCassetteVision":  ("SuperCassetteVision_MiSTer", "SCV", "20250906"),
    "Minimig-AGA":          ("Minimig-AGA_MiSTer", "Minimig", "20260603"),
    "Genesis":              ("MegaDrive_MiSTer", "MegaDrive", "20260603"),
    "EpochGalaxy2":         (None, "EpochGalaxyII", None),
    "OndraSPO186":          (None, "Ondra_SPO186", None),
    "RX-78":                (None, "RX78", None),
    "SAM-Coupe":            (None, "SAMCoupe", None),
    "TI-99_4A":             (None, "Ti994a", None),
}


def split_rbf(name):
    m = RBF_RE.match(name)
    return (m.group("base"), m.group("date")) if m else (name[:-4], None)


def norm(base):
    """The key two names share when only prefix style or case differs."""
    b = base.lower()
    return b[7:] if b.startswith("arcade-") else b


def candidate_of(prefix, entry):
    """get_rbf()'s acceptance test: strncasecmp prefix plus a '.'/'_' boundary."""
    n = len(prefix)
    return (len(entry) > n
            and entry[:n].lower() == prefix.lower()
            and entry[n] in "._"
            and entry.lower().endswith(".rbf"))


def get_rbf(rbfvalue, listing):
    """mra_loader.cpp get_rbf(), including the "Arcade-" second pass and the
    case-sensitive strcmp that keeps the lexicographic maximum."""
    best = None
    for entry in listing:
        for prefix in ("Arcade-" + rbfvalue, rbfvalue):
            if candidate_of(prefix, entry):
                if best is None or best < entry:
                    best = entry
    return best


def load_official(workdir):
    with open(os.path.join(workdir, "db.json")) as fh:
        db = json.load(fh)
    out = []
    for path in db["files"]:
        if not path.endswith(".rbf"):
            continue
        folder, name = path.rsplit("/", 1) if "/" in path else ("", path)
        base, date = split_rbf(name)
        out.append({"folder": folder, "name": name, "base": base, "date": date})
    return db, out


def load_mras(workdir):
    """Every .mra's <rbf> value, top-level and alternatives."""
    vals = {}
    for root in ("mra", "alt"):
        for dirpath, _, files in os.walk(os.path.join(workdir, root)):
            for f in files:
                if not f.endswith(".mra"):
                    continue
                p = os.path.join(dirpath, f)
                try:
                    node = ET.parse(p).getroot().find("rbf")
                    v = node.text.strip() if node is not None and node.text else None
                except ET.ParseError:
                    v = None
                    with open(p, "rb") as fh:
                        m = re.search(rb"<rbf>\s*([^<]+?)\s*</rbf>", fh.read())
                        if m:
                            v = m.group(1).decode("utf-8", "replace")
                if v:
                    vals.setdefault(v, []).append(os.path.relpath(p, workdir))
    return vals


def bump(date, floor):
    """A datecode strictly greater than floor, preferring the one we built on."""
    if floor is None or date > floor:
        return date, False
    d = datetime.datetime.strptime(floor, "%Y%m%d").date()
    return (d + datetime.timedelta(days=1)).strftime("%Y%m%d"), True


def main():
    workdir, wave2zip, refresh = sys.argv[1], sys.argv[2], sys.argv[3]
    db, official = load_official(workdir)
    mras = load_mras(workdir)

    # Ours, keyed by the wave-2 card path they occupy.
    ours = []
    with zipfile.ZipFile(wave2zip) as z:
        for info in z.infolist():
            if info.is_dir() or not info.filename.endswith(".rbf"):
                continue
            card = info.filename.split("SD-CARD-ROOT/", 1)[1]
            ours.append({"card": card, "source": "wave2",
                         "src_ref": info.filename, "size": info.file_size})
    replaced = set(SUPERSEDES.values())
    kept = [o for o in ours if o["card"] not in replaced]
    missing = replaced - {o["card"] for o in ours}
    for fresh, old in sorted(SUPERSEDES.items()):
        kept.append({"card": os.path.dirname(old) + "/" + fresh
                     if os.path.dirname(old) else fresh,
                     "source": "refresh",
                     "src_ref": os.path.join(refresh, fresh),
                     "size": os.path.getsize(os.path.join(refresh, fresh)),
                     "replaces": old})

    by_norm = {}
    for o in official:
        by_norm.setdefault(norm(o["base"]), []).append(o)

    plan = []
    for o in sorted(kept, key=lambda x: x["card"]):
        folder, name = (o["card"].rsplit("/", 1) if "/" in o["card"]
                        else ("", o["card"]))
        base, date = split_rbf(name)
        cands = by_norm.get(norm(base), [])
        if cands:
            tgt_base = cands[0]["base"]
            floor = max((c["date"] for c in cands if c["date"]), default=None)
            tgt_date, bumped = bump(date, floor) if date else (None, False)
            fallback = "layer3-distribution"
        elif base in LAYER2:
            repo, l2, l2date = LAYER2[base]
            tgt_base, floor, bumped = base, l2date, False
            tgt_date = date
            if l2date and date and date <= l2date:
                tgt_date, bumped = bump(date, l2date)
            same = (l2 == base)
            fallback = ("layer2-repo-release" if same else
                        f"layer2-differs (upstream installs {l2}); kept ours, "
                        f"duplicate list entry only")
        else:
            tgt_base, tgt_date, floor, bumped = base, date, None, False
            fallback = "UNKNOWN - no layer 3, no layer 2 recorded"
        tgt = f"{tgt_base}_{tgt_date}.rbf" if tgt_date else f"{tgt_base}.rbf"
        plan.append({
            "folder": folder, "wave2_name": name, "wave3_name": tgt,
            "source": o["source"], "src_ref": o["src_ref"], "size": o["size"],
            "fallback": fallback, "official_floor": floor, "bumped": bumped,
            "official_folders": sorted({c["folder"] for c in cands}),
            "replaces": o.get("replaces"),
        })

    json.dump({"plan": plan,
               "official": official,
               "mra_rbf_values": mras,
               "missing_supersede_targets": sorted(missing),
               "db_timestamp": db["timestamp"]},
              sys.stdout, indent=1, sort_keys=True)


if __name__ == "__main__":
    main()
