"""Lighting L2 offline bake: per stage, per chunk voxel occupancy of the static world (sky visibility / AO for the
runtime's lighting_occlusion.cpp). File format: docs/lighting/OCCUPANCY-FORMAT.md (keep the two in step).

  python tools/stage_editor/lighting_bake.py --all [--out lighting] [--report agents/claude/out/lightbake]
  python tools/stage_editor/lighting_bake.py --stage 100 [--slice-y -60] [--slice-z 0]
The Stage Workshop server calls bake_stage(stage, override=<build files dir>) for a project build (authored geometry
and stage-art edits are already inside the build's chunk archives)."""
import argparse, json, os, re, struct, sys, time, zlib
from pathlib import Path
import numpy as np
from formats import read_world, first, one

ROOT = Path(__file__).resolve().parents[2]
DISC = Path(os.environ.get('SHADOW_DISC', str(ROOT.parent / 'Shadow')))
MAGIC = b'SHOCC1\0\0'; VERSION = 1; EMPTY = 0xFFFFFFFF; BRICK_BYTES = 136
VOXEL = 16.0           # game units (1 unit ~ 10 cm: SHADOW_BODY.DFF is ~11 units tall) -> ~1.6 m
MAX_AXIS = 2048        # voxels per axis before the chunk's voxel size doubles
MAX_BRICKS = 4 << 20   # brick-index cells (16 MB) before the chunk's voxel size doubles
BOX_MARGIN = 1600.0    # geometry outside the chunk's BDT boxes grown by this is not baked
BACKDROP_SPAN = 14000  # same rule as server.import_stage: every visual world wider than this = sky backdrop

# ---- sources ------------------------------------------------------------------------------------------------------
def read_bdt(files):
    """{chunk: [(lo, hi), ...]} from stgNNNN_dat.one's BDT (same as server.read_bdt)"""
    name = next((n for n in files if n.upper().endswith('.BDT')), None); out = {}
    if not name: return out
    b = files[name]; count = struct.unpack_from('<I', b, 64)[0]
    for i in range(count):
        at = 68 + i * 32; num = struct.unpack_from('<I', b, at)[0]
        out.setdefault(num, []).append((np.array(struct.unpack_from('<3f', b, at + 4)), np.array(struct.unpack_from('<3f', b, at + 16))))
    return out

def main_collisions(files):
    """main collision files of a chunk (server.collision_files / collision_role: '_' companions are not main)"""
    cols = [n for n in files if 'COLI' in n.upper() and n.upper().endswith('.BSP')]
    stems = {n.upper()[:-4] for n in cols}
    return [n for n in cols if not (n.upper()[:-4] != n.upper()[:-4].rstrip('_') and n.upper()[:-4].rstrip('_') in stems)]

def triangles(meshes):
    out = []
    for m in meshes:
        if not m.get('vertices') or not m.get('triangles'): continue
        v = np.asarray(m['vertices'], np.float64); t = np.asarray(m['triangles'], np.int64)
        out.append(v[t])
    return np.concatenate(out) if out else np.zeros((0, 3, 3))

def stage_chunks(stage, override=None):
    """{chunk: dict(visual=(N,3,3), collision=(N,3,3), backdrop=bool, archive)} + crc of the archives read.
    override = a build's files/stgNNNN folder: its chunk archives replace the disc's."""
    src = DISC / 'files' / f'stg{stage:04d}'; paths = {p.name.lower(): p for p in src.glob(f'stg{stage:04d}_[0-9][0-9].one')}
    if override and Path(override).is_dir():
        paths.update({p.name.lower(): p for p in Path(override).glob(f'stg{stage:04d}_[0-9][0-9].one')})
    chunks = {}; crc = 0
    for name in sorted(paths):
        raw = paths[name].read_bytes(); crc = zlib.crc32(raw, crc); files = one.read_all(raw); num = int(name[-6:-4])
        vis = []; spans = []
        for n, d in files.items():
            if not n.upper().endswith('.RG1'): continue
            try: t = triangles(read_world(d)['meshes'])
            except (ValueError, IndexError, struct.error): continue
            if len(t): p = t.reshape(-1, 3); spans.append(float(max(np.ptp(p[:, 0]), np.ptp(p[:, 2])))); vis.append(t)
        col = []
        for n in main_collisions(files):
            try: col.append(triangles(read_world(files[n], True)['meshes']))
            except (ValueError, IndexError, struct.error): continue
        cat = lambda xs: np.concatenate(xs) if xs else np.zeros((0, 3, 3))
        chunks[num] = dict(visual=cat(vis), collision=cat(col), backdrop=bool(spans) and min(spans) > BACKDROP_SPAN, archive=name)
    return chunks, crc

