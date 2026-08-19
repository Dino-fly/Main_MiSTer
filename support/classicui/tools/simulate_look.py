#!/usr/bin/env python3
"""
Simulate the MiSTer scaler over a raw core capture - bit-exactly where the RTL
has been read, so a Display look can be judged (and lookshots authored) without
an HDMI grabber.

What is exact, and where it came from:

  * Coefficient quantisation: filter files carry integers at 128 scale, and
    video.cpp's read_video_filter() multiplies by 2 ("10bit" header: by 1) into
    the 256-scale 10-bit signed words ascal stores (poly_unpack, 2.8 fixed
    point, 256 = unity). 64-phase files are duplicated up to the 256 internal
    phases (scale_phases), not interpolated.
  * The MAC: ascal.vhd poly_cvt shifts each coefficient left 7 (3.15), each tap
    multiplies the 9-bit zero-extended pixel (4.23 products), taps sum IN PAIRS
    (t0+t1, t2+t3), each pair is truncated by 8 bits (poly_final takes bits
    26:8 - floor, no rounding), the two slices add, and bound() clamps: sign
    bit -> 0, any bit >= 15 -> 255, else bits 14:7 are the pixel. The two
    separate truncation points are why this cannot be modelled as one >>15.
  * The shadow mask: sys/shadowmask.sv. Each cell's LUT word gives a 1.4
    fixed-point multiplier per channel - bright cells {1,lut[7:4]}, dim cells
    {0,lut[3:0]} - applied as the same truncating shift-add the RTL uses
    (channel>>4 down to channel>>0, gated per multiplier bit). Our simple
    masks (one bit per channel) are 1.125 for a set bit and 0.625 for a clear
    one, because video.cpp fixes the low byte at 0x2A for the v1 form - the file
    never gets to say. 2x mode doubles the cell.

Measured against real hardware on 2026-08-19 - see
docs/SCALER-MODEL-2026-08-19.md for the rig, the numbers and the caveats. The
short version: the GEOMETRY is exact (the gutter lands on the same sub-pixel the
hardware puts it on, both axes, at 3x and 6x, and the model is 4.8x closer than
"no filter at all"), and the PHOTOMETRY is approximate - the gutter is about 3
luma levels too light at 3x and about 12 too dark at 6x, and the hardware's
gutter bleeds into its neighbours where this draws a hard notch.

What is NOT certified:

  * The source-position accumulator's initial phase (--phase-bias, in 1/256
    of a source pixel). The grid is symmetric, so being off by a phase moves
    the gutter a fraction of an output pixel - visible only side by side.

    CALIBRATION FAILED TO GENERALISE, which is itself the finding: the optimum
    is 222-224 at 3x and 0 at 6x, both sharp minima, and 223 is not congruent
    to 0 under the 256/N phase period. So this is not one hardware constant the
    model is missing - the initial phase varies with the ratio. Use the flag to
    author a lookshot at a known scale; do not treat any one value as "the"
    calibration.
  * Axis order: CONFIRMED H then V - ascal's horizontal stage consumes the
    input line (o_hpix0 <= hpix_v) and the vertical stage reads the H-scaled
    line buffers (o_vpixq), so this script's order is the RTL's.
  * sfilter (the scanline vfilter) and adaptive filters are not modelled.

Usage (old form still works):
  simulate_look.py in.png out.png scale [dmg|pocket|bgb|none] [grid|gridshadow|sharp]
  ... [--filter path.txt] [--mask path.txt --maskmode 1x|2x] [--phase-bias N]

--filter reads a real generated filter file and is preferred over the named
synthetic recomputes: the shipped artifact is the thing being previewed.
"""
import sys, struct, zlib, math

PHASES = 64          # our generated files
HW_PHASES = 256      # ascal FRAC=8
GRID_GUTTER = 0.22
GRID_DEPTH = 0.42
SHADOW_WIDTH = 0.30
SHADOW_MIX = 0.35
SHADOW_DIM = 0.10

