"""Build the wave-3 cores archive from the wave-2 archive plus the rebuilds.

Nothing is recompiled. A .rbf is a bitstream and carries no record of its own
filename, so renaming a wave-2 core is a complete fix for a core that was dead
only because the firmware could not find it. That is why this is a packaging
job and not a build job.

Wave 2 shipped SD-CARD-ROOT/MiSTer inside the same zip as the cores. Anyone who
merged that folder onto a card replaced their firmware as a side effect of
installing cores. Wave 3 is cores only, and archive_is_clean() fails the build
rather than trusting that no binary crept back in.

Entries are copied through in memory - decompress, md5, recompress under the new
name - so the 322MB input never gets a 950MB unpacked twin on disk.

Usage: wave3_pack.py <plan.json> <wave2.zip> <outdir>
"""

import hashlib
import json
import os
import sys
import zipfile

ROOT = "SD-CARD-ROOT"
FORBIDDEN_NAMES = {"MiSTer", "MiSTer.ini"}
FORBIDDEN_PREFIXES = ("linux/",)


def archive_is_clean(names):
    bad = []
    for n in names:
        rel = n.split(ROOT + "/", 1)[-1]
        if rel in FORBIDDEN_NAMES or os.path.basename(rel) in FORBIDDEN_NAMES:
            bad.append(n)
        if rel.startswith(FORBIDDEN_PREFIXES):
            bad.append(n)
    return bad


