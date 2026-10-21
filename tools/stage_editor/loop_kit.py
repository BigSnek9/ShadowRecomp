"""Loop kit (2026-10-20): a runnable vertical loop made by script - its track (a ribbon with collision flag
0x8000, the 'loop surface' the game needs before it attaches a loop) and its loop path (pathType 'loop' = PATH.PTP
settings 0x100; the writer gives it the game's loop frames + section flags, paths.loop_frames). Proven in game with
Glyphic Canyon's own loop collision and with this generated ribbon (docs/stage-workshop/research/loops-56.md).
Rules the game imposes (measured): the player must stand on 0x8000 triangles when the loop is picked up (within ~40 of
the path); nothing else solid may cover the track (a floor ABOVE the run-in keeps the player off the loop surface).
make(entry, yaw=0, radius=250, width=120, run_in=300, run_out=400, lift=2) -> dict(geometry=[row], paths=[row]):
  entry = the run-in's start on the floor (x, y = floor top, z); yaw degrees (0 = running toward -z, like a player
  start); the ribbon lies `lift` above the floor so it, not the floor, is under the player; the loop climbs sideways by
  width + 40 so the run-out lane passes beside the run-in."""
import math, uuid
import numpy as np

LOOP_SURFACE = 0x60008001   # stone + 0x8000 (Glyphic Canyon's loop track)
PATH_ABOVE = 2.5            # the game's loop paths run this far above the track (Glyphic: floor -604, path -601.5)


def _rot(yaw):
    a = math.radians(yaw); c, s = math.cos(a), math.sin(a)
    fwd = np.array([-s, 0.0, -c]); lat = np.array([c, 0.0, -s])   # yaw 0: forward -z, lateral +x
    return fwd, lat


def centreline(radius=250.0, width=120.0, run_in=300.0, run_out=400.0, steps=24):
    """local (forward, lateral, up) samples of the track surface + its 'up' (toward the loop centre / world up)"""
    shift = width + 40.0; pts = []; ups = []
    for k in range(4):   # run-in
        pts.append((run_in * k / 4, 0.0, 0.0)); ups.append((0.0, 0.0, 1.0))
    for k in range(steps + 1):   # the loop: angle 0 at the bottom, climbing forward first
        t = 2 * math.pi * k / steps
        pts.append((run_in + radius * math.sin(t), shift * k / steps, radius - radius * math.cos(t)))
        ups.append((-math.sin(t), 0.0, math.cos(t)))
    for k in range(1, 5):   # run-out on the side lane
        pts.append((run_in + run_out * k / 4, shift, 0.0)); ups.append((0.0, 0.0, 1.0))
    return np.array(pts), np.array(ups)


def make(entry, yaw=0.0, radius=250.0, width=120.0, run_in=300.0, run_out=400.0, lift=2.0, material=None, name='Loop'):
    fwd, lat = _rot(yaw); up = np.array([0.0, 1.0, 0.0]); e = np.asarray(entry, float) + up * lift
    C, U = centreline(radius, width, run_in, run_out)
    W = lambda p: e + fwd * p[0] + lat * p[1] + up * p[2]
    track = np.array([W(p) for p in C]); upw = np.array([fwd * u[0] + lat * u[1] + up * u[2] for u in U])
    verts = []; tris = []; uvs = []
    for i, (p, u) in enumerate(zip(track, upw)):
        verts += [(p - lat * width / 2).tolist(), (p + lat * width / 2).tolist()]
        uvs += [[0.0, i / 4], [1.0, i / 4]]
    for i in range(len(track) - 1):
        a, b, c, d = 2 * i, 2 * i + 1, 2 * i + 2, 2 * i + 3
        n = np.cross(np.subtract(verts[c], verts[a]), np.subtract(verts[b], verts[a]))
        tris += [[a, c, b], [b, c, d]] if np.dot(n, upw[i]) > 0 else [[a, b, c], [b, d, c]]   # faces toward 'up'
    path_pts = (track + upw * PATH_ABOVE).round(2).tolist()
    gid = uuid.uuid4().hex
    geom = dict(id=gid, kind='geometry', shape='mesh', name=f'{name} track', position=[0, 0, 0], rotation=[0, 0, 0], scale=[1, 1, 1],
                vertices=np.round(verts, 2).tolist(), triangles=tris, uvs=uvs, uvRepeat=[1, 1], colour=[200, 200, 210, 255], chunk=1,
                collision=True, surface=LOOP_SURFACE, **({'material': material} if material else {}), loopKit=dict(radius=radius, width=width))
    path = dict(id=uuid.uuid4().hex, kind='path', name=f'{name} path', position=[0, 0, 0], rotation=[0, 0, 0], scale=[1, 1, 1],
                pathType='loop', autoOrient=True, closed=False, banking=0, points=path_pts, loopKit=gid)
    return dict(geometry=[geom], paths=[path])
