#!/usr/bin/env python3
"""render_models.py "<System Shock 2 folder>" OUT_DIR [NAME ...]

Draws the weapon models from your own install (Data/res/OBJ.CRF), textured, in the model's own axes, to check how
they sit: first-person models (*_H) as the player sees them (from behind and above the camera), from the right and
from above; world models (*_W) from the right and from above. Axes are the Dark engine's: x forward, y left, z up.
The origin (red dot) is the model's origin. Needs numpy and Pillow. Writes one PNG per model plus a contact sheet."""
import os, sys, zipfile, math
import numpy as np
from PIL import Image, ImageDraw
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract_weapons import lgmd, pcx

def load_tex(z, names, name, cache):
    k = name.upper()
    if k not in cache:
        t = None
        for c in ('TXT16/' + k, 'TXT/' + k):
            if c in names:
                r = pcx(z.read(names[c]))
                if r: w, h, rgba = r; t = np.frombuffer(bytes(rgba), np.uint8).reshape(h, w, 4)
                break
        cache[k] = t
    return cache[k]

def raster(img, zb, P, UV, tex, shade, flat):
    """P: 3x(x,y,depth) in pixels; nearer = smaller depth."""
    H, W = zb.shape
    x0, x1 = max(0, int(P[:, 0].min())), min(W - 1, int(P[:, 0].max()) + 1)
    y0, y1 = max(0, int(P[:, 1].min())), min(H - 1, int(P[:, 1].max()) + 1)
    if x0 > x1 or y0 > y1: return
    ys, xs = np.mgrid[y0:y1 + 1, x0:x1 + 1]; xs = xs + 0.5; ys = ys + 0.5
    (ax, ay, _), (bx, by, _), (cx, cy, _) = P
    d = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy)
    if abs(d) < 1e-9: return
    l0 = ((by - cy) * (xs - cx) + (cx - bx) * (ys - cy)) / d
    l1 = ((cy - ay) * (xs - cx) + (ax - cx) * (ys - cy)) / d
    l2 = 1 - l0 - l1
    m = (l0 >= -1e-4) & (l1 >= -1e-4) & (l2 >= -1e-4)
    if not m.any(): return
    z = l0 * P[0, 2] + l1 * P[1, 2] + l2 * P[2, 2]
    sub = zb[y0:y1 + 1, x0:x1 + 1]; m &= z < sub
    if not m.any(): return
    if tex is not None and UV is not None:
        u = l0 * UV[0, 0] + l1 * UV[1, 0] + l2 * UV[2, 0]; v = l0 * UV[0, 1] + l1 * UV[1, 1] + l2 * UV[2, 1]
        th, tw = tex.shape[:2]
        tx = (np.floor(u * tw).astype(int)) % tw; ty = (np.floor(v * th).astype(int)) % th
        col = tex[ty, tx, :3].astype(float)
    else:
        col = np.broadcast_to(np.array(flat, float), m.shape + (3,))
    col = np.clip(col * shade, 0, 255)
    sub[m] = z[m]
    img[y0:y1 + 1, x0:x1 + 1][m] = col[m]

