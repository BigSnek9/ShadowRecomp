"""#128 tileset batch 1 (2026-10-20), built IN BLENDER headless (factory settings - the user's Blender setup is not
read or changed):
    "C:\\Program Files\\Blender Foundation\\Blender 4.3\\blender.exe" -b --factory-startup --python tools/tilesets/make_batch1.py -- <out dir>
Default out dir: work/tilesets. Writes <out>/<kit>/kit.json + one .glb per piece (textures embedded) + textures/ (the
copies used). Textures: play\\textures\\Custom (the user's remastered game textures, READ only - copied, resized to
1024 so every material has the same texel density); road paint colours are generated here.
Conventions (agreed with Shadow, #128): grid cell 5 m = 50 game units; every footprint is whole cells; pivot = the
footprint's MIN corner on the floor - the piece spans +x, +y (up), +z in game units (Blender: x, -y, z); 90 deg turns.
UVs: each face is unwrapped in its own plane (no stretching), one texture repeat per 5 m (TILE) for every material, so
neighbouring pieces continue the texture across the grid lines. Normals face out (closed solids) or up (ground).
Every piece has collision (the importer default); kit.json lists sizes, tags, sockets and the texture licence."""
import json, math, os, shutil, sys, glob
import bpy, bmesh
from mathutils import Vector

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))   # the project folder
OUT = sys.argv[sys.argv.index('--') + 1] if '--' in sys.argv else os.path.join(ROOT, 'work', 'tilesets')
CUSTOM = next((p for p in (os.path.join(ROOT, r'play\game\textures\Custom'), os.path.join(ROOT, r'play\textures\Custom')) if os.path.isdir(p)),
              os.path.join(ROOT, r'play\game\textures\Custom'))   # #138 moved the game into play\game (only read here)
SRC = os.path.join(CUSTOM, r'Stages\01 Westopolis (stg0100)\Level surfaces')
CELL = 5.0          # metres (= 50 game units)
TILE = 5.0          # metres per texture repeat, every material
TEXSIZE = 1024      # every texture copy is this square size -> one texel density for the whole batch
TEX = {   # material -> source file (prefix in Level surfaces) or a generated colour
    'asphalt': 'Road/s0100_nn005', 'pavement': 'Road/s0100_nn003', 'kerb': 'Road/s0100_nn019', 'concrete': 's0100_nn044',
    'office': 's0100_nn016', 'glass': 'Houses/s0100_nn015', 'office2': 'Houses/s0100_nn014', 'night': 's0100_nn034',
    'brick': 's0100_nn041', 'brickwin': 's0100_nn080', 'housewin': 's0100_nn057', 'door': 'Houses/s0100_nn010',
    'trim': 's0100_nn068', 'cornice': 's0100_nn027', 'metal': 's0100_nn031', 'roof': 's0100_nn024', 'plaster': 's0100_nn081',
    'sign': 's0101wn009', 'billboard': 's0101wn008',
    'paint_yellow': (0.95, 0.78, 0.15), 'paint_white': (0.93, 0.93, 0.9), 'lamp_glow': (1.0, 0.95, 0.75)}


# ------------------------------------------------------------------------------------------------ textures / materials
def tex_path(name):
    v = TEX[name]
    if isinstance(v, tuple): return None
    if os.path.isabs(v): return v   # batch 2: files from other folders (Objects and props)
    c = [f for f in glob.glob(os.path.join(SRC, v + ' *.png'))]
    if not c: c = [f for f in glob.glob(os.path.join(SRC, '**', os.path.basename(v) + ' *.png'), recursive=True)]
    if not c: raise SystemExit(f'texture {v} not found in {SRC}')
    return sorted(c)[0]