# chome_video.cpp pal_dmg / pal_pocket - keep in step. 'bgb' kept for A/B.
DMG    = [(0xC4,0xCF,0xA1),(0x8B,0x95,0x6D),(0x4D,0x53,0x3C),(0x1F,0x1F,0x1F)]
BGB    = [(0xE0,0xF8,0xD0),(0x88,0xC0,0x70),(0x34,0x68,0x56),(0x08,0x18,0x20)]
POCKET = [(0xE0,0xDB,0xCD),(0xA8,0x9F,0x94),(0x70,0x6B,0x66),(0x2B,0x2B,0x26)]

def read_png(path):
    data = open(path,'rb').read()
    assert data[:8] == b'\x89PNG\r\n\x1a\n'
    pos, w, h, bd, ct, raw, plte = 8, 0, 0, 0, 0, b'', b''
    while pos < len(data):
        ln = struct.unpack('>I', data[pos:pos+4])[0]; typ = data[pos+4:pos+8]
        body = data[pos+8:pos+8+ln]; pos += 12+ln
        if typ == b'IHDR': w,h,bd,ct = struct.unpack('>IIBB', body[:10])
        elif typ == b'PLTE': plte = body
        elif typ == b'IDAT': raw += body
        elif typ == b'IEND': break
    raw = zlib.decompress(raw)
    ch = {0:1, 2:3, 3:1, 6:4}[ct]
    assert bd == 8, f"bit depth {bd}"
    stride = w*ch
    px = bytearray(w*h*3)
    prev = bytearray(stride)
    pos = 0
    for y in range(h):
        f = raw[pos]; pos += 1
        line = bytearray(raw[pos:pos+stride]); pos += stride
        for i in range(stride):
            a = line[i-ch] if i >= ch else 0
            b = prev[i]
            c = prev[i-ch] if i >= ch else 0
            if f == 1: line[i] = (line[i]+a) & 255
            elif f == 2: line[i] = (line[i]+b) & 255
            elif f == 3: line[i] = (line[i]+(a+b)//2) & 255
            elif f == 4:
                p = a+b-c
                pa,pb,pc = abs(p-a),abs(p-b),abs(p-c)
                pr = a if (pa<=pb and pa<=pc) else (b if pb<=pc else c)
                line[i] = (line[i]+pr) & 255
        prev = line
        for x in range(w):
            if ct == 3:
                idx = line[x]; px[(y*w+x)*3:(y*w+x)*3+3] = plte[idx*3:idx*3+3]
            elif ch == 1:
                v = line[x]; px[(y*w+x)*3:(y*w+x)*3+3] = bytes((v,v,v))
            else:
                px[(y*w+x)*3:(y*w+x)*3+3] = line[x*ch:x*ch+3]
    return w,h,px

def write_png(path,w,h,px):
    def chunk(t,d): return struct.pack('>I',len(d))+t+d+struct.pack('>I',zlib.crc32(t+d)&0xffffffff)
    rows = b''.join(b'\x00'+bytes(px[y*w*3:(y+1)*w*3]) for y in range(h))
    open(path,'wb').write(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(rows,6))+chunk(b'IEND',b''))

# ---------------------------------------------------------------- filters ---

def to_hw_phases(taps):
    """video.cpp scale_phases: duplicate entries up to 256, never interpolate."""
    dup = HW_PHASES // len(taps)
    assert dup * len(taps) == HW_PHASES, f"{len(taps)} phases will not divide 256"
    out = []
    for t in taps: out += [t]*dup
    return out

def load_filter(path):
    """A real filter file, quantised the way read_video_filter() does."""
    scale, taps = 2, []
    for line in open(path):
        line = line.strip()
        if not taps and line.lower() == '10bit': scale = 1; continue
        if not taps and line.lower() == 'adaptive':
            raise SystemExit("adaptive filters are not modelled")
        parts = line.split(',')
        if len(parts) == 4:
            try: v = [int(p) for p in parts]
            except ValueError: continue
            taps.append([x*scale for x in v])
    if len(taps) == 32: taps = taps[:16]     # legacy 16-phase pairs
    return to_hw_phases(taps)

