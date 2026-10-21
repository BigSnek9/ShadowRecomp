"""#128 tileset batch 2 (2026-10-20) - more variety on batch 1's conventions (tools/tilesets/make_batch1.py:
grid 50 units, pivot = footprint min corner, planar UVs 1 repeat / 5 m, 1024 px textures, outward normals, collision):
    "C:\\Program Files\\Blender Foundation\\Blender 4.3\\blender.exe" -b --factory-startup --python tools/tilesets/make_batch2.py -- [work/tilesets]
Kits: roads2 (wide curve, crossroads with traffic lights, bridge deck + ramp, road end), shops (shop fronts, apartment
block, balcony), rooftop (AC unit, water tank, antenna, stair hut), street2 (mesh fence, railing, garden wall, tree,
planter, signs, traffic light, hazard board). New here: cut-out materials (alpha: fence mesh, leaves) and decals
(materials 'decal_*': one picture over a board of its own aspect - exempt from the density rule, still unstretched)."""
import math, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_batch1 as B
from make_batch1 import Piece, CELL, ROAD_W, SW, KH, T

PROPS = os.path.join(B.CUSTOM, r'Stages\01 Westopolis (stg0100)\Objects and props')
P = lambda f: os.path.join(PROPS, f)
B.TEX.update({
    'shutter': P('s0100_dn005 [tex1_64x64_e7944487b76d85f6_dae08290b48a6e46_8].png'),
    'foliage': P('s0100_dn011 [tex1_64x64_8c69c14cdc8d3476_7ab3caa87d838f9b_8].png'),
    'stripes': P('s0100_dn015 [tex1_64x64_3335ea94dfc72e5b_381ca4019e58bba0_8].png'),
    'hazard': P('s0100_dn022 [tex1_64x64_cd65db352fd915e3_f78e18c3a3327fd8_8].png'),
    'panel': P('s0100_dn024 [tex1_128x128_0be933ec8299d04f_b96739682afcb084_8].png'),
    'panel2': P('s0100_dn025 [tex1_64x64_fbc277c3fb1d874f_7d5119b97d557051_8].png'),
    'vent': P('s0100_dn027 [tex1_64x64_f4a62f2feddb8721_dfe94d43e7f10d86_8].png'),
    'soil': P('s0100_dn029 [tex1_64x64_b08f287e3e9b4ed3_236ee4f06567ec25_8].png'),
    'mesh': P('s0100_dp001 [tex1_128x128_88ee1563b1b9f007_4e298f621c3c79c4_8].png'),
    'decal_leaves': P('s0100_dp002 [tex1_64x64_6f6e4b65de86c57d_dd18fa705dba102d_8].png'),
    'decal_speed': P('s0100_dn030 [tex1_64x128_f85f2f3ca2c8b376_510a1c771e0bbafc_8].png'),
    'decal_parking': P('s0101wn003 [tex1_64x64_941389732a6a7c02_d76ab5363dbbc1af_8].png'),
    'decal_noentry_squeezed': P('s0101wn003 [tex1_64x64_941389732a6a7c02_d76ab5363dbbc1af_8].png'),   # same atlas, right half
    'shop': os.path.join(B.SRC, 's0100_nn055 [tex1_128x128_0131d67e3b3cff66_14].png'),
    'light_red': (0.95, 0.12, 0.08), 'light_amber': (1.0, 0.65, 0.05), 'light_green': (0.15, 0.9, 0.3), 'dark': (0.08, 0.08, 0.09)})
B.ALPHA.update({'mesh', 'decal_leaves'})


def decal(p, mat, a0, a1, b, c0, c1, out_b=-1, rect=(0, 0, 1, 1), turned=False):
    """a picture on a board face at depth b (looking toward out_b); rect = (u0, v0, u1, v1) of an atlas, turned = the
    atlas stores it a quarter turn clockwise (its top at +u)"""
    u0, v0, u1, v1 = rect
    uv = [(u0, v1), (u0, v0), (u1, v0), (u1, v1)] if turned else [(u0, v0), (u1, v0), (u1, v1), (u0, v1)]   # BL BR TR TL
    if out_b < 0: uv = [uv[1], uv[0], uv[3], uv[2]]   # piece b runs opposite to the viewer's right: mirror
    p.face(mat, [(a0, b, c0), (a1, b, c0), (a1, b, c1), (a0, b, c1)], (0, out_b, 0), uvs=uv)


