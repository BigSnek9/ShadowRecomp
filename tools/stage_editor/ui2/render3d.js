// UI 2: 3D builders ported from the classic editor (app.js) so both draw the same thing. Keep in step with app.js:
// material, geometryFromData, modelGroup, catalogModels, nativePreviewNode, pillarNode, partnerModels, volumeNode,
// playerStartNode, goalPoseNode, scriptObjectNode, destinationNode, and the reference / backdrop / stand-in rules.
import * as THREE from 'three';

// stage = the open project's stage; foreign = models of OTHER stages (Map stage -> models), loaded on demand for
// placements from other stages (All stages source): a name is looked up in this stage's models first
export const ctx = {models: [], artist: {}, catalog: [], materials: [], stage: null, foreign: new Map(), shared: [], recordModels: {}, classModels: {}, drawNotes: {}, tex: () => null};   // shared = single archives (enemy/<Name>Data.one, #54)
export function paramWord(o, i, float = false) { const b = Uint8Array.from(atob(o.params || ''), c => c.charCodeAt(0)); if (!(i >= 0) || b.length < i * 4 + 4) return null; const d = new DataView(b.buffer); return float ? d.getFloat32(i * 4, true) : d.getInt32(i * 4, true); }

// DFF material colour (RGBA) tints the texture and is the only colour of untextured parts
// #33 UV animation (: /api/models materials[].uvAnim = [{duration, frames [{time, uv [a,b,c,d,x,y]}]}]): every
// animation on the disc has the same 2x2 part [0,1,1,0] (the identity in RenderWare's element order) and only the
// position moves, so the editor applies the keyframes RELATIVE to the first one: a scroll by (x - x0, y - y0) in UV
// space, linear between keyframes, looping on duration, from elapsed editor seconds. Each animated material gets its
// own clone of the texture (same image) so its matrix does not move other materials.
const uvAnims = [];
function animated(map, anim) {
  if (!map || !anim?.frames?.length || !(anim.duration > 0)) return map;
  const t = map.clone(); t.matrixAutoUpdate = false; uvAnims.push({ref: new WeakRef(t), anim, x0: anim.frames[0].uv[4], y0: anim.frames[0].uv[5]}); return t;
}
export function animateUV(sec) {
  for (let i = uvAnims.length - 1; i >= 0; i--) {
    const e = uvAnims[i], t = e.ref.deref(); if (!t) { uvAnims.splice(i, 1); continue; }
    const f = e.anim.frames, d = e.anim.duration, s = ((sec % d) + d) % d; let k = 0; while (k < f.length - 2 && f[k + 1].time <= s) k++;
    const a = f[k], b = f[Math.min(k + 1, f.length - 1)], w = b.time > a.time ? Math.min(1, (s - a.time) / (b.time - a.time)) : 0;
    t.matrix.set(1, 0, a.uv[4] + (b.uv[4] - a.uv[4]) * w - e.x0, 0, 1, a.uv[5] + (b.uv[5] - a.uv[5]) * w - e.y0, 0, 0, 1);
  }
}
export const uvAnimCount = () => uvAnims.length;
export function material(m, extra = {}) {
  const c = m?.colour, map = animated(ctx.tex(ctx.texOverride?.(m?.texture) || m?.asset), m?.uvAnim?.[0]), soft = map?.userData.alpha === 'translucent';   // #44: a texture replaced in the project previews everywhere it is used   // gradient-alpha texture (#51): blended
  return new THREE.MeshStandardMaterial({map, color: c ? new THREE.Color(c[0] / 255, c[1] / 255, c[2] / 255) : 0xffffff,
    ...(c && c[3] < 250 ? {transparent: true, opacity: c[3] / 255, depthWrite: false} : {}), roughness: 1, vertexColors: true, side: THREE.DoubleSide, ...extra,
    ...(soft ? {transparent: true, depthWrite: false, alphaTest: .01} : {})});
}
// vertex alpha (prelit RGBA: sand patches, sand falls, glows) is kept as a 4th channel and drawn blended
export function geometryFromData(m) {
  const g = new THREE.BufferGeometry(); g.setAttribute('position', new THREE.Float32BufferAttribute(m.vertices.flat(), 3)); g.setIndex(m.triangles.flat());
  if (m.uvs) g.setAttribute('uv', new THREE.Float32BufferAttribute(m.uvs.flat(), 2));
  if (m.colours) { const fade = m.colours.some(c => (c[3] ?? 255) < 250); g.setAttribute('color', new THREE.Float32BufferAttribute(m.colours.flatMap(c => (fade ? c.slice(0, 4) : c.slice(0, 3)).map(x => x / 255)), fade ? 4 : 3)); g.userData.vertexAlpha = fade; }
  g.computeVertexNormals(); return g;
}
function groupByMaterial(g, order) { let s = 0; for (let i = 1; i <= order.length; i++) if (order[i] !== order[s]) { g.addGroup(s * 3, (i - s) * 3, order[s]); s = i; } }
export function modelGroup(model) {
  const root = new THREE.Group(), mats = model.materials.map(m => material(m, {alphaTest: .1})), blend = model.materials.map(m => material(m, {transparent: true, depthWrite: false, alphaTest: .01}));
  for (const mesh of model.meshes) { const g = geometryFromData(mesh); groupByMaterial(g, mesh.materials); root.add(new THREE.Mesh(g, g.userData.vertexAlpha ? blend : mats)); }
  root.userData.modelName = model.name; root.userData.model = model; return root;
}

