#!/usr/bin/env python3
"""extract_weapons.py "<System Shock 2 folder>" [--preview DIR]

Reads the weapon models from your own install (Data/res/OBJ.CRF) and writes <game>/vr/weapons.vrw, which the headset
build draws in the gun hand. Run it once (again after installing a model mod). Nothing from the game goes into this
repository; the output stays in your game folder.

For every first-person model (*_H.BIN) it records the materials (texture size and triangle count), which is how the
VR build recognises the weapon the game is drawing. For every world model (*_W.BIN: the one lying on the floor) it
writes the triangles, texture coordinates and normals, already turned so that the barrel points forward, the grip
points down and the origin is the grip, scaled to a believable length (metres), plus the muzzle point. Textures (PCX)
are decoded to RGBA. --preview writes a side view of each world model as PNG (needs matplotlib) to check the turn.

File (little endian): "NDVW", u32 version 1; u32 ntex, per texture: char name[32], u32 w, h, rgba[w*h*4];
u32 nh, per first-person model: char name[16], u32 world index (0xFFFFFFFF none), u32 nmat, per material: u32 tex_w,
tex_h, triangles; u32 nw, per world model: char name[16], float muzzle[3], float length_m, u32 nvert, per vertex:
float x, y, z, u, v, nx, ny, nz (metres; x right, y down, z forward), u32 ntri, per triangle: u16 a, b, c, texture
(0xFFFF none)."""
import os, struct, sys, zipfile, math

# believable lengths in metres of the longest side, by world model name (the game scales its objects per archetype)
LENGTH = {'ATEK': 0.24, 'AR15': 0.85, 'SG': 0.95, 'SGB': 0.95, 'GREN': 0.75, 'EMPGUB': 0.95, 'FSN': 1.05, 'AL': 0.5,
          'VIRO': 0.8, 'SFG': 0.55, 'ES': 0.45, 'AMP': 0.35, 'WRENCH': 0.5, 'SHARD': 0.5, 'RAPIER': 0.6, 'MACH': 0.9}
H_TO_W = {'EMPGUN': 'EMPGUB', 'PIPE': 'WRENCH'}

