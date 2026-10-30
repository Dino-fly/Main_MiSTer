#!/usr/bin/env python3
"""
Render every handheld look from a native frame, through the files the FIRMWARE
generated - not through a Python restatement of them.

Why it exists: the lookshots were hand-drawn sketches (gen_lookshots.py), and the
two earlier previews that re-stated the filter and mask tables in Python both
drifted from the device (the 4x grid that vanished, the SUB_BLEND the simulator
read in the wrong unit). So the inputs here are exactly what the card would hold:
filters, shadow masks, gamma LUTs, .gbp palettes and the preset rows, all written
by chome_video.cpp's own vp_install() and read back from disk. Only the arithmetic
(ascal, shadowmask, gamma) is Python, and that is simulate_look.py /
simulate_pipeline.py, which are pinned to the hardware.

How the generated files are obtained
  The host harness has a mode for it (gen_looks() in test/harness.cpp). From the
  repo root:

      CHOME_GEN_LOOKS=1 bash support/classicui/test/run.sh

  compiles the real chome_*.cpp, runs vp_install() under harness_set_scale() for
  every output scale 2x..10x - the same call sequence the firmware makes when it
  reads the scaler header - into one fake card root per scale, and writes a
  manifest of the preset table, to support/classicui/test/out/looks-gen/. The
  manifest carries each look's id, class, palette (found by applying the look
  against a Game Boy CONF_STR and watching which .gbp is pushed, since the table
  has no accessor), and its core-side options. `--generate` here runs that for you.

Usage
  render_looks.py [--out DIR] [--looks-dir DIR] [--corpus DIR] [--only ID,ID]
                  [--frames N] [--720] [--no-metrics] [--generate]
                  [--lookshots DIR]

  --out        where <look-id>/ directories go (default ./looks-out)
  --looks-dir  the harness output (default test/out/looks-gen)
  --corpus     tools/corpus by default: <system>/*.png native frames. A missing
               directory or system falls back to the committed test frames:
               test/frames/zelda_native_gb.png (4 colours, GB) fitted to each
               class's native size, and the GBA device captures in docs/img.
  --frames N   render the first N frames of each class (default 1); the first is
               screen.png, the rest screen-<frame>.png
  --720        also render 1280x720 into <look-id>/720p/
  --lookshots  write DIR/<id>.png, 320x240 macro crops in gen_lookshots.py's
               format, from the model, plus DIR/model-render.json marking them

Writes, per look:
  <out>/<look-id>/screen.png        1920x1080, the picture letterboxed
  <out>/<look-id>/full.png ...      look_review.py's crops (feed show_look)
  <out>/<look-id>/meta.json         scale, the files used (with md5), what is not
                                    simulated, and look_metrics.py's results
A look id shared by two classes (ws is offered on both WonderSwan screens) and the
per-class None looks get <id>.<class> as the directory name.
"""

import sys, os, json, glob, math, hashlib, subprocess, itertools

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from simulate_look import (read_png, write_png, load_filter, load_mask, apply_mask,
                           ascal_1d, synth_sharp, axis_taps)
from gba_colorshade import apply_modify_colors
from simulate_pipeline import (load_gbp, apply_palette, load_gamma, apply_gamma,
                               letterbox)
import look_review

CLASSIC = os.path.dirname(HERE)
GEN_DEFAULT = os.path.join(CLASSIC, 'test', 'out', 'looks-gen')
CORPUS_DEFAULT = os.path.join(HERE, 'corpus')
FRAMES = os.path.join(CLASSIC, 'test', 'frames')
DEVICE_IMG = os.path.join(CLASSIC, 'docs', 'img', 'device')

SCREENS = {'1080p': (1920, 1080), '720p': (1280, 720)}

# What each class's core emits natively. Corpus frames carry their own size and
# are rendered at whatever scale THAT size gets; this table is only what the
# fallback test frame is fitted to, so the 10x Lynx and 7x NGP paths still run.
NATIVE = {'gb': (160, 144), 'gbc': (160, 144), 'gg': (160, 144), 'gba': (240, 160),
          'lynx': (160, 102), 'ws': (224, 144), 'wsc': (224, 144), 'ngpc': (160, 152),
          'ngp': (160, 152)}

