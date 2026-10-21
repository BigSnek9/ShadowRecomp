"""#128 batch 3 proof: a platforming course + park from batch 3 pieces in a blank custom stage, through the same code path
the Tilesets tab uses (server.tileset_piece, which now also returns a piece's grind-rail paths). Prints the project id.
    python tools/tilesets/assemble_batch3.py
Course (game units, x -100..100, run toward -z from the start at z 900): plaza (z 550..950) -> 14 deg ramp up 5 m
(z 350..550) -> two 5 m blocks (z 300..350) -> a 10 m floating ledge at the same height (z 200..300) -> stairs down
(z 100..200) -> road tunnel (z -100..100) -> lawn with a path (z -300..-100). Beside it: grind rails (straight, rising,
curve) with their paths, pipes, scaffolding, stepping stones, ramps 27 / 45, blocks, fountain, hedges, park trees."""
import shutil, sys, uuid
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'stage_editor')); sys.argv = sys.argv[:1]
import server, custom_stage

Y = 21.0   # the blank stage's floor top is 20
p = custom_stage.blank_stage('[claude test] tileset batch 3 (#128)', populated=False)
old = p['id']; p['id'] = uuid.uuid5(uuid.NAMESPACE_DNS, 'rouge-128-batch3').hex; p.pop('reference', None)
shutil.rmtree(server.project_dir(p['id']), ignore_errors=True); server.save_project(p); shutil.rmtree(server.project_dir(old), ignore_errors=True)
geo, paths, mats, boxes, n = [], [], {}, {}, 0


def put(kit, piece, x, z, yaw=0, y=Y):
    global n
    r = server.tileset_piece(dict(project=p['id'], kit=kit, piece=piece, position=[x, y, z], yaw=yaw))
    geo.extend(r['geometry']); paths.extend(r.get('paths') or []); n += 1
    for m in r['materials']: mats[m['id']] = m
    for ch, bl in (r.get('chunkBoxes') or {}).items():
        for lo, hi in bl:
            if ch in boxes: boxes[ch] = [[min(a, b) for a, b in zip(boxes[ch][0], lo)], [max(a, b) for a, b in zip(boxes[ch][1], hi)]]
            else: boxes[ch] = [list(lo), list(hi)]


# the course (yaw 0 = rising toward +z; yaw 180 spans x px-w..px, z pz-d..pz and rises toward -z)
put('structures', 'plaza', -200, 550)
put('platforming', 'ramp_14', 50, 550, 180)           # z 350..550, up 5 m toward -z
put('platforming', 'block_5', -50, 300); put('platforming', 'block_5', 0, 300)
put('platforming', 'ledge_2', -25, 300, 90)           # yaw 90: x -25..25, z 200..300, top 5 m
put('platforming', 'stairs', -25, 100)                # z 100..200, top at z 200: down toward -z
put('structures', 'tunnel', -100, -100)               # z -100..100
put('park', 'lawn_path', -100, -300)                  # z -300..-100, path along z
# beside the course
put('rails', 'rail_straight', 250, 300)               # x 275, z 300..500, rail path 33.5 up
put('rails', 'rail_slope', 350, 300)                  # x 375, rising toward +z
put('rails', 'rail_curve', 250, 100)
put('rails', 'pipe_straight', -300, 300); put('rails', 'pipe_elbow', -350, 250); put('rails', 'pipe_vertical', -300, 150)
put('scaffold', 'scaffold_bay', 150, 100); put('scaffold', 'scaffold_2', 150, 0)
put('platforming', 'stepping_stones', 150, -100)
put('platforming', 'ramp_27', 250, -200); put('platforming', 'ramp_45', 400, -200)
put('platforming', 'block_10', 150, -200); put('platforming', 'stairs_wide', 300, -400); put('platforming', 'ledge_1', 450, -100)
put('park', 'lawn', -300, -300); put('park', 'fountain', -250, -250)
for x in (-300, -250, -200, -150): put('park', 'hedge', x, -350)
put('park', 'park_tree', -300, -150); put('park', 'park_tree', 100, -250)

p = server.load_project(p['id'])
p['geometry'] = (p.get('geometry') or []) + geo
p['paths'] = (p.get('paths') or []) + paths
known = {m['id'] for m in p.get('materials', [])}
p.setdefault('materials', []).extend(m for i, m in mats.items() if i not in known)
cb = dict(p.get('chunkBoxes') or {})
for ch, b in boxes.items(): cb[ch] = (cb.get(ch) or []) + [b]
p['chunkBoxes'] = cb
for o in p['objects']:
    if o.get('kind') == 'playerstart': o.update(position=[0, 30, 900], rotation=[0, 0, 0])
p['objects'] = [o for o in p['objects'] if not (-700 < o['position'][2] < 880 and o.get('kind') not in ('playerstart', 'goalpose'))
                or o.get('nativeName', o.get('name')) == 'Goal Ring']   # the blank stage's ring lines / checkpoints sit in the course
server.save_project(p)
b = server.build_project(server.load_project(p['id']))
print(f'{n} pieces placed, {len(paths)} rail paths, build', b['ok'], b.get('errors'))
for w in b.get('warnings', [])[:6]: print('  ', w[:160])
print(p['id'])
