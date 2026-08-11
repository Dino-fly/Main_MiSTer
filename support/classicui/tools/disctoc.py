#!/usr/bin/env python3
"""TOC + boot-area strings for the disc in the drive. Read-only, no writes."""
import fcntl, os, struct, sys

CDROMREADTOCHDR   = 0x5305
CDROMREADTOCENTRY = 0x5306
CDROM_LBA = 0x01

fd = os.open('/dev/sr0', os.O_RDONLY | os.O_NONBLOCK)

hdr = fcntl.ioctl(fd, CDROMREADTOCHDR, struct.pack('BB', 0, 0))
first, last = struct.unpack('BB', hdr)
print("tracks %d..%d" % (first, last))

# struct cdrom_tocentry: track, adr:4|ctrl:4, format, <pad>, addr(4), datamode, <pad*3>
FMT = '<BBBxIB3x'
entries = []
for t in list(range(first, last + 1)) + [0xAA]:
    buf = struct.pack(FMT, t, 0, CDROM_LBA, 0, 0)
    try:
        out = fcntl.ioctl(fd, CDROMREADTOCENTRY, buf)
    except OSError as e:
        print("track %d: ioctl failed %s" % (t, e))
        continue
    trk, adrctrl, fmt, lba, dmode = struct.unpack(FMT, out)
    ctrl = (adrctrl >> 4) & 0xF
    kind = "DATA" if (ctrl & 0x4) else "AUDIO"
    name = "leadout" if t == 0xAA else "track %d" % t
    print("%-9s lba %8d  ctrl %x %s" % (name, lba, ctrl, kind))
    entries.append((t, lba, kind))

# The first data track, which is where a boot header would be.
data = [e for e in entries if e[2] == "DATA" and e[0] != 0xAA]
if not data:
    print("no data track")
    sys.exit(0)

dt, dlba, _ = data[0]
print("\nfirst data track: %d at lba %d" % (dt, dlba))

# Raw 2352 sectors from the data track's start. Read via plain file I/O at the
# cooked offset first; if that fails the track is not MODE1 addressable that way.
for off in (0, 1, 2):
    try:
        os.lseek(fd, (dlba + off) * 2048, os.SEEK_SET)
        blk = os.read(fd, 2048)
    except OSError as e:
        print("sector %d: read failed %s" % (dlba + off, e))
        continue
    if not blk:
        print("sector %d: empty" % (dlba + off))
        continue
    print("\n--- sector %d, first 128 bytes" % (dlba + off))
    print(" ".join("%02x" % b for b in blk[:128]))
    txt = "".join(chr(b) if 32 <= b < 127 else "." for b in blk)
    print("--- printable runs >= 4")
    run = ""
    for ch in txt:
        if ch != ".":
            run += ch
        else:
            if len(run) >= 4:
                print("   %r" % run)
            run = ""
    if len(run) >= 4:
        print("   %r" % run)

os.close(fd)