IMAGES = {}
def image(name, texdir):
    if name in IMAGES: return IMAGES[name]
    dst = os.path.join(texdir, name + '.png')
    v = TEX[name]
    if isinstance(v, tuple):
        img = bpy.data.images.new(name, 64, 64); img.pixels[:] = list(v + (1.0,)) * (64 * 64)
    else:
        img = bpy.data.images.load(tex_path(name))
        if not name.startswith('decal_'): img.scale(TEXSIZE, TEXSIZE)   # same size for all = same density (decals keep their own)
        else: img.scale(*img.size)   # loads the pixels (save needs them)
    img.filepath_raw = dst; img.file_format = 'PNG'; img.save(); img.name = name
    IMAGES[name] = img; return img


MATS = {}
def material(name, texdir):
    if name in MATS: return MATS[name]
    m = bpy.data.materials.new(name); m.use_nodes = True; bsdf = m.node_tree.nodes['Principled BSDF']
    t = m.node_tree.nodes.new('ShaderNodeTexImage'); t.image = image(name, texdir)
    m.node_tree.links.new(t.outputs['Color'], bsdf.inputs['Base Color'])
    if name in ALPHA:   # cut-out (fence mesh, leaves): the PNG's alpha, exported as glTF alphaMode MASK
        m.node_tree.links.new(t.outputs['Alpha'], bsdf.inputs['Alpha']); m.blend_method = 'CLIP'
    if name == 'lamp_glow': bsdf.inputs['Emission Strength'].default_value = 0.0
    MATS[name] = m; return m


# ------------------------------------------------------------------------------------------------ geometry helpers
ALPHA = set()   # materials drawn as cut-outs (batch 2: fence mesh, leaves)


class Piece:
    """faces per material in GAME-style local coords (a = x, b = depth (game +z), c = height); Blender = (a, -b, c)"""
    def __init__(self): self.faces = {}; self.uvs = {}

    def face(self, mat, pts, out, uvs=None):
        """pts in (a, b, c); out = a direction the face must look toward (its outside)"""
        P = [Vector((a, -b, c)) for a, b, c in pts]
        o = Vector((out[0], -out[1], out[2]))
        n = (P[1] - P[0]).cross(P[2] - P[0])
        if n.dot(o) < 0: P = P[::-1]; uvs = uvs[::-1] if uvs else uvs
        if uvs: self.uvs[(mat, len(self.faces.get(mat, [])))] = uvs   # decals: the whole picture on the face
        self.faces.setdefault(mat, []).append(P)

    def box(self, mat, lo, hi, top=None, bottom=True, sides=None, skip=()):
        """closed box (outward); sides = {'-a','+a','-b','+b'} -> material per side; skip = sides left open"""
        (a0, b0, c0), (a1, b1, c1) = lo, hi; s = sides or {}
        F = {'-a': ([(a0, b0, c0), (a0, b1, c0), (a0, b1, c1), (a0, b0, c1)], (-1, 0, 0)),
             '+a': ([(a1, b0, c0), (a1, b1, c0), (a1, b1, c1), (a1, b0, c1)], (1, 0, 0)),
             '-b': ([(a0, b0, c0), (a1, b0, c0), (a1, b0, c1), (a0, b0, c1)], (0, -1, 0)),
             '+b': ([(a0, b1, c0), (a1, b1, c0), (a1, b1, c1), (a0, b1, c1)], (0, 1, 0)),
             '+c': ([(a0, b0, c1), (a1, b0, c1), (a1, b1, c1), (a0, b1, c1)], (0, 0, 1)),
             '-c': ([(a0, b0, c0), (a1, b0, c0), (a1, b1, c0), (a0, b1, c0)], (0, 0, -1))}
        for k, (pts, out) in F.items():
            if k in skip or (k == '-c' and not bottom): continue
            self.face(top if k == '+c' and top else s.get(k, mat), pts, out)

    def ground(self, mat, pts, c=0.0):
        self.face(mat, [(a, b, c) for a, b in pts], (0, 0, 1))


