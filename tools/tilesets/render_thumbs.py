"""#128 thumbnails: a 256 x 256 three-quarter render of every tileset piece, saved as <id>.png next to its .glb
(Blender headless, factory settings; Workbench engine with the textures, a soft studio light, transparent background).
    "C:\\Program Files\\Blender Foundation\\Blender 4.3\\blender.exe" -b --factory-startup --python tools/tilesets/render_thumbs.py -- [work/tilesets]"""
import glob, json, math, os, sys
import bpy
from mathutils import Vector

ROOT = sys.argv[sys.argv.index('--') + 1] if '--' in sys.argv and len(sys.argv) > sys.argv.index('--') + 1 else os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), 'work', 'tilesets')
SIZE = 256


def render(glb, png):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=glb)
    obs = [o for o in bpy.context.scene.objects if o.type == 'MESH']
    pts = [o.matrix_world @ Vector(c) for o in obs for c in o.bound_box]
    lo = Vector((min(p.x for p in pts), min(p.y for p in pts), min(p.z for p in pts)))
    hi = Vector((max(p.x for p in pts), max(p.y for p in pts), max(p.z for p in pts)))
    centre, size = (lo + hi) / 2, (hi - lo).length
    sc = bpy.context.scene
    sc.render.engine = 'BLENDER_WORKBENCH'
    sh = sc.display.shading; sh.light = 'STUDIO'; sh.color_type = 'TEXTURE'; sh.show_shadows = True; sh.show_cavity = True
    sc.render.film_transparent = True; sc.render.resolution_x = sc.render.resolution_y = SIZE
    sc.render.image_settings.file_format = 'PNG'; sc.render.image_settings.color_mode = 'RGBA'
    cam = bpy.data.objects.new('cam', bpy.data.cameras.new('cam')); sc.collection.objects.link(cam); sc.camera = cam
    cam.data.type = 'ORTHO'; cam.data.ortho_scale = size * 1.05
    d = Vector((1.0, 1.3, 0.9)).normalized()   # front-right from above: the street side (pieces face game -z = Blender +y)
    cam.location = centre + d * size * 2
    cam.rotation_euler = (centre - cam.location).to_track_quat('-Z', 'Y').to_euler()
    cam.data.clip_end = size * 10
    sc.render.filepath = png
    bpy.ops.render.render(write_still=True)


n = 0
for kj in sorted(glob.glob(os.path.join(ROOT, '*', 'kit.json'))):
    kit = json.load(open(kj)); d = os.path.dirname(kj)
    for pc in kit['pieces']:
        render(os.path.join(d, pc['file']), os.path.join(d, pc['id'] + '.png')); pc['thumb'] = pc['id'] + '.png'; n += 1
    json.dump(kit, open(kj, 'w'), indent=1)
print('THUMBS_OK', n)