// ---- stage art -------------------------------------------------------------------------------------------------------
// backdrop chunks (huge sky layers) draw first without depth; "P" worlds are low-detail distant stand-ins (hidden)
// One stage-art world (e.g. STG0201_O_04.RG1). Kind = the name token after the stage number: O / OS (main geometry),
// G / D (extra layers, meaning not confirmed), P (distant low-detail stand-ins). Returns a Group whose userData holds
// chunk / world / kind; meshes inside are opaque, plus a blended mesh ONLY for the faces whose vertex alpha fades.
// (Making a whole world transparent because a few vertices fade broke depth: big cliffs drew over each other.)
export function worldKind(name) { const t = String(name || '').toUpperCase().replace(/\.RG1$/, '').split(/[_.]/); return ['OS', 'O', 'G', 'D', 'P', 'A'].find(k => t.includes(k)) || '?'; }
export function referenceMesh(m, mats, backdrop) {
  const root = new THREE.Group(); root.userData.chunk = m.chunk; root.userData.world = m.name; root.userData.kind = worldKind(m.name); root.userData.art = m.id;
  const fadeFace = m.colours ? m.triangles.map(t => t.some(i => (m.colours[i]?.[3] ?? 255) < 250)) : null;
  const parts = fadeFace && fadeFace.some(Boolean) ? [[false, i => !fadeFace[i]], [true, i => fadeFace[i]]] : [[false, () => true]];
  const order = m.materials || Array(m.triangles.length).fill(0);
  for (const [blend, keep] of parts) {
    const ids = m.triangles.map((_, i) => i).filter(keep); if (!ids.length) continue;
    const g = geometryFromData({...m, triangles: ids.map(i => m.triangles[i])}); groupByMaterial(g, ids.map(i => order[i]));
    let mat = mats;
    if (backdrop) mat = mat.map(x => { const c = x.clone(); c.depthWrite = false; c.side = THREE.DoubleSide; return c; });   // inside-out: seen from inside the shell as sky
    if (blend) mat = mat.map(x => { const c = x.clone(); c.transparent = true; c.depthWrite = false; c.alphaTest = .01; return c; });
    const mesh = new THREE.Mesh(g, mat); mesh.userData.faceMap = ids; if (backdrop) { mesh.renderOrder = -10; mesh.frustumCulled = false; mesh.layers.set(1); }   // layer 1 = the sky pass (ui2 tick)   // #74: pickable (the viewport tries other art first), never culled   // faceMap: drawn triangle -> original face (#73)
    root.add(mesh);
  }
  if (backdrop) root.userData.backdrop = true;
  if (!backdrop && root.userData.kind === 'P') root.userData.standin = true;
  return root;
}