def uv_for(face):
    """isometric planar unwrap: floors use (x, forward), walls (horizontal, up); one repeat per TILE metres"""
    n = (face[1] - face[0]).cross(face[2] - face[0]).normalized()
    ref = Vector((0, -1, 0)) if abs(n.z) > 0.7 else Vector((0, 0, 1))   # forward (game +z) on floors, up on walls
    t2 = (ref - n * n.dot(ref)).normalized(); t1 = t2.cross(n).normalized()
    return [(p.dot(t1) / TILE, p.dot(t2) / TILE) for p in face]


def export(piece, path, texdir):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    IMAGES.clear(); MATS.clear()
    for mat, faces in piece.faces.items():
        bm = bmesh.new(); uvl = bm.loops.layers.uv.verify()
        for i, f in enumerate(faces):
            vs = [bm.verts.new(p) for p in f]; bf = bm.faces.new(vs)
            for l, uv in zip(bf.loops, piece.uvs.get((mat, i)) or uv_for(f)): l[uvl].uv = uv
        bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=1e-4)
        me = bpy.data.meshes.new(mat); bm.to_mesh(me); bm.free()
        ob = bpy.data.objects.new(mat, me); bpy.context.collection.objects.link(ob); me.materials.append(material(mat, texdir))
    bpy.ops.export_scene.gltf(filepath=path, export_format='GLB', use_selection=False, export_yup=True)


# ------------------------------------------------------------------------------------------------ road kit (20 m tiles)
ROAD_W = 4 * CELL   # 20 m: sidewalk 3 + road 14 + sidewalk 3
SW, KH = 3.0, 0.25  # sidewalk width, kerb height


def sidewalk(p, a0, a1, b0, b1, kerb_sides):
    p.box('pavement', (a0, b0, 0), (a1, b1, KH), sides={k: 'kerb' for k in kerb_sides}, bottom=False)


def dashes(p, a, b0, b1, mat='paint_white', w=0.15, every=5.0, length=2.5):
    b = b0
    while b + length <= b1 + 1e-6:
        p.ground(mat, [(a - w / 2, b), (a + w / 2, b), (a + w / 2, b + length), (a - w / 2, b + length)], 0.01); b += every


def double_line(p, a, b0, b1):
    for off in (-0.15, 0.15):
        p.ground('paint_yellow', [(a + off - 0.06, b0), (a + off + 0.06, b0), (a + off + 0.06, b1), (a + off - 0.06, b1)], 0.01)


def road_straight(crossing=False):
    p = Piece(); L = ROAD_W
    p.ground('asphalt', [(SW, 0), (L - SW, 0), (L - SW, L), (SW, L)])
    sidewalk(p, 0, SW, 0, L, ['+a']); sidewalk(p, L - SW, L, 0, L, ['-a'])
    if crossing:
        for k in range(7):   # zebra across the road at the tile's middle, double line broken there
            a0 = SW + 0.5 + k * 1.9
            p.ground('paint_white', [(a0, 7.5), (a0 + 1.1, 7.5), (a0 + 1.1, 12.5), (a0, 12.5)], 0.01)
        double_line(p, L / 2, 0, 6.5); double_line(p, L / 2, 13.5, L)
    else:
        double_line(p, L / 2, 0, L)
    return p


def road_cross(t=False):
    p = Piece(); L = ROAD_W
    p.ground('asphalt', [(SW, 0), (L - SW, 0), (L - SW, L), (SW, L)])   # north-south carriageway
    p.ground('asphalt', [(0, SW), (SW, SW), (SW, L - SW), (0, L - SW)]) if not t else None   # west arm (cross only)
    p.ground('asphalt', [(L - SW, SW), (L, SW), (L, L - SW), (L - SW, L - SW)])                # east arm
    for a0, b0 in ((0, 0), (L - SW, 0), (0, L - SW), (L - SW, L - SW)):                       # corner squares
        sidewalk(p, a0, a0 + SW, b0, b0 + SW, ['+a' if a0 == 0 else '-a', '+b' if b0 == 0 else '-b'])
    if t: sidewalk(p, 0, SW, SW, L - SW, ['+a'])                                                 # T: west side closed
    dashes(p, L / 2, 0, SW, length=2.5)   # stubs of the centre line into the junction
    return p


