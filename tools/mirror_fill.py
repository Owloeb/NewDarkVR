"""Complete a first-person model by mirroring it across its own centre plane.
The first-person weapons were only modelled on the side the player sees (their left, plus top and bottom); the
other side is missing. A gun is close to symmetric, so the plane that maps the most vertices onto other vertices
is found (vertical, through the barrel line), and every polygon on the modelled side is copied mirrored onto the
missing side, with the same texture. Hand and arm materials are not copied (that would grow a second hand)."""
import numpy as np

SKIN = ('HAND', 'FIST', 'ARM', 'SLEEVE')

def plane(model, keep):
    V = np.array([model['verts'][i] for i in keep], float)
    if len(V) < 8: return None
    from scipy.spatial import cKDTree
    t = cKDTree(V); span = np.ptp(V, 0).max(); eps = span * 0.01
    best = None
    for th in np.radians(np.arange(-20, 20.01, 0.5)):
        n = np.array([-np.sin(th), np.cos(th), 0.0])          # plane normal: the gun's right/left direction
        s = V @ n
        for d in np.linspace(s.min(), s.max(), 81):
            M = V - 2 * np.outer(s - d, n)
            hits = (t.query(M, distance_upper_bound=eps)[0] < eps)
            score = hits[np.abs(s - d) > eps].sum()            # vertices off the plane that land on another vertex
            if best is None or score > best[0]: best = (score, n, d)
    return best

def complete(model, material_names):
    skin = {m for m, n in material_names.items() if n.upper().startswith(SKIN)}
    keep = sorted({i for mat, vs, uvi, ni in model['polys'] if mat not in skin for i in vs})
    b = plane(model, keep)
    if not b: return model, None
    score, n, d = b
    if score < 12: return model, None                          # nothing symmetric to go on (or already whole)
    V = list(model['verts']); polys = list(model['polys'])
    P = np.array(V, float); span = np.ptp(P, 0).max()
    from scipy.spatial import cKDTree
    cents = np.array([P[list(vs)].mean(0) for mat, vs, uvi, ni in model['polys']]); ct = cKDTree(cents)
    for k, (mat, vs, uvi, ni) in enumerate(model['polys']):
        if mat in skin: continue
        Q = P[list(vs)]; s = Q @ n - d
        if np.abs(s).max() < 0.02 * span: continue               # a flat part on the centre line (a stock, a sight)
        mc = cents[k] - 2 * (cents[k] @ n - d) * n
        if ct.query(mc)[0] < 0.01 * span: continue               # its mirror image is already modelled
        base = len(V)
        for i in vs:
            p = np.array(V[i]); s = p @ n - d; V.append(tuple(p - 2 * s * n))
        polys.append((mat, tuple(range(base, base + len(vs)))[::-1], uvi[::-1] if uvi else None, ni))
    m2 = dict(model); m2['verts'] = V; m2['polys'] = polys
    return m2, (np.degrees(np.arctan2(-n[0], n[1])), d, score, len(keep))
