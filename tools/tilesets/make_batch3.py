"""#128 tileset batch 3 (2026-10-20) - platforming and park pieces on batch 1's conventions
(tools/tilesets/make_batch1.py: grid 50 units, pivot = footprint min corner, planar UVs 1 repeat / 5 m, 1024 px
textures, outward normals, collision; street side = -b = game -z at yaw 0):
    "C:\\Program Files\\Blender Foundation\\Blender 4.3\\blender.exe" -b --factory-startup --python tools/tilesets/make_batch3.py -- [work/tilesets]
Kits: platforming (stairs, ramps of 14 / 27 / 45 degrees, blocks, floating ledges, stepping stones), rails (GRIND rails:
each piece carries a rail path in kit.json 'paths' that the server turns and places with the piece - straight, slope,
curve; pipes: straight, elbow, standing), scaffold (bay, deck), park (lawn, lawn with path, hedge, park tree,
fountain), structures (road tunnel, 40 m plaza).
New here: three textures are generated (project-made, tileable, same 1024 px / 5 m density): grass, water, wood
planks - the game has no plain lawn / water / plank surface that tiles; kit.json rows may set 'collision' (grind
rails: off, so the rail does not hold Shadow) and 'paths' (local metres a, b, c)."""
import json, math, os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bpy
import make_batch1 as B
import make_batch2 as B2   # its textures (metal, panel, foliage, leaves ...) and tree()
from mathutils import Vector
from make_batch1 import Piece, CELL, ROAD_W, T

GEN = os.path.join(B.OUT, '_generated')
IRON = os.path.join(B.CUSTOM, r'Stages\05 Iron Jungle (stg0502)\Level surfaces')
B.TEX.update({'hedge': os.path.join(IRON, 's0502_nn019 [tex1_128x128_15db0509a21187a9_14].png'),
              'grass': os.path.join(GEN, 'grass.png'), 'water': os.path.join(GEN, 'water.png'), 'wood': os.path.join(GEN, 'wood.png')})
LICENCE3 = ('game-texture (play\\game\\textures\\Custom remasters: Westopolis, Iron Jungle hedge; private use); grass / water / '
            'wood generated (project-made)')


# ------------------------------------------------------------------------------------------------ generated textures
def periodic_noise(n, modes, fmax, seed, aniso=(1.0, 1.0)):
    """tileable noise on an n x n grid: a sum of cosines with integer frequencies (amplitude ~ 1 / f)"""
    rng = np.random.default_rng(seed); y, x = np.mgrid[0:n, 0:n] / n; out = np.zeros((n, n))
    for _ in range(modes):
        fx, fy = rng.integers(-fmax, fmax + 1, 2)
        fx, fy = int(round(fx * aniso[0])), int(round(fy * aniso[1]))
        if fx == 0 and fy == 0: continue
        out += np.cos(2 * np.pi * (fx * x + fy * y) + rng.uniform(0, 2 * np.pi)) / math.hypot(fx, fy)
    out -= out.min(); return out / max(out.max(), 1e-9)


def save_png(name, rgb):
    n = rgb.shape[0]; img = bpy.data.images.new(name, n, n)
    rgba = np.concatenate([np.clip(rgb, 0, 1), np.ones((n, n, 1))], 2)[::-1]   # Blender rows go bottom-up
    img.pixels[:] = rgba.ravel().tolist(); img.filepath_raw = os.path.join(GEN, name + '.png'); img.file_format = 'PNG'; img.save()


