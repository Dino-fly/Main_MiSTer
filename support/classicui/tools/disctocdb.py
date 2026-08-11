#!/usr/bin/env python3
"""
Builds classicui/disctoc.txt - the table that names a disc carrying no product code.

    python3 support/classicui/tools/disctocdb.py --fetch -o disctoc.txt
    python3 support/classicui/tools/disctocdb.py *.dat   -o disctoc.txt
    python3 support/classicui/tools/disctocdb.py --selftest

Copy the result to /media/fat/classicui/disctoc.txt. Optional: with no file there a PC
Engine CD disc shows what it shows today, which is the console's name.

WHY A TABLE OF DISC LENGTHS
PlayStation, Saturn and Mega CD discs carry a product code and disctitles.txt turns it into
a name. A PC Engine CD disc carries nothing: no product code, no ISO filesystem, and a boot
area holding only a Hudson copyright notice and code-module labels. Neo Geo CD has a volume
label that is a house code as often as a name.

What such a disc does have is its shape. Measured against a real disc on 2026-08-12: the
drive reported 22 tracks with the leadout at sector 221262, and exactly one release in
Redump's 551 fits - Akumajou Dracula X - Chi no Rondo (Japan), whose track sizes total
221262 sectors.

THE KEY IS THE TOTAL, NOT THE TRACK LAYOUT
The obvious key is every track's start sector, and it is the wrong one. Redump accounts a
track's index-0 pregap to the track before it and the drive does not, so per-track lengths
disagree - on that disc by -225, +75, +150 across the first three. They sum to zero. The
total is identical to the sector, because a pregap does not vanish, it only lands on the
other side of a boundary. So the key is "<tracks>:<total sectors>" and there is no pregap
question left to get wrong. Over both datfiles: 492 PC Engine CD keys and 103 Neo Geo CD
keys, which merge to 595 - no key is shared between the two systems, so one flat table needs
no system field and the firmware that reads it needed no change to gain Neo Geo CD.

AMBIGUITY IS RECORDED, NOT RESOLVED
40 keys name more than one dump, and the two kinds are different:
  - several dumps of ONE game (Doukyuusei Rev 3/Rev 4, Ys I & II and its Alt). Same game,
    same cover: naming it is right.
  - several DIFFERENT games (J. B. Harold Japan against USA, the two Hyper Catalog discs).
    Naming either would be a guess.
The second kind is written with a leading '?' and the reader refuses it. A wrong title feeds
the artwork path and yields a confidently wrong cover, which costs more than an empty one.

FORMAT - one record per line, tab separated, sorted by key:
    22:221262<TAB>Akumajou Dracula X - Chi no Rondo
    2:2<TAB>?Thing A
Region and revision decorations are stripped: what goes to ScreenScraper is a game name and
it matches with or without them - measured on 2026-08-12, both shapes hit.
"""

import io, re, sys, urllib.request, zipfile

# PC Engine CD and Neo Geo CD - the two consoles with no product code to read.
#
# One table for both, and that is measured rather than assumed: the two datfiles produce 492
# and 103 keys and the merge produces 595, so on 2026-08-12 not one key was shared between the
# systems and no name was lost to the merge. The key needs no system field, which is why
# adding Neo Geo CD needed no change to the key format or to the firmware that reads it.
#
# Worth re-checking if a third system is added: a cross-system collision is not an error - it
# is written with '?' and refused like any other - but it would cost two real names.
DAT_URLS = (
	"http://redump.org/datfile/pce/",
	"http://redump.org/datfile/ngcd/",
)
SECTOR = 2352


def base_title(name):
    n = re.sub(r'\s*\((?:Japan|USA|Europe|World|Korea|Taiwan|Brazil|Asia)[^)]*\)', '', name)
    n = re.sub(r'\s*\((?:Rev|Alt|Demo|Unl|Beta|Proto)[^)]*\)', '', n)
    n = re.sub(r'\s*\([A-Z]{4}\)', '', n)
    n = re.sub(r'\s*\((?:En|Ja|Es|Fr|De|It)[^)]*\)', '', n)
    return " ".join(n.split()).strip()


