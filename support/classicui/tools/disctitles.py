#!/usr/bin/env python3
"""
Builds classicui/disctitles.txt - the serial-to-title table the front-end reads when
there is a physical disc in the drive.

    python3 support/classicui/tools/disctitles.py --fetch -o disctitles.txt
    python3 support/classicui/tools/disctitles.py *.dat  -o disctitles.txt
    python3 support/classicui/tools/disctitles.py --selftest

Then copy the result to /media/fat/classicui/disctitles.txt on the card. It is
optional: with no file there the front-end shows what it shows today, which for a
PlayStation disc is the bare serial.

WHY THIS EXISTS AT ALL
----------------------
A disc has no filename. The library names everything else after the file it came
from; a pressed disc offers a serial - SLES-01506 - and nothing else. This turns the
serial into the name on the box.

THE INPUT IS NOT COMMITTED. THE OUTPUT IS
-----------------------------------------
The input is somebody else's several megabytes of metadata and it changes every week,
so it is not in the repository. Fetch it yourself, with --fetch or by hand.

One built table is committed, at support/classicui/disctitles.generated.txt, because
that is the file a release copies onto a card and running this script should never be
a prerequisite for shipping one. Regenerate that path, look at the diff, commit it -
and read support/classicui/DISCTITLES.md first, which records where that copy came
from and on what basis it is redistributed.

WHERE TO GET THE INPUT
----------------------
Any of these, mixed freely - later files fill gaps in earlier ones:

  Redump (default for --fetch)          https://redump.info/datfile/psx/serial
                                        .../mcd/serial .../pce/serial .../ngcd/serial

      Best coverage by a wide margin: ~10,600 PlayStation serials, versus ~2,800
      from MAME. **The /serial suffix on the URL is mandatory** - without it the
      DAT is generated with no <serial> elements at all and this script finds
      nothing. Roughly 4.8 MB of zips for the four systems.

      Licence: redump.info/about says the data "is considered public domain to be
      used however people see fit". Read that as a clearly stated intent rather
      than a formal grant - there is no CC0 deed or SPDX identifier behind it.
      Taking it at its word is what the committed table rests on; DISCTITLES.md
      says so in those words rather than claiming more. The DATs themselves are
      still yours to fetch - they are megabytes, and they change weekly.

      Note the domain: Redump moved from redump.org to redump.info in June 2026 and
      the old host no longer answers.

  MAME software lists                   https://github.com/mamedev/mame/tree/master/hash
                                        psx.xml, megacd.xml, pcecd.xml, neocd.xml

      Weaker coverage, and the only source here with an unambiguous licence: each
      file carries "license:CC0-1.0", MAME's COPYING dedicates hash/ to the public
      domain, and hash/README.md says CC0 1.0 Universal. The committed table is
      built from Redump instead, for the coverage; this is what to rebuild it from
      if Redump ever withdraws or disputes the statement above, and no code would
      have to change. (megacd.xml - not segacd.xml, which does not exist.)

  libretro-database                     metadat/redump/*.dat in
                                        github.com/libretro/libretro-database

      Redump's data restated in clrmamepro form with serials already hyphenated.
      Licensed CC-BY-SA-4.0, which is viral: a table built from it inherits
      ShareAlike, and mixing it with anything else muddies the position of the
      result. Supported because the format is, not because it is recommended.

  Anything you write yourself                                    KEY <TAB> Title

      A plain two-column file is read as-is. This is how you fix one wrong disc
      without regenerating anything - though for a single line it is easier to edit
      the output file on the card, which is half the reason the format is text.

  Deliberately not supported: DuckStation's data/resources/gamedb.yaml. It is the
  most convenient shape of all - keyed on serial already - and its repository is
  licensed CC BY-NC-ND 4.0. NoDerivatives forbids exactly what this script does.

WHAT COMES OUT
--------------
    #classicui-disctitles 1
    SLES01506	Metal Gear Solid
    SLUS00594	Final Fantasy VII

Keys are normalised - upper-cased, everything that is not A-Z or 0-9 removed - and
the file is sorted by byte order over the normalised key. That is not cosmetic: the
firmware binary-searches the file in place with strcmp, so this script's normalise()
and sort must agree with chome_titles.cpp's tdb_normalise() and its strcmp exactly.
The version in the magic line is what says so; change either end and change it.

Normalising is also what makes the sources agree with the discs. Redump writes a
Japanese PlayStation serial with a space ("SLPS 01204") and a European one with a
dash ("SLES-02375"); the disc itself carries "SLES_023.75". Strip everything but the
letters and digits and all three are one key.

Regional variants are kept apart, which is the point of keying on the serial at all:
SLES-01506 and SLUS-00594 are different discs and get different rows.
"""