# ------------------------------------------------------------------------------------------------ roads2
def road_curve_wide():
    """90 degree curve over 2 x 2 tiles (40 x 40 m): from the south edge (b = 0, a 0..20) to the east edge (a = 40,
    b 20..40), centre line radius 30 round the corner (40, 0)"""
    p = Piece(); L = 2 * ROAD_W; N = 24; K = (L, 0.0)
    ring = lambda r, k: (L - r * math.cos(math.pi / 2 * k / N), r * math.sin(math.pi / 2 * k / N))
    r_in, r_out = ROAD_W + SW, L - SW   # road from r 23 to r 37; sidewalks 20..23 and 37..40
    for k in range(N):
        a, b = ring(r_in, k), ring(r_in, k + 1); c, d = ring(r_out, k), ring(r_out, k + 1)
        e, f = ring(ROAD_W, k), ring(ROAD_W, k + 1); g, h = ring(L, k), ring(L, k + 1)
        p.ground('asphalt', [a, b, d, c])
        p.ground('pavement', [e, f, b, a], KH); p.ground('pavement', [c, d, h, g], KH)
        p.ground('pavement', [(0, L), g, h], KH)                      # outer corner up to the tile corner
        p.ground('pavement', [K, e, f], KH)                           # inner area (a quarter disc of radius 20)
        m = ((a[0] + b[0]) / 2, (a[1] + b[1]) / 2); p.face('kerb', [(*a, 0), (*b, 0), (*b, KH), (*a, KH)], (m[0] - K[0], m[1] - K[1], 0))
        m = ((c[0] + d[0]) / 2, (c[1] + d[1]) / 2); p.face('kerb', [(*c, 0), (*d, 0), (*d, KH), (*c, KH)], (K[0] - m[0], K[1] - m[1], 0))
        for off in (-0.15, 0.15):
            r0, r1 = 30 + off - 0.06, 30 + off + 0.06
            p.ground('paint_yellow', [ring(r0, k), ring(r0, k + 1), ring(r1, k + 1), ring(r1, k)], 0.01)
    return p


def traffic_light(p, a, b, facing):
    """pole + head with 3 lamps; facing = (da, db) the direction the lamps look"""
    p.box('metal', (a - 0.1, b - 0.1, 0), (a + 0.1, b + 0.1, 4.5))
    p.box('dark', (a - 0.25, b - 0.25, 3.3), (a + 0.25, b + 0.25, 4.5))
    for i, m in enumerate(('light_red', 'light_amber', 'light_green')):
        c = 4.2 - i * 0.38; da, db = facing
        if db:
            bb = b + db * 0.26; p.face(m, [(a - 0.12, bb, c - 0.14), (a + 0.12, bb, c - 0.14), (a + 0.12, bb, c + 0.14), (a - 0.12, bb, c + 0.14)], (0, db, 0))
        else:
            aa = a + da * 0.26; p.face(m, [(aa, b - 0.12, c - 0.14), (aa, b + 0.12, c - 0.14), (aa, b + 0.12, c + 0.14), (aa, b - 0.12, c + 0.14)], (da, 0, 0))


def road_lights_cross():
    p = B.road_cross()
    L = ROAD_W
    traffic_light(p, 1.5, 1.5, (0, -1)); traffic_light(p, L - 1.5, L - 1.5, (0, 1))
    traffic_light(p, L - 1.5, 1.5, (1, 0)); traffic_light(p, 1.5, L - 1.5, (-1, 0))
    for b0 in (2.0, L - 2.6):   # stop lines
        p.ground('paint_white', [(SW, b0), (L - SW, b0), (L - SW, b0 + 0.6), (SW, b0 + 0.6)], 0.01)
    return p


H_BRIDGE = 2 * CELL   # deck top at 10 m