def road_curve():
    """90 deg turn inside one 20 x 20 tile, from the south edge (b = 0) to the east edge (a = L), round the corner
    (L, 0): inner sidewalk r 0..3, road 3..17, outer sidewalk 17..20, the far corner (0, L) paved at kerb height"""
    p = Piece(); L = ROAD_W; N = 12; K = (L, 0)
    ring = lambda r, k: (L - r * math.cos(math.pi / 2 * k / N), r * math.sin(math.pi / 2 * k / N))
    for k in range(N):
        a, b = ring(SW, k), ring(SW, k + 1); c, d = ring(L - SW, k), ring(L - SW, k + 1); e, f = ring(L, k), ring(L, k + 1)
        p.ground('asphalt', [a, b, d, c])
        p.ground('pavement', [c, d, f, e], KH)
        p.ground('pavement', [K, a, b], KH)                       # inner quarter disc
        p.ground('pavement', [(0, L), e, f], KH)                  # beyond the outer sidewalk, up to the tile corner
        mid = lambda u, v: ((u[0] + v[0]) / 2, (u[1] + v[1]) / 2)
        m = mid(a, b); p.face('kerb', [(*a, 0), (*b, 0), (*b, KH), (*a, KH)], (m[0] - K[0], m[1] - K[1], 0))   # inner kerb faces the road (away from K)
        m = mid(c, d); p.face('kerb', [(*c, 0), (*d, 0), (*d, KH), (*c, KH)], (K[0] - m[0], K[1] - m[1], 0))
        for off in (-0.15, 0.15):
            r0, r1 = L / 2 + off - 0.06, L / 2 + off + 0.06
            p.ground('paint_yellow', [ring(r0, k), ring(r0, k + 1), ring(r1, k + 1), ring(r1, k)], 0.01)
    return p


def road_ramp():
    """20 m wide, 40 m long, rising 5 m (one cell) - low walls on both sides"""
    p = Piece(); L, D, H = ROAD_W, 8 * CELL, CELL
    p.face('asphalt', [(SW, 0, 0), (L - SW, 0, 0), (L - SW, D, H), (SW, D, H)], (0, -0.1, 1))
    for a0, a1 in ((0, SW), (L - SW, L)):   # sidewalks follow the slope at kerb height
        p.face('pavement', [(a0, 0, KH), (a1, 0, KH), (a1, D, H + KH), (a0, D, H + KH)], (0, -0.1, 1))
        inner = a1 if a0 == 0 else a0; outer = a0 if a0 == 0 else a1
        p.face('kerb', [(inner, 0, 0), (inner, D, H), (inner, D, H + KH), (inner, 0, KH)], (1 if a0 == 0 else -1, 0, 0))
        p.face('concrete', [(outer, 0, 0), (outer, D, 0), (outer, D, H + KH), (outer, 0, KH)], (-1 if a0 == 0 else 1, 0, 0))
    p.face('concrete', [(0, D, 0), (L, D, 0), (L, D, H), (0, D, H)], (0, 1, 0))
    for off in (-0.15, 0.15):   # the centre line follows the slope
        a = L / 2 + off
        p.face('paint_yellow', [(a - 0.06, 0, 0.01), (a + 0.06, 0, 0.01), (a + 0.06, D, H + 0.01), (a - 0.06, D, H + 0.01)], (0, -0.1, 1))
    return p


def kerb_piece():
    """a straight sidewalk + kerb module 5 m x 3 m (to edge plazas and dead ends)"""
    p = Piece(); sidewalk(p, 0, CELL, 0, SW, ['+b']); return p


# ------------------------------------------------------------------------------------------------ building kit (5 m)
T = 0.5   # wall thickness