import argparse, io, os, re, sys, unicodedata, urllib.request, xml.etree.ElementTree as ET, zipfile

MAGIC = "#classicui-disctitles 1"

# As DISC_TITLE_LEN in chome_titles.h, less the terminator. Longer titles are cut
# here rather than on the device, so what is in the file is what gets drawn.
TITLE_MAX = 63

REDUMP = "https://redump.info/datfile/%s/serial"

# psx, mcd and ss are the three the firmware can key on exactly - it reads a serial off
# those discs. pce and ngcd are fetched too and keyed on whatever the disc's volume label
# happens to be, which is a per-disc accident; see "which discs this helps" in
# chome_titles.h. Adding a system here is free, so the table is built for more than the
# reader can currently use rather than being regenerated every time the reader learns one.
REDUMP_SYSTEMS = ["psx", "mcd", "pce", "ngcd", "ss"]


# ---------------------------------------------------------------------- keys ---

def normalise(s):
    """The canonical key. Must match tdb_normalise() in chome_titles.cpp."""
    return "".join(c for c in s.upper() if ("A" <= c <= "Z") or ("0" <= c <= "9"))


def key_forms(serial):
    """
    The keys one source serial should answer to, most specific first.

    A serial as written is not always a serial as pressed. Redump records the
    variants of a release in the field - "SLES-03136-P", "SLUS-00975CE" - and Sega
    catalogue codes carry a region suffix that the disc header often omits:
    Redump's "MK-4420-50" is "GM MK-4420 -00" on the disc itself. So each serial
    yields its full form and, where it has more than two dash-separated parts, the
    form without the last one.

    The reduced form is only offered when it still looks like an identifier - at
    least six characters and containing a letter. Without that guard "4432-50"
    reduces to "4432", a four-digit key that would collide with anything.

    Two more forms exist for Sega's own catalogue numbers, and they are the difference
    between the Saturn and Mega CD readers working and not working at all.

    The reason is that **Redump's serial is not read out of the disc header.** Redump's
    own dumping guide says it is transcribed from "the serial/identification code on the
    disc label", so it agrees with the header only where Sega happened to silk-screen the
    same string - and for its own releases it did not. Three notations for one field:

        header "GM MK-4407 -00"  Sonic CD          Redump "4407"       MK- not printed
        header "MK-81084      "  Exhumed (EU)      Redump "81084-50"   MK- gone, region added
        header "MK-81009      "  Panzer Dragoon EU Redump "MK81009-50" hyphen dropped instead
        header "GM  T-81025-00"  Mortal Kombat     Redump "T-81025"    agrees
        header "GM G-6021  -00"  Sonic CD (JP)     Redump "G-6021"     agrees

    Third-party (T-) and Japanese Sega (G-, GS-) codes agree and need nothing. Sega's own
    MK- catalogue numbers never do. So after removing a trailing two-digit country code,
    a code that is nothing but four or five digits also yields itself with MK in front -
    which is what the disc will say - and one that already has letters yields itself.

    Both are reduced forms, so an exact serial from any source still wins over either.
    Measured against every disc in the two Sega DATs, modelling each disc's header field
    from its Redump serial, this takes the hit rate from 68.9% to 99.0% on Mega CD and
    from 76.7% to 99.8% on Saturn. What is left is the demo and magazine discs, where one
    catalogue number really does cover twenty volumes and no key could separate them.

    Neither rule can touch the other systems. A PlayStation serial's trailing part is
    five digits ("SLES-01506") and never two; PC Engine and Neo Geo codes carry letters
    throughout, so the digits-only test refuses them.
    """
    serial = re.sub(r"\([^)]*\)", " ", serial)        # MAME writes "NGCD-083 (JPN)"
    serial = serial.replace("#", " ")                 # Redump writes "# 4903" for Night Trap
    parts = [p for p in re.split(r"[\s\-_.]+", serial) if p]
    if not parts:
        return []

    full = normalise("".join(parts))
    if len(full) < 3:
        return []

    forms = [full]

    def reduce_to(base):
        if base and base != full and len(base) >= 6 and any("A" <= c <= "Z" for c in base) \
                and base not in forms:
            forms.append(base)

    if len(parts) > 2:
        reduce_to(normalise("".join(parts[:-1])))

    # A Sega country code sits in the same two digits a Mega Drive revision does, so it
    # is removed the same way, and what is left is the catalogue number the disc carries.
    core = parts[:-1] if len(parts) > 1 and re.fullmatch(r"\d\d", parts[-1]) else parts
    core = normalise("".join(core))

    reduce_to(core)
    if re.fullmatch(r"\d{4,5}", core):
        reduce_to("MK" + core)

    return forms