def bridge_deck():
    """20 x 20 m road deck at 10 m (1 m thick) on two pillars, railings on both sides"""
    p = Piece(); L, H = ROAD_W, H_BRIDGE
    p.box('concrete', (0, 0, H - 1.0), (L, L, H), top='asphalt', bottom=True)
    p.ground('paint_yellow', [(L / 2 - 0.21, 0), (L / 2 - 0.09, 0), (L / 2 - 0.09, L), (L / 2 - 0.21, L)], H + 0.01)
    p.ground('paint_yellow', [(L / 2 + 0.09, 0), (L / 2 + 0.21, 0), (L / 2 + 0.21, L), (L / 2 + 0.09, L)], H + 0.01)
    for a0 in (0.0, L - 0.5):
        p.box('concrete', (a0, 0, H), (a0 + 0.5, L, H + 0.9), top='metal')                     # parapet walls
    for b0 in (4.0, L - 6.0):
        p.box('concrete', (L / 2 - 2.0, b0, 0), (L / 2 + 2.0, b0 + 2.0, H - 1.0), top='concrete')   # pillars
    return p


def bridge_ramp():
    """20 m wide, 40 m long ramp from 5 m up to 10 m (meets bridge_deck; a road_ramp below it rises 0 -> 5 m)"""
    p = Piece(); L, D, h0, h1 = ROAD_W, 8 * CELL, CELL, H_BRIDGE
    p.face('asphalt', [(0, 0, h0), (L, 0, h0), (L, D, h1), (0, D, h1)], (0, -0.1, 1))
    p.face('concrete', [(0, 0, h0 - 1), (L, 0, h0 - 1), (L, D, h1 - 1), (0, D, h1 - 1)], (0, 0.1, -1))   # underside
    for a in (0.0, L):
        s = -1 if a == 0 else 1
        p.face('concrete', [(a, 0, h0 - 1), (a, D, h1 - 1), (a, D, h1), (a, 0, h0)], (s, 0, 0))
    p.face('concrete', [(0, 0, h0 - 1), (L, 0, h0 - 1), (L, 0, h0), (0, 0, h0)], (0, -1, 0))
    p.face('concrete', [(0, D, h1 - 1), (L, D, h1 - 1), (L, D, h1), (0, D, h1)], (0, 1, 0))
    for a0 in (0.0, L - 0.5):   # parapets along the slope
        for k in range(8):
            b0, b1 = k * CELL, (k + 1) * CELL; z0, z1 = h0 + (h1 - h0) * k / 8, h0 + (h1 - h0) * (k + 1) / 8
            p.face('concrete', [(a0 + 0.5 if a0 == 0 else a0, b0, z0), (a0 + 0.5 if a0 == 0 else a0, b1, z1), (a0 + 0.5 if a0 == 0 else a0, b1, z1 + 0.9), (a0 + 0.5 if a0 == 0 else a0, b0, z0 + 0.9)], (1 if a0 == 0 else -1, 0, 0))
            p.face('metal', [(a0, b0, z0 + 0.9), (a0 + 0.5, b0, z0 + 0.9), (a0 + 0.5, b1, z1 + 0.9), (a0, b1, z1 + 0.9)], (0, 0, 1))
    for b0 in (6.0, D - 8.0):
        z = h0 + (h1 - h0) * b0 / D - 1
        p.box('concrete', (L / 2 - 2.0, b0, 0), (L / 2 + 2.0, b0 + 2.0, z), top='concrete')
    return p


def road_end():
    """a straight tile closed at its far end: striped boards on posts across the road"""
    p = B.road_straight()
    L = ROAD_W
    for k in range(3):
        a0 = SW + 1.0 + k * 4.4
        p.box('metal', (a0, L - 1.2, 0), (a0 + 0.12, L - 1.08, 1.4), bottom=False); p.box('metal', (a0 + 3.4, L - 1.2, 0), (a0 + 3.52, L - 1.08, 1.4), bottom=False)
        p.box('stripes', (a0, L - 1.25, 0.9), (a0 + 3.52, L - 1.03, 1.4))
    return p