def make_textures():
    os.makedirs(GEN, exist_ok=True); n = B.TEXSIZE
    lo, mid, hi = periodic_noise(n, 40, 6, 1), periodic_noise(n, 80, 40, 2), periodic_noise(n, 160, 200, 3)
    g = (0.55 * lo + 0.3 * mid + 0.15 * hi)[..., None]
    save_png('grass', np.array([0.16, 0.30, 0.08]) + g * np.array([0.16, 0.24, 0.08]))
    w = periodic_noise(n, 60, 8, 4); c = np.abs(np.sin(periodic_noise(n, 60, 10, 5) * 18.0)) ** 6
    save_png('water', (np.array([0.06, 0.22, 0.40]) + w[..., None] * np.array([0.05, 0.12, 0.16]) + c[..., None] * np.array([0.25, 0.32, 0.30])))
    rows = 20; ph = n / rows; plank = (np.arange(n)[:, None] * rows) // n; y = np.arange(n)[:, None] - plank * ph   # 20 planks over 5 m (tiles)
    rng = np.random.default_rng(7); tint = rng.uniform(0.85, 1.12, rows)[plank]
    grain = periodic_noise(n, 80, 60, 8, aniso=(0.08, 1.0))
    wood = (np.array([0.42, 0.27, 0.15]) * (tint * (0.8 + 0.35 * grain))[..., None])
    wood[(y < 3).repeat(n, 1)] *= 0.35   # the gaps between planks
    ends = rng.integers(0, n, rows)   # one butt joint per plank row
    for r in range(rows): wood[int(r * ph):int((r + 1) * ph), ends[r]:ends[r] + 3] *= 0.4
    save_png('wood', wood)


# ------------------------------------------------------------------------------------------------ helpers
def beam(p, mat, p0, p1, w, h, up=(0, 0, 1)):
    """an oriented closed box from p0 to p1 (centre line), w wide and h high"""
    p0, p1 = np.array(p0, float), np.array(p1, float); d = p1 - p0; d /= np.linalg.norm(d)
    u = np.array(up, float); s = np.cross(d, u)
    if np.linalg.norm(s) < 1e-6: s = np.cross(d, [1.0, 0, 0])
    s /= np.linalg.norm(s); u = np.cross(s, d)
    C = [[p0 + i * s * w / 2 + j * u * h / 2 for i in (-1, 1) for j in (-1, 1)], [p1 + i * s * w / 2 + j * u * h / 2 for i in (-1, 1) for j in (-1, 1)]]
    q = lambda e, k: tuple(C[e][k])
    for out, pts in ((-s, [q(0, 0), q(1, 0), q(1, 1), q(0, 1)]), (s, [q(0, 2), q(0, 3), q(1, 3), q(1, 2)]), (-u, [q(0, 0), q(0, 2), q(1, 2), q(1, 0)]),
                     (u, [q(0, 1), q(1, 1), q(1, 3), q(0, 3)]), (-d, [q(0, 0), q(0, 1), q(0, 3), q(0, 2)]), (d, [q(1, 0), q(1, 2), q(1, 3), q(1, 1)])):
        p.face(mat, pts, tuple(out))


def tube(p, mat, centres, r, sides=10, caps=True, tangents=None):
    """a closed tube through the centre points (a straight run or an arc: pass its exact tangents so every ring is
    square to the arc and every side quad stays flat), round section, end caps"""
    C = [np.array(c, float) for c in centres]; rings = []; Tn = []
    for i, c in enumerate(C):
        t = np.array(tangents[i], float) if tangents else (C[min(i + 1, len(C) - 1)] - C[max(i - 1, 0)]); t /= np.linalg.norm(t); Tn.append(t)
        ref = np.array([0, 0, 1.0]) if abs(t[2]) < 0.9 else np.array([1.0, 0, 0])
        s = np.cross(t, ref); s /= np.linalg.norm(s); u = np.cross(s, t)
        rings.append([c + r * (math.cos(2 * math.pi * k / sides) * s + math.sin(2 * math.pi * k / sides) * u) for k in range(sides)])
    for i in range(len(C) - 1):
        for k in range(sides):
            a, b_, c_, d_ = rings[i][k], rings[i][(k + 1) % sides], rings[i + 1][(k + 1) % sides], rings[i + 1][k]
            out = (a + b_ + c_ + d_) / 4 - (C[i] + C[i + 1]) / 2
            p.face(mat, [tuple(a), tuple(b_), tuple(c_), tuple(d_)], tuple(out))
    if caps:
        for i, sgn in ((0, -1), (len(C) - 1, 1)):
            p.face(mat, [tuple(v) for v in rings[i]], tuple(sgn * Tn[i]))


