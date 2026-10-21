"""#103 Sky picker (2026-10-19). The game has no sky file: a stage's sky is ordinary world geometry in its
backdrop chunks (every visual world wider than 14000 units, server.atmosphere). A custom stage gets a sky as project
geometry (group 'Sky', no collision, own textures = custom materials), so the editor previews it and the normal geometry
build writes it (custom textures go into the stage's texCM pack):
  * stage N  -> the meshes of N's main backdrop chunk, moved so their centre sits over this stage's centre and their
               height keeps the same relation to the player start;
  * image    -> an inverted dome (radius 14000) with the picture wrapped around it (equirectangular).
list_skies() = one row per stage with a backdrop (name, thumbnail = its biggest backdrop texture), cached in
work/stage-editor-cache/skies.json."""
import base64, json, math
from pathlib import Path
import numpy as np


def S():
    import server
    return server


def _main_chunk(sky):
    worlds = sky['worlds'] or []
    if not worlds: return None, []
    by = {}
    for w in worlds: by.setdefault(w['chunk'], []).append(w)
    chunk = max(by, key=lambda c: (sum(w['span'] for w in by[c]), -c))
    return chunk, by[chunk]


THUMBS = Path(__import__('os').environ.get('STAGE_FORGE_DATA') or Path(__file__).resolve().parents[2]) / 'work/stage-editor-cache/sky-thumbs'


def list_skies(refresh=False):
    srv = S(); cache = srv.DATA / 'work/stage-editor-cache/skies.json'
    if cache.exists() and not refresh: return json.loads(cache.read_text())
    out = []
    for st, name in sorted(srv.STAGE_NAMES.items()):
        if not (100 <= st <= 710) or not (srv.stage_dir(st) / f'stg{st:04d}_cmn.dat').exists(): continue
        try: srv.disc_textures(st); a = srv.atmosphere(st)
        except Exception: continue
        chunk, worlds = _main_chunk(a['sky'])
        if not worlds: continue
        big = max(worlds, key=lambda w: w['span']); thumb = None; tex = None
        for m in sorted(big['materials'], key=lambda m: 0 if m.get('asset') else 1):
            e = (srv.CACHE.get('texture_names') or {}).get(str(m.get('texture') or '').lower()); path = srv.ASSETS.get(e['id']) if e else None
            if path and Path(path).exists():
                tex = m['texture']; thumb = THUMBS / f'{st}.png'; THUMBS.mkdir(parents=True, exist_ok=True); thumb.write_bytes(Path(path).read_bytes()); break
        out.append(dict(stage=st, name=name, chunk=chunk, worlds=len(worlds), thumb=f'/api/sky-thumb?stage={st}' if thumb else None, texture=tex))
    cache.parent.mkdir(parents=True, exist_ok=True); cache.write_text(json.dumps(out, indent=1)); return out


def _texture_material(pid, png_path, name):
    srv = S(); raw = Path(png_path).read_bytes()
    return srv.upload_asset(dict(project=pid, name=name, kind='texture', data=base64.b64encode(raw).decode()))


def _start(p):
    s = next((o for o in p.get('objects', []) if o.get('kind') in ('playerstart', 'spawn')), None)
    return np.asarray(s['position'], float) if s else np.zeros(3)


SLAB_EDGE, SLAB_DY = 4000.0, 2000.0   # #129: a backdrop face this flat, this wide and this near the start height is a slab


def slab_mask(V, T, start_y):
    """#129 (user: 'Westopolis sky adds a ground slab over blank stages'): True for the triangles of a backdrop that form a
    big flat cap at play height - Westopolis' skyline barrel is closed by a cone-shaped cap 650 units below to 200 above
    its start, hidden by the level in Westopolis, a floor without collision over an empty stage. Rule: normal within
    ~25 deg of vertical, centre within 2000 units of the source start height, an edge longer than 4000 units (horizontal).
    Dropped in 9 skies: Westopolis 8, Glyphic Canyon 16, Lethal Highway 8, Circus Park 16, Mad Matrix 88, The ARK 14,
    Lost Impact 6, Black Comet 16, Final Haunt 32 triangles; the rest of each sky is kept."""
    P = V[np.asarray(T)]
    n = np.cross(P[:, 1] - P[:, 0], P[:, 2] - P[:, 0]); n /= np.linalg.norm(n, axis=1, keepdims=True) + 1e-9
    edge = np.max([np.hypot(*(P[:, i, [0, 2]] - P[:, (i + 1) % 3, [0, 2]]).T) for i in range(3)], 0)
    return (np.abs(n[:, 1]) > 0.9) & (np.abs(P[:, :, 1].mean(1) - start_y) < SLAB_DY) & (edge > SLAB_EDGE)