# ------------------------------------------------------------------------------------------------ shops / apartments
def shopfront(lower='glass'):
    """one 5 x 5 m street-facing wall panel: shop window (or shutter) 0..3.5 m, sign band, awning"""
    p = Piece(); C = CELL; W = 1.6   # the wall stands 1.6 m into the cell: the awning stays inside the footprint
    p.box('plaster', (0, W, 3.5), (C, W + T, C), sides={'-b': 'brick'})
    p.box('plaster', (0, W, 0), (0.4, W + T, 3.5), sides={'-b': 'brick'}); p.box('plaster', (C - 0.4, W, 0), (C, W + T, 3.5), sides={'-b': 'brick'})
    p.box('plaster', (0.4, W + 0.2, 0), (C - 0.4, W + T, 3.5), sides={'-b': 'shop' if lower == 'glass' else 'shutter'})
    p.box('stripes', (0.2, 0.0, 3.6), (C - 0.2, W, 3.75), top='stripes')   # awning slab (striped canvas)
    return p


def apartment():
    """4 x 3 cells, 5 storeys (25 m), flat roof with parapet, a balcony per storey (2nd up) on the street side"""
    p = Piece(); A, F, Bd, H = 4 * CELL, 1.5, 3 * CELL, 5 * CELL   # the body starts 1.5 m back: balconies inside the 4 x 4 cells
    p.box('brickwin', (0, F, 0), (A, F + Bd, H), top='roof')
    for x0, x1, y0, y1 in ((0, A, F, F + T), (0, A, F + Bd - T, F + Bd), (0, T, F + T, F + Bd - T), (A - T, A, F + T, F + Bd - T)):
        p.box('concrete', (x0, y0, H), (x1, y1, H + 1.0))
    p.face('door', [(A / 2 - 1, F - 0.01, 0), (A / 2 + 1, F - 0.01, 0), (A / 2 + 1, F - 0.01, 3.5), (A / 2 - 1, F - 0.01, 3.5)], (0, -1, 0))
    for st in range(1, 5):
        for x0 in (2.0, A - 6.0):
            balcony_at(p, x0, st * CELL, F)
    return p


def balcony_at(p, a0, c, wall_b=1.5):
    b0 = wall_b - 1.5
    p.box('concrete', (a0, b0, c - 0.2), (a0 + 4.0, wall_b, c), top='concrete')
    p.box('metal', (a0, b0, c), (a0 + 4.0, b0 + 0.1, c + 1.0)); p.box('metal', (a0, b0 + 0.1, c), (a0 + 0.1, wall_b, c + 1.0)); p.box('metal', (a0 + 3.9, b0 + 0.1, c), (a0 + 4.0, wall_b, c + 1.0))


def balcony():
    p = Piece(); balcony_at(p, 0.5, 0.2); return p   # a balcony in the front 1.5 m of the cell (set the wall behind it)


# ------------------------------------------------------------------------------------------------ rooftop
def ac_unit():
    p = Piece(); c = CELL / 2
    p.box('panel2', (c - 1.0, c - 0.6, 0), (c + 1.0, c + 0.6, 1.2), sides={'-b': 'vent', '+b': 'vent'}, top='vent'); return p


def water_tank():
    p = Piece(); c = CELL / 2; r = 1.4; N = 12; z0, z1 = 1.5, 4.0
    for k in range(N):
        t0, t1 = 2 * math.pi * k / N, 2 * math.pi * (k + 1) / N
        q0, q1 = (c + r * math.cos(t0), c + r * math.sin(t0)), (c + r * math.cos(t1), c + r * math.sin(t1))
        p.face('panel', [(*q0, z0), (*q1, z0), (*q1, z1), (*q0, z1)], ((q0[0] + q1[0]) / 2 - c, (q0[1] + q1[1]) / 2 - c, 0))
        p.face('panel', [(c, c, z1), q0 + (z1,), q1 + (z1,)], (0, 0, 1)); p.face('panel', [(c, c, z0), q0 + (z0,), q1 + (z0,)], (0, 0, -1))
    for da, db in ((-1, -1), (1, -1), (-1, 1), (1, 1)):
        a, b = c + da * 0.9, c + db * 0.9; p.box('metal', (a - 0.1, b - 0.1, 0), (a + 0.1, b + 0.1, z0))
    return p


def antenna():
    p = Piece(); c = CELL / 2
    p.box('metal', (c - 0.06, c - 0.06, 0), (c + 0.06, c + 0.06, 6.0))
    for z in (2.0, 3.0, 4.0, 5.0): p.box('metal', (c - 0.8, c - 0.03, z), (c + 0.8, c + 0.03, z + 0.06))
    p.box('concrete', (c - 0.4, c - 0.4, 0), (c + 0.4, c + 0.4, 0.3)); return p


