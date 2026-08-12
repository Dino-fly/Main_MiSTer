#!/usr/bin/env python3
"""
Authoring pass for classicui/lookshots/<id>.png - the static preview images the
Display screen shows instead of a computed preview. Macro views: a small crop of
a real capture blown up so the structure of the look (phosphor triads, scanline
gaps, LCD grid, palette) is actually visible, the way a macro photo of the real
screen would show it.
"""
import sys, math
from simulate_look import read_png, write_png, BGB, POCKET

OUT_W, OUT_H = 320, 240

def crop(px, w, h, x0, y0, cw, ch):
    t = []
    for y in range(ch):
        row = []
        for x in range(cw):
            i = ((y0+y)*w + (x0+x))*3
            row.append((px[i], px[i+1], px[i+2]))
        t.append(row)
    return t

def emit(name, buf):
    out = bytearray(OUT_W*OUT_H*3)
    for y in range(OUT_H):
        for x in range(OUT_W):
            r,g,b = buf[y][x]
            out[(y*OUT_W+x)*3:(y*OUT_W+x)*3+3] = bytes((min(255,max(0,int(r))),
                                                        min(255,max(0,int(g))),
                                                        min(255,max(0,int(b)))))
    write_png(name, OUT_W, OUT_H, out)
    print(name)

def blank():
    return [[(0,0,0)]*OUT_W for _ in range(OUT_H)]

def crt(src, cell, scan_depth, grille=True, bleed=0.0, pitch=3):
    """cell px per source pixel; RGB grille stripes at `pitch`; scanline pair."""
    sw, sh = len(src[0]), len(src)
    out = blank()
    for y in range(OUT_H):
        sy = min(y // cell, sh-1)
        # position inside the source line drives the scanline envelope
        f = (y % cell) / cell
        env = (1-scan_depth) + scan_depth * math.sin(math.pi*f)**1.2
        for x in range(OUT_W):
            sx = min(x // cell, sw-1)
            r,g,b = src[sy][sx]
            if bleed > 0:
                l = src[sy][max(sx-1,0)]; rr = src[sy][min(sx+1,sw-1)]
                r = r*(1-2*bleed) + (l[0]+rr[0])*bleed
                g = g*(1-2*bleed) + (l[1]+rr[1])*bleed
                b = b*(1-2*bleed) + (l[2]+rr[2])*bleed
            if grille:
                ph = (x // 1) % pitch
                # each output column is one phosphor stripe; others dimmed hard
                k = [0.18, 0.18, 0.18]; k[ph] = 1.0
                r, g, b = r*k[0]*1.55, g*k[1]*1.55, b*k[2]*1.55
            out[y][x] = (r*env, g*env, b*env)
    return out

def lcd(src, cell, gutter_frac, depth, pal=None, shadow=False):
    sw, sh = len(src[0]), len(src)
    if pal:
        lum = sorted({(r*299+g*587+b*114)//1000 for row in src for (r,g,b) in row}, reverse=True)
        lv = {y: (i if len(lum)<=4 else min(int(i*4/len(lum)),3)) for i,y in enumerate(lum)}
        src = [[pal[lv[(r*299+g*587+b*114)//1000]] for (r,g,b) in row] for row in src]
    out = blank()
    gut = max(1, int(cell*gutter_frac))
    for y in range(OUT_H):
        sy = min(y // cell, sh-1)
        gy = (y % cell) >= cell - gut
        for x in range(OUT_W):
            sx = min(x // cell, sw-1)
            gx = (x % cell) >= cell - gut
            r,g,b = src[sy][sx]
            if gx or gy:
                d = depth
                r,g,b = r*(1-d), g*(1-d), b*(1-d)
            if shadow and not (gx or gy):
                # the DMG's drop shadow: a dark ghost down-right of dark cells
                psx, psy = sx - 1, sy - 1
                if psx >= 0 and psy >= 0:
                    pr,pg,pb = src[psy][psx]
                    if (pr+pg+pb) < (r+g+b):
                        r,g,b = r*0.88, g*0.88, b*0.88
            out[y][x] = (r,g,b)
    return out

def main(outdir):
    w,h,px = read_png('gba_off.png')       # vivid colours for the CRT structure
    csrc = crop(px, w, h, 60, 40, 46, 36)
    w2,h2,px2 = read_png('gb_dmg_v2.png')  # real DMG-green frame off the device
    gsrc = crop(px2, w2, h2, 40, 40, 46, 36)
    w3,h3,px3 = read_png('gb_palette_off.png')  # 4-level grayscale
    graw = crop(px3, w3, h3, 40, 40, 46, 36)
    w4,h4,px4 = read_png('gba_22.png')     # the core's own GBA 2.2 rendition
    asrc = crop(px4, w4, h4, 60, 40, 46, 36)

    emit(f'{outdir}/pvm-rgb.png',    crt(csrc, 7, 0.30))
    emit(f'{outdir}/bvm-rgb.png',    crt(csrc, 7, 0.48))
    emit(f'{outdir}/pvm-svideo.png', crt(csrc, 7, 0.30, bleed=0.10))
    emit(f'{outdir}/composite.png',  crt(csrc, 7, 0.30, bleed=0.22))
    emit(f'{outdir}/pal-tv.png',     crt(csrc, 7, 0.18, bleed=0.14))
    emit(f'{outdir}/vga.png',        crt(csrc, 7, 0.04, grille=True, pitch=3))
    emit(f'{outdir}/sharp.png',      lcd(csrc, 7, 0.0, 0.0))
    emit(f'{outdir}/none.png',       lcd(csrc, 7, 0.0, 0.0))

    emit(f'{outdir}/dmg.png',    lcd(graw, 7, 0.2, 0.45, pal=BGB, shadow=True))
    emit(f'{outdir}/pocket.png', lcd(graw, 7, 0.2, 0.45, pal=POCKET, shadow=True))
    emit(f'{outdir}/gbc.png',    lcd(gsrc, 7, 0.2, 0.40))
    emit(f'{outdir}/agb001.png', lcd(asrc, 7, 0.2, 0.35))
    emit(f'{outdir}/ags001.png', lcd(asrc, 7, 0.2, 0.30))
    emit(f'{outdir}/ags101.png', lcd(crop(px, w, h, 60, 40, 46, 36), 7, 0.2, 0.30))
    emit(f'{outdir}/gg.png',     lcd(asrc, 7, 0.2, 0.35))
    emit(f'{outdir}/gg-mod.png', lcd(crop(px, w, h, 60, 40, 46, 36), 7, 0.2, 0.30))
    emit(f'{outdir}/lynx.png',   lcd(asrc, 7, 0.2, 0.30))
    emit(f'{outdir}/ws.png',     lcd(graw, 7, 0.2, 0.35, pal=[(208,202,186),(150,146,132),(96,92,82),(40,38,32)]))
    emit(f'{outdir}/wsc.png',    lcd(asrc, 7, 0.2, 0.30))
    emit(f'{outdir}/ngpc.png',   lcd(asrc, 7, 0.2, 0.28))

if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else 'lookshots')