def stage_sky(p, stage, keep_slabs=False, lift=False):
    """[geometry], [materials] for stage's main backdrop chunk, placed over project p (big flat caps at play height
    dropped unless keep_slabs, #129). lift: some skies sit mostly BELOW their stage's start (Westopolis' skyline band
    runs 650-10000 under it - the city's streets are high up), so over a floor that reaches the horizon nothing shows;
    lift raises the whole sky so its lowest kept point is 100 under this project's start (the band then stands on the
    horizon all round). stage_sky.lifted = the units it was raised (0 when it already stood above)."""
    srv = S(); a = srv.atmosphere(stage); chunk, worlds = _main_chunk(a['sky'])
    if not worlds: raise ValueError(f'{srv.STAGE_NAMES.get(stage, stage)} has no sky (backdrop) geometry')
    srv.disc_textures(stage); src = srv.import_stage(stage); ref = {m['id']: m for m in src['reference']}; mats = src['materials']
    lo = np.min([w['bounds'][0] for w in worlds], 0); hi = np.max([w['bounds'][1] for w in worlds], 0); centre = (lo + hi) / 2
    sp = srv.stage_params(stage) or {}; pl = next((l for l in sp.get('lines', []) if l.strip().startswith('PLAYER')), None)
    src_start_y = float(pl.split(':')[1].split()[2]) if pl else 0.0
    here = _start(p); offset = np.array([here[0] - centre[0], here[1] - src_start_y, here[2] - centre[2]])
    if lift:   # the lowest point of the sky that is kept (slabs excluded), relative to the source start
        lows = []
        for w in worlds:
            for mid in w['meshes']:
                m = ref.get(mid)
                if not m or not m['triangles']: continue
                V0 = np.asarray(m['vertices'], float); keep = ~slab_mask(V0, m['triangles'], src_start_y) if not keep_slabs else np.ones(len(m['triangles']), bool)
                if keep.any(): lows.append(V0[np.asarray(m['triangles'])[keep]][:, :, 1].min())
        tops = []
        for w in worlds:
            for mid in w['meshes']:
                m = ref.get(mid)
                if m and m['triangles']: tops.append(np.asarray(m['vertices'], float)[:, 1].max())
        # only a sky lying wholly under the start (top < start + 500) is raised; one that already surrounds it is kept
        rise = max(0.0, (src_start_y - 100.0) - min(lows)) if lows and tops and max(tops) < src_start_y + 500 else 0.0
        offset = offset + np.array([0.0, rise, 0.0])
    else: rise = 0.0
    stage_sky.lifted = round(float(rise), 1)
    tn = srv.CACHE.get('texture_names') or {}; made = {}; geoms = []; dropped = 0
    for w in worlds:
        for mid in w['meshes']:
            m = ref.get(mid)
            if not m or not m['triangles']: continue
            V0 = np.asarray(m['vertices'], float); V = V0 + offset; UV = m.get('uvs') or [[0, 0]] * len(V)
            slab = np.zeros(len(m['triangles']), bool) if keep_slabs else slab_mask(V0, m['triangles'], src_start_y)
            if rise and not keep_slabs: slab |= slab_mask(V0, m['triangles'], src_start_y - rise)   # a cap the lift brings to play height
            dropped += int(slab.sum())
            per = {}
            for t, mi, sl in zip(m['triangles'], m['materials'], slab):
                if not sl: per.setdefault(mi, []).append(t)
            for mi, tris in per.items():
                tex = mats[mi].get('texture') if mi < len(mats) else None
                if not tex: continue
                if tex not in made:
                    e = tn.get(tex.lower()); path = srv.ASSETS.get(e['id']) if e else None
                    if not path: continue
                    made[tex] = _texture_material(p['id'], path, f'sky {tex}.png')
                used = sorted({i for t in tris for i in t}); remap = {o: n for n, o in enumerate(used)}
                geoms.append(dict(name=f'Sky ({srv.STAGE_NAMES.get(stage, stage)}) {tex}', vertices=V[used].round(2).tolist(), uvs=[list(UV[i]) for i in used],
                                  triangles=[[remap[i] for i in t] for t in tris], material=made[tex]['id']))
    if not geoms: raise ValueError('no sky textures found (texture library not loaded?)')
    stage_sky.dropped = dropped
    return geoms, list(made.values())


def dome(radius=14000.0, rings=12, segs=32, centre=(0, 0, 0)):
    """inverted hemisphere-plus (down to -25 degrees) with equirectangular UVs, triangles facing inwards"""
    V, U, T = [], [], []
    for r in range(rings + 1):
        el = math.radians(-25 + (90 + 25) * r / rings)
        for s in range(segs + 1):
            az = 2 * math.pi * s / segs
            V.append([centre[0] + radius * math.cos(el) * math.cos(az), centre[1] + radius * math.sin(el), centre[2] + radius * math.cos(el) * math.sin(az)])
            U.append([s / segs, 1 - r / rings])
    for r in range(rings):
        for s in range(segs):
            a = r * (segs + 1) + s; b = a + segs + 1
            T += [[a, a + 1, b], [a + 1, b + 1, b]]
    return V, U, T


def image_sky(p, material):
    c = _start(p); V, U, T = dome(centre=(c[0], c[1] - 200, c[2]))
    return [dict(name='Sky (your picture)', vertices=V, uvs=U, triangles=T, material=material['id'])], []


def make(p, stage=None, material=None, lift=False):
    """the project geometry rows (complete) + new materials for a sky choice (lift: see stage_sky)"""
    geoms, mats = stage_sky(p, int(stage), lift=lift) if stage is not None else image_sky(p, material)
    import uuid
    rows = [dict(id=uuid.uuid4().hex, kind='geometry', shape='mesh', position=[0, 0, 0], rotation=[0, 0, 0], scale=[1, 1, 1], colour=[255, 255, 255, 255],
                 chunk=1, collision=False, sky=True, uvRepeat=[1, 1], **g) for g in geoms]
    return dict(geometry=rows, materials=mats, source=dict(stage=stage, slabTrianglesDropped=getattr(stage_sky, 'dropped', 0), lift=bool(lift), lifted=getattr(stage_sky, 'lifted', 0)) if stage is not None else dict(image=material.get('id')))
