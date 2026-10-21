"""#128 tileset checker (2026-10-20). Reads every kit under a tilesets folder (kit.json + .glb pieces) and FAILS on:
  stretch   a triangle whose UV mapping is not isometric (singular-value ratio of the 3D -> UV map > 1.02)
  density   texels per metre off the batch target by more than 10 % (textures of one colour are exempt)
  normals   a closed part with inward faces (negative signed volume), or an open floor-like face looking down
  grid      a footprint that is not whole cells from the pivot (overhangs up to 0.5 m allowed), or a pivot off (0, 0)
  seams     cracks: unwelded vertices closer than 5 cm but not equal on open edges; road sockets whose cross-section
            differs from the straight road's (neighbouring tiles would leave a gap or a step)
    python tools/tilesets/check_tileset.py [work/tilesets] [--kit roads]
Prints one line per piece and FAIL lines; exit code 1 when anything fails."""
import json, sys, io
from pathlib import Path
import numpy as np
from PIL import Image
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'stage_editor'))
import import_geometry as ig

STRETCH_MAX, DENSITY_TOL, OVERHANG, CRACK = 1.02, 0.10, 0.5, 0.05


def load(glb):
    warnings = []
    mats, groups = ig.read_model([(glb.name, glb.read_bytes())], warnings)
    return mats, groups   # positions in metres, glTF axes (y up, game z = -Blender y ... = glTF z)


def tex_info(mat):
    img = mat.get('image')
    if not img: return None, True, (1, 1)
    im = Image.open(io.BytesIO(img)).convert('RGB'); a = np.asarray(im, float)
    return im.size[0], float(a.reshape(-1, 3).std(0).max()) < 2.0, im.size   # one colour: density does not show


def tri_metrics(V, U, T):
    """per triangle: stretch ratio and UV units per metre (isometric -> s1 == s2)"""
    P = V[T]; Q = U[T]
    e1, e2 = P[:, 1] - P[:, 0], P[:, 2] - P[:, 0]
    n = np.cross(e1, e2); area = np.linalg.norm(n, axis=1)
    ok = area > 1e-8
    x = e1 / (np.linalg.norm(e1, axis=1, keepdims=True) + 1e-12); y = np.cross(n / (area[:, None] + 1e-12), x)
    A = np.stack([np.stack([(e1 * x).sum(1), (e1 * y).sum(1)], 1), np.stack([(e2 * x).sum(1), (e2 * y).sum(1)], 1)], 1)   # 2x2 local
    B = np.stack([Q[:, 1] - Q[:, 0], Q[:, 2] - Q[:, 0]], 1)
    s_ratio = np.ones(len(T)); dens = np.zeros(len(T))
    for i in np.where(ok)[0]:
        try: J = np.linalg.solve(A[i], B[i])   # local metres -> uv
        except np.linalg.LinAlgError: continue
        s = np.linalg.svd(J, compute_uv=False)
        if s[1] > 1e-12: s_ratio[i] = s[0] / s[1]; dens[i] = float(np.sqrt(s[0] * s[1]))
    return s_ratio, dens, ok


def inside(pt, P, d=np.array([0.5773, 0.5774, 0.5775])):
    """ray parity (Moller-Trumbore, all triangles P[n, 3, 3]): True when pt is inside the closed solids"""
    e1, e2 = P[:, 1] - P[:, 0], P[:, 2] - P[:, 0]; h = np.cross(d, e2); a = (e1 * h).sum(1)
    ok = np.abs(a) > 1e-12; f = np.where(ok, 1.0 / np.where(ok, a, 1), 0); s_ = pt - P[:, 0]
    u = f * (s_ * h).sum(1); q = np.cross(s_, e1); v = f * (q * d).sum(1); t = f * (e2 * q).sum(1)
    hit = ok & (u >= 0) & (v >= 0) & (u + v <= 1) & (t > 1e-6)
    return int(hit.sum()) % 2 == 1


def winding(pt, P):
    """generalized winding number of point pt w.r.t. triangles P[n, 3, 3] (Van Oosterom - Strackee solid angles)"""
    A, B, Cc = P[:, 0] - pt, P[:, 1] - pt, P[:, 2] - pt
    a, b, c = np.linalg.norm(A, axis=1), np.linalg.norm(B, axis=1), np.linalg.norm(Cc, axis=1)
    num = np.einsum('ij,ij->i', A, np.cross(B, Cc))
    den = a * b * c + np.einsum('ij,ij->i', A, B) * c + np.einsum('ij,ij->i', A, Cc) * b + np.einsum('ij,ij->i', B, Cc) * a
    return float((2 * np.arctan2(num, den)).sum() / (4 * np.pi))