def split_serials(field):
    """Redump and MAME both put several serials in one field, comma-separated."""
    return [s for s in (t.strip() for t in re.split(r"[,;/]", field)) if s]


# -------------------------------------------------------------------- titles ---

BRACKETS = re.compile(r"\s*[\(\[].*$")


def clean_title(name, keep_flags):
    """
    The name as the front-end would show it.

    Bracket groups go by default, because that is what clean_title() in
    chome_lib.cpp does to every ROM filename on the shelf: a disc card reading
    "Metal Gear Solid" beside a shelf card reading "Metal Gear Solid" is the point.
    The region is not lost by dropping "(Europe)" - it is in the serial, which is
    what the row is keyed on.

    Non-ASCII is transliterated away rather than passed through. The front-end draws
    with an 8x8 bitmap font of 256 glyphs whose upper half is not Unicode, so a
    Japanese title arriving as UTF-8 would draw as a row of noise. Losing the
    accents off "Pokemon" is the lesser damage.
    """
    if not keep_flags:
        name = BRACKETS.sub("", name)

    name = unicodedata.normalize("NFKD", name)
    name = name.encode("ascii", "ignore").decode("ascii")
    name = name.replace("\t", " ")
    name = re.sub(r"\s+", " ", name).strip()

    return name[:TITLE_MAX].strip()


# ------------------------------------------------------------------ the input ---

def parse_xml(text, keep_flags, out):
    """
    Redump/no-intro <datafile> and MAME <softwarelist>, which differ only in where
    the name and the serial live.
    """
    try:
        root = ET.fromstring(text)
    except ET.ParseError as e:
        print("  not parseable as XML: %s" % e, file=sys.stderr)
        return 0

    n = 0
    for game in root.iter():
        if game.tag not in ("game", "machine", "software"):
            continue

        serials = []
        name = game.get("name") or ""

        # Redump: <serial>. MAME: <info name="serial" value="...">, and the name
        # attribute there is a short id ("mgs") rather than a title.
        for child in game:
            if child.tag == "serial" and child.text:
                serials.append(child.text)
            elif child.tag == "info" and child.get("name") == "serial":
                serials.append(child.get("value") or "")
            elif child.tag == "description" and child.text:
                name = child.text

        if not name:
            continue

        title = clean_title(name, keep_flags)
        if not title:
            continue

        for s in serials:
            for one in split_serials(s):
                n += add(out, key_forms(one), title)

    return n


CMP_GAME = re.compile(r"\bgame\s*\(", re.I)
CMP_FIELD = re.compile(r'^\s*(name|serial|description)\s+"([^"]*)"', re.I | re.M)


def parse_cmp(text, keep_flags, out):
    """clrmamepro, which is what libretro-database publishes."""
    n = 0
    for block in CMP_GAME.split(text)[1:]:
        block = block.split("\ngame", 1)[0]

        name, serials = "", []
        for field, value in CMP_FIELD.findall(block):
            field = field.lower()
            if field == "name" and not name:
                name = value
            elif field == "serial":
                serials.append(value)

        if not name:
            continue

        title = clean_title(name, keep_flags)
        if not title:
            continue

        # The rom lines repeat the game's serial, so the same pair arrives several
        # times. add() is idempotent for an identical title, so that costs nothing.
        for s in serials:
            for one in split_serials(s):
                n += add(out, key_forms(one), title)

    return n


