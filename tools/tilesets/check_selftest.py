"""Self-test for check_tileset.py: a good box passes; each deliberately broken copy fails with the expected reason.
    python tools/tilesets/check_selftest.py"""
import io, sys
from pathlib import Path
import numpy as np
from PIL import Image
sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_tileset as C

rng = np.random.default_rng(1)
tex = io.BytesIO(); Image.fromarray((rng.random((1024, 1024, 3)) * 255).astype('uint8')).save(tex, 'PNG'); TEX = tex.getvalue()
KIT = dict(cellMetres=5.0, textureSize=1024, unitsPerTexture=50)


def box(lo, hi):
    """closed box, outward triangles, planar UVs (1 repeat / 5 m) - like the generator"""
    (x0, y0, z0), (x1, y1, z1) = lo, hi
    quads = [([(x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)], (0, 0, 1)), ([(x0, y0, z0), (x0, y1, z0), (x1, y1, z0), (x1, y0, z0)], (0, 0, -1)),
             ([(x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1)], (0, -1, 0)), ([(x0, y1, z0), (x0, y1, z1), (x1, y1, z1), (x1, y1, z0)], (0, 1, 0)),
             ([(x0, y0, z0), (x0, y0, z1), (x0, y1, z1), (x0, y1, z0)], (-1, 0, 0)), ([(x1, y0, z0), (x1, y1, z0), (x1, y1, z1), (x1, y0, z1)], (1, 0, 0))]
    V, U, T = [], [], []
    for q, n in quads:
        q = np.array(q, float); n = np.array(n, float)
        if np.dot(np.cross(q[1] - q[0], q[2] - q[0]), n) < 0: q = q[::-1]
        ref = np.array([0, 0, 1.0]) if abs(n[1]) > 0.7 else np.array([0, 1.0, 0])
        t2 = ref - n * np.dot(n, ref); t2 /= np.linalg.norm(t2); t1 = np.cross(t2, n)
        b = len(V); V += q.tolist(); U += [[np.dot(p, t1) / 5, np.dot(p, t2) / 5] for p in q]; T += [[b, b + 1, b + 2], [b, b + 2, b + 3]]
    return np.array(V), np.array(U), np.array(T)


def run(name, groups, cells=(1, 1, 1), expect=None):
    C.load = lambda path: ({k: dict(image=TEX) for k in groups}, groups)
    fails, _, _ = C.check_piece(Path('.'), dict(file='x.glb', cells=list(cells)), KIT, None)
    ok = (not fails) if expect is None else any(expect in f for f in fails)
    print(f"{'PASS' if ok else 'WRONG'} {name}: {fails or 'clean'}")
    return ok


good = box((0, 0, 0), (5, 5, 5))
V, U, T = good
results = [run('good box', {'m': good})]
U2 = U.copy(); U2[:, 0] *= 1.5; results.append(run('stretched UVs', {'m': (V, U2, T)}, expect='stretch'))
T2 = T.copy(); T2[0] = T2[0][[0, 2, 1]]; T2[1] = T2[1][[0, 2, 1]]; results.append(run('inverted face', {'m': (V, U, T2)}, expect='inverted'))
V3 = V.copy(); V3[V3[:, 0] > 4.9, 0] += 0.02; V3[(V3[:, 0] > 4.9) & (np.arange(len(V3)) % 3 == 0), 0] -= 0.02
results.append(run('crack (vertices 2 cm apart)', {'m': (V3, U, T)}, expect='seams'))
results.append(run('box inside out', {'m': (V, U, T[:, [0, 2, 1]])}, expect='inverted'))
results.append(run('off the grid', {'m': (V + [1.0, 0, 0], U, T)}, expect='grid'))
Vf = V[12:16] - [0, 5, 0]; results.append(run('open floor upside down', {'m': (Vf, U[12:16], np.array([[0, 2, 1], [0, 3, 2]]))}, expect='look down'))
results.append(run('density outlier', {'m': (V, U * 0.5, T)}, expect='density'))
print('self-test', 'OK' if all(results) else 'FAILED'); sys.exit(0 if all(results) else 1)
