"""Custom stages from scratch (overnight plan C, 2026-10-18; docs/custom-stages/).

project.customStage = {
  description: str, cover: <asset id of an image in project.customAssets> | null, author: str,
  goals: {hero: GOAL, dark: GOAL, neutral: GOAL}     (a route left out = no mission on that route)
}
GOAL = {type: 'goal'} | {type: 'defeat', count: n} | {type: 'rings', count: n} | {type: 'timer', seconds: s} | {type: 'reach'}
       + optional target: <object id> (the Goal Ring / the spot to reach, picked in the 3D view) and next: <stage number>
       (null = back to the stage select / launcher it was started from = what Select Mode does natively; a stage =
       chain to it after the results, runtime only, #99)

Slot 100 (Westopolis, the resource slot of mode 'new' stages) has these native mission records (missions-phase7.md):
  Normal (neutral) = GoalMission id 0 (the goal ring), Dark = EnemyMission id 1 (defeat GUN, MISSIONCOUNT_D),
  Hero = EnemyMission id 2 (defeat Black Arms, MISSIONCOUNT_H).
So with NO runtime change: neutral 'goal', dark 'defeat' (GUN enemies), hero 'defeat' (Black Arms enemies) work. Every
other goal (rings, timer, a goal ring for hero / dark) needs a custom mission record (id 41+) and the slot patch
(runtime, missions-phase7.md step 3): the build writes them into mods/custom_stage.json for that runtime.

Package (.shadowstage = zip): manifest.json, project.json (no 'reference'), assets/<file> (the project's uploads: models,
textures, cover, sounds), README.txt. Import gives a NEW project id (never overwrites).
Stats (written by the runtime): <game folder>/stats/custom/<projectId>.jsonl, one JSON object per finished or
failed run (format in docs/custom-stages/STATS-FORMAT.md); the stage list reads every playtest folder of the project."""
import base64, io, json, time, uuid, zipfile
from pathlib import Path

FORMAT = 1
NATIVE = {   # slot 100's records: route -> (goal type it can do natively, faction / meaning, nukkoro key)
    'neutral': ('goal', 'goal ring', None),
    'dark': ('defeat', 'GUN', 'dark'),
    'hero': ('defeat', 'Black Arms', 'hero'),
}
GUN = ('GUNSoldier', 'GUNBeetle', 'GUN Robot', 'GUNRobot', 'GUN Bigfoot', 'GUNBigfoot')
BLACK_ARMS = ('Bk Soldier', 'Bk Wing Small', 'Bk Wing Large', 'Bk Giant', 'Bk Ninja', 'Bk Worm', 'Bk Chaos', 'Brown Cocoon', 'Black Arms Mine')


def S():
    import server
    return server


def cs(p):
    return p.get('customStage') or {}


def enemies(p, names):
    return [o for o in p['objects'] if o.get('kind') == 'native' and o.get('nativeName', o.get('name')) in names]


def plan_goals(p):
    """{route: dict(goal, how='native'|'runtime', note)}"""
    out = {}
    for route, g in (cs(p).get('goals') or {}).items():
        if route not in NATIVE or not isinstance(g, dict) or g.get('type') not in ('goal', 'defeat', 'rings', 'timer', 'reach'):
            out[route] = dict(goal=g, how='invalid', note=f'unknown route / goal {g!r}'); continue
        ntype, what, _ = NATIVE[route]
        if p.get('mode') == 'new' and g['type'] == ntype:
            what_txt = 'the goal ring' if ntype == 'goal' else f"defeat {what} x {g.get('count')}"
            out[route] = dict(goal=g, how='native', note=f'works in game: {what_txt}')
        else:
            out[route] = dict(goal=g, how='runtime', note='saved, but the game cannot run this goal yet (it needs a newer game build)')
    return out


def apply_goals(p, warnings):
    """build hook: native goals -> the slot's MISSIONCOUNT lines (p['stageParams'] of this build only)"""
    if not cs(p): return None
    srv = S(); plan = plan_goals(p)
    sp = p.get('stageParams') or srv.stage_params(p['stage'])
    sp = json.loads(json.dumps(sp)); counts = sp.setdefault('counts', {})
    for route, r in plan.items():
        if r['how'] == 'native' and r['goal']['type'] == 'defeat':
            counts[NATIVE[route][2]] = [max(1, int(r['goal'].get('count', 1))), 0]
        if r['how'] != 'native': warnings.append(f"Custom stage goal {route} ({r['goal'].get('type')}): {r['note']}")
    p['stageParams'] = sp
    return plan