def stair_hut():
    p = Piece(); C = CELL
    p.box('concrete', (0.5, 0.5, 0), (C - 0.5, C - 0.5, 3.0), top='roof')
    p.face('door', [(C / 2 - 0.8, 0.49, 0), (C / 2 + 0.8, 0.49, 0), (C / 2 + 0.8, 0.49, 2.4), (C / 2 - 0.8, 0.49, 2.4)], (0, -1, 0)); return p


# ------------------------------------------------------------------------------------------------ street2
def fence_mesh():
    """5 m chain-link style fence, 2 m high: posts + a cut-out mesh panel (both sides drawn)"""
    p = Piece(); C = CELL; b = CELL / 2
    for a in (0.0, C - 0.1): p.box('metal', (a, b - 0.05, 0), (a + 0.1, b + 0.05, 2.1))
    p.box('metal', (0, b - 0.03, 2.0), (C, b + 0.03, 2.06))
    for s in (-1, 1):   # the mesh as two faces back to back (the game draws one side)
        p.face('mesh', [(0.1, b + s * 0.03, 0.05), (C - 0.1, b + s * 0.03, 0.05), (C - 0.1, b + s * 0.03, 2.0), (0.1, b + s * 0.03, 2.0)], (0, s, 0))
    return p


def railing():
    p = Piece(); C = CELL; b = 0.25
    for a in (0.0, C / 2, C - 0.08): p.box('metal', (a, b - 0.04, 0), (a + 0.08, b + 0.04, 1.0))
    p.box('metal', (0, b - 0.05, 1.0), (C, b + 0.05, 1.08)); p.box('metal', (0, b - 0.02, 0.5), (C, b + 0.02, 0.54)); return p


def wall_low():
    p = Piece(); p.box('brick', (0, 0, 0), (CELL, 0.4, 1.0), top='concrete'); return p


def tree(p=None, a=CELL / 2, b=CELL / 2, c0=0.0, h=6.0):
    p = p or Piece()
    p.box('dark', (a - 0.15, b - 0.15, c0), (a + 0.15, b + 0.15, c0 + h * 0.55))       # trunk
    r = h * 0.3; cz0, cz1 = c0 + h * 0.4, c0 + h   # square cards (0.6 h x 0.6 h): the leaf picture is square
    for ang in (0, 60, 120):   # three crossed cut-out leaf cards, each drawn both ways
        t = math.radians(ang); dx, dy = math.cos(t) * r, math.sin(t) * r
        for s in (1, -1):
            n = (-math.sin(t) * s, math.cos(t) * s, 0)
            p.face('decal_leaves', [(a - dx, b - dy, cz0), (a + dx, b + dy, cz0), (a + dx, b + dy, cz1), (a - dx, b - dy, cz1)], n,
                   uvs=[(0, 0), (1, 0), (1, 1), (0, 1)] if s > 0 else [(1, 0), (0, 0), (0, 1), (1, 1)])
    return p


def planter():
    p = Piece(); c = CELL / 2
    p.box('concrete', (c - 1.0, c - 1.0, 0), (c + 1.0, c + 1.0, 0.8), top='soil')
    tree(p, c, c, 0.8, 3.0); return p


def sign_post(mat, w, h, rect=(0, 0, 1, 1), turned=False):
    """a post with a decal board w x h (m) facing -b (the picture's own aspect)"""
    p = Piece(); c = CELL / 2
    p.box('metal', (c - 0.05, c - 0.05, 0), (c + 0.05, c + 0.05, 2.2 + h))
    p.box('metal', (c - w / 2, c - 0.08, 2.2), (c + w / 2, c - 0.05, 2.2 + h))
    decal(p, mat, c - w / 2, c + w / 2, c - 0.09, 2.2, 2.2 + h, rect=rect, turned=turned); return p


def traffic_light_prop():
    p = Piece(); traffic_light(p, CELL / 2, CELL / 2, (0, -1)); return p