def lgmd(b):
    if b[:4] != b'LGMD': return None
    ver = struct.unpack_from('<I', b, 4)[0]; o = 8 + 8 + 8 + 36
    npg, nv, nparm = struct.unpack_from('<3H', b, o); o += 6
    nmat, nvc, nvh, nobj = struct.unpack_from('<4B', b, o); o += 4
    off_objs, off_mats, off_uv, off_vhots, off_verts, off_light, off_norms, off_pgons, off_nodes, msize = struct.unpack_from('<10I', b, o)
    mats = {}
    for i in range(nmat):
        r = off_mats + 26 * i; mats[b[r + 17]] = b[r:r + 16].split(b'\0')[0].decode('latin1')
    verts = [struct.unpack_from('<3f', b, off_verts + 12 * i) for i in range(nv)]
    nuv = (off_vhots if off_vhots > off_uv else off_verts) - off_uv
    uvs = [struct.unpack_from('<2f', b, off_uv + 8 * i) for i in range(max(0, nuv // 8))]
    nnorm = (off_pgons - off_norms) // 12 if off_pgons > off_norms else 0
    norms = [struct.unpack_from('<3f', b, off_norms + 12 * i) for i in range(nnorm)]
    p = off_pgons; polys = []
    while p < off_nodes:
        idx, mat = struct.unpack_from('<2H', b, p); typ, n = struct.unpack_from('<2B', b, p + 4); ni, = struct.unpack_from('<H', b, p + 6)
        vs = struct.unpack_from('<%dH' % n, b, p + 12); q = p + 12 + 4 * n; uvi = None
        if (typ & 3) == 3: uvi = struct.unpack_from('<%dH' % n, b, q); q += 2 * n
        polys.append((mat, vs, uvi, ni)); p = q + (1 if ver >= 4 else 0)
    return dict(mats=mats, verts=verts, uvs=uvs, norms=norms, polys=polys)

def pcx(b):
    if len(b) < 128 + 769 or b[0] != 10: return None
    bpp, (x0, y0, x1, y1), planes, bpl = b[3], struct.unpack_from('<4H', b, 4), b[65], struct.unpack_from('<H', b, 66)[0]
    if bpp != 8 or planes != 1: return None
    w, h = x1 - x0 + 1, y1 - y0 + 1
    pal = b[-768:] if b[-769] == 12 else bytes(range(256)) * 3
    out = bytearray(); p = 128; row = bytearray()
    while len(out) < w * h * 4 and p < len(b) - 769:
        c = b[p]; p += 1; n = 1
        if c >= 0xC0: n = c & 0x3F; c = b[p]; p += 1
        for _ in range(n):
            row.append(c)
            if len(row) == bpl:
                for i in row[:w]: out += bytes((pal[3 * i], pal[3 * i + 1], pal[3 * i + 2], 255))
                row = bytearray()
    out += bytes(w * h * 4 - len(out)) if len(out) < w * h * 4 else b''
    return w, h, bytes(out[:w * h * 4])

def orient(m):
    """axes for the world model: forward (barrel), down (grip), right; grip point; muzzle"""
    V = m['verts']; ext = [max(v[k] for v in V) - min(v[k] for v in V) for k in range(3)]
    order = sorted(range(3), key=lambda k: -ext[k]); L, Hh, T = order          # long, height, thin
    def side(axis, s):                                    # vertices far out on one side of an axis
        lim = 0.35 * ext[axis]; return [v for v in V if s * v[axis] > lim]
    # down = the side of the height axis whose far vertices cover less of the length (the grip, not the slide/barrel)
    spread = {}
    for s in (1, -1):
        far = side(Hh, s); spread[s] = (max(v[L] for v in far) - min(v[L] for v in far)) if far else 0
    down = 1 if spread[1] < spread[-1] else -1
    grip = side(Hh, down) or V
    gl = sum(v[L] for v in grip) / len(grip)              # the grip sits towards the back
    fwd = -1 if gl > 0 else 1
    f = [0, 0, 0]; f[L] = fwd; d = [0, 0, 0]; d[Hh] = down
    r = [d[1] * f[2] - d[2] * f[1], d[2] * f[0] - d[0] * f[2], d[0] * f[1] - d[1] * f[0]]   # right = down x forward
    gspread = max(v[L] for v in grip) - min(v[L] for v in grip)
    rear0 = min(fwd * v[L] for v in V); gfrac = (fwd * gl - rear0) / ext[L]   # where the grip sits, 0 = the back end
    if gspread < 0.4 * ext[L] and gfrac > 0.12:           # a distinct grip (not the butt of a stock): hold its middle
        gp = [sum(v[k] for v in grip) / len(grip) for k in range(3)]; gp[Hh] *= 0.5
    else:                                                 # a long gun without one: a third of the way from the back
        rear = min(fwd * v[L] for v in V); gp = [0, 0, 0]; gp[L] = fwd * (rear + 0.33 * ext[L])
    gp[T] = 0
    top = side(Hh, -down) or V                            # the barrel line: the top part's height, the front end
    muz = [0, 0, 0]; muz[L] = fwd * max(fwd * v[L] for v in V); muz[Hh] = sum(v[Hh] for v in top) / len(top)
    return r, d, f, gp, muz, max(ext)

def to_gun(p, r, d, f, o):
    q = [p[k] - o[k] for k in range(3)]
    return (sum(q[k] * r[k] for k in range(3)), sum(q[k] * d[k] for k in range(3)), sum(q[k] * f[k] for k in range(3)))

def main():
    argv = sys.argv[1:]; prev = None
    if '--preview' in argv:
        i = argv.index('--preview'); prev = argv[i + 1] if i + 1 < len(argv) else None; del argv[i:i + 2]
    if len(argv) != 1 or not prev and '--preview' in sys.argv: sys.exit(__doc__)
    game = argv[0]
    crf = None
    for nm in ('Data/res/OBJ.CRF', 'Data/res/obj.crf', 'res/obj.crf', 'obj.crf'):
        if os.path.isfile(os.path.join(game, nm)): crf = os.path.join(game, nm); break
    if not crf: sys.exit('no OBJ.CRF under ' + game)
    z = zipfile.ZipFile(crf); names = {i.filename.upper(): i.filename for i in z.infolist()}
    tex, texidx = [], {}
    def texture(name):
        k = name.upper()
        if k in texidx: return texidx[k]
        t = None
        for cand in ('TXT16/' + k, 'TXT/' + k):
            if cand in names: t = pcx(z.read(names[cand])); break
        texidx[k] = len(tex) if t else 0xFFFF
        if t: tex.append((k, t))
        return texidx[k]
    worlds, windex = [], {}
    for k in sorted(n for n in names if n.endswith('_W.BIN')):
        m = lgmd(z.read(names[k]))
        if not m or len(m['polys']) < 8: continue
        stem = k[:-6]; r, d, f, gp, muz, longest = orient(m)
        s = LENGTH.get(stem, 0.6) / longest                          # metres per model unit
        verts, tris = [], []
        for mat, vs, uvi, ni in m['polys']:
            t = texture(m['mats'].get(mat, '')) if uvi else 0xFFFF
            n = m['norms'][ni] if ni < len(m['norms']) else (0, 0, 1)
            nn = to_gun(n, r, d, f, (0, 0, 0)); base = len(verts)
            for i, vi in enumerate(vs):
                p = to_gun(m['verts'][vi], r, d, f, gp); uv = m['uvs'][uvi[i]] if uvi and uvi[i] < len(m['uvs']) else (0, 0)
                verts.append((p[0] * s, p[1] * s, p[2] * s, uv[0], uv[1]) + nn)
            for i in range(1, len(vs) - 1): tris.append((base, base + i, base + i + 1, t))
        mz = to_gun(muz, r, d, f, gp)
        windex[stem] = len(worlds); worlds.append((stem, [c * s for c in mz], LENGTH.get(stem, 0.6), verts, tris))
    hands = []
    for k in sorted(n for n in names if n.endswith('_H.BIN')):
        m = lgmd(z.read(names[k]))
        if not m: continue
        stem = k[:-6]; wstem = H_TO_W.get(stem, stem)
        counts = {}
        for mat, vs, uvi, ni in m['polys']: counts[mat] = counts.get(mat, 0) + len(vs) - 2
        mats = []
        for mat, n in sorted(counts.items()):
            t = texture(m['mats'].get(mat, '')); w, h = (tex[t][1][0], tex[t][1][1]) if t != 0xFFFF else (0, 0)
            mats.append((w, h, n))
        hands.append((stem, windex.get(wstem, 0xFFFFFFFF), mats))
    out = bytearray(b'NDVW') + struct.pack('<I', 1)
    out += struct.pack('<I', len(tex))
    for name, (w, h, rgba) in tex: out += name.encode()[:31].ljust(32, b'\0') + struct.pack('<2I', w, h) + rgba
    out += struct.pack('<I', len(hands))
    for stem, wi, mats in hands:
        out += stem.encode()[:15].ljust(16, b'\0') + struct.pack('<2I', wi, len(mats))
        for w, h, n in mats: out += struct.pack('<3I', w, h, n)
    out += struct.pack('<I', len(worlds))
    for stem, mz, length, verts, tris in worlds:
        out += stem.encode()[:15].ljust(16, b'\0') + struct.pack('<4f', *mz, length) + struct.pack('<I', len(verts))
        for v in verts: out += struct.pack('<8f', *v)
        out += struct.pack('<I', len(tris))
        for t in tris: out += struct.pack('<4H', *t)
    os.makedirs(os.path.join(game, 'vr'), exist_ok=True)
    dst = os.path.join(game, 'vr', 'weapons.vrw')
    with open(dst, 'wb') as fo: fo.write(out)
    print('wrote %s: %d first-person models, %d world models, %d textures (%d KB)' % (dst, len(hands), len(worlds), len(tex), len(out) // 1024))
    for stem, wi, mats in hands: print('  %-8s -> %s' % (stem, worlds[wi][0] if wi != 0xFFFFFFFF else '(no world model)'))
    if prev:
        import matplotlib; matplotlib.use('Agg'); import matplotlib.pyplot as plt
        os.makedirs(prev, exist_ok=True)
        for stem, mz, length, verts, tris in worlds:
            fig, ax = plt.subplots(1, 2, figsize=(8, 3.2))
            for a, (i, j, lab) in zip(ax, ((2, 1, 'side: forward ->, down v'), (2, 0, 'top: forward ->, right v'))):
                for t in tris:
                    xs = [verts[t[k]][i] for k in (0, 1, 2, 0)]; ys = [verts[t[k]][j] for k in (0, 1, 2, 0)]; a.plot(xs, ys, 'k-', lw=0.3)
                a.plot([0], [0], 'ro'); a.plot([mz[i]], [mz[j]], 'b^'); a.set_aspect('equal'); a.invert_yaxis(); a.set_title(stem + ' ' + lab, fontsize=8)
            fig.savefig(os.path.join(prev, stem + '.png'), dpi=80); plt.close(fig)

if __name__ == '__main__': main()
