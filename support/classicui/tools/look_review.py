#!/usr/bin/env python3
"""
Cut a simulated 1080p frame into the two views the look-review pane shows: a
1:1 crop of the screen and a 10x nearest-neighbour zoom of a small patch, so a
grid one output pixel wide survives to the eye (classicui-artifact-screenshots).

Usage:
  look_review.py screen.png outdir [--center X,Y] [--zoom-center X,Y]

screen.png is simulate_pipeline.py's output with --screen 1920x1080. Both
centres are in screen pixels and default to the middle of the picture.

Writes, in outdir:
  full.png, zoom.png     for the terminal (kitty graphics in Ghostty). Their
                         sizes are whole multiples of the terminal cell, read
                         from ~/.claude/look-review/cell.json (the pane's
                         /look-review cell command writes it), so the cell box
                         holds the picture exactly and nothing is resampled.
  full-d.png, zoom-d.png for Claude Desktop, which draws them inside an SVG
                         whose source is capped at 131072 characters: a smaller
                         crop, shrunk until the base64 fits.
Prints one JSON line with the paths and sizes, the show_look tool's input.
"""

import sys, os, json, base64

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from simulate_look import read_png, write_png

ZOOM = 10
DESK_BUDGET = 120000          # base64 chars: the SVG cap less room for the markup

def cell_size():
    try:
        with open(os.path.expanduser('~/.claude/look-review/cell.json')) as f:
            c = json.load(f)
        return int(c['w']), int(c['h'])
    except (OSError, ValueError, KeyError):
        return 16, 34

def crop(px, sw, sh, cx, cy, w, h):
    w, h = min(w, sw), min(h, sh)
    x0 = max(0, min(sw - w, cx - w // 2))
    y0 = max(0, min(sh - h, cy - h // 2))
    out = bytearray()
    for y in range(y0, y0 + h):
        out += px[(y * sw + x0) * 3:(y * sw + x0 + w) * 3]
    return out, w, h

def magnify(px, w, h, n):
    out = bytearray()
    for y in range(h):
        row = px[y * w * 3:(y + 1) * w * 3]
        big = b''.join(bytes(row[x * 3:x * 3 + 3]) * n for x in range(w))
        out += big * n
    return out, w * n, h * n

def save(path, px, w, h):
    write_png(path, w, h, px)
    return {'path': os.path.abspath(path), 'w': w, 'h': h}

def picture_centre(px, sw, sh):
    """Middle of the non-black extent: where the letterboxed picture is."""
    def lit(x, y): return px[(y * sw + x) * 3:(y * sw + x) * 3 + 3] != b'\0\0\0'
    ys = [y for y in range(0, sh, 2) if any(lit(x, y) for x in range(0, sw, 4))]
    xs = [x for x in range(0, sw, 2) if any(lit(x, y) for y in ys[::4])] if ys else []
    if not xs:
        return sw // 2, sh // 2
    return (xs[0] + xs[-1]) // 2, (ys[0] + ys[-1]) // 2

def fit(limit, cell):
    """Largest source patch whose x10 magnification is whole cells."""
    p = limit // ZOOM
    while p > 1 and (p * ZOOM) % cell:
        p -= 1
    return p

def make_views(px, sw, sh, outdir, center=None, zoom_center=None):
    """Write the four crops for one rendered 1080p frame; return the show_look dict.

    Split out of main() so render_looks.py can cut a frame it already holds in
    memory instead of writing a PNG and parsing it back, which costs seconds per
    look in pure Python. main() is a thin argv wrapper around this.
    """
    cx, cy = center if center else picture_centre(px, sw, sh)
    zx, zy = zoom_center if zoom_center else (cx, cy)
    os.makedirs(outdir, exist_ok=True)

    # Terminal: up to 960x540, trimmed to whole cells so the box is exact.
    cw, ch = cell_size()
    fw, fh = (960 // cw) * cw, (540 // ch) * ch
    res = {'full': save(os.path.join(outdir, 'full.png'), *crop(px, sw, sh, cx, cy, fw, fh)),
           'cell': {'w': cw, 'h': ch}}
    p, pw, ph = crop(px, sw, sh, zx, zy, fit(fw, cw), fit(fh, ch))
    res['zoom'] = save(os.path.join(outdir, 'zoom.png'), *magnify(p, pw, ph, ZOOM))

    # Desktop: shrink the crop until the base64 fits under the SVG source cap.
    for w, h in ((480, 270), (400, 220), (320, 180), (240, 130)):
        a = save(os.path.join(outdir, 'full-d.png'), *crop(px, sw, sh, cx, cy, w, h))
        p, pw, ph = crop(px, sw, sh, zx, zy, w // ZOOM, h // ZOOM)
        b = save(os.path.join(outdir, 'zoom-d.png'), *magnify(p, pw, ph, ZOOM))
        if max(len(base64.b64encode(open(x['path'], 'rb').read())) for x in (a, b)) <= DESK_BUDGET:
            res['full_desktop'], res['zoom_desktop'] = a, b
            break
    return res

def main():
    args = sys.argv[1:]
    if len(args) < 2 or args[0] in ('-h', '--help'):
        print(__doc__); sys.exit(0 if args else 1)
    src, outdir = args[0], args[1]
    opts = dict(zip(args[2::2], args[3::2]))
    sw, sh, px = read_png(src)
    center = tuple(map(int, opts['--center'].split(','))) if '--center' in opts else None
    zoom = tuple(map(int, opts['--zoom-center'].split(','))) if '--zoom-center' in opts else None
    print(json.dumps(make_views(px, sw, sh, outdir, center, zoom)))

if __name__ == '__main__':
    main()