// ---- native objects --------------------------------------------------------------------------------------------------
const nativeModelNames = {'Goal Ring': 'GOALRING.DFF', 'Ring': 'RING.DFF', 'Spring Shadow': 'SPRING00.DFF', 'Wide Spring': 'SPRINGB00.DFF', 'Dash Panel': 'DASHPANEL00.DFF', 'Checkpoint': 'SAVEPOINT00.DFF', 'Weapon': 'SETWEAPON.DFF', 'Weapon Box': 'BOXGW.DFF', 'Metal Box': 'BOXGM.DFF', 'Wood Box': 'BOXEW.DFF', 'Unbreakable Box': 'BOXGU.DFF', 'Balloon': 'BALLOON00.DFF', 'Bk Soldier': 'SOLDIERBK.DFF', 'GUNSoldier': 'SOLDIERGUN_ARK00.DFF', 'GUNBeetle': 'BEETLEGUN.DFF', 'Bk Wing Small': 'WINGBKSMALL.DFF', 'Platform': 'CT0000FOOTING.DFF', 'Roadblock': 'CT0000BARRICADE.DFF'};
let extra = null;   // the foreign pool while objectNode() builds a placement from another stage
const find = n => { const u = n.toUpperCase(), eq = m => m.name.toUpperCase() === u; return ctx.models.find(eq) || extra?.find(eq) || ctx.shared.find(eq); };
export const isForeign = o => o.kind === 'native' && o.sourceStage != null && ctx.stage != null && o.sourceStage !== ctx.stage;
function catalogModels(o) {
  // Destructable1 / Decoration1: the stage's own ARTISTGADGET / ARTISTWORK table entry (DestructableType / DecorationType)
  if (o.list === 0x25 && (o.type === 0x89 || o.type === 0x88)) { const e = (ctx.artist[o.type === 0x89 ? 'gadget' : 'work'] || [])[paramWord(o, 0) ?? -1]; return e ? e.models.map(find).filter(Boolean) : []; }
  const c = ctx.catalog.find(c => c.list === o.list && c.type === o.type); if (!c?.models?.length) return [];
  let set = 0; if (c.modelSetting) { const fi = (c.fields || []).findIndex(f => f.name === c.modelSetting); if (fi >= 0) set = paramWord(o, fi) ?? 0; }
  set = Math.max(0, Math.min(c.models.length - 1, set | 0)); return c.models[set].map(find).filter(Boolean);
}
// Collapsing Pillar (0837): base by ObjectType + five stones + head, 30 units per piece (approximation from bounds)
function pillarNode(o) {
  const base = find(['CN0000ROLLOBJECTFLAT.DFF', 'CN0201ROLLOBJECTSLOPE.DFF', 'CN0402ROLLOBJECTAIR.DFF'][paramWord(o, 0) ?? 0] || 'CN0000ROLLOBJECTFLAT.DFF') || find('CN0000ROLLOBJECTFLAT.DFF');
  const stone = find('CN0000ROLLOBJECTNML.DFF'), head = find('CN0000ROLLOBJECTHEAD.DFF'); if (!base || !stone || !head) return null;
  const root = new THREE.Group(); root.add(modelGroup(base));
  for (let i = 0; i < 5; i++) { const g = modelGroup(stone); g.position.y = 45 + i * 30; root.add(g); }
  const top = modelGroup(head); top.position.y = 195; root.add(top); return root;
}
const PARTNER_FILES = {1: 'sonic', 2: 'tails', 3: 'knuckles', 4: 'amy', 5: 'rouge', 6: 'omega', 7: 'vector', 8: 'espio', 9: 'maria', 12: 'doomseye'};
function partnerModels(o) { const f = PARTNER_FILES[paramWord(o, 0) ?? 0]; if (!f) return []; return ctx.models.filter(m => m.archive.toLowerCase() === `character\\${f}.one` && !/(_L|_R)0?[2-9]\.DFF$|TE[2-9]\.DFF$/i.test(m.name)); }
const SHADOW = ['SHADOW_BODY.DFF', 'SHADOW_EYE_L.DFF', 'SHADOW_EYE_R.DFF', 'SHADOW_HAND_L.DFF', 'SHADOW_HAND_R.DFF'];
function modelsFor(o) {
  if (o.previewModel) { const m = ctx.models.find(m => m.id === o.previewModel); return m ? [m] : []; }
  if (['spawn', 'goalpose', 'playerstart'].includes(o.kind)) return SHADOW.map(find).filter(Boolean);
  if (o.kind !== 'native') return [];
  if (o.list === 0x01 && o.type === 0x90) return partnerModels(o);
  let models = catalogModels(o);
  if (!models.length) { const internal = o.nativeName || ctx.catalog.find(c => c.list === o.list && c.type === o.type)?.internalName; const m = find(nativeModelNames[internal] || '~'); if (m) models = [m]; }
  return models;
}
// Native volumes (no model in game): see-through shapes from their parameters (half-extents; trigger colour by type)
const VOLUMES = {'0-80': {shape: 1, size: [2, 3, 4], sphere: 0, box: 1, cyl: 2, cone: 3, colour: 0xb98cff}, '0-81': {shape: 0, size: [1, 2, 3], sphere: 0, box: 1, cyl: 2, cone: 3, colour: 0x7fd4ff},
  '0-98': {shape: 1, size: [6, 7, 8], box: 0, sphere: 1, cyl: 2, colour: 0xffe07a}, '37-152': {fixed: 'box', size: [3, 4, 5], colour: 0x88f0a8}, '37-149': {fixed: 'sphere', size: [1, 1, 1], colour: 0xffd24a},
  '0-89': {fixed: 'sphere', size: [2, 2, 2], colour: 0x6fa8ff}, '8-53': {fixed: 'sphere', size: [0, 0, 0], colour: 0xd8c39a}, '8-52': {fixed: 'cyl', size: [3, 4, 4], colour: 0xd8c39a, cylHeight: true},
  '0-26': {shape: 1, size: [2, 3, 4], cyl: 0, box: 1, colour: 0x9fe2ff}};
