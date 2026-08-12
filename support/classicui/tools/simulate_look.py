#!/usr/bin/env python3
"""
Simulate the MiSTer scaler over a raw core capture, with the exact coefficients
chome_video.cpp generates, so a Display look can be judged without an HDMI grabber.

Pipeline: [palette remap (what the core will do)] -> H polyphase -> V polyphase.

Usage: simulate_look.py in.png out.png scale [dmg|pocket|none] [grid|sharp]
"""
import sys, struct, zlib, math

PHASES = 64
GRID_GUTTER = 0.22
GRID_DEPTH = 0.42

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

def grid_coeffs():
    half = GRID_GUTTER/2; soft = 1.0/PHASES
    boost = min(1.0/(1.0-GRID_DEPTH*GRID_GUTTER), 1.12)
    out = []
    for p in range(PHASES):
        x = p/PHASES; d = abs(x-0.5)
        e = 1.0 if d < half else (1.0-(d-half)/soft if d < half+soft else 0.0)
        if x < 0.5-half: w1,w2 = 1.0,0.0
        elif x > 0.5+half: w1,w2 = 0.0,1.0
        else:
            t = (x-(0.5-half))/GRID_GUTTER; w1,w2 = 1.0-t,t
        g = boost*(1.0-GRID_DEPTH*e)
        out.append([0, round(w1*g*128), round(w2*g*128), 0])
    return out

def sharp_coeffs():
    return [[0,128,0,0] if p/PHASES < 0.5 else [0,0,128,0] for p in range(PHASES)]

def scale_1d(src, sw, count, dw, coeffs):
    # src: list of rows, each row a list of (r,g,b) tuples along the scaled axis
    out = []
    for row in src:
        orow = []
        for x in range(dw):
            u = (x+0.5)*sw/dw - 0.5
            i = math.floor(u); ph = int((u-i)*PHASES) % PHASES
            c = coeffs[ph]
            acc = [0,0,0]
            for t in range(4):
                si = min(max(i-1+t,0),sw-1)
                for k in range(3): acc[k] += c[t]*row[si][k]
            orow.append(tuple(min(max(a//128,0),255) for a in acc))
        out.append(orow)
    return out

def main():
    inp, outp, scale, pal, filt = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4], sys.argv[5]
    w,h,px = read_png(inp)
    rows = [[tuple(px[(y*w+x)*3:(y*w+x)*3+3]) for x in range(w)] for y in range(h)]

    if pal in ('dmg','pocket'):
        # capture is 4-level grayscale: rank unique lumas light->dark onto the palette
        palette = BGB if pal == 'dmg' else POCKET
        lumas = sorted({(r*299+g*587+b*114)//1000 for row in rows for (r,g,b) in row}, reverse=True)
        lv = {y: min(int(i*4/max(len(lumas),1)),3) for i,y in enumerate(lumas)}
        # map each distinct luma rank into 4 buckets by order
        if len(lumas) <= 4:
            lv = {y:i for i,y in enumerate(lumas)}
        rows = [[palette[lv[(r*299+g*587+b*114)//1000]] for (r,g,b) in row] for row in rows]

    co = grid_coeffs() if filt == 'grid' else sharp_coeffs()
    dw, dh = w*scale, h*scale
    rows = scale_1d(rows, w, h, dw, co)                       # horizontal
    cols = [[rows[y][x] for y in range(h)] for x in range(dw)] # transpose
    cols = scale_1d(cols, h, dw, dh, co)                       # vertical
    out = bytearray(dw*dh*3)
    for x in range(dw):
        for y in range(dh):
            r,g,b = cols[x][y]
            out[(y*dw+x)*3:(y*dw+x)*3+3] = bytes((r,g,b))
    write_png(outp, dw, dh, out)
    print(f"{outp}: {dw}x{dh}")

if __name__ == '__main__':
    main()