def synth_grid(shadow):
    """chome_video.cpp write_filter_grid, including its integer commit."""
    half = GRID_GUTTER/2; soft = 1.0/PHASES
    boost = min(1.0/(1.0-GRID_DEPTH*GRID_GUTTER), 1.12)
    taps = []
    for p in range(PHASES):
        x = p/PHASES; d = abs(x-0.5)
        e = 1.0 if d < half else (1.0-(d-half)/soft if d < half+soft else 0.0)
        if x < 0.5-half: w1,w2 = 1.0,0.0
        elif x > 0.5+half: w1,w2 = 0.0,1.0
        else:
            t = (x-(0.5-half))/GRID_GUTTER; w1,w2 = 1.0-t,t
        gain = boost*(1.0-GRID_DEPTH*e)
        if shadow and x > 0.5+half:
            into = (x-(0.5+half))/(1.0-(0.5+half))
            if into < SHADOW_WIDTH:
                f = 1.0-into/SHADOW_WIDTH
                w1 += SHADOW_MIX*f*w2
                w2 -= SHADOW_MIX*f*w2
                gain *= 1.0-SHADOW_DIM*f
        taps.append([0, round(w1*gain*128)*2, round(w2*gain*128)*2, 0])
    return to_hw_phases(taps)

def synth_sharp():
    return to_hw_phases([[0,256,0,0] if p/PHASES < 0.5 else [0,0,256,0] for p in range(PHASES)])

# ------------------------------------------------------ the ascal pipeline ---

def bound(v):
    if v < 0: return 0
    if v >= (1 << 15): return 255
    return (v >> 7) & 0xFF

def ascal_1d(rows, sw, dw, hw_taps, phase_bias):
    """One axis of ascal's polyphase, bit-exact per the notes at the top."""
    plan = []
    for x in range(dw):
        u = (x+0.5)*sw/dw - 0.5
        i = math.floor(u)
        ph = (int((u-i)*HW_PHASES) + phase_bias) % HW_PHASES
        c = [t << 7 for t in hw_taps[ph]]          # poly_cvt: 2.8 -> 3.15
        idx = [min(max(i-1+t,0),sw-1) for t in range(4)]
        plan.append((idx, c))
    out = []
    for row in rows:
        orow = []
        for idx, c in plan:
            px = [row[j] for j in idx]
            o = []
            for k in range(3):
                pair0 = c[0]*px[0][k] + c[1]*px[1][k]     # 4.23
                pair1 = c[2]*px[2][k] + c[3]*px[3][k]
                o.append(bound((pair0 >> 8) + (pair1 >> 8)))  # two truncations, then clamp
            orow.append(tuple(o))
        out.append(orow)
    return out

# ------------------------------------------------------------------ masks ---

def load_mask(path):
    """w,h then h rows of hex words. Returns per-cell (rm,gm,bm) 1.4 multipliers.

    Only the FIRST table is read. Several distribution masks carry a second one
    under a "Resolution=" line for taller modes, and reading on past it produced
    rows of two different widths in one grid - which then indexed off the end of a
    row. setShadowMask() in video.cpp picks a table by output height; the preview
    and the tools want the one the file leads with.
    """
    dims, cells = None, []
    for line in open(path):
        line = line.split('#')[0].split(';')[0].strip()
        if not line: continue
        if line.lower().startswith('resolution='):
            if cells: break                      # the next table is for another mode
            continue
        if line.lower() == 'v2': continue
        if dims is None and ',' in line:
            w,h = line.split(','); dims = (int(w),int(h)); continue
        row = []
        for tok in line.split(','):
            v = int(tok.strip(), 16)
            if v <= 7:
                # simple mask: one bit per channel, full on or black
                # video.cpp ORs a constant 0x2A into the low byte for v1 cells, so a
                # set bit is {1,2} = 1.125 and a clear bit is {0,A} = 0.625. NOT a
                # switch - measured on hardware, see docs/SCALER-MODEL-2026-08-19.md.
                m = tuple(0x12 if (v>>b)&1 else 0x0A for b in (2,1,0))
            else:
                # v2 LUT word per shadowmask.sv: bits 10/9/8 pick bright/dim nibble
                m = tuple((0x10 | ((v>>4)&0xF)) if (v>>bit)&1 else (v&0xF)
                          for bit in (10,9,8))
            row.append(m)
        if row: cells.append(row)
        if dims and len(cells) >= dims[1]: break

    # Ragged rows would index off the end later; a short one is a truncated line.
    if cells:
        wide = min(len(r) for r in cells)
        cells = [r[:wide] for r in cells if len(r) >= wide]
    return cells