export const VOLUME_CLASSES = new Set(Object.keys(VOLUMES));
function volumeNode(o) {
  const v = VOLUMES[`${o.list}-${o.type}`]; if (!v) return null; const w = i => paramWord(o, i, true) ?? 0;
  let kind = v.fixed; if (!kind) { const sh = paramWord(o, v.shape) ?? 0; kind = sh === v.sphere ? 'sphere' : sh === v.box ? 'box' : sh === v.cyl ? 'cyl' : sh === v.cone ? 'cone' : 'box'; }
  const [x, y, z] = v.size.map(i => Math.abs(w(i)) || 20); let geo;
  if (kind === 'sphere') geo = new THREE.SphereGeometry(x, 20, 14);
  else if (kind === 'cyl' || kind === 'cone') { const h = v.cylHeight ? Math.abs(w(4)) || 40 : (y + z) || 40; geo = kind === 'cone' ? new THREE.ConeGeometry(x, h, 24, 1, true) : new THREE.CylinderGeometry(x, x, h, 24, 1, true); geo.translate(0, h / 2, 0); }
  else geo = new THREE.BoxGeometry(x * 2, y * 2, z * 2);
  const colour = o.list === 0 && o.type === 0x50 ? ({0: 0x9a9aa8, 2: 0xb98cff, 3: 0xff9a3c, 4: 0xff3040, 5: 0x5fb0ff, 6: 0x5fb0ff, 7: 0x5fb0ff, 11: 0xffd24a}[paramWord(o, 0)] ?? v.colour) : v.colour;
  const root = new THREE.Group(); root.add(new THREE.Mesh(geo, new THREE.MeshBasicMaterial({color: colour, transparent: true, opacity: .1, depthWrite: false, side: THREE.DoubleSide})));
  root.add(new THREE.LineSegments(new THREE.EdgesGeometry(geo, 20), new THREE.LineBasicMaterial({color: colour, transparent: true, opacity: .75}))); root.userData.volume = true; return root;
}
function ghostFigure(root, colour, shadow) {
  const ghost = new THREE.MeshBasicMaterial({color: colour, transparent: true, opacity: .55, depthWrite: false});
  if (shadow) { shadow.traverse(c => { if (c.isMesh) c.material = ghost; }); root.add(shadow); }
  const torso = new THREE.Mesh(new THREE.CapsuleGeometry(1.6, 3.6, 4, 10), ghost); torso.position.y = 4; root.add(torso);
  const head = new THREE.Mesh(new THREE.SphereGeometry(2.1, 14, 10), ghost); head.position.y = 8.6; root.add(head);
}
export function shadowGhost(colour) {   // #124 the apex silhouette of a launch arc: Shadow's model (or the stand-in) in one see-through colour
  const root = new THREE.Group(), ms = SHADOW.map(find).filter(Boolean); let shadow = null;
  if (ms.length) { shadow = new THREE.Group(); for (const m of ms) shadow.add(modelGroup(m)); }
  ghostFigure(root, colour, shadow); root.traverse(c => { c.raycast = () => {}; c.renderOrder = 15; }); return root;
}
function playerStartNode(o, shadow) {
  const root = new THREE.Group(), colour = o.player ? 0x6fa8ff : 0x73e2a8; ghostFigure(root, colour, shadow);
  const ring = new THREE.Mesh(new THREE.RingGeometry(5, 6, 32), new THREE.MeshBasicMaterial({color: colour, side: THREE.DoubleSide, transparent: true, opacity: .7, depthWrite: false})); ring.rotation.x = -Math.PI / 2; ring.position.y = .3; root.add(ring);
  root.add(new THREE.ArrowHelper(new THREE.Vector3(0, 0, 1), new THREE.Vector3(0, .5, 0), 16, colour, 5, 3)); return root;
}
const ROUTE_COLOURS = {N: 0x5ef0ff, H: 0x4f9dff, D: 0xff5060, X: 0xffc94a};
// The finish-pose camera (classic GOAL_CAMERAS): the game's clear animation moves the camera along fixed keys relative
// to the pose (normal routes 40 frames, expert 60); drawn as the eye path, a faint start camera, a solid end camera
// and a dashed sight line to where the end camera looks
function lerpKeys(keys, f) { if (f <= keys[0][0]) return keys[0][1]; for (let i = 1; i < keys.length; i++) if (f <= keys[i][0]) { const [f0, a] = keys[i - 1], [f1, b] = keys[i]; return a + (b - a) * (f - f0) / (f1 - f0); } return keys.at(-1)[1]; }
const GOAL_CAMERAS = {
  normal: {eye: f => [lerpKeys([[0, 24.23], [20, 21.33], [40, 13.48]], f), lerpKeys([[0, 5.07], [40, 3.69]], f), lerpKeys([[0, 8.28], [17, .17], [40, -15.17]], f)], target: () => [2.81, 5.41, 3.85], frames: 40},
  expert: {eye: f => [lerpKeys([[0, 11.35], [40, 8.98], [50, 6.43], [720, 6.05]], f), lerpKeys([[0, .9], [40, 2.91], [50, 5.19], [720, 5.34]], f), lerpKeys([[0, -15.24], [40, -4.79], [50, -6.46], [720, -5.84]], f)],
    target: f => [lerpKeys([[0, 2.81], [40, -1.16], [50, -1.21], [720, -1.1]], f), lerpKeys([[0, 5.41], [40, 7.73], [50, 7.57]], f), lerpKeys([[0, 3.85], [40, .94], [50, 1.32], [720, 1.43]], f)], frames: 60}};
