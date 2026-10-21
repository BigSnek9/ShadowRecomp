"""#128 batch 1 proof: a street assembled ONLY from tileset pieces (work/tilesets) in a blank custom stage, through the same
code path the Tilesets tab uses (server.tileset_piece). Prints the project id.
    python tools/tilesets/assemble_street.py
Layout (game units; pivots = piece min corners, yaw in 90 degree steps): the main road x -100..100 runs from the start
(z 850) toward -z: straights, a zebra crossing, a crossroads at z 0..200 with an east arm and a west arm that turns south
through the curve, then a ramp up 50 units; towers, a house and a 2-storey building-kit facade beside it; lamps, benches,
a sign and barriers on the sidewalks."""
import shutil, sys, uuid
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'stage_editor')); sys.argv = sys.argv[:1]
import server, custom_stage

Y = 21.0   # the blank stage's floor top is 20: pieces sit 1 unit above it
p = custom_stage.blank_stage('[test] tileset street (#128 batch 1)')
old = p['id']; p['id'] = uuid.uuid5(uuid.NAMESPACE_DNS, 'rouge-128-street').hex; p.pop('reference', None)
shutil.rmtree(server.project_dir(p['id']), ignore_errors=True); server.save_project(p); shutil.rmtree(server.project_dir(old), ignore_errors=True)
geo, mats, boxes, n = [], {}, {}, 0


def put(kit, piece, x, z, yaw=0, y=Y):
    global n
    r = server.tileset_piece(dict(project=p['id'], kit=kit, piece=piece, position=[x, y, z], yaw=yaw))
    geo.extend(r['geometry']); n += 1
    for m in r['materials']: mats[m['id']] = m
    for ch, bl in (r.get('chunkBoxes') or {}).items():   # one box per chunk round every piece
        for lo, hi in bl:
            if ch in boxes: boxes[ch] = [[min(a, b) for a, b in zip(boxes[ch][0], lo)], [max(a, b) for a, b in zip(boxes[ch][1], hi)]]
            else: boxes[ch] = [list(lo), list(hi)]


# main road (yaw 0: a 200 x 200 tile spans x..x+200, z..z+200)
for z, piece in ((800, 'road_straight'), (600, 'road_straight'), (400, 'road_crossing'), (200, 'road_straight'), (0, 'road_cross'),
                 (-200, 'road_straight'), (-400, 'road_straight')):
    put('roads', piece, -100, z)
put('roads', 'road_ramp', 100, -400, 180)            # yaw 180 about the pivot: spans x -100..100, z -800..-400, rising toward -z
put('roads', 'road_straight', 100, 200, 90)          # east arm: yaw 90 spans x 100..300, z 0..200
put('roads', 'road_straight', -300, 200, 90)         # west arm x -300..-100
put('roads', 'road_curve', -500, 0)                  # curve: east socket meets the west arm, south socket leads to -z
put('roads', 'road_straight', -500, -200)
put('roads', 'kerb', -500, -230, 0)                  # a kerb module closing the side road's end
# buildings
put('skyscrapers', 'tower_glass', -300, 600)
put('skyscrapers', 'tower_office', 100, 600)
put('skyscrapers', 'tower_night', 100, -400)
put('houses', 'house_brick', -200, 400)
put('houses', 'house_long', -250, -400)
for k, z0 in enumerate((300, 350, 400)):              # facade on the east sidewalk edge (x 100), walls face -x (yaw 90)
    put('building', 'wall_door' if k == 1 else 'wall_window', 100, z0, 90)
    put('building', 'wall_window', 100, z0, 90, Y + 50)
    put('building', 'roof', 100, z0, 90, Y + 100)
    put('building', 'trim', 100, z0, 90, Y + 94)
for z0 in (245, 400):
    for yy in (Y, Y + 50): put('building', 'corner', 100, z0, 0, yy)
# props
for zz in (750, 550, 350, 150, -150, -350):
    put('props', 'lamp', -55, zz - 25, -90)          # pole on the west sidewalk (x -80), arm over the road
for zz in (650, 450):
    put('props', 'bench', -55, zz, -90)
put('props', 'sign', 50, 500)
for x in (-100, -50, 0, 50):
    put('props', 'barrier', x, -900, 0, 20.5)

p = server.load_project(p['id'])
p['geometry'] = (p.get('geometry') or []) + geo
known = {m['id'] for m in p.get('materials', [])}
p.setdefault('materials', []).extend(m for i, m in mats.items() if i not in known)
cb = dict(p.get('chunkBoxes') or {})
for ch, b in boxes.items(): cb[ch] = (cb.get(ch) or []) + [b]
p['chunkBoxes'] = cb
for o in p['objects']:
    if o.get('kind') == 'playerstart': o.update(position=[0, 30, 850], rotation=[0, 0, 0])
server.save_project(p)
b = server.build_project(server.load_project(p['id']))
print(f'{n} pieces placed, build', b['ok'], b.get('errors'))
for w in b.get('warnings', [])[:6]: print('  ', w[:160])
print(p['id'])
