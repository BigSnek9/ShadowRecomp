"""#119 Import geometry (2026-10-19): a whole level from another program (Blender export, a ripped map) as
the editor's own textured meshes with collision. docs/custom-stages/IMPORT-GEOMETRY.md has the limits.

  import_geometry(pid, files, options, upload) -> dict(materials, geometry, chunkBoxes, warnings, stats)

files   [(name, bytes)]: one .obj (+ its .mtl and texture images), or one .gltf (+ .bin / images) or one .glb.
options units 'game' | 'meters' (x10, 1 m ~ 10 game units: Shadow's model is ~10 high) | 'centimeters' (x0.1)
        scale (extra factor), up 'y' (glTF, Blender's OBJ default) | 'z' (raw Blender / Z-up tools), flipWinding,
        position [x, y, z] (where the model's origin goes), yaw (degrees about +y round the origin, #128 pieces),
        ground (bool: lift so its lowest point sits on position.y),
        chunks [numbers] (chunks the pieces may use, in order), surfaces {material: surface code}, defaultSurface,
        collision (bool), maxTexture (largest texture side, default 512).
upload  callable(name, bytes, kind) -> the server's asset dict (server.upload_asset), so textures land in the project.
Every piece is one material and at most PIECE_VERTS vertices; pieces are grouped by place into chunks so no chunk's
collision passes COL_TRIS triangles (the game's 16-bit collision world); every used chunk gets one box over the whole
model so the game draws it wherever the player is (#53: a chunk is drawn only while the player is inside one of its boxes)."""
import base64, io, json, math, re, struct, uuid
from pathlib import PurePosixPath
import numpy as np
from PIL import Image

PIECE_VERTS = 12000        # vertices per piece (one native sector; indices are 16-bit) - kept well below 65535
COL_TRIS = 60000           # collision triangles per chunk (make_collision: 65535 vertices / triangles)
COL_VERTS = 60000
UNITS = {'game': 1.0, 'meters': 10.0, 'centimeters': 0.1}
DEFAULT_SURFACE = 0x00020001


# ---------------------------------------------------------------------------------------------------- parsing
def _lookup(files, ref):
    """a file referenced by a model (relative path, any case, maybe with folders the user did not keep)"""
    if not ref:
        return None
    ref = ref.replace('\\', '/').split('?')[0]
    from urllib.parse import unquote
    ref = unquote(ref)
    want = PurePosixPath(ref).name.lower()
    for n, b in files:
        if n.replace('\\', '/').lower().endswith(ref.lower()) or PurePosixPath(n.replace('\\', '/')).name.lower() == want:
            return b
    return None