def merge(p, q, off):
    """add piece q's faces into p, moved by off = (a, b, c) metres"""
    sh = Vector((off[0], -off[1], off[2]))
    for mat, faces in q.faces.items():
        base = len(p.faces.get(mat, []))
        for i, f in enumerate(faces):
            if (mat, i) in q.uvs: p.uvs[(mat, base + i)] = q.uvs[(mat, i)]
            p.faces.setdefault(mat, []).append([v + sh for v in f])


def prism(p, mat, cx, cb, r, c0, c1, n=24, top=None, bottom=False):
    """an n-sided upright prism (round rims, discs, columns)"""
    ring = [(cx + r * math.cos(2 * math.pi * k / n), cb + r * math.sin(2 * math.pi * k / n)) for k in range(n)]
    for k in range(n):
        (a0, b0), (a1, b1) = ring[k], ring[(k + 1) % n]
        p.face(mat, [(a0, b0, c0), (a1, b1, c0), (a1, b1, c1), (a0, b0, c1)], ((a0 + a1) / 2 - cx, (b0 + b1) / 2 - cb, 0))
    p.face(top or mat, [(a, b, c1) for a, b in ring], (0, 0, 1))
    if bottom: p.face(mat, [(a, b, c0) for a, b in ring], (0, 0, -1))


# ------------------------------------------------------------------------------------------------ platforming
def stairs(width=1, run=2, rise=CELL, steps=25):
    """steps up toward +b over run cells, 'rise' metres in all"""
    p = Piece(); W, L = width * CELL, run * CELL; tr, rs = L / steps, rise / steps
    for i in range(steps):
        p.box('concrete', (0, i * tr, 0), (W, (i + 1) * tr, (i + 1) * rs), top='pavement', bottom=False)
    return p


def ramp(width=2, run=4, rise=CELL):
    """a solid wedge rising toward +b"""
    p = Piece(); W, L, H = width * CELL, run * CELL, rise
    p.face('pavement', [(0, 0, 0), (W, 0, 0), (W, L, H), (0, L, H)], (0, -H, L))
    p.face('concrete', [(0, 0, 0), (0, L, H), (0, L, 0)], (-1, 0, 0))
    p.face('concrete', [(W, 0, 0), (W, L, 0), (W, L, H)], (1, 0, 0))
    p.face('concrete', [(0, L, 0), (W, L, 0), (W, L, H), (0, L, H)], (0, 1, 0))
    return p


def block(h_cells=1):
    p = Piece(); p.box('concrete', (0, 0, 0), (CELL, CELL, h_cells * CELL), top='pavement', bottom=False); return p


def ledge(length=1, top_c=CELL, thick=0.5):
    p = Piece(); p.box('concrete', (0, 0, top_c - thick), (length * CELL, CELL, top_c), top='pavement', sides={'-b': 'trim', '+b': 'trim'}); return p


def stepping_stones():
    p = Piece()
    for k, h in enumerate((1.0, 2.0, 3.0)):
        a0 = k * CELL + 1.25; p.box('concrete', (a0, 1.25, 0), (a0 + 2.5, 3.75, h), top='pavement', bottom=False)
    return p


# ------------------------------------------------------------------------------------------------ rails and pipes
RAIL_R, PATH_UP = 0.06, 0.25   # grind rail tube radius; the rail path runs this far above the rail's centre line


def rail_piece(centres, posts, tangents=None):
    """a grind rail: tube + posts (no collision: the rail path carries Shadow) and its path (local metres)"""
    p = Piece(); tube(p, 'metal', centres, RAIL_R, sides=8, tangents=tangents)
    for a, b, c in posts: p.box('metal', (a - 0.05, b - 0.05, 0), (a + 0.05, b + 0.05, c - RAIL_R), bottom=False)
    p.paths = [dict(pathType='rail', name='grind rail', points=[[round(a, 3), round(b, 3), round(c + PATH_UP, 3)] for a, b, c in centres])]
    return p


def rail_straight():
    a = CELL / 2; return rail_piece([(a, 0.1, 1.0), (a, 4 * CELL - 0.1, 1.0)], [(a, 1.0, 1.0), (a, 10.0, 1.0), (a, 19.0, 1.0)])