def main():
    planfile, wave2zip, outdir = sys.argv[1], sys.argv[2], sys.argv[3]
    with open(planfile) as fh:
        plan = json.load(fh)["plan"]
    os.makedirs(outdir, exist_ok=True)
    outzip = os.path.join(outdir, "snac-psx-pad-wave3-cores.zip")

    rows = []
    seen = {}
    by_md5 = {}
    src = zipfile.ZipFile(wave2zip)
    with zipfile.ZipFile(outzip, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as out:
        for e in sorted(plan, key=lambda x: (x["folder"], x["wave3_name"])):
            if e["source"] == "wave2":
                blob = src.read(e["src_ref"])
            else:
                with open(e["src_ref"], "rb") as fh:
                    blob = fh.read()
            digest = hashlib.md5(blob).hexdigest()
            card = f"{e['folder']}/{e['wave3_name']}" if e["folder"] else e["wave3_name"]
            if card in seen:
                raise SystemExit(f"collision: {card} from {e['wave2_name']} "
                                 f"and {seen[card]}")
            seen[card] = e["wave2_name"]
            by_md5.setdefault(digest, []).append(card)
            out.writestr(f"{ROOT}/{card}", blob)
            rows.append({**e, "card": card, "md5": digest,
                         "real_size": len(blob)})
        src.close()

        dup = {k: v for k, v in by_md5.items() if len(v) > 1}

        manifest = [
            "MANIFEST_wave3.txt - SNAC-PSX pad cores, wave 3",
            "",
            "Cores only. No firmware binary, no MiSTer.ini, nothing under linux/.",
            "Every file is a wave-2 or freshly rebuilt bitstream renamed to the",
            "basename the official distribution installs, in that exact case, with",
            "a datecode above every official candidate so mra_loader.cpp get_rbf()",
            "picks ours.",
            "",
            f"{'folder':22} {'final name':36} {'bytes':>9}  "
            f"{'md5':32}  source    naming",
        ]
        for r in rows:
            manifest.append(
                f"{r['folder'] or '.':22} {r['wave3_name']:36} "
                f"{r['real_size']:9d}  {r['md5']:32}  "
                f"{'wave2' if r['source']=='wave2' else 'rebuild':9} "
                f"{r['fallback']}")
        manifest += [
            "",
            f"total files: {len(rows)}",
            f"  from wave-2 archive: {sum(1 for r in rows if r['source']=='wave2')}",
            f"  from 20260807 rebuild: {sum(1 for r in rows if r['source']!='wave2')}",
            f"  total bytes: {sum(r['real_size'] for r in rows)}",
        ]
        if dup:
            manifest.append("")
            manifest.append("byte-identical files shipped under more than one name:")
            for k, v in sorted(dup.items()):
                manifest.append(f"  {k}  {'  '.join(v)}")

        renames = [
            "RENAMES_wave3.txt - wave-2 name -> wave-3 name",
            "",
            "A renamed core is a core wave 2 shipped dead. get_rbf() compares whole",
            "filenames with a case-sensitive strcmp, so Freeze_20260731.rbf sorted",
            "below upstream's freeze_20240526.rbf and never loaded; ATetris was a",
            "repository name that no .mra on the card ever mentions.",
            "",
        ]
        changed = []
        for r in rows:
            old = r["replaces"] or (
                f"{r['folder']}/{r['wave2_name']}" if r["folder"]
                else r["wave2_name"])
            if os.path.basename(old) != r["wave3_name"] or \
                    os.path.dirname(old) != r["folder"]:
                changed.append((old, r["card"], r["source"], r["fallback"]))
        for old, new, source, _ in sorted(changed):
            ob, nb = os.path.basename(old), os.path.basename(new)
            note = ("rebuilt 20260807" if source != "wave2"
                    else "case fix, same bitstream")
            if ob.lower() == nb.lower():
                note += "; CASE-ONLY - delete the old file first"
            renames.append(f"{old}  ->  {new}   [{note}]")
        renames.append("")
        renames.append(f"{len(changed)} of {len(rows)} files changed name or folder.")

        case_only, distinct = [], []
        for old, new, _, _ in changed:
            ob, nb = os.path.basename(old), os.path.basename(new)
            (case_only if ob.lower() == nb.lower() else distinct).append(old)

        cleanup = [
            "CLEANUP_wave3.txt - old filenames to delete from the card",
            "",
            "snac_remove_old_cores cannot remove a name it never installed, and it",
            "never installed a wave-3 name. Every path below is a wave-1/wave-2 file",
            "that wave 3 replaces under a different name, so it would otherwise sit",
            "on the card forever - and for the arcade entries it would keep losing",
            "the get_rbf() comparison anyway, which is exactly the bug being fixed.",
            "",
            "DELETE THESE BEFORE MERGING THE ARCHIVE. Six of the fixes change only",
            "the letter case, and a MiSTer card is exFAT or FAT32, which is case",
            "insensitive. Copying freeze_20260731.rbf onto a card that already holds",
            "Freeze_20260731.rbf overwrites the contents but leaves the directory",
            "entry spelled the old way, so the file keeps losing to upstream's",
            "freeze_20240526.rbf and the core stays exactly as dead as it was in",
            "wave 2. Deleting first is the only order that works.",
            "",
            "-- case-only renames: MUST be deleted before the merge --",
        ]
        for old in sorted(case_only):
            cleanup.append(f"{ROOT}/{old}")
        cleanup += [
            "",
            "-- superseded datecodes: safe to delete before or after the merge --",
        ]
        for old in sorted(distinct):
            cleanup.append(f"{ROOT}/{old}")

        for name, body in (("MANIFEST_wave3.txt", manifest),
                           ("RENAMES_wave3.txt", renames),
                           ("CLEANUP_wave3.txt", cleanup)):
            text = "\n".join(body) + "\n"
            with open(os.path.join(outdir, name), "w") as fh:
                fh.write(text)
            out.writestr(f"{ROOT}/{name}", text)

    with zipfile.ZipFile(outzip) as z:
        names = z.namelist()
    bad = archive_is_clean(names)
    if bad:
        raise SystemExit(f"archive contains forbidden files: {bad}")

    with open(os.path.join(outdir, "packed.json"), "w") as fh:
        json.dump({"rows": rows, "duplicate_md5": dup, "renames": changed},
                  fh, indent=1)

    print(f"{outzip}")
    print(f"  entries: {len(names)}  rbf: {sum(1 for n in names if n.endswith('.rbf'))}")
    print(f"  zip size: {os.path.getsize(outzip)}")
    print(f"  renamed: {len(changed)}")
    print(f"  byte-identical name pairs: {len(dup)}")


if __name__ == "__main__":
    main()