def write_sidecar(p, build, warnings):
    if not cs(p): return None
    plan = plan_goals(p); c = cs(p)
    data = dict(format=FORMAT, projectId=p['id'], name=p.get('name'), description=c.get('description', ''), author=c.get('author', ''),
                slot=p['stage'], mode=p.get('mode'), goals=plan, stats=f'stats/custom/{p["id"]}.jsonl', voice=c.get('voice') or 'none',
                customRecords=[dict(route=r, slotIndex={'dark': 0, 'neutral': 1, 'hero': 2}[r], **x['goal']) for r, x in plan.items() if x['how'] == 'runtime'],
                next={r: x['goal'].get('next') for r, x in plan.items() if isinstance(x['goal'], dict) and x['goal'].get('next') is not None},
                targets={r: target_info(p, x['goal'].get('target')) for r, x in plan.items() if isinstance(x['goal'], dict) and x['goal'].get('target')})
    for r, n in data['next'].items():
        warnings.append(f"Custom stage {r}: after the results go to stage {n} - needs the runtime (#99); without it the game returns to the stage select.")
    path = Path(build) / 'mods/custom_stage.json'; path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=1), 'utf-8')
    warnings.append(f"Custom stage: mods/custom_stage.json ({sum(1 for x in plan.values() if x['how'] == 'native')} goal(s) native, "
                    f"{sum(1 for x in plan.values() if x['how'] == 'runtime')} need the runtime).")
    return dict(file='mods/custom_stage.json', goals={k: v['how'] for k, v in plan.items()})


def target_info(p, oid):
    o = next((x for x in p['objects'] if x.get('id') == oid), None)
    return dict(id=oid, name=o.get('name'), position=o.get('position')) if o else dict(id=oid, missing=True)


def validate(p):
    """what a custom stage needs to be playable: errors block, warnings inform"""
    srv = S(); E, W, ok = [], [], []
    starts = [o for o in p['objects'] if o.get('kind') in ('playerstart', 'spawn')]
    (ok if starts else E).append('player start placed' if starts else 'no player start (Add > Player start)')
    floor = [g for g in p.get('geometry', []) if g.get('collision', True) and g.get('enabled', True)]
    if p.get('mode') == 'new':
        (ok if floor else E).append(f'{len(floor)} solid piece' + ('s' if len(floor) != 1 else '') + ' to stand on' if floor else 'no solid geometry: the player falls (add a floor)')
        if starts and floor:
            import numpy as np
            s = starts[0]['position']; hit = False
            for g in floor:
                v = np.asarray(g['vertices'], float) * np.asarray(g.get('scale', [1, 1, 1]), float) + np.asarray(g.get('position', [0, 0, 0]), float)
                if (v[:, 0].min() <= s[0] <= v[:, 0].max()) and (v[:, 2].min() <= s[2] <= v[:, 2].max()) and v[:, 1].min() <= s[1] + 5: hit = True; break
            (ok if hit else E).append('floor under the start' if hit else 'the player start is not above any solid mesh')
    goals = plan_goals(p)
    if not goals: W.append('no goals: the stage can only be left by the pause menu')
    for route, r in goals.items():
        if r['how'] == 'invalid': E.append(f'goal {route}: {r["note"]}')
        elif r['how'] == 'runtime': W.append(f'goal {route}: {r["note"]}')
        else: ok.append(f'goal {route}: {r["note"]}')
        g = r['goal']
        if isinstance(g, dict) and g.get('target') and not any(o.get('id') == g['target'] for o in p['objects']):
            E.append(f'goal {route}: the selected target is not in the stage any more (Select another)')
        if r['how'] == 'native' and g.get('type') == 'goal' and not any(o.get('nativeName', o.get('name')) == 'Goal Ring' for o in p['objects']):
            E.append(f'goal {route}: place a Goal Ring')
        if r['how'] == 'native' and g.get('type') == 'defeat':
            have = len(enemies(p, GUN if route == 'dark' else BLACK_ARMS)); need = int(g.get('count', 1))
            (ok if have >= need else E).append(f'goal {route}: {have} {"GUN" if route == "dark" else "Black Arms"} enemies placed, {need} to defeat')
    if not cs(p).get('description'): W.append('no description (shown in the custom stage list)')
    if not cs(p).get('cover'): W.append('no cover image (the list shows a placeholder)')
    return dict(ok=not E, errors=E, warnings=W, checks=ok)