def parse(txt):
    out = {}
    for block in re.findall(r'<game name=.*?</game>', txt, re.S):
        m = re.search(r'<game name="([^"]+)"', block)
        if not m:
            continue
        tracks = {}
        for fn, size in re.findall(r'<rom name="([^"]+)" size="(\d+)"', block):
            t = re.search(r'\(Track (\d+)\)', fn)
            if t and fn.lower().endswith('.bin'):
                tracks[int(t.group(1))] = int(size)
        if not tracks:
            continue
        key = "%d:%d" % (len(tracks), sum(v // SECTOR for v in tracks.values()))
        out.setdefault(key, set()).add(base_title(m.group(1)))
    return out


def emit(table, out):
    named = amb = 0
    with open(out, 'w', encoding='utf-8') as f:
        f.write("#classicui-disctoc 1\n")
        for key in sorted(table, key=lambda k: tuple(int(x) for x in k.split(':'))):
            titles = sorted(table[key])
            f.write("%s\t%s%s\n" % (key, "" if len(titles) == 1 else "?", titles[0]))
            if len(titles) == 1:
                named += 1
            else:
                amb += 1
    print("%s: %d keys, %d named, %d ambiguous" % (out, named + amb, named, amb))


def selftest():
    dat = '''<game name="Akumajou Dracula X - Chi no Rondo (Japan)">
      <rom name="a (Track 01).bin" size="8620080"/><rom name="a (Track 02).bin" size="512566368"/>
      </game><game name="Thing A (Japan)">
      <rom name="b (Track 01).bin" size="2352"/><rom name="b (Track 02).bin" size="2352"/>
      </game><game name="Thing B (USA)">
      <rom name="c (Track 01).bin" size="2352"/><rom name="c (Track 02).bin" size="2352"/>
      </game><game name="Thing A (Japan) (Rev 1)">
      <rom name="d (Track 01).bin" size="4704"/><rom name="d (Track 02).bin" size="2352"/>
      </game>'''
    t, ok = parse(dat), True
    # Computed rather than written out: getting this constant wrong by one is exactly the
    # mistake the selftest exists to catch, and it caught it in me first.
    want = "2:%d" % (8620080 // SECTOR + 512566368 // SECTOR)
    if t.get(want) != {"Akumajou Dracula X - Chi no Rondo"}:
        print("FAIL key/title: %s -> %s" % (want, t.get(want))); ok = False
    if t.get("2:2") != {"Thing A", "Thing B"}:
        print("FAIL different games must share a key: %s" % t.get("2:2")); ok = False
    if t.get("2:3") != {"Thing A"}:
        print("FAIL revisions of one game must not: %s" % t.get("2:3")); ok = False
    if base_title("Ryuuko no Ken (Japan) (En,Ja,Es) (FABT)") != "Ryuuko no Ken":
        print("FAIL decorations"); ok = False
    print("selftest: %s" % ("ok" if ok else "FAILED"))
    return 0 if ok else 1


def main():
    args = sys.argv[1:]
    if '--selftest' in args:
        return selftest()

    out = 'disctoc.txt'
    if '-o' in args:
        i = args.index('-o')
        out = args[i + 1]
        del args[i:i + 2]

    texts = []
    if '--fetch' in args:
        args.remove('--fetch')
        for url in DAT_URLS:
            req = urllib.request.Request(url, headers={'User-Agent': 'Mozilla/5.0'})
            with zipfile.ZipFile(io.BytesIO(urllib.request.urlopen(req, timeout=120).read())) as z:
                for nm in z.namelist():
                    if nm.lower().endswith('.dat'):
                        texts.append(z.read(nm).decode('utf-8', 'replace'))
    for p in args:
        texts.append(open(p, encoding='utf-8', errors='replace').read())
    if not texts:
        print(__doc__)
        return 2

    table = {}
    for t in texts:
        for k, v in parse(t).items():
            table.setdefault(k, set()).update(v)
    emit(table, out)
    return 0


if __name__ == '__main__':
    sys.exit(main())