def components(V, T):
    """connected parts by shared (welded) positions -> list of triangle index arrays"""
    key = np.round(V, 4); _, inv = np.unique(key, axis=0, return_inverse=True); inv = inv.ravel()
    parent = list(range(len(T)))
    def find(i):
        while parent[i] != i: parent[i] = parent[parent[i]]; i = parent[i]
        return i
    owner = {}
    for t, tri in enumerate(T):
        for v in inv[tri]:
            if v in owner: parent[find(t)] = find(owner[v])
            else: owner[v] = t
    groups = {}
    for t in range(len(T)): groups.setdefault(find(t), []).append(t)
    return [np.array(g) for g in groups.values()], inv


def check_piece(kit_dir, piece, kit, ref_profile):
    fails = []; glb = kit_dir / piece['file']
    mats, groups = load(glb)
    cell = kit.get('cellMetres', 5.0); target = kit.get('textureSize', 1024) / (kit.get('unitsPerTexture', 50) / 10.0)
    allV = np.concatenate([g[0] for g in groups.values()])
    lo, hi = allV.min(0), allV.max(0)
    # glTF: x, y up, z = game z (the exporter's +Y up turns Blender -y into +z) -> the footprint is x / z
    foot = np.array(piece['cells'])[[0, 2]] * cell
    if lo[0] < -OVERHANG or lo[2] < -OVERHANG or hi[0] > foot[0] + OVERHANG or hi[2] > foot[1] + OVERHANG:
        fails.append(f"grid: bounds x {lo[0]:.2f}..{hi[0]:.2f} z {lo[2]:.2f}..{hi[2]:.2f} outside the {foot[0]:g} x {foot[1]:g} m footprint")
    if lo[1] < -0.01: fails.append(f'grid: below the floor ({lo[1]:.2f} m)')
    stats = []
    for name, (V, U, T) in groups.items():
        size, uniform, wh = tex_info(mats.get(name, {}))
        sr, dens, ok = tri_metrics(V, U * np.array(wh, float) / max(wh), T)   # pixel-space: a 1:2 picture on a 1:2 board is not stretched
        bad = np.where(ok & (sr > STRETCH_MAX))[0] if not name.endswith('_squeezed') else []   # the game's atlas stores it squeezed
        if len(bad): fails.append(f'stretch: {name} {len(bad)} triangle(s), worst {sr[bad].max():.3f}')
        if size and not uniform and not name.startswith('decal_'):   # decals: one picture per board (own density)
            d = dens[ok] * size; off = np.abs(d / target - 1)
            if (off > DENSITY_TOL).any(): fails.append(f'density: {name} {d[off > DENSITY_TOL].min():.0f}..{d[off > DENSITY_TOL].max():.0f} px/m (target {target:.0f})')
            stats.append(f'{name} {np.median(d):.0f}px/m')
    # normals by the generalized winding number w (sum of the triangles' signed solid angles / 4 pi; robust to touching
    # and overlapping boxes): just in front of a correct outward face w is about 0 (or the w of a neighbouring solid),
    # just behind it 1 more. Wrong: front more 'inside' than back (an inverted face); a floor-like face looking down with
    # no solid behind it (w behind < 0.75) is an exposed upside-down floor.
    Tall = np.concatenate([g[2] + o for g, o in zip(groups.values(), np.cumsum([0] + [len(g[0]) for g in groups.values()])[:-1])])
    Vall = np.concatenate([g[0] for g in groups.values()]); P = Vall[Tall]
    n = np.cross(P[:, 1] - P[:, 0], P[:, 2] - P[:, 0]); area = np.linalg.norm(n, axis=1); keep = area > 1e-9
    n = n[keep] / area[keep, None]; C = P[keep].mean(1)
    comps, cinv = components(Vall, Tall); closed = np.zeros(len(Tall), bool); closed_comps = []
    for ci in comps:   # parts whose every edge has two faces: their down faces are undersides of solids, not floors
        es = {}
        for t in cinv[Tall[ci]]:
            for a_, b_ in ((t[0], t[1]), (t[1], t[2]), (t[2], t[0])): e = (min(a_, b_), max(a_, b_)); es[e] = es.get(e, 0) + 1
        if es and all(c == 2 for c in es.values()): closed[ci] = True; closed_comps.append(ci)
    inv_bad = down_bad = 0
    for c, nn, cl in zip(C, n, closed[keep]):
        wf, wb = winding(c + nn * 0.01, P), winding(c - nn * 0.01, P)
        if wf > wb + 0.5: inv_bad += 1
        elif nn[1] < -0.7 and wb < 0.75 and not cl: down_bad += 1
    # orientation consistency: an edge shared by exactly two faces must be run in opposite directions by them (a flipped
    # face breaks that on its edges); edges with more faces (touching boxes) are skipped
    key = np.round(Vall, 4); ukey, wid = np.unique(key, axis=0, return_inverse=True); wid = wid.ravel()
    dir_count = {}; und = {}
    for t in wid[Tall]:
        for a_, b_ in ((t[0], t[1]), (t[1], t[2]), (t[2], t[0])):
            if a_ == b_: continue
            dir_count[(a_, b_)] = dir_count.get((a_, b_), 0) + 1; e = (min(a_, b_), max(a_, b_)); und[e] = und.get(e, 0) + 1
    flips = sum(1 for (a_, b_), c in dir_count.items() if und[(min(a_, b_), max(a_, b_))] == 2 and c == 2)
    for ci in closed_comps:   # a fully closed part must enclose positive volume (a box turned inside out)
        Q = Vall[Tall[ci]]
        if np.einsum('ij,ij->i', Q[:, 0], np.cross(Q[:, 1], Q[:, 2])).sum() < 0: inv_bad += len(ci)
    if flips: inv_bad += flips
    if inv_bad: fails.append(f'normals: {inv_bad} inverted face edge(s) / face(s) (neighbouring faces wound the same way or facing into the solid)')
    if down_bad: fails.append(f'normals: {down_bad} open floor-like face(s) look down')
    # cracks: vertices on open edges closer than CRACK but not equal (a seam that will show a gap) - across all
    # materials; closed parts (thin rails, boards) and their decals may sit cm apart without leaving a hole
    open_ids = {v for e, c in und.items() if c == 1 for v in e}
    U_ = ukey[np.array(sorted(open_ids), int)] if open_ids else np.zeros((0, 3)); cells = {}
    for i, v in enumerate(U_): cells.setdefault(tuple(np.floor(v / CRACK).astype(int)), []).append(i)
    cracks = 0
    for k, idx in cells.items():
        near = [j for dk in np.ndindex(3, 3, 3) for j in cells.get((k[0] + dk[0] - 1, k[1] + dk[1] - 1, k[2] + dk[2] - 1), [])]
        for i in idx:
            d = np.linalg.norm(U_[near] - U_[i], axis=1)
            cracks += int(((d > 2e-3) & (d < CRACK)).sum())
    if cracks: fails.append(f'seams: {cracks // 2} vertex pair(s) closer than {CRACK * 100:.0f} cm but not welded')
    return fails, stats, (lo, hi)


