#!/usr/bin/env python3
"""
Numeric checks on the handheld looks, so a regression is a number and not an
impression. Each one is a failure that has already happened once:

  a. phase floor   A polyphase filter is only SAMPLED at the N phases the scale
                   visits (N of 256, evenly spaced). A feature narrower than the
                   spacing can sit between two of them and do nothing at all -
                   the GBA grid vanished at 4x that way (mister-polyphase-phase-
                   sampling). Every run of phases in a generated filter that
                   differs from the plain pass-through (a shadow band, a gutter)
                   must contain a visited phase.
  b. mask shape    Period equals the scale on both axes, and the corner cell
                   equals the line cells - a brighter "square" at every
                   intersection is what two separable filter gaps produced.
  c. no substrate  Bit 0x800 makes a cell emit a fixed colour; while SUB_BLEND is
                   0 none may be written, because the first patched core painted
                   them flat and wiped the shadow out of the gap.
  d. gap shadow    Measured on a RENDERED frame, by the binning that found the
                   flat-substrate bug: the gap beside a dark pixel must be no
                   brighter than the body beside it (the shadow leaks into the
                   gap), and the gap between two equal pixels must be what the
                   mask says it is, +-3 luma.
  e. None is identity  The None look is byte-equal to nearest-neighbour.
  f. mean luma     Mean luma of a grid look against the unfiltered frame.
                   Reported, never fails: a lit gap lifts it and a shadow lowers
                   it, and how far is a judgement.

d's expected gap is derived from the mask's own gap cell (1.125 for the mono
reflective looks: bright nibble 2 is 1 + 2/16, and the 1.15 the constants name
quantises to it), not from the 1.15 the constant states, or every lit look would
miss by 3-4 luma on a light body for reasons that are not a bug.

Usage:
  look_metrics.py [--looks-dir DIR] [--corpus DIR] [--only ID,ID] [--720]
                  [--all-scales] [--quick]

  --720         also check at the 720p scale (a-c always run at both, since they
                only read files; d-f render, so they are the slow part)
  --all-scales  a-c at every generated scale 2x..10x, informational for the ones a
                handheld never runs at (a 2x look cannot hold a shadow, and says so)
  --quick       a-c only, no rendering
Exits non-zero when any check FAILS.
"""

import sys, os, math

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from simulate_look import load_filter, load_mask, mask_mul, HW_PHASES

LUMA_TOL = 3              # d: flat gap vs the mask's own answer
MEAN_TOL = 0.08           # f: reported band, not a gate
DARK_RATIO = 0.7          # d: a neighbour this much darker counts as dark
DARK_STEP = 20            # ...and at least this many luma darker
MIN_SAMPLES = 8
KR, KG, KB = 0.2126, 0.7152, 0.0722


def luma(c):
    return KR * c[0] + KG * c[1] + KB * c[2]


# ------------------------------------------------------------ a. phase floor --

def visited_phases(n):
    """The ascal phase indices a magnification of n lands on: ascal_1d's own
    arithmetic for one source pixel spread over n output pixels."""
    out = []
    for k in range(n):
        u = (k + 0.5) / n - 0.5
        out.append(int((u - math.floor(u)) * HW_PHASES))
    return out


