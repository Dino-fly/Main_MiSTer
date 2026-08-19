#!/usr/bin/env python3
"""
The whole MiSTer picture path, from a core's native frame to what the television
gets - so a look can be iterated without a capture dongle on the desk.

simulate_look.py models one stage, the scaler, and models it bit-exactly. This
wraps it in the stages either side, in the order the fabric actually runs them,
which is not the order the settings screen lists them and not the order I first
assumed. Read off sys_top.v in Template_MiSTer:

    core video
      -> video_mixer.sv   gamma_corr      a per-channel LUT, BEFORE the scaler
      -> ascal.vhd        the polyphase filter IS the scaler; the coefficient
                          file is its tap set, so anything the "filter" draws
                          happens here
      -> shadowmask.sv    a 2D cell table, per OUTPUT pixel, AFTER the scaler,
                          its counters anchored to brd_in - the active picture
                          border - which is why masks line up with the picture
                          rather than with the screen
      -> osd -> transmitter

Two consequences worth writing down, because both were guessed wrong before
being read:

  * Gamma runs before the multiplies. A source pixel of true black is only
    stuck at black if it is still black when it reaches ascal - a LUT with a
    lifted toe, or a palette whose darkest entry is not black, gives the
    multiplicative stages something to work with.
  * Nothing is programmable after the mask, so a filter-drawn shadow is always
    upstream of a mask-drawn grid. It does not matter: both stages multiply, so
    they commute up to rounding.

The palette is applied first of all, standing in for the core's own Custom
Palette slot, because that is where it happens on hardware - in the core's pixel
pipeline, before any of this.

Usage:
  simulate_pipeline.py src.png out.png --scale 7 [options]

    --palette F.gbp     16-byte .gbp (4 RGB triples); source is luma-ranked to 4
    --gamma FILE        256 lines of "r,g,b", applied before the scaler
    --filter FILE       ascal coefficients, both axes
    --hfilter/--vfilter per-axis, overriding --filter
    --mask FILE         shadow mask, applied after the scaler
    --maskmode 1x|2x
    --screen WxH        letterbox the result into a screen (default: none)
    --grid-mask N:LIFT  write a 2D grid mask for scale N to --mask's path and
                        use it; LIFT is the gap multiplier, e.g. 3:1.15
    --shadow N:MIX:DIM[:W]
                        write a shadow-only filter for scale N and use it; W is
                        the shadow's width in output pixels (default 1)
    --shadow-file PATH  where to write it
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from simulate_look import (read_png, write_png, load_filter, load_mask,
                           apply_mask, ascal_1d, synth_grid, synth_sharp, PHASES)

KR, KG, KB = 0.2126, 0.7152, 0.0722


def luma(c):
    return KR * c[0] + KG * c[1] + KB * c[2]


def load_gbp(path):
    """A .gbp is 16 bytes: four RGB triples, lightest first, then a 4-byte tag."""
    d = open(path, 'rb').read()
    if len(d) < 12:
        raise SystemExit(f"{path}: too short to be a .gbp")
    return [(d[i], d[i + 1], d[i + 2]) for i in range(0, 12, 3)]


def apply_palette(rows, pal):
    """What the core's Custom Palette slot does: four shades in, four out.

    Ranked by luma so a frame captured under one palette can be re-shown under
    another - which is the point of comparing palettes from a single capture.

    The ranking is over DISTINCT COLOURS, and the source must carry at most four
    of them. An earlier version bucketed rounded luma values instead, and lost a
    whole surface: Link's floor and a wall tile in the same frame both rounded to
    luma 103, so they collapsed into one shade and the floor stopped existing. A
    capture straight off the core is exactly four levels and cannot collide; a
    resampled screenshot or a savestate thumbnail is not, which is what made that
    bug reachable. Refusing the input is the fix, not a cleverer bucketing.
    """
    seen = sorted({px for row in rows for px in row}, key=luma, reverse=True)
    if len(seen) > 4:
        raise SystemExit(
            f"source has {len(seen)} distinct colours; a core's native frame has at "
            "most 4. This is a scaled or blended capture - grab the frame with "
            "'screenshot' on the device (pre-scaler), with the core's own palette, "
            "frame blend and screen shadow off.")
    if len(seen) < 4:
        print(f"  note: source uses only {len(seen)} of the 4 shades; mapping the "
              "lightest present to palette entry 0", file=sys.stderr)
    idx = {c: i for i, c in enumerate(seen)}
    return [[pal[idx[px]] for px in row] for row in rows]


def load_gamma(path):
    lut = []
    for line in open(path):
        line = line.split('#')[0].strip()
        if not line:
            continue
        p = line.split(',')
        if len(p) == 3:
            lut.append(tuple(max(0, min(255, int(v))) for v in p))
    if len(lut) != 256:
        raise SystemExit(f"{path}: expected 256 gamma entries, got {len(lut)}")
    return lut


def apply_gamma(rows, lut):
    return [[(lut[r][0], lut[g][1], lut[b][2]) for (r, g, b) in row] for row in rows]


def write_grid_mask(path, scale, lift):
    """A grid drawn as a mask instead of as filter taps.

    The filter cannot do this well for two reasons met on hardware: it runs on
    each axis separately, so the two gap lifts multiply and every intersection
    comes out as the square (1.18x lines, 1.39x crossings - visible as a bright
    dot at every corner); and it sits upstream of nothing, so it multiplies
    whatever the palette handed it.

    A mask is a 2D table, so the intersection cell is simply written with the
    same multiplier as the line cells and the corners disappear.

    v2 words: bits 10/9/8 select bright-or-dim per channel, bits 7:4 are the
    bright nibble as 1.0 + n/16, bits 3:0 the dim nibble as n/16.
    """
    # A gap above unity is the bright nibble (1 + n/16); a gap BELOW unity has to
    # switch the channels to the dim side instead (n/16), because the bright
    # nibble cannot go under 1.0. Clamping a dark gap into the bright encoding is
    # how the first version of this silently drew no grid at all on every colour
    # and backlit panel - 0.80 rounded to a bright nibble of 0, which is unity.
    # chome_video.cpp's write_grid_mask() has the same two branches; they must
    # keep agreeing.
    if lift >= 1.0:
        n = max(0, min(15, int(round((lift - 1.0) * 16))))
        gap = (7 << 8) | (n << 4)
    else:
        n = max(0, min(15, int(round(lift * 16))))
        gap = n                      # all three channels take the dim nibble
    body = (7 << 8)
    rows = []
    for y in range(scale):
        row = [gap if (x == 0 or y == 0) else body for x in range(scale)]
        rows.append(','.join('%03X' % v for v in row))
    with open(path, 'w') as f:
        f.write("v2\n%d,%d\n" % (scale, scale))
        f.write('\n'.join(rows) + '\n')
    return path


def write_shadow_filter(path, scale, mix, dim, width=1.0):
    """The pixel shadow, and only that - the grid is the mask's job now.

    A reflective pixel casts down and to the right, onto the gap and the leading
    edge of the next cell. That is a job for the filter rather than the mask,
    for the reason the core's own Screen Shadow shows by contrast: the core draws
    at 160x144, so its shadow darkens a whole Game Boy pixel uniformly - measured
    on hardware, all seven output samples of a cell dropping by the same amount.
    A filter tap set is evaluated per OUTPUT sample from the source pixels around
    it, so the same effect lands at screen resolution and can fade across a cell.

    `width` is in output pixels: the shadow starts at the cell's leading sample
    and decays linearly over that many. Mixing toward the previous tap is what
    makes it a shadow rather than a dimming - the darkness of the pixel that
    casts it is what arrives.
    """
    # One cell is the furthest a shadow can reach. ascal has four taps, spanning
    # i-1..i+2, so the sample the shadow mixes in is always the source pixel
    # immediately to the left - a pixel cannot cast past its own neighbour. Asking
    # for more silently starts mixing the WRONG pixel, so it is refused instead.
    if width > scale:
        raise SystemExit(f"shadow width {width} exceeds one cell ({scale} output px "
                         "at this scale); the 4-tap filter cannot reach further back")

    # Anchor the band on the LINE the hardware reads, not on the ideal phase.
    # ascal truncates the phase to one of PHASES lines, and at 7x the leading
    # sample wants 0.5714 while the line it actually reads is 36/64 = 0.5625 -
    # just before it. Anchoring on the fraction put the band's start a hair
    # above line 36, so the leading sample wrapped to the far end of the cell
    # and came out with no shadow at all: the gap stayed lit right beside the
    # pixel casting onto it.
    lines_for = [int((((k + 0.5) / scale - 0.5) % 1.0) * PHASES) for k in range(scale)]
    lead = lines_for[0] / PHASES

    def depth(x):
        t = ((x - lead) % 1.0) * scale
        return max(0.0, 1.0 - t / width) if t < width else 0.0

    mean_s = sum(depth(p / PHASES) for p in lines_for) / scale
    boost = 1.0 / (1.0 - dim * mean_s) if dim * mean_s < 0.9 else 1.0

    lines = []
    for p in range(PHASES):
        x = p / PHASES
        w = [0.0, 0.0, 0.0, 0.0]
        cur = 1 if x < 0.5 else 2
        w[cur] = 1.0
        gain = boost
        s = depth(x)
        if s > 0:
            w[cur - 1] += mix * s * w[cur]
            w[cur] -= mix * s * w[cur]
            gain *= 1.0 - dim * s
        c = []
        for t in range(4):
            e = w[t] * gain * 128.0
            v = int(e + 0.5) if e >= 0 else -int(-e + 0.5)
            c.append(max(-255, min(255, v)))
        lines.append("%4d,%4d,%4d,%4d" % tuple(c))
    with open(path, 'w') as f:
        f.write("# pixel shadow, %dx: mix %.2f dim %.2f over %.1f output px\n\n" %
                (scale, mix, dim, width))
        f.write('\n'.join(lines) + '\n')
    return path


def letterbox(rows, sw, sh, screen):
    W, H = screen
    out = [[(0, 0, 0)] * W for _ in range(H)]
    x0, y0 = (W - sw) // 2, (H - sh) // 2
    if x0 < 0 or y0 < 0:
        raise SystemExit(f"picture {sw}x{sh} does not fit in {W}x{H}")
    for y in range(sh):
        out[y0 + y][x0:x0 + sw] = rows[y]
    return out


def main():
    args, flags, pos = sys.argv[1:], {}, []
    i = 0
    while i < len(args):
        if args[i].startswith('--'):
            flags[args[i][2:]] = args[i + 1] if i + 1 < len(args) else '1'
            i += 2
        else:
            pos.append(args[i]); i += 1
    if len(pos) < 2:
        raise SystemExit(__doc__)
    inp, outp = pos[0], pos[1]
    scale = int(flags.get('scale', '3'))

    w, h, px = read_png(inp)
    rows = [[(px[(y * w + x) * 3], px[(y * w + x) * 3 + 1], px[(y * w + x) * 3 + 2])
             for x in range(w)] for y in range(h)]

    # 1. the core's palette slot
    if 'palette' in flags:
        rows = apply_palette(rows, load_gbp(flags['palette']))

    # 2. gamma_corr, before the scaler
    if 'gamma' in flags:
        rows = apply_gamma(rows, load_gamma(flags['gamma']))

    # 3. ascal, with the filter as its taps
    if 'grid-mask' in flags:
        n, lift = flags['grid-mask'].split(':')
        flags['mask'] = write_grid_mask(flags.get('mask', 'grid_mask.txt'),
                                        int(n), float(lift))
        flags.setdefault('maskmode', '1x')
    if 'shadow' in flags:
        parts = flags['shadow'].split(':')
        n, mix, dim = int(parts[0]), float(parts[1]), float(parts[2])
        wid = float(parts[3]) if len(parts) > 3 else 1.0
        flags['filter'] = write_shadow_filter(
            flags.get('shadow-file', 'shadow_filter.txt'), n, mix, dim, wid)
    hf = flags.get('hfilter', flags.get('filter'))
    vf = flags.get('vfilter', flags.get('filter'))
    ht = load_filter(hf) if hf and hf != 'off' else synth_sharp()
    vt = load_filter(vf) if vf and vf != 'off' else synth_sharp()
    dw, dh = w * scale, h * scale
    rows = ascal_1d(rows, w, dw, ht, 0)
    cols = [list(c) for c in zip(*rows)]
    cols = ascal_1d(cols, h, dh, vt, 0)
    rows = [list(c) for c in zip(*cols)]

    # 4. shadowmask, after the scaler, per output pixel
    if flags.get('mask') and flags['mask'] != 'off':
        rows = apply_mask(rows, load_mask(flags['mask']),
                          flags.get('maskmode', '1x') == '2x')

    if 'screen' in flags:
        sw, sh = (int(v) for v in flags['screen'].lower().split('x'))
        rows = letterbox(rows, dw, dh, (sw, sh))
        dw, dh = sw, sh

    out = bytearray(dw * dh * 3)
    for y in range(dh):
        for x in range(dw):
            r, g, b = rows[y][x]
            out[(y * dw + x) * 3:(y * dw + x) * 3 + 3] = bytes(
                (max(0, min(255, int(r))), max(0, min(255, int(g))), max(0, min(255, int(b)))))
    write_png(outp, dw, dh, out)
    print(f"{outp}: {dw}x{dh}")


if __name__ == '__main__':
    main()