# ---- package -------------------------------------------------------------------------------------------------------
def export_package(pid):
    srv = S(); p = srv.load_project(pid); p.pop('reference', None)
    d = srv.project_dir(pid); buf = io.BytesIO(); assets = []
    # every file of the project's assets folder the project refers to: customAssets (music, cover) AND the textures that
    # imports / tileset pieces / uploads put there for materials (Forge sweep 2026-10-21: those were left out, so an
    # imported package could not build)
    text = json.dumps(p)
    with zipfile.ZipFile(buf, 'w', zipfile.ZIP_DEFLATED) as z:
        for f in sorted((d / 'assets').glob('*')) if (d / 'assets').is_dir() else []:
            if f.is_file() and (f.name in text or f.stem in text): z.write(f, f'assets/{f.name}'); assets.append(f.name)
        c = cs(p)
        man = dict(format=FORMAT, kind='shadowstage', name=p.get('name'), description=c.get('description', ''), author=c.get('author', ''),
                   slot=p['stage'], mode=p.get('mode'), cover=c.get('cover'), goals={k: v['goal'] for k, v in plan_goals(p).items()},
                   exported=time.strftime('%Y-%m-%d %H:%M'), sourceId=pid, assets=assets, editor='Stage Workshop')
        z.writestr('manifest.json', json.dumps(man, indent=1))
        z.writestr('project.json', json.dumps(p))
        z.writestr('README.txt', f"{p.get('name')}\r\n{c.get('description', '')}\r\n\r\nA Shadow the Hedgehog (recomp) custom stage. Import it in the "
                                 f"editor (Custom stages > Import) - it becomes a new project; build it to play.\r\n")
    return buf.getvalue(), f"{''.join(ch if ch.isalnum() or ch in ' -_' else '_' for ch in p.get('name', 'stage'))[:60].strip() or 'stage'}.shadowstage"


def import_package(data):
    srv = S()
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        man = json.loads(z.read('manifest.json'))
        if man.get('kind') != 'shadowstage' or int(man.get('format', 0)) > FORMAT: raise ValueError('Not a Stage Workshop stage package (or a newer format)')
        p = json.loads(z.read('project.json'))
        p['id'] = uuid.uuid4().hex; p['created'] = time.time(); p.pop('fresh', None)
        p['name'] = str(p.get('name', 'Imported stage'))[:120] + ' (imported)'
        d = srv.project_dir(p['id'])
        for n in z.namelist():
            if n.startswith('assets/') and not n.endswith('/'):
                rel = Path(n[len('assets/'):])
                if rel.is_absolute() or '..' in rel.parts: raise ValueError(f'unsafe asset path {n}')
                t = d / 'assets' / rel; t.parent.mkdir(parents=True, exist_ok=True); t.write_bytes(z.read(n))
    srv.save_project(p)
    return dict(id=p['id'], name=p['name'], manifest=man)


# ---- stage list + stats ------------------------------------------------------------------------------------------------
def stats(pid):
    """every stats line of the project's playtest folders (newest last)"""
    srv = S(); rows = []
    for f in sorted((srv.project_dir(pid) / 'playtests').glob(f'*/stats/custom/{pid}.jsonl')):
        for l in f.read_text('utf-8', 'replace').splitlines():
            try: rows.append(json.loads(l))
            except ValueError: pass
    return sorted(rows, key=lambda r: r.get('time', ''))


def list_stages():
    """the launcher's custom stage list: projects made from scratch or with customStage data"""
    srv = S(); out = []
    for f in sorted(srv.PROJECTS.glob('*/project.json')):
        try: p = json.loads(f.read_text('utf-8'))
        except (OSError, ValueError): continue
        if p.get('mode') != 'new' and not p.get('customStage'): continue
        c = p.get('customStage') or {}; st = stats(p['id'])
        best = max((r.get('score', 0) for r in st if r.get('result') == 'clear'), default=None)
        latest = f.parent / 'latest-build.json'; lb = json.loads(latest.read_text()) if latest.exists() else {}
        cover = next((a for a in p.get('customAssets') or [] if a.get('id') == c.get('cover') or a.get('file') == c.get('cover')), None)
        out.append(dict(id=p['id'], name=p.get('name'), description=c.get('description', ''), author=c.get('author', ''),
                        cover=f"/api/project-asset?id={p['id']}&file={cover['file']}" if cover else None,
                        goals={k: v['goal'] for k, v in plan_goals(p).items()}, bestScore=best, runs=len(st),
                        clears=sum(1 for r in st if r.get('result') == 'clear'), built=bool(lb.get('ok')), lastBuild=lb.get('path'),
                        updated=p.get('updated')))
    return sorted(out, key=lambda r: -(r.get('updated') or 0))