def parse_obj(text, files, warnings):
    """-> materials {name: {colour, image bytes|None, imageName}}, groups {material: (positions, uvs, triangles)}"""
    mats = {}
    for line in text.splitlines():
        if line.strip().lower().startswith('mtllib'):
            mtl = _lookup(files, line.strip()[6:].strip())
            if mtl is None:
                warnings.append(f'Material file {line.strip()[6:].strip()} was not included: plain grey materials.')
                continue
            cur = None
            for l in mtl.decode('utf-8', 'replace').splitlines():
                p = l.strip().split(None, 1)
                if not p:
                    continue
                k = p[0].lower()
                if k == 'newmtl' and len(p) > 1:
                    cur = p[1].strip(); mats[cur] = dict(colour=[0.8, 0.8, 0.8], image=None, imageName=None)
                elif cur and k == 'kd' and len(p) > 1:
                    try: mats[cur]['colour'] = [float(x) for x in p[1].split()[:3]]
                    except ValueError: pass
                elif cur and k == 'map_kd' and len(p) > 1:
                    ref = p[1].split()[-1]   # options like -s 1 1 1 come first
                    img = _lookup(files, ref)
                    if img is None: warnings.append(f'Texture {ref} (material {cur}) was not included: its colour is used.')
                    mats[cur].update(image=img, imageName=PurePosixPath(ref.replace('\\', '/')).name)
    pos, tex = [], []
    groups = {}
    cur = 'default'
    for line in text.splitlines():
        if not line or line[0] == '#':
            continue
        p = line.split()
        if not p:
            continue
        k = p[0]
        if k == 'v':
            pos.append([float(p[1]), float(p[2]), float(p[3])])
        elif k == 'vt':
            tex.append([float(p[1]), float(p[2]) if len(p) > 2 else 0.0])
        elif k == 'usemtl':
            cur = line.split(None, 1)[1].strip() if len(p) > 1 else 'default'
        elif k == 'f':
            g = groups.setdefault(cur, ([], [], [], {}))
            V, U, T, seen = g
            idx = []
            for w in p[1:]:
                s = w.split('/')
                pi = int(s[0]); ti = int(s[1]) if len(s) > 1 and s[1] else 0
                pi = pi - 1 if pi > 0 else len(pos) + pi
                ti = (ti - 1 if ti > 0 else len(tex) + ti) if ti else -1
                key = (pi, ti)
                if key not in seen:
                    seen[key] = len(V); V.append(pos[pi]); U.append(tex[ti] if ti >= 0 else [0.0, 0.0])
                idx.append(seen[key])
            for i in range(1, len(idx) - 1):   # fan (Blender exports triangles or convex n-gons)
                T.append([idx[0], idx[i], idx[i + 1]])
    out = {}
    for name, (V, U, T, _) in groups.items():
        if T:
            uv = np.asarray(U, float); uv[:, 1] = 1 - uv[:, 1]   # OBJ v up -> image rows down
            out[name] = (np.asarray(V, float), uv, np.asarray(T, int))
        mats.setdefault(name, dict(colour=[0.8, 0.8, 0.8], image=None, imageName=None))
    return mats, out


_CT = {5120: 'b', 5121: 'B', 5122: 'h', 5123: 'H', 5125: 'I', 5126: 'f'}
_NC = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}