def wall(face_mat='brick', opening=None):
    """5 x 5 m wall panel on the cell's -b edge, outside facing -b (the street); opening = 'window' | 'door'"""
    p = Piece(); C = CELL
    if opening is None:
        p.box('plaster', (0, 0, 0), (C, T, C), sides={'-b': face_mat})
    else:
        w0, w1, h0, h1 = (1.5, 3.5, 1.5, 3.5) if opening == 'window' else (1.5, 3.5, 0.0, 3.5)
        for lo, hi in (((0, 0, 0), (w0, T, C)), ((w1, 0, 0), (C, T, C)), ((w0, 0, h1), (w1, T, C))) + ((((w0, 0, 0), (w1, T, h0)),) if h0 > 0 else ()):
            p.box('plaster', lo, hi, sides={'-b': face_mat})
        inset = 0.2   # the window / door panel set back in the opening
        p.face('housewin' if opening == 'window' else 'door', [(w0, inset, h0), (w1, inset, h0), (w1, inset, h1), (w0, inset, h1)], (0, -1, 0))
    return p


def corner():
    p = Piece(); p.box('brick', (0, 0, 0), (T, T, CELL), top='concrete'); return p


def floor_slab():
    p = Piece(); p.box('concrete', (0, 0, 0), (CELL, CELL, 0.3)); return p


def roof_flat():
    p = Piece(); p.box('concrete', (0, 0, 0), (CELL, CELL, 0.3), top='roof')
    p.box('concrete', (0, 0, 0.3), (CELL, T, 1.2)); return p   # parapet on the street edge


def trim():
    p = Piece(); p.box('trim', (0, -0.3, 0), (CELL, T, 0.6), top='concrete'); return p   # cornice strip on a wall top (sticks out 0.3)


# ------------------------------------------------------------------------------------------------ skyscrapers / houses
def tower(face, h, crown=True):
    p = Piece(); W = 4 * CELL
    p.box(face, (0, 0, 0), (W, W, h), top='roof')
    p.box('concrete', (0, 0, h), (W, T, h + 1.2)); p.box('concrete', (0, W - T, h), (W, W, h + 1.2))
    p.box('concrete', (0, T, h), (T, W - T, h + 1.2)); p.box('concrete', (W - T, T, h), (W, W - T, h + 1.2))
    p.box('trim', (-0.3, -0.3, 5.0), (W + 0.3, W + 0.3, 5.6), top='concrete')   # canopy / cornice over the ground floor
    if crown: p.box('metal', (W / 2 - 2.5, W / 2 - 2.5, h), (W / 2 + 2.5, W / 2 + 2.5, h + CELL), top='concrete')
    return p


def house(cells_a, cells_b, storeys, face, gable=True):
    p = Piece(); A, B, H = cells_a * CELL, cells_b * CELL, storeys * CELL
    p.box(face, (0, 0, 0), (A, B, H), top='concrete')
    p.face('door', [(A / 2 - 1, -0.01, 0), (A / 2 + 1, -0.01, 0), (A / 2 + 1, -0.01, 3.5), (A / 2 - 1, -0.01, 3.5)], (0, -1, 0))
    if gable:   # pitched roof along a, ridge at B / 2, 3 m high, overhang 0
        R = 3.0
        p.face('roof', [(0, 0, H), (A, 0, H), (A, B / 2, H + R), (0, B / 2, H + R)], (0, -1, 1))
        p.face('roof', [(0, B, H), (A, B, H), (A, B / 2, H + R), (0, B / 2, H + R)], (0, 1, 1))
        p.face(face, [(0, 0, H), (0, B, H), (0, B / 2, H + R)], (-1, 0, 0)); p.face(face, [(A, 0, H), (A, B, H), (A, B / 2, H + R)], (1, 0, 0))
    return p


