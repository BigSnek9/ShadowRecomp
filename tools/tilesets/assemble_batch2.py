"""#128 batch 2 proof: a street from batch 2 pieces (plus batch 1 roads) in a blank custom stage, through the same code
path the Tilesets tab uses (server.tileset_piece). Prints the project id.
    python tools/tilesets/assemble_batch2.py
Layout (game units; pivots = piece min corners; yaw 0 = street side -z, 90 = -x, 270 = +x, 180 = +z): the main road
x -100..100 runs from the start (z 950) toward -z: a straight, the junction with traffic lights, then an overpass -
road ramp 0 -> 5 m, bridge ramp 5 -> 10 m, two bridge decks over a cross street, and down again - to a closed road end
and a mesh fence. East of the lights a side road turns south through the wide curve to a second road end. Shop fronts
and signs along the first straight, an apartment block opposite with rooftop props, trees and planters on the side
road, a low wall beside the ramp. The blank stage's floor is stretched to hold it all (its walls dropped)."""
import shutil, sys, uuid
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'stage_editor')); sys.argv = sys.argv[:1]
import server, custom_stage

Y = 21.0   # the floor top is 20: pieces sit 1 unit above it
p = custom_stage.blank_stage('[test] tileset batch 2 (#128)')
old = p['id']; p['id'] = uuid.uuid5(uuid.NAMESPACE_DNS, 'rouge-128-batch2').hex; p.pop('reference', None)
shutil.rmtree(server.project_dir(p['id']), ignore_errors=True); server.save_project(p); shutil.rmtree(server.project_dir(old), ignore_errors=True)
geo, mats, boxes, n = [], {}, {}, 0


def grow(ch, lo, hi):
    if ch in boxes: boxes[ch] = [[min(a, b) for a, b in zip(boxes[ch][0], lo)], [max(a, b) for a, b in zip(boxes[ch][1], hi)]]
    else: boxes[ch] = [list(lo), list(hi)]


def put(kit, piece, x, z, yaw=0, y=Y):
    global n
    r = server.tileset_piece(dict(project=p['id'], kit=kit, piece=piece, position=[x, y, z], yaw=yaw))
    geo.extend(r['geometry']); n += 1
    for m in r['materials']: mats[m['id']] = m
    for ch, bl in (r.get('chunkBoxes') or {}).items():
        for lo, hi in bl: grow(ch, lo, hi)


# main road toward -z
put('roads', 'road_straight', -100, 800)
put('roads2', 'road_lights', -100, 600)
put('roads', 'road_ramp', 100, 600, 180)               # z 200..600, 0 -> 5 m toward -z
put('roads2', 'bridge_ramp', 100, 200, 180)            # z -200..200, 5 -> 10 m
put('roads2', 'bridge_deck', -100, -400)
put('roads2', 'bridge_deck', -100, -600)
put('roads2', 'bridge_ramp', -100, -1000)              # z -1000..-600, 10 -> 5 m toward -z
put('roads', 'road_ramp', -100, -1400)                 # z -1400..-1000, 5 -> 0 m
put('roads2', 'road_end', 100, -1400, 180)             # z -1600..-1400, boards at the far end
for x in (-100, -50, 0, 50): put('street2', 'fence_mesh', x, -1650)
for x in (-300, 100): put('roads', 'road_straight', x, -300, 90)   # the cross street under the bridge
# side road east of the lights, through the wide curve, south to a road end
put('roads', 'road_straight', 100, 800, 90)            # x 100..300, z 600..800
put('roads2', 'road_curve_wide', 300, 800, 90)         # west socket x 300 z 600..800, south socket z 400 x 500..700
put('roads', 'road_straight', 500, 200)                # x 500..700, z 200..400
put('roads2', 'road_end', 700, 200, 180)               # z 0..200
# shops (face the road, -x) and an apartment block opposite (+x) with its rooftop
for k, z0 in enumerate((850, 900, 950, 1000)): put('shops', 'shopfront_glass' if k % 2 == 0 else 'shopfront_shutter', 100, z0, 90)
put('shops', 'apartment', -100, 800, 270)              # x -265..-100, z 800..1000
roof = Y + 250
put('rooftop', 'ac_unit', -220, 830, 0, roof); put('rooftop', 'ac_unit', -220, 860, 0, roof)
put('rooftop', 'water_tank', -170, 900, 0, roof); put('rooftop', 'antenna', -240, 960, 0, roof)
put('rooftop', 'stair_hut', -180, 940, 0, roof)
# street furniture
put('street2', 'sign_speed', 110, 1000, 180)           # signs face the runner (+z), on the sidewalks
put('street2', 'sign_parking', -60, 950, 180)
put('street2', 'sign_noentry', 110, -1325, 180)
for x in (125, 225): put('street2', 'tree', x, 760)
for x in (175, 275): put('street2', 'planter', x, 590)
put('street2', 'hazard_board', 520, 660, 90)
for z0 in (300, 350, 400, 450, 500, 550): put('street2', 'wall_low', 100, z0, 90)
for z0 in (-100, -50, 0, 50, 100, 150): put('street2', 'railing', -150, z0, 270)

p = server.load_project(p['id'])
p['geometry'] = [g for g in p.get('geometry') or [] if not g['name'].startswith('Wall ')]
fl = next(g for g in p['geometry'] if g['name'] == 'Floor')
V, T, U = custom_stage.box_faces(250, 0, -300, 950, 20, 1550); fl.update(vertices=V, triangles=T, uvs=U, uvRepeat=[13, 21])
grow('1', [-700, -20, -1850], [1200, 20, 1250])
p['geometry'] += geo
known = {m['id'] for m in p.get('materials', [])}
p.setdefault('materials', []).extend(m for i, m in mats.items() if i not in known)
cb = dict(p.get('chunkBoxes') or {})
for ch, b in boxes.items(): cb[ch] = (cb.get(ch) or []) + [b]
p['chunkBoxes'] = cb
for o in p['objects']:
    if o.get('kind') == 'playerstart': o.update(position=[0, 30, 950], rotation=[0, 0, 0])
    elif o.get('kind') == 'goalpose' or o.get('nativeName', o.get('name')) == 'Goal Ring': o['position'] = [o['position'][0], o['position'][1], o['position'][2] - 650]
p['objects'] = [o for o in p['objects'] if not (-1000 < o['position'][2] < 900 and o.get('kind') not in ('playerstart', 'goalpose'))
                or o.get('nativeName', o.get('name')) == 'Goal Ring']   # ring lines / checkpoints of the blank stage would sit inside the ramps
server.save_project(p)
b = server.build_project(server.load_project(p['id']))
print(f'{n} pieces placed, build', b['ok'], b.get('errors'))
for w in b.get('warnings', [])[:6]: print('  ', w[:160])
print(p['id'])
