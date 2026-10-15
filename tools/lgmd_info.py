#!/usr/bin/env python3
"""lgmd_info.py MODEL.bin ...: what is in a Dark Engine object model (LGMD .bin, versions 3/4), e.g. a first-person
weapon (*_h.bin) or its world model (*_w.bin): polygon / vertex / material / sub-object counts, the bounding box, and
how open the mesh is (edges used by only one polygon, after merging vertices at the same place). No game data is
included in this repository; point it at files from your own install (obj/ inside the .crf archives, or a mod)."""
import struct, sys, collections
def parse(path):
    b = open(path,'rb').read()
    sig, ver = struct.unpack_from('<4sI', b, 0)
    o = 8
    name = b[o:o+8].split(b'\0')[0].decode('latin1'); o += 8
    rad, maxpr = struct.unpack_from('<2f', b, o); o += 8
    bmax = struct.unpack_from('<3f', b, o); o += 12
    bmin = struct.unpack_from('<3f', b, o); o += 12
    pcen = struct.unpack_from('<3f', b, o); o += 12
    npg, nv, nparm = struct.unpack_from('<3H', b, o); o += 6
    nmat, nvc, nvh, nobj = struct.unpack_from('<4B', b, o); o += 4
    offs = struct.unpack_from('<10I', b, o); o += 40
    off_objs, off_mats, off_uv, off_vhots, off_verts, off_light, off_norms, off_pgons, off_nodes, msize = offs
    verts = [struct.unpack_from('<3f', b, off_verts + 12*i) for i in range(nv)]
    # polygons: walk from off_pgons to off_nodes
    p = off_pgons; polys = []
    while p < off_nodes and len(polys) < 20000:
        idx, data = struct.unpack_from('<2H', b, p); typ, n = struct.unpack_from('<2B', b, p+4); norm, = struct.unpack_from('<H', b, p+6); d, = struct.unpack_from('<f', b, p+8)
        vs = struct.unpack_from('<%dH' % n, b, p+12); q = p + 12 + 2*n + 2*n
        if (typ & 3) == 3: q += 2*n      # textured: uv indices
        if ver >= 4: q += 1               # material index
        polys.append((typ, vs, data)); p = q
    return dict(sig=sig, ver=ver, name=name, rad=rad, bmin=bmin, bmax=bmax, npg=npg, nv=nv, nmat=nmat, nobj=nobj, polys=polys, verts=verts)
for path in sys.argv[1:]:
    m = parse(path)
    edges = collections.Counter()
    for t, vs, _ in m['polys']:
        for i in range(len(vs)): a, c = vs[i], vs[(i+1)%len(vs)]; edges[(min(a,c), max(a,c))] += 1
    # merge vertices at equal positions (models duplicate verts across uv seams)
    key = {i: tuple(round(x, 4) for x in v) for i, v in enumerate(m['verts'])}
    pe = collections.Counter()
    for t, vs, _ in m['polys']:
        for i in range(len(vs)): a, c = key.get(vs[i]), key.get(vs[(i+1)%len(vs)]); pe[tuple(sorted((a, c)))] += 1
    border = sum(1 for k, c in pe.items() if c == 1)
    print('%-60s %s v%d %-8s polys %d (header %d) verts %d mats %d objs %d  size x %.2f..%.2f y %.2f..%.2f z %.2f..%.2f  open edges %d of %d (%.0f%%)' % (
        path.split('uploads/')[-1], m['sig'].decode(), m['ver'], m['name'], len(m['polys']), m['npg'], m['nv'], m['nmat'], m['nobj'],
        m['bmin'][0], m['bmax'][0], m['bmin'][1], m['bmax'][1], m['bmin'][2], m['bmax'][2], border, len(pe), 100.0 * border / max(1, len(pe))))

# ---- sub-objects and materials
for path in sys.argv[1:]:
    b = open(path,'rb').read()
    nobj = b[8+8+8+36+6+3]; nmat = b[8+8+8+36+6]
    offs = struct.unpack_from('<10I', b, 8+8+8+36+6+4)
    off_objs, off_mats = offs[0], offs[1]
    names = [b[off_objs + 93*i: off_objs + 93*i + 8].split(b'\0')[0].decode('latin1') for i in range(nobj)]
    mats = []
    for i in range(nmat):
        r = b[off_mats + 26*i: off_mats + 26*i + 16].split(b'\0')[0].decode('latin1'); mats.append(r)
    print(path.split('obj/')[-1] if 'obj/' in path else path, 'subobjects', names, 'materials', mats)