# ---- 'Blank stage' template ------------------------------------------------------------------------------------------
def box_faces(cx, cy, cz, hx, hy, hz):
    f = [((-hx, hy, hz), (hx, hy, hz), (hx, hy, -hz), (-hx, hy, -hz)), ((-hx, -hy, -hz), (hx, -hy, -hz), (hx, -hy, hz), (-hx, -hy, hz)),
         ((hx, -hy, hz), (hx, -hy, -hz), (hx, hy, -hz), (hx, hy, hz)), ((-hx, -hy, -hz), (-hx, -hy, hz), (-hx, hy, hz), (-hx, hy, -hz)),
         ((-hx, -hy, hz), (hx, -hy, hz), (hx, hy, hz), (-hx, hy, hz)), ((hx, -hy, -hz), (-hx, -hy, -hz), (-hx, hy, -hz), (hx, hy, -hz))]
    V, T, U = [], [], []
    for a, b, c, d in f:
        i = len(V); V += [[cx + x, cy + y, cz + z] for x, y, z in (a, b, c, d)]; U += [[0, 0], [1, 0], [1, 1], [0, 1]]; T += [[i, i + 1, i + 2], [i, i + 2, i + 3]]
    return V, T, U


def blank_stage(name='Blank stage', populated=True):
    """a brand-new stage (mode 'new', slot 100): floor 1200 x 2000 + four walls 200 high, start at the south end, goal ring
    at the north end, ring lines, two checkpoints, and (populated) 2 GUN + 2 Black Arms soldiers for the Dark / Hero goals"""
    srv = S(); p = srv.new_stage(dict(name=name, populated=False))
    mat = next(g['material'] for g in p['geometry'] if g['name'] == 'Starter rectangle')
    p['geometry'] = []
    def mesh(nm, V, T, U, rep=(8, 8)):
        p['geometry'].append(dict(id=uuid.uuid4().hex, kind='geometry', name=nm, shape='mesh', position=[0, 0, 0], rotation=[0, 0, 0], scale=[1, 1, 1],
                                  vertices=V, triangles=T, uvs=U, uvRepeat=list(rep), colour=[220, 220, 220, 255], chunk=1, collision=True, surface=0x00020001, material=mat))
    mesh('Floor', *box_faces(0, 0, 0, 600, 20, 1000), rep=(8, 14))
    for nm, (cx, cz, hx, hz) in {'Wall west': (-620, 0, 20, 1000), 'Wall east': (620, 0, 20, 1000), 'Wall south': (0, 1020, 640, 20), 'Wall north': (0, -1020, 640, 20)}.items():
        mesh(nm, *box_faces(cx, 120, cz, hx, 100, hz), rep=(8, 2))
    p['objects'] = [o for o in p['objects'] if o.get('kind') not in ('spawn', 'goalpose') and o.get('nativeName', o.get('name')) != 'Goal Ring']
    p['objects'].append(dict(id=uuid.uuid4().hex, kind='playerstart', player=0, name='Player start', position=[0, 25, 850], rotation=[0, 0, 0], scale=[1, 1, 1]))
    def place(label, pos, **kw):
        src = next(o for o in p['nativeTemplates'] if o.get('nativeName', o['name']) == label)
        o = json.loads(json.dumps(src)); o.update(id=uuid.uuid4().hex, position=list(pos), rotation=[0, 0, 0], link=0, render=src.get('render') or 10, layer='cmn', **kw)
        o.pop('sourceIndex', None); p['objects'].append(o); return o
    place('Goal Ring', (0, 40, -850))
    for g in srv.goal_pose_objects(p.get('stageParams')): g['position'] = [0, 20, -930]; p['objects'].append(g)
    for z in (600, 200, -200, -600):
        r = place('Ring', (0, 35, z)); b = bytearray(base64.b64decode(r['params'])); import struct; struct.pack_into('<iiff', b, 0, 1, 5, 160.0, 0.0); r['params'] = base64.b64encode(bytes(b)).decode()
    place('Checkpoint', (0, 20, 300)); place('Checkpoint', (0, 20, -300))
    if populated:
        place('GUNSoldier', (-300, 20, 0)); place('GUNSoldier', (300, 20, -100))
        place('Bk Soldier', (-300, 20, -450)); place('Bk Soldier', (300, 20, -550))
    p['customStage'] = dict(description='A small arena made from scratch: run north to the goal ring (Neutral), defeat the 2 GUN soldiers (Dark) '
                                        'or the 2 Black Arms soldiers (Hero).', author='', cover=None,
                            goals=dict(neutral=dict(type='goal'), dark=dict(type='defeat', count=2), hero=dict(type='defeat', count=2)))
    p['playerStart'] = dict(removedOriginal=True, yawUnits='radians-line')
    p['editor'] = {'camera': {'position': [1500, 1400, 1700], 'target': [0, 0, 0]}}
    srv.save_project(p); return p