def rail_slope():
    a = CELL / 2; L = 4 * CELL - 0.2; c = lambda b: 1.0 + 5.0 * (b - 0.1) / L
    return rail_piece([(a, 0.1, c(0.1)), (a, 4 * CELL - 0.1, c(4 * CELL - 0.1))], [(a, b, c(b)) for b in (1.0, 10.0, 19.0)])


def rail_curve():
    R, cx, cb = 7.5, 1.25, 1.25; n = 12
    ang = [math.pi / 2 * k / n for k in range(n + 1)]
    pts = [(cx + R * math.cos(t), cb + R * math.sin(t), 1.0) for t in ang]
    return rail_piece(pts, [pts[1], pts[6], pts[11]], tangents=[(-math.sin(t), math.cos(t), 0) for t in ang])


def pipe_straight():
    p = Piece(); a = CELL / 2
    tube(p, 'panel2', [(a, 0.0, 1.2), (a, 4 * CELL, 1.2)], 0.6, sides=12)
    for b in (2.5, 10.0, 17.5): p.box('metal', (a - 0.7, b - 0.3, 0), (a + 0.7, b + 0.3, 0.7), bottom=False)
    return p


def pipe_elbow():
    p = Piece(); R, n = 3.8, 8; cx, cb = 0.6, 0.6   # a quarter bend round (0.6, 0.6)
    ang = [math.pi / 2 * k / n for k in range(n + 1)]
    tube(p, 'panel2', [(cx + R * math.cos(t), cb + R * math.sin(t), 1.2) for t in ang], 0.6, sides=12, tangents=[(-math.sin(t), math.cos(t), 0) for t in ang])
    p.box('metal', (cx + R * 0.707 - 0.6, cb + R * 0.707 - 0.6, 0), (cx + R * 0.707 + 0.6, cb + R * 0.707 + 0.6, 0.7), bottom=False)
    return p


def pipe_vertical():
    p = Piece(); c = CELL / 2
    tube(p, 'panel2', [(c, c, 0.0), (c, c, 2 * CELL)], 0.7, sides=12)
    for z in (0.0, 4.0, 8.0): prism(p, 'metal', c, c, 0.85, z, z + 0.3, n=12, bottom=z > 0)
    return p


# ------------------------------------------------------------------------------------------------ scaffold
def scaffold(length=1):
    """poles at the corners, ledgers, a plank deck at 5 m you can stand on, a guard rail, one brace per bay"""
    p = Piece(); L, W, H, t = length * CELL, CELL, CELL, 0.08
    poles = [(a, b) for a in [x * CELL + (0.15 if x == 0 else -0.15 if x == length else 0) for x in range(length + 1)] for b in (0.15, W - 0.15)]
    for a, b in poles: p.box('metal', (a - t / 2, b - t / 2, 0), (a + t / 2, b + t / 2, H + 1.1), bottom=False)
    for c in (2.5, H - 0.12, H + 1.0):
        for b in (0.15, W - 0.15): p.box('metal', (0.15 - t / 2, b - t / 2, c - t / 2), (L - 0.15 + t / 2, b + t / 2, c + t / 2))
    p.box('wood', (0.1, 0.1, H - 0.08), (L - 0.1, W - 0.1, H), top='wood')
    for x in range(length):
        a0, a1 = x * CELL + 0.3, (x + 1) * CELL - 0.3
        beam(p, 'metal', (a0, W - 0.15 + t, 0.3), (a1, W - 0.15 + t, H - 0.4), t, t, up=(0, 1, 0))
    return p


# ------------------------------------------------------------------------------------------------ park
def lawn(path=False):
    p = Piece(); L = 4 * CELL
    if not path: p.ground('grass', [(0, 0), (L, 0), (L, L), (0, L)]); return p
    w0, w1 = L / 2 - 1.5, L / 2 + 1.5   # a 3 m paved path along b
    p.ground('grass', [(0, 0), (w0, 0), (w0, L), (0, L)]); p.ground('grass', [(w1, 0), (L, 0), (L, L), (w1, L)])
    p.ground('pavement', [(w0, 0), (w1, 0), (w1, L), (w0, L)]); return p