function cameraIcon(colour, eye, target, opacity) {
  const g = new THREE.Group(), m = new THREE.MeshBasicMaterial({color: colour, wireframe: true, transparent: true, opacity});
  g.add(new THREE.Mesh(new THREE.BoxGeometry(2.4, 1.8, 3), m)); const lens = new THREE.Mesh(new THREE.ConeGeometry(1.2, 1.8, 4, 1, true), m); lens.rotation.x = Math.PI / 2; lens.position.z = -2.3; g.add(lens);
  g.position.fromArray(eye); g.lookAt(new THREE.Vector3(...target)); return g;
}
function goalPoseNode(o, shadow) {
  const root = new THREE.Group(), colour = ROUTE_COLOURS[o.route] ?? 0xcccccc, cam = GOAL_CAMERAS[o.route === 'X' ? 'expert' : 'normal']; ghostFigure(root, colour, shadow);
  const end = cam.eye(cam.frames), path = []; for (let f = 0; f <= cam.frames; f += 2) path.push(new THREE.Vector3(...cam.eye(f)));
  root.add(new THREE.Line(new THREE.BufferGeometry().setFromPoints(path), new THREE.LineBasicMaterial({color: colour, transparent: true, opacity: .8})));
  root.add(cameraIcon(colour, cam.eye(0), cam.target(0), .45), cameraIcon(colour, end, cam.target(cam.frames), .95));
  const sight = new THREE.Line(new THREE.BufferGeometry().setFromPoints([new THREE.Vector3(...end), new THREE.Vector3(...cam.target(cam.frames))]), new THREE.LineDashedMaterial({color: colour, dashSize: 1, gapSize: .7})); sight.computeLineDistances(); root.add(sight);
  return root;
}
// PATH.PTP paths (rails, platform paths, camera...): a polyline through the local control points (closed = back to the
// first) and the points themselves as one Points object (userData.pathPoints; a raycast hit's index = the point)
export const PATH_COLOURS = {rail: 0x6edbb0, platform: 0xffb347, camera: 0x9fb4ff, loop: 0xff7ab6, native: 0x9aa2b5};
export function pathNode(o) {
  const root = new THREE.Group(), colour = PATH_COLOURS[o.pathType] ?? PATH_COLOURS.native, pts = (o.points || []).map(p => new THREE.Vector3(...p));
  if (pts.length > 1) root.add(new THREE.Line(new THREE.BufferGeometry().setFromPoints(o.closed ? [...pts, pts[0]] : pts), new THREE.LineBasicMaterial({color: colour})));
  const dots = new THREE.Points(new THREE.BufferGeometry().setFromPoints(pts), new THREE.PointsMaterial({color: colour, size: 9, sizeAttenuation: true, depthTest: false, transparent: true, opacity: .9}));
  dots.userData.pathPoints = true; dots.renderOrder = 5; root.add(dots); root.userData.path = true; return root;
}
function scriptObjectNode(o) {
  const root = new THREE.Group(), gold = 0xffc94d;
  const crystal = new THREE.Mesh(new THREE.OctahedronGeometry(7), new THREE.MeshBasicMaterial({color: gold})); crystal.position.y = 14; crystal.scale.y = 1.5; root.add(crystal);
  const touch = new THREE.Mesh(new THREE.BoxGeometry(...o.size), new THREE.MeshBasicMaterial({color: gold, transparent: true, opacity: .08, depthWrite: false, side: THREE.DoubleSide}));
  touch.add(new THREE.LineSegments(new THREE.EdgesGeometry(touch.geometry), new THREE.LineBasicMaterial({color: gold, transparent: true, opacity: .5}))); root.add(touch); return root;
}
function triggerNode(o) {
  const g = new THREE.BoxGeometry(...(o.size || [100, 100, 100])), root = new THREE.Group();
  root.add(new THREE.Mesh(g, new THREE.MeshBasicMaterial({color: 0x9e83ef, transparent: true, opacity: .14, depthWrite: false, side: THREE.DoubleSide})));
  root.add(new THREE.LineSegments(new THREE.EdgesGeometry(g), new THREE.LineBasicMaterial({color: 0xb99aff}))); return root;
}
function destinationNode() {
  const root = new THREE.Group(), violet = 0xc28bff;
  root.add(new THREE.Mesh(new THREE.CylinderGeometry(12, 12, 1.5, 6), new THREE.MeshBasicMaterial({color: violet, transparent: true, opacity: .6})));
  const beam = new THREE.Mesh(new THREE.CylinderGeometry(2, 2, 30, 8, 1, true), new THREE.MeshBasicMaterial({color: violet, transparent: true, opacity: .35, depthWrite: false, side: THREE.DoubleSide})); beam.position.y = 15; root.add(beam);
  root.add(new THREE.ArrowHelper(new THREE.Vector3(0, 0, 1), new THREE.Vector3(0, 1, 0), 18, violet, 5, 3)); return root;
}
// ---- authored geometry (project.geometry, kind 'geometry'): classic createShape / meshData / rampData + makeNode ----
// Shape data is local (the item's position / rotation / scale place it). Box and cylinder are lifted so their base sits
// at y = 0 (classic centres them, so half of a new box sank into the floor); the ramp already starts at y = 0.
export const SHAPES = [['box', 'Box / platform', [100, 30, 100]], ['ramp', 'Ramp', [100, 30, 100]], ['cylinder', 'Cylinder', [100, 80, 100]], ['loop', 'Loop ribbon', [180, 100, 12]]];
export function meshData(g) {
  if (g.index) g = g.toNonIndexed(); const p = g.getAttribute('position'), uv = g.getAttribute('uv'), v = [], u = [], t = [];
  for (let i = 0; i < p.count; i++) { v.push([p.getX(i), p.getY(i), p.getZ(i)]); u.push(uv ? [uv.getX(i), uv.getY(i)] : [0, 0]); if (i % 3 === 0) t.push([i, i + 1, i + 2]); }
  g.dispose(); return {vertices: v, uvs: u, triangles: t};
}
function rampData(w, h, d) { const v = [[-w / 2, 0, -d / 2], [w / 2, 0, -d / 2], [-w / 2, 0, d / 2], [w / 2, 0, d / 2], [-w / 2, h, d / 2], [w / 2, h, d / 2]]; return {vertices: v, uvs: [[0, 0], [1, 0], [0, 1], [1, 1], [0, 1], [1, 1]], triangles: [[0, 4, 5], [0, 5, 1], [0, 1, 3], [0, 3, 2], [2, 3, 5], [2, 5, 4], [0, 2, 4], [1, 5, 3]]}; }
// loop ribbon: radius, width, thickness (classic loopData; inverted running / camera are not integrated in game yet)
function loopData(radius = 180, width = 100, thickness = 12, segments = 96) {
  const v = [], u = [], t = [];
  for (let i = 0; i <= segments; i++) {
    const a = i / segments * Math.PI * 2;
    for (const [x, r] of [[-width / 2, radius], [width / 2, radius], [-width / 2, radius + thickness], [width / 2, radius + thickness]]) { v.push([x, radius - r * Math.cos(a), r * Math.sin(a)]); u.push([x / width + .5, i / segments * 12]); }
    if (i < segments) { const k = i * 4; for (const [a, b, c, d] of [[0, 4, 5, 1], [2, 3, 7, 6], [0, 2, 6, 4], [1, 5, 7, 3]]) t.push([k + a, k + b, k + c], [k + a, k + c, k + d]); }
  }
  return {vertices: v, uvs: u, triangles: t};
}
export function shapeData(type, [w, h, d]) {
  if (type === 'ramp') return rampData(w, h, d);
  if (type === 'loop') return loopData(w, h, d);
  const g = type === 'cylinder' ? new THREE.CylinderGeometry(w / 2, w / 2, h, 24) : new THREE.BoxGeometry(w, h, d); g.translate(0, h / 2, 0); return meshData(g);
}
export function geometryNode(o) {
  const g = geometryFromData({...o, uvs: o.uvs?.map(v => v.map((x, i) => x * (o.uvRepeat?.[i] ?? 1))), colours: o.vertices.map(() => o.colour || [200, 200, 200, 255])});
  const mat = o.renderVisible === false ? new THREE.MeshBasicMaterial({color: 0x60d9e9, wireframe: true, transparent: true, opacity: .65, side: THREE.DoubleSide}) : material(ctx.materials.find(m => m.id === o.material));
  const root = new THREE.Group(), mesh = new THREE.Mesh(g, mat); root.add(mesh);
  if (o.surface === 32 && o.collision !== false) mesh.add(new THREE.Mesh(g, new THREE.MeshBasicMaterial({color: 0xff2a3a, transparent: true, opacity: .35, depthWrite: false, side: THREE.DoubleSide})));   // own kill surface: not solid, kills when falling down through it (proven in game, #38)
  if (o.collision === false) root.add(new THREE.LineSegments(new THREE.EdgesGeometry(g, 20), new THREE.LineBasicMaterial({color: 0x60d9e9, transparent: true, opacity: .5})));   // not solid: outlined
  root.userData.geometry = true; return root;
}