def feature_runs(taps):
    """Runs of consecutive phases (circular) whose taps differ from the plain
    pass-through row, which is [0,256,0,0] below 0.5 and [0,0,256,0] above."""
    odd = [taps[p] != ([0, 256, 0, 0] if p < HW_PHASES // 2 else [0, 0, 256, 0])
           for p in range(HW_PHASES)]
    if not any(odd):
        return []
    if all(odd):                    # a band as wide as the cell (lcd_span at 5x)
        return [list(range(HW_PHASES))]
    # start at a phase that is not odd so a run never straddles the seam
    s = next(p for p in range(HW_PHASES) if not odd[p])
    runs, cur = [], []
    for k in range(HW_PHASES):
        p = (s + k) % HW_PHASES
        if odd[p]:
            cur.append(p)
        elif cur:
            runs.append(cur); cur = []
    if cur:
        runs.append(cur)
    return runs


def check_phase_floor(card, preset, n):
    seen = visited_phases(n)
    detail, bad, features = [], 0, 0
    for key in ('hfilter', 'vfilter'):
        name = preset.get(key, 'off')
        if name == 'off':
            continue
        runs = feature_runs(load_filter(os.path.join(card, 'filters', name)))
        for run in runs:
            features += 1
            hit = [p for p in seen if p in set(run)]
            detail.append({'filter': name, 'axis': key[0], 'phases': [run[0], run[-1]],
                           'width': len(run), 'visited_inside': len(hit)})
            if not hit:
                bad += 1
    if not features:
        return {'status': 'n/a', 'note': 'no filter feature to sample', 'detail': detail}
    return {'status': 'FAIL' if bad else 'PASS', 'features': features, 'missed': bad,
            'visited': seen, 'detail': detail}


# ------------------------------------------------- b. c. the mask, as written --

def read_mask_words(path):
    """(header lines, w, h, rows of ints) straight from the file, no decoding."""
    hdr, dims, rows = [], None, []
    for line in open(path):
        line = line.split('#')[0].strip()
        if not line:
            continue
        if line.lower() == 'v2' or '=' in line:
            hdr.append(line.lower()); continue
        if dims is None:
            w, h = line.split(','); dims = (int(w), int(h)); continue
        rows.append([int(t, 16) for t in line.split(',')])
        if len(rows) >= dims[1]:
            break
    return hdr, dims, rows


def check_mask_shape(path, n):
    _, dims, rows = read_mask_words(path)
    why = []
    if dims != (n, n):
        why.append('mask is %dx%d, scale is %d' % (dims[0], dims[1], n))
    if len(rows) != dims[1] or any(len(r) != dims[0] for r in rows):
        why.append('ragged table')
    if not why:
        corner = rows[0][0]
        # the leading gap rows/cols are those equal to the corner; body is the
        # opposite corner. Every cell must be one or the other.
        body = rows[n - 1][n - 1]
        gy = next((y for y in range(n) if rows[y][n - 1] == body), n)
        gx = next((x for x in range(n) if rows[n - 1][x] == body), n)
        if gx != gy:
            why.append('gap is %d columns but %d rows' % (gx, gy))
        for y in range(n):
            for x in range(n):
                want = corner if (x < gx or y < gy) else body
                if rows[y][x] != want:
                    why.append('cell (%d,%d)=%03X, expected %03X: corner is not the '
                               'line cell' % (x, y, rows[y][x], want))
                    break
            if why: break
        if not why and body == corner:
            why.append('gap cell equals the body: no grid at this scale')
        if not why and (rows[0][n - 1] != corner or rows[n - 1][0] != corner):
            why.append('corner differs from the line cells')
    r = {'status': 'FAIL' if why else 'PASS', 'size': list(dims)}
    if not why:
        r['gap_px'] = gx
        r['gap_word'] = '%03X' % corner
        r['body_word'] = '%03X' % body
    else:
        r['why'] = why
    return r


def check_no_substrate(path):
    hdr, _, rows = read_mask_words(path)
    cells = sum(1 for r in rows for v in r if v & 0x800)
    sub = [h for h in hdr if h.startswith('substrate=')]
    bad = cells or sub
    return {'status': 'FAIL' if bad else 'PASS', 'substrate_cells': cells,
            'substrate_header': bool(sub)}


# --------------------------------------------------- d. the gap on a frame ---

def _bin(v):
    return min(3, int(v) // 64)


def gap_samples(out, src, n):
    """Yield (axis, left/up colour, right/down colour, gap px, body px).

    Taken on the trailing row (or column) of each cell, which the other axis'
    shadow has already left, and only where the two source rows (columns) that
    could contaminate it are equal - so a sample isolates one axis."""
    h, w = len(src), len(src[0])
    for j in range(1, h):
        y = j * n + n - 1
        for i in range(1, w):
            a, b = src[j][i - 1], src[j][i]
            if src[j - 1][i - 1] == a and src[j - 1][i] == b:
                yield ('x', a, b, out[y][i * n], out[y][i * n + n - 1])
    for j in range(1, h):
        for i in range(1, w):
            a, b = src[j - 1][i], src[j][i]
            if src[j - 1][i - 1] == a and src[j][i - 1] == b:
                x = i * n + n - 1
                yield ('y', a, b, out[j * n][x], out[j * n + n - 1][x])


def check_gap(r):
    cells = r['cells']
    if not cells:
        na = {'status': 'n/a', 'note': 'no mask'}
        return na, na
    n = r['scale']
    gap_cell = cells[0][0]
    body_cell = cells[n - 1][n - 1]
    dark = {'x': [], 'y': []}
    flat = {'x': [], 'y': []}
    for axis, a, b, gap, body in gap_samples(r['out'], r['src'], n):
        la, lb = luma(a), luma(b)
        if a == b:
            if la >= 30:
                exp = luma(tuple(mask_mul(c, m) for c, m in zip(a, gap_cell)))
                flat[axis].append((luma(body), luma(gap), exp))
        elif la <= DARK_RATIO * lb and lb - la >= DARK_STEP:
            dark[axis].append(luma(gap) - luma(body))

    # d1: gap minus body after a dark neighbour
    d1 = {'axes': {}}
    status, worst = 'PASS', None
    for axis, v in dark.items():
        if len(v) < MIN_SAMPLES:
            d1['axes'][axis] = {'n': len(v), 'note': 'too few dark->light edges'}
            continue
        mean = sum(v) / len(v)
        d1['axes'][axis] = {'n': len(v), 'mean': round(mean, 2), 'max': round(max(v), 2),
                            'min': round(min(v), 2),
                            'share_brighter': round(sum(1 for x in v if x > 0.5) / len(v), 3)}
        worst = mean if worst is None else max(worst, mean)
        if mean > 0:
            status = 'FAIL'
    if worst is None:
        d1['status'] = 'n/a'
    else:
        d1['status'], d1['worst_mean'] = status, round(worst, 2)

    # d2: gap between equal pixels is the mask's answer, binned by body luma
    d2 = {'bins': {}}
    status, maxerr, total = 'PASS', 0.0, 0
    for axis, v in flat.items():
        for body, gap, exp in v:
            total += 1
            b = d2['bins'].setdefault(_bin(body), {'n': 0, 'body': 0.0, 'gap': 0.0, 'err': 0.0})
            b['n'] += 1; b['body'] += body; b['gap'] += gap
            b['err'] = max(b['err'], abs(gap - exp))
            maxerr = max(maxerr, abs(gap - exp))
    for b in d2['bins'].values():
        b['body'] = round(b['body'] / b['n'], 1); b['gap'] = round(b['gap'] / b['n'], 1)
        b['ratio'] = round(b['gap'] / b['body'], 3) if b['body'] else 0
        b['err'] = round(b['err'], 2)
    d2['bins'] = {'%d-%d' % (k * 64, k * 64 + 63): v for k, v in sorted(d2['bins'].items())}
    if total < MIN_SAMPLES:
        d2['status'] = 'n/a'; d2['note'] = 'too few flat edges (%d)' % total
    else:
        d2['status'] = 'FAIL' if maxerr > LUMA_TOL else 'PASS'
        d2['max_err'] = round(maxerr, 2)
    d2['n'] = total
    return d1, d2


# ------------------------------------------------------ e. none, f. mean -----

def check_identity(look, r):
    if look.id != 'none':
        return {'status': 'n/a'}
    same = r['out'] == r['nearest']
    return {'status': 'PASS' if same else 'FAIL'}


def mean_luma(rows):
    t = 0.0
    for row in rows:
        for p in row:
            t += luma(p)
    return t / (len(rows) * len(rows[0]))


def check_mean(r):
    if not r['cells']:
        return {'status': 'n/a'}
    a, b = mean_luma(r['out']), mean_luma(r['nearest'])
    ratio = a / b if b else 0.0
    return {'status': 'INFO', 'look': round(a, 2), 'unfiltered': round(b, 2),
            'ratio': round(ratio, 4), 'within_tolerance': abs(ratio - 1) <= MEAN_TOL,
            'tolerance': MEAN_TOL}


# --------------------------------------------------------------------- API ---

def evaluate(look, r, gen_dir):
    """All checks for one rendered look. r is render_looks.render_look()'s dict."""
    n, card, pre = r['scale'], r['card'], r['preset']
    m = pre.get('mask', 'off')
    res = {'a_phase_floor': check_phase_floor(card, pre, n)}
    if m != 'off':
        mp = os.path.join(card, 'shadow_masks', m)
        res['b_mask_shape'] = check_mask_shape(mp, n)
        res['c_no_substrate'] = check_no_substrate(mp)
    else:
        res['b_mask_shape'] = res['c_no_substrate'] = {'status': 'n/a', 'note': 'no mask'}
    res['d1_gap_after_dark'], res['d2_gap_between_light'] = check_gap(r)
    res['e_none_identity'] = check_identity(look, r)
    res['f_mean_luma'] = check_mean(r)
    return res


def evaluate_files(look, gen_dir, n):
    """a-c only, for scales nobody renders."""
    import render_looks as rl
    card = rl.card_for(gen_dir, n)
    pre = rl.read_preset(card, look)
    res = {'a_phase_floor': check_phase_floor(card, pre, n)}
    m = pre.get('mask', 'off')
    if m != 'off':
        mp = os.path.join(card, 'shadow_masks', m)
        res['b_mask_shape'] = check_mask_shape(mp, n)
        res['c_no_substrate'] = check_no_substrate(mp)
    return res


def count_fail(metrics):
    return sum(1 for v in metrics.values() if v.get('status') == 'FAIL')


def _cell(v):
    s = v.get('status', '?')
    return s


def table_row(look, n, res, tag=''):
    d1, d2, f = res.get('d1_gap_after_dark'), res.get('d2_gap_between_light'), res.get('f_mean_luma')
    d1t = '-' if not d1 else (d1['status'] if d1['status'] == 'n/a' else
                              '%s %+.1f' % (d1['status'], d1['worst_mean']))
    d2t = '-' if not d2 else (d2['status'] if d2['status'] == 'n/a' else
                              '%s e%.1f' % (d2['status'], d2['max_err']))
    ft = '-' if not f else ('n/a' if f['status'] == 'n/a' else
                            '%.3f%s' % (f['ratio'], '' if f['within_tolerance'] else '!'))
    g = lambda k: _cell(res[k]) if k in res else '-'
    return '%-12s %-5s %2dx%-3s %-5s %-5s %-5s %-12s %-12s %-5s %s' % (
        look.dirname, look.cls, n, tag, g('a_phase_floor'), g('b_mask_shape'),
        g('c_no_substrate'), d1t, d2t, g('e_none_identity'), ft)


HEADER = '%-12s %-5s %-6s %-5s %-5s %-5s %-12s %-12s %-5s %s' % (
    'look', 'class', 'scale', 'a', 'b', 'c', 'd1 dark Δ', 'd2 flat err', 'e', 'f mean')


def main():
    args = sys.argv[1:]
    if '-h' in args or '--help' in args:
        print(__doc__); return 0
    import render_looks as rl
    opt = {'--looks-dir': rl.GEN_DEFAULT, '--corpus': rl.CORPUS_DEFAULT, '--only': ''}
    flags, i = set(), 0
    while i < len(args):
        if args[i] in opt:
            opt[args[i]] = args[i + 1]; i += 2
        elif args[i] in ('--720', '--all-scales', '--quick'):
            flags.add(args[i]); i += 1
        else:
            raise SystemExit('unknown argument %s\n%s' % (args[i], __doc__))
    looks = rl.load_looks(opt['--looks-dir'])
    only = [s for s in opt['--only'].split(',') if s]
    if only:
        looks = [lk for lk in looks if lk.id in only or lk.dirname in only]

    screens = ['1080p', '720p']
    fails = 0
    print(HEADER)
    for lk in looks:
        frames = rl.frames_for(lk, opt['--corpus'], 1)
        fname, rows, _ = frames[0]
        h, w = len(rows), len(rows[0])
        scales = {}
        for res in screens:
            scales.setdefault(rl.scale_for(w, h, rl.SCREENS[res]), []).append(res)
        for n, tags in sorted(scales.items(), reverse=True):
            tag = '/'.join(t[:-1] for t in tags)
            heavy = '--quick' not in flags and ('1080p' in tags or '--720' in flags)
            if heavy:
                r = rl.render_look(lk, opt['--looks-dir'], rows, n)
                res = evaluate(lk, r, opt['--looks-dir'])
            else:
                res = evaluate_files(lk, opt['--looks-dir'], n)
            fails += count_fail(res)
            print(table_row(lk, n, res, tag), flush=True)
        if '--all-scales' in flags:
            for n in range(2, 11):
                res = evaluate_files(lk, opt['--looks-dir'], n)
                print('  files-only ' + table_row(lk, n, res), flush=True)
                # informational: a handheld never runs at most of these scales
    print('\n%d check(s) FAILED' % fails if fails else '\nall checks passed')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
