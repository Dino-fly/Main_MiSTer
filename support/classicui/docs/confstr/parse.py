import re, sys

def entries(f):
    out = []
    for line in open(f, errors='replace'):
        m = re.search(r'get cfgstring \d+ = (.*)$', line.rstrip('\n'))
        if m: out.append(m.group(1))
    return out

def opts(f):
    page, res = {}, []
    for e in entries(f):
        if not e or e == '-': continue
        spec, rest = e.split(',')[0], e.split(',')[1:]
        mp = re.match(r'^P(\d+)$', spec)
        if mp and rest:
            page[mp.group(1)] = rest[0].rstrip(';'); continue
        body, pg, cond = spec, None, ''
        # prefixes come in any order: D1P1O[..] and P1O[..] both occur
        while True:
            m = re.match(r'^([DdHh])(\d+)', body)
            if m: cond += m.group(0); body = body[m.end():]; continue
            m = re.match(r'^P(\d+)', body)
            if m and body[m.end():m.end()+1] not in (',',''):
                pg = m.group(1); body = body[m.end():]; continue
            break
        if body[:1] not in ('O','o'): continue
        res.append((page.get(pg,''), body[1:], rest[0] if rest else '?', rest[1:], cond))
    return res

for f in ['NES.txt','SNES.txt','Gameboy.txt','GBA.txt','MegaDrive.txt','SMS.txt','N64.txt','PSX.txt']:
    o = opts(f)
    print("%-11s %d settings" % (f[:-4], len(o)))