def mask_mul(ch, mul):
    """shadowmask.sv's truncating shift-add: sum of ch>>(4-k) for set bits."""
    s = 0
    for k in range(5):
        if (mul >> k) & 1: s += ch >> (4-k)
    return min(s, 255)

def apply_mask(rows, cells, twox):
    mh, mw = len(cells), len(cells[0])
    step = 2 if twox else 1
    for y, row in enumerate(rows):
        crow = cells[(y//step) % mh]
        for x, (r,g,b) in enumerate(row):
            rm,gm,bm = crow[(x//step) % mw]
            row[x] = (mask_mul(r,rm), mask_mul(g,gm), mask_mul(b,bm))
    return rows

# ------------------------------------------------------------------- main ---

def main():
    args = sys.argv[1:]
    flags, pos = {}, []
    i = 0
    while i < len(args):
        if args[i].startswith('--'):
            flags[args[i][2:]] = args[i+1]; i += 2
        else:
            pos.append(args[i]); i += 1

    inp, outp, scale = pos[0], pos[1], int(pos[2])
    pal = pos[3] if len(pos) > 3 else 'none'
    filt = pos[4] if len(pos) > 4 else 'grid'

    w,h,px = read_png(inp)
    rows = [[tuple(px[(y*w+x)*3:(y*w+x)*3+3]) for x in range(w)] for y in range(h)]

    if pal in ('dmg','pocket','bgb'):
        palette = {'dmg':DMG,'bgb':BGB,'pocket':POCKET}[pal]
        lumas = sorted({(r*299+g*587+b*114)//1000 for row in rows for (r,g,b) in row}, reverse=True)
        lv = {y: min(int(i*4/max(len(lumas),1)),3) for i,y in enumerate(lumas)}
        if len(lumas) <= 4:
            lv = {y:i for i,y in enumerate(lumas)}
        rows = [[palette[lv[(r*299+g*587+b*114)//1000]] for (r,g,b) in row] for row in rows]

    if 'filter' in flags: taps = load_filter(flags['filter'])
    elif filt == 'grid': taps = synth_grid(0)
    elif filt == 'gridshadow': taps = synth_grid(1)
    else: taps = synth_sharp()

    bias = int(flags.get('phase-bias', '0'))
    dw, dh = w*scale, h*scale
    rows = ascal_1d(rows, w, dw, taps, bias)                    # horizontal
    cols = [[rows[y][x] for y in range(h)] for x in range(dw)]  # transpose
    cols = ascal_1d(cols, h, dh, taps, bias)                    # vertical
    rows = [[cols[x][y] for x in range(dw)] for y in range(dh)] # transpose back

    if 'mask' in flags:
        rows = apply_mask(rows, load_mask(flags['mask']), flags.get('maskmode','1x') == '2x')

    out = bytearray(dw*dh*3)
    for y in range(dh):
        for x in range(dw):
            r,g,b = rows[y][x]
            out[(y*dw+x)*3:(y*dw+x)*3+3] = bytes((r,g,b))
    write_png(outp, dw, dh, out)
    print(f"{outp}: {dw}x{dh}")

if __name__ == '__main__':
    main()
