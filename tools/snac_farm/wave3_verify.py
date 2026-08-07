"""Prove the wave-3 archive loads, one check per failure mode wave 2 hit.

Wave 2 was audited before release and the audit passed. It passed because it
grouped candidate filenames case-sensitively, so StarForce_20260731.rbf and
Starforce_20260418.rbf looked like two unrelated cores that could not possibly
shadow each other. get_rbf() groups with strncasecmp and only then compares with
strcmp, and under those two rules upstream's older file won. Every grouping here
is case-insensitive and every winner is chosen case-sensitively.

The checks are deliberately separate because the two ways a core dies are
separate. RESOLVE answers "does any .mra name this file at all", which is what
killed ATetris, GundamSD and RushnAttack. SHADOW answers "when something does
name it, do we win", which is what killed Freeze and StarForce. A core can pass
either one and still be dead.

Usage: wave3_verify.py <workdir> <plan.json> <packed.json> <wave3.zip>
"""

import json
import os
import sys
import zipfile

ARCADE = "_Arcade/cores"


def candidate_of(prefix, entry):
    """get_rbf()'s test: strncasecmp on the prefix, then a '.' or '_' boundary."""
    n = len(prefix)
    return (len(entry) > n
            and entry[:n].lower() == prefix.lower()
            and entry[n] in "._"
            and entry.lower().endswith(".rbf"))


def candidates(rbfvalue, listing):
    """Both passes get_rbf() makes: the bare <rbf> value and "Arcade-" + it."""
    out = []
    for entry in listing:
        for prefix in ("Arcade-" + rbfvalue, rbfvalue):
            if candidate_of(prefix, entry):
                out.append(entry)
                break
    return out


def get_rbf(rbfvalue, listing):
    """The lexicographic maximum under a case-sensitive comparison, as strcmp."""
    c = candidates(rbfvalue, listing)
    return max(c) if c else None