// ---- in-game capture (#35; served by as /api/models recordModels / classModels) ------------------
// recordModels['<layer>-<index>'] = what the game drew for that SET record: draws [{model, during, match_dist, ltm (RW
// row vectors right / up / at / pos, scale kept), rec_pos, rec_rot}]. A part is placed at inverse(record) * ltm, so it
// keeps its offset / angle / scale relative to the placement (record rotation applied Z, then X, then Y = three.js
// 'YXZ', like the editor's own). Draws are kept when tied to the record (drawn by its own object, matched within 10
// units, or the matrix sits on the record); one part per model and offset. classModels['LLTT'][first int param] = the
// model names for a class / variant (capture | artist | catalog) for new placements and records without draws.
const _rec = new THREE.Matrix4(), _q = new THREE.Quaternion(), _e = new THREE.Euler(), _v = new THREE.Vector3(), _one = new THREE.Vector3(1, 1, 1);
function ltmMatrix(l) { return new THREE.Matrix4().set(l[0], l[3], l[6], l[9], l[1], l[4], l[7], l[10], l[2], l[5], l[8], l[11], 0, 0, 0, 1); }
const recordKey = o => o.kind === 'native' && o.sourceIndex != null && !isForeign(o) ? `${o.layer}-${o.sourceIndex}` : null;
export function captureParts(o) {
  const rec = ctx.recordModels?.[recordKey(o)]; if (!rec || rec.source !== 'capture') return null;
  const parts = [], seen = new Set();
  for (const d of rec.draws || []) {
    const l = d.ltm; if (!Array.isArray(l) || l.length !== 12 || !l.every(Number.isFinite) || d.during === 'none') continue;
    const rp = d.rec_pos || [0, 0, 0], onRecord = Math.hypot(l[9] - rp[0], l[10] - rp[1], l[11] - rp[2]) < 1;
    if (!(d.during === 'node' || (d.match_dist ?? 0) <= 10 || onRecord)) continue;
    _e.set(...(d.rec_rot || [0, 0, 0]).map(THREE.MathUtils.degToRad), 'YXZ'); _rec.compose(_v.set(...rp), _q.setFromEuler(_e), _one);
    const rel = _rec.clone().invert().multiply(ltmMatrix(l)), pos = new THREE.Vector3().setFromMatrixPosition(rel);
    if (pos.length() > 3000) continue;   // a shared atomic drawn somewhere else
    const k = `${d.model}|${pos.toArray().map(Math.round).join(',')}`; if (seen.has(k)) continue; seen.add(k);
    parts.push({model: d.model, matrix: rel});
  }
  if (!parts.length) looseParts(rec, parts);
  return parts.length ? {parts, rec} : {parts: [], rec};
}
// #52: a record whose game draws all fail the tie test above (a moving or hidden piece: Lightspeed Firewall / Rising
// Block drawn 420 below until they rise, the Rising Laser Bar, Roll Circle arms) still gets the game's scale and angle,
// from the draws of the models this record itself looked up. Their offset is kept when the pieces sit at different
// offsets (the arms of a Roll Circle, +-400); a shift they all share is a state (hidden / lowered), so they go on the record.
const NO_LOOSE = typeof location !== 'undefined' && new URLSearchParams(location.search).has('noloose');   // debug: the editor before #52 (before / after frames)
function looseParts(rec, parts) {
  const own = new Set((rec.looked || []).map(n => String(n).toUpperCase())), got = [];
  if (!own.size || NO_LOOSE) return;
  for (const d of rec.draws || []) {
    const l = d.ltm; if (!own.has(String(d.model).toUpperCase()) || !Array.isArray(l) || l.length !== 12 || !l.every(Number.isFinite) || d.during === 'none') continue;
    _e.set(...(d.rec_rot || [0, 0, 0]).map(THREE.MathUtils.degToRad), 'YXZ'); _rec.compose(_v.set(...(d.rec_pos || [0, 0, 0])), _q.setFromEuler(_e), _one);
    const rel = _rec.clone().invert().multiply(ltmMatrix(l)), pos = new THREE.Vector3().setFromMatrixPosition(rel);
    if (pos.length() > 1500) continue;
    got.push({model: d.model, matrix: rel, key: pos.toArray().map(Math.round).join(',')});
  }
  const shared = new Set(got.map(g => g.key)).size === 1, seen = new Set();
  for (const g of got) {
    if (shared) g.matrix.setPosition(0, 0, 0);
    const k = `${g.model}|${shared ? '0,0,0' : g.key}`; if (seen.has(k)) continue; seen.add(k);
    parts.push({model: g.model, matrix: g.matrix, loose: true});
  }
}
function captureNames(o) {
  const rec = ctx.recordModels?.[recordKey(o)]; if (rec?.models?.length && rec.source === 'capture') return {names: rec.models, source: 'capture'};
  const byClass = ctx.classModels?.[((o.list << 8) | o.type).toString(16).toUpperCase().padStart(4, '0')]; if (!byClass) return null;
  const v = byClass[String(paramWord(o, 0) ?? 0)]; return v?.models?.length ? {names: v.models, source: v.source} : null;
}