# ------------------------------------------------------------------------------------------------ props (in one cell)
def lamp():
    p = Piece(); c = CELL / 2
    p.box('metal', (c - 0.12, c - 0.12, 0), (c + 0.12, c + 0.12, 6.0))
    p.box('metal', (c - 0.08, c - 1.6, 5.8), (c + 0.08, c + 0.08, 6.0))
    p.box('lamp_glow', (c - 0.3, c - 1.9, 5.5), (c + 0.3, c - 1.3, 5.8), top='metal'); return p


def bench():
    p = Piece(); c = CELL / 2
    p.box('metal', (c - 1.0, c - 0.3, 0), (c - 0.9, c + 0.3, 0.45)); p.box('metal', (c + 0.9, c - 0.3, 0), (c + 1.0, c + 0.3, 0.45))
    p.box('plaster', (c - 1.1, c - 0.3, 0.45), (c + 1.1, c + 0.3, 0.55))
    p.box('plaster', (c - 1.1, c + 0.3, 0.55), (c + 1.1, c + 0.4, 1.0)); return p   # back rest flush with the seat's back edge


def sign():
    p = Piece(); c = CELL / 2
    p.box('metal', (c - 0.1, c - 0.1, 0), (c + 0.1, c + 0.1, 3.0))
    p.box('metal', (c - 1.0, c - 0.05, 3.0), (c + 1.0, c + 0.05, 4.2), sides={'-b': 'sign', '+b': 'sign'}); return p


def barrier():
    """concrete jersey barrier, 5 m long along a, wedge-shaped section"""
    p = Piece(); C = CELL; m = 'concrete'
    sec = [(1.5, 0.0), (3.5, 0.0), (3.1, 0.35), (2.75, 0.9), (2.25, 0.9), (1.9, 0.35)]   # (b, c) around the cell centre line
    for (b0, c0), (b1, c1) in zip(sec, sec[1:] + sec[:1]):
        p.face(m, [(0, b0, c0), (C, b0, c0), (C, b1, c1), (0, b1, c1)], (0, (b0 + b1) / 2 - 2.5, (c0 + c1) / 2 - 0.4))   # away from the section's centre
    for a, out in ((0, -1), (C, 1)):
        p.face(m, [(a, b, c) for b, c in sec], (out, 0, 0))
    return p