# ---- voxelisation -------------------------------------------------------------------------------------------------
def sample_voxels(tris, origin, s, dims):
    """sorted unique linear voxel indices (x + Dx*(y + Dy*z)) touched by the triangles' surfaces: barycentric point
    grid with spacing <= s/2 along every edge"""
    if not len(tris): return np.zeros(0, np.int64)
    D = np.array(dims); lo = tris.min(1); hi = tris.max(1)
    keep = np.all(hi >= origin, 1) & np.all(lo <= origin + D * s, 1); tris = tris[keep]
    if not len(tris): return np.zeros(0, np.int64)
    L = np.max(np.linalg.norm(tris - np.roll(tris, 1, 1), axis=2), 1)
    n = np.clip(np.ceil(L / (0.5 * s)), 1, 8192).astype(np.int64); found = []
    for k in np.unique(n):
        grp = tris[n == k]; i, j = np.meshgrid(np.arange(k + 1), np.arange(k + 1), indexing='ij'); m = (i + j) <= k
        u = (i[m] / k)[None, :, None]; w = (j[m] / k)[None, :, None]; P = u.shape[1]
        step = max(1, (2 << 20) // P)
        for a in range(0, len(grp), step):
            g = grp[a:a + step]
            pts = g[:, None, 0] + u * (g[:, None, 1] - g[:, None, 0]) + w * (g[:, None, 2] - g[:, None, 0])
            v = np.floor((pts.reshape(-1, 3) - origin) / s).astype(np.int64)
            ok = np.all((v >= 0) & (v < D), 1); v = v[ok]
            if len(v): found.append(np.unique(v[:, 0] + D[0] * (v[:, 1] + D[1] * v[:, 2])))
    return np.unique(np.concatenate(found)) if found else np.zeros(0, np.int64)

def near(cand, base, dims):
    """the candidates (linear indices) that have a base voxel within one voxel (26-neighbourhood + itself)"""
    if not len(cand) or not len(base): return cand[:0]
    D = np.array(dims); x = cand % D[0]; y = cand // D[0] % D[1]; z = cand // (D[0] * D[1]); hit = np.zeros(len(cand), bool)
    for dz in (-1, 0, 1):
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                X, Y, Z = x + dx, y + dy, z + dz; ok = (X >= 0) & (Y >= 0) & (Z >= 0) & (X < D[0]) & (Y < D[1]) & (Z < D[2])
                lin = X + D[0] * (Y + D[1] * Z); i = np.searchsorted(base, lin).clip(0, len(base) - 1)
                hit |= ok & (base[i] == lin)
    return cand[hit]

def grid_for(tris, boxes):
    """origin, voxel size, dims: geometry bounds clipped to the BDT boxes (+margin), one voxel padding, whole bricks"""
    p = tris.reshape(-1, 3); lo, hi = p.min(0), p.max(0)
    if boxes:
        blo = np.min([b[0] for b in boxes], 0) - BOX_MARGIN; bhi = np.max([b[1] for b in boxes], 0) + BOX_MARGIN
        lo, hi = np.maximum(lo, blo), np.minimum(hi, bhi)
        if np.any(hi <= lo): return None
    s = VOXEL
    while True:
        o = np.floor(lo / s) * s - s; dims = (np.ceil((hi + s - o) / s / 8).astype(np.int64) * 8)
        if dims.max() <= MAX_AXIS and np.prod(dims // 8) <= MAX_BRICKS: return o, s, [int(d) for d in dims]
        s *= 2

# ---- encoding (OCCUPANCY-FORMAT.md) -------------------------------------------------------------------------------
def encode_chunk(occ, dims):
    """section bytes, brick count, mip count for sorted linear level-0 indices"""
    D = np.array(dims); B = D // 8
    x = occ % D[0]; y = occ // D[0] % D[1]; z = occ // (D[0] * D[1])
    blin = (x >> 3) + B[0] * ((y >> 3) + B[1] * (z >> 3)); bricks, inv = np.unique(blin, return_inverse=True)
    index = np.full(int(np.prod(B)), EMPTY, np.uint32); index[bricks] = np.arange(len(bricks), dtype=np.uint32)
    bits = np.zeros((len(bricks), 512), np.uint8); bits[inv, (x & 7) + 8 * (y & 7) + 64 * (z & 7)] = 1
    cube = bits.reshape(-1, 8, 8, 8)                                                    # [n, z, y, x]
    m1 = cube.reshape(-1, 4, 2, 4, 2, 4, 2).sum((2, 4, 6)); m2 = cube.reshape(-1, 2, 4, 2, 4, 2, 4).sum((2, 4, 6))
    rec = np.concatenate([np.packbits(bits, axis=1, bitorder='little'), np.rint(m1.reshape(-1, 64) * 255 / 8).astype(np.uint8),
                          np.rint(m2.reshape(-1, 8) * 255 / 64).astype(np.uint8)], 1)
    out = [index.tobytes(), rec.tobytes()]; mips = 1 + int(np.ceil(np.log2(max(D.max(), 1))))
    for k in range(3, mips):
        Dk = -(-D // (1 << k)); cell = (x >> k) + Dk[0] * ((y >> k) + Dk[1] * (z >> k))
        cnt = np.bincount(cell, minlength=int(np.prod(Dk))).astype(np.float64)
        b = np.rint(cnt * 255 / float(8 ** k)).astype(np.uint8)
        b[(b == 0) & (cnt > 0)] = 1                                                         # keep max = value > 0 exact
        out.append(b.tobytes() + b'\0' * (-len(b) % 4))
    return b''.join(out), len(bricks), mips

def bake_stage(stage, override=None, project=False, log=None):
    """(.occ bytes, report dict)"""
    t0 = time.time(); chunks, crc = stage_chunks(stage, override)
    dat = DISC / 'files' / f'stg{stage:04d}' / f'stg{stage:04d}_dat.one'
    if override and (Path(override) / dat.name).exists(): dat = Path(override) / dat.name   # a build's moved / new chunk boxes
    bdt = read_bdt(one.read_all(dat.read_bytes())) if dat.exists() else {}
    rows = []; sections = []; rep = []
    for num in sorted(chunks):
        c = chunks[num]; vis = c['visual'] if not c['backdrop'] else np.zeros((0, 3, 3)); col = c['collision']
        if not len(vis) and not len(col): rep.append(dict(chunk=num, skipped='backdrop' if c['backdrop'] else 'no geometry')); continue
        g = grid_for(np.concatenate([vis, col]), bdt.get(num))
        if g is None: rep.append(dict(chunk=num, skipped='geometry outside its BDT boxes')); continue
        origin, s, dims = g
        V = sample_voxels(vis, origin, s, dims); C = sample_voxels(col, origin, s, dims)
        Cn = near(np.setdiff1d(C, V, assume_unique=True), V, dims); occ = np.union1d(V, Cn)
        if not len(occ): rep.append(dict(chunk=num, skipped='no voxel')); continue
        sec, nb, mips = encode_chunk(occ, dims); rows.append((num, origin, s, dims, mips, nb, len(occ))); sections.append((zlib.compress(sec, 9), len(sec)))
        rep.append(dict(chunk=num, voxel=s, dims=dims, origin=[round(float(v), 1) for v in origin], voxels=int(len(occ)),
                        visualVoxels=int(len(V)), collisionAdded=int(len(Cn)), collisionDropped=int(len(C) - len(np.intersect1d(C, V)) - len(Cn)),
                        bricks=nb, rawBytes=len(sec), bytes=len(sections[-1][0]), backdrop=c['backdrop']))
        if log: log(f'  chunk {num:2d}: {len(occ)} voxels, {nb} bricks, {len(sec)} bytes, voxel {s:g}, dims {dims}')
    head = MAGIC + struct.pack('<4If I8I', VERSION, stage, len(rows), int(bool(project)), VOXEL, crc, *[0] * 8)
    at = len(head) + 64 * len(rows); table = []
    for (num, origin, s, dims, mips, nb, nv), (sec, raw) in zip(rows, sections):
        table.append(struct.pack('<3I3ff3I6I', num, at, len(sec), *map(float, origin), s, *dims, mips, nb, nv, 0, 0, raw)); at += len(sec)
    data = head + b''.join(table) + b''.join(sec for sec, raw in sections)
    return data, dict(stage=stage, bytes=len(data), rawBytes=64 + 64 * len(rows) + sum(r for _, r in sections), chunks=len(rows), voxels=sum(r[6] for r in rows), seconds=round(time.time() - t0, 2),
                      project=bool(project), detail=rep)

# ---- reading (debug slices, tests; the runtime has its own reader) ----------------------------------------------
def read_occ(data):
    assert data[:8] == MAGIC, 'not an occupancy file'
    version, stage, count, flags, vs, crc = struct.unpack_from('<4IfI', data, 8); out = []
    for i in range(count):
        num, off, size, ox, oy, oz, s, dx, dy, dz, mips, nb, nv, _, _, raw = struct.unpack_from('<3I3ff3I6I', data, 64 + 64 * i)
        sec = zlib.decompress(data[off:off + size]); assert len(sec) == raw
        D = np.array([dx, dy, dz]); B = D // 8; nidx = int(np.prod(B))
        index = np.frombuffer(sec, np.uint32, nidx, 0); rec = np.frombuffer(sec, np.uint8, nb * BRICK_BYTES, 4 * nidx).reshape(nb, BRICK_BYTES)
        out.append(dict(chunk=num, origin=np.array([ox, oy, oz]), voxel=s, dims=D, index=index, bricks=rec, mips=mips, voxels=nv))
    return dict(stage=stage, flags=flags, chunks=out)

def occupied(c, pts):
    """level-0 occupancy of world points (N,3) in one chunk grid"""
    v = np.floor((pts - c['origin']) / c['voxel']).astype(np.int64); D = c['dims']; B = D // 8
    ok = np.all((v >= 0) & (v < D), 1); v = np.where(ok[:, None], v, 0)
    b = c['index'][(v[:, 0] >> 3) + B[0] * ((v[:, 1] >> 3) + B[1] * (v[:, 2] >> 3))]; has = ok & (b != EMPTY)
    k = (v[:, 0] & 7) + 8 * (v[:, 1] & 7) + 64 * (v[:, 2] & 7)
    bits = c['bricks'][np.where(has, b, 0).astype(np.int64), k >> 3] >> (k & 7) & 1
    return has & (bits == 1)

def slice_png(occ, path, axis, value, step=None, chunk_colours=True, region=None):
    """picture of one plane through every chunk grid: axis 'y' = top-down at height value (x right, z down), 'z' = vertical
    (x right, y up), 'x' = vertical (z right, y up); region = (lo, hi) world corners to crop to"""
    from PIL import Image
    cs = occ['chunks']; lo = np.min([c['origin'] for c in cs], 0); hi = np.max([c['origin'] + c['dims'] * c['voxel'] for c in cs], 0)
    if region: lo, hi = np.maximum(lo, region[0]), np.minimum(hi, region[1])
    step = step or min(c['voxel'] for c in cs); a = 'xyz'.index(axis); u, w = dict(y=(0, 2), z=(0, 1), x=(2, 1))[axis]
    nu = int((hi[u] - lo[u]) / step); nw = int((hi[w] - lo[w]) / step)
    while nu * nw > 16e6: step *= 2; nu = int((hi[u] - lo[u]) / step); nw = int((hi[w] - lo[w]) / step)
    U, W = np.meshgrid(lo[u] + (np.arange(nu) + .5) * step, lo[w] + (np.arange(nw) + .5) * step)
    pts = np.zeros((U.size, 3)); pts[:, u] = U.ravel(); pts[:, w] = W.ravel(); pts[:, a] = value
    img = np.full((U.size, 3), 18, np.uint8); rng = np.random.default_rng(7)
    for c in cs:
        inside = np.all((pts >= c['origin']) & (pts < c['origin'] + c['dims'] * c['voxel']), 1); img[inside & (img[:, 0] == 18)] = 40
    for c in cs:
        col = rng.integers(90, 256, 3) if chunk_colours else np.array([235, 235, 235]); img[occupied(c, pts)] = col
    im = img.reshape(nw, nu, 3)
    if axis != 'y': im = im[::-1]                      # vertical slices: up is up
    Image.fromarray(im).save(path); return dict(path=str(path), axis=axis, value=value, step=step, size=[nu, nw])

def stages():
    return sorted(int(p.name[3:]) for p in (DISC / 'files').glob('stg[0-9][0-9][0-9][0-9]') if p.is_dir())

if __name__ == '__main__':
    ap = argparse.ArgumentParser(); ap.add_argument('--all', action='store_true'); ap.add_argument('--stage', type=int, action='append', default=[])
    ap.add_argument('--out', default=str(ROOT / 'lighting')); ap.add_argument('--report')
    ap.add_argument('--slice-y', type=float); ap.add_argument('--slice-z', type=float); ap.add_argument('--slice-x', type=float)
    a = ap.parse_args(); todo = stages() if a.all else a.stage; out = Path(a.out); out.mkdir(parents=True, exist_ok=True)
    report = []; t0 = time.time()
    for st in todo:
        data, r = bake_stage(st); (out / f'stg{st:04d}.occ').write_bytes(data); report.append(r)
        print(f"stg{st:04d}: {r['chunks']} chunks, {r['voxels']} voxels, {r['bytes']} bytes, {r['seconds']} s", flush=True)
        for axis in 'yzx':
            v = getattr(a, f'slice_{axis}')
            if v is not None and a.report:
                print('  ', slice_png(read_occ(data), Path(a.report) / f'stg{st:04d}_slice_{axis}{v:g}.png', axis, v))
    if a.report:
        Path(a.report).mkdir(parents=True, exist_ok=True)
        Path(a.report, 'report.json' if a.all else f'report_{"_".join(map(str, todo))}.json').write_text(json.dumps(dict(
            stages=len(report), seconds=round(time.time() - t0, 1), totalBytes=sum(r['bytes'] for r in report),
            totalVoxels=sum(r['voxels'] for r in report), voxel=VOXEL, perStage=report), indent=1))
    print(f'{len(report)} stages, {sum(r["bytes"] for r in report)} bytes, {time.time() - t0:.1f} s')