// One 3D node per placement, in its local frame (the caller applies position / rotation). null = no model: the caller
// draws a category marker instead. root.userData.modelSource = capture | artist | catalog (what the models come from).
// #70: a model must not vanish because of bad data. A node whose drawn box is empty, not finite, under 1 unit or more
// than 5000 units from its record (a degenerate captured matrix, a zero / NaN scale) is not used: the capture falls
// back to the catalog model, and a degenerate catalog model to a marker; ctx.drawNotes[list-type] keeps the reason
// (Add-panel tooltip, inspector)
const _box = new THREE.Box3(), _dv = new THREE.Vector3();
export function degenerate(root) {
  root.updateMatrixWorld(true); _box.setFromObject(root);
  if (_box.isEmpty()) return 'the model has no drawable geometry';
  const size = _box.getSize(_dv).length(); if (!Number.isFinite(size)) return 'the model has a broken (not finite) size';
  if (size < 1) return `the model is drawn ${size.toFixed(2)} units big (a zero scale in its data)`;
  if (_box.getCenter(_dv).length() > 5000) return `the model is drawn ${Math.round(_dv.length())} units away from where it is placed`;
  return '';
}
const noteDraw = (o, why) => { if (o.kind === 'native') ctx.drawNotes[`${o.list}-${o.type}`] = why; };
// #78 Ring (0/0x10) rows: params [RingType 0 single | 1 line | 2 circle | 3 arch][NumberOfRings][LengthRadius f][Angle f]
// [Ghost] (catalog Object0010_Ring). Line = proven from the 202 capture (cmn-194 / 22 / 106): ring i at local
// (0, 0, -i * length / count), the first at the record's position. Circle (radius = length, around the position) and arch
// (the line's chord bent by Angle degrees, up for positive) are drawn from the field names only: not proven.
export function ringOffsets(o) {
  if (o.kind !== 'native' || o.list !== 0 || o.type !== 0x10) return null;
  const kind = paramWord(o, 0), n = Math.min(64, Math.max(1, paramWord(o, 1) | 0)), L = paramWord(o, 2, true) || 0, ang = paramWord(o, 3, true) || 0;
  if (!(kind >= 1 && kind <= 3) || n < 2 || !Number.isFinite(L)) return null;
  const out = [];
  for (let i = 0; i < n; i++) {
    if (kind === 1) out.push([0, 0, -i * L / n]);
    else if (kind === 2) { const a = 2 * Math.PI * i / n; out.push([Math.sin(a) * L, 0, -Math.cos(a) * L]); }
    else { const z = i * L / n, a = THREE.MathUtils.degToRad(Math.abs(ang)); let y = 0;
      if (a > 1e-4) { const R = L / (2 * Math.sin(a / 2)), x = z - L / 2; y = Math.sqrt(Math.max(0, R * R - x * x)) - R * Math.cos(a / 2); y *= Math.sign(ang); }
      out.push([0, y, -z]); }
  }
  return {kind, offsets: out, proven: kind === 1};
}
export function objectNode(o) {
  const n = objectNode0(o), row = n && ringOffsets(o); if (!row) return n;
  const proto = n.clone();   // shares geometry / materials: cheap
  for (const off of row.offsets.slice(1)) { const c = proto.clone(); c.position.set(...off); n.add(c); }
  n.userData.ringRow = row; return n;
}
function objectNode0(o) {
  extra = isForeign(o) ? ctx.foreign.get(o.sourceStage) || null : null;
  if (o.kind === 'geometry') return geometryNode(o);
  if (o.kind === 'path') return pathNode(o);
  if (o.kind === 'trigger') return o.script ? scriptObjectNode(o) : triggerNode(o);
  if (o.kind === 'destination') return destinationNode(o);
  if (o.kind === 'native') {
    const vol = volumeNode(o); if (vol) return vol;
    if (!o.previewModel) {   // what the game drew for this record, at the recorded offsets
      const cap = captureParts(o), pillar = o.list === 0x08 && o.type === 0x37;
      const missing = cap ? (cap.rec.models || []).filter(name => !cap.parts.some(p => p.model === name)) : [];
      // a capture whose draws miss some of the record's models (Collapsing Pillar: only the base was drawn with a matrix)
      // keeps the pillar's stack approximation; other classes add the missing models at the record's origin
      if (cap?.parts.length && !(pillar && missing.length)) {
        const root = new THREE.Group(); let n = 0;
        let flat = 0;   // #70: a part whose recorded matrix has a zero / broken scale would be invisible: skipped and noted
        for (const p of cap.parts) { const m = find(p.model); if (!m) continue; const det = p.matrix.determinant(); if (!Number.isFinite(det) || Math.abs(det) < 1e-6) { flat++; continue; } const g = modelGroup(m); g.matrixAutoUpdate = false; g.matrix.copy(p.matrix); root.add(g); n++; }
        if (flat) noteDraw(o, `capture: ${flat} part${flat > 1 ? 's' : ''} recorded with a zero scale ${n ? 'left out' : '- the catalog model is shown'}`);
        for (const name of missing) { const m = find(name); if (m) { root.add(modelGroup(m)); n++; } }
        const bad = n ? degenerate(root) : ''; if (bad) noteDraw(o, 'capture: ' + bad + ' - the catalog model is shown');
        if (n && !bad) { root.userData.modelSource = 'capture'; root.userData.captureHow = cap.rec.how + (cap.parts.some(p => p.loose) ? ' (scale / angle from a moved piece)' : '') + (missing.length ? ` (${missing.length} model${missing.length > 1 ? 's' : ''} without a recorded matrix)` : ''); return root; }
      }
      const named = pillar ? null : captureNames(o);
      if (named?.source === 'capture') {
        const ms = named.names.map(find).filter(Boolean);
        if (ms.length) { const root = new THREE.Group(), inner = new THREE.Group(); for (const m of ms) inner.add(modelGroup(m)); if (o.list === 0x25 && (o.type === 0x88 || o.type === 0x8A)) { const sc = [1, 2, 3].map(i => paramWord(o, i, true)); if (sc.every(v => Number.isFinite(v) && v > 0)) inner.scale.set(...sc.map(v => Math.min(100, Math.max(.01, v)))); } const bad = degenerate(inner); if (bad) noteDraw(o, 'capture: ' + bad); else { root.add(inner); root.userData.modelSource = 'capture'; root.userData.captureHow = 'names'; return root; } }
      }
    }
    if (o.list === 0x08 && o.type === 0x37 && !o.previewModel) { const p = pillarNode(o); if (p) { p.userData.modelSource = 'catalog'; return p; } }
  }
  const models = modelsFor(o);
  if (!models.length) return o.kind === 'goalpose' ? goalPoseNode(o, null) : o.kind === 'playerstart' ? playerStartNode(o, null) : null;
  const root = new THREE.Group(), inner = new THREE.Group(); for (const m of models) inner.add(modelGroup(m));
  const bad = o.kind === 'native' ? degenerate(inner) : ''; if (bad) { noteDraw(o, bad + ' - shown as a marker'); return null; }
  if (o.kind === 'native' && o.list === 0x25 && (o.type === 0x88 || o.type === 0x8A)) { const sc = [1, 2, 3].map(i => paramWord(o, i, true)); if (sc.every(v => Number.isFinite(v) && v > 0)) inner.scale.set(...sc.map(v => Math.min(100, Math.max(.01, v)))); }
  root.add(inner); if (o.kind === 'native') root.userData.modelSource = o.list === 0x25 && (o.type === 0x88 || o.type === 0x89) ? 'artist' : 'catalog';
  return o.kind === 'goalpose' ? goalPoseNode(o, root) : o.kind === 'playerstart' ? playerStartNode(o, root) : root;
}