# Corpus directory names to try per class, first hit wins.
CORPUS_NAMES = {
    'gb': ['gb', 'gameboy', 'game boy'], 'gbc': ['gbc', 'gameboycolor', 'game boy color'],
    'gba': ['gba', 'gameboyadvance'], 'gg': ['gg', 'gamegear', 'game gear'],
    'lynx': ['lynx', 'atarilynx'], 'ws': ['ws', 'wonderswan'],
    'wsc': ['wsc', 'wonderswancolor'], 'ngpc': ['ngpc', 'neogeopocketcolor', 'ngp'],
    # Mono carts: no native mono frame in the corpus yet, so the NGPC frames stand in.
    'ngp': ['ngp-mono', 'ngpc', 'ngp'],
}

# Not handheld panels: the Super Game Boy is a Super Nintendo on a television
# (CRT treatment, border canvas) and is judged with the CRT looks.
SKIP_IDS = {'sgb', 'none-sgb'}

# core-side options the renderer DOES honour; everything else is reported.
SIMULATED_OPTS = {'Custom Palette': 'the .gbp is applied as the palette',
                  'Scale': 'integer scale is how the frame is rendered',
                  'Modify Colors': 'gba_colorshade.py, the core\'s own tables, over a raw frame',
                  'GBC Colors': 'Corrected is what routes the frame through the .lut'}


# ---------------------------------------------------------------- the looks --

class Look:
    """One preset row plus where its generated files live."""

    def __init__(self, cls, lid, name, preset_rel, palette_rel, core_colour, core_struct):
        self.cls, self.id, self.name = cls, lid, name
        self.preset_rel, self.palette_rel = preset_rel, palette_rel
        self.core_colour, self.core_struct = core_colour, core_struct
        self.dirname = lid

    def not_simulated(self):
        out = []
        for lst in (self.core_colour, self.core_struct):
            for pair in filter(None, lst.split(';')):
                if pair.split('=', 1)[0] not in SIMULATED_OPTS:
                    out.append(pair)
        return out


def load_looks(gen_dir):
    manifest = os.path.join(gen_dir, 'looks.tsv')
    if not os.path.exists(manifest):
        raise SystemExit("%s missing: run  CHOME_GEN_LOOKS=1 bash support/classicui/"
                         "test/run.sh  (or pass --generate)" % manifest)
    looks = []
    for line in open(manifest):
        f = line.rstrip('\n').split('\t')
        if len(f) < 7 or f[1] in SKIP_IDS:
            continue
        looks.append(Look(*f[:7]))
    # Directory names: bare id when it is unique, <id>.<class> otherwise.
    count = {}
    for lk in looks:
        count[lk.id] = count.get(lk.id, 0) + 1
    for lk in looks:
        if count[lk.id] > 1:
            lk.dirname = '%s.%s' % (lk.id, lk.cls)
    return looks


def card_for(gen_dir, scale):
    d = os.path.join(gen_dir, '%dx' % scale)
    if not os.path.isdir(d):
        raise SystemExit("no generated files for %dx in %s (the harness writes 2x-10x)"
                         % (scale, gen_dir))
    return d


def read_preset(card, look):
    """The preset file the firmware wrote: key=value lines."""
    p = {}
    for line in open(os.path.join(card, look.preset_rel), encoding='utf-8'):
        line = line.strip()
        if line and not line.startswith('#') and '=' in line:
            k, v = line.split('=', 1)
            p[k.strip()] = v.strip()
    return p


def md5(path):
    return hashlib.md5(open(path, 'rb').read()).hexdigest()


# --------------------------------------------------------------- the frames --