# ------------------------------------------------------------------------------------------------ the batch
KITS = {
    'roads': ('Road kit', [
        ('road_straight', 'Road, straight', road_straight, ['road'], [4, 0, 4], [{'side': 's', 'kind': 'road'}, {'side': 'n', 'kind': 'road'}]),
        ('road_curve', 'Road, 90 degree curve', road_curve, ['road'], [4, 0, 4], [{'side': 's', 'kind': 'road'}, {'side': 'e', 'kind': 'road'}]),
        ('road_t', 'Road, T junction', lambda: road_cross(True), ['road'], [4, 0, 4], [{'side': s, 'kind': 'road'} for s in 'sne']),
        ('road_cross', 'Road, crossroads', road_cross, ['road'], [4, 0, 4], [{'side': s, 'kind': 'road'} for s in 'snew']),
        ('road_crossing', 'Road, zebra crossing', lambda: road_straight(True), ['road'], [4, 0, 4], [{'side': 's', 'kind': 'road'}, {'side': 'n', 'kind': 'road'}]),
        ('road_ramp', 'Road, ramp (up one cell)', road_ramp, ['road', 'ramp'], [4, 1, 8], [{'side': 's', 'kind': 'road'}, {'side': 'n', 'kind': 'road', 'height': 1}]),
        ('kerb', 'Sidewalk + kerb, 5 m', kerb_piece, ['road', 'kerb'], [1, 0, 1], [])]),
    'building': ('Building kit', [
        ('wall_brick', 'Wall, brick', lambda: wall('brick'), ['building', 'wall'], [1, 1, 1], [{'side': 's', 'kind': 'wall'}]),
        ('wall_office', 'Wall, office', lambda: wall('office'), ['building', 'wall'], [1, 1, 1], [{'side': 's', 'kind': 'wall'}]),
        ('wall_window', 'Wall with window', lambda: wall('brick', 'window'), ['building', 'wall', 'window'], [1, 1, 1], [{'side': 's', 'kind': 'wall'}]),
        ('wall_door', 'Wall with door', lambda: wall('brick', 'door'), ['building', 'wall', 'door'], [1, 1, 1], [{'side': 's', 'kind': 'wall'}]),
        ('corner', 'Corner column', corner, ['building', 'corner'], [1, 1, 1], []),
        ('floor', 'Floor slab', floor_slab, ['building', 'floor'], [1, 0, 1], []),
        ('roof', 'Roof with parapet', roof_flat, ['building', 'roof'], [1, 0, 1], []),
        ('trim', 'Cornice trim', trim, ['building', 'trim'], [1, 0, 1], [])]),
    'skyscrapers': ('Skyscrapers', [
        ('tower_glass', 'Glass tower, 60 m', lambda: tower('glass', 60), ['skyscraper'], [4, 12, 4], []),
        ('tower_office', 'Office tower, 90 m', lambda: tower('office2', 90), ['skyscraper'], [4, 18, 4], []),
        ('tower_night', 'Night tower, 120 m', lambda: tower('night', 120), ['skyscraper'], [4, 24, 4], [])]),
    'houses': ('Houses', [
        ('house_brick', 'Brick house, 2 storeys', lambda: house(2, 2, 2, 'brickwin'), ['house'], [2, 3, 2], []),
        ('house_long', 'Long house, 1 storey', lambda: house(3, 2, 1, 'brick'), ['house'], [3, 2, 2], []),
        ('house_shop', 'Shop house, flat roof', lambda: house(2, 3, 2, 'office', gable=False), ['house'], [2, 2, 3], [])]),
    'props': ('Street props', [
        ('lamp', 'Street lamp', lamp, ['prop'], [1, 2, 1], []),
        ('bench', 'Bench', bench, ['prop'], [1, 1, 1], []),
        ('sign', 'Sign', sign, ['prop'], [1, 1, 1], []),
        ('barrier', 'Concrete barrier', barrier, ['prop'], [1, 1, 1], [])]),
}
LICENCE = 'game-texture (play\\textures\\Custom remasters of Westopolis, private use); paint colours generated'

def build_kits(KITS, LICENCE):
  for kit, (title, pieces) in KITS.items():
      d = os.path.join(OUT, kit); texdir = os.path.join(d, 'textures'); os.makedirs(texdir, exist_ok=True)
      rows = []
      for pid, name, fn, tags, cells, sockets in pieces:
          p = fn(); path = os.path.join(d, pid + '.glb'); export(p, path, texdir)
          allp = [v for fs in p.faces.values() for f in fs for v in f]
          lo = [min(v[i] for v in allp) for i in range(3)]; hi = [max(v[i] for v in allp) for i in range(3)]
          size = [round((hi[0] - lo[0]) * 10, 2), round((hi[2] - lo[2]) * 10, 2), round((hi[1] - lo[1]) * 10, 2)]   # game x, y, z units
          rows.append(dict(id=pid, name=name, file=pid + '.glb', cells=cells, size=size, pivot='min-corner', tags=tags,
                           collision=True, surface=0x00020001, sockets=sockets, materials=sorted(p.faces), licence=LICENCE))
          print('PIECE', kit, pid, size, flush=True)
      json.dump(dict(kit=kit, title=title, grid=50, cellMetres=CELL, unitsPerTexture=TILE * 10, textureSize=TEXSIZE, pieces=rows),
                open(os.path.join(d, 'kit.json'), 'w'), indent=1)
  print('BATCH_OK', OUT)


if __name__ == '__main__' and os.path.basename(sys.argv[sys.argv.index('--python') + 1] if '--python' in sys.argv else '') == 'make_batch1.py':
    build_kits(KITS, LICENCE)