def road_profile(kit_dir, piece, side):
    """(lateral, height) pairs of the vertices on a road tile's socket edge (glTF: s = z 0, n = z L, w = x 0, e = x L)"""
    _, groups = load(kit_dir / piece['file']); L = piece['cells'][0] * 5.0
    out = set()
    for name, (V, U, T) in groups.items():
        if name not in ('asphalt', 'pavement', 'kerb'): continue   # the walkable surface; paint decals / walls do not join
        for x, y, z in V:
            if side == 's' and abs(z) < 1e-3: out.add((round(x, 3), round(y, 3)))
            if side == 'n' and abs(z - piece['cells'][2] * 5.0) < 1e-3: out.add((round(L - x, 3), round(y - (piece['sockets'][-1].get('height', 0) * 5.0 if side == 'n' and any(s.get('height') for s in piece['sockets']) else 0), 3)))
            if side == 'w' and abs(x) < 1e-3: out.add((round(L - z, 3), round(y, 3)))
            if side == 'e' and abs(x - L) < 1e-3: out.add((round(z, 3), round(y, 3)))
    return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    root = Path(args[0] if args else Path(__file__).resolve().parents[2] / 'work' / 'tilesets')
    only = sys.argv[sys.argv.index('--kit') + 1] if '--kit' in sys.argv else None
    total_fail = 0; n = 0
    for kj in sorted(root.glob('*/kit.json')):
        kit = json.loads(kj.read_text()); d = kj.parent
        if only and kit['kit'] != only: continue
        ref = None
        if kit['kit'] == 'roads':
            straight = next(p for p in kit['pieces'] if p['id'] == 'road_straight'); ref = road_profile(d, straight, 's')
        for piece in kit['pieces']:
            n += 1
            fails, stats, _ = check_piece(d, piece, kit, ref)
            if ref is not None:
                for s in piece.get('sockets', []):
                    if s.get('kind') != 'road': continue
                    prof = road_profile(d, piece, s['side'])
                    # every vertex height on the shared edge must match the straight road's profile at that lateral spot
                    refd = {}
                    for x, y in ref: refd.setdefault(x, set()).add(y)
                    miss = [(x, y) for x, y in prof if not any(abs(x - rx) < 1e-3 and y in ys for rx, ys in refd.items())]
                    gaps = [(rx, ys) for rx, ys in refd.items() if not any(abs(rx - x) < 1e-3 for x, _ in prof)]
                    if miss or gaps: fails.append(f"seams: socket {s['side']} differs from the straight road ({len(miss)} extra, {len(gaps)} missing edge points)")
            total_fail += bool(fails)
            print(f"{'FAIL' if fails else 'ok  '} {kit['kit']}/{piece['id']}  {' '.join(stats)}")
            for f in fails: print('      ', f)
    print(f'{n} pieces, {total_fail} failing')
    sys.exit(1 if total_fail else 0)


if __name__ == '__main__':
    main()