def hedge():
    p = Piece(); p.box('hedge', (0.2, 2.0, 0), (CELL - 0.2, 3.0, 1.2), top='hedge', bottom=False); return p


def park_tree():
    p = Piece(); p.ground('grass', [(0, 0), (CELL, 0), (CELL, CELL), (0, CELL)]); B2.tree(p, CELL / 2, CELL / 2, 0.0, 8.0); return p


def fountain():
    p = Piece(); c = CELL   # 2 x 2 cells, centre (5, 5)
    prism(p, 'concrete', c, c, 4.2, 0, 0.6, n=24, top='concrete')   # rim (solid disc: the water sits on it)
    prism(p, 'water', c, c, 3.8, 0.6, 0.62, n=24, top='water')      # water surface inside the rim's top
    prism(p, 'concrete', c, c, 0.5, 0.62, 2.0, n=12, top='concrete')
    prism(p, 'concrete', c, c, 1.4, 2.0, 2.3, n=16, top='water', bottom=True)
    return p


# ------------------------------------------------------------------------------------------------ structures
def tunnel():
    """the straight road (same socket profile) inside two walls and a roof; the walls stand just OUTSIDE the road's
    edge (0.5 m, the allowed overhang) so the sidewalks stay whole"""
    p = B.road_straight(); L = ROAD_W; o = 0.5
    for a0, a1 in ((-o, 0.0), (L, L + o)): p.box('concrete', (a0, 0, 0), (a1, L, 7.5), bottom=False)
    p.box('concrete', (-o, 0, 7.5), (L + o, L, 8.5), top='roof')
    for b0 in (2.0, 8.0, 14.0): p.box('lamp_glow', (L / 2 - 2, b0, 7.4), (L / 2 + 2, b0 + 2, 7.5))   # light strips under the roof
    return p


def plaza():
    p = Piece(); L = 8 * CELL; k = 0.3
    p.ground('pavement', [(k, k), (L - k, k), (L - k, L - k), (k, L - k)])
    for lo, hi in (((0, 0, 0), (L, k, 0.15)), ((0, L - k, 0), (L, L, 0.15)), ((0, k, 0), (k, L - k, 0.15)), ((L - k, k, 0), (L, L - k, 0.15))):
        p.box('kerb', lo, hi, bottom=False)
    prism(p, 'concrete', L / 2, L / 2, 6.0, 0, 0.2, n=32, top='concrete')
    for a, b in ((6, 6), (L - 6, 6), (6, L - 6), (L - 6, L - 6)): merge(p, B2.planter(), (a - 1, b - 1, 0))
    return p