def main():
    workdir, planfile, packedfile, zippath = sys.argv[1:5]
    with open(planfile) as fh:
        plan = json.load(fh)
    with open(packedfile) as fh:
        packed = json.load(fh)
    rows = packed["rows"]
    mras = plan["mra_rbf_values"]
    official = plan["official"]

    ours_arcade = sorted(r["wave3_name"] for r in rows if r["folder"] == ARCADE)
    off_arcade = sorted(o["name"] for o in official if o["folder"] == ARCADE)
    on_card = sorted(set(ours_arcade) | set(off_arcade))

    fail = 0

    print("=" * 78)
    print("CHECK 1  RESOLVE - every _Arcade/cores file is named by some .mra")
    print("=" * 78)
    unresolved = []
    for name in ours_arcade:
        hits = sorted(v for v in mras if candidates(v, [name]))
        if not hits:
            unresolved.append(name)
            print(f"  UNRESOLVABLE  {name}")
            continue
        v = hits[0]
        ex = os.path.basename(mras[v][0])
        print(f"  ok  {name:36} <rbf>{v}</rbf>  "
              f"({len(hits)} value(s), {sum(len(mras[h]) for h in hits)} mra) "
              f"e.g. {ex}")
    print(f"\n  {len(ours_arcade) - len(unresolved)}/{len(ours_arcade)} resolvable, "
          f"{len(unresolved)} unresolvable")
    fail += len(unresolved)

    print()
    print("=" * 78)
    print("CHECK 2  SHADOW - for every .mra <rbf> value, get_rbf() picks ours")
    print("=" * 78)
    shadowed = []
    for v in sorted(mras, key=str.lower):
        c = candidates(v, on_card)
        if not c:
            continue
        mine = [x for x in c if x in set(ours_arcade)]
        if not mine:
            continue
        winner = max(c)
        beaten = sorted(x for x in c if x != winner)
        if winner in set(ours_arcade):
            print(f"  ok  <rbf>{v:26}</rbf> -> {winner:36} "
                  f"beats {beaten if beaten else '(sole candidate)'}")
        else:
            shadowed.append((v, winner, mine))
            print(f"  SHADOWED  <rbf>{v}</rbf> -> {winner} (official) "
                  f"over ours {mine}")
    print(f"\n  {len(shadowed)} shadowed")
    fail += len(shadowed)

    print()
    print("=" * 78)
    print("CHECK 2b  no upstream-only core was broken by adding our files")
    print("=" * 78)
    regress = []
    for v in sorted(mras, key=str.lower):
        before = get_rbf(v, off_arcade)
        after = get_rbf(v, on_card)
        if before and not after:
            regress.append(v)
    print(f"  <rbf> values that resolved before and not after: {regress or 'none'}")
    orphan = [v for v in sorted(mras) if not candidates(v, on_card)]
    print(f"  <rbf> values no .rbf satisfies, ours or upstream's: {orphan}")
    fail += len(regress)

    print()
    print("=" * 78)
    print("CHECK 3  COLLISIONS and byte-identical twins")
    print("=" * 78)
    paths = [r["card"] for r in rows]
    dupe_paths = sorted({p for p in paths if paths.count(p) > 1})
    print(f"  duplicate card paths: {dupe_paths or 'none'}")
    names = {}
    for r in rows:
        names.setdefault(r["wave3_name"].lower(), []).append(r["card"])
    same_name = {k: v for k, v in names.items() if len(v) > 1}
    print(f"  same basename in different folders: {same_name or 'none'}")
    print(f"  byte-identical md5 under >1 name: "
          f"{packed['duplicate_md5'] or 'none'}")
    fail += len(dupe_paths)

    print()
    print("=" * 78)
    print("CHECK 3b  no wave-3 name equals an official name exactly")
    print("=" * 78)
    # menu.rbf is the one file that must overwrite upstream's. It carries no
    # datecode, the firmware loads it by that exact name, and ours is the
    # patched SNAC menu - 8a23d2d6.. against upstream's 7e7dc9af.., different
    # bitstreams. Sitting beside upstream's copy is not an option the name
    # allows, so this is a deliberate replacement, not a collision.
    off_all = {(o["folder"], o["name"]) for o in official}
    clash = sorted((r["folder"], r["wave3_name"]) for r in rows
                   if (r["folder"], r["wave3_name"]) in off_all
                   and r["wave3_name"] != "menu.rbf")
    print(f"  deliberate: menu.rbf replaces upstream's (no datecode possible)")
    print(f"  unintended overwrites of an official file: {clash or 'none'}")
    ci = {(f.lower(), n.lower()) for f, n in off_all}
    cclash = sorted((r["folder"], r["wave3_name"]) for r in rows
                    if (r["folder"].lower(), r["wave3_name"].lower()) in ci
                    and (r["folder"], r["wave3_name"]) not in off_all)
    print(f"  case-only variants of an official file (would confuse a "
          f"case-insensitive card): {cclash or 'none'}")
    fail += len(clash)

    print()
    print("=" * 78)
    print("CHECK 4  COUNTS")
    print("=" * 78)
    with zipfile.ZipFile(zippath) as z:
        zn = z.namelist()
    zrbf = [n for n in zn if n.endswith(".rbf")]
    w2 = sum(1 for r in rows if r["source"] == "wave2")
    fresh = sum(1 for r in rows if r["source"] != "wave2")
    print(f"  wave-2 archive .rbf entries:      273")
    print(f"  carried over unchanged in source: {w2}")
    print(f"  replaced by 20260807 rebuilds:    {fresh}")
    print(f"  {w2} + {fresh} = {w2 + fresh}")
    print(f"  wave-3 archive .rbf entries:      {len(zrbf)}")
    print(f"  planned rows:                     {len(rows)}")
    for r in sorted((r for r in rows if r["source"] != "wave2"),
                    key=lambda x: x["wave3_name"]):
        print(f"    substitution  {r['replaces']:38} -> {r['card']}")
    ok = len(zrbf) == 273 == len(rows) == w2 + fresh
    print(f"  reconciled: {ok}")
    if not ok:
        fail += 1

    print()
    print("=" * 78)
    print("CHECK 5  ARCHIVE HYGIENE")
    print("=" * 78)
    nonrbf = sorted(n for n in zn if not n.endswith(".rbf"))
    print(f"  non-.rbf entries: {nonrbf}")
    bad = [n for n in zn
           if os.path.basename(n) in ("MiSTer", "MiSTer.ini")
           or "/linux/" in n or n.startswith("linux/")]
    print(f"  firmware / MiSTer.ini / linux entries: {bad or 'none'}")
    print(f"  directory entries: "
          f"{[n for n in zn if n.endswith('/')] or 'none'}")
    fail += len(bad)

    print()
    print("=" * 78)
    print(f"RESULT: {'PASS' if fail == 0 else f'FAIL ({fail} problems)'}")
    print("=" * 78)
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