def parse_tsv(text, keep_flags, out):
    n = 0
    for line in text.splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        if "\t" not in line:
            continue
        key, title = line.split("\t", 1)
        title = clean_title(title, True)          # already a title, not a filename
        if title:
            n += add(out, [normalise(key)], title)
    return n


def parse(text, keep_flags, out):
    head = text[:4096].lstrip()
    if head.startswith("<") or "<datafile" in head or "<softwarelist" in head:
        return parse_xml(text, keep_flags, out)
    if CMP_GAME.search(text[:65536]) or "clrmamepro" in head:
        return parse_cmp(text, keep_flags, out)
    return parse_tsv(text, keep_flags, out)


def read_source(path):
    """One path, one or more (label, text) pairs - a DAT or a zip full of them."""
    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as z:
            for member in z.namelist():
                if member.endswith("/"):
                    continue
                yield "%s:%s" % (os.path.basename(path), member), \
                      z.read(member).decode("utf-8", "replace")
        return

    with open(path, "rb") as f:
        yield os.path.basename(path), f.read().decode("utf-8", "replace")


# ------------------------------------------------------------------ the table ---

def add(out, forms, title):
    """
    Record a title under each of its key forms.

    Two levels, because a full serial must always beat a reduced one: a reduced key
    is a guess about what a disc header will say, and a full key is not. So the
    reduced forms are held apart and only used for keys nothing exact claimed.

    Within a level a contested key keeps the alphabetically first title, so that
    re-running over the same DAT produces the same file rather than whichever entry
    the parser happened to reach first.
    """
    if not forms:
        return 0

    added = 0
    for level, key in enumerate(forms):
        table = out[0] if level == 0 else out[1]
        if key not in table:
            table[key] = title
            added += 1
        elif title < table[key]:
            table[key] = title

    return added


def write(out, path):
    exact, reduced = out

    merged = dict(exact)
    shadowed = 0
    for key, title in reduced.items():
        if key in merged:
            shadowed += 1
        else:
            merged[key] = title

    # Byte order over keys of A-Z0-9 only, which is what strcmp does on the device.
    lines = ["%s\t%s\n" % (k, merged[k]) for k in sorted(merged)]

    body = MAGIC + "\n" + "".join(lines)
    with open(path, "wb") as f:                       # LF, on every host
        f.write(body.encode("ascii", "replace"))

    return len(lines), len(exact), len(reduced) - shadowed, len(body)


# -------------------------------------------------------------------- selftest ---

SELFTEST_REDUMP = """<?xml version="1.0"?>
<datafile>
 <game name="Metal Gear Solid (Europe)">
  <category>Games</category>
  <serial>SLES-01506, SLES-11506</serial>
  <description>Metal Gear Solid (Europe)</description>
 </game>
 <game name="Sonic the Hedgehog CD (Europe)">
  <category>Games</category>
  <serial>MK-4407-50</serial>
 </game>
 <game name="Nameless Disc (Japan)">
  <category>Games</category>
  <serial>4432-50-01</serial>
 </game>
 <game name="No Serial Here (USA)">
  <category>Games</category>
 </game>
 <game name="Panzer Dragoon (Japan)">
  <category>Games</category>
  <serial>GS-9032</serial>
 </game>
 <game name="Clockwork Knight (Europe)">
  <category>Games</category>
  <serial>MK81007-50</serial>
 </game>
 <game name="Guardian Heroes (USA)">
  <category>Games</category>
  <serial>81035</serial>
 </game>
 <game name="Batman Forever (Europe)">
  <category>Games</category>
  <serial>4432-50</serial>
 </game>
</datafile>
"""

SELFTEST_MAME = """<?xml version="1.0"?>
<softwarelist name="neocd">
 <software name="2020bb">
  <description>2020 Super Baseball (Japan)</description>
  <info name="serial" value="NGCD-030 (JPN)" />
 </software>
</softwarelist>
"""

SELFTEST_CMP = """clrmamepro (
	name "Sony - PlayStation"
)

game (
	name "Final Fantasy VII (USA) (Disc 1)"
	serial "SLUS-00594"
	rom ( name "x.bin" size 1 crc 0 serial "SLUS-00594" )
)
"""

SELFTEST_TSV = "SCES 00001\tMy Own Correction\n"