KITS = {
    'platforming': ('Platforms and stairs', [
        ('stairs', 'Stairs, 5 m up over 10 m', stairs, ['platform', 'stairs'], [1, 1, 2], []),
        ('stairs_wide', 'Stairs, wide (10 m)', lambda: stairs(width=2), ['platform', 'stairs'], [2, 1, 2], []),
        ('ramp_14', 'Ramp 14 deg (5 m over 20 m)', lambda: ramp(2, 4), ['platform', 'ramp'], [2, 1, 4], []),
        ('ramp_27', 'Ramp 27 deg (5 m over 10 m)', lambda: ramp(2, 2), ['platform', 'ramp'], [2, 1, 2], []),
        ('ramp_45', 'Ramp 45 deg (5 m over 5 m)', lambda: ramp(2, 1), ['platform', 'ramp'], [2, 1, 1], []),
        ('block_5', 'Block, 5 m', block, ['platform'], [1, 1, 1], []),
        ('block_10', 'Block, 10 m', lambda: block(2), ['platform'], [1, 2, 1], []),
        ('ledge_1', 'Floating ledge, 5 m', ledge, ['platform', 'ledge'], [1, 1, 1], []),
        ('ledge_2', 'Floating ledge, 10 m', lambda: ledge(2), ['platform', 'ledge'], [2, 1, 1], []),
        ('stepping_stones', 'Stepping stones 1 / 2 / 3 m', stepping_stones, ['platform'], [3, 1, 1], [])]),
    'rails': ('Grind rails and pipes', [
        ('rail_straight', 'Grind rail, 20 m', rail_straight, ['rail', 'grind'], [1, 0, 4], [], {'collision': False}),
        ('rail_slope', 'Grind rail, 20 m rising 5 m', rail_slope, ['rail', 'grind'], [1, 1, 4], [], {'collision': False}),
        ('rail_curve', 'Grind rail, 90 deg curve', rail_curve, ['rail', 'grind'], [2, 0, 2], [], {'collision': False}),
        ('pipe_straight', 'Pipe, 20 m', pipe_straight, ['pipe'], [1, 0, 4], []),
        ('pipe_elbow', 'Pipe elbow', pipe_elbow, ['pipe'], [1, 0, 1], []),
        ('pipe_vertical', 'Standing pipe, 10 m', pipe_vertical, ['pipe'], [1, 2, 1], [])]),
    'scaffold': ('Scaffolding', [
        ('scaffold_bay', 'Scaffold bay (deck at 5 m)', scaffold, ['platform', 'scaffold'], [1, 1, 1], []),
        ('scaffold_2', 'Scaffold, 2 bays', lambda: scaffold(2), ['platform', 'scaffold'], [2, 1, 1], [])]),
    'park': ('Park', [
        ('lawn', 'Lawn, 20 x 20 m', lawn, ['ground', 'park'], [4, 0, 4], []),
        ('lawn_path', 'Lawn with a path', lambda: lawn(True), ['ground', 'park'], [4, 0, 4], []),
        ('hedge', 'Hedge, 5 m', hedge, ['prop', 'park'], [1, 0, 1], []),
        ('park_tree', 'Park tree on grass', park_tree, ['prop', 'park'], [1, 1, 1], []),
        ('fountain', 'Fountain', fountain, ['prop', 'park'], [2, 0, 2], [])]),
    'structures': ('Structures', [
        ('tunnel', 'Road tunnel, 20 m', tunnel, ['road', 'tunnel'], [4, 1, 4], [{'side': 's', 'kind': 'road'}, {'side': 'n', 'kind': 'road'}]),
        ('plaza', 'Plaza, 40 x 40 m', plaza, ['ground', 'plaza'], [8, 0, 8], [])]),
}


def build_kits3(kits, licence):
    """make_batch1.build_kits plus per-piece options: collision (default on) and the piece's paths (kit.json 'paths')"""
    for kit, (title, pieces) in kits.items():
        d = os.path.join(B.OUT, kit); texdir = os.path.join(d, 'textures'); os.makedirs(texdir, exist_ok=True); rows = []
        for row in pieces:
            pid, name, fn, tags, cells, sockets = row[:6]; opt = row[6] if len(row) > 6 else {}
            p = fn(); B.export(p, os.path.join(d, pid + '.glb'), texdir)
            allp = [v for fs in p.faces.values() for f in fs for v in f]
            lo = [min(v[i] for v in allp) for i in range(3)]; hi = [max(v[i] for v in allp) for i in range(3)]
            size = [round((hi[0] - lo[0]) * 10, 2), round((hi[2] - lo[2]) * 10, 2), round((hi[1] - lo[1]) * 10, 2)]
            r = dict(id=pid, name=name, file=pid + '.glb', cells=cells, size=size, pivot='min-corner', tags=tags,
                     collision=opt.get('collision', True), surface=0x00020001, sockets=sockets, materials=sorted(p.faces), licence=licence)
            if getattr(p, 'paths', None): r['paths'] = p.paths
            rows.append(r); print('PIECE', kit, pid, size, flush=True)
        json.dump(dict(kit=kit, title=title, grid=50, cellMetres=CELL, unitsPerTexture=B.TILE * 10, textureSize=B.TEXSIZE, pieces=rows),
                  open(os.path.join(d, 'kit.json'), 'w'), indent=1)
    print('BATCH_OK', B.OUT)


if __name__ == '__main__':
    make_textures()
    build_kits3(KITS, LICENCE3)