def hazard_board():
    p = Piece(); c = CELL / 2
    p.box('metal', (c - 1.4, c - 0.05, 0), (c - 1.3, c + 0.05, 1.2)); p.box('metal', (c + 1.3, c - 0.05, 0), (c + 1.4, c + 0.05, 1.2))
    p.box('hazard', (c - 1.5, c - 0.08, 0.7), (c + 1.5, c + 0.08, 1.2)); return p


KITS = {
    'roads2': ('Road kit 2', [
        ('road_curve_wide', 'Road, wide curve (2 x 2 tiles)', road_curve_wide, ['road'], [8, 0, 8], [{'side': 's', 'kind': 'road'}, {'side': 'e', 'kind': 'road'}]),
        ('road_lights', 'Crossroads with traffic lights', road_lights_cross, ['road', 'lights'], [4, 1, 4], [{'side': s, 'kind': 'road'} for s in 'snew']),
        ('bridge_deck', 'Bridge deck (10 m up)', bridge_deck, ['road', 'bridge'], [4, 2, 4], [{'side': 's', 'kind': 'road', 'height': 2}, {'side': 'n', 'kind': 'road', 'height': 2}]),
        ('bridge_ramp', 'Bridge ramp (5 -> 10 m)', bridge_ramp, ['road', 'bridge', 'ramp'], [4, 2, 8], [{'side': 's', 'kind': 'road', 'height': 1}, {'side': 'n', 'kind': 'road', 'height': 2}]),
        ('road_end', 'Road end with barriers', road_end, ['road'], [4, 0, 4], [{'side': 's', 'kind': 'road'}])]),
    'shops': ('Shops and apartments', [
        ('shopfront_glass', 'Shop front, window', lambda: shopfront('glass'), ['building', 'shop'], [1, 1, 1], [{'side': 's', 'kind': 'wall'}]),
        ('shopfront_shutter', 'Shop front, shutter', lambda: shopfront('shutter'), ['building', 'shop'], [1, 1, 1], [{'side': 's', 'kind': 'wall'}]),
        ('apartment', 'Apartment block, 5 storeys', apartment, ['building', 'apartment'], [4, 5, 4], []),
        ('balcony', 'Balcony', balcony, ['building'], [1, 0, 1], [])]),
    'rooftop': ('Rooftop props', [
        ('ac_unit', 'AC unit', ac_unit, ['prop', 'rooftop'], [1, 0, 1], []),
        ('water_tank', 'Water tank', water_tank, ['prop', 'rooftop'], [1, 1, 1], []),
        ('antenna', 'Antenna mast', antenna, ['prop', 'rooftop'], [1, 1, 1], []),
        ('stair_hut', 'Roof stair hut', stair_hut, ['prop', 'rooftop'], [1, 1, 1], [])]),
    'street2': ('Street props 2', [
        ('fence_mesh', 'Mesh fence, 5 m', fence_mesh, ['prop', 'fence'], [1, 0, 1], []),
        ('railing', 'Railing, 5 m', railing, ['prop', 'fence'], [1, 0, 1], []),
        ('wall_low', 'Low garden wall, 5 m', wall_low, ['prop', 'fence'], [1, 0, 1], []),
        ('tree', 'Street tree', tree, ['prop', 'green'], [1, 1, 1], []),
        ('planter', 'Planter with shrub', planter, ['prop', 'green'], [1, 1, 1], []),
        ('sign_speed', 'Sign: speed limit', lambda: sign_post('decal_speed', 0.75, 0.75, (0, 0.5, 1, 1), True), ['prop', 'sign'], [1, 1, 1], []),
        ('sign_parking', 'Sign: parking', lambda: sign_post('decal_parking', 0.6, 1.2, (0, 0, 0.5, 1)), ['prop', 'sign'], [1, 1, 1], []),
        ('sign_noentry', 'Sign: no entry', lambda: sign_post('decal_noentry_squeezed', 0.75, 0.75, (0.5, 0, 1, 1), True), ['prop', 'sign'], [1, 1, 1], []),
        ('traffic_light', 'Traffic light', traffic_light_prop, ['prop', 'lights'], [1, 1, 1], []),
        ('hazard_board', 'Hazard board', hazard_board, ['prop'], [1, 0, 1], [])]),
}

if __name__ == '__main__':
    B.build_kits(KITS, B.LICENCE)
