#!/usr/bin/env python3
"""Makes gba/gfxdata.c: the music player's fonts (DejaVu Sans) and icons as
4-bit coverage masks, so the Game Boy Advance can draw smooth text and shapes.
Run from the project folder: python3 tools/gba_gfxgen.py  (needs Pillow)."""
from PIL import Image, ImageDraw, ImageFont
import sys, os

REG = '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf'
BOLD = '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'
FONTS = [('font8', REG, 8), ('font8b', BOLD, 8), ('font9b', BOLD, 9), ('font12b', BOLD, 12), ('font14b', BOLD, 14)]
SS = 4

out = ['/* gfxdata.c - made by tools/gba_gfxgen.py; do not edit. */', '#include "gfxdata.h"', '']
pool = bytearray()

def add_mask(img):
    """img: L image (0..255). Returns offset into the pool; 4 bits a pixel, rows padded to bytes."""
    w, h = img.size
    off = len(pool)
    px = img.load()
    for y in range(h):
        row = []
        for x in range(w):
            row.append(min(15, (px[x, y] + 8) // 17))
        if len(row) % 2: row.append(0)
        for i in range(0, len(row), 2):
            pool.append(row[i] | (row[i + 1] << 4))
    return off

fdefs = []
for name, path, size in FONTS:
    f = ImageFont.truetype(path, size)
    asc, desc = f.getmetrics()
    glyphs = []
    for c in range(32, 131):
        ch = chr(c) if c < 127 else '\u2190\u2192\u2191\u2193'[c - 127]   # 127-130: arrows
        adv = round(f.getlength(ch))
        bbox = f.getbbox(ch)
        if bbox[2] <= bbox[0] or bbox[3] <= bbox[1]:
            glyphs.append((adv, 0, 0, 0, 0, 0)); continue
        x0, y0, x1, y1 = bbox
        w, h = x1 - x0, y1 - y0
        im = Image.new('L', (w, h), 0)
        ImageDraw.Draw(im).text((-x0, -y0), ch, font=f, fill=255)
        glyphs.append((adv, x0, y0, w, h, add_mask(im)))
    out.append(f'static const Glyph {name}_g[99] = {{')
    for g in glyphs:
        out.append('    {%d, %d, %d, %d, %d, %d},' % g)
    out.append('};')
    fdefs.append(f'const Font {name} = {{ {asc + desc}, {asc}, {name}_g }};')

def icon(name, w, h, draw_fn):
    big = Image.new('L', (w * SS, h * SS), 0)
    d = ImageDraw.Draw(big)
    draw_fn(d, SS)
    im = big.resize((w, h), Image.LANCZOS)
    off = add_mask(im)
    return f'const Mask {name} = {{ {w}, {h}, {off} }};'

S = SS
def sc(pts, s): return [(x * s, y * s) for x, y in pts]
masks = []
def circ(r): return lambda d, s: d.ellipse([0, 0, (2 * r) * s - 1, (2 * r) * s - 1], fill=255)
masks.append(icon('m_circle9', 18, 18, circ(9)))
masks.append(icon('m_circle10', 20, 20, circ(10)))
masks.append(icon('m_circle12', 24, 24, circ(12)))
masks.append(icon('m_circle13', 26, 26, circ(13)))
masks.append(icon('m_circle15', 30, 30, circ(15)))
masks.append(icon('m_dot5', 5, 5, circ(2.5)))
def ring(r, t):
    def f(d, s):
        d.ellipse([0, 0, 2 * r * s - 1, 2 * r * s - 1], fill=255)
        d.ellipse([t * s, t * s, (2 * r - t) * s - 1, (2 * r - t) * s - 1], fill=0)
    return f
masks.append(icon('m_ring9', 9, 9, ring(4.5, 1)))
masks.append(icon('m_dot5r', 5, 5, circ(2.5)))
# rounded-rect corners (top-left quarter of a circle of radius r)
for r in (3, 5, 6, 8, 10):
    masks.append(icon(f'm_corner{r}', r, r, lambda d, s, r=r: d.ellipse([0, 0, 2 * r * s - 1, 2 * r * s - 1], fill=255)))
# player icons, 12x12, centred at 6,6
def I(fn): return fn
def play(d, s): d.polygon(sc([(3.5, 1), (3.5, 11), (11, 6)], s), fill=255)
def pause(d, s): d.rectangle(sc([(2.5, 1), (5, 11)], s), fill=255); d.rectangle(sc([(7, 1), (9.5, 11)], s), fill=255)
def nxt(d, s): d.polygon(sc([(2, 2), (2, 10), (8, 6)], s), fill=255); d.rectangle(sc([(8, 2), (10, 10)], s), fill=255)
def prv(d, s): d.polygon(sc([(10, 2), (10, 10), (4, 6)], s), fill=255); d.rectangle(sc([(2, 2), (4, 10)], s), fill=255)
def stop(d, s): d.rectangle(sc([(2.5, 2.5), (9.5, 9.5)], s), fill=255)
def shuffle(d, s):
    w = int(1.3 * s)
    d.line(sc([(1, 3.5), (4, 3.5), (7, 8.5), (9, 8.5)], s), fill=255, width=w)
    d.line(sc([(1, 8.5), (4, 8.5), (7, 3.5), (9, 3.5)], s), fill=255, width=w)
    d.polygon(sc([(8.5, 1.5), (8.5, 5.5), (11.5, 3.5)], s), fill=255)
    d.polygon(sc([(8.5, 6.5), (8.5, 10.5), (11.5, 8.5)], s), fill=255)
def repeat(d, s):
    w = int(1.3 * s)
    d.arc(sc([(1.5, 2), (10.5, 10)], s), 200, 500, fill=255, width=w)
    d.polygon(sc([(7.5, 0.5), (11.5, 3), (7.5, 5)], s), fill=255)
def note(d, s):
    d.ellipse(sc([(1, 7), (6, 11.5)], s), fill=255)
    d.rectangle(sc([(4.8, 1), (6, 9)], s), fill=255)
    d.polygon(sc([(4.8, 1), (10, 2.5), (10, 5), (6, 3.6)], s), fill=255)
for n, fn in [('play', play), ('pause', pause), ('next', nxt), ('prev', prv), ('stop', stop),
              ('shuffle', shuffle), ('repeat', repeat), ('note', note)]:
    masks.append(icon('m_' + n, 12, 12, fn))
# the cube (three faces, drawn in three colours), 44x46
def face(pts): return lambda d, s: d.polygon(sc(pts, s), fill=255)
masks.append(icon('m_cube_top', 44, 46, face([(22, 1), (43, 12), (22, 23), (1, 12)])))
masks.append(icon('m_cube_left', 44, 46, face([(1, 12), (22, 23), (22, 45), (1, 34)])))
masks.append(icon('m_cube_right', 44, 46, face([(43, 12), (22, 23), (22, 45), (43, 34)])))

body = []
body.append('const unsigned char gfx_pool[%d] = {' % len(pool))
for i in range(0, len(pool), 24):
    body.append('    ' + ','.join(str(b) for b in pool[i:i + 24]) + ',')
body.append('};')
open('gba/gfxdata.c', 'w').write('\n'.join(out + body + fdefs + masks) + '\n')
hdr = ['/* gfxdata.h - made by tools/gba_gfxgen.py; do not edit. */', '#pragma once',
       'typedef struct { unsigned char adv; signed char x, y; unsigned char w, h; unsigned short off; } Glyph;',
       'typedef struct { unsigned char height, ascent; const Glyph *g; } Font;',
       'typedef struct { unsigned char w, h; unsigned short off; } Mask;',
       'extern const unsigned char gfx_pool[];']
hdr += ['extern const Font %s;' % n for n, _, _ in FONTS]
hdr += ['extern const Mask %s;' % m.split(' ')[2] for m in masks]
open('gba/gfxdata.h', 'w').write('\n'.join(hdr) + '\n')
print('pool bytes', len(pool))