def parse_gltf(doc, buffers, files, warnings):
    """glTF 2.0 (JSON + buffers): meshes with every node transform applied; -> materials, groups (as parse_obj)"""
    def acc(i):
        a = doc['accessors'][i]; bv = doc['bufferViews'][a['bufferView']]
        buf = buffers[bv['buffer']]; n = _NC[a['type']]; t = _CT[a['componentType']]; size = struct.calcsize(t)
        start = bv.get('byteOffset', 0) + a.get('byteOffset', 0); stride = bv.get('byteStride') or size * n
        raw = np.frombuffer(buf, dtype=np.uint8, count=stride * (a['count'] - 1) + size * n, offset=start)
        rows = np.lib.stride_tricks.as_strided(raw, shape=(a['count'], size * n), strides=(stride, 1)).copy()
        arr = rows.view('<' + t).reshape(a['count'], n).astype(float)
        if a.get('normalized') and t in 'BHbh':
            arr /= {'B': 255, 'H': 65535, 'b': 127, 'h': 32767}[t]
        return arr

    def image_bytes(ti):
        tex = doc.get('textures', [])[ti]; img = doc.get('images', [])[tex.get('source', 0)]
        if 'bufferView' in img:
            bv = doc['bufferViews'][img['bufferView']]
            return buffers[bv['buffer']][bv.get('byteOffset', 0): bv.get('byteOffset', 0) + bv['byteLength']], img.get('name') or f'image{tex.get("source", 0)}'
        uri = img.get('uri', '')
        if uri.startswith('data:'):
            return base64.b64decode(uri.split(',', 1)[1]), img.get('name') or 'image'
        b = _lookup(files, uri)
        if b is None: warnings.append(f'Texture {uri} was not included: its colour is used.')
        return b, PurePosixPath(uri).name

    mats = {}
    for i, m in enumerate(doc.get('materials', [])):
        pbr = m.get('pbrMetallicRoughness', {}); name = m.get('name') or f'material{i}'
        img, iname = (None, None)
        if 'baseColorTexture' in pbr:
            img, iname = image_bytes(pbr['baseColorTexture']['index'])
        mats[name] = dict(colour=list(pbr.get('baseColorFactor', [0.8, 0.8, 0.8, 1]))[:3], image=img, imageName=iname, index=i)
    names = {m.get('index'): n for n, m in mats.items()}

    def local(node):
        if 'matrix' in node:
            return np.asarray(node['matrix'], float).reshape(4, 4).T
        t = np.eye(4); t[:3, 3] = node.get('translation', [0, 0, 0])
        x, y, z, w = node.get('rotation', [0, 0, 0, 1])
        r = np.eye(4); r[:3, :3] = [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                                    [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                                    [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]
        s = np.eye(4); s[0, 0], s[1, 1], s[2, 2] = node.get('scale', [1, 1, 1])
        return t @ r @ s

    groups = {}
    def visit(ni, parent):
        node = doc['nodes'][ni]; M = parent @ local(node)
        if 'mesh' in node:
            for prim in doc['meshes'][node['mesh']]['primitives']:
                if prim.get('mode', 4) != 4:
                    warnings.append('A non-triangle primitive (lines / points / strips) was skipped.'); continue
                P = acc(prim['attributes']['POSITION'])
                UV = acc(prim['attributes']['TEXCOORD_0']) if 'TEXCOORD_0' in prim['attributes'] else np.zeros((len(P), 2))
                I = acc(prim['indices']).astype(int).ravel() if 'indices' in prim else np.arange(len(P))
                P = (np.c_[P, np.ones(len(P))] @ M.T)[:, :3]
                T = I.reshape(-1, 3)
                if np.linalg.det(M[:3, :3]) < 0: T = T[:, [0, 2, 1]]   # mirrored node
                name = names.get(prim.get('material'), 'default'); mats.setdefault(name, dict(colour=[0.8, 0.8, 0.8], image=None, imageName=None))
                g = groups.setdefault(name, [])
                g.append((P, UV, T))
        for c in node.get('children', []):
            visit(c, M)
    scene = doc.get('scenes', [{}])[doc.get('scene', 0)] if doc.get('scenes') else {'nodes': list(range(len(doc.get('nodes', []))))}
    for ni in scene.get('nodes', []):
        visit(ni, np.eye(4))
    out = {}
    for name, parts in groups.items():
        V, U, T, off = [], [], [], 0
        for P, UV, Tr in parts:
            V.append(P); U.append(UV); T.append(Tr + off); off += len(P)
        out[name] = (np.concatenate(V), np.concatenate(U), np.concatenate(T))
    return mats, out


def read_model(files, warnings):
    names = [n.lower() for n, _ in files]
    glb = next((b for n, b in files if n.lower().endswith('.glb')), None)
    if glb is not None:
        if glb[:4] != b'glTF': raise ValueError('Not a GLB file (no glTF header)')
        length = struct.unpack_from('<I', glb, 8)[0]; off = 12; doc = None; bins = []
        while off < length:
            clen, ctype = struct.unpack_from('<II', glb, off); body = glb[off + 8: off + 8 + clen]
            if ctype == 0x4E4F534A: doc = json.loads(body)
            elif ctype == 0x004E4942: bins.append(body)
            off += 8 + clen
        bufs = [bins[i] if i < len(bins) and 'uri' not in b else _lookup(files, b.get('uri')) for i, b in enumerate(doc.get('buffers', []))]
        return parse_gltf(doc, bufs, files, warnings)
    gl = next((b for n, b in files if n.lower().endswith('.gltf')), None)
    if gl is not None:
        doc = json.loads(gl); bufs = []
        for b in doc.get('buffers', []):
            uri = b.get('uri', '')
            data = base64.b64decode(uri.split(',', 1)[1]) if uri.startswith('data:') else _lookup(files, uri)
            if data is None: raise ValueError(f'The glTF needs {uri}: add it to the import')
            bufs.append(data)
        return parse_gltf(doc, bufs, files, warnings)
    obj = next((b for n, b in files if n.lower().endswith('.obj')), None)
    if obj is not None:
        return parse_obj(obj.decode('utf-8', 'replace'), files, warnings)
    raise ValueError('Give an .obj (with its .mtl and textures), a .gltf (with .bin / images) or a .glb')


# ---------------------------------------------------------------------------------------------------- splitting
def split_pieces(V, U, T, max_verts=PIECE_VERTS):
    """one material's triangles -> pieces of at most max_verts vertices, cut by place (recursive halving)"""
    out = []
    def rec(tri):
        used = np.unique(tri)
        if len(used) <= max_verts:
            remap = -np.ones(len(V), int); remap[used] = np.arange(len(used))
            out.append((V[used], U[used], remap[tri])); return
        c = V[tri].mean(axis=1); ext = V[used].max(0) - V[used].min(0); ax = int(np.argmax(ext))
        mid = np.median(c[:, ax]); a = c[:, ax] < mid
        if a.all() or not a.any(): a = np.arange(len(tri)) < len(tri) // 2
        rec(tri[a]); rec(tri[~a])
    rec(T)
    return out


def assign_chunks(pieces, chunks, warnings):
    """pieces sorted by place along the longest axis -> chunk numbers, each chunk below the collision limits"""
    if not pieces: return []
    cen = np.array([p['centre'] for p in pieces]); ax = int(np.argmax(cen.max(0) - cen.min(0)))
    order = np.argsort(cen[:, ax]); out = [None] * len(pieces); ci = 0; tris = verts = 0
    for i in order:
        p = pieces[i]
        if p['collision'] and (tris + p['tris'] > COL_TRIS or verts + p['verts'] > COL_VERTS) and (tris or verts):
            ci += 1; tris = verts = 0
        if ci >= len(chunks):
            raise ValueError(f'The model needs more than {len(chunks)} chunks of collision ({COL_TRIS} triangles each): make it smaller, '
                             'turn collision off for detail materials, or import it in parts')
        out[i] = chunks[ci]
        if p['collision']: tris += p['tris']; verts += p['verts']
    return out


# ---------------------------------------------------------------------------------------------------- import
def colour_png(rgb):
    im = Image.new('RGBA', (8, 8), tuple(int(max(0, min(1, c)) * 255) for c in rgb) + (255,))
    b = io.BytesIO(); im.save(b, 'PNG'); return b.getvalue()


def fit_texture(data, side):
    im = Image.open(io.BytesIO(data)); im.load()
    w, h = im.size; s = min(1.0, side / max(w, h))
    p2 = lambda x: 1 << max(3, int(round(math.log2(max(8, x)))))   # power of two sides (the game's textures are)
    size = (min(side, p2(w * s)), min(side, p2(h * s)))
    if size != (w, h): im = im.resize(size, Image.LANCZOS)
    b = io.BytesIO(); im.convert('RGBA').save(b, 'PNG'); return b.getvalue(), (w, h), size


def import_geometry(pid, files, options, upload):
    warnings = []
    o = dict(options or {})
    mats, groups = read_model(files, warnings)
    if not groups: raise ValueError('The model has no triangles')
    k = UNITS.get(o.get('units', 'meters'), 1.0) * float(o.get('scale') or 1.0)
    pos = np.asarray(o.get('position') or [0, 0, 0], float)
    allv = np.concatenate([g[0] for g in groups.values()])
    R = np.eye(3)
    if o.get('up', 'y') == 'z': R = np.array([[1, 0, 0], [0, 0, 1], [0, -1, 0]], float)   # Z-up -> Y-up (x, z, -y)
    if o.get('yaw'):   # #128 tileset pieces: turn about the model's origin (the piece's pivot), degrees, like the editor's rotation[1]
        t = math.radians(float(o['yaw'])); c_, s_ = round(math.cos(t), 9), round(math.sin(t), 9)
        R = np.array([[c_, 0, s_], [0, 1, 0], [-s_, 0, c_]], float) @ R
    lift = 0.0
    if o.get('ground'): lift = -float((allv @ R.T)[:, 1].min()) * k
    surfaces = o.get('surfaces') or {}; default_surface = int(o.get('defaultSurface') or DEFAULT_SURFACE)
    collide = o.get('collision', True); side = int(o.get('maxTexture') or 512)
    material_ids = {}; materials_out = []; pieces = []; resized = []
    for name, (V, U, T) in groups.items():
        m = mats.get(name, {})
        if name not in material_ids:
            data, label = (m.get('image'), m.get('imageName') or name)
            if data is not None:
                try:
                    png, was, now = fit_texture(data, side)
                    if was != now: resized.append(f'{label} {was[0]}x{was[1]} -> {now[0]}x{now[1]}')
                except Exception as e:
                    warnings.append(f'Texture {label} could not be read ({e}): its colour is used.'); png = colour_png(m.get('colour', [0.8] * 3))
            else:
                png = colour_png(m.get('colour', [0.8] * 3))
            a = upload(f'{name}.png', png, 'texture'); materials_out.append(a); material_ids[name] = a['id']
        Vw = (V @ R.T) * k + pos + [0, lift, 0]
        Tw = T[:, [0, 2, 1]] if o.get('flipWinding') else T
        good = np.isfinite(Vw).all(axis=1)
        if not good.all(): raise ValueError(f'Material {name}: the model has broken (NaN) vertices')
        surface = int(surfaces.get(name, default_surface))
        # weld: exporters write a vertex per face corner wherever normals are split (Blender's flat shading: 4x the
        # vertices); normals are not used here, so corners with the same place and UV become one vertex again
        key = np.round(np.c_[Vw, U * 4096], 3)
        uniq, inv = np.unique(key, axis=0, return_inverse=True)
        if len(uniq) < len(Vw):
            first = np.zeros(len(uniq), int); first[inv[::-1]] = np.arange(len(Vw))[::-1]
            Vw, U, Tw = Vw[first], U[first], inv.ravel()[Tw]
        Tw = Tw[(Tw[:, 0] != Tw[:, 1]) & (Tw[:, 1] != Tw[:, 2]) & (Tw[:, 0] != Tw[:, 2])]   # drop degenerate triangles
        for Vp, Up, Tp in split_pieces(Vw, U, Tw):
            pieces.append(dict(material=name, V=Vp, U=Up, T=Tp, centre=Vp.mean(0), verts=len(Vp), tris=len(Tp), surface=surface,
                               collision=bool(collide) and surface != 0))
    chunks = [int(c) for c in (o.get('chunks') or [1])]
    where = assign_chunks(pieces, chunks, warnings)
    geometry = []
    group = uuid.uuid4().hex; base = o.get('name') or 'Imported'
    for i, (p, ch) in enumerate(zip(pieces, where)):
        c = p['centre']
        geometry.append(dict(id=uuid.uuid4().hex, kind='geometry', shape='mesh', name=f"{base} {p['material']}" + (f' {i + 1}' if len(pieces) > 1 else ''),
                             group=group, position=[round(float(x), 3) for x in c], rotation=[0, 0, 0], scale=[1, 1, 1], chunk=int(ch),
                             collision=p['collision'], surface=p['surface'], material=material_ids[p['material']], uvRepeat=[1, 1], colour=[255, 255, 255, 255],
                             vertices=np.round(p['V'] - c, 3).tolist(), uvs=np.round(p['U'], 5).tolist(), triangles=p['T'].tolist(), importedFrom=base))
    allw = np.concatenate([p['V'] for p in pieces]); lo = allw.min(0) - 400; hi = allw.max(0) + 400
    used = sorted(set(where))
    chunk_boxes = {str(c): [[lo.round(1).tolist(), hi.round(1).tolist()]] for c in used}
    stats = dict(materials=len(material_ids), pieces=len(pieces), triangles=int(sum(p['tris'] for p in pieces)), vertices=int(sum(p['verts'] for p in pieces)),
                 chunks=used, bounds=[allw.min(0).round(1).tolist(), allw.max(0).round(1).tolist()],
                 collisionTriangles={str(c): int(sum(p['tris'] for p, w in zip(pieces, where) if w == c and p['collision'])) for c in used})
    if resized:   # one line, not one per texture (Forge sweep 2026-10-21)
        warnings.append(f'{len(resized)} texture(s) made smaller to fit the game (largest side {side}, powers of two): ' + ', '.join(resized[:6]) + (' ...' if len(resized) > 6 else '') + '.')
    return dict(materials=materials_out, geometry=geometry, chunkBoxes=chunk_boxes, warnings=warnings, stats=stats)