def view(model, texs, R, size=360, label='', grid=False, marks=()):
    """R: 3x3, rows = screen right, screen down, into the screen (all in model axes)."""
    V = np.array(model['verts']); S = V @ R.T
    lo, hi = S[:, :2].min(0), S[:, :2].max(0); span = max(hi - lo) or 1
    k = (size - 40) / span; off = (size - (hi - lo) * k) / 2 - lo * k
    P = np.c_[S[:, :2] * k + off, S[:, 2]]
    img = np.full((size, size, 3), 235.0); zb = np.full((size, size), np.inf)
    if grid:                                                     # model units, under the model
        step = 0.25 if span < 3 else 0.5
        for g in np.arange(-6, 6.001, step):
            for ax in (0, 1):
                p = int(round(g * k + off[ax]))
                if 0 <= p < size:
                    c = (90, 110, 250) if abs(g) < 1e-6 else (190, 200, 245)
                    if ax == 0: img[:, p] = c
                    else: img[p, :] = c
    light = np.array([0.3, -0.5, 0.8]); light /= np.linalg.norm(light)
    for mat, vs, uvi, ni in model['polys']:
        tex = texs.get(mat)
        a = V[vs[0]]
        for i in range(1, len(vs) - 1):
            idx = [vs[0], vs[i], vs[i + 1]]
            n = np.cross(V[idx[1]] - a, V[idx[2]] - a); nn = np.linalg.norm(n)
            shade = 0.55 + 0.45 * abs(n @ light) / nn if nn > 0 else 1
            UV = np.array([model['uvs'][uvi[j]] for j in (0, i, i + 1)]) if uvi and max(uvi) < len(model['uvs']) else None
            raster(img, zb, P[idx], UV, tex, shade, (150, 150, 160))
    im = Image.fromarray(img.astype(np.uint8)); d = ImageDraw.Draw(im)
    o = np.array([0.0, 0, 0]) @ R.T; ox, oy = o[:2] * k + off
    d.ellipse([ox - 4, oy - 4, ox + 4, oy + 4], fill=(220, 30, 30))
    d.text((6, 4), label, fill=(0, 0, 0))
    if grid:
        step = 0.25 if span < 3 else 0.5
        for g in np.arange(-6, 6.001, step * 2):
            px, py = g * k + off[0], g * k + off[1]
            if 8 < px < size - 20: d.text((px + 2, size - 12), '%g' % g, fill=(40, 40, 200))
            if 14 < py < size - 14: d.text((2, py + 1), '%g' % (-g if R is RIGHT else -g), fill=(40, 40, 200))
    for (pt, col) in marks:
        q = np.array(pt, float) @ R.T; qx, qy = q[:2] * k + off
        d.ellipse([qx - 7, qy - 7, qx + 7, qy + 7], fill=col, outline=(0, 0, 0))
    return im

# view matrices: rows = (screen right, screen down, depth into the screen), in model axes (x fwd, y left, z up)
def look(fwd, up):
    f = np.array(fwd, float); f /= np.linalg.norm(f); u = np.array(up, float)
    r = np.cross(f, u); r /= np.linalg.norm(r); u = np.cross(r, f)
    return np.array([r, -u, f])
PLAYER = look([1, 0, -0.35], [0, 0, 1])          # from behind and a little above, looking forward
RIGHT = look([0, 1, 0], [0, 0, 1])               # standing on the right, looking left: forward is to the right
TOP = look([0, 0, -1], [1, 0, 0])                # from above, forward up the page

def main():
    a = [x for x in sys.argv[1:]]
    if len(a) < 2: sys.exit(__doc__)
    game, out, only = a[0], a[1], {x.upper() for x in a[2:]}
    os.makedirs(out, exist_ok=True)
    crf = next((os.path.join(game, p) for p in ('Data/res/OBJ.CRF', 'Data/res/obj.crf', 'res/obj.crf') if os.path.isfile(os.path.join(game, p))), None)
    if not crf: sys.exit('no OBJ.CRF under ' + game)
    z = zipfile.ZipFile(crf); names = {i.filename.upper(): i.filename for i in z.infolist()}; cache = {}
    sheet = []
    for key in sorted(n for n in names if n.endswith(('_H.BIN', '_W.BIN'))):
        stem = key[:-4]
        if only and stem not in only and stem[:-2] not in only: continue
        m = lgmd(z.read(names[key]))
        if not m: continue
        texs = {mat: load_tex(z, names, t, cache) for mat, t in m['mats'].items()}
        hand = key.endswith('_H.BIN')
        views = ([view(m, texs, PLAYER, label=stem + '  as the player sees it')] if hand else []) + \
                [view(m, texs, RIGHT, label=stem + '  right side (forward ->)'), view(m, texs, TOP, label=stem + '  top (forward ^)')]
        row = Image.new('RGB', (360 * len(views), 360), (255, 255, 255))
        for i, v in enumerate(views): row.paste(v, (360 * i, 0))
        row.save(os.path.join(out, stem + '.png')); sheet.append(row)
        print('wrote', stem + '.png')

if __name__ == '__main__': main()