SELFTEST_WANT = """#classicui-disctitles 1
443250\tBatman Forever
44325001\tNameless Disc
81035\tGuardian Heroes
GS9032\tPanzer Dragoon
MK4407\tSonic the Hedgehog CD
MK440750\tSonic the Hedgehog CD
MK4432\tBatman Forever
MK81007\tClockwork Knight
MK8100750\tClockwork Knight
MK81035\tGuardian Heroes
NGCD030\t2020 Super Baseball
SCES00001\tMy Own Correction
SLES01506\tMetal Gear Solid
SLES11506\tMetal Gear Solid
SLUS00594\tFinal Fantasy VII
"""


def selftest():
    """
    Covers the things that are easy to get quietly wrong: multi-serial fields, the
    space-versus-dash disagreement, the reduced Sega codes, a game with no serial at
    all, and the sort order the device depends on.

    Most of the expected rows are about the guards rather than about any real disc:

      "MK-4407-50"  reduces to MK4407, which is what a Mega CD header would give.
      "MK81007-50"  reduces to MK81007 through the two-digit rule, which is what a
                    Saturn header gives - Redump writes that one without the dash.
      "81035"       gains MK81035 as well, which is what the disc says, while keeping
                    81035 so the Redump notation still resolves.
      "GS-9032"     has nothing added: two parts, last one four digits, so no rule
                    fires and only the full form appears. This is the row that fails
                    if the Sega rules ever start firing on ordinary serials.
      "4432-50"     reduces to 4432, which has no letter and is therefore *refused* -
                    so 443250 appears and 4432 does not. What it does gain is MK4432,
                    which is what that disc's header says. Likewise "4432-50-01" gives
                    44325001 alone. If a bare 4432 ever shows up here the reduction has
                    started inventing keys that could match anything.
      "NGCD-030 (JPN)"  MAME's parenthesised region must be stripped whole, not turned
                    into characters: an NGCD030JPN row matches no disc ever pressed.
    """
    out = ({}, {})
    for text in (SELFTEST_REDUMP, SELFTEST_MAME, SELFTEST_CMP, SELFTEST_TSV):
        parse(text, False, out)

    exact, reduced = out
    merged = dict(exact)
    for k, v in reduced.items():
        merged.setdefault(k, v)

    got = MAGIC + "\n" + "".join("%s\t%s\n" % (k, merged[k]) for k in sorted(merged))

    if got == SELFTEST_WANT:
        print("selftest: ok (%d rows)" % len(merged))
        return 0

    print("selftest: FAILED\n--- want ---\n%s\n--- got ---\n%s" % (SELFTEST_WANT, got))
    return 1


# ------------------------------------------------------------------------ main ---

def main():
    ap = argparse.ArgumentParser(description="build classicui/disctitles.txt")
    ap.add_argument("inputs", nargs="*", help="DAT, clrmamepro or TSV files, or zips of them")
    ap.add_argument("-o", "--out", default="disctitles.txt")
    ap.add_argument("--fetch", action="store_true",
                    help="download the four Redump DATs (%s) first" % ", ".join(REDUMP_SYSTEMS))
    ap.add_argument("--keep-flags", action="store_true",
                    help="keep \"(Europe)\" and the like in titles; off, to match the shelf")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    inputs = list(args.inputs)

    if args.fetch:
        for sys_id in REDUMP_SYSTEMS:
            url = REDUMP % sys_id
            path = "redump-%s.zip" % sys_id
            print("fetching %s" % url)
            try:
                # Redump serves the DAT as a zip whatever the URL looks like.
                with urllib.request.urlopen(url, timeout=60) as r, open(path, "wb") as f:
                    f.write(r.read())
            except Exception as e:
                print("  failed: %s" % e, file=sys.stderr)
                continue
            inputs.append(path)

    if not inputs:
        ap.error("no input files - pass some, or use --fetch")

    out = ({}, {})
    for path in inputs:
        try:
            for label, text in read_source(path):
                n = parse(text, args.keep_flags, out)
                print("  %-40s %6d keys" % (label, n))
        except OSError as e:
            print("  cannot read %s: %s" % (path, e), file=sys.stderr)

    rows, exact, reduced, size = write(out, args.out)
    print("\n%s: %d rows (%d exact, %d reduced), %d bytes" %
          (args.out, rows, exact, reduced, size))

    if not rows:
        print("nothing matched - if these are Redump DATs, check the URL ended in "
              "/serial: without it the DAT has no <serial> elements", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
