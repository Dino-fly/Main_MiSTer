#!/usr/bin/env python3
"""
Pull the identifying fields out of the disc images on the card.

Runs on the device so that only the fields travel, not the images. Emits one JSON
object per line: everything a physical disc could tell us about itself, which is the
whole point - the matching study has to work from what the drive can read, not from
the filename, because a disc in a tray does not have one.

Sector geometry is detected rather than assumed. A Redump .bin is 2352 bytes per
sector with the user data at 16 (MODE1) or 24 (MODE2 form 1), and getting it wrong
shifts every field by eight bytes, which reads as plausible garbage rather than as an
error.
"""
import json, os, sys

SEC = 2352

def geom(fh):
    fh.seek(0)
    s = fh.read(SEC)
    if len(s) < 32:
        return None
    if s[:12] != b"\x00\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\x00":
        return 0            # 2048-byte sectors, no sync
    return 24 if s[15] == 2 else 16

def sector(fh, off, lba):
    if off == 0:
        fh.seek(lba * 2048)
        return fh.read(2048)
    fh.seek(lba * SEC + off)
    return fh.read(2048)

def txt(b):
    return b.decode("latin-1", "replace").strip().strip("\x00").strip()

def saturn(fh, off):
    d = sector(fh, off, 0)
    if b"SEGASATURN" not in d[:32] and b"SEGA SEGASATURN" not in d[:32]:
        return None
    return {
        "hdr": "saturn",
        "serial": txt(d[0x20:0x2A]),
        "version": txt(d[0x2A:0x30]),
        "date": txt(d[0x30:0x38]),
        "region": txt(d[0x40:0x50]),
        "label": txt(d[0x60:0xD0]),
    }

def segacd(fh, off):
    d = sector(fh, off, 0)
    if b"SEGADISCSYSTEM" not in d[:16]:
        return None
    # The Mega Drive-style header sits at 0x100 of the first sector on a Mega CD disc.
    return {
        "hdr": "segacd",
        "serial": txt(d[0x180:0x18E]),
        "label": txt(d[0x150:0x180]) or txt(d[0x120:0x150]),
        "label_jp": txt(d[0x120:0x150]),
        "region": txt(d[0x1F0:0x1F3]),
    }

def iso_pvd(fh, off):
    d = sector(fh, off, 16)
    if d[1:6] != b"CD001":
        return None
    return {
        "volume": txt(d[40:72]),
        "publisher": txt(d[318:446]),
        "created": txt(d[813:830]),
        "root_lba": int.from_bytes(d[158:162], "little"),
        "root_len": int.from_bytes(d[166:170], "little"),
    }

def iso_find(fh, off, lba, length, want):
    """Walk one ISO9660 directory extent looking for a file, case-insensitively."""
    data = b""
    for i in range((length + 2047) // 2048):
        data += sector(fh, off, lba + i)
    p = 0
    out = []
    while p < len(data):
        rl = data[p]
        if rl == 0:
            p = (p // 2048 + 1) * 2048
            if p >= len(data):
                break
            continue
        nlen = data[p + 32]
        name = txt(data[p + 33:p + 33 + nlen]).split(";")[0]
        ext = int.from_bytes(data[p + 2:p + 6], "little")
        sz = int.from_bytes(data[p + 10:p + 14], "little")
        out.append(name)
        if name.upper() == want.upper():
            return ext, sz, out
        p += rl
    return None, None, out

def psx(fh, off):
    pvd = iso_pvd(fh, off)
    if not pvd:
        return None
    r = {"hdr": "psx", "volume": pvd["volume"], "created": pvd["created"],
         "publisher": pvd["publisher"], "serial": "", "boot": ""}
    ext, sz, names = iso_find(fh, off, pvd["root_lba"], pvd["root_len"], "SYSTEM.CNF")
    r["root_files"] = names[:24]
    if ext:
        cnf = sector(fh, off, ext)[:max(sz, 1)]
        r["boot"] = txt(cnf).replace("\r", " ").replace("\n", " ")
        for tok in r["boot"].replace("\\", " ").replace("/", " ").split():
            t = tok.split(";")[0]
            if len(t) >= 9 and t[4] in "_-." and t[:4].isalpha():
                r["serial"] = t.replace("_", "-").replace(".", "")
                break
    return r


def probe(path):
    try:
        with open(path, "rb") as fh:
            off = geom(fh)
            if off is None:
                return {"error": "too short"}
            for fn in (saturn, segacd, psx):
                try:
                    r = fn(fh, off)
                except Exception as e:
                    r = None
                if r:
                    r["geom"] = off
                    return r
            return {"error": "no recognised header", "geom": off}
    except Exception as e:
        return {"error": str(e)}


def data_track(d):
    """The data track of a dumped disc: track 1, whatever the naming style."""
    if os.path.isfile(d):
        return d
    best = None
    for f in sorted(os.listdir(d)):
        low = f.lower()
        if not low.endswith((".bin", ".iso", ".img")):
            continue
        if "track 01" in low or "track01" in low or "track1)" in low:
            return os.path.join(d, f)
        if best is None:
            best = os.path.join(d, f)
    return best


for arg in sys.argv[1:]:
    t = data_track(arg)
    rec = {"entry": os.path.basename(arg.rstrip("/")), "file": os.path.basename(t) if t else None}
    rec.update(probe(t) if t else {"error": "no data track"})
    print(json.dumps(rec, ensure_ascii=False))