def fit_frame(w, h, px, tw, th):
    """Centre-crop or edge-pad a frame to tw x th: the fallback for classes whose
    corpus has not arrived, so every scale path still gets exercised. Padding
    repeats the edge pixel, which keeps a 4-colour frame at 4 colours."""
    rows = [[tuple(px[(y * w + x) * 3:(y * w + x) * 3 + 3]) for x in range(w)] for y in range(h)]
    out = []
    for y in range(th):
        sy = min(max(y + (h - th) // 2, 0), h - 1)
        out.append([rows[sy][min(max(x + (w - tw) // 2, 0), w - 1)] for x in range(tw)])
    return out


def to_rows(w, h, px):
    return [[tuple(px[(y * w + x) * 3:(y * w + x) * 3 + 3]) for x in range(w)] for y in range(h)]


def fallback_path(look):
    """The committed test frame for a look when the corpus has none."""
    if look.cls == 'gba':
        mc = dict(p.split('=', 1) for p in look.core_colour.split(';') if '=' in p).get('Modify Colors', '')
        name = 'look-gba-agb001-ingame.png' if mc == 'GBA 2.2' else 'look-gba-ags101-ingame.png'
        return os.path.join(DEVICE_IMG, name)
    return os.path.join(FRAMES, 'zelda_native_gb.png')


def frames_for(look, corpus, count):
    """[(name, rows, source-note)] for a look: corpus first, else the test frame."""
    names = CORPUS_NAMES.get(look.cls, [look.cls])
    for n in names:
        files = sorted(glob.glob(os.path.join(corpus, n, '*.png')))
        if files:
            out = []
            for f in files:
                w, h, px = read_png(f)
                rows = to_rows(w, h, px)
                # A palette look re-ranks the frame's colours into four shades, so
                # it needs a frame straight off the core with at most four. A
                # blended or colourised capture is skipped, not mangled.
                if look.palette_rel and look.palette_rel.endswith('.gbp') \
                        and len({p for r in rows for p in r}) > 4:
                    print('  skip %s for %s: more than 4 colours' % (os.path.basename(f), look.id),
                          file=sys.stderr)
                    continue
                out.append((os.path.splitext(os.path.basename(f))[0], rows,
                            'corpus/%s/%s' % (n, os.path.basename(f))))
                if len(out) >= count:
                    break
            if out:
                return out
    f = fallback_path(look)
    w, h, px = read_png(f)
    if look.cls == 'gba':
        rows, note = to_rows(w, h, px), 'test frame %s' % os.path.basename(f)
    else:
        tw, th = NATIVE[look.cls]
        rows = fit_frame(w, h, px, tw, th)
        note = 'test frame %s fitted to %dx%d' % (os.path.basename(f), tw, th)
    return [(os.path.splitext(os.path.basename(f))[0], rows, note)]


def scale_for(w, h, screen):
    """The integer scale a 1080p/720p set would run this frame at."""
    return max(1, min(screen[1] // h, screen[0] // w))


# ------------------------------------------------------------------ render --

def nearest(rows, n):
    out = []
    for r in rows:
        big = [p for p in r for _ in range(n)]
        out.extend([big] * n)
    return out


def load_lut(path):
    """The Game Boy core's colour LUT: 32768 (R,G,B), indexed {r5,g5,b5}."""
    d = open(path, 'rb').read()
    if len(d) != 32768 * 3:
        raise SystemExit('%s: %d bytes, not a GBC LUT' % (path, len(d)))
    return [tuple(d[i * 3:i * 3 + 3]) for i in range(32768)]

def invert_lut(lut):
    inv = {}
    for i, c in enumerate(lut):
        inv.setdefault(c, i)
    return inv

def apply_lut(rows, inv, lut):
    out, miss = [], set()
    for r in rows:
        o = []
        for p in r:
            i = inv.get(tuple(p))
            if i is None:
                miss.add(tuple(p))
                i = (p[0] >> 3) << 10 | (p[1] >> 3) << 5 | (p[2] >> 3)
            o.append(lut[i])
        out.append(o)
    if miss:
        print('  %d colours not produced by the stock table; top bits used' % len(miss), file=sys.stderr)
    return out

def render_look(look, gen_dir, src_rows, scale):
    """Run one look's generated files over a native frame.

    Returns a dict: out (scale-sized rows), src (the frame after palette and
    gamma, i.e. what reaches the scaler), nearest (that frame unfiltered), card,
    preset, files (role -> {path, md5}), mask (the loaded cells or None).
    """
    card = card_for(gen_dir, scale)
    pre = read_preset(card, look)
    files = {}

    def use(role, sub, name):
        p = os.path.join(card, sub, name)
        if not os.path.exists(p):
            raise SystemExit("%s: preset names %s %r but the harness did not generate it"
                             % (look.id, role, name))
        files[role] = {'path': os.path.relpath(p, gen_dir), 'md5': md5(p)}
        return p

    h, w = len(src_rows), len(src_rows[0])
    rows = src_rows
    if look.palette_rel and look.palette_rel.endswith('.lut'):
        # A GBC frame in the corpus was captured through the core's stock table;
        # map it back to 15 bits through that table (generated alongside, and
        # pinned to the core's own file by the harness), then through the look's.
        lut = os.path.join(card, look.palette_rel)
        files['lut'] = {'path': os.path.relpath(lut, gen_dir), 'md5': md5(lut)}
        stock = load_lut(os.path.join(os.path.dirname(lut), 'ClassicHome GBC Corrected.lut'))
        rows = apply_lut(rows, invert_lut(stock), load_lut(lut))
    elif look.cls == 'gba':
        # The corpus GBA frames are raw (Modify Colors=Off); a profile is the core's
        # own arithmetic over them.
        mc = dict(p.split('=', 1) for p in (look.core_colour or '').split(';') if '=' in p)
        rows = apply_modify_colors(rows, mc.get('Modify Colors', 'Off'))
    elif look.palette_rel:
        pal = os.path.join(card, look.palette_rel)
        files['palette'] = {'path': os.path.relpath(pal, gen_dir), 'md5': md5(pal)}
        rows = apply_palette(rows, load_gbp(pal))
    g = pre.get('gamma', 'off')
    if g != 'off':
        rows = apply_gamma(rows, load_gamma(use('gamma', 'gamma', g)))
    post = rows

    def flt(key):
        v = pre.get(key, 'off')
        return use(key, 'filters', v) if v.lower() not in ('off', 'same') else None

    # No look turns a core's own scanline option on, so the sfilter never applies.
    ht, vt = axis_taps(flt('hfilter'), flt('vfilter'))
    dw, dh = w * scale, h * scale
    rows = ascal_1d(post, w, dw, ht, 0)
    cols = [list(c) for c in zip(*rows)]
    cols = ascal_1d(cols, h, dh, vt, 0)
    rows = [list(c) for c in zip(*cols)]

    cells = None
    m = pre.get('mask', 'off')
    if m != 'off':
        mp = use('mask', 'shadow_masks', m)
        cells = load_mask(mp)
        rows = apply_mask(rows, cells, pre.get('maskmode', '1x') == '2x')
    files['preset'] = {'path': os.path.relpath(os.path.join(card, look.preset_rel), gen_dir),
                       'md5': md5(os.path.join(card, look.preset_rel))}
    return {'out': rows, 'src': post, 'nearest': nearest(post, scale), 'card': card,
            'preset': pre, 'files': files, 'cells': cells, 'scale': scale}


def flatten(rows, w, h, screen):
    """Letterbox into the screen and return packed RGB bytes."""
    sw, sh = screen
    full = letterbox(rows, w, h, screen)
    return b''.join(bytes(itertools.chain.from_iterable(r)) for r in full)


# ------------------------------------------------------------- lookshots ----

def busy_spot(src, scale, win_w, win_h):
    """Centre (in output px) of the window of the frame with the most edges.

    A macro crop of flat sky shows nothing about a look; the structure is in the
    edges (a shadow needs a dark-to-light step to cast across). Searched in the
    middle of the picture, on the source grid, so the crop lands on cell edges.
    """
    h, w = len(src), len(src[0])
    ww, wh = max(2, win_w // scale), max(2, win_h // scale)
    lum = [[sum(p) for p in r] for r in src]
    best, spot = -1, (w // 2, h // 2)
    for y0 in range(h // 8, max(h // 8 + 1, h - wh - h // 8), 2):
        for x0 in range(w // 8, max(w // 8 + 1, w - ww - w // 8), 2):
            s = 0
            for y in range(y0, min(y0 + wh, h - 1)):
                for x in range(x0, min(x0 + ww, w - 1)):
                    s += abs(lum[y][x] - lum[y][x + 1]) + abs(lum[y][x] - lum[y + 1][x])
            if s > best:
                best, spot = s, (x0, y0)
    return spot


def write_lookshot(path, rows, src, scale, size=(320, 240)):
    """gen_lookshots.py's format: 320x240 RGB, a macro crop at the look's scale,
    cut on a cell boundary so the grid starts where the mask does."""
    tw, th = size
    x0, y0 = busy_spot(src, scale, tw, th)
    ox, oy = x0 * scale, y0 * scale
    oh, ow = len(rows), len(rows[0])
    ox, oy = min(ox, max(0, ow - tw)), min(oy, max(0, oh - th))
    buf = bytearray()
    for y in range(th):
        buf += bytes(itertools.chain.from_iterable(rows[oy + y][ox:ox + tw]))
    write_png(path, tw, th, buf)
    return {'source_px': [x0, y0], 'crop_out_px': [ox, oy]}


# --------------------------------------------------------------------- main --

def main():
    args = sys.argv[1:]
    if '-h' in args or '--help' in args:
        print(__doc__); return 0
    opt = {'--out': 'looks-out', '--looks-dir': GEN_DEFAULT, '--corpus': CORPUS_DEFAULT,
           '--only': '', '--frames': '1', '--lookshots': ''}
    flags, i = set(), 0
    while i < len(args):
        if args[i] in opt:
            opt[args[i]] = args[i + 1]; i += 2
        elif args[i] in ('--720', '--no-metrics', '--generate'):
            flags.add(args[i]); i += 1
        else:
            raise SystemExit('unknown argument %s\n%s' % (args[i], __doc__))

    if '--generate' in flags:
        env = dict(os.environ, CHOME_GEN_LOOKS='1')
        subprocess.check_call(['bash', os.path.join(CLASSIC, 'test', 'run.sh')], env=env)

    gen = opt['--looks-dir']
    looks = load_looks(gen)
    only = [s for s in opt['--only'].split(',') if s]
    if only:
        looks = [lk for lk in looks if lk.id in only or lk.dirname in only]
    screens = ['1080p'] + (['720p'] if '--720' in flags else [])
    count = int(opt['--frames'])
    metrics = None
    if '--no-metrics' not in flags:
        import look_metrics as metrics

    failed = 0
    shots = {}
    for lk in looks:
        base = os.path.join(opt['--out'], lk.dirname)
        os.makedirs(base, exist_ok=True)
        meta = {'look': lk.id, 'class': lk.cls, 'name': lk.name,
                'not_simulated': lk.not_simulated(), 'core_side': {
                    'colour': lk.core_colour, 'structure': lk.core_struct},
                'renders': {}}
        for res in screens:
            frames = frames_for(lk, opt['--corpus'], count)
            sub = base if res == '1080p' else os.path.join(base, '720p')
            os.makedirs(sub, exist_ok=True)
            for fi, (fname, rows, note) in enumerate(frames):
                h, w = len(rows), len(rows[0])
                n = scale_for(w, h, SCREENS[res])
                r = render_look(lk, gen, rows, n)
                png = 'screen.png' if fi == 0 else 'screen-%s.png' % fname
                px = flatten(r['out'], w * n, h * n, SCREENS[res])
                write_png(os.path.join(sub, png), SCREENS[res][0], SCREENS[res][1], px)
                entry = {'scale': n, 'frame': fname, 'source': note, 'native': [w, h],
                         'picture': [w * n, h * n], 'screen': png,
                         'files': r['files']}
                if fi == 0 and res == '1080p':
                    px_arr = bytearray(px)
                    # The zoom patch is ~96x54 output px; put it on the busiest edges.
                    spot = busy_spot(r['src'], n, 96, 54)
                    zc = ((SCREENS[res][0] - w * n) // 2 + spot[0] * n + 48,
                          (SCREENS[res][1] - h * n) // 2 + spot[1] * n + 27)
                    entry['review'] = look_review.make_views(
                        px_arr, SCREENS[res][0], SCREENS[res][1], sub, zoom_center=zc)
                if metrics:
                    entry['metrics'] = metrics.evaluate(lk, r, gen)
                    failed += metrics.count_fail(entry['metrics'])
                meta['renders'].setdefault(res, []).append(entry)
                if fi == 0 and res == '1080p' and opt['--lookshots'] and lk.id != 'none' \
                        and lk.id not in shots:
                    os.makedirs(opt['--lookshots'], exist_ok=True)
                    info = write_lookshot(os.path.join(opt['--lookshots'], lk.id + '.png'),
                                          r['out'], r['src'], n)
                    shots[lk.id] = dict(info, scale=n, frame=fname, source=note,
                                        files=r['files'])
                print('%-12s %-5s %s %dx  %s' % (lk.dirname, lk.cls, res, n, png), flush=True)
        with open(os.path.join(base, 'meta.json'), 'w') as f:
            json.dump(meta, f, indent=1)

    if opt['--lookshots'] and shots:
        with open(os.path.join(opt['--lookshots'], 'model-render.json'), 'w') as f:
            json.dump({'note': 'MODEL RENDERS: simulate_look/simulate_pipeline run over the '
                               'files chome_video.cpp generates (render_looks.py --lookshots). '
                               'Not captures. Core-side options (colour profile, frame blend) '
                               'are not simulated; see each look meta.json for the list.',
                       'size': [320, 240], 'looks': shots}, f, indent=1)
    if metrics:
        print('metric failures: %d' % failed)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
