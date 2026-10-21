// Stage Workshop UI 2 (test build) - see docs/stage-workshop/STAGE-WORKSHOP-UI2.md. Same server / API / projects as the classic editor.
import * as THREE from 'three';
import {OrbitControls} from 'three/addons/controls/OrbitControls.js';
import {TransformControls} from 'three/addons/controls/TransformControls.js';
import {createLogic} from './logic.js';
import {animateUV} from './render3d.js';
import {ctx as R, objectNode, ringOffsets, modelGroup, referenceMesh, material, VOLUME_CLASSES, SHAPES, shapeData, meshData, isForeign, shadowGhost} from './render3d.js';
import {LAUNCHERS, LAUNCHER_NAMES, trace as arcTrace, solveLanding, solveApex} from './springs.js';
let APP_NAME = 'Stage Forge';   // the editor's name: tools/stage_editor/branding.json (one place; the launcher reads it too)
fetch('../branding.json').then(r => r.ok ? r.json() : null).then(b => { if (b?.name) { APP_NAME = String(b.name); const el = document.getElementById('appName'); if (el) el.textContent = APP_NAME; document.title = document.title.replace(/Stage Forge/g, APP_NAME); } }).catch(() => {});

const $ = id => document.getElementById(id);
// script errors since load (uncaught, rejected promises, console.error): the self-test (selftest.js) reads them
const uiErrors = [];
addEventListener('error', e => uiErrors.push({t: Date.now(), msg: `${e.message} @${String(e.filename || '').split('/').pop()}:${e.lineno}`}));
addEventListener('unhandledrejection', e => uiErrors.push({t: Date.now(), msg: 'promise: ' + (e.reason?.message || e.reason)}));
{ const ce = console.error.bind(console); console.error = (...a) => { uiErrors.push({t: Date.now(), msg: a.map(x => x?.message || String(x)).join(' ').slice(0, 300)}); ce(...a); }; }
const esc = s => String(s ?? '').replace(/[&<>"]/g, c => ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;'}[c]));
let config = null, project = null, items = [], byId = new Map(), selected = null, dirty = false;
const catalog = new Map();

// ---- one colour language: category -> colour, used by outliner, markers, minimap and flow strip -----------------
const CATS = [
  ['key', 'Key items', '#ffd34d'], ['pickups', 'Pickups', '#4cc9f0'], ['enemies', 'Enemies', '#ff5470'],
  ['objects', 'Objects', '#a78bfa'], ['decor', 'Decor & breakables', '#6ee7a0'], ['volumes', 'Volumes & triggers', '#7c8aa5'], ['shapes', 'Shapes & geometry', '#60d9e9'], ['paths', 'Paths & rails', '#fb923c'], ['lighting', 'Lighting', '#fde047'], ['stagecode', 'Stage code', '#94a3b8'], ['prefabs', 'Prefabs', '#f472b6']];
const CAT = Object.fromEntries(CATS.map(([id, label, colour]) => [id, {id, label, colour}]));
const LAYERS = [['cmn', 'Common', 'always loaded'], ['nrm', 'Normal', 'SET layer nrm'], ['hrd', 'Hard', 'SET layer hrd'], ['ds1', 'DS1', 'SET layer ds1']];
const hiddenCats = new Set(['volumes']), hiddenLayers = new Set(), hiddenZones = new Set();
let groupBy = 'category', query = '', panel = 'outliner', openGroups = new Set(['category:key']);
let showArt = true, showKill = true, showColl = false, faceMode = false, skyMode = (() => { try { return ['game', 'always', 'hidden'].includes(localStorage.getItem('ui2.backdrop')) ? localStorage.getItem('ui2.backdrop') : 'hidden'; } catch { return 'hidden'; } })(), models = null;   // backdrop art OFF by default (user: the view sat inside Final Haunt's shells; a new key, so the old 'all' resets once), remembered per browser

// ---- server ------------------------------------------------------------------------------------------------------
async function api(path, body) {
  const r = await fetch(path, body ? {method: 'POST', headers: {'Content-Type': 'application/json', 'X-Workshop-Token': config.token}, body: JSON.stringify(body)} : {});
  const data = await r.json(); if (!r.ok) throw Error(data.error || r.statusText); return data;
}
const pageId = (crypto.randomUUID ? crypto.randomUUID() : String(Math.random()).slice(2) + Date.now()).replace(/-/g, '').slice(0, 32);
const life = k => `/api/${k}?t=${encodeURIComponent(config?.token || '')}&page=${pageId}`;
// #48 window lifecycle log: the last 30 events (pagehide + persisted, visibility, beforeunload, freeze / resume, WebGL
// context lost / restored, uncaught errors) in a ring kept in localStorage (survives the window dying) and sent to the
// server: the heartbeat carries the last event, the bye beacon the last 12, the first heartbeat the previous page's tail
const LIFE_KEY = 'ui2.lifelog', lifeLog = [];
const lifePrev = (() => { try { return JSON.parse(localStorage.getItem(LIFE_KEY) || '[]'); } catch { return []; } })();
function lifeEvent(type, detail = '') {
  const d = new Date(); lifeLog.push(`${d.toTimeString().slice(0, 8)}.${String(d.getMilliseconds()).padStart(3, '0')} ${type}${detail ? ' ' + String(detail).slice(0, 160) : ''}`); if (lifeLog.length > 30) lifeLog.shift();
  try { localStorage.setItem(LIFE_KEY, JSON.stringify({page: pageId, project: project?.id || null, events: lifeLog})); } catch {}
}
const lifeTail = (list, n) => encodeURIComponent(list.slice(-n).join('\n')).slice(0, 1800);
window.ui2Life = () => ({now: lifeLog, previous: lifePrev});   // console: what happened before the last window closed
lifeEvent('load', `discarded=${!!document.wasDiscarded} prev=${lifePrev.events?.at?.(-1) || '-'}`);
addEventListener('pagehide', e => lifeEvent('pagehide', `persisted=${e.persisted}`), {capture: true});
addEventListener('pageshow', e => e.persisted && lifeEvent('pageshow', 'persisted=true'));
document.addEventListener('visibilitychange', () => lifeEvent('visibility', document.visibilityState));
document.addEventListener('freeze', () => lifeEvent('freeze')); document.addEventListener('resume', () => lifeEvent('resume'));
addEventListener('beforeunload', () => lifeEvent('beforeunload', `dirty=${dirty}`), {capture: true});
addEventListener('error', e => lifeEvent('error', `${e.message} @${(e.filename || '').split('/').pop()}:${e.lineno}`));
addEventListener('unhandledrejection', e => lifeEvent('rejection', e.reason?.message || e.reason));
let lifeFirst = true;
function alive() {
  if (!config?.token) return; let q = '&last=' + lifeTail(lifeLog, 1);
  if (lifeFirst && lifePrev.events?.length) q += '&prev=' + lifeTail(lifePrev.events, 8); lifeFirst = false;
  fetch(life('alive') + q).catch(() => {});
}
setInterval(alive, 4000);
addEventListener('pagehide', () => config?.token && fetch(life('bye') + '&ev=' + lifeTail(lifeLog, 12), {keepalive: true}).catch(() => {}));
function toast(text, err = false) { const t = $('toast'); t.textContent = text; t.className = 'toast' + (err ? ' err' : ''); t.hidden = false; clearTimeout(toast.t); toast.t = setTimeout(() => t.hidden = true, 4200); }

// ---- facets -------------------------------------------------------------------------------------------------------
function paramWord(o, i, float = false) { const b = Uint8Array.from(atob(o.params || ''), c => c.charCodeAt(0)); if (!(i >= 0) || b.length < i * 4 + 4) return null; const d = new DataView(b.buffer); return float ? d.getFloat32(i * 4, true) : d.getInt32(i * 4, true); }
function setParamWord(o, i, v, float = false) { const b = Uint8Array.from(atob(o.params || ''), c => c.charCodeAt(0)); if (b.length < i * 4 + 4) return; const d = new DataView(b.buffer); float ? d.setFloat32(i * 4, v, true) : d.setInt32(i * 4, v, true); o.params = btoa(String.fromCharCode(...b)); }
function category(o) {
  if (o.kind === 'trigger') return 'volumes';
  if (LIGHT_KINDS[o.kind]) return 'lighting';
  if (o.kind === 'stageobject') return 'stagecode';
  if (o.kind === 'geometry') return 'shapes';
  if (o.kind === 'path') return 'paths';
  if (o.kind !== 'native') return 'key';
  const {list, type} = o, n = (classOf(o) || '').toLowerCase();
  if ((list === 0 && (type === 5 || type === 20)) || (list === 0x25 && type === 0x95)) return 'key';
  if (VOLUME_CLASSES.has(`${list}-${type}`) || /trigger|wind|light colli|set se|collision|generator|sound/.test(n)) return 'volumes';
  if (list === 0x25 && (type === 0x88 || type === 0x89 || type === 0x8A)) return 'decor';
  if ((list === 0 && [12, 16, 17, 18, 19, 32, 51, 58].includes(type)) || /\bring\b|capsule|weapon|energy core|\bkey\b|hint/.test(n)) return 'pickups';
  if ((list === 0 && type >= 100 && type < 200) || /soldier|beetle|worm|larva|\bwing|monster|guardian|hawk|robot|bomb|tank|turret|enemy/.test(n)) return 'enemies';
  return 'objects';
}
function classOf(o) {
  if (o.kind === 'native') return catalog.get(`${o.list}-${o.type}`)?.name || `Object ${o.list.toString(16).padStart(2, '0')}${o.type.toString(16).padStart(2, '0')}`;
  if (o.kind === 'path') return {rail: 'Rail path', platform: 'Platform path', camera: 'Camera path', loop: 'Loop path'}[o.pathType] || 'Path';
  if (o.kind === 'geometry') return SHAPES.find(s => s[0] === o.shape)?.[1] || (o.shape === 'mesh' ? 'Mesh' : `${o.shape || 'Custom'} geometry`);
  return {playerstart: 'Player start', goalpose: 'Finish pose', spawn: 'Start override', teleport: 'Teleport pad', destination: 'Teleport destination', trigger: 'Trigger volume', lightregion: 'Lighting region', light: 'Point light', stageobject: 'Stage code object'}[o.kind] || o.kind;
}
let zones = [], spine = [], spineLen = [];
function computeZones() {
  zones = (project.native?.chunks || []).filter(c => c.bounds && !c.backdrop).map(c => ({number: c.number, min: c.bounds[0], max: c.bounds[1],
    vol: (c.bounds[1][0] - c.bounds[0][0]) * (c.bounds[1][1] - c.bounds[0][1]) * (c.bounds[1][2] - c.bounds[0][2])})).sort((a, b) => a.vol - b.vol);
}
function zoneOf(p) { for (const z of zones) if (p[0] >= z.min[0] && p[0] <= z.max[0] && p[1] >= z.min[1] && p[1] <= z.max[1] && p[2] >= z.min[2] && p[2] <= z.max[2]) return z.number; return null; }
// spine = player start -> checkpoints by their Number -> goal ring; progress = arc-length position of the closest point
function computeSpine() {
  const nat = project.objects.filter(o => o.kind === 'native');
  const start = project.objects.find(o => o.kind === 'playerstart' && !o.player) || project.objects.find(o => o.kind === 'spawn');
  const cps = nat.filter(o => o.list === 0 && o.type === 5).sort((a, b) => (paramWord(a, 0) ?? 0) - (paramWord(b, 0) ?? 0));
  const goal = nat.find(o => o.list === 0 && o.type === 20);
  spine = [start?.position || [0, 0, 0], ...cps.map(o => o.position), ...(goal ? [goal.position] : [])].map(p => new THREE.Vector3(...p));
  spine.cps = cps; spine.goal = goal; spineLen = [0];
  for (let i = 1; i < spine.length; i++) spineLen.push(spineLen[i - 1] + spine[i].distanceTo(spine[i - 1]));
}
const _a = new THREE.Vector3(), _b = new THREE.Vector3(), _p = new THREE.Vector3();
function progressOf(pos) {
  if (spine.length < 2) return 0;
  _p.set(...pos); let best = Infinity, at = 0;
  for (let i = 1; i < spine.length; i++) {
    _a.copy(spine[i - 1]); _b.copy(spine[i]).sub(_a); const L = _b.length() || 1;
    const t = THREE.MathUtils.clamp(_p.clone().sub(_a).dot(_b) / (L * L), 0, 1);
    const d = _a.clone().addScaledVector(_b, t).distanceToSquared(_p);
    if (d < best) { best = d; at = spineLen[i - 1] + t * L; }
  }
  return at / (spineLen.at(-1) || 1);
}
// where an item is: its position, except paths (imported game paths sit at 0,0,0 with world points): their centre
function posOf(o) {
  if (o.kind !== 'path' || !o.points?.length) return o.position;
  const c = o.points.reduce((a, q) => a.map((x, i) => x + q[i] / o.points.length), [0, 0, 0]), v = new THREE.Vector3(...c).multiply(new THREE.Vector3(...(o.scale || [1, 1, 1])));
  return v.applyEuler(new THREE.Euler(...(o.rotation || [0, 0, 0]).map(THREE.MathUtils.degToRad), 'YXZ')).add(new THREE.Vector3(...o.position)).toArray();
}
function makeItem(o) { const at = posOf(o); return {o, id: o.id, name: o.name || classOf(o), cls: classOf(o), cat: category(o), layer: o.kind === 'native' ? o.layer : null, at, zone: zoneOf(at), progress: progressOf(at)}; }
function relocate(it) { it.at = posOf(it.o); it.zone = zoneOf(it.at); it.progress = progressOf(it.at); }
const placed = () => [...project.objects, ...(project.triggers || []), ...(project.geometry || []), ...(project.paths || []), ...(project.lighting?.regions || []), ...(project.lighting?.lights || []), ...stageObjs()];
// ---- #68 / #69 the player start and finish poses: the game's own PLAYER / GOALEVENTPOS lines (stage parameters)
// as editor objects (the same shape as server.py player_start_objects / goal_pose_objects). Finish poses cannot be
// deleted; the player start can (user request, #68 delete / place): Delete / Remove start (undo brings it back), then
// Add > Player start, right-click > Place start here or the Move buttons place one anywhere (camera view = the surface
// under the screen centre + the camera heading). Restore puts the originals back. project.playerStart records it for
// the build: {removedOriginal (the game's PLAYER 0 line was removed in this project), present, kind, id, position,
// rotation} (reads removedOriginal: load_project no longer re-adds the original, spec 7d); with no start,
// Issues shows an error and Build / Playtest ask first (the server refuses a build without one).
const GOAL_NAMES = {N: 'Normal', H: 'Hero', D: 'Dark', X: 'Expert'};
function startObjectsFrom(sp) {
  const out = [];
  for (const line of sp?.lines || []) {
    let m = line.match(/^\s*PLAYER\s*:\s*(\d+)((?:\s+-?\d+(?:\.\d+)?){6})\s*$/);
    if (m) { const v = m[2].trim().split(/\s+/).map(Number), n = +m[1]; out.push({id: `playerstart-${n}`, kind: 'playerstart', player: n, name: n ? `Player ${n + 1} start (game)` : 'Player start (game)', position: v.slice(0, 3), rotation: v.slice(3, 6), scale: [1, 1, 1]}); continue; }
    m = line.match(/^\s*GOALEVENTPOS_([A-Z])\s*:\s*(-?\d+(?:\.\d+)?)\s+(-?\d+(?:\.\d+)?)\s+(-?\d+(?:\.\d+)?)\s*$/);
    if (m) out.push({id: `goalpose-${m[1]}`, kind: 'goalpose', route: m[1], name: `Finish pose (${GOAL_NAMES[m[1]] || m[1]})`, position: [+m[2], +m[3], +m[4]], rotation: [0, 0, 0], scale: [1, 1, 1]});
  }
  return out;
}
async function originalParams() {
  if (!project.stageParamsOriginal) { try { const sp = await api('/api/stage-params?id=' + project.stage); if (sp?.lines) project.stageParamsOriginal = structuredClone(sp); } catch {} }
  return project.stageParamsOriginal || project.stageParams;
}
const hasStart = () => project.objects.some(o => (o.kind === 'playerstart' && !o.player) || o.kind === 'spawn');
const isStart = o => (o.kind === 'playerstart' && !o.player) || o.kind === 'spawn';
function syncStartRecord() {   // project.playerStart: what the build needs to know about the start (#68)
  if (!project?.objects) return; const s = project.objects.find(o => o.kind === 'playerstart' && !o.player) || project.objects.find(o => o.kind === 'spawn');
  project.playerStart = {...project.playerStart, removedOriginal: !!project.playerStart?.removedOriginal, present: !!s, kind: s?.kind || null, id: s?.id || null, position: s ? [...s.position] : null, rotation: s ? [...(s.rotation || [0, 0, 0])] : null};
}
function removeStart() {
  const it = byId.get(project.objects.find(isStart)?.id); if (!it) return toast('This project has no player start', true);
  select(it, false); deleteSelected();
}
const syncPlaced = keepId => { restore(editable()); computeSpine(); drawFlow(); renderPanel(); renderIssueBadge(); if (keepId && byId.get(keepId)) select(byId.get(keepId)); };
async function restoreStart() {
  const orig = startObjectsFrom(await originalParams()); if (!orig.length) return toast(project.mode === 'new' ? 'New stages use the Start override (Add > Player start)' : 'The stage parameters have no PLAYER line to restore', true);
  snapshot(); let added = 0, reset = 0;
  for (const g of orig) {
    const cur = project.objects.find(o => o.kind === g.kind && (g.kind === 'playerstart' ? (o.player || 0) === g.player : o.route === g.route));
    if (cur) { if (JSON.stringify([cur.position, cur.rotation]) !== JSON.stringify([g.position, g.rotation])) { cur.position = [...g.position]; cur.rotation = [...g.rotation]; reset++; } }
    else { project.objects.push(g); added++; }
    if (g.kind === 'playerstart' && !g.player) { if (project.playerStart) project.playerStart.removedOriginal = false; if (cur && cur.name !== g.name) { cur.name = g.name; reset++; } }   // the original is back
  }
  changed(); syncPlaced('playerstart-0'); toast(added || reset ? `Original start and finish poses: ${added} put back, ${reset} moved back (undo with Ctrl Z)` : 'The start and finish poses are already where the game has them');
}
// move a start / finish pose (or the Start override) to a point; heading from the camera when asked
function moveMarker(id, point, faceCamera = false) {
  let o = project.objects.find(x => x.id === id);
  snapshot();
  if (!o) {   // Add > Player start when there is none: the game's player 0 (imported stages) or a Start override (new stages)
    o = project.mode === 'new' ? {id: crypto.randomUUID(), kind: 'spawn', name: 'Player start', position: [0, 0, 0], rotation: [0, 0, 0], scale: [1, 1, 1]} : {id: 'playerstart-0', kind: 'playerstart', player: 0, name: project.playerStart?.removedOriginal ? 'Player start (placed)' : 'Player start (game)', position: [0, 0, 0], rotation: [0, 0, 0], scale: [1, 1, 1]};
    project.objects.push(o); faceCamera = true;   // a new start faces where the camera looks
  }
  o.position = point.toArray().map(round2);
  if (faceCamera && o.kind !== 'goalpose') { const d = orbit.target.clone().sub(camera.position); o.rotation = [o.rotation?.[0] || 0, round2(THREE.MathUtils.radToDeg(Math.atan2(d.x, d.z))), o.rotation?.[2] || 0]; }
  changed(); syncPlaced(o.id); toast(`${o.name} moved to ${o.position.map(Math.round).join(', ')}${faceCamera && o.kind !== 'goalpose' ? `, facing ${Math.round(o.rotation[1])}°` : ''}`);
}
const startId = () => project.objects.find(o => o.kind === 'playerstart' && !o.player)?.id || project.objects.find(o => o.kind === 'spawn')?.id || (project.mode === 'new' ? null : 'playerstart-0');
function placeAtCursor(id, label) { placing = {name: label, moveTo: id}; renderer.domElement.style.cursor = 'crosshair'; toast(`Click the stage where ${label} goes (Esc cancels)`); }
function moveButtons(id, label) { return `<div class="wsBtns"><button class="ghost small" data-mv="cam" data-mid="${esc(id || '')}" title="The surface under the centre of the 3D view${label === 'the finish pose' ? '' : '; facing where the camera looks'}">Set to camera view</button> <button class="ghost small" data-mv="cursor" data-mid="${esc(id || '')}" data-ml="${esc(label)}">Place at cursor</button>${label === 'the player start' && id && isStart(project.objects.find(x => x.id === id) || {}) ? ` <button class="ghost small" data-mv="del" title="Delete the player start (Ctrl Z brings it back); place one again with Add > Player start or right-click > Place start here">Remove start</button>` : ''}</div>`; }
function wireMoveButtons(root) {
  root.querySelectorAll('[data-mv]').forEach(b => b.onclick = () => { if (b.dataset.mv === 'del') return removeStart(); const id = b.dataset.mid || startId(); if (b.dataset.mv === 'cam') moveMarker(id, centrePoint(), true); else placeAtCursor(id, b.dataset.ml); });
}
// Build / Playtest without a start: ask first (saving is never blocked)
async function startGuard(what) {
  if (hasStart() || (what === 'Playtest' && project.playtestStart?.enabled !== false && project.playtestStart?.position)) return true;
  // the server refuses a build without one (spec 7d (c)): place one or put the original back
  const fresh = project.mode === 'new', r = await choose('No player start', `The game needs a start and this ${fresh ? 'new stage' : 'project'} has none${project.playerStart?.removedOriginal ? ' (you removed it)' : ''}: the build is refused without one. Place one (Add > Player start, right-click > Place start here)${fresh ? '' : ' or put the original back'}.`,
    [['cam', `Place start at camera, then ${what.toLowerCase()}`], ...(fresh ? [] : [['orig', `Restore the original start, then ${what.toLowerCase()}`]]), ['cancel', 'Cancel']]);
  if (r === 'cam') { moveMarker(startId(), centrePoint(), true); return true; }
  if (r === 'orig') { await restoreStart(); return hasStart(); }
  return false;
}

// ---- #74 stage-code objects (: /api/stage stageObjects, read-only, decoded from the data, not proven): e.g. the
// Black Tank of Lethal Highway, which the stage's own code drives along PATH.PTP splines (routes) by its Command
// Collision boxes. Drawn as a ghost (its model, see-through) at the start, facing along its first route; the routes get
// a highlight line and every command box a label (category Stage code: its eye hides all of it). Never editable: no
// gizmo, no delete / copy; the command boxes themselves stay ordinary (editable) SET objects.
const stageObjs = () => (project?.stageObjects || []).filter(o => o.kind === 'stageobject' && Array.isArray(o.start) && o.start.length === 3).map(o => { if (!o.position) { o.position = [...o.start]; o.rotation = [0, 0, 0]; } return o; });
const readOnlyKind = o => o?.kind === 'stageobject';
var stageCode = new THREE.Group();   // added to the scene on the first build (the scene is made further down)
function stageObjectNode(o) {
  const g = new THREE.Group(), m = (models?.models || []).find(x => x.id === o.modelId) || (models?.models || []).find(x => x.name === o.model);
  if (m) { const mg = modelGroup(m); mg.traverse(c => { if (c.isMesh) c.material = [c.material].flat().map(x => { const k = x.clone(); k.transparent = true; k.opacity = .55; k.depthWrite = false; return k; }); }); g.add(mg); g.userData.ghostModel = m.name; }
  else { const k = new THREE.Mesh(new THREE.OctahedronGeometry(40, 0), new THREE.MeshBasicMaterial({color: 0x94a3b8, wireframe: true})); g.add(k); }
  const ring = new THREE.Mesh(new THREE.TorusGeometry(60, 3, 8, 40), new THREE.MeshBasicMaterial({color: 0x94a3b8, transparent: true, opacity: .8})); ring.rotation.x = Math.PI / 2; g.add(ring);
  const route = routePoints(o, o.startRoute ?? 0); if (route.length > 1) { const d = route[1].clone().sub(route[0]); g.rotation.y = Math.atan2(d.x, d.z); }
  return g;
}
function routePoints(o, k) {   // world points of route k (its PATH.PTP spline in project.paths)
  const sid = o.routes?.[k]?.splineId ?? o.pathIds?.[k]; const path = (project.paths || []).find(x => x.splineId === sid); if (!path) return [];
  const e = new THREE.Euler(...(path.rotation || [0, 0, 0]).map(THREE.MathUtils.degToRad), 'YXZ'), sc = new THREE.Vector3(...(path.scale || [1, 1, 1])), at = new THREE.Vector3(...path.position);
  return (path.points || []).map(q => new THREE.Vector3(...q).multiply(sc).applyEuler(e).add(at));
}
function labelSprite(text, colour = '#e2e8f0') {
  const c = document.createElement('canvas'), x = c.getContext('2d'); x.font = '600 26px Segoe UI'; const w = Math.ceil(x.measureText(text).width) + 20; c.width = w; c.height = 40;
  x.font = '600 26px Segoe UI'; x.fillStyle = '#0f1116cc'; x.fillRect(0, 0, w, 40); x.fillStyle = colour; x.fillText(text, 10, 29);
  const sp = new THREE.Sprite(new THREE.SpriteMaterial({map: new THREE.CanvasTexture(c), depthTest: false, transparent: true, sizeAttenuation: false})); sp.scale.set(w / 40 * .028, .028, 1); sp.renderOrder = 20; sp.raycast = () => {}; return sp;   // the same size on screen at any distance
}
const CMD_COLOURS = {move: 0x60a5fa, stop: 0xf87171, accelerate: 0x34d399, setpos: 0xfbbf24};
function buildStageCode() {
  if (!stageCode) return; if (!stageCode.parent) scene.add(stageCode); clear(stageCode); if (!project) return;
  for (const o of stageObjs()) {
    (o.routes || []).forEach((r, k) => { const pts = routePoints(o, k); if (pts.length < 2) return; const line = new THREE.Line(new THREE.BufferGeometry().setFromPoints(pts), new THREE.LineBasicMaterial({color: k === (o.startRoute ?? 0) ? 0xfbbf24 : 0xf59e0b, transparent: true, opacity: .9, depthTest: false})); line.renderOrder = 15; line.raycast = () => {}; stageCode.add(line);
      const lab = labelSprite(`route ${r.route ?? k} · spline ${r.splineId}`, '#fbbf24'); lab.position.copy(pts[0]).add(new THREE.Vector3(0, 80, 0)); stageCode.add(lab); });
    for (const c of o.commands || []) {
      const box = new THREE.LineSegments(new THREE.EdgesGeometry(new THREE.BoxGeometry(...(c.detect || [40, 40, 40]).map(v => Math.max(2, Math.abs(v) * 2)))), new THREE.LineBasicMaterial({color: CMD_COLOURS[c.action] ?? 0x94a3b8}));
      box.position.set(...c.position); box.rotation.set(...(c.rotation || [0, 0, 0]).map(THREE.MathUtils.degToRad), 'YXZ'); box.raycast = () => {}; stageCode.add(box);
      const lab = labelSprite(cmdText(c)); lab.position.set(c.position[0], c.position[1] + Math.abs(c.detect?.[1] || 40) + 30, c.position[2]); stageCode.add(lab);
    }
  }
  stageCode.visible = !hiddenCats.has('stagecode');
}
const cmdText = c => ({move: 'Move', stop: 'Stop', accelerate: `Accelerate${c.speed != null ? ' → ' + Math.round(c.speed) : ''}`, setpos: `Set pos${c.route != null ? ' route ' + c.route : ''}${c.fraction != null ? ' @' + Math.round(c.fraction * 100) + '%' : ''}`}[c.action] || c.action);
const commandFor = id => { for (const o of stageObjs()) { const c = (o.commands || []).find(x => x.id === id); if (c) return [o, c]; } return null; };

// ---- lighting (roadmap 7, L4 UI): project.lighting = {version 1, regions [...], lights [...], profiles [...]}; the shape
// is in STAGE-WORKSHOP-UI2-SPEC.md (builds it into lighting rules). Regions and lights are placements (category
// Lighting); profiles are trigger-only rules (no bounds) for the Set lighting profile node.
const LIGHT_KINDS = {lightregion: 'regions', light: 'lights'};
const lightingData = () => (project.lighting ||= {version: 1, regions: [], lights: [], profiles: []});
function listFor(o) {
  if (LIGHT_KINDS[o.kind]) { const L = lightingData(); return (L[LIGHT_KINDS[o.kind]] ||= []); }
  return o.kind === 'geometry' ? (project.geometry ||= []) : o.kind === 'path' ? (project.paths ||= []) : o.kind === 'trigger' ? (project.triggers ||= []) : project.objects;
}
// the keys of lighting_profiles.ini (runtime/lighting_environment.cpp clamps); empty = inherited from the stage profile
const PROFILE_KEYS = [['sun_tint', 'Sun tint (r, g, b)', 3, 0, 3], ['sun_strength', 'Sun strength', 1, 0, 3], ['sky', 'Sky colour (r, g, b)', 3, 0, 3], ['sky_strength', 'Sky strength', 1, 0, 3],
  ['ground', 'Ground colour (r, g, b)', 3, 0, 3], ['fog_colour', 'Fog colour (r, g, b)', 3, 0, 3], ['fog_density', 'Fog density', 1, 0, .02], ['fog_start', 'Fog start (units)', 1, 0, 60000],
  ['sun_azimuth', 'Sun azimuth (deg)', 1, 0, 360], ['sun_elevation', 'Sun elevation (deg)', 1, 2, 90]];
function profileFields(prof, attr) {
  return PROFILE_KEYS.map(([k, label, n, lo, hi]) => `<label class="field"><span>${label}</span><input class="num" ${attr}="${k}" placeholder="inherit" title="${n === 3 ? 'three numbers ' : ''}${lo}..${hi}; empty = inherited" value="${prof?.[k] === undefined ? '' : esc(Array.isArray(prof[k]) ? prof[k].join(', ') : prof[k])}"></label>`).join('');
}
function readProfileKey(prof, k, text) {   // -> error text or ''
  const [, label, n, lo, hi] = PROFILE_KEYS.find(x => x[0] === k); text = String(text).trim();
  if (!text) { delete prof[k]; return ''; }
  const v = text.split(/[\s,]+/).filter(Boolean).map(Number);
  if (v.length !== n || v.some(x => !Number.isFinite(x))) return `${label}: ${n === 3 ? 'three numbers' : 'a number'}`;
  const c = v.map(x => Math.min(hi, Math.max(lo, x))); prof[k] = n === 3 ? c : c[0]; return c.some((x, i) => x !== v[i]) ? `${label} kept within ${lo}..${hi}` : '';
}
const regionBox = o => { const sz = (o.size || []).map((v, i) => Math.abs(v * (o.scale?.[i] ?? 1))); return sz.length === 3 && sz.every(v => v > 0 && Number.isFinite(v)) ? [o.position.map((p, i) => p - sz[i] / 2), o.position.map((p, i) => p + sz[i] / 2)] : null; };
function lightingNode(o) {
  const g = new THREE.Group();
  if (o.kind === 'lightregion') {
    const sz = (o.size || [400, 300, 400]).map(v => Math.max(1, Math.abs(v))), box = new THREE.BoxGeometry(...sz);
    g.add(new THREE.LineSegments(new THREE.EdgesGeometry(box), new THREE.LineBasicMaterial({color: 0xfde047})));
    g.add(new THREE.Mesh(box, new THREE.MeshBasicMaterial({color: 0xfde047, transparent: true, opacity: .06, depthWrite: false, side: THREE.DoubleSide})));
    g.userData.volume = true;
  } else {
    const c = new THREE.Color(...(o.colour || [1, .9, .7]).map(v => Math.min(1, v)));
    g.add(new THREE.Mesh(new THREE.SphereGeometry(9, 16, 12), new THREE.MeshBasicMaterial({color: c})));
    const r = Math.max(1, o.radius || 300), ring = new THREE.EllipseCurve(0, 0, r, r).getPoints(48), geo = new THREE.BufferGeometry().setFromPoints(ring), mat = new THREE.LineBasicMaterial({color: c, transparent: true, opacity: .55});
    for (const rot of [[Math.PI / 2, 0, 0], [0, 0, 0], [0, Math.PI / 2, 0]]) { const l = new THREE.LineLoop(geo, mat); l.rotation.set(...rot); l.raycast = () => {}; g.add(l); }   // radius gizmo (not pickable)
  }
  return g;
}
function buildItems() { items = placed().filter(o => o.position).map(makeItem); byId = new Map(items.map(it => [it.id, it])); }
function visible(it) { return !hiddenCats.has(it.cat) && !(it.layer && hiddenLayers.has(it.layer)) && !hiddenZones.has(String(it.zone)); }
function matches(it) {
  if (!query) return true;
  return query.toLowerCase().split(/\s+/).filter(Boolean).every(tok => {
    const [k, v] = tok.includes(':') ? tok.split(':') : [null, tok];
    if (k === 'zone') return String(it.zone) === v; if (k === 'layer') return it.layer === v;
    if (k === 'cat') return it.cat.startsWith(v); if (k === 'class') return it.cls.toLowerCase().includes(v);
    return (it.name + ' ' + it.cls).toLowerCase().includes(v);
  });
}
// #93 (research/breakable-tile-93.md, server.py linked_switch_rules): a Digital Breakable Tile (7/215, ElecCristalWall)
// starts hidden and appears only when a 'Trigger: Digital Breakable Tile' (7/225, ElecCristalWallSwitch) with the SAME
// Link ID is touched; Link ID 0 / an unused one = it never appears
const isTile = o => o?.kind === 'native' && o.list === 7 && o.type === 215, isTileTrigger = o => o?.kind === 'native' && o.list === 7 && o.type === 225;
const tileTriggers = () => project.objects.filter(isTileTrigger);
const tileUnlinked = o => isTile(o) && !tileTriggers().some(t => (t.link || 0) === (o.link || 0));
function tileTriggerChoices(o) {   // per Link ID the nearest trigger that has it, nearest first
  const by = new Map(); for (const t of tileTriggers()) { const d = Math.hypot(...t.position.map((v, i) => v - o.position[i])), k = t.link || 0; if (!by.has(k) || d < by.get(k).d) by.set(k, {link: k, t, d}); }
  return [...by.values()].sort((a, b) => a.d - b.d);
}
function setTileLink(it, link) { snapshot(); it.o.link = link; changed(); renderIssueBadge(); if (panel === 'issues') renderPanel(); renderInspector(); toast(`${it.name}: Link ID ${link} · it appears when that trigger is touched`); }
function createTileTrigger(it) {
  const tmpl = tileTriggers()[0] || (project.nativeTemplates || []).find(isTileTrigger);
  if (!tmpl) return toast("No 'Trigger: Digital Breakable Tile' in this stage to copy: add one from the Add panel (All stages), then give both the same Link ID", true);
  const used = new Set(project.objects.filter(o => o.kind === 'native').map(o => o.link || 0)); let link = 1; while (used.has(link) && link < 255) link++;
  snapshot(); const t = structuredClone(tmpl); t.id = crypto.randomUUID(); delete t.sourceIndex; delete t.events; t.name = classOf(t); t.position = it.o.position.map((v, i) => round2(v + [0, 0, 300][i])); t.rotation = [0, 0, 0]; t.scale = [1, 1, 1]; t.link = link; if (!t.render) t.render = 10;
  it.o.link = link; project.objects.push(t); const ti = makeItem(t); items.push(ti); byId.set(ti.id, ti); addNode(ti); buildMarkers(); changed(); select(ti, false); drawFlow(); renderIssueBadge();
  toast(`Trigger added with Link ID ${link}, 300 units from the tile: move it where the player passes. The tile has Link ID ${link} now (Ctrl Z undoes both)`);
}
// ---- B5 bosses (docs/bosses/BOSS-SUPPORT.md, research): which boss classes can move to a new stage, an arena
// (centre + circle / box / ring + size) and a movement mode per placed boss, saved as o.boss = {arena: {shape, centre,
// radius, size, inner}, move: {mode: chase | patrol | circle | stationary, path, radius}} for the runtime (boss
// phase 2: the game does not read it yet). The arena is drawn while the boss is selected; Issues: not portable, an
// arena-bound boss without an arena, more than two bosses (one HP bar, one camera, the stage ends at the first kill).
const BOSSES = {'0-120': ['ready', 'Enemy framework: it works like an enemy anywhere.'],
  '0-180': ['try', 'Flies a chase around its spawn; no stage-number switch in its code.'], '0-181': ['try', 'Flies a chase around its spawn; no stage-number switch.'],
  '0-182': ['try', 'Runs on the reworked boss AI (boss_ai.cpp): portable now. New, so check the fight in a playtest.'], '0-183': ['try', 'Chase / escape, barrier, mines; no stage-number switch.'],
  '0-184': ['try', 'Runs on the reworked boss AI (boss_ai.cpp) and no longer needs his stage helpers. New, so check the fight in a playtest.'],
  '0-185': ['arena', "Its arena clamp is hard-coded per stage around (0, 0): set an Arena (below) and the game keeps it inside. It can walk a lap path between attacks (Movement)."],
  '0-186': ['arena', "Its arena clamp is hard-coded per stage around (0, 0): set an Arena (below) and the game keeps it inside. It can walk a lap path between attacks (Movement)."],
  '0-187': ['arena', "Its arena clamp is hard-coded per stage around (0, 0): set an Arena (below) and the game keeps it inside. It can walk a lap path between attacks (Movement)."],
  '0-188': ['try', 'Placed by the build, but this large stationary robot expects its platform layout and its own camera: it may not fight well in a new stage.'],
  '0-189': ['arena', 'Steers around the player on a ring track: a new floor needs the circle arena (runtime phase 4).'],
  '0-190': ['no', 'Left out by the build: the final battle needs the whole final-stage set-up (Super Shadow flight, psycho objects, clouds).']};
// the build keeps every placed boss (server.py, #125: the boss limits are lifted; boss_ai.cpp sums their HP into one bar and
// the stage clears when the last one is down). Only 'not portable' classes are left out. Sound: a stage loads 3 extra boss
// sound families at most (sound_banks.cpp, banks 8-10); the 4th+ family plays silent (the build warns too).
function bossBuildFate(o) { const b = bossOf(o); return b?.[0] === 'no' ? 'left out by the build' : ''; }
const BOSS_FAMILY = {0xB4: 'Black Bull', 0xB5: 'Black Bull', 0xB6: 'GUN', 0xB7: 'GUN', 0xB8: 'Black Doom', 0xB9: 'Egg Breaker', 0xBA: 'Egg Breaker', 0xBB: 'Egg Breaker', 0xBC: 'Diablon', 0xBD: 'Egg Dealer', 0xBE: 'Devil Doom'};
function bossFamilies() { const f = []; for (const o of realBosses()) { const n = o.list === 0 && BOSS_FAMILY[o.type]; if (n && bossOf(o)[0] !== 'no' && o.sourceStage != null && o.sourceStage !== project.stage && !f.includes(n)) f.push(n); } return f; }
const BOSS_PORT = {ready: ['ready', 'okText'], try: ['to try', 'warnText'], arena: ['arena needed', 'warnText'], no: ['not portable', 'badText']};
const bossOf = o => o?.kind === 'native' ? (BOSSES[`${o.list}-${o.type}`] || null) : null;
const realBosses = () => project.objects.filter(o => bossOf(o) && bossOf(o)[0] !== 'ready');   // Egg Pierrot counts as an enemy
const bossGroup = new THREE.Group(); bossGroup.renderOrder = 5;
// #127 the boss lap (Boss card movement 'Patrol a path' / 'Circle the arena centre'): the closed loop the build writes to
// mods/boss_path.txt (server.py: the path object's position + Y rotation, like three.js; a circle = 16 points at the
// radius round the arena centre). Egg Breaker (00/B9 B A B B) walks it between attacks at Speed units/s (egg_breaker.cpp).
const LAP_BOSSES = new Set([0xB9, 0xBA, 0xBB]), ARENA_BOSSES = new Set([0xB4, 0xB7, 0xB9, 0xBA, 0xBB]);   // runtime/egg_breaker.cpp, boss_arena.cpp kKinds
function bossLap(o) {
  const mv = o?.boss?.move; if (!mv || !['patrol', 'circle'].includes(mv.mode)) return null; const y = o.position[1] + 4;
  if (mv.mode === 'circle') { const a = o.boss.arena || {}, c = a.centre || o.position, r = +mv.radius || (a.radius ? a.radius * .7 : 600); return {pts: Array.from({length: 16}, (_, k) => new THREE.Vector3(c[0] + r * Math.cos(2 * Math.PI * k / 16), y, c[2] + r * Math.sin(2 * Math.PI * k / 16))), speed: +mv.speed || 300}; }
  const path = (project.paths || []).find(x => x.id === mv.path); if (!path || (path.points || []).length < 2) return {pts: [], speed: +mv.speed || 300, missing: !path};
  const pp = path.position || [0, 0, 0], yw = THREE.MathUtils.degToRad((path.rotation || [0, 0, 0])[1]), cy = Math.cos(yw), sy = Math.sin(yw);
  return {pts: path.points.map(q => new THREE.Vector3(pp[0] + q[0] * cy + q[2] * sy, (pp[1] || 0) + (q[1] || 0) + 4, pp[2] - q[0] * sy + q[2] * cy)), speed: +mv.speed || 300, path};
}
const lapOwner = () => { const o = selected && !multi.size ? selected.o : null; if (!o) return null; if (bossOf(o)) return o; if (o.kind === 'path') return project.objects.find(b => b.boss?.move?.mode === 'patrol' && b.boss.move.path === o.id) || null; return null; };
function drawBossLap(o) {
  const lap = bossLap(o); if (!lap || lap.pts.length < 2) return; const C = 0x22d3ee, loop = [...lap.pts, lap.pts[0]];
  const line = new THREE.Line(new THREE.BufferGeometry().setFromPoints(loop), new THREE.LineDashedMaterial({color: C, dashSize: 18, gapSize: 10, depthTest: false, transparent: true, opacity: .95})); line.computeLineDistances(); bossGroup.add(line);
  let len = 0; for (let i = 0; i < lap.pts.length; i++) { const a = loop[i], b = loop[i + 1], d = b.clone().sub(a), l = d.length(); len += l; if (l < 1) continue;   // a direction arrow at each segment's middle
    const cone = new THREE.Mesh(new THREE.ConeGeometry(9, 26, 10), new THREE.MeshBasicMaterial({color: C, depthTest: false, transparent: true, opacity: .95})); cone.position.copy(a).addScaledVector(d, .5); cone.quaternion.setFromUnitVectors(new THREE.Vector3(0, 1, 0), d.normalize()); bossGroup.add(cone); }
  const dots = new THREE.Points(new THREE.BufferGeometry().setFromPoints(lap.pts), new THREE.PointsMaterial({color: 0xffffff, size: 7, sizeAttenuation: false, depthTest: false})); bossGroup.add(dots);
  const ghost = shadowGhost(C); ghost.traverse(c => { if (c.isMesh) c.visible = !c.userData.modelName; }); ghost.scale.setScalar(4); ghost.position.copy(lap.pts[0]); ghost.rotation.y = Math.atan2(loop[1].x - loop[0].x, loop[1].z - loop[0].z); bossGroup.add(ghost);   // where the lap starts
  const sp = labelSprite(`Lap: ${lap.pts.length} points · ${Math.round(len)} units · about ${(len / lap.speed).toFixed(1)} s at ${Math.round(lap.speed)}/s${LAP_BOSSES.has(o.type) ? '' : ' · only Egg Breaker walks it so far'}`, '#67e8f9'); sp.center.set(.5, -1.2); sp.position.copy(lap.pts[0]); bossGroup.add(sp);
}
let lapSig = '';
function drawBossLapLive() {   // every frame: redraw while a lap point is dragged (the inspector is not rebuilt then)
  const o = lapOwner(), lap = o && bossLap(o), sig = lap ? JSON.stringify([o.id, o.boss, lap.path?.points, lap.path?.position, lap.path?.rotation]) : '';
  if (sig !== lapSig) { lapSig = sig; drawBossArena(); }
}
function makeLapPath(it) {
  const o = it.o; snapshot(); const r = (o.boss?.arena?.radius ? o.boss.arena.radius * .6 : 600);
  const pts = Array.from({length: 8}, (_, k) => [round2(r * Math.sin(2 * Math.PI * k / 8)), 0, round2(r * Math.cos(2 * Math.PI * k / 8))]);
  const [p] = insertObjects([{id: crypto.randomUUID(), name: `${it.name} lap`, kind: 'path', position: [...(o.boss?.arena?.centre || o.position)].map(round2), rotation: [0, 0, 0], scale: [1, 1, 1], pathType: 'native', bossLap: true, splineId: nextSplineId(), autoOrient: true, closed: true, points: pts, banking: 0}]);
  (o.boss ||= {}).move = {...(o.boss.move || {}), mode: 'patrol', path: p.o.id, speed: o.boss.move?.speed || 300}; changed(); renderIssueBadge(); renderInspector();
  toast(`Lap path made: 8 points round ${it.name}. Edit points: select the path (or "Edit the lap points"), drag its points, click the line to add one, Delete removes one`);
}
function drawBossArena() {
  if (!bossGroup.parent) scene.add(bossGroup); clear(bossGroup); const lo = lapOwner(); if (lo) drawBossLap(lo); const o = selected && !multi.size ? selected.o : null, a = bossOf(o) && o.boss?.arena; if (!a?.shape) { bossGroup.traverse(x => { x.renderOrder = 5; }); return; }
  const c = a.centre || o.position, mat = new THREE.LineBasicMaterial({color: 0xf97316, depthTest: false, transparent: true, opacity: .9});
  const ring = rad => { const pts = []; for (let i = 0; i <= 96; i++) { const t = i / 96 * Math.PI * 2; pts.push(new THREE.Vector3(c[0] + Math.cos(t) * rad, c[1] + 2, c[2] + Math.sin(t) * rad)); } bossGroup.add(new THREE.Line(new THREE.BufferGeometry().setFromPoints(pts), mat)); };
  if (a.shape === 'circle') ring(a.radius || 800);
  else if (a.shape === 'ring') { ring(a.radius || 800); ring(Math.min(a.inner ?? 300, (a.radius || 800) - 10)); }
  else if (a.shape === 'box') { const [w, d] = a.size || [1600, 1600], y = c[1] + 2; bossGroup.add(new THREE.LineLoop(new THREE.BufferGeometry().setFromPoints([[-1, -1], [1, -1], [1, 1], [-1, 1]].map(([x, z]) => new THREE.Vector3(c[0] + x * w / 2, y, c[2] + z * d / 2))), mat)); }
  bossGroup.traverse(x => { x.renderOrder = 5; });
}
// ---- Logic card (any object): friendly rules 'When <trigger> then <actions>' - an optional layer on top of the game's own
// behaviour (nothing changes until a rule is added). A rule is written as ordinary nodes into the graph where it can run,
// so it opens in the Logic tab and builds into sidecar v4 like any graph: the object's own graph (killed / collected /
// appears / entered / every N s / custom event), the stage graph (N seconds after the start, a variable reaches a value),
// the other object's graph (when it is destroyed) or a small trigger zone the editor keeps on the object (the player comes
// near). The event node carries ruleSpec {id, forObject, when, then}; every node of the rule has rule = its id. More
// triggers / actions: add a row to RULE_WHEN / RULE_THEN (and the node to nodes.json).
const RULE_WHEN = [
  ['killed', 'this is destroyed / defeated', 'self', ['native']], ['collected', 'this is collected / removed', 'self', ['native']], ['appears', 'this comes to life (spawns)', 'self', ['native']],
  ['enter', 'the player enters it', 'self', ['trigger']], ['leave', 'the player leaves it', 'self', ['trigger']],
  ['near', 'the player comes within … units', 'zone', ['*', '-trigger']], ['timer', '… seconds after the stage starts', 'stage', ['*']],
  ['every', 'every … seconds', 'selfOrStage', ['*']], ['other', 'another object is destroyed', 'other', ['*']],
  ['variable', 'a variable reaches a value', 'stage', ['*']], ['custom', 'a custom event is sent', 'selfOrStage', ['*']]];
const RULE_THEN = [
  ['score', 'add score'], ['rings', 'give rings'], ['hide', 'hide (destroy) an object', 'native'], ['show', 'show (spawn) a hidden object', 'native'], ['move', 'move an object', 'native'],
  ['setvar', 'set a variable'], ['addvar', 'add to a variable'], ['send', 'send a custom event'], ['teleport', 'teleport the player to'], ['wait', 'wait'],
  ['text', 'show text'], ['sound', 'play a sound'], ['link', 'open a link group']];   // 4th element true = 'needs game' (none since the VM actions, 10-19)
const ruleKind = o => o.kind === 'native' ? 'native' : o.kind === 'trigger' && !o.script ? 'trigger' : 'other';
const whenOk = (w, o) => { const k = ruleKind(o), list = w[3]; return list.includes(k) || (list.includes('*') && !list.includes('-' + k)); };
const ruleGraphs = () => [['stage', project.logic], ...(project.triggers || []).map(t => [t, t.logic]), ...project.objects.map(x => [x, x.logic])].filter(([, g]) => g?.nodes);
function rulesFor(o) { const out = []; for (const [own, g] of ruleGraphs()) for (const n of g.nodes) if (n.ruleSpec?.forObject === o.id) out.push({spec: n.ruleSpec, owner: own}); return out.sort((a, b) => (a.spec.t || 0) - (b.spec.t || 0)); }   // in the order they were made
function nativeChoices(o) {   // native objects to point at, nearest first
  return project.objects.filter(x => x.kind === 'native' && x !== o).map(x => [x, Math.hypot(...x.position.map((v, i) => v - o.position[i]))]).sort((a, b) => a[1] - b[1]).slice(0, 300).map(([x, d]) => ({id: x.id, label: `${x.name || classOf(x)} · ${Math.round(d)} u`}));
}
function dropRuleNodes(id) {
  for (const [own, g] of ruleGraphs()) { const gone = new Set(g.nodes.filter(n => n.rule === id).map(n => n.id)); if (!gone.size) continue;
    g.nodes = g.nodes.filter(n => !gone.has(n.id)); g.links = g.links.filter(l => !gone.has(l.from) && !gone.has(l.to)); logic.compile(own); }
  for (const t of (project.triggers || []).filter(t => t.ruleZone?.rule === id)) removeRuleZone(t);
}
function removeRuleZone(t) { const arr = project.triggers, i = arr.indexOf(t); if (i >= 0) arr.splice(i, 1); const it = byId.get(t.id); if (it) { items.splice(items.indexOf(it), 1); byId.delete(it.id); const n = nodeOf.get(it); if (n) { nodes.remove(n); nodeOf.delete(it); } } }
function ruleZoneFor(o, spec) {
  const R = Math.max(50, +spec.when.n || 300); let t = (project.triggers ||= []).find(x => x.ruleZone?.rule === spec.id);
  if (!t) { t = {id: 'zone-' + spec.id, name: `${o.name || classOf(o)} · near zone`, kind: 'trigger', position: o.position.map(round2), rotation: [0, 0, 0], scale: [1, 1, 1], size: [2 * R, 2 * R, 2 * R], event: 'enter', once: true, cooldown: 1, actions: [], ruleZone: {forObject: o.id, rule: spec.id}};
    project.triggers.push(t); const ti = makeItem(t); items.push(ti); byId.set(ti.id, ti); addNode(ti); }
  t.size = [2 * R, 2 * R, 2 * R]; t.position = o.position.map(round2); const ti = byId.get(t.id); if (ti) { ti.at = posOf(t); rebuildNode(ti); } return t;
}
function syncRuleZones() { for (const t of project?.triggers || []) { if (!t.ruleZone) continue; const o = byId.get(t.ruleZone.forObject)?.o; if (o && o.position.some((v, i) => Math.abs(v - t.position[i]) > .01)) { t.position = o.position.map(round2); const ti = byId.get(t.id); if (ti) { ti.at = posOf(t); const n = nodeOf.get(ti); if (n) n.position.set(...t.position); } } } }
function writeRule(o, spec) {
  dropRuleNodes(spec.id); const w = RULE_WHEN.find(x => x[0] === spec.when.kind); if (!w) return;
  let where = w[2]; if (where === 'selfOrStage') where = ruleKind(o) === 'other' ? 'stage' : 'self';
  const other = where === 'other' ? project.objects.find(x => x.id === spec.when.object && x.kind === 'native') : null; if (where === 'other' && !other) return;   // choose the object first
  const owner = where === 'self' ? o : where === 'stage' ? 'stage' : where === 'other' ? other : ruleZoneFor(o, spec);
  if (owner !== 'stage' && !owner.logic && owner.kind === 'trigger' && !(owner.actions || []).length) owner.logic = {version: 1, nodes: [], links: [], groups: []};   // an empty volume: no imported empty 'On enter' above the rule (v3 builds a volume's top event)
  const g = logic.graph(owner);
  const row = g.nodes.filter(n => logic.def(n.type)?.category === 'event').length, y0 = 40 + row * 240; let k = 0;
  const N = (type, x, y, values = {}) => { const d = logic.def(type), v = {}; for (const wd of d?.widgets || []) v[wd.name] = structuredClone(wd.default); Object.assign(v, values);
    const n = {id: `${spec.id}_${k++}`, type, x: Math.round(x), y: Math.round(y), values: v, collapsed: false, title: null, rule: spec.id}; g.nodes.push(n); return n; };
  const L = (a, out, b, inp) => g.links.push({id: `${spec.id}_l${g.links.length}_${k++}`, from: a.id, out, to: b.id, in: inp});
  const W = spec.when, EV = {killed: 'event.killed', collected: 'event.removed', appears: 'event.spawn', enter: 'event.enter', leave: 'event.exit', near: 'event.enter', other: 'event.killed', every: 'event.every', custom: 'event.custom', timer: 'event.stage_start', variable: 'event.stage_start'}[W.kind];
  const ev = N(EV, 0, y0, W.kind === 'every' ? {seconds: Math.max(.05, +W.n || 5)} : W.kind === 'custom' ? {name: W.name || 'my_event'} : ['killed', 'collected', 'appears', 'other', 'enter', 'leave', 'near'].includes(W.kind) ? {once: W.once !== false} : {});
  ev.ruleSpec = structuredClone(spec); ev.title = 'When ' + ruleText(spec.when, o); let prev = ev, x = 260;
  if (W.kind === 'timer') { const d = N('flow.delay', x, y0, {seconds: Math.max(0, +W.n || 10)}); L(prev, 'then', d, 'exec'); prev = d; x += 260; }
  if (W.kind === 'variable') { const gv = N('data.get_variable', x, y0 + 130, {name: W.name || 'my_variable'}), c = N('data.compare', x + 230, y0 + 130, {op: W.op || '>=', b: +W.value || 0}), wu = N('flow.wait_until', x + 230, y0);
    L(gv, 'value', c, 'a'); L(c, 'result', wu, 'condition'); L(prev, 'then', wu, 'exec'); prev = wu; x += 490; }
  const tgt = a => a.object === 'this' || !a.object ? (o.kind === 'native' ? o.id : '') : a.object, objOf = id => project.objects.find(z => z.id === id);
  for (const a of spec.then || []) {
    let n;
    if (a.kind === 'score') n = N('action.score', x, y0, {points: +a.n || 100});
    else if (a.kind === 'rings') { n = N('action.rings', x, y0); const lv = N('data.live', x - 60, y0 + 150, {value: '@rings'}), m = N('data.math', x + 140, y0 + 150, {op: '+', b: +a.n || 10}); L(lv, 'value', m, 'a'); L(m, 'result', n, 'amount'); }
    else if (a.kind === 'hide') n = N('action.destroy', x, y0, {target: tgt(a)});
    else if (a.kind === 'show') { const t = objOf(tgt(a)); n = N('action.spawn', x, y0, {template: tgt(a), at: (t?.position || o.position).map(round2)}); }
    else if (a.kind === 'move') { const t = objOf(tgt(a)), off = (a.vec || [0, 200, 0]).map(Number); n = N('action.move', x, y0, {target: tgt(a), to: (t?.position || o.position).map((v, i) => round2(v + (off[i] || 0))), seconds: Math.max(0, +a.seconds || 1)}); }
    else if (a.kind === 'setvar') n = N('action.set_variable', x, y0, {name: a.name || 'my_variable', value: +a.n || 0});
    else if (a.kind === 'addvar') n = N('action.add_variable', x, y0, {name: a.name || 'my_variable', amount: +a.n || 1});
    else if (a.kind === 'send') n = N('action.send', x, y0, {name: a.name || 'my_event'});
    else if (a.kind === 'teleport') n = N('action.teleport', x, y0, {destination: a.object || ''});
    else if (a.kind === 'wait') n = N('flow.delay', x, y0, {seconds: Math.max(0, +a.n || 1)});
    else if (a.kind === 'text') n = N('action.hud_text', x, y0, {text: a.text || 'Go!', seconds: Math.max(.5, +a.n || 3)});
    else if (a.kind === 'sound') n = N('action.play_sound', x, y0, {asset: a.name || ''});
    else if (a.kind === 'link') n = N('action.link_trigger', x, y0, {link: Math.max(1, +a.n || 1)});
    if (!n) continue; L(prev, 'then', n, 'exec'); prev = n; x += 260;
  }
  logic.compile(owner);
}
function ruleText(w, o) {
  const obj = id => { const x = project.objects.find(z => z.id === id); return x ? (x.name || classOf(x)) : 'an object'; };
  return {killed: 'this is destroyed', collected: 'this is collected', appears: 'this comes to life', enter: 'the player enters', leave: 'the player leaves', near: `the player comes within ${+w.n || 300} units`,
    timer: `${+w.n || 10} s after the start`, every: `every ${+w.n || 5} s`, other: `${obj(w.object)} is destroyed`, variable: `${w.name || 'my_variable'} ${w.op || '>='} ${+w.value || 0}`, custom: `"${w.name || 'my_event'}" is sent`}[w.kind] || w.kind;
}
let ruleOpen = new Set();
function appendLogicCard() {
  const el = $('inspector'), it = selected && !multi.size ? selected : null; if (!el || !it || !el.querySelector('#nameIn') || ['stageobject', 'playerstart', 'goalpose', 'spawn'].includes(it.o.kind) || it.o.ruleZone) return;
  const o = it.o, rules = rulesFor(o), whens = RULE_WHEN.filter(w => whenOk(w, o)), nat = nativeChoices(o), own = logic.kindOf(o) && o.logic ? o.logic.nodes.filter(n => logic.def(n.type)?.category === 'event' && !n.ruleSpec).length : logic.kindOf(o) ? logic.count(o) : 0;
  const objSel = (attr, cur, withThis) => `<select class="num" ${attr}>${withThis && o.kind === 'native' ? `<option value="this" ${!cur || cur === 'this' ? 'selected' : ''}>this object</option>` : '<option value="">choose…</option>'}${nat.map(c => `<option value="${esc(c.id)}" ${cur === c.id ? 'selected' : ''}>${esc(c.label)}</option>`).join('')}</select>`;
  const num = (attr, v, ph = '') => `<input class="num rN" type="number" step="any" ${attr} value="${v ?? ''}" placeholder="${ph}">`, txt = (attr, v, ph) => `<input class="num rT" ${attr} value="${esc(v ?? '')}" placeholder="${ph}">`;
  const whenParams = (r, w) => w.kind === 'near' ? num(`data-rp="${r}" data-k="n"`, w.n ?? 300, 'units') + '<span class="muted">units</span>' : ['timer', 'every'].includes(w.kind) ? num(`data-rp="${r}" data-k="n"`, w.n ?? (w.kind === 'timer' ? 10 : 5)) + '<span class="muted">s</span>'
    : w.kind === 'other' ? objSel(`data-rp="${r}" data-k="object"`, w.object, false) : w.kind === 'custom' ? txt(`data-rp="${r}" data-k="name"`, w.name, 'event name')
    : w.kind === 'variable' ? txt(`data-rp="${r}" data-k="name"`, w.name, 'variable') + `<select class="num rOp" data-rp="${r}" data-k="op">${['>=', '>', '==', '<=', '<', '!='].map(x => `<option ${(w.op || '>=') === x ? 'selected' : ''}>${x}</option>`).join('')}</select>` + num(`data-rp="${r}" data-k="value"`, w.value ?? 1) : '';
  const thenParams = (r, i, a) => ({score: () => num(`data-ra="${r}:${i}:n"`, a.n ?? 100), rings: () => num(`data-ra="${r}:${i}:n"`, a.n ?? 10), wait: () => num(`data-ra="${r}:${i}:n"`, a.n ?? 1) + '<span class="muted">s</span>',
    hide: () => objSel(`data-ra="${r}:${i}:object"`, a.object, true), show: () => objSel(`data-ra="${r}:${i}:object"`, a.object, true), teleport: () => objSel(`data-ra="${r}:${i}:object"`, a.object, false),
    move: () => objSel(`data-ra="${r}:${i}:object"`, a.object, true) + `<span class="muted">by</span>${[0, 1, 2].map(j => num(`data-ra="${r}:${i}:vec${j}"`, (a.vec || [0, 200, 0])[j], 'xyz'[j])).join('')}`,
    setvar: () => txt(`data-ra="${r}:${i}:name"`, a.name, 'variable') + num(`data-ra="${r}:${i}:n"`, a.n ?? 0), addvar: () => txt(`data-ra="${r}:${i}:name"`, a.name, 'variable') + num(`data-ra="${r}:${i}:n"`, a.n ?? 1),
    send: () => txt(`data-ra="${r}:${i}:name"`, a.name, 'event name'), text: () => txt(`data-ra="${r}:${i}:text"`, a.text, 'text'), sound: () => txt(`data-ra="${r}:${i}:name"`, a.name, 'sound'), link: () => num(`data-ra="${r}:${i}:n"`, a.n ?? 1)}[a.kind] || (() => ''))();
  const ruleHtml = ({spec}) => { const r = esc(spec.id), w = spec.when;
    return `<div class="rule" data-rule="${r}"><div class="rLine"><b>When</b><select class="num rSel" data-rw="${r}">${whens.map(x => `<option value="${x[0]}" ${w.kind === x[0] ? 'selected' : ''}>${esc(x[1])}</option>`).join('')}</select>${whenParams(r, w)}</div>
      ${(spec.then || []).map((a, i) => { const d = RULE_THEN.find(x => x[0] === a.kind); return `<div class="rLine then"><b>${i ? 'and' : 'then'}</b><select class="num rSel" data-rk="${r}:${i}">${RULE_THEN.filter(x => !x[2] || x[2] !== 'native' || nat.length || o.kind === 'native').map(x => `<option value="${x[0]}" ${a.kind === x[0] ? 'selected' : ''}>${esc(x[1])}${x[3] ? ' (needs game)' : ''}</option>`).join('')}</select>${thenParams(r, i, a)}${d?.[3] ? '<em class="soonTag" title="Defined, not in the game yet: the build leaves it out">needs game</em>' : ''}<button class="ghost small rX" data-rdel="${r}:${i}" title="Remove this action">×</button></div>`; }).join('')}
      <div class="rFoot"><button class="ghost small" data-radd="${r}">+ and then…</button><span class="grow"></span><button class="ghost small" data-rgraph="${r}">Open as graph</button><button class="ghost small" data-rrm="${r}">Delete rule</button></div></div>`; };
  const html = `<details class="sect" open id="logicCard"><summary>Rules <span>${rules.length || ''}</span></summary><div>
    ${rules.length ? '' : '<p class="muted">Optional: the object keeps its game behaviour. Add a rule to make something happen.</p>'}
    ${rules.map(ruleHtml).join('')}
    <div class="rAdd"><select class="num" id="ruleNewKind">${whens.map(x => `<option value="${x[0]}">When ${esc(x[1])}</option>`).join('')}</select><button class="ghost small" id="ruleNew">+ Add rule</button></div>
    ${own ? `<p class="muted">${own} more event${own > 1 ? 's' : ''} made in the Logic tab (not shown as rules).</p>` : ''}
    ${logic.kindOf(o) ? `<button class="ghost small" id="graphEdit2">Open this object's graph</button>` : ''}</div></details>`;
  const old = [...el.querySelectorAll('details.sect')].find(d => d.querySelector('summary')?.textContent.trim().startsWith('Logic') && d.id !== 'logicCard'); if (old) old.remove();   // the old 'Graphs: N · Edit' section
  const rel = [...el.querySelectorAll('details.sect')].find(d => /^Relations/.test(d.querySelector('summary')?.textContent || '')); if (rel) rel.insertAdjacentHTML('beforebegin', html); else el.insertAdjacentHTML('beforeend', html);
  const specOf = id => rules.find(x => x.spec.id === id)?.spec, redo = spec => { snapshot(); writeRule(o, spec); changed(); renderInspector(); renderIssueBadge(); };
  el.querySelector('#ruleNew')?.addEventListener('click', () => { const kind = el.querySelector('#ruleNewKind').value, spec = {id: 'r' + Math.random().toString(36).slice(2, 8), t: Date.now(), forObject: o.id, when: {kind}, then: [{kind: 'score', n: 100}]};
    if (kind === 'other') spec.when.object = nat[0]?.id; if (kind === 'near') spec.when.n = 300; redo(spec); toast(`Rule added: when ${ruleText(spec.when, o)}, then add 100 score. Change the actions below; it also opens as a graph in the Logic tab`); });
  el.querySelectorAll('[data-rw]').forEach(sel => sel.onchange = () => { const sp = structuredClone(specOf(sel.dataset.rw)); sp.when = {kind: sel.value}; if (sel.value === 'other') sp.when.object = nat[0]?.id; redo(sp); });
  el.querySelectorAll('[data-rp]').forEach(inp => inp.onchange = () => { const sp = structuredClone(specOf(inp.dataset.rp)); sp.when[inp.dataset.k] = inp.type === 'number' ? +inp.value : inp.value; redo(sp); });
  el.querySelectorAll('[data-rk]').forEach(sel => sel.onchange = () => { const [id, i] = sel.dataset.rk.split(':'), sp = structuredClone(specOf(id)); sp.then[+i] = {kind: sel.value}; redo(sp); });
  el.querySelectorAll('[data-ra]').forEach(inp => inp.onchange = () => { const [id, i, key] = inp.dataset.ra.split(':'), sp = structuredClone(specOf(id)), a = sp.then[+i];
    if (key.startsWith('vec')) { (a.vec ||= [0, 200, 0])[+key.slice(3)] = +inp.value || 0; } else a[key] = inp.type === 'number' ? +inp.value : inp.value; redo(sp); });
  el.querySelectorAll('[data-rdel]').forEach(b => b.onclick = () => { const [id, i] = b.dataset.rdel.split(':'), sp = structuredClone(specOf(id)); sp.then.splice(+i, 1); redo(sp); });
  el.querySelectorAll('[data-radd]').forEach(b => b.onclick = () => { const sp = structuredClone(specOf(b.dataset.radd)); sp.then.push({kind: 'wait', n: 1}); redo(sp); });
  el.querySelectorAll('[data-rrm]').forEach(b => b.onclick = () => { snapshot(); dropRuleNodes(b.dataset.rrm); changed(); renderInspector(); renderIssueBadge(); toast('Rule deleted'); });
  el.querySelectorAll('[data-rgraph]').forEach(b => b.onclick = () => { const r = rules.find(x => x.spec.id === b.dataset.rgraph); if (r) showWorkspace('logic', r.owner); });
  el.querySelector('#graphEdit2')?.addEventListener('click', () => showWorkspace('logic', o));
}
// ---- inspector sections: Basic (name, transform, the key parameters with friendly names), Logic (the rules card), and
// Advanced (collapsed: Link ID, activation, relations, preview model, textures, advanced / raw parameters, raw values)
let advOpen = false;
const ADV_SECT = /^(Relations|Game data|Preview model|Textures|Raw points)/;
function organizeInspector() {
  const el = $('inspector'), it = selected && !multi.size ? selected : null; if (!el || !it || !el.querySelector('#nameIn')) return;
  const sects = [...el.children].filter(c => c.matches('details.sect')); if (!sects.length) return;
  const basic = document.createElement('div'), logicG = document.createElement('div'), adv = document.createElement('details');
  basic.className = 'iGroup'; basic.innerHTML = '<h5>Basic</h5>'; logicG.className = 'iGroup'; logicG.innerHTML = '<h5>Logic</h5>';
  adv.className = 'iAdv'; adv.open = advOpen; adv.innerHTML = '<summary>Advanced <small>Link ID, activation, relations, raw values</small></summary>'; adv.ontoggle = () => { advOpen = adv.open; };
  for (const d of sects) { const t = d.querySelector('summary')?.textContent.trim() || ''; (d.id === 'logicCard' ? logicG : ADV_SECT.test(t) ? adv : basic).append(d); }
  const o = it.o, entry = o.kind === 'native' ? catalog.get(`${o.list}-${o.type}`) : null, advRows = [...basic.querySelectorAll('.pf[data-adv]')];
  if (advRows.length) { const d = document.createElement('details'); d.className = 'sect'; d.open = true; d.innerHTML = '<summary>Advanced parameters</summary><div></div>'; advRows.forEach(x => d.lastChild.append(x)); adv.prepend(d); }
  if (entry?.fields?.length) { const d = document.createElement('details'); d.className = 'sect';
    d.innerHTML = `<summary>Raw values</summary><div><div class="rawGrid">${entry.fields.map((f, off) => { const v = paramWord(o, off, f.type === 'float'); return v === null ? '' : `<span>${esc(f.name)}</span><b>${esc(String(f.type === 'float' ? +v.toFixed(4) : v))}</b>`; }).join('')}</div><p class="muted">The record's parameters as the game stores them (word ${'0'}..${entry.fields.length - 1}).</p></div>`; adv.append(d); }
  el.append(basic); if (logicG.children.length > 1) el.append(logicG); if (adv.children.length > 1) el.append(adv);
}
// City Laser (3/234) has no model the editor can draw (invisible after placing): a red beam + its detect radius until
// data gives the real beam path
function laserMarker(o) {
  const g = new THREE.Group(), red = new THREE.MeshBasicMaterial({color: 0xff2a3a, transparent: true, opacity: .85}), f = catalog.get('3-234')?.fields || [], ri = f.findIndex(x => x.name === 'DetectRadius');
  const hz = fieldAt(o, 'HurtPlayer'), scenery = hz >= 0 && paramWord(o, hz) === 1; if (scenery) red.opacity = .35;   // Hazard = No: background laser
  const beam = new THREE.Mesh(new THREE.CylinderGeometry(10, 10, 1500, 8), red); beam.position.y = 750; g.add(beam);   // rises 1500 from its foot (capture)
  const head = new THREE.Mesh(new THREE.SphereGeometry(14, 12, 8), red); g.add(head);
  const R = Math.max(50, Math.min(5000, Math.abs(+(ri >= 0 ? paramWord(o, ri, f[ri]?.type === 'float') : 0)) || 300)), pts = [];
  for (let i = 0; i <= 64; i++) { const a = i / 64 * Math.PI * 2; pts.push(new THREE.Vector3(Math.cos(a) * R, 0, Math.sin(a) * R)); }
  g.add(new THREE.Line(new THREE.BufferGeometry().setFromPoints(pts), new THREE.LineBasicMaterial({color: 0xff2a3a, transparent: true, opacity: .6})));
  g.userData.laserMarker = true; return g;
}
function appendBossCard() {
  const el = $('inspector'), it = selected && !multi.size ? selected : null, b = it && bossOf(it.o); if (!el || !b) { drawBossArena(); return; }
  const o = it.o, bs = o.boss || {}, a = bs.arena || {}, mv = bs.move || {}, [port, note] = b, [pl, pc] = BOSS_PORT[port], c = a.centre || o.position, n = realBosses().length;
  const num = (attr, v, ph = '') => `<input class="num" type="number" step="any" ${attr} value="${v ?? ''}" placeholder="${ph}">`;
  const paths = (project.paths || []).filter(p => p.points?.length > 1);
  const html = `<details class="sect" open id="bossCard"><summary>Boss <small class="${pc}">${pl}</small></summary><div>
    <p class="muted">${esc(note)}${port === 'ready' ? '' : (f => f ? ` <b class="badText">Build: ${esc(f)}.</b>` : n >= 2 ? ` <b class="okText">${n} bosses: one HP bar shows their summed HP, and the stage clears when the last one is down.</b>` : '')(bossBuildFate(o))}</p>
    ${port === 'ready' ? '' : `<label class="field"><span>Arena (where it fights)</span><select class="num" id="bArShape">${[['', 'None (as the game does it)'], ['circle', 'Circle'], ['box', 'Box'], ['ring', 'Ring (inner / outer)']].map(([v, l]) => `<option value="${v}" ${(a.shape || '') === v ? 'selected' : ''}>${l}</option>`).join('')}</select></label>
    ${a.shape ? `<div class="field"><span>Centre</span><div class="vec">${['X', 'Y', 'Z'].map((ax, i) => `<div class="ax" data-ax="${ax}">${num(`data-bac="${i}"`, +(+c[i]).toFixed(2))}</div>`).join('')}</div><button class="ghost small" id="bArHere">At the boss</button></div>
      ${a.shape === 'box' ? `<div class="field"><span>Width × depth</span><div class="vec2">${num('data-bas="0"', (a.size || [1600, 1600])[0])}${num('data-bas="1"', (a.size || [1600, 1600])[1])}</div></div>` : `<label class="field"><span>${a.shape === 'ring' ? 'Outer radius' : 'Radius'}</span>${num('id="bArR"', a.radius ?? 800)}</label>`}
      ${a.shape === 'ring' ? `<label class="field"><span>Inner radius</span>${num('id="bArIn"', a.inner ?? 300)}</label>` : ''}` : ''}
    <label class="field"><span>Movement</span><select class="num" id="bMove">${[['chase', 'Chase (its own AI)'], ['patrol', 'Patrol a path'], ['circle', 'Circle the arena centre'], ['stationary', 'Stay in place']].map(([v, l]) => `<option value="${v}" ${(mv.mode || 'chase') === v ? 'selected' : ''}>${l}</option>`).join('')}</select></label>
    ${mv.mode === 'patrol' ? `<label class="field"><span>Path</span><select class="num" id="bPath"><option value="">choose a path…</option>${paths.map(p => `<option value="${esc(p.id)}" ${mv.path === p.id ? 'selected' : ''}>${esc(p.name || 'Path')} (${p.points.length} points)</option>`).join('')}</select>${paths.length ? '' : '<small class="warnText">No path yet: make a lap below.</small>'}</label><div class="wsBtns"><button class="ghost small" id="bLapNew">Make a lap path round it</button>${mv.path && paths.some(p => p.id === mv.path) ? '<button class="ghost small" id="bLapEdit">Edit the lap points</button>' : ''}</div>` : ''}
    ${['patrol', 'circle'].includes(mv.mode) ? `<label class="field"><span>Speed <small>units / second</small></span>${num('id="bSpeed"', mv.speed ?? 300)}</label>` : ''}
    ${mv.mode === 'circle' ? `<label class="field"><span>Circle radius</span>${num('id="bCircR"', mv.radius ?? (a.radius ? Math.round(a.radius * .7) : 600))}</label>` : ''}
    <p class="muted">${a.shape ? (ARENA_BOSSES.has(o.type) ? '<small class="tagOk">works in game</small> The game keeps it inside this arena.' : '<small class="tagNo">saved</small> The game does not read the arena for this boss yet.') : ''} ${['patrol', 'circle'].includes(mv.mode) ? (LAP_BOSSES.has(o.type) ? '<small class="tagOk">works in game</small> It walks the cyan lap (arrows = direction) between attacks.' : '<small class="tagNo">saved</small> Only Egg Breaker walks a lap so far; this boss keeps its own AI.') : mv.mode === 'stationary' ? '<small class="tagNo">saved</small> Stay in place is not read by the game yet.' : ''}</p>`}</div></details>`;
  const par = [...el.querySelectorAll('details.sect')].find(d => d.querySelector('summary')?.textContent.startsWith('Parameters'));
  if (par) par.insertAdjacentHTML('beforebegin', html); else el.insertAdjacentHTML('beforeend', html);
  const B = () => (o.boss ||= {}), AR = () => (B().arena ||= {}), MV = () => (B().move ||= {}), done = () => { changed(); renderIssueBadge(); renderInspector(); };
  el.querySelector('#bArShape')?.addEventListener('change', e => { snapshot(); if (!e.target.value) { delete B().arena; } else { AR().shape = e.target.value; AR().centre ||= o.position.map(round2); } done(); });
  el.querySelector('#bArHere')?.addEventListener('click', () => { snapshot(); AR().centre = o.position.map(round2); done(); });
  el.querySelectorAll('[data-bac]').forEach(inp => inp.onchange = () => { snapshot(); const cc = (AR().centre ||= o.position.map(round2)); cc[+inp.dataset.bac] = +inp.value || 0; done(); });
  el.querySelectorAll('[data-bas]').forEach(inp => inp.onchange = () => { snapshot(); const sz = (AR().size ||= [1600, 1600]); sz[+inp.dataset.bas] = Math.max(50, +inp.value || 50); done(); });
  el.querySelector('#bArR')?.addEventListener('change', e => { snapshot(); AR().radius = Math.max(50, +e.target.value || 50); done(); });
  el.querySelector('#bArIn')?.addEventListener('change', e => { snapshot(); AR().inner = Math.max(0, +e.target.value || 0); done(); });
  el.querySelector('#bMove')?.addEventListener('change', e => { snapshot(); if (e.target.value === 'chase') delete B().move; else MV().mode = e.target.value; done(); });
  el.querySelector('#bPath')?.addEventListener('change', e => { snapshot(); MV().path = e.target.value || undefined; done(); });
  el.querySelector('#bCircR')?.addEventListener('change', e => { snapshot(); MV().radius = Math.max(50, +e.target.value || 50); done(); });
  el.querySelector('#bSpeed')?.addEventListener('change', e => { snapshot(); MV().speed = Math.min(3000, Math.max(20, +e.target.value || 300)); done(); });
  el.querySelector('#bLapNew')?.addEventListener('click', () => makeLapPath(it));
  el.querySelector('#bLapEdit')?.addEventListener('click', () => { const p = items.find(x => x.o.id === mv.path); if (p) { select(p, false); toast('Lap points: drag them, click the line to add one, select one + Delete removes it (each edit is one undo step)'); } });
  drawBossArena();
}
function appendTileCard() {
  const el = $('inspector'), it = selected && !multi.size ? selected : null; if (!el || !it || !isTile(it.o)) return;
  const o = it.o, ch = tileTriggerChoices(o), mine = ch.find(c => c.link === (o.link || 0));
  const html = `<details class="sect" open id="tileCard"><summary>Appears when <small class="tagNo">Link ID ${o.link || 0}</small></summary><div>
    ${mine ? `<p class="okText">It appears when "${esc(mine.t.name)}" (Link ID ${mine.link}, ${Math.round(mine.d)} units away) is touched.</p>` : `<p class="badText"><b>No trigger uses Link ID ${o.link || 0}: this tile never appears.</b> It starts hidden and appears when a 'Trigger: Digital Breakable Tile' with the same Link ID is touched.</p>`}
    ${ch.length ? `<div class="tlList">${ch.slice(0, 8).map(c => `<div class="tlRow ${c === mine ? 'on' : ''}"><b>Link ${c.link}</b><span title="${esc(c.t.name)}">${esc(c.t.name)}</span><small>${Math.round(c.d)} u</small>${c === mine ? '<small class="okText">in use</small>' : `<button class="ghost small" data-tlink="${c.link}">Use this Link ID</button>`}<button class="ghost small" data-tfocus="${esc(c.t.id)}" title="Fly to that trigger">Focus</button></div>`).join('')}</div>` : '<p class="muted">There is no Trigger: Digital Breakable Tile in this project.</p>'}
    <div class="wsBtns"><button class="ghost small" id="tileNewTrig">Create a trigger with a new Link ID</button></div></div></details>`;
  const par = [...el.querySelectorAll('details.sect')].find(d => d.querySelector('summary')?.textContent.startsWith('Parameters'));
  if (par) par.insertAdjacentHTML('beforebegin', html); else el.insertAdjacentHTML('beforeend', html);
  el.querySelectorAll('[data-tlink]').forEach(b => b.onclick = () => setTileLink(it, +b.dataset.tlink));
  el.querySelectorAll('[data-tfocus]').forEach(b => b.onclick = () => { const t = project.objects.find(x => x.id === b.dataset.tfocus); if (t) flyTo(t.position, 500); });
  el.querySelector('#tileNewTrig')?.addEventListener('click', () => createTileTrigger(it));
}
// #83 Link ID: natives with the same non-zero Link ID form a group in game (City Lasers 100.., triggers 10..); a copy
// that keeps its source's ID joins that group (a Destructible Object with the City Lasers' 100 draws one frame and is
// gone). Copies get 0 (toast); Issues flags copies (not untouched game records) sharing an ID with another class.
const linkedElsewhere = (o, others) => !!o.link && others.some(x => x !== o && x.kind === 'native' && x.link === o.link);
function resetCopiedLinks(objs, existing) {
  let n = 0; for (const o of objs) if (o.kind === 'native' && o.link && linkedElsewhere(o, existing)) { o.link = 0; n++; }
  if (n) setTimeout(() => toast(`Link ID reset to 0 on ${n} cop${n > 1 ? 'ies' : 'y'}: copies of linked objects lose their group (set it again in the inspector if you build a linked group)`), 80);
  return n;
}
function issues() {
  const out = [];
  if (project) for (const [own, g] of ruleGraphs()) for (const n of g.nodes) if (n.ruleSpec && !byId.has(n.ruleSpec.forObject)) { const it = own === 'stage' ? null : byId.get(own.id); out.push([it || items[0], 'A rule of a deleted object', `"${n.title || 'When …'}" still runs (in ${own === 'stage' ? 'the stage graph' : esc(own.name || 'a graph')}). Remove it, or undo the delete.`, () => { snapshot(); dropRuleNodes(n.ruleSpec.id); changed(); renderIssueBadge(); if (panel === 'issues') renderPanel(); }]); }
  for (const it of items) {
    const o = it.o;
    if (o.kind === 'lightregion' && !regionBox(o)) out.push([it, 'Lighting region without bounds', 'Give it a size above 0 on X, Y and Z: the rule only applies inside its box.']);
    if (o.kind === 'lightregion' && regionBox(o) && !Object.keys(o.profile || {}).length) out.push([it, 'Lighting region changes nothing', 'Set at least one atmosphere value (sun, sky, fog) in the inspector.']);
    if (o.kind === 'light' && !(o.radius > 0)) out.push([it, 'Point light without a radius', 'Set a radius above 0.']);
    if (o.kind === 'native' && o.link && o.sourceIndex == null) { const other = project.objects.find(x => x !== o && x.kind === 'native' && x.link === o.link && (x.list !== o.list || x.type !== o.type));
      if (other) out.push([it, `Link ID ${o.link} is ${other.name}'s group`, `A copy that keeps Link ID ${o.link} joins the ${other.name} group and can break in game (a Destructible Object there draws one frame, then is gone). Link ID 0 unless you build a linked group.`, () => { snapshot(); o.link = 0; changed(); renderIssueBadge(); if (panel === 'issues') renderPanel(); renderInspector(); toast(`${it.name}: Link ID set to 0`); }]); }
    if (tileUnlinked(o)) { const c = tileTriggerChoices(o)[0]; out.push([it, `Never appears: no trigger uses Link ID ${o.link || 0}`, `A Digital Breakable Tile starts hidden and appears when a 'Trigger: Digital Breakable Tile' with the same Link ID is touched. ${c ? `Fix: Link ID ${c.link} of the nearest trigger (${Math.round(c.d)} units).` : 'Fix: add such a trigger with a new Link ID.'}`, () => c ? setTileLink(it, c.link) : createTileTrigger(it)]); }
    const bo = bossOf(o);
    if (bo?.[0] === 'no') out.push([it, `${it.name} is left out`, bo[1] + ' (docs/bosses/BOSS-SUPPORT.md)']);
    if (bo?.[0] === 'arena' && !o.boss?.arena?.shape) out.push([it, `${it.name} needs an arena`, bo[1] + ' Inspector > Boss > Arena.']);
    if (o.kind === 'native' && !o.render) out.push([it, 'Activation distance 0', 'The game never activates it (originals use 3-10).']);
    if (o.kind === 'native' && o.scale && o.scale.some(s => Math.abs(s - 1) > 1e-6)) out.push([it, 'Scaled with the gizmo', 'Builds at normal size: native objects size themselves from their parameters.']);
    if (o.kind === 'geometry' && o.surface === 32 && o.collision !== false) out.push([it, 'Kill plane', 'Faces with surface 32 are not solid: Shadow falls through them and is killed when he falls down through them (proven in game, #38). Use Solid with another surface for a floor.']);
    if (o.kind === 'geometry' && o.renderVisible !== false && !project.materials.find(m => m.id === o.material)?.custom && project.materials.find(m => m.id === o.material)?.chunk !== o.chunk) out.push([it, 'Material not in its chunk', `Chunk ${o.chunk} does not load this texture. Pick one in the inspector, or "From position".`]);
    if (it.zone === null && o.kind === 'native') out.push([it, 'Outside every zone', 'Not inside any stage chunk; it may never load or have no collision under it.']);
  }
  if (project) { const fam = bossFamilies(); if (fam.length > 3) out.push([items.find(it => bossOf(it.o) && BOSS_FAMILY[it.o.type] === fam[3]) || items[0], `${fam.length} boss sound families: ${fam.slice(3).join(', ')} play silent`, `A stage loads 3 extra boss sound families at most (${fam.slice(0, 3).join(', ')} here). Keep bosses from 3 families, or accept the silent ones.`]); }
  return out;
}

// ---- viewport ------------------------------------------------------------------------------------------------------
const viewport = $('viewport');
// logarithmic depth: even depth precision from 2 to 80000 units (plain depth made distant layered surfaces show through each other)
const renderer = new THREE.WebGLRenderer({antialias: true, logarithmicDepthBuffer: true}); renderer.setPixelRatio(Math.min(devicePixelRatio, 2)); renderer.outputColorSpace = THREE.SRGBColorSpace;
renderer.domElement.className = 'gl'; viewport.prepend(renderer.domElement);
renderer.domElement.addEventListener('webglcontextlost', () => lifeEvent('webglcontextlost')); renderer.domElement.addEventListener('webglcontextrestored', () => lifeEvent('webglcontextrestored'));
const scene = new THREE.Scene(); scene.fog = new THREE.Fog(0x0a0c12, 6000, 26000);
const camera = new THREE.PerspectiveCamera(55, 1, 2, 80000); camera.position.set(400, 400, 400);
const orbit = new OrbitControls(camera, renderer.domElement); orbit.enableDamping = true; orbit.dampingFactor = .12; orbit.zoomSpeed = 2.2; orbit.zoomToCursor = true; orbit.panSpeed = 1.4;
orbit.mouseButtons = {LEFT: -1, MIDDLE: THREE.MOUSE.ROTATE, RIGHT: THREE.MOUSE.PAN};   // user request 2026-10-16: hold LEFT = first-person look (below), middle = orbit, right = pan, wheel zoom; WASD/QE fly on top
// First-person look: hold the left button and move the mouse to turn the camera on the spot (yaw / pitch); the orbit target
// follows the view direction at its current distance, so WASD / wheel / double-click keep working from the new view.
let fpLook = null;
renderer.domElement.addEventListener('pointerdown', () => { if (document.activeElement?.id === 'aq') document.activeElement.blur(); }, true);   // #85 the search gives the keys back to the view (WASD)
renderer.domElement.addEventListener('pointerdown', e => { if (e.button === 0 && !e.altKey && !e.shiftKey && !gizmo.dragging && !gizmo.axis && !placing && !drawing) fpLook = {x: e.clientX, y: e.clientY}; });
window.addEventListener('pointermove', e => {
  if (!fpLook) return; const dx = e.clientX - fpLook.x, dy = e.clientY - fpLook.y; fpLook.x = e.clientX; fpLook.y = e.clientY; if (!dx && !dy) return;
  fly = null; const off = orbit.target.clone().sub(camera.position), sph = new THREE.Spherical().setFromVector3(off);
  sph.theta -= dx * 0.0032; sph.phi = Math.max(0.05, Math.min(Math.PI - 0.05, sph.phi + dy * 0.0032));
  orbit.target.copy(camera.position).add(new THREE.Vector3().setFromSpherical(sph));
});
window.addEventListener('pointerup', () => fpLook = null); window.addEventListener('blur', () => fpLook = null);
const hemi = new THREE.HemisphereLight(0xdfe7ff, 0x3a3226, 1.7); scene.add(hemi); const sun = new THREE.DirectionalLight(0xfff1dd, 1.6), sunDir = new THREE.Vector3(1, 2, .6).normalize(); scene.add(sun.target); sun.position.set(1, 2, .6); scene.add(sun);
const art = new THREE.Group(), kill = new THREE.Group(), coll = new THREE.Group(), markers = new THREE.Group(), nodes = new THREE.Group(), selRing = new THREE.Group(); scene.add(art, kill, coll, nodes, markers, selRing);
const nodeOf = new Map();
const textures = new Map();
R.tex = a => tex(a);
function tex(asset) { if (!asset) return null; if (!textures.has(asset)) { const t = new THREE.TextureLoader().load('/asset/' + asset, () => inspectAlpha(asset, t)); t.colorSpace = THREE.SRGBColorSpace; t.wrapS = t.wrapT = THREE.RepeatWrapping; t.flipY = false; textures.set(asset, t); } return textures.get(asset); }
// #51: a texture whose alpha is a gradient (glows, cloud rims: Sky Troops' s0402_za025, white RGB + soft alpha) is
// blended in game; drawn with alpha-test only it showed as opaque white bands. When such a texture loads, every
// material using it switches to blending (render3d.material() does the same for textures already known). A texture
// that is fully transparent is reported (decode suspect).
const texAlpha = new Map();   // asset -> 'translucent' | 'empty' | 'ok'
function inspectAlpha(asset, t) {
  try {
    const c = document.createElement('canvas'); c.width = c.height = 32; const g = c.getContext('2d', {willReadFrequently: true}); g.drawImage(t.image, 0, 0, 32, 32);
    const a = g.getImageData(0, 0, 32, 32).data; let mid = 0, zero = 0; for (let i = 3; i < a.length; i += 4) { if (a[i] < 13) zero++; else if (a[i] < 230) mid++; }
    const kind = zero === 1024 ? 'empty' : mid / 1024 > .3 ? 'translucent' : 'ok'; texAlpha.set(asset, kind); t.userData.alpha = kind;
    if (kind === 'translucent') scene.traverse(n => { for (const m of n.isMesh ? [n.material].flat() : []) if (m.map === t && !m.transparent) { m.transparent = true; m.depthWrite = false; m.alphaTest = .01; m.needsUpdate = true; } });
    if (kind === 'empty') { texReport = null; if (panel === 'issues') renderPanel(); renderIssueBadge(); }
  } catch {}
}
// materials of the stage art that draw without a texture, per world: no texture name in the world (vertex colours
// only), a name the server has no image for (not in the texture library, no disc fallback: #27), or an image that is
// fully transparent. Listed in Issues and once in the console (console.table).
let texReport = null;
function textureReport() {
  if (texReport) return texReport; const out = [], seen = new Set();
  for (const m of project.reference || []) {
    if (m.collision) continue;
    for (const i of new Set(m.materials || [0])) {
      const mat = project.materials[i]; if (!mat || seen.has(m.name + '|' + i)) continue; seen.add(m.name + '|' + i);
      const why = !mat.texture ? 'vertex-coloured, no texture (by design)' : !mat.asset ? 'no image: not in the texture library, no disc fallback' : texAlpha.get(mat.asset) === 'empty' ? 'image is fully transparent (decode suspect)' : null;
      if (why) out.push({world: m.name, chunk: m.chunk, material: mat.id, texture: mat.texture || '(none)', why, faces: (m.materials || []).filter(x => x === i).length || m.triangles.length});
    }
  }
  return texReport = out;
}
function geometry(m) {
  const g = new THREE.BufferGeometry(); g.setAttribute('position', new THREE.Float32BufferAttribute(m.vertices.flat(), 3)); g.setIndex(m.triangles.flat());
  if (m.uvs) g.setAttribute('uv', new THREE.Float32BufferAttribute(m.uvs.flat(), 2));
  if (m.colours) g.setAttribute('color', new THREE.Float32BufferAttribute(m.colours.flatMap(c => c.slice(0, 3).map(x => x / 255)), 3));
  g.computeVertexNormals(); return g;
}
function clear(group) { for (const c of [...group.children]) { group.remove(c); c.traverse(n => { n.geometry?.dispose(); }); } }
function buildArt() {
  clear(art);
  const mats = project.materials.map(m => material(m, {alphaTest: .1}));
  const top = Math.max(-1, ...(project.reference || []).filter(m => !m.collision).flatMap(m => m.materials?.length ? [Math.max(...m.materials)] : [0]));
  if (top >= mats.length) { const grey = material(null, {alphaTest: .1}); grey.color.set(0x9aa0aa); while (mats.length <= top) mats.push(grey); console.warn(`[ui2] stage art uses material ${top} but the project has ${project.materials.length}: drawn grey`); }   // a project saved without its materials (server import cache, NOTE)
  const backdrop = new Set((project.native?.chunks || []).filter(c => c.backdrop).map(c => c.number));
  for (const m of project.reference || []) if (!m.collision) art.add(referenceMesh(m, mats, backdrop.has(m.chunk)));
  applyArtEdits(); markShells();
  skyActive = null; updateSkybox(0); applyArt(); buildCollision(); buildSurfColl();
  texReport = null; const tr = textureReport(); if (tr.length) { console.info(`[ui2] ${tr.length} stage-art materials draw without a texture (#51):`); console.table(tr); }
}
// #88 enclosing shells: low-poly art layers (mostly 'G' / 'D' worlds: Final Haunt STG0604_01_G_, Digital Circuit
// STG0200_G_01) whose box holds most of their chunk's other art, but too small for the server's backdrop rule (> 14000).
// The editor draws art from both sides, so from inside one the user saw 'a dark elliptical glass shape over the whole
// view' (its inside, dark vertex colours, the open rim as the ellipse edge). While the camera is inside a shell it is
// drawn in the background pass (layer 1, before the stage, depth cleared: never over it) and clicks / placing skip it.
let shells = [];
function markShells() {
  const solid = art.children.filter(g => !g.userData.backdrop), info = solid.map(g => { g.updateMatrixWorld(true); const b = new THREE.Box3().setFromObject(g); let tris = 0; g.traverse(m => { if (m.isMesh) tris += (m.geometry.index?.count || 0) / 3; }); return {g, b, tris}; });
  shells = [];
  for (const a of info) {
    a.g.userData.shell = false; a.g.userData.shellIn = false; if (a.b.isEmpty() || a.tris >= 2000 || ['O', 'OS'].includes(a.g.userData.kind)) continue;   // main geometry (rooms the player walks in) is never a shell
    const sz = a.b.getSize(new THREE.Vector3()); if (Math.min(sz.x, sz.z) < 3000) continue;
    const same = info.filter(o => o !== a && o.g.userData.chunk === a.g.userData.chunk && !o.b.isEmpty()); if (!same.length) continue;
    if (!['G', 'D'].includes(a.g.userData.kind) && same.filter(o => a.b.containsPoint(o.b.getCenter(new THREE.Vector3()))).length / same.length < .5) continue;   // G / D layers this big are shells as they are (self-test: Westopolis STG0100_G_01 darkened the sky at the start)
    a.g.userData.shell = true; a.g.userData.shellBox = a.b.clone(); shells.push(a.g);
    // drawn as glow (additive): dark vertex colours add nothing, so the shell never darkens the view (Prison Island's G
    // layer = see-through light shafts, proven; these shells' blend in game is not proven)
    a.g.traverse(m => { if (m.isMesh) m.material = [m.material].flat().map(x => { const c = x.clone(); c.transparent = true; c.depthWrite = false; c.blending = THREE.AdditiveBlending; c.alphaTest = 0; return c; }); });
  }
}
// #157: a 'D' shell is the distant sky of its chunk (Cryptic Castle STG0300_D_23 / D_40, 40 km boxes). The game camera
// is always inside the one it shows; the editor camera is often outside one of them, which drew that sky OVER the stage
// as huge orange / grey shapes - so D shells always go to the background pass. G shells keep the inside rule.
function updateShells() {   // called per frame: a shell the camera is inside goes to the background pass
  for (const g of shells) { const inside = g.userData.kind === 'D' || g.userData.shellBox.containsPoint(camera.position); if (inside === g.userData.shellIn) continue; g.userData.shellIn = inside; g.traverse(m => { if (m.isMesh) m.layers.set(inside ? 1 : 0); }); }
}
// Original collision (project.reference meshes with collision: true): a faint wireframe (Collision lens) and the kill
// planes (faces with flag exactly 0x20, Kill planes lens). Faces listed in project.collisionEdits[<reference id>] are
// removed (classic killNode / referenceNode; ids collision-<chunk>-<mesh> or collision-<chunk>-<file>-<mesh>, #14).
// userData.faceMap maps a drawn triangle back to its face index in the original mesh (face editing, classic
// editCollisionFace).
function buildCollision() {
  clear(coll); clear(kill);
  for (const m of project.reference || []) {
    if (!m.collision) continue;
    const gone = new Set(project.collisionEdits?.[m.id] || []), keep = m.triangles.map((_, i) => i).filter(i => !gone.has(i)), deadly = m.collisionRole === 'extra' ? [] : keep.filter(i => (m.flags?.[i] ?? 0) === 0x20);   // extra files (#39) hold raw words, not surface flags
    const mesh = (faces, mat) => { const x = new THREE.Mesh(geometry({vertices: m.vertices, triangles: faces.map(i => m.triangles[i])}), mat); x.userData = {ref: m.id, faceMap: faces, chunk: m.chunk}; return x; };
    if (keep.length) coll.add(mesh(keep, new THREE.MeshBasicMaterial({color: 0x60bcce, wireframe: true, transparent: true, opacity: .3, side: THREE.DoubleSide})));
    if (!deadly.length) continue;
    const km = mesh(deadly, new THREE.MeshBasicMaterial({color: 0xff2a3a, transparent: true, opacity: .05, depthWrite: false, side: THREE.DoubleSide}));
    km.add(new THREE.LineSegments(new THREE.EdgesGeometry(km.geometry, 1), new THREE.LineBasicMaterial({color: 0xff3b4b, transparent: true, opacity: .55}))); kill.add(km);
  }
}
// Stage art visibility: hidden chunks ('c:<n>') and hidden worlds ('w:<reference id>'), editor-only (the game is not
// changed). P worlds (distant stand-ins) stay hidden unless shown; backdrop chunks follow the Sky mode.
const hiddenArt = new Set(); let showStandins = false, skyWant = new Set();
function artVisible(m) { const u = m.userData; if (hiddenArt.has('c:' + u.chunk) || hiddenArt.has('w:' + u.art)) return false;
  if (visChunks && !u.backdrop && !visChunks.has(u.chunk)) return false; if (u.standin) return showStandins;
  if (u.backdrop) return skyMode === 'always' || (skyMode === 'game' && skyWant.has(u.chunk)); return true; }
function applyArt() { for (const m of art.children) m.visible = artVisible(m); }
const hiddenByYou = () => art.children.filter(m => hiddenArt.has('c:' + m.userData.chunk) || hiddenArt.has('w:' + m.userData.art)).length;   // pieces hidden by the user (not by the sky / visibility modes)
function showAllArt() { if (!hiddenArt.size) return; snapshot(); hiddenArt.clear(); applyArt(); changed(); renderLens(); renderIssueBadge(); if (['layers', 'issues'].includes(panel)) renderPanel(); toast('All stage art shown'); }
// Sky layers "as in game": the nearest Trigger Skybox (0059, DrawMode 1) picks its backdrop chunks (Chunk_1..4)
let skyActive = null, skyAt = 0;
// #53 Visibility: all chunks / as in game (the chunk the camera is in + the chunks the stage's BDT lists as visible
// from it: /api/stage chunks[].visible) / distance (chunks whose bounds are within N units of the camera).
// A chunk without a list (new chunks, or before the server sends them) falls back to distance. Backdrop chunks follow
// the Sky chip unless the list names them; P stand-ins follow the same chunk rule. Saved in project.editor.visibility.
let visMode = 'all', visDist = 12000, visChunks = null, visAt = 0, visInfo = '';
function boundsDist(c, p) { let s = 0; for (let i = 0; i < 3; i++) { const v = Math.max(0, c.bounds[0][i] - p[i], p[i] - c.bounds[1][i]); s += v * v; } return Math.sqrt(s); }
// As in game (#53, proven rule): the game draws a chunk only while the player / camera is inside one of
// that chunk's BDT boxes (native.chunks[].boxes = [[lo], [hi]] per box). So: every chunk with a box containing the
// camera; in no box -> the nearest chunk by box distance (never an empty view); chunks without boxes (new chunks, old
// imports) -> the distance rule. Backdrop chunks follow the Sky chip. The chunk is shown in the hint line.
const inBox = (c, p) => (c.boxes || []).some(([lo, hi]) => p.every((v, i) => v >= lo[i] && v <= hi[i]));
const boxDist = (c, p) => Math.min(...(c.boxes?.length ? c.boxes : [c.bounds]).map(b => boundsDist({bounds: b}, p)));
function updateVisibility(now, force = false) {
  if (!project || (!force && now - visAt < 250)) return; visAt = now;
  let next = null; const chunks = (project.native?.chunks || []).filter(c => c.bounds || c.boxes?.length), p = camera.position.toArray();
  const solid = chunks.filter(c => !c.backdrop), boxed = solid.filter(c => c.boxes?.length);
  const drawn = new Set(art.children.map(m => m.userData.chunk)), pool = (boxed.length ? boxed : solid).filter(c => drawn.has(c.number));   // nearest = a chunk that has art
  const inside = boxed.filter(c => inBox(c, p)), nearest = (inside.some(c => drawn.has(c.number)) ? null : pool.reduce((a, c) => !a || boxDist(c, p) < boxDist(a, p) ? c : a, null));
  const here = [...inside.map(c => c.number), ...(nearest ? [nearest.number] : [])];
  if (visMode !== 'all' && chunks.length) {
    if (visMode === 'game' && boxed.length) {
      next = new Set(here); for (const c of solid) if (!c.boxes?.length && c.bounds && boundsDist(c, p) <= visDist) next.add(c.number);
      visInfo = (inside.length ? `camera inside the boxes of chunk${inside.length > 1 ? 's' : ''} ${inside.map(c => c.number).join(', ')}` : 'camera in no chunk box') + (nearest ? ` · nearest chunk with art ${nearest.number} added` : '') + ` · ${next.size} of ${solid.length} drawn`;
    } else { next = new Set(solid.filter(c => c.bounds && boundsDist(c, p) <= visDist).map(c => c.number)); for (const n of here) next.add(n); visInfo = `${visMode === 'game' ? 'no BDT boxes in this project: by distance · ' : ''}${next.size} of ${solid.length} chunks within ${visDist} units`; }
  } else visInfo = 'every chunk drawn';
  const hint = $('hintChunk'); if (hint) hint.textContent = here.length ? `${inside.length ? 'camera in chunk' : 'nearest chunk'} ${here.join(', ')}` : '';
  const key = next ? [...next].sort((a, b) => a - b).join(',') : '';
  const pill = $('visPill'); if (pill) pill.title = pill.title.replace(/now: .*$/, 'now: ' + visInfo);
  if (key === updateVisibility.key && !force) return; updateVisibility.key = key; visChunks = next; applyArt();
}
function updateSkybox(now) {
  if (!project || now - skyAt < 250) return; skyAt = now;
  const back = (project.native?.chunks || []).filter(c => c.backdrop).map(c => c.number); if (!back.length) return;
  let best = null, bd = Infinity;
  for (const o of project.objects) { if (o.kind !== 'native' || o.list !== 0 || o.type !== 0x59 || (paramWord(o, 1) ?? 0) !== 1) continue; const d = camera.position.distanceToSquared(_p.set(...o.position)); if (d < bd) { bd = d; best = o; } }
  const want = new Set(best ? [3, 4, 5, 6].map(i => paramWord(best, i)).filter(n => back.includes(n)) : []); if (!want.size) want.add(Math.min(...back));
  const key = [...want].sort().join(','); if (key === skyActive) return; skyActive = key; skyWant = want; applyArt();
}
const markerGeo = new THREE.OctahedronGeometry(7, 0); let instanced = [];
// #63: each placement is built on its own; one that fails (bad model / material / position) is drawn as a marker and
// the toast names the categories and the first reason, so a whole category never silently disappears
function buildNodes() {
  clear(nodes); nodeOf.clear(); const failed = new Map();
  for (const it of items) { try { addNode(it); } catch (e) { const n = nodeOf.get(it); if (n) { nodes.remove(n); nodeOf.delete(it); } const f = failed.get(it.cat) || {n: 0, why: e.message || String(e), name: it.name}; f.n++; failed.set(it.cat, f); } }
  buildMarkers(); buildSurfColl(); buildStageCode();
  if (failed.size) {
    const text = [...failed].map(([c, f]) => `${f.n} ${CAT[c]?.label || c}`).join(', '), [first] = [...failed.values()];
    lifeEvent('build-failed', `${text}: ${first.name}: ${first.why}`);
    setTimeout(() => toast(`Could not build ${text} as models (${first.name}: ${first.why}) - shown as markers`, true), 60);
  }
  return failed;
}
function addNode(it) {   // natives need the stage's models; authored geometry draws at once
  if (!models && it.o.kind !== 'geometry' && it.o.kind !== 'path' && !LIGHT_KINDS[it.o.kind]) return null; const n = LIGHT_KINDS[it.o.kind] ? lightingNode(it.o) : it.o.kind === 'stageobject' ? stageObjectNode(it.o) : objectNode(it.o);
  if (!n) { if (isForeign(it.o) && models) { if (!it.classTried) { it.classTried = true; classModels(it.o.list, it.o.type).then(ok => ok ? rebuildNode(it) : loadForeign(it.o.sourceStage)); } else loadForeign(it.o.sourceStage); } return null; }
  if (!it.o.position.every(Number.isFinite)) throw new Error('position is not a number');
  n.position.set(...it.o.position); n.rotation.order = 'YXZ'; n.rotation.set(...(it.o.rotation || [0, 0, 0]).map(THREE.MathUtils.degToRad));
  if (it.o.kind !== 'native' && it.o.scale) n.scale.set(...it.o.scale);
  if (it.o.kind === 'lightregion') n.rotation.set(0, 0, 0);
  if (it.o.kind === 'stageobject') { const r = routePoints(it.o, it.o.startRoute ?? 0); n.rotation.set(0, r.length > 1 ? Math.atan2(r[1].x - r[0].x, r[1].z - r[0].z) : 0, 0); }   // regions are boxes along the world axes (the rule's bounds)
  if (it.o.kind === 'native' && it.o.list === 3 && it.o.type === 234) n.add(laserMarker(it.o));   // City Laser: drawn as a beam + detect radius
  n.traverse(c => c.userData.item = it); nodes.add(n); nodeOf.set(it, n); return n;
}
function buildMarkers() {
  clear(markers); instanced = [];
  for (const [cat] of CATS) {
    const list = items.filter(it => it.cat === cat && !nodeOf.has(it)); if (!list.length) continue;
    const mesh = new THREE.InstancedMesh(markerGeo, new THREE.MeshStandardMaterial({color: CAT[cat].colour, emissive: CAT[cat].colour, emissiveIntensity: .35, roughness: .5}), list.length);
    mesh.frustumCulled = false;   // #63: one marker far away (or a bad position) must not cull the whole category's mesh
    mesh.userData.items = list; markers.add(mesh); instanced.push(mesh);
  }
  updateMarkers();
}
const _m = new THREE.Matrix4(), _s = new THREE.Vector3();
// #61: the outliner search, hidden categories / layers / zones filter the 3D view too; the selected placement is always
// drawn (a placed object that a filter hid looked like "no model"), and the lens shows a Filter pill while one is active
const shown = it => it === selected || multi.has(it) || (visible(it) && matches(it));
// multi-selection (Ctrl-click in the viewport / outliner, Shift-drag a box in the viewport): `selected` stays the
// primary (last added); `multi` holds all members while there are two or more
const multi = new Set();
const selection = () => multi.size ? [...multi] : selected ? [selected] : [];
function setSelection(list) { multi.clear(); if (list.length > 1) for (const it of list) multi.add(it); selected = list.at(-1) || null; pathPoint = null; wpSel = null; arcSel = null; if (artSel) { artSel = null; artBox.visible = false; } updateMarkers(); attachGizmo(); renderInspector(); drawTree(); drawFlow(); revealRow(selected); }
// #66: the selected placement's outliner row: its group opens, the row scrolls into view and flashes
function revealRow(it) {
  if (!it || panel !== 'outliner' || !$('tree') || !matches(it)) return;
  const [k] = groupKey(it); if (!openGroups.has(k)) { openGroups.add(k); drawTree(); }
  const row = [...$('tree').querySelectorAll('.row')].find(r => r.dataset.id === it.id); if (!row) return;
  row.scrollIntoView({block: 'nearest'}); row.classList.remove('flash'); void row.offsetWidth; row.classList.add('flash');
}
const filtering = () => !!query || hiddenLayers.size > 0 || hiddenZones.size > 0 || [...hiddenCats].some(c => c !== 'volumes');
function clearFilters() { query = ''; hiddenLayers.clear(); hiddenZones.clear(); for (const c of [...hiddenCats]) if (c !== 'volumes') hiddenCats.delete(c); const q = $('q'); if (q) q.value = ''; refreshVisibility(); }
function updateMarkers() {
  if (stageCode) stageCode.visible = !hiddenCats.has('stagecode');
  for (const [it, n] of nodeOf) n.visible = shown(it);
  for (const mesh of instanced) {
    mesh.userData.items.forEach((it, i) => {
      const show = shown(it); _s.setScalar(show ? (it === selected || multi.has(it) ? 1.8 : 1) : 0);
      _m.compose(_p.set(...it.at).setY(it.at[1] + 10), new THREE.Quaternion(), _s); mesh.setMatrixAt(i, _m);
    });
    mesh.instanceMatrix.needsUpdate = true; mesh.computeBoundingSphere();
  }
  clear(selRing);
  if (multi.size) for (const it of [...multi].slice(0, 200)) {   // every member: a box (and the primary its ring below)
    const n = nodeOf.get(it); if (n) selRing.add(new THREE.BoxHelper(n, it === selected ? 0xff4d6d : 0xff8fa3));
    else { const r = new THREE.Mesh(new THREE.TorusGeometry(10, 1, 6, 24), new THREE.MeshBasicMaterial({color: 0xff8fa3})); r.rotation.x = Math.PI / 2; r.position.set(...it.at).setY(it.at[1] + 1); selRing.add(r); }
  }
  if (selected) {
    const n = nodeOf.get(selected);
    if (n) { const box = new THREE.BoxHelper(n, 0xff4d6d); selRing.add(box); }
    const ring = new THREE.Mesh(new THREE.TorusGeometry(14, 1.2, 8, 40), new THREE.MeshBasicMaterial({color: 0xff4d6d}));
    ring.rotation.x = Math.PI / 2; ring.position.set(...selected.at).setY(selected.at[1] + 1); selRing.add(ring);
  }
}
function resize() { const w = viewport.clientWidth, h = viewport.clientHeight; renderer.setSize(w, h, false); camera.aspect = w / Math.max(h, 1); camera.updateProjectionMatrix(); drawFlow(); }
new ResizeObserver(resize).observe(viewport);
let fly = null, speed = 1200; const keys = new Set(); let look = null;
addEventListener('keydown', e => { if (logic.active() || /INPUT|SELECT|TEXTAREA/.test(document.activeElement?.tagName) || e.ctrlKey || e.metaKey) return; keys.add(e.key.toLowerCase()); if (e.key === '[' || e.key === ']') { speed = THREE.MathUtils.clamp(speed * (e.key === ']' ? 1.5 : 1 / 1.5), 5, 50000); renderSpeed(); } });
addEventListener('keyup', e => keys.delete(e.key.toLowerCase())); addEventListener('blur', () => { keys.clear(); endLook(); });
function endLook() {}
function renderSpeed() { $('viewChips').innerHTML = `<span class="pill ${animateTextures ? 'on' : 'off'}" id="animPill" title="Animate textures: UV-animated materials (sand falls, rivers, glows) scroll as in game (direction as decoded, #33)">Animate textures</span><span class="pill on" title="W A S D move, Q / E down / up, Shift faster; [ and ] change the speed">Fly ${Math.round(speed)}</span><span class="pill on" id="skyPill" title="Backdrop art (the stage's far / sky shells): off (default) / all / as in game (only the chunks the nearest Trigger Skybox forces on). Shown, it is drawn as a distant sky: behind everything, from both sides, never in front of the stage; still selectable (clicks try the stage first)">Backdrop art: ${{game: 'as in game', always: 'all', hidden: 'off'}[skyMode]}</span>`; $('animPill').onclick = () => { animateTextures = !animateTextures; renderSpeed(); }; $('skyPill').onclick = () => { skyMode = {hidden: 'always', always: 'game', game: 'hidden'}[skyMode]; try { localStorage.setItem('ui2.backdrop', skyMode); } catch {} skyActive = null; updateSkybox(1e9); renderSpeed(); };
  $('viewChips').insertAdjacentHTML('beforeend', `<span class="pill ${visMode === 'all' ? 'off' : 'on'}" id="visPill" title="Which chunks are drawn: all / as in game (the chunks whose BDT box contains the camera, as the game does) / by distance from the camera · now: ${esc(visInfo)}">View: ${{all: 'all chunks', game: 'as in game', distance: 'by distance'}[visMode]}</span>${visMode !== 'all' ? `<span class="pill on visRange" title="Distance for the distance mode (and for chunks without a visibility list)"><input type="range" id="visDist" min="1000" max="60000" step="1000" value="${visDist}"><small>${visDist}</small></span>` : ''}`);
  $('visPill').onclick = () => { visMode = {all: 'game', game: 'distance', distance: 'all'}[visMode]; updateVisibility(0, true); renderSpeed(); toast('View: ' + visInfo); };
  $('visDist')?.addEventListener('input', e => { visDist = +e.target.value; e.target.nextElementSibling.textContent = visDist; updateVisibility(0, true); }); }
// #64: a saved camera is used only if it is numbers, not both at one point, and its target is in or near the stage
// (the trackA harness had its target 650 under the floor plane, 2300 away: the wheel dived under the floor and looking
// around a plain plane looked as if nothing moved)
function badCamera(cam) {
  const num = a => Array.isArray(a) && a.length === 3 && a.every(Number.isFinite);
  if (!num(cam.position) || !num(cam.target)) return 'not numbers';
  const p = new THREE.Vector3(...cam.position), t = new THREE.Vector3(...cam.target), d = p.distanceTo(t);
  if (d < 1) return 'position and target are the same point'; if (d > 60000) return `${Math.round(d)} units from its target`;
  const box = new THREE.Box3(); for (const it of items) if (it.at.every(Number.isFinite)) box.expandByPoint(_p.set(...it.at));
  if (art.children.length) box.union(new THREE.Box3().setFromObject(art));
  if (box.isEmpty()) return null;
  if (!box.clone().expandByScalar(400).containsPoint(t)) return 'its target is outside the stage';
  if (!box.clone().expandByScalar(8000).containsPoint(p)) return 'it is far outside the stage';
  return null;
}
function flyTo(pos, dist = 260) {
  if (!camera.position.toArray().every(Number.isFinite) || !orbit.target.toArray().every(Number.isFinite)) { camera.position.set(0, 600, 600); orbit.target.set(0, 0, 0); }
  const target = new THREE.Vector3(...pos), dir = camera.position.clone().sub(orbit.target).normalize();
  if (!(dir.lengthSq() > .5)) dir.set(0, .45, 1);   // camera on its target: a default view direction
  if (dir.y < .25) dir.y = .45; dir.normalize();
  fly = {t0: performance.now(), fromT: orbit.target.clone(), fromP: camera.position.clone(), toT: target, toP: target.clone().addScaledVector(dir, dist)};
}
let animateTextures = true;   // #33 sand falls / rivers scroll (Animate chip)
// Lighting preview (lens chip): the rules of the regions containing the editor camera, lowest priority first (later
// ones override the keys they set, like the runtime's stable sort), drive the hemisphere (sky / ground), the sun
// (tint x strength, azimuth / elevation) and the fog (linear from fog_start to where exp fog reaches ~90%). Point
// lights become three.js point lights (no decay inside their radius). Approximate: the game's own stage profile and
// sky textures are not read, so outside every region the editor's normal light is shown.
let showLightPreview = false; const lightPreview = {key: null, at: 0, region: ''}, previewLights = new THREE.Group(); scene.add(previewLights);
const LIGHT_DEFAULT = {hemiSky: hemi.color.clone(), hemiGround: hemi.groundColor.clone(), hemiI: hemi.intensity, sunC: sun.color.clone(), sunI: sun.intensity, sunP: sunDir.clone(), fog: scene.fog};
// ---- In-game look (lens chip; per browser, never saved in the project): an approximation of the game's atmosphere
// from GET /api/atmosphere (meanings decoded from the files, not proven in game). Sky = the stage's backdrop
// worlds centred on the camera, drawn first, never culled, without fog; Fog = the linear fog entry of the chunk the
// camera is in (background = its colour); Sun = the stage light (set 0 light 0: ambient + directional colour, angles a =
// elevation, b = azimuth - order not proven); Shadows = a soft shadow map from that sun around the view. Lighting
// regions of the project (L4) the camera is in apply on top. Not the engine's own shadows / AO / GI: Playtest shows those.
const LOOK_KEY = 'ui2.inGameLook', LOOK_DEFAULT = {on: false, sky: true, fog: true, sun: true, shadows: false};
let gameLook = (() => { try { return {...LOOK_DEFAULT, ...JSON.parse(localStorage.getItem(LOOK_KEY) || '{}')}; } catch { return {...LOOK_DEFAULT}; } })();
const saveLook = () => { try { localStorage.setItem(LOOK_KEY, JSON.stringify(gameLook)); } catch {} };
let atmos = null, atmosFor = null, lookInfo = '';
async function loadAtmos() {
  if (!project || atmosFor === project.stage) return; atmosFor = project.stage; atmos = null;
  try { atmos = await api('/api/atmosphere?stage=' + project.stage); } catch (e) { atmos = {error: e.message}; }
  atmosOwn = atmos; applyStageFilesAtmos();
  lightPreview.key = null; renderLens(); if (panel === 'settings') { const el = $('autoLight'); if (el) { el.innerHTML = autoLightCard(); wireAutoLight(el); } }
}
function cameraChunk() {   // the chunk whose BDT box holds the camera, else the nearest one
  const p = camera.position.toArray(), chunks = (project?.native?.chunks || []).filter(c => !c.backdrop && (c.boxes?.length || c.bounds));
  const inside = chunks.find(c => inBox(c, p)); if (inside) return inside.number;
  let best = null, bd = Infinity; for (const c of chunks) { const d = boxDist(c, p); if (d < bd) { bd = d; best = c; } } return best?.number ?? null;
}
const fogColour = e => new THREE.Color('#' + e.colour.map(v => (v | 0).toString(16).padStart(2, '0')).join(''));   // file bytes are sRGB
let shadowCount = -1;
function applyShadows(on) {
  const was = renderer.shadowMap.enabled; renderer.shadowMap.enabled = on; renderer.shadowMap.type = THREE.PCFSoftShadowMap; sun.castShadow = on;
  if (on) { sun.shadow.mapSize.set(2048, 2048); const c = sun.shadow.camera; c.left = c.bottom = -1600; c.right = c.top = 1600; c.near = 10; c.far = 14000; c.updateProjectionMatrix(); sun.shadow.bias = -.0004; sun.shadow.normalBias = 1.5; sun.shadow.radius = 3; }
  const look = project ? lookOf() : null, chars = new Set(); if (look) for (const it of items) if (it.o.kind === 'native' && (it.cat === 'enemies' || /partner|npc|chao|researcher|soldier|robot/i.test(it.cls || ''))) { const nd = nodeOf.get(it); if (nd) chars.add(nd); }
  let n = 0; for (const g of [art, nodes]) for (const top of g.children) { const ch = chars.has(top); top.traverse(o => { if (!o.isMesh) return; n++; const bd = o.parent?.userData?.backdrop || o.userData.backdrop;
    o.castShadow = on && !bd && (!ch || !!look?.characters_cast_shadows); o.receiveShadow = on && !bd && !(ch && look?.world_only !== false); if (was !== on) for (const m of [o.material].flat()) if (m) m.needsUpdate = true; }); }   // World shading only: characters keep their own shading
  shadowCount = on ? n : -1;
}
function skyFollow(now) {   // backdrop worlds centred on the camera (and never culled / fogged) while Sky is on
  const on = gameLook.on && gameLook.sky;
  for (const g of art.children) {
    if (!g.userData.backdrop) continue;
    if (g.userData.skyOn !== on) { g.userData.skyOn = on; g.traverse(o => { if (!o.isMesh) return; o.frustumCulled = false;   // backdrop art is never culled (#74)
      for (const m of [o.material].flat()) if (m && m.fog !== !on) { m.fog = !on; m.needsUpdate = true; } }); if (!on) applyArtEdits(); }
    if (!on) continue;
    if (!g.userData.skyCentre) { const w = atmos?.sky?.worlds?.find(x => x.chunk === g.userData.chunk && x.world === g.userData.world); g.userData.skyCentre = w?.centre ? new THREE.Vector3(...w.centre) : new THREE.Box3().setFromObject(g).getCenter(new THREE.Vector3()); }
    const off = camera.position.clone().sub(g.userData.skyCentre); g.matrixAutoUpdate = false; g.matrix.makeTranslation(off.x, off.y, off.z); g.matrixWorldNeedsUpdate = true;
  }
}
function updateLightPreview(now) {
  if (now - lightPreview.at < 250) return; lightPreview.at = now;
  const lookOn = !!(gameLook.on && project), regionsOn = showLightPreview || lookOn;
  const p = camera.position.toArray(), regions = regionsOn && project ? (project.lighting?.regions || []).filter(o => { const b = regionBox(o); return b && !o.trigger && p.every((v, i) => v >= b[0][i] && v <= b[1][i]); }).sort((a, b) => (a.priority ?? 0) - (b.priority ?? 0)) : [];
  const chunk = lookOn ? cameraChunk() : null, fogE = lookOn && gameLook.fog && atmos?.fog ? atmos.fog.entries?.find(e => e.index === atmos.fog.byChunk?.[String(chunk)]) || null : null, main = lookOn && gameLook.sun ? atmos?.light?.main || null : null;
  if (lookOn && gameLook.shadows && art.children.length + nodes.children.length !== shadowCount) applyShadows(true);
  const key = showLightPreview + '|' + JSON.stringify(gameLook) + '|' + chunk + '|' + !!atmos + '|' + JSON.stringify(project?.lighting?.auto || null) + '|' + JSON.stringify(project?.lighting?.look || null) + JSON.stringify(project?.lighting?.options || null) + '|' + regions.map(r => r.id + JSON.stringify(r.profile)).join(',') + '|' + JSON.stringify(project?.lighting?.lights || []);
  if (key === lightPreview.key) return; lightPreview.key = key;
  if (renderer.shadowMap.enabled !== !!(lookOn && gameLook.shadows)) applyShadows(!!(lookOn && gameLook.shadows));
  const auto = project?.lighting?.auto, ov = auto?.overrides || {}, lkv = lookOn && atmos?.generated?.looks?.[lookPresetName()], lkBase = lkv ? Object.fromEntries(['sun_tint', 'sun_strength', 'sky', 'ground', 'sky_strength'].filter(k => lkv[k] !== undefined).map(k => [k, lkv[k]])) : {};
  const gen = lookOn && atmos?.generated?.rule && auto?.enabled !== false ? {...atmos.generated.rule, ...lkBase, ...ov} : null;   // the stage's look (softer sun, its ambient) under the overrides
  if (gen && !Object.keys(ov).some(k => k.startsWith('fog'))) { delete gen.fog_colour; delete gen.fog_density; delete gen.fog_start; }   // the per-chunk fog (.fog) stays the base unless fog is set here
  const prof = Object.assign({}, gen || {}, ...regions.map(r => r.profile || {})), col = (v, k = 1) => new THREE.Color(v[0] * k, v[1] * k, v[2] * k);
  const D = {...LIGHT_DEFAULT};   // the base: the editor's light, or the stage's own (In-game look)
  if (main) {
    const amb = main.ambient || [.3, .3, .3], luma = .3 * amb[0] + .59 * amb[1] + .11 * amb[2], cc = main.colour || [1, 1, 1];
    D.hemiSky = D.hemiGround = new THREE.Color(...amb.slice(0, 3)).multiplyScalar(1 / Math.max(.05, Math.max(...amb.slice(0, 3)))); D.hemiI = LIGHT_DEFAULT.hemiI * THREE.MathUtils.clamp(luma * 4, .25, 2);
    D.sunC = new THREE.Color(...cc.slice(0, 3)); D.sunI = LIGHT_DEFAULT.sunI * 1.2;
    const el = THREE.MathUtils.degToRad(main.angles?.[0] ?? 45), az = THREE.MathUtils.degToRad(main.angles?.[1] ?? 0);
    D.sunP = new THREE.Vector3(Math.cos(el) * Math.sin(az), Math.max(.15, Math.abs(Math.sin(el))), Math.cos(el) * Math.cos(az)).normalize();   // tuned by eye: light from below never
  }
  if (lookOn && gameLook.fog) D.fog = fogE?.enabled !== false && fogE ? new THREE.Fog(fogColour(fogE), Math.max(0, fogE.start), Math.max(fogE.start + 1, fogE.end)) : null;
  scene.background = lookOn && (gameLook.sky || gameLook.fog) ? (fogE ? fogColour(fogE) : new THREE.Color(0x05060a)) : null;
  lookInfo = lookOn ? [atmos?.error ? 'no /api/atmosphere: ' + atmos.error : !atmos ? 'loading the stage atmosphere…' : '', gameLook.fog ? (fogE ? `fog #${fogE.colour.map(v => v.toString(16).padStart(2, '0')).join('')} ${Math.round(fogE.start)}..${Math.round(fogE.end)} (chunk ${chunk})` : `no fog entry for chunk ${chunk}`) : '', gameLook.sun ? (main ? `stage light ${(main.angles || []).join(' / ')}` : 'no stage light file') : '', gameLook.sky ? `sky: ${(atmos?.sky?.worlds || []).length} backdrop world${(atmos?.sky?.worlds || []).length === 1 ? '' : 's'}` : ''].filter(Boolean).join(' · ') : '';
  hemi.color.copy(prof.sky ? col(prof.sky) : D.hemiSky); hemi.groundColor.copy(prof.ground ? col(prof.ground) : D.hemiGround); hemi.intensity = D.hemiI * (prof.sky_strength ?? 1);
  sun.color.copy(prof.sun_tint ? col(prof.sun_tint) : D.sunC); sun.intensity = D.sunI * (prof.sun_strength ?? 1);
  if (prof.sun_azimuth !== undefined || prof.sun_elevation !== undefined) { const az = THREE.MathUtils.degToRad(prof.sun_azimuth ?? 225), el = THREE.MathUtils.degToRad(prof.sun_elevation ?? 45); sunDir.set(Math.cos(el) * Math.sin(az), Math.sin(el), Math.cos(el) * Math.cos(az)); } else sunDir.copy(D.sunP);
  if (prof.fog_density > 0 || prof.fog_colour) { const d = prof.fog_density ?? .0003, start = prof.fog_start ?? 0; scene.fog = new THREE.Fog(prof.fog_colour ? col(prof.fog_colour) : new THREE.Color(0x0a0c12), start, start + 2.3 / Math.max(d, 1e-6)); } else scene.fog = D.fog;
  clear(previewLights);
  if (showLightPreview) for (const o of (project?.lighting?.lights || []).slice(0, 16)) { const l = new THREE.PointLight(new THREE.Color(...(o.colour || [1, 1, 1])), o.intensity ?? 2, Math.max(1, o.radius || 300), 0); l.position.set(...o.position); previewLights.add(l); }
  const look = lookOn ? lookOf() : null;   // the stage's Look (approximate): exposure / ambient scale the lights, the shadow tint colours the ambient, saturation filters the view
  if (look) { const t = new THREE.Color(...look.shadow_tint.map(x => Math.min(1, x))), mix = new THREE.Color(1, 1, 1).lerp(t, look.shadow_strength);
    if (look.replace) hemi.intensity *= look.exposure; else hemi.intensity = Math.max(hemi.intensity, LIGHT_DEFAULT.hemiI * .9);   // overlay (default): the game's own brightness stays the base (its baked colours); exposure is the real-time sun only
    hemi.color.multiply(mix); hemi.groundColor.multiply(mix); sun.intensity *= look.exposure; if ('intensity' in sun.shadow) sun.shadow.intensity = look.shadow_strength * (1 - .5 * Math.min(2, (look.shadow_tint[0] + look.shadow_tint[1] + look.shadow_tint[2]) / 3)); }   // the runtime's Look(): shaded = lit x (1 - strength x (1 - tint / 2))
  else if ('intensity' in sun.shadow) sun.shadow.intensity = 1;
  renderer.domElement.style.filter = look && Math.abs(look.saturation - 1) > .005 ? `saturate(${look.saturation})` : '';
  if (renderer.shadowMap.enabled && shadowCount === -2) applyShadows(true);   // the shadow tickboxes changed
  const name = regions.map(r => r.name).join(' + '); if (name !== lightPreview.region || lookInfo !== lightPreview.info) { lightPreview.region = name; lightPreview.info = lookInfo; renderLens(); }
}
function tick(now) {
  requestAnimationFrame(tick); if (animateTextures) animateUV(now / 1000);
  if (tileGhost.visible) { if (!placing?.tilePiece) tileGhost.visible = false; else tileGhost.rotation.y = THREE.MathUtils.degToRad(tileYaw()); }   // #128 ghost
  if (fly) { const k = Math.min(1, (now - fly.t0) / 450), e = 1 - Math.pow(1 - k, 3); orbit.target.lerpVectors(fly.fromT, fly.toT, e); camera.position.lerpVectors(fly.fromP, fly.toP, e); if (k >= 1) fly = null; }
  const dt = Math.min((now - (tick.last || now)) / 1000, .1); tick.last = now;
  const v = new THREE.Vector3((keys.has('d') ? 1 : 0) - (keys.has('a') ? 1 : 0), (keys.has('e') ? 1 : 0) - (keys.has('q') ? 1 : 0), (keys.has('s') ? 1 : 0) - (keys.has('w') ? 1 : 0));
  if (v.lengthSq()) { fly = null; const up = v.y; v.y = 0; v.applyQuaternion(camera.quaternion); v.y += up; v.normalize().multiplyScalar(dt * speed * (keys.has('shift') ? 4 : 1)); camera.position.add(v); orbit.target.add(v); }
  orbit.update(); drawEnemyGuide(); drawSpringGuide(); drawBossLapLive(); updateSkybox(now); updateVisibility(now); updateLightPreview(now); skyFollow(now); sun.target.position.copy(orbit.target); sun.position.copy(orbit.target).addScaledVector(sunDir, 6000); art.visible = showArt; kill.visible = showKill || faceMode; coll.visible = showColl || faceMode;
  // backdrop art = a distant sky: its own pass first (layer 1), then the depth is cleared and the stage drawn over it, so a
  // shell the camera is inside (or that crosses the stage) never covers the stage
  updateShells(); const sky = showArt && art.children.some(g => (g.userData.backdrop || g.userData.shellIn) && g.visible);
  if (sky) { if (now - (tick.lights || 0) > 1000) { tick.lights = now; scene.traverse(o => { if (o.isLight) o.layers.enable(1); }); }
    renderer.autoClear = true; camera.layers.set(1); renderer.render(scene, camera); renderer.autoClear = false; renderer.clearDepth(); camera.layers.set(0); renderer.render(scene, camera); renderer.autoClear = true; }
  else renderer.render(scene, camera);
  if (now - (tick.mm || 0) > 120) { tick.mm = now; drawMinimap(); drawFlowCamera(); }
}
requestAnimationFrame(tick);
// console inspection (classic has window.workshopDebug): ui2Debug().project, .camera, .orbit ...
window.ui2Debug = () => ({project, items, selected, nodeOf, nodes: nodeOf, models: models?.models || [], camera, orbit, scene, renderer, diskSaved, render: R, rebuild: () => buildNodes(), gizmo: () => gizmo, proxy: () => proxy, selection: () => selection()});
window.workshopDebug = window.ui2Debug;   // the classic editor's name for the same inspection hook
const ray = new THREE.Raycaster(), ndc = new THREE.Vector2(); ray.layers.enableAll(); ray.params.Points.threshold = 7; ray.params.Line.threshold = 3; let down = null;   // layers: backdrop art lives on layer 1 (the sky pass)
renderer.domElement.addEventListener('pointerdown', e => down = [e.clientX, e.clientY]);
// double-click: fly to the spot under the cursor (stage art or an object)
renderer.domElement.addEventListener('dblclick', e => {
  const r = renderer.domElement.getBoundingClientRect(); ndc.set((e.clientX - r.left) / r.width * 2 - 1, -(e.clientY - r.top) / r.height * 2 + 1); ray.setFromCamera(ndc, camera);
  const hit = ray.intersectObjects([...art.children.filter(m => m.visible && !m.userData.backdrop && !m.userData.shellIn), ...nodes.children.filter(n => n.visible), ...instanced], true)[0];
  const it = hit && (hit.object.isInstancedMesh ? hit.object.userData.items[hit.instanceId] : (() => { for (let n = hit.object; n; n = n.parent) if (n.userData.item) return n.userData.item; })());
  if (it) select(it, true); else if (hit) flyTo(hit.point.toArray(), Math.min(400, Math.max(120, hit.distance * .35)));
});
renderer.domElement.addEventListener('pointerup', e => {
  if (!down || Math.hypot(e.clientX - down[0], e.clientY - down[1]) > 4 || e.button !== 0) return;
  if (drawing) return drawClick(e.clientX, e.clientY);
  if (faceMode && pickFace(e.clientX, e.clientY)) return;
  if (surfMode) { pickSurface(e.clientX, e.clientY, e.altKey); return; }
  if (placing) { place(placing, surfacePoint(e.clientX, e.clientY)); if (!e.shiftKey && !placingKeep) { placing = null; renderer.domElement.style.cursor = ''; if (panel === 'add') renderPanel(); } else if (panel === 'add') renderAdd.refresh?.(); return; }
  if (gizmo.dragging || gizmo.axis) return;
  const r = renderer.domElement.getBoundingClientRect(); ndc.set((e.clientX - r.left) / r.width * 2 - 1, -(e.clientY - r.top) / r.height * 2 + 1);
  ray.setFromCamera(ndc, camera);
  const raw = ray.intersectObjects([...nodes.children, ...instanced], true).filter(h => { for (let n = h.object; n; n = n.parent) if (!n.visible) return false; return true; });
  const top = raw[0]?.object.userData.item, ph = raw.find(h => h.object.userData.pathPoints && (h.object.userData.item === top || h.object.userData.item === selected));   // a control point of the path under the cursor
  if (ph) { select(ph.object.userData.item, false); return setPathPoint(ph.index); }
  const hits = raw.map(h => h.object.isInstancedMesh ? h.object.userData.items[h.instanceId] : h.object.userData.item).filter(Boolean);
  const it = hits.find(x => x.cat !== 'volumes') || hits[0];
  if ((it?.o.kind === 'geometry' || it?.o.kind === 'path') && it.o.group && !e.altKey && !(e.ctrlKey || e.metaKey)) {   // #119: an imported level / model moves as one (a loop kit: track + path)
    const grp = items.filter(x => (x.o.kind === 'geometry' || x.o.kind === 'path') && x.o.group === it.o.group); if (grp.length > 1) { setSelection([...grp.filter(x => x !== it), it]); return; }
  }
  if (it) return select(it, false, e.ctrlKey || e.metaKey);
  const ah = ray.intersectObjects(art.children.filter(m => m.visible && !m.userData.backdrop && !m.userData.shellIn), true)[0] || ray.intersectObjects(art.children.filter(m => m.visible && (m.userData.backdrop || m.userData.shellIn)), true)[0];   // #88 a shell around the camera too; #74: backdrop art is selectable, behind everything else
  let g = ah?.object; while (g && g.parent !== art) g = g.parent; selectArt(g || null);
});
// Shift + left-drag in the viewport: box-select every shown placement whose position is inside (Ctrl adds)
let boxSel = null; const boxEl = document.createElement('div'); boxEl.className = 'selBox'; boxEl.hidden = true; viewport.append(boxEl);
renderer.domElement.addEventListener('pointerdown', e => { if (e.button === 0 && e.shiftKey && !placing && !drawing && !gizmo.axis && !faceMode) boxSel = {x: e.clientX, y: e.clientY}; });
addEventListener('pointermove', e => { if (!boxSel) return; const r = viewport.getBoundingClientRect(); Object.assign(boxEl.style, {left: Math.min(boxSel.x, e.clientX) - r.left + 'px', top: Math.min(boxSel.y, e.clientY) - r.top + 'px', width: Math.abs(e.clientX - boxSel.x) + 'px', height: Math.abs(e.clientY - boxSel.y) + 'px'}); boxEl.hidden = Math.hypot(e.clientX - boxSel.x, e.clientY - boxSel.y) < 5; });
addEventListener('pointerup', e => {
  if (!boxSel) return; const b = boxSel; boxSel = null; boxEl.hidden = true; if (Math.hypot(e.clientX - b.x, e.clientY - b.y) < 5) return;
  const r = renderer.domElement.getBoundingClientRect(), x0 = Math.min(b.x, e.clientX), x1 = Math.max(b.x, e.clientX), y0 = Math.min(b.y, e.clientY), y1 = Math.max(b.y, e.clientY), v = new THREE.Vector3();
  camera.updateMatrixWorld(); const picked = items.filter(it => { if (!visible(it) || !matches(it)) return false; v.set(...it.at).project(camera); if (v.z > 1 || v.z < -1) return false; const sx = r.left + (v.x + 1) / 2 * r.width, sy = r.top + (1 - v.y) / 2 * r.height; return sx >= x0 && sx <= x1 && sy >= y0 && sy <= y1; });
  setSelection(e.ctrlKey || e.metaKey ? [...new Set([...selection(), ...picked])] : picked); toast(`${selection().length} selected · G / R move or rotate them together · Delete · Ctrl D · Ctrl K for align / distribute / prefab`);
});
let artSel = null; const artBox = new THREE.BoxHelper(undefined, 0x5fb0ff); artBox.visible = false; scene.add(artBox);
// #47 move / rotate / scale ORIGINAL stage art (builds it: visuals + matching collision + BDT bounds):
// project.artEdits['<art id>'] = {offset [x,y,z], rotation [rx,ry,rz] degrees (Z, then X, then Y), scale [sx,sy,sz],
// pivot [x,y,z] = the piece's box centre}; drawn as T(pivot + offset) * R * S * T(-pivot). The handles sit on a proxy at
// the pivot; the editor's collision / kill overlays are not moved (the build moves the real collision).
function artPivot(g) { if (!g.userData.pivot) { const b = new THREE.Box3(); for (const c of g.children) { c.geometry.computeBoundingBox(); b.union(c.geometry.boundingBox); } g.userData.pivot = b.getCenter(new THREE.Vector3()).toArray().map(round2); } return g.userData.pivot; }
function artMatrix(e, pivot) {
  const pv = new THREE.Vector3(...pivot), q = new THREE.Quaternion().setFromEuler(new THREE.Euler(...(e.rotation || [0, 0, 0]).map(THREE.MathUtils.degToRad), 'YXZ'));
  return new THREE.Matrix4().makeTranslation(pv.x + (e.offset?.[0] || 0), pv.y + (e.offset?.[1] || 0), pv.z + (e.offset?.[2] || 0)).multiply(new THREE.Matrix4().compose(new THREE.Vector3(), q, new THREE.Vector3(...(e.scale || [1, 1, 1])))).multiply(new THREE.Matrix4().makeTranslation(-pv.x, -pv.y, -pv.z));
}
function applyArtEdits() {
  for (const g of art.children) { const e = project.artEdits?.[g.userData.art]; g.matrixAutoUpdate = false; if (e) g.matrix.copy(artMatrix(e, e.pivot || artPivot(g))); else g.matrix.identity(); g.matrixWorldNeedsUpdate = true; }
  if (artSel) artBox.setFromObject(artSel);
}
function artProxyFromEdit(g) {
  const e = project.artEdits?.[g.userData.art] || {}, pv = e.pivot || artPivot(g);
  proxy.position.set(pv[0] + (e.offset?.[0] || 0), pv[1] + (e.offset?.[1] || 0), pv[2] + (e.offset?.[2] || 0)); proxy.rotation.set(...(e.rotation || [0, 0, 0]).map(THREE.MathUtils.degToRad), 'YXZ'); proxy.scale.set(...(e.scale || [1, 1, 1]));
}
function artEditFromProxy(g) {
  const pv = artPivot(g), r = new THREE.Euler().setFromQuaternion(proxy.quaternion, 'YXZ');
  const e = {offset: proxy.position.toArray().map((v, i) => round2(v - (project.artEdits?.[g.userData.art]?.pivot || pv)[i])), rotation: [r.x, r.y, r.z].map(v => round2(THREE.MathUtils.radToDeg(v))), scale: proxy.scale.toArray().map(v => Math.round(v * 1000) / 1000), pivot: project.artEdits?.[g.userData.art]?.pivot || pv};
  const plain = e.offset.every(v => !v) && e.rotation.every(v => !v) && e.scale.every(v => v === 1);
  if (plain) delete project.artEdits?.[g.userData.art]; else (project.artEdits ||= {})[g.userData.art] = e;
}
function selectArt(g) { artSel = g; if (g) { selected = null; updateMarkers(); attachGizmo(); drawTree(); drawFlow(); artBox.setFromObject(g); artBox.visible = true; } else artBox.visible = false; renderInspector(); }
function setArtHidden(key, hide) { snapshot(); hide ? hiddenArt.add(key) : hiddenArt.delete(key); applyArt(); changed(); renderLens(); renderIssueBadge(); if (artSel && !artSel.visible) selectArt(null); if (panel === 'layers') renderPanel(); }

// ---- minimap: top-down silhouette of the stage art + markers + camera ------------------------------------------------
const mm = $('minimap'), mmCtx = mm.getContext('2d'); let mmBase = null, mmBox = null;
// #79 the map is a small panel: drag its header to move it, its corner to resize it; on the map the wheel zooms about
// the cursor, a drag pans, a click flies there; double-click the header resets. Remembered per browser.
const MM_BASE = 512, MM_DEFAULT = {left: 12, bottom: 12, w: 190, h: 190, zoom: 1, cx: MM_BASE / 2, cy: MM_BASE / 2};
let mmView = (() => { try { return {...MM_DEFAULT, ...JSON.parse(localStorage.getItem('ui2.minimap') || '{}')}; } catch { return {...MM_DEFAULT}; } })();
const mmSave = () => { try { localStorage.setItem('ui2.minimap', JSON.stringify(mmView)); } catch {} };
const mmPanel = Object.assign(document.createElement('div'), {className: 'mmPanel', id: 'mmPanel'});
mm.before(mmPanel); mmPanel.innerHTML = '<div class="mmHead" title="Drag to move the map · double-click resets size, place and zoom">Map <small>wheel zoom · drag pan · click flies</small></div>'; mmPanel.append(mm);
const mmGrip = Object.assign(document.createElement('div'), {className: 'mmGrip', title: 'Drag to resize'}); mmPanel.append(mmGrip);
function mmLayout() {
  const vp = viewport.getBoundingClientRect(), w = Math.min(Math.max(120, mmView.w), Math.max(120, vp.width - 24)), h = Math.min(Math.max(120, mmView.h), Math.max(120, vp.height - 24));
  mmView.w = w; mmView.h = h; mmView.left = Math.min(Math.max(0, mmView.left), Math.max(0, vp.width - w)); mmView.bottom = Math.min(Math.max(0, mmView.bottom), Math.max(0, vp.height - h));
  Object.assign(mmPanel.style, {left: mmView.left + 'px', bottom: mmView.bottom + 'px', width: w + 'px', height: h + 'px'});
  const head = 20, cw = Math.round(w), ch = Math.round(h - head); if (mm.width !== cw * devicePixelRatio || mm.height !== ch * devicePixelRatio) { mm.width = cw * devicePixelRatio; mm.height = ch * devicePixelRatio; }
}
const mmK = () => Math.min(mm.width, mm.height) / MM_BASE * mmView.zoom;   // base pixels -> canvas pixels
const mmToScreen = b => [(b[0] - mmView.cx) * mmK() + mm.width / 2, (b[1] - mmView.cy) * mmK() + mm.height / 2];
const mmFromScreen = (x, y) => [(x - mm.width / 2) / mmK() + mmView.cx, (y - mm.height / 2) / mmK() + mmView.cy];
const mmEvent = e => { const r = mm.getBoundingClientRect(); return [(e.clientX - r.left) / r.width * mm.width, (e.clientY - r.top) / r.height * mm.height]; };
mm.addEventListener('wheel', e => { e.preventDefault(); const [x, y] = mmEvent(e), b = mmFromScreen(x, y); mmView.zoom = THREE.MathUtils.clamp(mmView.zoom * (e.deltaY < 0 ? 1.2 : 1 / 1.2), .5, 40); const k = mmK(); mmView.cx = b[0] - (x - mm.width / 2) / k; mmView.cy = b[1] - (y - mm.height / 2) / k; mmSave(); drawMinimap(); }, {passive: false});
let mmDrag = null;
mm.addEventListener('pointerdown', e => { if (e.button !== 0) return; mm.setPointerCapture(e.pointerId); mmDrag = {x: e.clientX, y: e.clientY, cx: mmView.cx, cy: mmView.cy, moved: false}; });
mm.addEventListener('pointermove', e => { if (!mmDrag) return; const dx = e.clientX - mmDrag.x, dy = e.clientY - mmDrag.y; if (!mmDrag.moved && Math.hypot(dx, dy) < 4) return; mmDrag.moved = true; const r = mm.getBoundingClientRect(), k = mmK() * r.width / mm.width; mmView.cx = mmDrag.cx - dx / k; mmView.cy = mmDrag.cy - dy / k; drawMinimap(); });
mm.addEventListener('pointerup', () => { if (mmDrag?.moved) { mmSave(); mm.dataset.dragged = '1'; } mmDrag = null; });
function mmPanelDrag(el, onMove) {
  el.addEventListener('pointerdown', e => { if (e.button !== 0) return; e.preventDefault(); e.stopPropagation(); el.setPointerCapture(e.pointerId); const start = {x: e.clientX, y: e.clientY, ...mmView};
    const move = ev => { onMove(ev.clientX - start.x, ev.clientY - start.y, start); mmLayout(); drawMinimap(); };
    const up = () => { el.removeEventListener('pointermove', move); el.removeEventListener('pointerup', up); mmSave(); };
    el.addEventListener('pointermove', move); el.addEventListener('pointerup', up); });
}
mmPanelDrag(mmPanel.querySelector('.mmHead'), (dx, dy, s0) => { mmView.left = s0.left + dx; mmView.bottom = s0.bottom - dy; });
mmPanelDrag(mmGrip, (dx, dy, s0) => { mmView.w = s0.w + dx; mmView.h = s0.h - dy; });   // the grip is the top-right corner (the panel sits on its bottom-left)
mmPanel.querySelector('.mmHead').addEventListener('dblclick', () => { mmView = {...MM_DEFAULT}; mmSave(); mmLayout(); drawMinimap(); });
new ResizeObserver(() => { mmLayout(); drawMinimap(); }).observe(viewport);
setTimeout(() => { mmLayout(); drawMinimap(); }, 0);
function buildMinimap() {
  const pts = []; for (const m of project.reference || []) if (!m.collision && art.children.some(c => c.userData.chunk === m.chunk)) pts.push(m);
  let min = [Infinity, Infinity], max = [-Infinity, -Infinity];
  for (const it of items) { const p = it.at; min = [Math.min(min[0], p[0]), Math.min(min[1], p[2])]; max = [Math.max(max[0], p[0]), Math.max(max[1], p[2])]; }
  const pad = 300, size = Math.max(max[0] - min[0], max[1] - min[1]) + pad * 2, cx = (min[0] + max[0]) / 2, cz = (min[1] + max[1]) / 2;
  mmBox = {x0: cx - size / 2, z0: cz - size / 2, size};
  const base = document.createElement('canvas'); base.width = base.height = MM_BASE; const g = base.getContext('2d');
  g.fillStyle = '#0b0d12'; g.fillRect(0, 0, base.width, base.height); g.fillStyle = 'rgba(160,170,195,0.10)';
  for (const m of pts) for (const t of m.triangles) {
    const [a, b, c] = t.map(i => m.vertices[i]); g.beginPath();
    g.moveTo(...toMM(a)); g.lineTo(...toMM(b)); g.lineTo(...toMM(c)); g.fill();
  }
  g.strokeStyle = '#ffffff30'; g.lineWidth = 1.2; g.beginPath(); spine.forEach((p, i) => i ? g.lineTo(...toMM([p.x, p.y, p.z])) : g.moveTo(...toMM([p.x, p.y, p.z]))); g.stroke();
  mmBase = base;
}
function toMM(p) { return [(p[0] - mmBox.x0) / mmBox.size * MM_BASE, (p[2] - mmBox.z0) / mmBox.size * MM_BASE]; }   // base-map pixels
const toMMs = p => mmToScreen(toMM(p));   // canvas pixels
function drawMinimap() {
  if (!mmBase) return; const k = mmK(), d = devicePixelRatio;
  mmCtx.setTransform(1, 0, 0, 1, 0, 0); mmCtx.fillStyle = '#0b0d12'; mmCtx.fillRect(0, 0, mm.width, mm.height);
  mmCtx.setTransform(k, 0, 0, k, mm.width / 2 - mmView.cx * k, mm.height / 2 - mmView.cy * k); mmCtx.drawImage(mmBase, 0, 0); mmCtx.setTransform(1, 0, 0, 1, 0, 0);
  for (const it of items) { if (!visible(it) || !matches(it)) continue; const [x, y] = toMMs(it.at); if (x < -4 || y < -4 || x > mm.width + 4 || y > mm.height + 4) continue; mmCtx.fillStyle = CAT[it.cat].colour; mmCtx.fillRect(x - d, y - d, 2 * d, 2 * d); }
  if (selected) { const [x, y] = toMMs(selected.at); mmCtx.strokeStyle = '#ff4d6d'; mmCtx.lineWidth = 2 * d; mmCtx.beginPath(); mmCtx.arc(x, y, 6 * d, 0, 7); mmCtx.stroke(); }
  const [cx, cy] = toMMs(camera.position.toArray()), [tx, ty] = toMMs(orbit.target.toArray());
  mmCtx.strokeStyle = '#fff'; mmCtx.lineWidth = 1.5; mmCtx.beginPath(); mmCtx.moveTo(cx, cy); mmCtx.lineTo(tx, ty); mmCtx.stroke();
  mmCtx.fillStyle = '#fff'; mmCtx.beginPath(); mmCtx.arc(cx, cy, 3.5 * d, 0, 7); mmCtx.fill();
}
mm.addEventListener('click', e => {
  if (!mmBox) return; if (mm.dataset.dragged) { delete mm.dataset.dragged; return; }   // the end of a pan, not a click
  const [sx, sy] = mmEvent(e), b = mmFromScreen(sx, sy), x = mmBox.x0 + b[0] / MM_BASE * mmBox.size, z = mmBox.z0 + b[1] / MM_BASE * mmBox.size;
  let best = null, bd = Infinity; for (const it of items) { const d = (it.o.position[0] - x) ** 2 + (it.o.position[2] - z) ** 2; if (d < bd) { bd = d; best = it; } }
  if (best) flyTo([x, best.o.position[1], z], 700);
});

// ---- stage flow strip ------------------------------------------------------------------------------------------------
const fc = $('flowCanvas'), fx = fc.getContext('2d'); let flowHits = [];
const FLOW = {left: 132, right: 18, top: 22, lane: 18};
function drawFlow() {
  const w = fc.clientWidth, h = fc.clientHeight; if (!w || !h) return;
  fc.width = w * devicePixelRatio; fc.height = h * devicePixelRatio; fx.setTransform(devicePixelRatio, 0, 0, devicePixelRatio, 0, 0);
  fx.clearRect(0, 0, w, h); flowHits = []; if (!project) return;
  const X = t => FLOW.left + t * (w - FLOW.left - FLOW.right);
  // checkpoint ticks
  fx.font = '10px Segoe UI'; fx.textAlign = 'center';
  const marks = [['START', 0], ...spine.cps.map(o => [`CP${paramWord(o, 0)}`, progressOf(o.position)]), ...(spine.goal ? [['GOAL', 1]] : [])];
  for (const [label, t] of marks) { fx.fillStyle = '#ffffff10'; fx.fillRect(X(t), FLOW.top - 6, 1, h - FLOW.top); fx.fillStyle = '#8b93a7'; fx.fillText(label, X(t), FLOW.top - 9); }
  CATS.filter(([c]) => !['paths', 'prefabs', 'lighting', 'stagecode'].includes(c)).forEach(([cat, label, colour], lane) => {
    const y = FLOW.top + lane * FLOW.lane + FLOW.lane / 2;
    fx.textAlign = 'left'; fx.fillStyle = hiddenCats.has(cat) ? '#3a4152' : '#9aa2b5'; fx.fillText(label, 12, y + 3);
    fx.fillStyle = '#ffffff06'; fx.fillRect(FLOW.left, y - FLOW.lane / 2 + 2, w - FLOW.left - FLOW.right, FLOW.lane - 4);
    for (const it of items) {
      if (it.cat !== cat || !matches(it)) continue;
      const x = X(it.progress), on = visible(it);
      fx.fillStyle = on ? colour : '#2a3040'; fx.globalAlpha = it === selected ? 1 : .8;
      fx.beginPath(); fx.arc(x, y, it === selected ? 5 : 2.6, 0, 7); fx.fill(); fx.globalAlpha = 1;
      if (it === selected) { fx.strokeStyle = '#fff'; fx.lineWidth = 1.5; fx.stroke(); }
      flowHits.push([x, y, it]);
    }
  });
  drawFlowCamera();
}
function drawFlowCamera() {
  if (!project || !fc.clientWidth) return; const w = fc.clientWidth, h = fc.clientHeight;
  const t = progressOf(orbit.target.toArray()), x = FLOW.left + t * (w - FLOW.left - FLOW.right);
  const prev = drawFlowCamera.x; drawFlowCamera.x = x; if (prev === undefined || Math.abs(prev - x) > .5) { if (prev !== undefined) { drawFlowCamera.x = undefined; drawFlow(); drawFlowCamera.x = x; } }
  fx.fillStyle = '#ff4d6d'; fx.fillRect(x - 1, 6, 2, h - 6); fx.beginPath(); fx.moveTo(x - 5, 4); fx.lineTo(x + 5, 4); fx.lineTo(x, 10); fx.fill();
}
function flowAt(e) { const r = fc.getBoundingClientRect(), x = e.clientX - r.left, y = e.clientY - r.top; let best = null, bd = 64; for (const [hx, hy, it] of flowHits) { const d = (hx - x) ** 2 + (hy - y) ** 2; if (d < bd) { bd = d; best = it; } } return [best, x, y]; }
fc.addEventListener('mousemove', e => { const [it, x, y] = flowAt(e), tip = $('flowTip'); if (!it) { tip.hidden = true; return; } tip.hidden = false; tip.textContent = `${it.name} · ${Math.round(it.progress * 100)}% · zone ${it.zone ?? '-'}`; tip.style.left = Math.min(x + 12, fc.clientWidth - 220) + 'px'; tip.style.top = (y + 20) + 'px'; });
fc.addEventListener('mouseleave', () => $('flowTip').hidden = true);
fc.addEventListener('dblclick', e => { const [it] = flowAt(e); if (it) select(it, true); });
fc.addEventListener('click', e => { const [it, x] = flowAt(e); if (it) return select(it); const w = fc.clientWidth, t = (x - FLOW.left) / (w - FLOW.left - FLOW.right); if (t < 0 || t > 1 || spine.length < 2) return; let d = t * spineLen.at(-1), i = 1; while (i < spine.length - 1 && spineLen[i] < d) i++; const k = (d - spineLen[i - 1]) / ((spineLen[i] - spineLen[i - 1]) || 1); flyTo(spine[i - 1].clone().lerp(spine[i], THREE.MathUtils.clamp(k, 0, 1)).toArray(), 700); });

// ---- lens pills (viewport overlay) -----------------------------------------------------------------------------------
function renderLens() {
  const counts = l => items.filter(it => it.layer === l).length, nShown = filtering() ? items.filter(it => visible(it) && matches(it)).length : 0;
  const grp = (cap, html) => html ? `<span class="lgrp"><i>${cap}</i>${html}</span>` : '';   // small captions: what each group of pills is for
  $('lens').innerHTML = (filtering() ? `<span class="pill warnPill" data-filter title="The outliner search${query ? ` "${esc(query)}"` : ''} or hidden categories / layers / zones are hiding placements in the 3D view and the flow strip. Click to show everything">Filter: ${nShown} of ${items.length} shown · show all</span>` : '')
    + grp('Layers', LAYERS.filter(([l]) => counts(l)).map(([l, label, help]) => `<span class="pill ${hiddenLayers.has(l) ? 'off' : 'on'}" data-layer="${l}" title="${esc(help)}">${label} <small>${counts(l)}</small></span>`).join(''))
    + grp('Show', `<span class="pill ${showArt ? 'on' : 'off'}" data-art>Stage art${hiddenByYou() ? ` <b class="cnt" data-artall title="${hiddenByYou()} stage-art pieces hidden by you - click to show them all">${hiddenByYou()} hidden</b>` : ''}</span>`
      + `<span class="pill ${showColl ? 'on' : 'off'}" data-coll title="The stage's own collision faces (faint wireframe)">Collision</span>`
      + `<span class="pill ${showKill ? 'on' : 'off'}" data-kill title="Kill planes: invisible collision sheets with surface 32 (flag 0x20), the stage's and your own. They are not solid: falling down through one kills Shadow"><span class="dot" style="background:#ff2a3a"></span>Kill planes</span>`
      + `<span class="pill ${showArcs ? 'on' : 'off'}" data-arcs title="Launch arcs: the path Shadow flies from every spring, dash ring, ramp and panel (stick untouched; approximate). The selected one always shows its arc"><span class="dot" style="background:${ARC_LAND}"></span>Launch arcs</span>`)
    + grp('Look', `<span class="pill ${gameLook.on ? 'on' : 'off'}" data-look title="In-game look (approximate, this browser only; never saved): the stage's sky, fog and light from its own files (/api/atmosphere, decoded, not proven in game) and your lighting regions. Not the engine's shadows / AO: Playtest shows those.${lookInfo ? ' Now: ' + esc(lookInfo) : ''}">In-game look</span>`
      + (gameLook.on ? ['sky', 'fog', 'sun', 'shadows'].map(k => `<span class="pill small ${gameLook[k] ? 'on' : 'off'}" data-lookk="${k}">${k[0].toUpperCase() + k.slice(1)}</span>`).join('') + `<span class="pill small off" data-look title="Back to the plain editor view">Editor view</span>` : '')
      + `<span class="pill ${showLightPreview ? 'on' : 'off'}" data-lightprev title="Lighting preview (approximate): the atmosphere of the lighting region the camera is in (hemisphere, sun, fog) and the point lights. Outside every region the editor's normal light">Lighting${showLightPreview && lightPreview.region ? ': ' + esc(lightPreview.region) : ''}</span>`)
    + grp('Collision tools', `<span class="pill ${faceMode ? 'on' : 'off'}" data-faces title="Edit collision faces: click an original collision or kill-plane face to turn it into your own invisible triangle (select it and press Delete to remove the collision there)">Edit faces</span>`
      + `<span class="pill ${surfMode ? 'on' : 'off'}" data-surf title="Collision from surface: click a flat surface of stage art, a shape or an object to make it solid (Alt-click: the whole connected piece). The list is in the inspector">Collision from surface${(project?.surfaceCollision || []).length ? ` <b class="cnt">${project.surfaceCollision.length}</b>` : ''}</span>`)
    + grp('Game', `<span class="pill ${gameView.on ? 'on' : 'off'}" data-gv title="Game view: stream this camera to the private test game (start it with Play → Game view; it opens muted and unfocused). The game does not follow it yet, so it keeps its own camera for now">Game view${gameView.on ? ': ' + esc(gameView.state) : ''}</span>`);
  $('lens').querySelectorAll('[data-layer]').forEach(p => p.onclick = () => { const l = p.dataset.layer; hiddenLayers.has(l) ? hiddenLayers.delete(l) : hiddenLayers.add(l); refreshVisibility(); });
  $('lens').querySelector('[data-filter]')?.addEventListener('click', clearFilters);
  $('lens').querySelector('[data-kill]').onclick = () => { showKill = !showKill; renderLens(); };
  $('lens').querySelector('[data-arcs]').onclick = () => setShowArcs(!showArcs);
  $('lens').querySelector('[data-art]').onclick = e => { if (e.target.closest('[data-artall]')) return showAllArt(); showArt = !showArt; renderLens(); };
  $('lens').querySelector('[data-coll]').onclick = () => { showColl = !showColl; renderLens(); };
  $('lens').querySelector('[data-gv]').onclick = () => gameView.on ? gameViewOff() : gameViewOn();
  $('lens').querySelectorAll('[data-look]').forEach(b => b.onclick = () => { gameLook.on = !gameLook.on; saveLook(); lightPreview.key = null; if (gameLook.on) loadAtmos(); renderLens(); });
  $('lens').querySelectorAll('[data-lookk]').forEach(b => b.onclick = () => { gameLook[b.dataset.lookk] = !gameLook[b.dataset.lookk]; saveLook(); lightPreview.key = null; renderLens(); });
  $('lens').querySelector('[data-lightprev]').onclick = () => { showLightPreview = !showLightPreview; lightPreview.key = null; renderLens(); };
  $('lens').querySelector('[data-faces]').onclick = () => setFaceMode(!faceMode);
  requestAnimationFrame(placeGizmoBar);
  $('lens').querySelector('[data-surf]').onclick = () => setSurfMode(!surfMode);
}
// 'Keys ?' (bottom right of the view) / the ? key: the mouse and keyboard card (replaces the long hint line)
$('keysBtn')?.addEventListener('click', e => { e.stopPropagation(); $('keysCard').hidden = !$('keysCard').hidden; });
addEventListener('keydown', e => { if (e.key === '?' && !/INPUT|SELECT|TEXTAREA/.test(document.activeElement?.tagName) && !logic.active()) $('keysCard').hidden = !$('keysCard').hidden; else if (e.key === 'Escape' && !$('keysCard').hidden) $('keysCard').hidden = true; });
addEventListener('pointerdown', e => { if (!$('keysCard').hidden && !e.target.closest('#keysCard, #keysBtn')) $('keysCard').hidden = true; });
function placeGizmoBar() {   // under the pill rows, however many there are; at the right edge when that would cover the minimap (narrow windows)
  const l = $('lens'), g = $('gizmoBar'), mm = $('minimap'), vc = $('viewChips'); if (!l || !g) return;
  l.style.maxWidth = vc && vc.offsetTop < viewport.clientHeight / 2 ? Math.max(240, viewport.clientWidth - vc.offsetWidth - 36) + 'px' : '';   // the pills wrap before the view chips on the right (UX pass: a pill hid under them)
  const top = Math.max(96, l.offsetTop + l.offsetHeight + 12), room = viewport.clientHeight - (mm ? mm.offsetHeight + 24 : 0);
  g.classList.toggle('side', top + g.offsetHeight > room); const vcTop = vc && vc.offsetTop < viewport.clientHeight / 2 ? vc.offsetTop + vc.offsetHeight + 12 : 12;   // the view chips sit at the bottom in narrow windows
  g.style.top = (g.classList.contains('side') ? vcTop : top) + 'px';
}
addEventListener('resize', () => requestAnimationFrame(placeGizmoBar));
function refreshVisibility() { updateMarkers(); renderLens(); renderPanel(); drawFlow(); }
let lensFilterKey = '';
function lensFilterCheck() { const k = filtering() + '|' + query + '|' + [...hiddenLayers, ...hiddenZones, ...hiddenCats].join(); if (k !== lensFilterKey) { lensFilterKey = k; renderLens(); } }

// ---- side panels -----------------------------------------------------------------------------------------------------
const EYE = on => `<svg viewBox="0 0 24 24">${on ? '<path d="M2 12s4-7 10-7 10 7 10 7-4 7-10 7S2 12 2 12z"/><circle cx="12" cy="12" r="3"/>' : '<path d="M3 3l18 18M10.6 5.1A9.7 9.7 0 0 1 12 5c6 0 10 7 10 7a17 17 0 0 1-3.2 3.9M6.6 6.6C3.8 8.4 2 12 2 12s4 7 10 7a9.6 9.6 0 0 0 5.4-1.6"/>'}</svg>`;
function groupKey(it) {
  if (groupBy === 'category') return ['category:' + it.cat, CAT[it.cat].label, CAT[it.cat].colour, CATS.findIndex(c => c[0] === it.cat)];
  if (groupBy === 'zone') return ['zone:' + it.zone, it.zone === null ? 'Outside zones' : `Zone ${it.zone}`, '#8b93a7', it.zone ?? 999];
  if (groupBy === 'layer') return ['layer:' + it.layer, it.layer ? (LAYERS.find(l => l[0] === it.layer)?.[1] || it.layer) + ' layer' : 'Editor objects', '#8b93a7', LAYERS.findIndex(l => l[0] === it.layer)];
  if (groupBy === 'progress') { const b = Math.min(9, Math.floor(it.progress * 10)); return ['progress:' + b, `${b * 10}-${b * 10 + 10}% of the run`, '#8b93a7', b]; }
  return ['class:' + it.cls, it.cls, CAT[it.cat].colour, it.cls];
}
function renderOutliner(body) {
  body.innerHTML = `<div class="searchBox"><svg viewBox="0 0 24 24"><circle cx="11" cy="11" r="6"/><path d="M20 20l-4-4"/></svg><input id="q" placeholder="Search · zone:3 layer:ds1 cat:enemies" value="${esc(query)}"></div>
   <div class="seg" id="groupSeg">${[['category', 'Category'], ['zone', 'Zone'], ['progress', 'Run order'], ['class', 'Class'], ['layer', 'Layer']].map(([k, l]) => `<button data-g="${k}" class="${groupBy === k ? 'on' : ''}">${l}</button>`).join('')}</div><div class="tree" id="tree"></div>`;
  const q = body.querySelector('#q'); q.oninput = () => { query = q.value; updateMarkers(); drawTree(); drawFlow(); lensFilterCheck(); }; if (renderOutliner.focus) { q.focus(); q.setSelectionRange(q.value.length, q.value.length); }
  body.querySelectorAll('[data-g]').forEach(b => b.onclick = () => { groupBy = b.dataset.g; renderPanel(); });
  drawTree();
}
function drawTree() {
  const tree = $('tree'); if (!tree) return;
  const groups = new Map();
  for (const it of items) { if (!matches(it)) continue; const [k, label, colour, order] = groupKey(it); if (!groups.has(k)) groups.set(k, {k, label, colour, order, list: []}); groups.get(k).list.push(it); }
  const sorted = [...groups.values()].sort((a, b) => typeof a.order === 'string' ? a.order.localeCompare(b.order) : a.order - b.order);
  $('panelCount').textContent = `${items.filter(matches).length} / ${items.length}`;
  tree.innerHTML = sorted.map(g => {
    const open = openGroups.has(g.k) || (query && sorted.length < 6), on = g.list.some(visible);
    const nameN = new Map(), nameI = new Map(); if (open) for (const it of g.list) nameN.set(it.name, (nameN.get(it.name) || 0) + 1);   // 'Checkpoint 1..6' in run order when a name repeats 2..20 times
    const label = it => { const n = nameN.get(it.name); if (n < 2 || n > 20) return it.name; const j = (nameI.get(it.name) || 0) + 1; nameI.set(it.name, j); return `${it.name} ${j}`; };
    const rows = open ? g.list.sort((a, b) => a.progress - b.progress).slice(0, 400).map(it => `<div class="row ${it === selected || multi.has(it) ? 'sel' : ''}" data-id="${esc(it.id)}"><span class="dot" style="background:${CAT[it.cat].colour}"></span><span class="name">${esc(label(it))}</span><span class="meta" title="zone ${it.zone ?? '-'} · ${Math.round(it.progress * 100)}% along the run">z${it.zone ?? '-'} · ${Math.round(it.progress * 100)}%</span></div>`).join('') + (g.list.length > 400 ? `<div class="sub">+${g.list.length - 400} more - refine the search</div>` : '') : '';
    const hasSel = g.list.some(it => it === selected || multi.has(it));
    return `<div class="group ${open ? 'open' : ''} ${hasSel ? 'hasSel' : ''}" data-k="${esc(g.k)}" ${hasSel && !open ? 'title="The selection is in this group"' : ''}><div class="gHead"><span class="chev">▸</span><span class="dot" style="background:${g.colour}"></span><span class="name">${esc(g.label)}</span><span class="n">${g.list.length}</span><span class="eye ${on ? '' : 'off'}" data-eye title="Show / hide">${EYE(on)}</span></div><div class="items">${rows}</div></div>`;
  }).join('') || '<div class="sub">Nothing matches.</div>';
  tree.querySelectorAll('.gHead').forEach(h => h.onclick = e => {
    const k = h.parentElement.dataset.k;
    if (e.target.closest('[data-eye]')) { toggleGroup(k); return; }
    const opening = !openGroups.has(k); opening ? openGroups.add(k) : openGroups.delete(k); drawTree();
    if (opening) { const row = [...tree.querySelectorAll(`.group[data-k="${CSS.escape(k)}"] .row.sel`)].find(r => r.dataset.id === selected?.id) || tree.querySelector(`.group[data-k="${CSS.escape(k)}"] .row.sel`); row?.scrollIntoView({block: 'nearest'}); }
  });
  tree.querySelectorAll('.row').forEach(r => { r.onclick = e => select(byId.get(r.dataset.id), false, e.ctrlKey || e.metaKey); r.ondblclick = () => select(byId.get(r.dataset.id), true); });
}
function toggleGroup(k) {
  const [kind, v] = k.split(/:(.*)/s);
  const set = kind === 'category' ? hiddenCats : kind === 'layer' ? hiddenLayers : kind === 'zone' ? hiddenZones : null;
  if (!set) { toast('Hide by category, zone or layer (switch "group by").'); return; }
  set.has(v) ? set.delete(v) : set.add(v); refreshVisibility();
}
function renderZones(body) {
  const z = [...new Set(items.map(it => it.zone))].sort((a, b) => (a ?? 999) - (b ?? 999));
  body.innerHTML = `<div class="sub">Stage areas (chunks). Each placement belongs to the smallest area that contains it. Click to fly; the switch hides an area's placements.</div><div class="cards">${z.map(n => {
    const list = items.filter(it => it.zone === n), by = CATS.map(([c, , col]) => [col, list.filter(it => it.cat === c).length]).filter(x => x[1]);
    const prog = list.length ? Math.round(list.reduce((a, it) => a + it.progress, 0) / list.length * 100) : 0;
    return `<div class="card" data-z="${n}"><h4>${n === null ? 'Outside zones' : 'Zone ' + n}<small>${list.length} placements · ~${prog}% of the run</small></h4><div class="bars">${by.map(([col, c]) => `<span style="flex:${c};background:${col}"></span>`).join('')}</div>
     <div class="legend"><span class="toggle"><span class="switch ${hiddenZones.has(String(n)) ? '' : 'on'}" data-zs="${n}"></span>shown</span></div></div>`;
  }).join('')}</div>`;
  body.querySelectorAll('[data-zs]').forEach(s => s.onclick = e => { e.stopPropagation(); const k = s.dataset.zs; hiddenZones.has(k) ? hiddenZones.delete(k) : hiddenZones.add(k); refreshVisibility(); });
  body.querySelectorAll('.card').forEach(c => c.onclick = () => { const n = c.dataset.z === 'null' ? null : +c.dataset.z; const zz = zones.find(z => z.number === n); if (zz) flyTo(zz.min.map((v, i) => (v + zz.max[i]) / 2), Math.max(800, (zz.max[0] - zz.min[0]) * .8)); });
}
function renderLayers(body) {
  body.innerHTML = `<div class="sub">Shadow's SET layers: placements live in one of these files. Common is always loaded; the others load per mission set (meaning of hrd / ds1 still being confirmed). Toggle to see a stage the way one set of files sees it.</div><div class="cards">${LAYERS.map(([l, label, help]) => {
    const list = items.filter(it => it.layer === l); if (!list.length) return '';
    const by = CATS.map(([c, , col]) => [col, list.filter(it => it.cat === c).length]).filter(x => x[1]);
    return `<div class="card"><h4><span class="toggle"><span class="switch ${hiddenLayers.has(l) ? '' : 'on'}" data-ls="${l}"></span>${label}</span><small>${list.length} · ${esc(help)}</small></h4><div class="bars">${by.map(([col, c]) => `<span style="flex:${c};background:${col}"></span>`).join('')}</div></div>`;
  }).join('')}</div>`;
  body.querySelectorAll('[data-ls]').forEach(s => s.onclick = () => { const l = s.dataset.ls; hiddenLayers.has(l) ? hiddenLayers.delete(l) : hiddenLayers.add(l); refreshVisibility(); });
  // stage art: the stage's chunks (areas) and the worlds (geometry pieces) inside each; click a piece name to select it
  const byChunk = new Map(); for (const m of art.children) { const c = m.userData.chunk; if (!byChunk.has(c)) byChunk.set(c, []); byChunk.get(c).push(m); }
  const tri = m => Math.round(m.children.reduce((a, c) => a + (c.geometry?.index?.count || 0) / 3, 0));
  const sec = document.createElement('div');
  const hc = hiddenByYou();
  sec.innerHTML = `<div class="divider"></div><div class="sub artHead"><b>Stage art</b> <button class="ghost" id="artAll" ${hc ? '' : 'disabled'}>${hc ? `Show all (${hc} hidden)` : 'Nothing hidden'}</button> <label><input type="checkbox" id="artP" ${showStandins ? 'checked' : ''}> Show P stand-ins</label></div><div class="sub"> - the stage's own geometry, split into chunks (areas; the Zones panel uses the same numbers) and worlds inside each chunk (O/OS main geometry, G/D extra layers, P distant stand-ins, sky chunks follow the Sky chip). Editor-only: hiding never changes the game.</div>
    <div class="cards">${[...byChunk].sort((a, b) => a[0] - b[0]).map(([c, list]) => `<div class="card"><h4><span class="toggle"><span class="switch ${hiddenArt.has('c:' + c) ? '' : 'on'}" data-ac="${c}"></span>Chunk ${c}${list[0].userData.backdrop ? ' · sky' : ''}</span><small>${list.length} piece${list.length > 1 ? 's' : ''}</small></h4>
      ${list.map(m => `<div class="rel"><span class="switch small ${hiddenArt.has('w:' + m.userData.art) || (m.userData.standin && !showStandins) ? '' : 'on'}" data-aw="${esc(m.userData.art)}"></span><span style="flex:1;cursor:pointer" data-sel="${esc(m.userData.art)}">${esc(m.userData.world.replace(/\.RG1$/i, ''))}</span><span class="muted">${m.userData.kind} · ${tri(m)}</span></div>`).join('')}</div>`).join('')}</div>
`;
  body.append(sec);
  sec.querySelectorAll('[data-ac]').forEach(s => s.onclick = () => setArtHidden('c:' + s.dataset.ac, !hiddenArt.has('c:' + s.dataset.ac)));
  sec.querySelectorAll('[data-aw]').forEach(s => s.onclick = () => { const m = art.children.find(x => x.userData.art === s.dataset.aw); if (m?.userData.standin && !hiddenArt.has('w:' + s.dataset.aw)) { showStandins = !showStandins; applyArt(); renderPanel(); return; } setArtHidden('w:' + s.dataset.aw, !hiddenArt.has('w:' + s.dataset.aw)); });
  sec.querySelectorAll('[data-sel]').forEach(s => s.onclick = () => { const m = art.children.find(x => x.userData.art === s.dataset.sel); if (m) selectArt(m); });
  sec.querySelector('#artAll').onclick = showAllArt;
  sec.querySelector('#artP').onchange = e => { snapshot(); showStandins = e.target.checked; applyArt(); changed(); };
  if (renderLayers.focusHidden) { renderLayers.focusHidden = false; const off = sec.querySelector('.switch:not(.on)'); off?.scrollIntoView({block: 'center'}); off?.closest('.card')?.classList.add('on'); }
}
function renderIssues(body) {
  const list = issues(), hc = hiddenByYou(), tr = textureReport(), noStart = !hasStart();
  const ae = Object.keys(project.artEdits || {}).length;
  const lostSurf = (project.surfaceCollision || []).filter(e => !e.triangles?.length);
  const extra = (lostSurf.length ? `<div class="card issue"><span class="sev">▲</span><div><b>Collision from surface: ${lostSurf.length} without faces</b><p>${esc(lostSurf.map(e => e.name).join(', '))}: the surface it was picked from is gone (deleted or changed), so the build refuses it. Remove it in the inspector (Collision from surface).</p></div></div>` : '') + stageFilesIssues().map(([t, x, err]) => `<div class="card issue ${err ? 'err' : ''}"><span class="sev ${err ? 'err' : ''}">${err ? '✕' : 'ⓘ'}</span><div><b>${esc(t)}</b><p>${esc(x)}</p><button class="ghost small" data-sfgo>Stage settings</button></div></div>`).join('') + (noStart ? `<div class="card issue err"><span class="sev err">✕</span><div><b>No player start</b><p>${project.mode === 'new' ? 'The game needs a start and this new stage has none: place one.' : `The game needs a start and ${project.playerStart?.removedOriginal ? 'you removed it' : 'this project has none'}.`} Saving works; Build and Playtest are refused until you place one${project.mode === 'new' ? '' : ' or restore the original'} (they ask first).</p><button class="ghost small" data-mv="cam" data-mid="">Place start at camera</button> <button class="ghost small" data-mv="cursor" data-mid="" data-ml="the player start">Place at cursor</button>${project.mode === 'new' ? '' : ' <button class="ghost small" id="issStart">Restore original</button>'}</div></div>` : '') + (shells.length ? `<div class="card issue" id="issShells"><span class="sev">ⓘ</span><div><b>Enclosing shells: ${shells.length} art layer${shells.length > 1 ? 's' : ''}</b><p>${esc(shells.map(g => `${g.userData.world} (chunk ${g.userData.chunk})`).join(', '))} wrap around the play area. They are drawn as glow (additive, so they never darken the view) and, while the camera is inside one, behind the stage (clicks try the stage first). What these G / D layers are in game is not confirmed; hide one in the art inspector (select it with a click on it) if it is in the way.</p></div></div>` : '') + (ae ? `<div class="card issue" id="issArt"><span class="sev">ⓘ</span><div><b>${ae} stage-art piece${ae > 1 ? 's' : ''} moved / rotated / scaled</b><p>The build rewrites ${ae > 1 ? 'their' : 'its'} geometry, the matching collision and the chunk's draw box. Check them in a playtest: the editor's collision overlay is not moved.</p></div></div>` : '') + (hc ? `<div class="card issue" id="issHidden"><span class="sev">▲</span><div><b>${hc} stage-art piece${hc > 1 ? 's' : ''} hidden</b> - editor only<p>Hidden by you in Layers or the art inspector. Click to see them in Layers; "Show all" there (or the badge on the Stage art lens) brings them back.</p></div></div>` : '')
    + (tr.length ? `<div class="card issue" id="issTex"><span class="sev">${texMissing().length ? '▲' : 'ⓘ'}</span><div><b>${texMissing().length ? `${texMissing().length} material${texMissing().length > 1 ? 's' : ''} without texture` : ''}${texMissing().length && tr.length > texMissing().length ? ' · ' : ''}${tr.length > texMissing().length ? `${tr.length - texMissing().length} material${tr.length - texMissing().length > 1 ? 's' : ''} vertex-coloured, no texture (by design)` : ''}</b> in the stage art<p>${texMissing().length ? 'Materials that name a texture the editor has no (usable) image for are drawn untextured. ' : ''}Vertex-coloured materials have no texture in the game either: their colour comes from the world's vertex colours, so they look right. Click for the list (also in the browser console).</p><div class="texList" hidden>${tr.map(r => `<div class="rel"><span style="flex:1">${esc(r.world.replace(/\.RG1$/i, ''))} · ${esc(r.texture)}</span><span class="muted">${esc(r.why)} · ${r.faces} faces</span></div>`).join('')}</div></div></div>` : '');
  body.innerHTML = list.length || extra ? `<div class="sub">Things the build will warn about or that may not behave in game. Click to jump.</div><div class="cards">${list.slice(0, 300).map(([it, title, text, fix], i) => `<div class="card issue" data-i="${i}"><span class="sev">▲</span><div><b>${esc(it.name)}</b> - ${esc(title)}<p>${esc(text)}</p>${fix ? `<button class="ghost small" data-fix="${i}">Fix</button>` : ''}</div></div>`).join('')}</div>` : '<div class="sub">No issues found.</div>';
  body.querySelectorAll('[data-fix]').forEach(b => b.onclick = e => { e.stopPropagation(); list[+b.dataset.fix][3](); });
  body.querySelectorAll('[data-i]').forEach(c => { c.onclick = () => select(list[+c.dataset.i][0]); c.ondblclick = () => select(list[+c.dataset.i][0], true); });
  if (extra) { body.insertAdjacentHTML('afterbegin', `<div class="cards">${extra}</div>`); }
  body.querySelector('#issStart')?.addEventListener('click', restoreStart); body.querySelectorAll('[data-sfgo]').forEach(b => b.onclick = () => showPanel('settings')); wireMoveButtons(body);
  body.querySelector('#issHidden')?.addEventListener('click', () => { renderLayers.focusHidden = true; showPanel('layers'); });
  body.querySelector('#issTex')?.addEventListener('click', e => { const l = e.currentTarget.querySelector('.texList'); l.hidden = !l.hidden; });
}
function renderPanel() {
  const body = $('panelBody'); if (!project) { body.innerHTML = ''; return; }
  $('panelTitle').textContent = {add: 'Add to stage', outliner: 'Outliner', zones: 'Zones', layers: 'Layers', issues: 'Issues', settings: 'Stage settings'}[panel]; $('panelCount').textContent = '';
  body.classList.toggle('scroll', ['layers', 'settings', 'issues'].includes(panel));   // #49: these panels scroll as one list
  ({add: renderAdd, outliner: renderOutliner, zones: renderZones, layers: renderLayers, issues: renderIssues, settings: renderSettings})[panel](body);
  renderIssueBadge();
}
const texMissing = () => textureReport().filter(r => !r.why.startsWith('vertex-coloured'));   // vertex-coloured by design is information, not an issue
function renderIssueBadge() { const n = project ? issues().length + (hiddenByYou() ? 1 : 0) + (texMissing().length ? 1 : 0) + (hasStart() ? 0 : 1) + stageFilesIssues().filter(x => x[2]).length : 0; $('issueBadge').hidden = !n; $('issueBadge').textContent = n; }
function showPanel(name) { if (name === 'add' && panel !== 'add') renderAdd.autoFocus = true; hideAddCard(); panel = name; $('rail').querySelectorAll('button').forEach(x => x.classList.toggle('on', x.dataset.panel === name)); renderPanel(); }
$('rail').querySelectorAll('button').forEach(b => b.onclick = () => showPanel(b.dataset.panel));
$('stageBtn').onclick = () => project ? showPanel('settings') : showProjects();

// ---- stage settings (classic Stage settings): grades & mission counts (built into nukkoro2.inf), player start (built),
// missions and other settings (saved only) ------------------------------------------------------------------------------
const MISSION_DEFAULT = slot => ({slot, name: {hero: 'Hero', neutral: 'Neutral', dark: 'Dark'}[slot], enabled: false, objective: 'reach_goal', description: 'Reach the goal', count: 1, target: '', nextStage: project.stage, icon: null, voice: null});
const SETTINGS_DEFAULT = () => ({lighting: {enabled: false, sun_strength: 1, sky_strength: .6, sun_azimuth: 225, sun_elevation: 45, sun_color: '#ffd8a3', sky_color: '#8195bd', fog_color: '#656477', fog_density: 0, sky_source: 'stage_skybox'}, music: {asset: '', gain: 1, loop: true}, traits: {timer: false, timer_mode: 'count_up', timer_seconds: 180, timer_expired_event: 'time_up', lives_limit: 0}});
async function renderSettings(body) {
  if (!project.stageParams && !renderSettings.loading) {   // projects without the stage's parameter section yet (classic gradesCard)
    renderSettings.loading = true; body.innerHTML = '<div class="sub">Reading the stage parameters…</div>';
    try { const sp = await api('/api/stage-params?id=' + project.stage); if (sp.section) { project.stageParams = sp; project.stageParamsOriginal = structuredClone(sp); } else project.stageParams = null; } catch (e) { toast(e.message, true); }
    renderSettings.loading = false; if (panel === 'settings') renderPanel(); return;
  }
  const sp = project.stageParams, boss = sp?.boss, routes = boss ? [['normal', 'Boss']] : [['hero', 'Hero'], ['normal', 'Normal'], ['dark', 'Dark']];
  const start = project.objects.find(o => o.kind === 'playerstart' && !o.player) || project.objects.find(o => o.kind === 'spawn'), missions = project.missions || ['hero', 'neutral', 'dark'].map(MISSION_DEFAULT), settings = project.stageSettings || SETTINGS_DEFAULT();
  const num = (attr, v, ph = '-') => `<input class="num" type="number" step="any" ${attr} value="${v ?? ''}" placeholder="${ph}">`;
  const targets = items.filter(it => it.o.kind !== 'path').slice(0, 1500);
  const custom = project.mode === 'new' || !!project.customStage;
  const group = (title, html) => `<div class="spGroup"><h3 class="spHead">${title}</h3>${html}</div>`;
  body.innerHTML = `<div class="cards">
   ${group('This stage', `<div class="card"><h4>Project name</h4><label class="field"><span>Name</span><input class="num" id="projName" maxlength="80" value="${esc(project.name || '')}"><small>How it shows in Your projects and the top bar. Saved with the next Save.</small></label></div>${custom ? `<div id="customStage">${customStageCard()}</div>` : ''}`)}
   ${group('Missions', `<div id="missionsCard">${missionsCard()}</div>
   <div class="card"><h4>Grades <small class="tagOk">works in game</small></h4>${sp ? `<p class="muted" title="Section [${esc(sp.section)}] of the stage parameters">${boss ? 'The slowest clear time (seconds) that still earns each grade. Slower than D gives E.' : 'The lowest score that earns each grade (A is best). Below D gives E. A route left empty always gives A.'}</p>
     <table class="grades"><tr><th></th><th>A</th><th>B</th><th>C</th><th>D</th></tr>${routes.map(([r, label]) => `<tr><td>${label}</td>${[0, 1, 2, 3].map(i => `<td>${num(`data-grade="${r}" data-i="${i}" min="0" step="1"`, sp.grades?.[r]?.[i])}</td>`).join('')}</tr>`).join('')}</table>
     ${boss || custom ? '' : [['hero', 'Hero'], ['dark', 'Dark']].map(([r, label]) => `<label class="field"><span>${label}: enemies to defeat</span>${num(`data-count="${r}" min="0" step="1"`, sp.counts?.[r]?.[0])}<small>How many the ${label} mission counts.</small></label>`).join('')}
     <div class="wsBtns"><button class="ghost small" id="spReset">Back to the game's grades</button> <button class="ghost small" id="spRaw" title="Every line of this stage's parameter section, as the game reads it">Advanced: raw lines…</button></div>
     <textarea class="num mono" id="spLines" rows="8" spellcheck="false" hidden>${esc((sp.lines || []).join('\n'))}</textarea>` : '<p class="muted">This stage has no grades.</p>'}</div>`)}
   ${group('Start', `<div class="card"><h4>Player start <small class="tagOk">works in game</small></h4>${start ? `<div class="field"><span>Position</span><div class="vec">${['X', 'Y', 'Z'].map((ax, i) => `<div class="ax" data-ax="${ax}"><input class="num" data-start="${i}" type="number" step="any" value="${+(+start.position[i]).toFixed(2)}"></div>`).join('')}</div></div>
     <label class="field"><span>Facing (degrees)</span>${num('data-start="heading"', round2(start.rotation?.[1] || 0), '0')}<small>The arrow on the start marker shows which way Shadow faces.</small></label>
     <div class="wsBtns"><button class="ghost small" id="startSel">Show in the 3D view</button>${moveButtons(start.id, 'the player start')}</div>` : `<p class="badText">There is no player start: Shadow has nowhere to begin.</p>${moveButtons('', 'the player start')}`}<button class="ghost small" id="ssRestore" title="Use the game's own start position and finish poses again">Back to the game's start</button></div>
   <div id="rulesCard">${rulesCard()}</div>`)}
   ${group('Look &amp; sound', `<div id="musicCard">${musicCard()}</div><div id="soundCard">${soundCard()}</div><div id="skyCard">${skyCard()}</div><div id="voiceCard">${voiceCard()}</div>`)}
   <details class="sect spAdvanced"><summary>Advanced</summary><div class="cards">
     <div id="stageFiles">${stageFilesCard()}</div><div id="drawDist">${drawDistCard()}</div><div id="autoLight">${autoLightCard()}</div>${profilesCard()}</div></details>
   <p class="muted">${settingsNote()}</p></div>`;
  const ensure = () => { project.missions ||= missions; project.stageSettings ||= settings; };   // created on the first edit, like classic does on load
  body.querySelectorAll('[data-grade]').forEach(inp => inp.onchange = () => { snapshot(); const r = inp.dataset.grade; const vals = [...body.querySelectorAll(`[data-grade="${r}"]`)].map(x => x.value === '' ? null : Number(x.value));
    sp.grades ||= {}; if (vals.every(v => v === null)) delete sp.grades[r]; else sp.grades[r] = vals.map(v => v ?? 0);
    const g = sp.grades[r]; if (g && !(boss ? g.every((v, i) => !i || g[i - 1] <= v) : g.every((v, i) => !i || g[i - 1] >= v))) toast(`Grades (${r}): ${boss ? 'times should rise' : 'scores should fall'} from A to D, or the build refuses them.`, true); changed(); });
  body.querySelectorAll('[data-count]').forEach(inp => inp.onchange = () => { snapshot(); const r = inp.dataset.count; sp.counts ||= {}; if (inp.value === '') delete sp.counts[r]; else sp.counts[r] = [Number(inp.value), sp.counts[r]?.[1] ?? 0]; changed(); });
  body.querySelector('#spReset')?.addEventListener('click', async () => { snapshot(); const o = project.stageParamsOriginal || await api('/api/stage-params?id=' + project.stage); project.stageParams = structuredClone(o); changed(); renderPanel(); });
  body.querySelector('#spRaw')?.addEventListener('click', () => { const t = body.querySelector('#spLines'); t.hidden = !t.hidden; });
  body.querySelector('#spLines')?.addEventListener('change', e => { snapshot(); sp.lines = e.target.value.split('\n').map(l => l.trimEnd()).filter(l => l.trim()); changed(); });
  body.querySelectorAll('[data-start]').forEach(inp => inp.onchange = () => { const v = +inp.value; if (!Number.isFinite(v)) return; snapshot(); const k = inp.dataset.start;
    if (k === 'heading') { start.rotation = [start.rotation?.[0] || 0, v, start.rotation?.[2] || 0]; } else start.position[+k] = v;
    const it = byId.get(start.id); if (it) { const n = nodeOf.get(it); if (n) { n.position.set(...start.position); n.rotation.set(...start.rotation.map(THREE.MathUtils.degToRad)); } relocate(it); } computeSpine(); changed(); updateMarkers(); drawFlow(); });
  body.querySelector('#startSel')?.addEventListener('click', () => select(byId.get(start.id), true));
  wireProfiles(body); wireAutoLight(body); wireStageFiles(body); wireDrawDist(body); wireCustomStage(body); wireMissions(body); wireRules(body); wireMusic(body); wireSound(body); wireProjectName(body); wireSky(body); wireVoice(body); loadAtmos(); loadStageFilesPreview(); wireMoveButtons(body); body.querySelector('#ssRestore')?.addEventListener('click', restoreStart);
  body.querySelectorAll('[data-mi]').forEach(inp => inp.onchange = () => { snapshot(); ensure(); const m = project.missions[+inp.dataset.mi], k = inp.dataset.mk; m[k] = inp.type === 'checkbox' ? inp.checked : ['count', 'nextStage'].includes(k) ? Number(inp.value) : inp.value; changed(); });
  body.querySelectorAll('[data-ss]').forEach(inp => inp.onchange = () => { snapshot(); ensure(); project.stageSettings[inp.dataset.ss][inp.dataset.sk] = inp.type === 'checkbox' ? inp.checked : inp.type === 'number' ? Number(inp.value) : inp.value; changed(); });
}
const settingsNote = () => 'Everything marked works in game is written into the build. Marked needs the runtime = saved and written for the game runtime, which does not use it yet.';

// ---- add to stage: drag a tile into the viewport, or click a tile then click a spot -----------------------------------
// ---- #85 the Add panel (docs/stage-workshop/ADD-PANEL-REDESIGN.md): one list of addable things (classes with their
// variants, tools, prefabs, stage-code ghosts, assets) built once per project / library arrival (addItems), one search
// (free text + cat: from: ok: here: type:), category chips with counts, filter toggles (not modes), a virtualised grid
// (only the visible rows are in the page) sectioned by category in All; per-viewer prefs, recents and favourites in
// localStorage, nothing in the project
// UX follow-up: friendlier names in Add (display and search only; placed objects keep the catalog name, which the build
// and the editor's own rules match on). From the ShadowSET list names where they read better.
const FRIENDLY = {'Spring Shadow': 'Spring', 'GUNSoldier': 'GUN Soldier', 'GUNBeetle': 'GUN Beetle', 'GUNBigfoot': 'GUN Big Foot', 'GUNRobot': 'GUN Robot',
  'Bk Giant': 'Black Arms Oak', 'Bk Soldier': 'Black Arms Soldier', 'Bk Wing Large': 'Black Arms Hawk', 'Bk Wing Small': 'Black Arms Wing', 'Bk Worm': 'Black Arms Worm',
  'Bk Larva': 'Black Arms Larva', 'Bk Chaos': 'Artificial Chaos', 'Bk Ninja': 'Black Arms Assassin', 'Egg Shadow Android': 'Shadow Android', 'Light Colli': 'Local Lighting',
  'Trigger Talking': 'Trigger: Talking', 'Trigger Skybox': 'Trigger: Skybox', 'Elec Security': 'Defense Program', 'Elec Cube': 'Spinning Dark Cube', 'Elec Roll Hexa': 'Spinning Hexagon',
  'Zipline': 'Zipline / Coaster', 'Fleet Holder Eggman Battleship': 'Eggman Battleship', 'Dark Spin Entrance': 'Dark Spin Entry', 'Case': 'Locked Case', 'Egg Pierrot': 'Egg Pierrot (clown)'};
const friendly = n => FRIENDLY[n] || n;
const ADD_ORDER = ['key', 'pickups', 'enemies', 'bosses', 'objects', 'decor', 'volumes', 'lighting', 'tilesets', 'shapes', 'paths', 'prefabs', 'stagecode', 'assets'];
const ADD_LABEL = {key: 'Key', pickups: 'Pickups', enemies: 'Enemies', bosses: 'Bosses', objects: 'Objects', decor: 'Decor', volumes: 'Volumes', lighting: 'Lighting', tilesets: 'Tilesets', shapes: 'Shapes', paths: 'Paths', prefabs: 'Prefabs', stagecode: 'Stage', assets: 'Assets'};
const ADD_GLYPH = {key: '★', pickups: '◎', enemies: '✶', bosses: '♛', objects: '◆', decor: '❀', volumes: '▢', lighting: '☀', tilesets: '▦', shapes: '⬢', paths: '∿', prefabs: '⧉', stagecode: '⚙', assets: '♫'};
const addColour = c => CAT[c]?.colour || (c === 'tilesets' ? '#38bdf8' : '#c084fc');
const ADD_DEFAULTS = {cat: 'all', here: false, placeable: true, fav: false, recent: false, list: false, allTypes: false, collapsed: [], kit: ''};
const addPrefs = (() => { try { return {...ADD_DEFAULTS, ...JSON.parse(localStorage.getItem('ui2.addPanel') || '{}')}; } catch { return {...ADD_DEFAULTS}; } })();
const saveAddPrefs = () => { try { localStorage.setItem('ui2.addPanel', JSON.stringify(addPrefs)); } catch {} };
const readList = k => { try { const v = JSON.parse(localStorage.getItem(k) || '[]'); return Array.isArray(v) ? v : []; } catch { return []; } };
let addRecent = readList('ui2.addRecent'), addFav = new Set(readList('ui2.addFav'));
function pushRecent(key) { if (!key) return; addRecent = [key, ...addRecent.filter(k => k !== key)].slice(0, 8); try { localStorage.setItem('ui2.addRecent', JSON.stringify(addRecent)); } catch {} }
function toggleFav(key) { if (addFav.has(key)) addFav.delete(key); else addFav.add(key); try { localStorage.setItem('ui2.addFav', JSON.stringify([...addFav])); } catch {} }
let addQuery = '', placing = null, placingKeep = false, addCache = null; const addExpanded = new Set();
const addSource = () => addPrefs.here ? 'stage' : 'all';   // All stages = default (research doc #32 step 4)
let memInfo = null; fetch('../capture/memory.json').then(r => r.ok ? r.json() : null).then(j => memInfo = j).catch(() => {});
// other stages' classes: every stage's object library (SET records, ~50 KB each, read once and cached) gives one
// template per class and the stage it comes from; catalog anyStage says whether the build can place it in
// this stage, classReason why not
const libs = new Map(), libsState = {done: 0, total: 0, busy: false};
async function loadLibs() {
  if (libsState.busy || libsState.total) return; libsState.busy = true; const ids = config.stages.map(st => st.id); libsState.total = ids.length; let i = 0;
  const work = async () => { while (i < ids.length) { const id = ids[i++]; libsState.reading = stageName(id); try { libs.set(id, (await api('/api/object-library?id=' + id)).objects); } catch { libs.set(id, []); } libsState.done++; addLibArrived(); } };
  await Promise.all([1, 2, 3, 4, 5, 6].map(work)); libsState.busy = false; libsState.reading = ''; addLibArrived();
}
function addLibArrived() { addCache = null; clearTimeout(addLibArrived.t); addLibArrived.t = setTimeout(() => { if (panel === 'add') renderAdd.refresh?.(); }, 250); }
function foreignRows(seen) {
  const out = [], have = new Set(seen.keys()), order = [...config.stages.filter(st => !/^stg\d+$/i.test(st.name)), ...config.stages.filter(st => /^stg\d+$/i.test(st.name))].map(st => st.id);
  // #85 one tile per class: its origin = the stage the compatibility sweep took it from (proven to work), else the first
  // stage in story order that has it
  const pick = new Map(), proven = new Map(); for (const v of Object.values(compat || {})) for (const [cls, e] of Object.entries(v)) if (e.sourceStage != null) proven.set(cls, e.sourceStage);
  for (const id of order) { if (id === project.stage) continue; for (const r of libs.get(id) || []) { const k = `${r.list}-${r.type}`, pr = proven.get(classHex(r.list, r.type).toUpperCase()); if (!pick.has(k) || (pr === id && pick.get(k)[0] !== pr)) pick.set(k, [id, r]); } }
  for (const [k, [id, r]] of pick) {
    if (have.has(k) || (r.list === 0x25 && [0x88, 0x89, 0x8A].includes(r.type))) continue; have.add(k);   // decor / breakables use their own stage's tables
    const c = catalog.get(k), t = r.template;
    const ok = c?.anyStage || c?.copiesFiles;   // copiesFiles (phase 3): the build copies the gadget's files from its stage
    out.push({name: classOf(t), template: t, cat: category(t), count: 0, foreign: id, disabled: !ok, reason: c?.classReason || 'not known to work outside its stage', sub: `${classOf(t)} · from ${stageName(id)}${c?.copiesFiles && !c?.anyStage ? ' · the build copies its models and textures from there' : c?.classReason ? ' · ' + c.classReason : ''}`});
  }
  return out;
}
// enemies keep their models in enemy/<Name>Data.one (catalog classReason 'own loader enemy/...'): one archive is far
// smaller than a whole stage, so a foreign enemy tile asks /api/models?archive=<name> (#54); if the server has
// no such endpoint the tile keeps its dot until hovered (whole stage, as before)
const archives = new Map();
const archiveOf = row => (catalog.get(`${row.template.list}-${row.template.type}`)?.classReason || '').match(/\b(enemy\/[\w]+\.one)\b/i)?.[1];
// #54: GET /api/models?class=<hex list<<8|type> returns the models of that class's own archive(s); cached
// per class, merged into the shared pool; a class with nothing there keeps the hover fallback (its whole stage)
const classHex = (list, type) => ((list << 8) | type).toString(16).padStart(4, '0');
function classModels(list, type) {
  const k = classHex(list, type);
  if (!archives.has(k)) archives.set(k, api('/api/models?class=' + k).then(d => { const have = new Set(R.shared.map(m => m.id)); const add = (d.models || []).filter(m => !have.has(m.id)); R.shared.push(...add); return add.length > 0 || (d.models || []).length > 0; }).catch(() => false));
  return archives.get(k);
}
const foreignDone = new Set(), foreignClassDone = new Set();
async function foreignThumb(row, el, bucket = 's') {
  if (R.foreign.has(row.foreign)) return;
  const key = thumbKeyOf(row, bucket); if (foreignDone.has(key)) return;
  if (!await classModels(row.template.list, row.template.type)) return;
  if (!foreignDone.has(key)) { foreignDone.add(key); thumbs.delete(key); }
  const url = thumbFor(row, bucket); if (url && el.isConnected) setThumb(el, url, row, bucket);
  const ck = `${row.template.list}-${row.template.type}`; if (foreignClassDone.has(ck)) return; foreignClassDone.add(ck);
  for (const it of items) if (isForeign(it.o) && !nodeOf.has(it) && `${it.o.list}-${it.o.type}` === ck) rebuildNode(it);
}
// models of another stage, only the parts this stage lacks (its own gadgets, enemies, characters): /api/models is
// ~37 MB per stage, so it loads on demand (a placed foreign object, a hovered foreign tile) and stays cached
const foreignBusy = new Set();
async function loadForeign(stage) {
  if (R.foreign.has(stage) || foreignBusy.has(stage) || stage == null) return; foreignBusy.add(stage); toast(`Loading the models of ${stageName(stage)} for objects from there…`);
  try {
    const data = await api('/api/models?id=' + stage), common = /^(stg_cmn|common\\|character\\shadow)/i;
    R.foreign.set(stage, data.models.filter(m => !common.test(m.archive)));
    for (const it of items) if (isForeign(it.o) && it.o.sourceStage === stage) rebuildNode(it);
    for (const k of [...thumbs.keys()]) if (k.replace(/#l$/, '').endsWith('@' + stage)) { thumbs.delete(k); foreignDone.delete(k); }
    addCache = null;
    if (panel === 'add') renderPanel();
  } catch (e) { toast(`Models of ${stageName(stage)}: ${e.message}`, true); } finally { foreignBusy.delete(stage); }
}
function addRows() {
  const templates = project.nativeTemplates?.length ? project.nativeTemplates : project.objects.filter(o => o.kind === 'native');
  const seen = new Map(), byClass = new Map();
  for (const it of items) if (it.o.kind === 'native') { const k = `${it.o.list}-${it.o.type}`; if (!byClass.has(k)) byClass.set(k, []); byClass.get(k).push(it); }
  for (const t of templates) { const k = `${t.list}-${t.type}`; if (!seen.has(k)) seen.set(k, {name: classOf(t), template: t, cat: category(t), count: 0}); }
  for (const it of items) if (it.o.kind === 'native' && isForeign(it.o) && !seen.has(`${it.o.list}-${it.o.type}`)) seen.set(`${it.o.list}-${it.o.type}`, {name: classOf(it.o), template: it.o, cat: it.cat, count: 0, placedForeign: true});
  for (const it of items) if (it.o.kind === 'native') { const r = seen.get(`${it.o.list}-${it.o.type}`); if (r) r.count++; }
  const rows = [...seen.values()].filter(r => !(r.template.list === 0x25 && (r.template.type === 0x88 || r.template.type === 0x89)));
  // decor / breakables: one tile per entry of the stage's own model tables (the type word picks the model)
  for (const [key, type, label] of [['gadget', 0x89, 'Breakable'], ['work', 0x88, 'Decoration']]) {
    const base = templates.find(t => t.list === 0x25 && t.type === type); if (!base) continue;
    const placedOf = byClass.get(`37-${type}`) || [], vars = [];
    (R.artist[key] || []).forEach((e, i) => { if (!e.models.length || /_bk$|_brk$/i.test(e.name)) return; vars.push({key: `c:37-${type}:t${i}`, name: e.name.replace(/_/g, ' '), template: base, cat: 'decor', count: placedOf.filter(it => paramWord(it.o, 0) === i).length, typeWord: i, sub: `${label} · type ${i}: one of this stage's ${label === 'Decoration' ? 'decoration' : 'breakable'} models`, variantOf: label, variantLabel: e.name.replace(/_/g, ' ')}); });
    if (!vars.length) continue; const def = vars.reduce((a, b) => b.count > a.count ? b : a, vars[0]);
    rows.push({...def, key: `c:37-${type}`, name: label, placeName: def.name, variantOf: undefined, variantLabel: undefined, variants: vars, defVariant: def.name, count: placedOf.length, sub: `${label}: the stage's own ${label === 'Decoration' ? 'decoration' : 'breakable'} models (${vars.length} types; the type word picks the model)`});
  }
  const geoCount = f => items.filter(it => it.o.kind === 'geometry' && f(it.o)).length;
  for (const k of tilesetKits()) for (const pc of k.pieces || []) rows.push({name: pc.name || pc.id, tilePiece: {kit: k.kit, piece: pc.id}, piece: pc, kitTitle: k.title || k.kit, cat: 'tilesets', thumbUrl: pc.thumbUrl || '', keys: ['tileset', 'tile', 'kit', k.kit, k.title, pc.id, ...(pc.tags || []), ...(pc.sockets || []).map(x => x.kind)].filter(Boolean).join(' '),
    count: geoCount(o => o.tile?.kit === k.kit && o.tile?.piece === pc.id), small: `${k.title || k.kit} · ${(pc.cells || []).join(' × ')} cells`, sub: `${k.title || k.kit}: ${(pc.size || (pc.cells || []).map(c => c * TILE_GRID)).join(' × ')} units. Snaps to the ${TILE_GRID}-unit grid at its corner; it faces the way the camera looks, in 90° steps (R turns it while placing, the blue ghost shows where it lands)${k.placeholder ? ' (placeholder kit until the real tiles arrive)' : ''}`});
  for (const [shape, label] of SHAPES) rows.push({name: label, shape, cat: 'shapes', count: geoCount(o => o.shape === shape && o.renderVisible !== false), sub: shape === 'loop' ? 'Loop ribbon: the geometry builds and is solid; running upside down and the loop camera are not integrated in game yet' : `${label}: your own solid geometry, built into the chunk it is placed in, textured with that chunk's main material`});
  rows.push({name: 'Invisible wall', shape: 'box', dims: [200, 150, 10], extra: {name: 'Invisible wall', renderVisible: false}, cat: 'shapes', count: geoCount(o => o.renderVisible === false && o.shape !== 'collision'), sub: 'Invisible wall: solid in game but not drawn (a box with "Visible in game" off)'});
  rows.push({name: 'Draw & extrude', action: beginDraw, cat: 'shapes', count: geoCount(o => o.shape === 'polygon'), sub: 'Click corners on a flat plane (the first click sets its height); Enter extrudes it 30 units up, Backspace removes the last corner, Esc cancels'});
  rows.push({name: 'Import level / model (glTF, OBJ)', keys: 'blender map level model mesh import 3d glb gltf obj file', action: () => levelPicker.click(), cat: 'shapes', count: geoCount(o => !!o.importGroup), sub: 'A whole level or a big model from Blender or another game: .glb, or .gltf + .bin + images, or .obj + .mtl + images (pick them all together). Textured pieces with collision, cut to fit the game.'});
  rows.push({name: 'Import OBJ mesh', action: () => objPicker.click(), cat: 'shapes', count: geoCount(o => o.shape === 'mesh' && !o.sourceModel), sub: 'A Wavefront .obj file becomes your own textured mesh with collision (placed at the view centre)'});
  // editor objects for logic (classic defaults; a new volume has no actions, it gets them in the Logic workspace)
  rows.push({name: 'Trigger volume', editor: 'trigger', cat: 'volumes', count: (project.triggers || []).filter(t => !t.script).length, sub: 'An invisible box the player enters / leaves / stays in; its graph (Logic workspace) runs actions'});
  rows.push({name: 'Teleport pad', editor: 'teleport', cat: 'volumes', count: items.filter(it => it.o.kind === 'teleport').length, sub: 'Touching it teleports the player to its destination (set it in the Logic workspace or the inspector)'});
  rows.push({name: 'Player start', moveTo: startId(), cat: 'key', count: project.objects.filter(o => (o.kind === 'playerstart' && !o.player) || o.kind === 'spawn').length, sub: hasStart() ? 'Moves the player start to where you click (the game has one start; heading unchanged)' : 'This project has no player start: click where it goes (Restore original start in Stage settings puts the game\'s back)'});
  for (const g of project.objects.filter(o => o.kind === 'goalpose')) rows.push({name: g.name, moveTo: g.id, cat: 'key', count: 1, sub: 'Moves this finish pose (where the clear animation plays) to where you click'});
  rows.push({name: 'Lighting region', editor: 'lightregion', cat: 'lighting', count: (project.lighting?.regions || []).length, sub: 'A box with its own atmosphere (sun, sky, fog: the lighting_profiles.ini keys) while the player is inside; blends in over its Blend seconds'});
  rows.push({name: 'Point light', editor: 'light', cat: 'lighting', count: (project.lighting?.lights || []).length, sub: 'A light (colour, intensity, radius) for the lighting engine; previewed in the editor (Lighting preview chip)'});
  rows.push({name: 'Teleport destination', editor: 'destination', cat: 'volumes', count: items.filter(it => it.o.kind === 'destination').length, sub: 'A marker to teleport to (any placement works as a target)'});
  if (!prefabsList) loadPrefabs(); for (const pf of prefabsList || []) rows.push({name: pf.name, prefab: pf, cat: 'prefabs', count: pf.objects.length, small: `${pf.objects.length} placement${pf.objects.length > 1 ? 's' : ''} · ${stageName(pf.stage)}`, sub: `Prefab: ${pf.objects.length} placements saved ${pf.created?.slice(0, 10) || ''} on ${stageName(pf.stage)}; click it, then click where it goes`});
  const pathCount = t => items.filter(it => it.o.kind === 'path' && (t ? it.o.pathType === t : true)).length;
  rows.push({name: 'Grind rail', newPath: 'rail', cat: 'paths', count: pathCount('rail'), sub: 'Grind rail: Shadow lands on it and grinds along it (PATH.PTP type 2). Click where it starts; then drag its blue points, click the line to add a point, Shift-drag an end to extend it'});
  rows.push({name: 'Loop (runnable)', loopKit: true, keys: 'loop loop-de-loop vertical', cat: 'paths', count: items.filter(it => it.o.kind === 'geometry' && it.o.loopKit).length, sub: 'A loop Shadow runs round: the track (surface Loop track) and its Loop path in one piece. Click the floor where it starts; it runs away from the camera. Move / turn it as a whole (changing its points means placing a new one)'});
  rows.push({name: 'Platform path', newPath: 'platform', cat: 'paths', count: pathCount('platform'), sub: 'Platform path: a route for a moving object (PATH.PTP type 8). Give its spline ID to a Platform (0/0x0F) with Movement Type Path / Loop Path; nothing rides it on its own'});
  rows.push({name: 'Spline (plain)', newPath: 'native', cat: 'paths', count: pathCount('native'), sub: 'Plain spline: a curve with a spline ID for objects that take one (Zipline 12/0x88, Escape Pod Path Switch 19/0xED, stage code). Not a rail, not a platform route'});
  rows.push(...foreignRows(seen));
  for (let i = 0; i < rows.length; i++) if (rows[i].template?.kind === 'native' && !rows[i].variants && rows[i].typeWord === undefined) rows[i] = withVariants(rows[i], byClass);
  rows.push({name: 'Save selection as prefab', action: savePrefab, cat: 'prefabs', count: 0, small: 'from the selection', sub: 'Saves the selected placements as a prefab (kept by the editor, usable in every project); insert it from this category later'});
  for (const it of items) if (it.cat === 'stagecode') rows.push({key: 's:' + it.id, name: it.name, stageId: it.id, cat: 'stagecode', count: 1, small: 'read-only ghost', action: () => focusObject(it.id), sub: `${it.name}: an object the stage's own code creates and moves. Drawn as a read-only ghost; click to show it in the 3D view. It cannot be placed or moved here`});
  for (const a of (project.customAssets || []).filter(a => ['sfx', 'music'].includes(a.kind))) rows.push({key: 'a:' + (a.id || a.name), name: a.name, assetId: a.id || a.name, cat: 'assets', count: 0, small: a.kind === 'music' ? 'music' : 'sound', action: () => { showPanel('settings'); toast(a.kind === 'music' ? `${a.name}: pick it as the stage music in Stage settings` : `${a.name}: play it from a Logic graph (Play sound) or a trigger`); }, sub: `${a.kind === 'music' ? 'Music' : 'Sound'} imported into this project. Placing a sound means binding it: the stage music (Stage settings) or a Play sound node in Logic`});
  const meshes = new Map(); for (const g of project.geometry || []) if (g.shape === 'mesh' && !g.sourceModel && g.name) meshes.set(g.name, [...(meshes.get(g.name) || []), g]);
  for (const [name, gs] of meshes) rows.push({key: 'a:mesh:' + name, name, template: gs[0], assetId: 'mesh:' + name, thumbKey: 'asset-' + gs[0].id, cat: 'assets', count: gs.length, small: `imported mesh · ${gs.length} placed`, sub: `${name}: a mesh imported into this project (OBJ); places another copy with the same chunk and material`});
  rows.push({name: 'Stage model as geometry', action: openModelPicker, cat: 'shapes', count: geoCount(o => !!o.sourceModel), sub: "One of this stage's models (DFF) as static textured geometry with collision. It does not get the object's behaviour"});
  return rows.sort((a, b) => a.name.localeCompare(b.name));
}
// a class's variants = the options of its type field (the first enum field named ...Type / Weapon / Partner / Item /
// Vehicle / Model / Chao / EnergyCore / Color among its first two, else its model setting): ring layouts (#78), box
// sides, partners, capsule items... The class tile places the variant the stage uses most (else the template's own)
const VARIANT_FIELD = /type$|^(weapon|partner|item|vehicle|model|chao|energycore|color)$/i;
function variantField(c) {
  const f = c?.fields || [], ok = x => x?.type === 'enum' && (x.options || []).filter(o => o[0] >= 0).length > 1;
  let i = f.slice(0, 2).findIndex(x => ok(x) && VARIANT_FIELD.test(x.name)); if (i >= 0) return i;
  i = c?.modelSetting ? f.findIndex(x => x.name === c.modelSetting) : -1; return ok(f[i]) ? i : -1;
}
function withVariants(r, byClass) {
  const t = r.template, ck = `${t.list}-${t.type}`, c = catalog.get(ck), fi = variantField(c); if (fi < 0) return r;
  const cur = paramWord(t, fi), ring = t.list === 0 && t.type === 0x10, placedOf = r.foreign != null ? [] : byClass.get(ck) || [], fname = c.fields[fi].name;
  const vars = c.fields[fi].options.filter(o => o[0] >= 0).slice(0, 64).map(([v, label]) => {
    const words = [[fi, v]]; if (ring && v) { if ((paramWord(t, 1) ?? 0) < 2) words.push([1, 5]); if (!(paramWord(t, 2, true) > 0)) words.push([2, 120, true]); }   // a layout needs rings to show
    return {...r, key: `c:${ck}:${fi}=${v}`, name: `${r.name} · ${label}`, placeName: `${r.name} (${label})`, words, variantOf: r.name, variantLabel: label, variants: undefined, count: placedOf.filter(it => paramWord(it.o, fi) === v).length, sub: `${r.name} with ${fname} = ${label}${r.sub ? ' · ' + r.sub : ''}`};
  });
  const own = vars.find(v => v.words[0][1] === cur) || vars[0], def = vars.reduce((a, b) => b.count > a.count ? b : a, own);
  return {...r, key: 'c:' + ck, variants: vars, words: def.words, placeName: def.placeName, defVariant: def.variantLabel, variantField: fname};
}
// #71 tile size (per browser) + the left panel's width (drag its right edge; double-click resets), both remembered
const store = (k, v) => { try { localStorage.setItem(k, String(v)); } catch {} }, stored = (k, d) => { try { const v = +localStorage.getItem(k); return v > 0 ? v : d; } catch { return d; } };
let tileW = stored('ui2.tileW', 120);
const applyTileW = () => document.documentElement.style.setProperty('--tileW', tileW + 'px');
const applyPanelW = w => { document.querySelector('.shell')?.style.setProperty('--panelW', w + 'px'); resize(); };
applyTileW(); { const w = stored('ui2.panelW', 0); if (w) setTimeout(() => applyPanelW(w), 0); }
{ const h = Object.assign(document.createElement('div'), {className: 'panelResize', title: 'Drag to resize the panel · double-click resets'}); $('panel').append(h);
  h.addEventListener('pointerdown', e => { e.preventDefault(); h.setPointerCapture(e.pointerId); h.classList.add('on'); const x0 = e.clientX, w0 = $('panel').getBoundingClientRect().width;
    const move = ev => applyPanelW(Math.round(Math.min(Math.max(220, w0 + ev.clientX - x0), Math.max(260, innerWidth - 600))));
    const up = () => { h.classList.remove('on'); h.removeEventListener('pointermove', move); h.removeEventListener('pointerup', up); store('ui2.panelW', Math.round($('panel').getBoundingClientRect().width)); };
    h.addEventListener('pointermove', move); h.addEventListener('pointerup', up); });
  h.addEventListener('dblclick', () => { document.querySelector('.shell')?.style.removeProperty('--panelW'); store('ui2.panelW', 0); resize(); }); }
function compatOf(t, cat) {
  const k = classHex(t.list, t.type).toUpperCase(), here = compat?.[String(project.stage)]?.[k], any = here || (compat && Object.entries(compat).map(([s, v]) => v[k] && {...v[k], tested: +s}).find(Boolean));
  if (!any) return cat === 'enemies' ? {cls: 'unk', text: compat ? 'Not in the enemy sweep yet (untested)' : 'Enemy sweep results not loaded'} : null;
  const res = any.result, cls = res === 'spawn' ? 'ok' : res === 'spawn-no-model' ? 'mid' : res === 'crash' || res === 'hang' ? 'bad' : 'unk';
  const what = {spawn: 'spawns', 'spawn-no-model': 'spawns but is invisible (no model)', crash: 'crashes the game', hang: 'hangs the game', 'no-template': 'no template record to test it with'}[res] || res;
  return {cls, text: `Compatibility sweep ${here ? 'in this stage' : 'in ' + stageName(any.tested ?? any.sourceStage)}: ${what}${any.detail ? ' · ' + any.detail : ''}`};
}
function addItems() {
  const sig = [project.id, items.length, libsState.done, prefabsList?.length ?? -1, compat ? 1 : 0, models ? 1 : 0, (project.customAssets || []).length, (project.geometry || []).length].join('|');
  if (addCache?.sig === sig) return addCache;
  const rows = addRows(), byKey = new Map();
  const decorate = (r, ord) => {
    const t = r.template, ck = t?.kind === 'native' ? `${t.list}-${t.type}` : '', c = ck ? catalog.get(ck) : null;
    r.key ||= ck ? 'c:' + ck : r.prefab ? 'p:' + r.prefab.name : 't:' + r.name; r.ord = ord;
    r.kind = r.prefab ? 'prefab' : r.stageId ? 'stagecode' : r.assetId ? 'asset' : ck ? 'class' : 'tool';
    r.copies = r.foreign != null && !!c?.copiesFiles && !c?.anyStage; r.place = r.disabled ? 'blocked' : r.copies ? 'copies' : 'ok';
    r.compat = ck ? compatOf(t, r.cat) : null;
    const bo = t?.kind === 'native' ? BOSSES[ck] : null;
    if (bo) { r.boss = bo; if (bo[0] === 'no') { r.place = 'blocked'; r.reason = 'not portable: ' + bo[1]; } else if (r.place === 'blocked') { r.place = 'ok'; r.bossPreview = true; } }   // portable bosses: placeable for the set-up (arena, movement); the build leaves them out until boss phase 1
    r.hay = [r.name, FRIENDLY[r.name], r.placeName, r.keys, r.boss && 'boss bosses', ck && classOf(t), c?.listName, ADD_LABEL[r.cat], r.foreign != null && stageName(r.foreign), ck && classHex(t.list, t.type)].filter(Boolean).join(' ').toLowerCase();
    byKey.set(r.key, r);
  };
  rows.forEach((r, i) => { decorate(r, i); (r.variants || []).forEach((v, j) => decorate(v, j)); });
  return addCache = {sig, rows, byKey};
}
function parseAddQuery(q) {
  const t = {text: [], cat: null, from: null, ok: null, here: null, type: null, kit: null};
  for (const w of q.trim().toLowerCase().split(/\s+/).filter(Boolean)) { const m = w.match(/^(cat|from|ok|here|type|kit):(.*)$/); if (m && m[2]) t[m[1]] = m[2]; else if (!m) t.text.push(w); }
  return t;
}
function tokenMatch(r, t) {
  if (t.kit && !(r.tilePiece && (tilesetKits().some(k => k.kit.toLowerCase() === t.kit) ? r.tilePiece.kit.toLowerCase() === t.kit : r.tilePiece.kit.toLowerCase().includes(t.kit) || (r.kitTitle || '').toLowerCase().replace(/\s/g, '').includes(t.kit)))) return false;   // kit:roads = that kit exactly (not roads2); else a part of a kit name
  if (t.cat && !(r.cat.startsWith(t.cat) || ADD_LABEL[r.cat].toLowerCase().startsWith(t.cat) || (r.boss && 'bosses'.startsWith(t.cat)))) return false;
  if (t.from) { if (['here', 'this', 'stage'].includes(t.from) ? r.foreign != null : !(r.foreign != null && (stageName(r.foreign).toLowerCase().replace(/\s/g, '').includes(t.from) || String(r.foreign) === t.from))) return false; }
  if (t.here && (['yes', 'y', '1', 'true'].includes(t.here) ? !(r.count > 0) : r.count > 0)) return false;
  if (t.ok) { const o = t.ok, cc = r.compat?.cls;
    const ok = ['spawns', 'spawn', 'ok', 'green'].includes(o) ? cc === 'ok' : ['invisible', 'mid', 'amber'].includes(o) ? cc === 'mid' : ['crash', 'bad', 'red'].includes(o) ? cc === 'bad' : ['untested', 'unk', 'grey'].includes(o) ? cc === 'unk'
      : ['blocked', 'locked', 'no'].includes(o) ? r.place === 'blocked' : o === 'copies' ? r.place === 'copies' : ['placeable', 'yes'].includes(o) ? r.place !== 'blocked' : true;
    if (!ok) return false; }
  if (t.type) { const n = /^0x/.test(t.type) ? parseInt(t.type, 16) : /^[0-9a-f]{4}$/.test(t.type) && /[a-f]|^0/.test(t.type) ? NaN : +t.type, tm = r.template;
    if (!tm || tm.kind !== 'native') return false;
    if (!(tm.type === n || classHex(tm.list, tm.type) === t.type.padStart(4, '0') || r.typeWord === n || r.words?.[0]?.[1] === n || r.variants?.some(v => v.typeWord === n || v.words?.[0]?.[1] === n))) return false; }
  return true;
}
// filter the item list: counts per category (all filters but the category), what the Placeable only toggle hides,
// search matches inside a class's variants (the class shows with just those, expanded)
function addFilter(data, t) {
  const counts = {all: 0}, vmatch = new Map(); let hidden = 0, list = [];
  const base = addPrefs.fav || addPrefs.recent ? [...data.byKey.values()].filter(r => (!addPrefs.fav || addFav.has(r.key)) && (!addPrefs.recent || addRecent.includes(r.key))) : data.rows;
  for (const r of base) {
    if (addPrefs.here && r.foreign != null) continue;
    if (!tokenMatch(r, t)) continue;
    if (r.tilePiece && addPrefs.cat === 'tilesets' && tileKitOn() && r.tilePiece.kit !== addPrefs.kit) continue;   // the kit chips of the Tilesets tab
    if (t.text.length && !t.text.every(w => r.hay.includes(w))) {
      const vs = (r.variants || []).filter(v => t.text.every(w => (r.hay + ' ' + v.name.toLowerCase()).includes(w))); if (!vs.length) continue; vmatch.set(r, vs);
    }
    const inCat = addPrefs.cat === 'all' || r.cat === addPrefs.cat || (addPrefs.cat === 'bosses' && !!r.boss);   // Bosses: a view of the boss classes (they stay enemies)
    if (r.place === 'blocked' && addPrefs.placeable && t.ok !== 'blocked' && t.ok !== 'locked') { if (inCat) hidden++; continue; }
    counts[r.cat] = (counts[r.cat] || 0) + 1; counts.all++; if (r.boss) counts.bosses = (counts.bosses || 0) + 1;
    if (!inCat) continue;
    list.push(r);
  }
  const rank = r => r.kind === 'class' ? (r.foreign != null ? 2 : 1) : 0;
  // a search ranks by the name: exact 4, starts with 3, a word starts with 2, contains 1, only its class / types / stage 0
  const q = t.text.join(' '), score = r => { if (!q || vmatch.has(r)) return 0; const n = r.name.toLowerCase(); return n === q ? 4 : n.startsWith(q) ? 3 : (' ' + n).includes(' ' + q) ? 2 : n.includes(q) ? 1 : 0; };
  const direct = score;
  const order = (a, b) => score(b) - score(a) || rank(a) - rank(b) || (rank(a) === 1 ? b.count - a.count : 0) || a.name.localeCompare(b.name);
  let sections;
  if (addPrefs.recent) { list.sort((a, b) => addRecent.indexOf(a.key) - addRecent.indexOf(b.key)); sections = [{cat: null, rows: list}]; }
  else if (addPrefs.cat === 'all') { sections = ADD_ORDER.map(cat => ({cat, rows: list.filter(r => r.cat === cat).sort(order)})).filter(x => x.rows.length); if (q) sections.sort((a, b) => score(b.rows[0]) - score(a.rows[0])); }
  else sections = [{cat: null, rows: list.sort(order)}];
  return {counts, hidden, vmatch, sections, shown: list.length, sectioned: addPrefs.cat === 'all' && !addPrefs.recent};
}
function buildRule(r) {
  if (r.place === 'blocked') return 'Not placeable yet: ' + r.reason;
  if (r.kind === 'stagecode') return 'Read-only: the stage code places and moves it; the editor only shows it';
  if (r.kind === 'prefab') return 'Inserts its placements; each one follows its own build rule';
  if (r.kind === 'asset') return r.template ? 'The build writes it into the chunk it is placed in (geometry + collision)' : 'Kept in the project; the build copies it into the mod files when something uses it';
  if (r.kind === 'tool') return r.cat === 'shapes' ? 'The build writes it into the chunk it is placed in (geometry + collision)' : r.cat === 'paths' ? 'The build writes it into PATH.PTP' : r.cat === 'lighting' ? 'Kept with the project for the lighting engine (runtime)' : r.cat === 'key' ? "The build writes it into the stage parameters (nukkoro2.inf)" : 'Kept with the project; the runtime runs its logic (sidecar)';
  const t = r.template, c = catalog.get(`${t.list}-${t.type}`);
  if (r.foreign == null) return "The build writes its SET record (this stage loads the class already)";
  if (r.copies) return `The build copies its models and textures from ${stageName(r.foreign)} (dependency compiler); it tells you if a file is missing`;
  return (c?.setidStages || []).includes(project.stage) ? 'This stage allows the class already; the build writes its SET record' : 'The build patches setid.bin so this stage loads the class, then writes its SET record';
}
const addCard = Object.assign(document.createElement('div'), {className: 'addCard', hidden: true}); document.body.append(addCard);
function showAddCard(r, el) {
  const t = r.template, ck = t?.kind === 'native' ? `${t.list}-${t.type}` : '', c = ck ? catalog.get(ck) : null;
  let o = t; if (t && (r.words || r.typeWord !== undefined) && ck) { o = structuredClone(t); if (r.typeWord !== undefined) setParamWord(o, 0, r.typeWord); for (const [i, v, f] of r.words || []) setParamWord(o, i, v, !!f); }
  const params = c && ck ? (c.fields || []).slice(0, 4).map((f, i) => { const v = paramWord(o, i, f.type === 'float'); if (v == null) return ''; const opt = f.options?.find(x => x[0] === v); return `<span>${esc(f.name)}</span><b>${esc(opt ? opt[1] : f.type === 'float' ? round2(v) : v)}</b>`; }).join('') : '';
  const url = thumbFor(r, 'l'), note = ck ? R.drawNotes[ck] : '';
  addCard.innerHTML = `<div class="cardPic" style="--c:${addColour(r.cat)}${url ? `;background-image:url(${url})` : ''}">${url ? '' : `<i class="agl">${ADD_GLYPH[r.cat]}</i>`}</div>
    <b>${esc(r.name)}</b><small>${esc(ADD_LABEL[r.cat])}${ck ? ' · class ' + classHex(t.list, t.type).toUpperCase() : ''}${r.foreign != null ? ' · from ' + esc(stageName(r.foreign)) : ''}${r.count ? ` · ${r.count} in stage` : ''}</small>
    <p>${esc(r.sub || (c ? `${classOf(t)}${c.listName && c.listName !== classOf(t) ? ' (' + c.listName + ')' : ''}: a class this stage loads` : r.name))}</p>
    ${params ? `<div class="cardParams">${params}</div>` : ''}${r.compat ? `<p class="cc ${r.compat.cls}">${esc(r.compat.text)}</p>` : ''}${note ? `<p class="cc mid">Model: ${esc(note)}</p>` : ''}
    <p class="rule">${esc(buildRule(r))}</p><small class="keys">${r.action ? 'Click runs it' : 'Click, then click where it goes · drag into the view · Enter arms'} · F favourite${r.variants ? ` · Space: its ${r.variants.length} types` : ''}</small>`;
  const pr = $('panel').getBoundingClientRect(), er = el.getBoundingClientRect(); addCard.hidden = false;
  addCard.style.left = Math.round(pr.right + 8) + 'px'; addCard.style.top = Math.round(Math.max(8, Math.min(er.top, innerHeight - addCard.offsetHeight - 8))) + 'px';
}
function hideAddCard() { try { clearTimeout(showAddCard.t); addCard.hidden = true; } catch {} }   // (showPanel may run before this part of the file)
function cancelPlacing() { placing = null; placingKeep = false; tileTurn = null; renderer.domElement.style.cursor = ''; if (panel === 'add') renderAdd.refresh?.(); }
function placeAtCentre() { if (!placing) return; const r = placing; place(r, centrePoint()); if (!placingKeep) cancelPlacing(); else renderAdd.refresh?.(); }
function armRow(r, keep = false) {
  hideAddCard(); if (!r) return;
  if (r.place === 'blocked') return toast('Not yet: ' + r.reason, true);
  if (r.action) { placing = null; placingKeep = false; renderer.domElement.style.cursor = ''; pushRecent(r.key); return r.action(); }
  if (placing?.key === r.key && !keep) return cancelPlacing();
  placing = r; placingKeep = keep; renderer.domElement.style.cursor = 'crosshair'; tileTurn = null;
  if (r.tilePiece) { const c = renderer.domElement.getBoundingClientRect(); updateTileGhost(c.left + c.width / 2, c.top + c.height / 2); }
  toast(`Click where the ${r.placeName || r.name} goes${keep ? ' (keeps placing)' : ''}${r.tilePiece ? ' · R turns it 90° (Shift R back)' : ''} · Enter puts it at the view centre · Esc cancels`); renderAdd.refresh?.(false);
}
function renderAdd(body) {
  const prevScroll = $('vgrid')?.scrollTop ?? renderAdd.scroll ?? 0, focusSearch = document.activeElement?.id === 'aq' || renderAdd.autoFocus; renderAdd.autoFocus = false; hideAddCard();
  if (addSource() === 'all') loadLibs();
  body.innerHTML = `<div class="searchBox"><svg viewBox="0 0 24 24"><circle cx="11" cy="11" r="6"/><path d="M20 20l-4-4"/></svg><input id="aq" placeholder="${addPrefs.cat === 'tilesets' ? 'Search tiles: name, tag, kit:roads   ( / )' : 'Search things to add   ( / )'}" value="${esc(addQuery)}" title="Free text plus tokens: cat:enemies from:glyphic ok:spawns here:yes type:17 kit:roads · Enter arms the first result (Shift+Enter keeps placing) · ↓ goes to the tiles"></div>
   <div class="chipRow" id="addCats"></div>
   <div class="fchips" id="addFilters"></div>
   <div class="addMenu" id="addMenu" hidden><label class="allTypes"><input type="checkbox" id="allTypes" ${addPrefs.allTypes ? 'checked' : ''}> Show every class's types (variants) open</label><div class="tileSize"><span>Tile size</span><input type="range" min="70" max="460" step="2" id="tileW" value="${tileW}" title="Smaller = more tiles per row; all the way right = one big tile per row"></div></div>
   <div class="pinRow" id="pinRow" hidden></div>
   <div class="vwrap"><div class="secFloat" id="secFloat" hidden></div><div class="vgrid" id="vgrid" tabindex="0"><div class="vsizer" id="vsizer"></div></div></div>
   <div class="addStatus" id="addStatus"></div>`;
  const aq = $('aq'), grid = $('vgrid'), sizer = $('vsizer'), gap = 6;
  let data, res, entries = [], lines = [], navPos = new Map(), cols = 1, colW = 100, tileH = 100, painted = '', focusKey = null;
  const bucket = () => addPrefs.list || tileW * devicePixelRatio <= 300 ? 's' : 'l';
  const rowOf = k => data.byKey.get(k);
  function tileHTML(r, mode, style = '') {
    const c = addColour(r.cat), b = mode === 'sub' || mode === 'pin' || mode === 'lst' ? 's' : bucket(), url = r.thumbUrl || thumbs.get(thumbKeyOf(r, b)), fav = addFav.has(r.key);
    const badges = `${r.compat ? `<i class="cdot ${r.compat.cls}" title="${esc(r.compat.text)}"></i>` : ''}${r.place === 'blocked' ? `<i class="lock" title="${esc('Not placeable yet: ' + r.reason)}"><svg viewBox="0 0 16 16"><rect x="3" y="7" width="10" height="7" rx="1.5"/><path d="M5 7V5a3 3 0 0 1 6 0v2" fill="none"/></svg></i>` : ''}${r.copies ? `<i class="copies" title="${esc(`The build copies its files from ${stageName(r.foreign)}`)}">⇣</i>` : ''}${fav ? '<i class="fav" title="Favourite (F)">★</i>' : ''}${r.boss ? `<i class="bossT ${r.boss[0]}" title="${esc('Boss: ' + BOSS_PORT[r.boss[0]][0] + '. ' + r.boss[1])}">${r.boss[0] === 'ready' ? '♛' : '♛ ' + BOSS_PORT[r.boss[0]][0]}</i>` : ''}`;
    return `<div class="vt ${mode} ${placing?.key === r.key ? 'on' : ''} ${r.place === 'blocked' ? 'off' : ''}" data-k="${esc(r.key)}" draggable="${r.place !== 'blocked' && !r.action}" tabindex="-1" style="${style}">
      <div class="th ${url ? 'has' : ''} ${url && thumbSrc.get(thumbKeyOf(r, b)) === 'catalog' ? 'guess' : ''}" style="--c:${c}${url ? `;background-image:url(${url})` : ''}" data-b="${b}"><i class="agl">${ADD_GLYPH[r.cat]}</i></div>
      ${badges ? `<div class="bdg">${badges}</div>` : ''}${r.foreign != null && mode !== 'sub' ? `<em class="origin" title="From ${esc(stageName(r.foreign))}">${esc(stageName(r.foreign))}</em>` : ''}
      ${r.variants && mode !== 'sub' && mode !== 'pin' ? `<button class="types ${addExpanded.has(r.key) !== addPrefs.allTypes || res?.vmatch.has(r) ? 'open' : ''}" data-x="${esc(r.key)}" tabindex="-1" title="Show its types (Space)">${r.variants.length} types</button>` : ''}
      <div class="nm"><span class="dot" style="background:${c}"></span><b title="${esc(friendly(r.name))}${FRIENDLY[r.name] ? ` (${esc(r.name)})` : ''}">${esc(mode === 'sub' ? r.variantLabel || friendly(r.name) : friendly(r.name))}</b><small>${esc(r.small ?? (r.count ? `${r.count} in stage` : r.foreign != null ? '' : '0 in stage'))}</small></div></div>`;
  }
  function chips() {
    $('addCats').innerHTML = [['all', 'All'], ...ADD_ORDER.map(c => [c, ADD_LABEL[c]])].map(([c, l]) => `<button data-c="${c}" class="${addPrefs.cat === c ? 'on' : ''} ${(res.counts[c] || 0) ? '' : 'none'}" style="${addPrefs.cat === c && c !== 'all' ? `background:${addColour(c)}2b;border-color:${addColour(c)}` : ''}">${l}<small>${res.counts[c] || 0}</small></button>`).join('');
    const on = k => addPrefs[k] ? 'on' : '', dens = addPrefs.list ? 'list' : String(tileW);
    $('addFilters').innerHTML = `<button data-f="here" class="${on('here')}" title="Only classes this stage loads (off: every stage's library)">This stage only</button><button data-f="placeable" class="${on('placeable')}" title="${res.hidden ? res.hidden + ' not placeable yet are hidden' : 'Hides what the build cannot place yet'}">Placeable only${addPrefs.placeable && res.hidden ? `<small>${res.hidden} hidden</small>` : ''}</button><button data-f="fav" class="${on('fav')}" title="Only your favourites (F on a tile)">★ Favourites</button><button data-f="recent" class="${on('recent')}" title="The last 8 things you placed">Recent</button>
      ${addPrefs.cat === 'tilesets' ? `<span class="kitChips" id="kitChips">${[['', 'All kits', tilesetKits().reduce((n, k) => n + (k.pieces || []).length, 0)], ...tilesetKits().map(k => [k.kit, k.title || k.kit, (k.pieces || []).length])].map(([k, l, n]) => `<button data-kit="${esc(k)}" class="${(tileKitOn() ? addPrefs.kit : '') === k ? 'on' : ''}" title="${k ? `Only the ${esc(l)} (search: kit:${esc(k)})` : 'Every kit'}">${esc(l)}<small>${n}</small></button>`).join('')}</span>` : ''}
      <span class="dens">${[['76', 'S'], ['120', 'M'], ['220', 'L'], ['list', 'List']].map(([d, l]) => `<button data-d="${d}" class="${dens === d ? 'on' : ''}" title="${l === 'List' ? 'One line per item' : 'Grid tile size ' + l}">${l}</button>`).join('')}<button data-menu="1" title="Fine tile size" class="${$('addMenu').hidden ? '' : 'on'}">⋯</button></span>`;
  }
  function pins() {
    const show = !addQuery.trim() && addPrefs.cat === 'all' && !addPrefs.fav && !addPrefs.recent && !addPrefs.here && addPrefs.placeable;
    const rec = addRecent.map(rowOf).filter(Boolean), fav = [...addFav].map(rowOf).filter(r => r && !addRecent.includes(r.key));
    const el = $('pinRow'); el.hidden = !show || !(rec.length + fav.length); if (el.hidden) return;
    el.innerHTML = `${rec.length ? `<span class="pinLabel">Recent</span>${rec.map(r => tileHTML(r, 'pin')).join('')}` : ''}${fav.length ? `<span class="pinLabel">★</span>${fav.map(r => tileHTML(r, 'pin')).join('')}` : ''}`;
    queueThumbs(el);
  }
  function layout() {
    const W = Math.max(60, grid.clientWidth - 24), list = addPrefs.list;
    cols = list ? 1 : Math.max(1, Math.floor((W + gap) / (Math.min(tileW, W) + gap))); colW = (W - gap * (cols - 1)) / cols; tileH = list ? 40 : Math.round(colW * 3 / 5) + 30;
    const subW = 96, subCols = Math.max(1, Math.floor((W + gap) / (subW + gap))), subH = Math.round(((W - gap * (subCols - 1)) / subCols) * 3 / 5) + 26;
    entries = []; lines = []; navPos = new Map(); let y = 4;
    const add = e => { e.y = y; y += e.h; entries.push(e); return e; };
    const line = (keys, ei) => { lines.push({keys, ei}); keys.forEach((k, c) => navPos.set(k, [lines.length - 1, c])); };
    for (const sec of res.sections) {
      if (res.sectioned) { add({t: 'head', cat: sec.cat, n: sec.rows.length, h: 32}); if (addPrefs.collapsed.includes(sec.cat)) continue; }
      for (let i = 0; i < sec.rows.length; i += cols) {
        const tiles = sec.rows.slice(i, i + cols); add({t: 'row', tiles, h: tileH + gap}); line(tiles.map(r => r.key), entries.length - 1);
        for (const r of tiles) if (r.variants && (addExpanded.has(r.key) !== addPrefs.allTypes || res.vmatch.has(r))) {
          const vs = res.vmatch.get(r) || r.variants, n = Math.ceil(vs.length / subCols), e = add({t: 'band', parent: r, tiles: vs, subCols, subH, h: 30 + n * (subH + gap) + 8});
          for (let j = 0; j < vs.length; j += subCols) line(vs.slice(j, j + subCols).map(v => v.key), entries.length - 1); e.lines = n;
        }
      }
    }
    sizer.style.height = (y + 8) + 'px'; painted = '';
  }
  function entryHTML(e) {
    if (e.t === 'head') { const shut = addPrefs.collapsed.includes(e.cat); return `<div class="vhead" data-sec="${e.cat}" style="top:${e.y}px" title="${shut ? 'Show' : 'Hide'} this section"><i>${shut ? '▸' : '▾'}</i><span class="dot" style="background:${addColour(e.cat)}"></span>${esc(ADD_LABEL[e.cat])}<small>${e.n}</small></div>`; }
    if (e.t === 'row') return e.tiles.map((r, i) => tileHTML(r, addPrefs.list ? 'lst' : 'grid', `left:${12 + i * (colW + gap)}px;top:${e.y}px;width:${colW}px;height:${tileH}px`)).join('');
    return `<div class="vband" style="top:${e.y}px;height:${e.h - 8}px;--sub:${e.subCols};--subH:${e.subH}px"><div class="bandHead"><span class="dot" style="background:${addColour(e.parent.cat)}"></span>${esc(e.parent.name)} · ${res.vmatch.has(e.parent) ? `${e.tiles.length} of ${e.parent.variants.length}` : e.parent.variants.length} types${e.parent.variantField ? ` <small>(${esc(e.parent.variantField)})</small>` : ''}<button data-x="${esc(e.parent.key)}" tabindex="-1" title="Close (Space on the class tile)">✕</button></div><div class="bandGrid">${e.tiles.map(v => tileHTML(v, 'sub')).join('')}</div></div>`;
  }
  function paint(force = false) {
    const top = grid.scrollTop, bot = top + grid.clientHeight, m = tileH * 2 + 60;
    let a = 0; while (a < entries.length && entries[a].y + entries[a].h < top - m) a++; let b = a; while (b < entries.length && entries[b].y < bot + m) b++;
    const sig = a + ':' + b; if (force || sig !== painted) {
      painted = sig; const had = sizer.contains(document.activeElement);
      sizer.innerHTML = entries.length ? entries.slice(a, b).map(entryHTML).join('') : `<div class="vempty">${addQuery.trim() ? `Nothing matches <code>${esc(addQuery.trim())}</code>. Try fewer words` : 'Nothing here with these filters'}${res.hidden ? `, or turn off <b>Placeable only</b> (${res.hidden} hidden)` : ''}.</div>`;
      queueThumbs(sizer);
      if (had && focusKey) { const el = sizer.querySelector(`[data-k="${CSS.escape(focusKey)}"]`); if (el) el.focus({preventScroll: true}); else { grid.dataset.quiet = 1; grid.focus({preventScroll: true}); delete grid.dataset.quiet; } }
    }
    const head = res.sectioned ? entries.filter(e => e.t === 'head' && e.y < top + 2).pop() : null, sf = $('secFloat');
    sf.hidden = !head || top <= head.y; if (!sf.hidden) { sf.dataset.sec = head.cat; sf.innerHTML = entryHTML({...head, y: 0}).replace('class="vhead"', 'class="vhead float"'); }
  }
  function queueThumbs(root) {
    for (const el of root.querySelectorAll('.th:not(.has)')) { const r = rowOf(el.closest('.vt').dataset.k); if (!r) continue; const b = el.dataset.b;
      if (r.foreign != null && !R.foreign.has(r.foreign)) foreignThumb(r, el, b); thumbQueue.push([el, r, b]); }
    fillThumbs();
  }
  function status() {
    const p = placing, free = memInfo?.stages?.[String(project.stage)]?.free;
    $('addStatus').innerHTML = `${res.shown} shown${res.hidden && addPrefs.placeable ? ` · ${res.hidden} not placeable hidden` : ''}${addSource() === 'all' && libsState.done < libsState.total ? ` · reading ${esc(libsState.reading || 'the other stages')}'s library… ${libsState.done} / ${libsState.total}` : ''}${p ? ` · <b>placing ${esc(p.placeName || p.name)}</b>${p.copies ? ` (copies files from ${esc(stageName(p.foreign))}${free ? `; ${(free / 1048576).toFixed(1)} MB free at this stage's start, measured` : ''})` : ''}${placingKeep ? ' · keeps placing' : ''}${p.tilePiece ? ` · facing ${tileYaw()}°${tileTurn == null ? ' (the camera)' : ''}, R turns` : ''} · Esc cancels` : ' · drag a tile into the view, or click it then click where it goes'}`;
  }
  function refresh(scrollTop = false) {
    if (!grid.isConnected) return;   // a late refresh (search debounce, the tileset list arriving) after the Add panel closed
    data = addItems(); res = addFilter(data, parseAddQuery(addQuery)); chips(); pins(); layout(); if (scrollTop === true) grid.scrollTop = 0; paint(true); status();
  }
  renderAdd.refresh = (relayout = true) => { if (!$('vgrid') || panel !== 'add') return; if (relayout) refresh(); else { chips(); pins(); paint(true); status(); } };
  function focusTile(key) {
    const p = navPos.get(key); if (!p) return; const e = entries[lines[p[0]].ei]; focusKey = key;
    if (e.y < grid.scrollTop || e.y + e.h > grid.scrollTop + grid.clientHeight) { grid.scrollTop = Math.max(0, e.y - (e.t === 'band' ? 0 : 40)); }
    paint(); const el = sizer.querySelector(`[data-k="${CSS.escape(key)}"]`); if (el) { el.focus(); showAddCard.t = setTimeout(() => el.isConnected && document.activeElement === el && showAddCard(rowOf(key), el), 150); }
  }
  const toggleExpand = key => { if (addExpanded.has(key)) addExpanded.delete(key); else addExpanded.add(key); const st = grid.scrollTop; layout(); grid.scrollTop = st; paint(true); };
  // ---- events (delegated: the tiles are re-made while scrolling) ----
  aq.oninput = () => { addQuery = aq.value; clearTimeout(aq.t); aq.t = setTimeout(() => { aq.t = 0; refresh(true); }, 90); };
  aq.onkeydown = e => {
    if (aq.t && ['Enter', 'ArrowDown'].includes(e.key)) { clearTimeout(aq.t); aq.t = 0; addQuery = aq.value; refresh(true); }
    if (e.key === 'Enter') { e.preventDefault(); const k = lines[0]?.keys[0], r = k && rowOf(k); if (!r) return toast('Nothing matches', true); if (placing?.key === r.key) placeAtCentre(); else armRow(r, e.shiftKey); }
    else if (e.key === 'ArrowDown' && lines.length) { e.preventDefault(); focusTile(lines[0].keys[0]); }
    else if (e.key === 'Escape') { e.preventDefault(); if (placing) cancelPlacing(); else if (aq.value) { aq.value = addQuery = ''; refresh(true); } else aq.blur(); }
  };
  body.querySelector('#tileW').oninput = e => { tileW = +e.target.value; addPrefs.list = false; saveAddPrefs(); applyTileW(); store('ui2.tileW', tileW); cancelAnimationFrame(renderAdd.raf); renderAdd.raf = requestAnimationFrame(() => { chips(); layout(); paint(true); }); };
  body.querySelector('#allTypes').onchange = e => { addPrefs.allTypes = e.target.checked; addExpanded.clear(); saveAddPrefs(); refresh(); };
  body.querySelector('#addCats').onclick = e => { const b = e.target.closest('[data-c]'); if (!b) return; addPrefs.cat = b.dataset.c; saveAddPrefs(); refresh(true); aq.placeholder = addPrefs.cat === 'tilesets' ? 'Search tiles: name, tag, kit:roads   ( / )' : 'Search things to add   ( / )'; };
  body.querySelector('#addCats').onwheel = e => { if (Math.abs(e.deltaY) > Math.abs(e.deltaX)) { e.currentTarget.scrollLeft += e.deltaY; e.preventDefault(); } };
  body.querySelector('#addFilters').onclick = e => {
    const f = e.target.closest('[data-f]'), d = e.target.closest('[data-d]'), m = e.target.closest('[data-menu]'), kb = e.target.closest('[data-kit]');
    if (kb) { addPrefs.kit = kb.dataset.kit; saveAddPrefs(); refresh(true); }
    else if (f) { addPrefs[f.dataset.f] = !addPrefs[f.dataset.f]; saveAddPrefs(); if (f.dataset.f === 'here' && !addPrefs.here) loadLibs(); refresh(true); }
    else if (d) { if (d.dataset.d === 'list') addPrefs.list = true; else { addPrefs.list = false; tileW = +d.dataset.d; applyTileW(); store('ui2.tileW', tileW); $('tileW').value = tileW; } saveAddPrefs(); refresh(); }
    else if (m) { $('addMenu').hidden = !$('addMenu').hidden; chips(); }
  };
  const wireTiles = root => {
    root.addEventListener('click', e => {
      const x = e.target.closest('[data-x]'); if (x) { e.stopPropagation(); toggleExpand(x.dataset.x); return; }
      const h = e.target.closest('.vhead'); if (h) { const c = h.dataset.sec, i = addPrefs.collapsed.indexOf(c); if (i >= 0) addPrefs.collapsed.splice(i, 1); else addPrefs.collapsed.push(c); saveAddPrefs(); layout(); const hd = entries.find(en => en.t === 'head' && en.cat === c); if (hd && grid.scrollTop > hd.y) grid.scrollTop = hd.y; paint(true); return; }
      const t = e.target.closest('.vt'); if (t) { focusKey = t.dataset.k; armRow(rowOf(t.dataset.k), e.shiftKey); }
    });
    root.addEventListener('dragstart', e => { const t = e.target.closest('.vt'), r = t && rowOf(t.dataset.k); hideAddCard(); if (!r || r.action || r.place === 'blocked') return e.preventDefault(); e.dataTransfer.setData('text/plain', 'ui2-add'); e.dataTransfer.effectAllowed = 'copy'; dragRow = r; });
    root.addEventListener('mouseover', e => { const t = e.target.closest('.vt'); if (!t || t === showAddCard.el) return; showAddCard.el = t; clearTimeout(showAddCard.t); addCard.hidden = true; const r = rowOf(t.dataset.k);
      if (r?.foreign != null && (!r.disabled || r.bossPreview) && !R.foreign.has(r.foreign)) loadForeign(r.foreign);   // as before: a hovered foreign tile loads its stage's models
      showAddCard.t = setTimeout(() => t.isConnected && showAddCard(r, t), 400); });
    root.addEventListener('mouseleave', () => { showAddCard.el = null; hideAddCard(); });
    root.addEventListener('mouseout', e => { if (!e.relatedTarget?.closest?.('.vt')) { showAddCard.el = null; hideAddCard(); } });
  };
  wireTiles(sizer); wireTiles($('pinRow'));
  $('secFloat').onclick = () => { const c = $('secFloat').dataset.sec, i = addPrefs.collapsed.indexOf(c); if (i >= 0) addPrefs.collapsed.splice(i, 1); else addPrefs.collapsed.push(c); saveAddPrefs(); layout(); const hd = entries.find(en => en.t === 'head' && en.cat === c); if (hd) grid.scrollTop = hd.y; paint(true); };
  grid.onscroll = () => { hideAddCard(); renderAdd.scroll = grid.scrollTop; paint(); };
  grid.addEventListener('focus', e => { if (e.target === grid && !grid.dataset.quiet && lines.length) focusTile(focusKey && navPos.has(focusKey) ? focusKey : lines[0].keys[0]); });
  grid.addEventListener('focusin', e => { const t = e.target.closest?.('.vt'); if (t) focusKey = t.dataset.k; });
  grid.onkeydown = e => {
    const t = document.activeElement?.closest?.('.vt'); if (!t || !sizer.contains(t)) return; const r = rowOf(t.dataset.k), k = e.key; let done = true;
    if (k.startsWith('Arrow')) { const p = navPos.get(t.dataset.k); if (p) { let [li, ci] = p;
        if (k === 'ArrowLeft' || k === 'ArrowRight') { const flat = lines.flatMap(l => l.keys), i = flat.indexOf(t.dataset.k) + (k === 'ArrowRight' ? 1 : -1); if (flat[i]) focusTile(flat[i]); }
        else { li += k === 'ArrowDown' ? 1 : -1; if (lines[li]) focusTile(lines[li].keys[Math.min(ci, lines[li].keys.length - 1)]); else if (li < 0) aq.focus(); } } }
    else if (k === 'Enter') { if (placing?.key === r.key) placeAtCentre(); else armRow(r, e.shiftKey); }
    else if (k === ' ') { if (r.variants) toggleExpand(r.key); }
    else if (k.toLowerCase() === 'f' && !e.ctrlKey && !e.metaKey) { toggleFav(r.key); paint(true); pins(); }
    else if (k === 'Escape') { if (placing) cancelPlacing(); aq.focus(); }
    else if (k === '/') aq.focus();
    else done = false;
    if (done) { e.preventDefault(); e.stopPropagation(); }
  };
  renderAdd.ro?.disconnect(); renderAdd.ro = new ResizeObserver(() => { if (!grid.isConnected) return; const w = grid.clientWidth; if (w === renderAdd.w) return; renderAdd.w = w; layout(); paint(true); }); renderAdd.ro.observe(grid);
  refresh(); renderAdd.w = grid.clientWidth; grid.scrollTop = prevScroll; paint();
  if (focusSearch) { aq.focus(); aq.setSelectionRange(aq.value.length, aq.value.length); }
}
let dragRow = null;
// enemy compatibility sweep: {stage: {LLTT: {result: spawn | spawn-no-model | crash | hang | no-template, detail}}}
let compat = null; fetch('../capture/enemy-compat.json').then(r => r.ok ? r.json() : null).then(j => { compat = j; if (panel === 'add') renderPanel(); }).catch(() => {});
function compatBadge(t) {
  if (!compat) return ''; const k = ((t.list << 8) | t.type).toString(16).toUpperCase().padStart(4, '0');
  const here = compat[String(project.stage)]?.[k], any = here || Object.entries(compat).map(([s, v]) => v[k] && {...v[k], tested: +s}).find(Boolean); if (!any) return '';
  const ok = any.result === 'spawn', icon = ok ? '✓' : any.result === 'spawn-no-model' ? '◐' : any.result === 'no-template' ? '?' : '✗';
  const where = here ? 'in this stage' : `in ${stageName(any.tested ?? any.sourceStage)}`;
  return `<em class="compat ${ok ? 'ok' : any.result === 'crash' || any.result === 'hang' ? 'bad' : 'mid'}" title="Enemy sweep ${where}: ${esc(any.result)}${any.detail ? ' · ' + esc(any.detail) : ''}">${icon}</em>`;
}
const thumbs = new Map(), thumbSrc = new Map(); let thumbR = null, thumbQueue = [], thumbBusy = false;
const thumbKeyOf = (row, bucket = 's') => (row.thumbKey || (row.shape ? 'shape-' + row.name : row.template ? `${row.template.list}-${row.template.type}-${row.typeWord ?? ''}${row.words ? '-' + row.words.map(w => w[0] + '=' + w[1]).join(',') : ''}${row.foreign != null ? '@' + row.foreign : ''}` : 'none')) + (bucket === 'l' ? '#l' : '');
function setThumb(el, url, row, bucket) { el.style.backgroundImage = `url(${url})`; el.classList.add('has'); if (thumbSrc.get(thumbKeyOf(row, bucket)) === 'catalog') el.classList.add('guess'); }
function thumbFor(row, bucket = 's') {
  if (!row || row.action || row.prefab || row.stageId) return '';
  if (row.newPath || row.editor || row.moveTo !== undefined || !(row.shape || row.template)) return '';
  const key = thumbKeyOf(row, bucket); if (thumbs.has(key)) return thumbs.get(key);
  if (!models && !row.shape) return null;
  const o = row.shape ? {...createShape(row.shape, [0, 0, 0], row.dims), ...row.extra, material: null} : structuredClone(row.template);
  if (!row.shape) { const before = o.params; if (row.typeWord !== undefined) setParamWord(o, 0, row.typeWord); for (const [i, v, f] of row.words || []) setParamWord(o, i, v, !!f); if (o.params !== before) delete o.sourceIndex; }   // another type than the template's: not the template record's captured model
  const n = objectNode(o); if (!n && row.foreign != null && !R.foreign.has(row.foreign)) return null;   // its stage's models are not loaded yet
  if (!n || n.userData.volume) { thumbs.set(key, ''); return ''; }
  if (!thumbR) { thumbR = new THREE.WebGLRenderer({antialias: true, alpha: true, preserveDrawingBuffer: true}); thumbR.outputColorSpace = THREE.SRGBColorSpace; }
  const [tw, tt] = bucket === 'l' ? [520, 312] : [250, 150]; if (thumbR.domElement.width !== tw) thumbR.setSize(tw, tt); thumbSrc.set(key, n.userData.modelSource || '');
  const sc = new THREE.Scene(); sc.add(new THREE.HemisphereLight(0xffffff, 0x50586a, 2.2)); const dl = new THREE.DirectionalLight(0xffffff, 1.6); dl.position.set(2, 3, 2); sc.add(dl); sc.add(n);
  const box = new THREE.Box3().setFromObject(n), size = Math.max(4, box.getSize(new THREE.Vector3()).length() || 20), c = box.getCenter(new THREE.Vector3());
  if (!Number.isFinite(size) || ![c.x, c.y, c.z].every(Number.isFinite)) { thumbs.set(key, ''); return ''; }
  // #84 fit the box (all 8 corners) into the view with a 10% margin, centred on the box centre; any shape (tall pillars,
  // flat panels, huge ships) - the old fixed distance (1.35 x the diagonal) cut tall models at the top and bottom
  const cam = new THREE.PerspectiveCamera(32, 250 / 150, 1, 10), dir = new THREE.Vector3(.75, .55, 1).normalize(), corners = [];
  n.updateMatrixWorld(true); n.traverse(m => { if (!m.isMesh || !m.geometry?.attributes?.position) return; const pa = m.geometry.attributes.position, step = Math.max(1, Math.ceil(pa.count / 600)); for (let i = 0; i < pa.count; i += step) corners.push(new THREE.Vector3().fromBufferAttribute(pa, i).applyMatrix4(m.matrixWorld)); });   // the model's own points (tighter than its box)
  if (!corners.length) for (const x of [box.min.x, box.max.x]) for (const y of [box.min.y, box.max.y]) for (const z of [box.min.z, box.max.z]) corners.push(new THREE.Vector3(x, y, z));
  const tv = Math.tan(THREE.MathUtils.degToRad(cam.fov / 2)) * .9, th = tv * cam.aspect, _q = new THREE.Vector3();
  const fits = d => { cam.position.copy(c).addScaledVector(dir, d); cam.lookAt(c); cam.updateMatrixWorld(); const inv = cam.matrixWorldInverse;
    return corners.every(k => { _q.copy(k).applyMatrix4(inv); const z = -_q.z; return z > 0 && Math.abs(_q.x) <= z * th && Math.abs(_q.y) <= z * tv; }); };
  let lo = size * .05, hi = size * 6; for (let i = 0; i < 24; i++) { const mid = (lo + hi) / 2; if (fits(mid)) hi = mid; else lo = mid; }
  cam.position.copy(c).addScaledVector(dir, hi); cam.lookAt(c); cam.near = Math.max(.1, hi - size); cam.far = hi + size * 2; cam.updateProjectionMatrix();
  thumbR.render(sc, cam); const url = thumbR.domElement.toDataURL('image/png'); thumbs.set(key, url); return url;
}
// #70: why a tile has no picture / its model is drawn differently (render3d ctx.drawNotes per class)
function drawNoteOn(el, row, url) {
  const t = el.closest('.vt'); if (!t || !row.template || row.template.kind !== 'native') return;
  const why = R.drawNotes[`${row.template.list}-${row.template.type}`] || (url === '' && models ? 'no model found for this class: placed as a category marker' : '');
  if (!why || el.querySelector('.tileNote')) return; el.insertAdjacentHTML('beforeend', `<span class="tileNote" title="${esc('Model: ' + why)}">!</span>`);
}
function fillThumbs() {   // a few per frame so the panel opens instantly; tiles scrolled away (no longer in the page) are skipped
  if (thumbBusy) return; thumbBusy = true;
  const step = () => { const t0 = performance.now(); while (thumbQueue.length && performance.now() - t0 < 8) { const [el, row, b] = thumbQueue.shift(); if (!el.isConnected || el.classList.contains('has')) continue; const url = thumbFor(row, b); if (url) setThumb(el, url, row, b); drawNoteOn(el, row, url); }   // #85 an 8 ms budget per tick: scrolling stays smooth while pictures render
    if (thumbQueue.length) setTimeout(step, 0); else thumbBusy = false; }; setTimeout(step, 0);
}
function surfacePoint(clientX, clientY) {
  const r = renderer.domElement.getBoundingClientRect(); ndc.set((clientX - r.left) / r.width * 2 - 1, -(clientY - r.top) / r.height * 2 + 1); ray.setFromCamera(ndc, camera);
  const hit = ray.intersectObjects([...art.children.filter(m => m.visible && !m.userData.backdrop && !m.userData.shellIn), ...nodes.children.filter(n => !n.userData.volume)], true)[0];
  if (hit) return hit.point;
  const p = new THREE.Vector3(); return ray.ray.intersectPlane(new THREE.Plane(new THREE.Vector3(0, 1, 0), -orbit.target.y), p) ? p : orbit.target.clone();
}
// the runnable loops (loops-56.md): POST /api/loop-kit -> a track mesh (surface 0x60008001, loopKit {radius, width})
// + a Loop path (pathType 'loop', loopKit = the track id) on top of it, inserted as one group; it starts on the clicked
// floor and runs the way the camera looks (yaw 0 = toward -z)
async function placeLoopKit(point) {
  const d = orbit.target.clone().sub(camera.position), yaw = round2(THREE.MathUtils.radToDeg(Math.atan2(-d.x, -d.z)));
  let r; try { toast('Making the loop…'); r = await api('/api/loop-kit', {project: project.id, entry: point.toArray().map(round2), yaw, radius: 250, width: 120, runIn: 300, runOut: 400}); }
  catch (e) { return toast('Loop: ' + e.message, true); }
  const rows = [...(r.geometry || []), ...(r.paths || [])]; if (!rows.length) return toast('Loop: nothing came back', true);
  const g = crypto.randomUUID(); for (const o of rows) o.group = g;
  snapshot(); const added = insertObjects(rows); changed(); setSelection(added); buildMarkers(); drawFlow();
  toast('Loop placed: track + Loop path, one group (move / turn it as a whole; Ctrl Z undoes)');
}
// ---- #128 tilesets (the kits: work/tilesets/<kit>/kit.json + <piece>.glb; GET /api/tilesets, POST /api/tileset-piece
// {project, kit, piece, position, yaw} -> geometry rows + materials through the #119 importer). Grid 50 units (5 m): a piece's
// pivot is its footprint's MIN corner on the floor and it spans +x +y +z; turns in 90° steps. Until the server has the
// routes a placeholder kit of plain boxes stands in (same grid and pivot), so the editor side can be used and tested.
const TILE_GRID = 50;
const TILE_PLACEHOLDER = {kit: 'placeholder', title: 'Placeholder kit', grid: TILE_GRID, placeholder: true, pieces: [
  {id: 'road-straight', name: 'Road straight', cells: [2, 0, 4], size: [100, 6, 200], tags: ['road'], surface: 0x00020001},
  {id: 'wall', name: 'Wall', cells: [2, 3, 0], size: [100, 150, 10], tags: ['building', 'wall'], surface: 0x60000001},
  {id: 'block', name: 'Block', cells: [2, 2, 2], size: [100, 100, 100], tags: ['building'], surface: 0x60000001}]};
let tilesetsList = null, tilesetsAsked = false;
function tilesetKits() {
  if (!tilesetsAsked) { tilesetsAsked = true; api('/api/tilesets').then(r => { tilesetsList = r.kits?.length ? r.kits : null; addCache = null; if (panel === 'add') renderPanel(); }).catch(() => { tilesetsList = null; }); }
  return tilesetsList || [TILE_PLACEHOLDER];
}
const tileSnap = v => Math.round(v / TILE_GRID) * TILE_GRID;
// the importer answers with the piece's own draw box per chunk ({chunk: [[lo, hi]]}); a level import REPLACES a chunk's
// boxes with it, a tile ADDS its box (else each tile would shrink the chunk's draw region to itself). A chunk of an existing
// stage without authored boxes keeps its own region (the tile draws where that chunk draws); past 64 boxes they merge.
function addTileBoxes(boxes) {
  const cb = (project.chunkBoxes ||= {});
  for (const [k, list] of Object.entries(boxes || {})) {
    if (!cb[k] && project.mode !== 'new') continue;
    const all = [...(cb[k] || []), ...list];
    cb[k] = all.length <= 64 ? all : [[[0, 1, 2].map(i => Math.min(...all.map(b => b[0][i]))), [0, 1, 2].map(i => Math.max(...all.map(b => b[1][i])))]];
  }
  if (!Object.keys(cb).length) delete project.chunkBoxes;
}
let tileTurn = null;   // R while placing a tile: a fixed turn (degrees) instead of the camera's; a new arm / Esc drops it
const tileKitOn = () => !!addPrefs.kit && tilesetKits().some(k => k.kit === addPrefs.kit);
function turnTile(dir) { tileTurn = ((tileTurn ?? tileYaw()) + (dir < 0 ? 270 : 90)) % 360; updateTileGhost(); renderAdd.refresh?.(false); toast(`${placing.placeName || placing.name}: facing ${tileTurn}° (R turns, Shift R back)`); }
// the ghost: the piece's footprint box where a click would put it (grid-snapped pivot, the turn it would get); the
// brighter wedge marks its -z side (yaw 0 = that side away from the camera)
const tileGhost = new THREE.Group(); tileGhost.visible = false; tileGhost.renderOrder = 20; scene.add(tileGhost);
function tileDims(pc) { const s = pc.size || (pc.cells || [2, 2, 2]).map(c => c * TILE_GRID); return [Math.max(s[0], 1), Math.max(s[1], 4), Math.max(s[2], 1)]; }
function updateTileGhost(clientX, clientY) {
  const pc = placing?.tilePiece ? placing.piece || {} : null; if (!pc || !project) { tileGhost.visible = false; return; }
  if (tileGhost.userData.key !== placing.key) {
    clear(tileGhost); tileGhost.userData.key = placing.key; const [w, h, d] = tileDims(pc), col = 0x38bdf8;
    const box = new THREE.BoxGeometry(w, h, d); box.translate(w / 2, h / 2, d / 2);
    tileGhost.add(new THREE.Mesh(box, new THREE.MeshBasicMaterial({color: col, transparent: true, opacity: .18, depthWrite: false})));
    tileGhost.add(new THREE.LineSegments(new THREE.EdgesGeometry(box), new THREE.LineBasicMaterial({color: col, transparent: true, opacity: .9, depthTest: false})));
    const a = Math.min(w, d) * .3, tri = new THREE.BufferGeometry().setFromPoints([new THREE.Vector3(w / 2 - a / 2, 1, a * .2), new THREE.Vector3(w / 2 + a / 2, 1, a * .2), new THREE.Vector3(w / 2, 1, -a * .6)]);
    tileGhost.add(new THREE.Mesh(tri, new THREE.MeshBasicMaterial({color: 0xfacc15, side: THREE.DoubleSide, transparent: true, opacity: .9, depthTest: false})));
    tileGhost.traverse(n => { n.renderOrder = 20; });
  }
  const at = clientX == null ? tileGhost.userData.at : surfacePoint(clientX, clientY); if (!at) return; tileGhost.userData.at = at.clone();
  tileGhost.position.set(tileSnap(at.x), round2(at.y), tileSnap(at.z)); tileGhost.rotation.set(0, THREE.MathUtils.degToRad(tileYaw()), 0); tileGhost.visible = true;
}
renderer.domElement.addEventListener('pointermove', e => { if (placing?.tilePiece) updateTileGhost(e.clientX, e.clientY); else if (tileGhost.visible) tileGhost.visible = false; });
renderer.domElement.addEventListener('pointerleave', () => { tileGhost.visible = false; });
function tileYaw() { if (tileTurn != null) return tileTurn; const d = orbit.target.clone().sub(camera.position); return ((Math.round(THREE.MathUtils.radToDeg(Math.atan2(-d.x, -d.z)) / 90) * 90) % 360 + 360) % 360; }   // 0 = the piece's -z side toward the camera's view
async function placeTile(row, point) {
  const {kit, piece} = row.tilePiece, pos = [tileSnap(point.x), round2(point.y), tileSnap(point.z)], yaw = tileYaw(), pc = row.piece || {}, g = crypto.randomUUID();
  let rows = [], mats = [], boxes = {};
  if (kit === TILE_PLACEHOLDER.kit) {   // a plain box with its min corner at the pivot
    const [w, h, d] = pc.size || [100, 100, 100], o = createShape('box', pos, [w, h, d]); o.vertices = o.vertices.map(v => [round2(v[0] + w / 2), v[1], round2(v[2] + d / 2)]);
    rows = [{...o, name: pc.name, surface: pc.surface ?? o.surface, rotation: [0, yaw, 0]}];
  } else {
    try { toast(`Placing ${pc.name || piece}…`); const r = await api('/api/tileset-piece', {project: project.id, kit, piece, position: pos, yaw}); rows = r.geometry || []; mats = r.materials || []; boxes = r.chunkBoxes || {}; }
    catch (e) { return toast('Tileset: ' + e.message, true); }
    if (!rows.length) return toast('Tileset: nothing came back', true);
  }
  snapshot(); for (const m of mats) if (!project.materials.some(x => x.id === m.id)) project.materials.push(m);
  for (const o of rows) { o.group = g; o.tile = {kit, piece, yaw}; addGeometry(o, false); }
  addTileBoxes(boxes);
  const snapSet = !(snapState.grid > 0); if (snapSet) { snapState.grid = TILE_GRID; snapState.rot = 90; saveSnap(); applySnapUi(); }
  buildMarkers(); changed(); drawFlow(); setSelection(items.filter(x => x.o.group === g));
  toast(`${pc.name || piece} placed at ${pos[0]}, ${pos[2]} (grid ${TILE_GRID}), facing ${yaw}°${snapSet ? ` · Snap grid set to ${TILE_GRID} and turn to 90° for tiles (gizmo bar)` : ''}`);
}
function place(row, point) {
  if (row.waypoint !== undefined) return placeWaypoint(row.it, row.waypoint, point, ' at the mouse');
  if (row.importModel) return importModelAt(row, point);
  if (row.loopKit) return placeLoopKit(point);
  if (row.tilePiece) return placeTile(row, point);
  if (row.prefab) return insertPrefab(row.prefab, point);
  if (row.disabled && !row.bossPreview) return toast(row.reason, true);   // a portable boss: placed for its set-up (Issues says the build leaves it out)
  if (row.key) pushRecent(row.key);   // #85 Recent (per browser)
  if (row.moveTo !== undefined) return moveMarker(row.moveTo || startId(), point);
  snapshot(); const pos = point.toArray().map(v => Math.round(v * 100) / 100); let o;
  if (row.shape) o = {...createShape(row.shape, pos, row.dims), ...row.extra};
  else if (row.editor === 'trigger') o = {id: crypto.randomUUID(), name: 'Trigger ' + ((project.triggers || []).length + 1), kind: 'trigger', position: pos, rotation: [0, 0, 0], scale: [1, 1, 1], size: [100, 100, 100], event: 'enter', once: true, actions: []};
  else if (row.editor === 'teleport') o = {id: crypto.randomUUID(), name: 'Teleport pad ' + (items.filter(it => it.o.kind === 'teleport').length + 1), kind: 'teleport', position: pos, rotation: [0, 0, 0], scale: [1, 1, 1], target: '', speed: 150, cooldown: 1};
  else if (row.editor === 'destination') o = {id: crypto.randomUUID(), name: 'Teleport destination ' + (items.filter(it => it.o.kind === 'destination').length + 1), kind: 'destination', position: pos, rotation: [0, 0, 0], scale: [1, 1, 1]};
  else if (row.editor === 'lightregion') o = {id: crypto.randomUUID(), name: 'Lighting region ' + ((project.lighting?.regions || []).length + 1), kind: 'lightregion', position: pos.map((v, i) => i === 1 ? round2(v + 150) : v), rotation: [0, 0, 0], scale: [1, 1, 1], size: [600, 300, 600], follow: 'player', priority: 10, transition: 1.5, trigger: '', profile: {fog_density: .0006, fog_colour: [.3, .3, .35]}};
  else if (row.editor === 'light') o = {id: crypto.randomUUID(), name: 'Point light ' + ((project.lighting?.lights || []).length + 1), kind: 'light', position: pos.map((v, i) => i === 1 ? round2(v + 60) : v), rotation: [0, 0, 0], scale: [1, 1, 1], colour: [1, .85, .6], intensity: 2, radius: 400};
  else if (row.newPath) o = {id: crypto.randomUUID(), name: {rail: 'Grind rail', platform: 'Platform path', native: 'Spline'}[row.newPath] || 'Path', kind: 'path', position: pos, rotation: [0, 0, 0], scale: [1, 1, 1], pathType: typeof row.newPath === 'string' ? row.newPath : 'rail', splineId: nextSplineId(), autoOrient: true, closed: false, points: [[0, 0, 0], [0, 0, 150], [100, 30, 300]], banking: 0};   // classic defaults
  else {
    o = structuredClone(row.template); o.id = crypto.randomUUID(); if (o.kind === 'native') resetCopiedLinks([o], project.objects); o.position = pos; o.rotation = [0, 0, 0]; o.name = row.placeName || row.name;
    if (o.kind === 'path') { o.splineId = nextSplineId(); delete o.sourceIndex; }   // a copy is a new path: its own spline ID
    else if (o.kind !== 'geometry') { o.scale = [1, 1, 1]; delete o.sourceIndex; delete o.events; if (row.typeWord !== undefined) setParamWord(o, 0, row.typeWord); for (const [i, v, f] of row.words || []) setParamWord(o, i, v, !!f); if (!o.render) o.render = 10;
      const tails = Object.keys(project.native?.tails || {}); if (tails.length && !tails.includes(o.layer)) o.layer = tails.includes('cmn') ? 'cmn' : tails[0]; }   // a record from another stage may use a SET layer this stage lacks   // a duplicated shape keeps its size, chunk and material
  }
  listFor(o).push(o);
  if (o.kind === 'trigger' && hiddenCats.has('volumes')) { hiddenCats.delete('volumes'); renderLens(); }   // volumes start hidden: show them for the new one
  const it = makeItem(o); items.push(it); byId.set(it.id, it);
  const why = [!matches(it) && `the search "${query}"`, hiddenCats.has(it.cat) && `hidden ${CAT[it.cat].label}`, it.layer && hiddenLayers.has(it.layer) && `the hidden ${it.layer} layer`, hiddenZones.has(String(it.zone)) && `hidden zone ${it.zone}`].filter(Boolean);
  if (why.length) { if (!matches(it)) { query = ''; const q = $('q'); if (q) q.value = ''; } hiddenCats.delete(it.cat); if (it.layer) hiddenLayers.delete(it.layer); hiddenZones.delete(String(it.zone)); setTimeout(() => toast(`Added ${row.name} · it was hidden by ${why.join(' and ')}: that filter is off now`), 50); }
  addNode(it); if (snapOn()) { magnet(it, nodeOf.get(it)); relocate(it); setTimeout(snapClear, 1500); }   // #94
  buildMarkers(); changed(); select(it, false); drawFlow(); if (why.length) refreshVisibility(); else if (panel !== 'add') renderPanel();
  addCache = null; toast(o.kind === 'geometry' ? `Added ${row.name} · built into chunk ${o.chunk} · ${o.renderVisible === false ? 'invisible' : project.materials.find(m => m.id === o.material)?.texture || 'no material'}${o.shape === 'loop' ? ' · loop running is not integrated in game yet' : ''}` : `Added ${row.name} · zone ${it.zone ?? '-'} · ${Math.round(it.progress * 100)}% of the run`);
}
// Authored shapes (classic createShape / defaultChunk / defaultMaterial; server.py default_chunk agrees): the chunk is
// one with a visual world and collision whose bounds contain the point (else the nearest), the material is a textured
// one of that chunk's main world (target, else O, else OS, else any of the chunk)
function createShape(type, position, size) {
  const [, label, base] = SHAPES.find(s => s[0] === type), dims = size || base, chunk = defaultChunk(position) ?? 1;
  return {id: crypto.randomUUID(), kind: 'geometry', name: label, shape: type, dimensions: [...dims], position, rotation: [0, 0, 0], scale: [1, 1, 1], chunk, collision: true, surface: 0x00020001,
    material: defaultMaterial(chunk), uvRepeat: [1, 1], colour: [220, 220, 220, 255], ...shapeData(type, dims)};
}
const chunkInfo = n => project.native?.chunks?.find(c => c.number === n);
function defaultChunk(pos) {
  const all = project.native?.chunks || [], vis = c => c.target ?? (c.visual || [])[0], col = c => c.collisionTarget ?? (c.collision || [])[0];
  let good = all.filter(c => vis(c) && col(c)); if (!good.length) good = all.filter(vis); if (!good.length) return null;
  if (!pos || !good.every(c => c.bounds)) return good[0].number;
  const d = c => { let s = 0; for (let i = 0; i < 3; i++) { const v = Math.max(0, c.bounds[0][i] - pos[i], pos[i] - c.bounds[1][i]); s += v * v; } return s; };
  return good.reduce((a, b) => d(b) < d(a) ? b : a).number;
}
const mainKind = w => { const t = String(w || '').toUpperCase().replace(/\.RG1$/, '').split(/[_.]/); return t.includes('O') ? 'O' : t.includes('OS') ? 'OS' : ''; };
function defaultMaterial(chunk) {
  const world = chunkInfo(chunk)?.target, ok = m => m.texture && m.chunk === chunk;
  return (project.materials.find(m => ok(m) && world && m.world === world) || project.materials.find(m => ok(m) && mainKind(m.world) === 'O') || project.materials.find(m => ok(m) && mainKind(m.world) === 'OS') || project.materials.find(ok))?.id;
}
function chunkMaterials(chunk) {   // textured materials the chunk loads, main world first, one per texture
  const world = chunkInfo(chunk)?.target, rank = m => m.world === world ? 0 : mainKind(m.world) ? 1 : 2, seen = new Set();
  return project.materials.filter(m => m.texture && m.chunk === chunk).sort((a, b) => rank(a) - rank(b)).filter(m => !seen.has(m.texture) && seen.add(m.texture)).slice(0, 160);
}
// a new authored mesh (drawn, OBJ, extracted face, imported model part): add it to the project, the items and the view
function addGeometry(o, sel = true) { project.geometry.push(o); const it = makeItem(o); items.push(it); byId.set(it.id, it); addNode(it); if (sel) { buildMarkers(); changed(); select(it, false); drawFlow(); renderPanel(); } return it; }
const round2 = v => Math.round(v * 100) / 100;

// draw & extrude (classic beginDraw / finishDraw): corners on a horizontal plane whose height is the first click's
// surface; the polygon is extruded 30 units up. Position = the outline's centre, vertices local to it.
let drawing = null; const drawLine = new THREE.Group(); scene.add(drawLine);
function beginDraw() { placing = null; setFaceMode(false); drawing = {y: null, points: []}; renderer.domElement.style.cursor = 'crosshair'; toast('Draw: click the corners (the first click sets the height) · Enter extrudes · Backspace removes a corner · Esc cancels'); }
function drawClick(cx, cy) {
  if (drawing.y === null) { const p = surfacePoint(cx, cy); drawing.y = round2(p.y); drawing.points.push(new THREE.Vector3(p.x, drawing.y, p.z)); }
  else { const p = new THREE.Vector3(); surfacePoint(cx, cy); if (!ray.ray.intersectPlane(new THREE.Plane(new THREE.Vector3(0, 1, 0), -drawing.y), p)) return; drawing.points.push(p); }
  redrawOutline();
}
function redrawOutline() {
  clear(drawLine); if (!drawing?.points.length) return;
  const lift = drawing.points.map(p => p.clone().setY(p.y + 1));
  drawLine.add(new THREE.Line(new THREE.BufferGeometry().setFromPoints([...lift, lift[0]]), new THREE.LineBasicMaterial({color: 0xff667d, depthTest: false})));
  for (const p of lift) { const d = new THREE.Mesh(new THREE.SphereGeometry(3, 8, 6), new THREE.MeshBasicMaterial({color: 0xff667d, depthTest: false})); d.position.copy(p); drawLine.add(d); }
}
function cancelDraw() { drawing = null; clear(drawLine); renderer.domElement.style.cursor = ''; }
function finishDraw() {
  if (drawing.points.length < 3) return toast('A shape needs at least three corners.', true);
  const pts = drawing.points, cx = pts.reduce((a, p) => a + p.x, 0) / pts.length, cz = pts.reduce((a, p) => a + p.z, 0) / pts.length, pos = [round2(cx), drawing.y, round2(cz)];
  const g = new THREE.ExtrudeGeometry(new THREE.Shape(pts.map(p => new THREE.Vector2(p.x - pos[0], -(p.z - pos[2])))), {depth: 30, bevelEnabled: false, steps: 1}); g.rotateX(-Math.PI / 2);
  snapshot(); const o = {...createShape('box', pos), ...meshData(g), name: 'Drawn platform', shape: 'polygon', outline: pts.map(p => [round2(p.x - pos[0]), 0, round2(p.z - pos[2])])};
  cancelDraw(); addGeometry(o); toast(`Drawn platform · ${pts.length} corners · chunk ${o.chunk}`);
}

// OBJ import (classic objPicker): positions / texture coordinates / faces, polygons triangulated, placed at the view centre
const objPicker = Object.assign(document.createElement('input'), {type: 'file', accept: '.obj', hidden: true}); document.body.append(objPicker);
objPicker.onchange = async () => {
  const f = objPicker.files[0]; if (!f || !project) return;
  try { importObj(await f.text(), f.name); } catch (e) { toast(e.message, true); } finally { objPicker.value = ''; }
};
function importObj(text, fileName) {
  const positions = [], uv = [], vertices = [], uvs = [], triangles = [];
  for (const raw of text.split(/\r?\n/)) {
    const line = raw.trim(); if (!line || line[0] === '#') continue; const [type, ...parts] = line.split(/\s+/);
    if (type === 'v') positions.push(parts.slice(0, 3).map(Number)); else if (type === 'vt') uv.push(parts.slice(0, 2).map(Number));
    else if (type === 'f') {
      const face = [];
      for (const word of parts) { const [pi, ti] = word.split('/').map(Number); const p = positions[pi < 0 ? positions.length + pi : pi - 1]; if (!p || p.length !== 3 || !p.every(Number.isFinite)) throw Error('Invalid OBJ vertex'); const t = uv[ti < 0 ? uv.length + ti : ti - 1] || [0, 0]; face.push(vertices.length); vertices.push([...p]); uvs.push([t[0], 1 - t[1]]); }
      if (face.length === 3) triangles.push(face);
      else { const ps = face.map(i => new THREE.Vector3(...vertices[i])), n = new THREE.Vector3().crossVectors(ps[1].clone().sub(ps[0]), ps[2].clone().sub(ps[0])), a = [Math.abs(n.x), Math.abs(n.y), Math.abs(n.z)], drop = a.indexOf(Math.max(...a));
        for (const tri of THREE.ShapeUtils.triangulateShape(ps.map(p => new THREE.Vector2(...p.toArray().filter((_, i) => i !== drop))), [])) triangles.push(tri.map(i => face[i])); }
    }
  }
  if (!triangles.length) throw Error('OBJ has no faces'); if (vertices.length > 65535) throw Error('Split this mesh into parts below 65535 vertices');
  snapshot(); const o = {...createShape('box', orbit.target.toArray().map(round2)), vertices, uvs, triangles, name: fileName.replace(/\.obj$/i, ''), shape: 'mesh'};
  addGeometry(o); toast(`Imported ${fileName}: ${vertices.length} vertices, ${triangles.length} triangles · collision on`);
}

// ---- #119 Import level / model (the POST /api/import-geometry, docs/custom-stages/IMPORT-GEOMETRY.md): pick the
// files together (glb | gltf + bin + images | obj + mtl + images), choose units / up axis / where / chunks / collision,
// the server cuts the model into the game's pieces (one material, <= 12,000 vertices) and chunks (<= ~60,000 collision
// triangles); added as ONE undo step with the chunk draw boxes. A result panel shows the stats and warnings and sets the
// collision surface per material (0 = no collision). The pieces share a group: a click selects the whole level.
const levelPicker = Object.assign(document.createElement('input'), {type: 'file', multiple: true, accept: '.glb,.gltf,.bin,.obj,.mtl,.png,.jpg,.jpeg,.bmp,.tga', hidden: true}); document.body.append(levelPicker);
levelPicker.onchange = () => { const fs = [...levelPicker.files]; levelPicker.value = ''; if (fs.length && project) openLevelImport(fs); };
const fileB64 = f => new Promise((res, rej) => { const r = new FileReader(); r.onload = () => res(String(r.result).split(',', 2)[1] || ''); r.onerror = () => rej(r.error); r.readAsDataURL(f); });
const IMP_SURF = () => [[0, 'No collision'], ...SURFACES];
function openLevelImport(files) {
  const main = files.find(f => /\.(glb|gltf|obj)$/i.test(f.name)); if (!main) return toast('Pick a .glb, .gltf or .obj (with its .bin / .mtl / images)', true);
  const blank = project.mode === 'new', vc = orbit.target.toArray().map(round2), ch = blank ? [1, 2, 3, 4, 5, 6] : [defaultChunk(vc) ?? 1];
  const box = $('imp119'), mb = (files.reduce((t, f) => t + f.size, 0) / 1048576).toFixed(1);
  $('imp119Body').innerHTML = `<p>${esc(files.map(f => f.name).join(', '))} · ${mb} MB</p>
    <div class="impGrid">
      <label class="field"><span>Units</span><select class="num" id="impUnits"><option value="meters">Metres (Blender, x10)</option><option value="game">Game units (x1)</option><option value="centimeters">Centimetres (x0.1)</option></select></label>
      <label class="field"><span>Where (x, y, z)</span><input class="num" id="impPos" value="${vc.join(', ')}" title="x, y, z of the model's origin (default: the view centre)"></label>
      <label class="field chk"><input type="checkbox" id="impGround" checked> <span>Sit on the ground (its lowest point at that y)</span></label>
      <label class="field chk"><input type="checkbox" id="impColl" checked> <span>Solid (Shadow can stand on it)</span></label>
      <label class="field"><span>Surface (sound and grip)</span><select class="num" id="impSurf">${SURFACES.map(([v, n]) => `<option value="${v}">${esc(n)}</option>`).join('')}</select></label>
    </div>
    <details class="impMore"><summary>Advanced: scale, axis, faces, chunks, textures</summary><div class="impGrid">
      <label class="field"><span>Extra scale</span><input class="num" type="number" step="any" id="impScale" value="1"></label>
      <label class="field"><span>Up axis</span><select class="num" id="impUp"><option value="y">Y up (glTF, Blender OBJ)</option><option value="z">Z up</option></select></label>
      <label class="field chk"><input type="checkbox" id="impFlip"> <span>Flip faces (if they show from the wrong side)</span></label>
      <label class="field"><span>Chunks to use</span><input class="num" id="impChunks" value="${ch.join(', ')}" title="In order; each gets ONE draw box over the whole model (its old region is replaced)"></label>
      <label class="field"><span>Max texture size</span><select class="num" id="impTex">${[128, 256, 512, 1024].map(v => `<option ${v === 512 ? 'selected' : ''}>${v}</option>`).join('')}</select></label>
    </div>
    <p class="muted">Chunks are the stage's draw regions. ${blank ? 'A blank stage: chunks 1-6 hold several hundred thousand collision triangles.' : `This stage's own chunk ${ch[0]} is used: its draw region becomes one box around the model (it is drawn wherever the player is). Pick free chunk numbers if you have them.`}</p></details>
    <p class="muted">Each material can get its own surface after the import (inspector).</p>`;
  $('imp119Go').hidden = false; $('imp119Go').disabled = false; $('imp119Go').textContent = 'Import'; box.hidden = false;
  $('imp119Go').onclick = () => runLevelImport(files);
  $('imp119Close').onclick = () => { box.hidden = true; };
}
async function runLevelImport(files) {
  const num = (id, d) => { const v = +$(id).value; return Number.isFinite(v) && v > 0 ? v : d; };
  const pos = $('impPos').value.split(/[ ,]+/).map(Number), chunks = [...new Set($('impChunks').value.split(/[ ,]+/).map(Number).filter(n => Number.isInteger(n) && n >= 0))];
  if (pos.length !== 3 || !pos.every(Number.isFinite)) return toast('Place at: three numbers x, y, z', true);
  if (!chunks.length) return toast('Chunks to use: at least one chunk number', true);
  const options = {units: $('impUnits').value, scale: num('impScale', 1), up: $('impUp').value, flipWinding: $('impFlip').checked, position: pos, ground: $('impGround').checked,
    chunks, name: (files.find(f => /\.(glb|gltf|obj)$/i.test(f.name))?.name || 'Imported').replace(/\.[^.]+$/, ''), collision: $('impColl').checked, defaultSurface: +$('impSurf').value, maxTexture: +$('impTex').value};
  const go = $('imp119Go'); go.disabled = true; go.textContent = 'Importing…';
  let data;
  try { data = await api('/api/import-geometry', {project: project.id, files: await Promise.all(files.map(async f => ({name: f.name, data: await fileB64(f)}))), options}); }
  catch (e) { go.disabled = false; go.textContent = 'Import'; return toast('Import failed: ' + e.message, true); }
  applyLevelImport(data, files.find(f => /\.(glb|gltf|obj)$/i.test(f.name))?.name || 'import');
}
let lastLevelImport = null;
function applyLevelImport(data, name) {
  lastLevelImport = data;
  const geo = data.geometry || []; if (!geo.length) { $('imp119Go').disabled = false; return toast('Nothing came back from the import', true); }
  const group = geo[0].group || crypto.randomUUID();
  snapshot();
  for (const m of data.materials || []) if (!project.materials.some(x => x.id === m.id)) project.materials.push(m);
  let last; for (const o of geo) { o.group ||= group; o.importGroup = group; last = addGeometry(o, false); }
  project.chunkBoxes ||= {}; for (const [k, v] of Object.entries(data.chunkBoxes || {})) project.chunkBoxes[k] = v;
  buildMarkers(); changed(); drawFlow(); renderIssueBadge(); if (panel === 'add' || panel === 'outline') renderPanel();
  const grp = items.filter(it => it.o.importGroup === group); if (grp.length) setSelection(grp);
  showLevelResult(data, name, group);
  toast(`${name}: ${geo.length} piece${geo.length === 1 ? '' : 's'} added (Ctrl Z undoes the whole import)`);
}
function showLevelResult(data, name, group) {
  const st = data.stats || {}, mats = [...new Set(items.filter(it => it.o.importGroup === group).map(it => it.o.material))];
  const matName = id => project.materials.find(m => m.id === id)?.name || id, surfOf = id => { const o = items.find(it => it.o.importGroup === group && it.o.material === id)?.o; return o?.collision === false ? 0 : o?.surface ?? 0x00020001; };
  const per = st.collisionTriangles || {};
  $('imp119Body').innerHTML = `<p><b>${esc(name)}</b>: ${st.pieces ?? '?'} pieces · ${(st.triangles ?? 0).toLocaleString()} triangles · ${(st.vertices ?? 0).toLocaleString()} vertices · ${st.materials ?? mats.length} materials · chunks ${esc((st.chunks || Object.keys(data.chunkBoxes || {})).join(', '))}</p>
    ${Object.keys(per).length ? `<p class="muted">Collision triangles per chunk: ${Object.entries(per).map(([k, v]) => `${esc(k)}: ${(+v).toLocaleString()}`).join(' · ')}</p>` : ''}
    ${(data.warnings || []).length ? `<ul class="impWarn">${data.warnings.map(w => `<li>${esc(w)}</li>`).join('')}</ul>` : ''}
    <h4>Surface per material</h4><div class="impMats">${mats.map(id => `<label class="field"><span>${esc(matName(id))}</span><select class="num" data-impmat="${esc(id)}">${IMP_SURF().map(([v, n]) => `<option value="${v}" ${v === surfOf(id) ? 'selected' : ''}>${esc(n)}</option>`).join('')}</select></label>`).join('')}</div>
    <p class="muted">Click any piece to select the whole level (Alt-click: one piece); move it with the gizmo.</p>`;
  $('imp119Go').hidden = true;
  $('imp119Body').querySelectorAll('[data-impmat]').forEach(sel => sel.onchange = () => {
    const v = +sel.value; snapshot();
    for (const it of items.filter(it => it.o.importGroup === group && it.o.material === sel.dataset.impmat)) { if (v === 0) it.o.collision = false; else { it.o.collision = true; it.o.surface = v; } }
    changed(); toast(`${matName(sel.dataset.impmat)}: ${v === 0 ? 'no collision' : IMP_SURF().find(x => x[0] === v)[1]}`);
  });
}
// a stage model (DFF) as static geometry (classic content-browser drop -> /api/import-model): choose it, click a spot
function openModelPicker() {
  if (!models) return toast('The stage models are still loading.', true);
  const box = $('modelPick'), q = $('modelQ'); box.hidden = false; q.value = ''; q.focus();
  const list = () => { const t = q.value.toLowerCase(); $('modelList').innerHTML = R.models.filter(m => (m.name + ' ' + m.archive).toLowerCase().includes(t)).slice(0, 200).map(m => `<div class="pItem" data-m="${esc(m.id)}"><span class="name">${esc(m.name.replace(/\.DFF$/i, ''))}</span><small>${esc(m.archive)}</small></div>`).join('') || '<div class="sub">No models match.</div>';
    $('modelList').querySelectorAll('[data-m]').forEach(r => r.onclick = () => { const m = R.models.find(x => x.id === r.dataset.m); box.hidden = true; placing = {name: m.name.replace(/\.DFF$/i, ''), importModel: m.id}; renderer.domElement.style.cursor = 'crosshair'; toast(`Click where ${placing.name} goes · Esc cancels`); }); };
  q.oninput = list; q.onkeydown = e => { if (e.key === 'Escape') box.hidden = true; }; list(); $('modelCancel').onclick = () => box.hidden = true;
}
async function importModelAt(row, point) {
  const pos = point.toArray().map(round2); toast(`Importing ${row.name}…`);
  try {
    const data = await api('/api/import-model', {project: project.id, stage: project.stage, model: row.importModel, chunk: defaultChunk(pos)});
    snapshot(); for (const m of data.materials) if (!project.materials.some(x => x.id === m.id)) project.materials.push(m);
    let last; for (const o of data.geometry) { o.position = pos; last = addGeometry(o, false); }
    buildMarkers(); changed(); select(last, false); drawFlow(); renderPanel();
    toast(`${row.name}: ${data.geometry.length} textured part${data.geometry.length > 1 ? 's' : ''} with collision · no object behaviour`);
  } catch (e) { toast(e.message, true); }
}

// collision face editing (classic editCollisionFace): in Edit faces mode a click on an original face lists it in
// project.collisionEdits (the build drops it) and adds an editable invisible triangle in its place (shape 'collision',
// the face's own surface flags). Delete removes the collision there; "Put original back" undoes the extraction.
// ---- #73 collision from surface (UI part; build reads it, proven in game): project.surfaceCollision = [{id,
// name, chunk?, surface, triangles [[[x,y,z] x3]] in WORLD space (refreshed from the scene, so art edits / moves are in),
// at [x,y,z], source {kind 'art' | 'geometry' | 'object', ref, model? + mesh? (object), faces [face indices], mode 'flat' |
// 'piece'} (what was picked; the build does not read it)}] - exact shape in STAGE-WORKSHOP-UI2-SPEC.md. A click takes the flat face under the
// cursor (connected triangles in the same plane), Alt-click the whole connected piece; picked faces are drawn green
// (red for a kill surface). Faces index the ORIGINAL triangle lists: project.reference[ref].triangles (stage art),
// the shape's triangles (geometry), the stage model's meshes[mesh].triangles (objects).
var surfMode = false, surfFound = new Map(), surfColl = new THREE.Group(); scene.add(surfColl);   // var: buildNodes / renderLens may run before this line
const SURFACES = [[0x00020001, 'Road / floor'], [0x60000001, 'Stone floor'], [0x10010101, 'Metal floor'], [2, 'Jump wall'], [0x60008001, 'Loop track (needed for loops)'], [32, 'Kill plane (not solid; kills when falling down through it)']];
function setSurfMode(on) {
  surfMode = on; if (on) { if (faceMode) setFaceMode(false); placing = null; if (drawing) cancelDraw(); toast('Collision from surface: click a flat surface (Alt-click: the whole connected piece) · Esc leaves'); }
  renderer.domElement.style.cursor = on ? 'cell' : ''; renderLens(); renderInspector();
}
// the hit mesh -> what it belongs to, and how a drawn triangle maps to an original face
function surfaceTarget(obj) {
  for (let n = obj; n; n = n.parent) {
    if (n.parent === art) return n.userData.backdrop ? null : {source: 'art', ref: n.userData.art, chunk: n.userData.chunk, label: n.userData.world, map: obj.userData.faceMap || null};
    if (n.userData.modelName && obj.parent === n) { let it = null; for (let a = n; a && !it; a = a.parent) it = a.userData.item || null; const holder = it || obj.userData.item;
      return holder && holder.o.kind === 'native' ? {source: 'object', ref: holder.o.id, model: n.userData.modelName, mesh: n.children.indexOf(obj), label: holder.name, map: null} : null; }
    if (n.userData.geometry && n.userData.item) { const geo = n.userData.item.o; return geo.shape === 'collision' ? null : {source: 'geometry', ref: geo.id, label: n.userData.item.name, map: null}; }
  }
  return null;
}
function floodFaces(geo, start, piece) {   // drawn triangles connected to `start` (by shared corner positions): same plane, or any (piece)
  const pos = geo.attributes.position, idx = geo.index, nt = idx ? idx.count / 3 : pos.count / 3, vi = (t, k) => idx ? idx.getX(t * 3 + k) : t * 3 + k;
  const key = i => `${Math.round(pos.getX(i) * 100)},${Math.round(pos.getY(i) * 100)},${Math.round(pos.getZ(i) * 100)}`;
  const byVert = new Map(); for (let t = 0; t < nt; t++) for (let k = 0; k < 3; k++) { const kk = key(vi(t, k)); if (!byVert.has(kk)) byVert.set(kk, []); byVert.get(kk).push(t); }
  const a = new THREE.Vector3(), b = new THREE.Vector3(), c = new THREE.Vector3();
  const plane = t => { a.fromBufferAttribute(pos, vi(t, 0)); b.fromBufferAttribute(pos, vi(t, 1)); c.fromBufferAttribute(pos, vi(t, 2)); const n = b.clone().sub(a).cross(c.clone().sub(a)); if (n.lengthSq() < 1e-12) return null; n.normalize(); return {n, d: n.dot(a)}; };
  const p0 = plane(start), out = new Set([start]), q = [start];
  while (q.length && out.size < 20000) { const t = q.pop(); for (let k = 0; k < 3; k++) for (const u of byVert.get(key(vi(t, k))) || []) { if (out.has(u)) continue; if (!piece) { const pu = plane(u); if (!pu || !p0 || Math.abs(pu.n.dot(p0.n)) < .996 || Math.abs(pu.d * Math.sign(pu.n.dot(p0.n)) - p0.d) > 1) continue; } out.add(u); q.push(u); } }
  return [...out];
}
function pickSurface(cx, cy, piece) {
  surfacePoint(cx, cy);   // aims `ray`
  const hits = ray.intersectObjects([...art.children.filter(m => m.visible && !m.userData.backdrop && !m.userData.shellIn), ...nodes.children.filter(n => !n.userData.volume)], true).filter(h => { for (let n = h.object; n; n = n.parent) if (!n.visible) return false; return h.object.isMesh && h.faceIndex != null; });
  for (const h of hits) {
    const t = surfaceTarget(h.object); if (!t) continue;
    const drawn = floodFaces(h.object.geometry, h.faceIndex, piece), faces = [...new Set(drawn.map(i => t.map ? t.map[i] : i))].filter(f => f != null).sort((x, y) => x - y);
    const same = e => e.source?.kind === t.source && e.source.ref === t.ref && (e.source.model ?? null) === (t.model ?? null) && (e.source.mesh ?? null) === (t.mesh ?? null);
    const have = new Set((project.surfaceCollision || []).filter(same).flatMap(e => e.source.faces)); const fresh = faces.filter(f => !have.has(f));
    if (!fresh.length) return toast('These faces are already collision (inspector: Collision from surface)', true);
    snapshot(); const geoChunk = t.source === 'geometry' ? byId.get(t.ref)?.o.chunk : null;
    const e = {id: crypto.randomUUID(), name: `${t.label} · ${fresh.length} face${fresh.length > 1 ? 's' : ''}`, ...(t.chunk != null ? {chunk: t.chunk} : geoChunk != null ? {chunk: geoChunk} : {}), surface: 0x00020001, triangles: [], at: h.point.toArray().map(round2),
      source: {kind: t.source, ref: t.ref, ...(t.model ? {model: t.model, mesh: t.mesh} : {}), faces: fresh, mode: piece ? 'piece' : 'flat'}};
    (project.surfaceCollision ||= []).push(e); changed(); buildSurfColl(); renderLens(); renderInspector();
    return toast(`Collision from surface: ${fresh.length} face${fresh.length > 1 ? 's' : ''} of ${t.label} (${piece ? 'connected piece' : 'flat face'}) · Build makes them solid`);
  }
  toast('No stage art, shape or object surface there', true);
}
function surfMeshes(e) {   // [[mesh, drawn triangle indices]] of an entry in the current scene
  const S = e.source || {}; const pick = (mesh, map) => { const set = new Set(S.faces || []), nt = (mesh.geometry.index ? mesh.geometry.index.count : mesh.geometry.attributes.position.count) / 3, out = []; for (let t = 0; t < nt; t++) if (set.has(map ? map[t] : t)) out.push(t); return [mesh, out]; };
  if (S.kind === 'art') { const g = art.children.find(c => c.userData.art === S.ref); return g ? g.children.filter(c => c.isMesh).map(m => pick(m, m.userData.faceMap)) : []; }
  const it = byId.get(S.ref), n = it && nodeOf.get(it); if (!n) return [];
  if (S.kind === 'geometry') { const m = n.children.find(c => c.isMesh); return m ? [pick(m, null)] : []; }
  let grp = null; n.traverse(c => { if (!grp && c.userData.modelName === S.model) grp = c; }); const m = grp?.children[S.mesh]; return m?.isMesh ? [pick(m, null)] : [];
}
function buildSurfColl() {
  if (!surfColl) return; clear(surfColl); if (!project) return;
  for (const e of project.surfaceCollision || []) {
    const pts = [];
    for (const [mesh, tris] of surfMeshes(e)) { mesh.updateWorldMatrix(true, false);   // parents too: right after a rebuild nothing has rendered yet
      const pos = mesh.geometry.attributes.position, idx = mesh.geometry.index; for (const t of tris) for (let k = 0; k < 3; k++) pts.push(new THREE.Vector3().fromBufferAttribute(pos, idx ? idx.getX(t * 3 + k) : t * 3 + k).applyMatrix4(mesh.matrixWorld)); }
    surfFound.set(e.id, pts.length / 3); if (!pts.length) continue;
    e.triangles = []; for (let k = 0; k < pts.length; k += 3) e.triangles.push([pts[k], pts[k + 1], pts[k + 2]].map(v => v.toArray().map(x => Math.round(x * 1000) / 1000)));   // what the build reads (world space)
    const g = new THREE.BufferGeometry().setFromPoints(pts), mat = new THREE.MeshBasicMaterial({color: e.surface === 32 ? 0xff2a3a : 0x34d399, transparent: true, opacity: .45, depthWrite: false, side: THREE.DoubleSide, polygonOffset: true, polygonOffsetFactor: -2, polygonOffsetUnits: -2});
    const m = new THREE.Mesh(g, mat); m.raycast = () => {}; m.userData.surf = e.id; surfColl.add(m);
  }
}
function appendSurfCard() {
  const el = $('inspector'); if (!project || !el) return;
  const ref = surfMode ? null : selected ? selected.o.id : artSel ? artSel.userData.art : null, list = (project.surfaceCollision || []).filter(e => !ref || e.source?.ref === ref);
  if (!list.length && !surfMode) return;
  if (!selected && !artSel) el.innerHTML = '';
  const opts = v => (SURFACES.some(([x]) => x === v) ? '' : `<option value="${v}" selected>Other (${v})</option>`) + SURFACES.map(([x, n]) => `<option value="${x}" ${x === v ? 'selected' : ''}>${esc(n)}</option>`).join('');
  el.insertAdjacentHTML('beforeend', `<details class="sect" open id="surfCard"><summary>Collision from surface <span>${list.length || ''}</span></summary><div>
    ${list.map(e => `<div class="surfRow" data-sid="${esc(e.id)}"><div class="surfHead"><b>${esc(e.name)}</b><small>${e.source.faces.length} face${e.source.faces.length > 1 ? 's' : ''} · ${e.source.mode === 'piece' ? 'connected piece' : 'flat face'} · ${esc(e.source.kind === 'art' ? 'stage art' : e.source.kind)}${surfFound.get(e.id) === 0 && (e.source.kind === 'art' || models) ? ' · <span class="badText">not found in the view</span>' : ''}</small></div>
      <select class="num" data-ssurf="${esc(e.id)}">${opts(e.surface)}</select><div class="wsBtns"><button class="ghost small" data-sfly="${esc(e.id)}">Fly there</button> <button class="ghost small" data-srm="${esc(e.id)}">Remove</button></div></div>`).join('') || '<p class="muted">Nothing picked yet.</p>'}
    <p class="muted">${surfMode ? 'Click a flat surface; Alt-click takes the whole connected piece; Esc leaves.' : '<button class="ghost small" id="surfOn">Pick more surfaces</button>'} Track B builds these faces into the chunk's collision (with the art's own move / rotate / scale); nothing of this is in game yet.</p></div></details>`);
  el.querySelectorAll('[data-ssurf]').forEach(s => s.onchange = () => { const e = project.surfaceCollision.find(x => x.id === s.dataset.ssurf); snapshot(); e.surface = +s.value; changed(); buildSurfColl(); });
  el.querySelectorAll('[data-srm]').forEach(b => b.onclick = () => { snapshot(); project.surfaceCollision = project.surfaceCollision.filter(x => x.id !== b.dataset.srm); if (!project.surfaceCollision.length) delete project.surfaceCollision; changed(); buildSurfColl(); renderLens(); renderInspector(); });
  el.querySelectorAll('[data-sfly]').forEach(b => b.onclick = () => { const e = project.surfaceCollision.find(x => x.id === b.dataset.sfly); if (e?.at) flyTo(e.at, 300); });
  el.querySelector('#surfOn')?.addEventListener('click', () => setSurfMode(true));
}
function setFaceMode(on) { faceMode = on; if (on) { if (surfMode) setSurfMode(false); placing = null; if (drawing) cancelDraw(); toast('Edit faces: click a collision or kill-plane face · Esc leaves this mode'); } renderer.domElement.style.cursor = on ? 'cell' : ''; renderLens(); }
function pickFace(cx, cy) {
  surfacePoint(cx, cy); const hit = ray.intersectObjects([...kill.children, ...coll.children], false)[0]; if (!hit) return false;
  const m = project.reference.find(x => x.id === hit.object.userData.ref), face = hit.object.userData.faceMap[hit.faceIndex]; if (!m || face === undefined) return false;
  const vs = m.triangles[face].map(i => [...m.vertices[i]]), c = vs.reduce((a, v) => a.map((x, i) => x + v[i] / 3), [0, 0, 0]).map(round2);
  snapshot(); ((project.collisionEdits ||= {})[m.id] ||= []).push(face);
  const o = {...createShape('box', c), shape: 'collision', name: 'Collision face ' + face, vertices: vs.map(v => v.map((x, i) => x - c[i])), triangles: [[0, 1, 2]], uvs: [[0, 0], [1, 0], [0, 1]], renderVisible: false, chunk: m.chunk, surface: m.collisionRole === 'extra' ? 0x00020001 : m.flags?.[face] || 2, sourceRef: m.id, sourceFace: face};
  buildCollision(); addGeometry(o);
  toast(m.collisionRole !== 'extra' && (m.flags?.[face] ?? 0) === 0x20 ? 'Kill-plane face extracted: your copy still kills (surface 32, not solid). Delete removes the kill plane here.' : 'Collision face extracted: move / edit it, or press Delete to remove the collision here.');
  return true;
}
function putFaceBack(it) {
  const o = it.o, list = project.collisionEdits?.[o.sourceRef]; snapshot();
  if (list) { const i = list.indexOf(o.sourceFace); if (i >= 0) list.splice(i, 1); if (!list.length) delete project.collisionEdits[o.sourceRef]; }
  project.geometry.splice(project.geometry.indexOf(o), 1); items.splice(items.indexOf(it), 1); byId.delete(it.id); const n = nodeOf.get(it); if (n) { nodes.remove(n); nodeOf.delete(it); }
  gizmo.detach(); selected = null; buildCollision(); buildMarkers(); changed(); renderInspector(); renderPanel(); drawFlow(); toast('Original collision face restored.');
}
const pngPicker = Object.assign(document.createElement('input'), {type: 'file', accept: 'image/png,image/*', hidden: true}); document.body.append(pngPicker); let pngFor = null;
pngPicker.onchange = async () => {
  const f = pngPicker.files[0], it = pngFor; if (!f || !project) return;
  try {
    const bytes = new Uint8Array(await f.arrayBuffer()); let bin = ''; for (let i = 0; i < bytes.length; i += 16384) bin += String.fromCharCode(...bytes.slice(i, i + 16384));
    const m = await api('/api/upload', {project: project.id, name: f.name, kind: 'texture', data: btoa(bin)});
    snapshot(); if (!project.materials.some(x => x.id === m.id)) project.materials.push(m);
    if (it?.o.kind === 'geometry' && byId.get(it.id) === it) { it.o.material = m.id; rebuildNode(it); }
    changed(); renderInspector(); toast(`Imported ${f.name} (${m.width}×${m.height}). Full resolution goes in the private PC texture pack.`);
  } catch (e) { toast(e.message, true); } finally { pngPicker.value = ''; }
};
// ---- paths: control points with the move handle (click a point), inspector fields as classic --------------------------
let pathPoint = null, wpSel = null, arcSel = null;   // #160 arcSel: the selected launcher's arc handle ('land' / 'top') the gizmo moves   // #92 wpSel: the selected enemy's waypoint (0 / 1) the gizmo moves
// #90 one colour per waypoint, the same in the inspector boxes and on the 3D points / lines (Home = back at the spot)
const WP_COLOURS = ['#fb923c', '#f472b6', '#94a3b8'], WP_NAMES = ['Waypoint 1', 'Waypoint 2', 'Home (back at its spot)'], PATH_COLOUR = '#22d3ee', PATH_OFF = '#64748b';
const WP_FAMILY = {act: ['▶', 'Action at the waypoint'], time: ['◷', 'Timing'], speed: ['»', 'Speed on the way there'], pos: ['⌖', 'Position (from the spot)'], other: ['·', '']};
const WP_ORDER = ['act', 'time', 'speed', 'pos', 'other'];
const wpFamily = name => /WaitType|ActionType/.test(name) ? 'act' : /WaitSec|ActionTime/.test(name) ? 'time' : /Speed/.test(name) ? 'speed' : /Translation|_X$|_Y$|_Z$/.test(name) ? 'pos' : 'other';
function groupWaypointRows(fields, rows) {   // rows[i] = the html of field i; Pos<n>_ fields go into one box per waypoint
  const out = [], boxes = new Map();
  fields.forEach((f, i) => { if (!rows[i]) return; const m = /^Pos(\d)_/.exec(f.name);
    if (!m) { out.push(rows[i]); return; }
    const k = +m[1]; if (!boxes.has(k)) { boxes.set(k, []); out.push({box: k}); } boxes.get(k).push([wpFamily(f.name), rows[i]]); });
  return out.map(x => typeof x === 'string' ? x : (() => { const k = x.box, list = boxes.get(k).sort((a, b) => WP_ORDER.indexOf(a[0]) - WP_ORDER.indexOf(b[0]));
    return `<div class="wpBox" style="--wp:${WP_COLOURS[k] || WP_COLOURS[2]}"><div class="wpHead"><span class="wpDot"></span>${esc(WP_NAMES[k] || 'Waypoint ' + (k + 1))}<small>Pos${k}${k < 2 ? ' · the point of that colour in the 3D view' : ''}</small></div>${k < 2 ? `<div class="wpBtns" data-wpk="${k}"></div>` : ''}${list.map(r => r[1]).join('')}</div>`; })()).join('');
}
// ---- Appear Types in the viewport (UI2 spec 7e, research/appear-types.md) -------------------------------------------
// GUN / Bk Soldier, GUN Robot, Egg Pawn: Waypoint 1 / 2 = Pos0 / Pos1 X, Z from the spot, drawn as orange points with
// the route their Behaviour walks (drag = edit the two parameters; frame = turned with the enemy, NOT proven in game).
// GUN Beetle / Bk Wing L / S: the Flight path preset (enemy/EnemyPath.one, local to the enemy) drawn at it; dragging a
// cyan point edits project.enemyPaths[PathType] = one shared file: every flyer using that preset in the mod changes.
const WAYPOINT_CLASSES = new Set(['0-100', '0-141', '0-104', '0-121']), FLYER_CLASSES = new Set(['0-101', '0-142', '0-143']);
const enemyGuide = new THREE.Group(); enemyGuide.renderOrder = 15; scene.add(enemyGuide);
let enemyPresets = null, enemyPresetsAsked = false;
function loadEnemyPresets() { if (enemyPresetsAsked) return; enemyPresetsAsked = true; api('/api/enemy-paths').then(d => { enemyPresets = d.presets || []; enemyGuide.userData.sig = null; if (selected && enemyKind(selected.o) === 'fly') renderInspector(); }).catch(() => { enemyPresets = []; }); }
const enemyKind = o => o?.kind !== 'native' ? null : WAYPOINT_CLASSES.has(`${o.list}-${o.type}`) ? 'wp' : FLYER_CLASSES.has(`${o.list}-${o.type}`) ? 'fly' : null;
const fieldAt = (o, name) => (catalog.get(`${o.list}-${o.type}`)?.fields || []).findIndex(f => f.name === name);
function enemyFrame(o) { const m = new THREE.Matrix4(), e = new THREE.Euler(...(o.rotation || [0, 0, 0]).map(THREE.MathUtils.degToRad), 'YXZ'); return m.compose(new THREE.Vector3(...o.position), new THREE.Quaternion().setFromEuler(e), new THREE.Vector3(1, 1, 1)); }
// #98 (quiet runs 2026-10-17 / 10-19, + B): soldier / robot / pawn waypoints are WORLD-axis offsets from the spot
// (a turned soldier still walks +X), and Move range is a leash: a circle around the spot the enemy never leaves.
const wpFrame = o => new THREE.Matrix4().makeTranslation(...o.position);
function wpLeash(o, w = waypointInfo(o)) { if (!w) return null; const used = wpUsed(w), far = Math.max(0, ...w.w.slice(0, used).map(p => Math.hypot(p[0], p[2]))); return {used, far, range: w.range, out: used > 0 && far + 30 > w.range, need: Math.ceil(far + 30)}; }
function extendRange(it) { const o = it.o, L = wpLeash(o); if (!L?.out) return; snapshot(); setParamWord(o, fieldAt(o, 'MoveRange'), L.need, true); changed(); drawEnemyGuide(true); renderInspector(); toast(`Move range ${Math.round(L.range)} -> ${L.need}: ${it.name} can reach its waypoints now`); }
function leashCheck(it) { const L = it && wpLeash(it.o); if (L?.out) toast(`Waypoint outside Move range (${Math.round(L.range)}): ${it.name} would stop at the circle. Inspector: Extend range (to ${L.need}).`, true); }
const WP_MODES = ['stand', 'linear', 'triangle', 'random'];
function waypointInfo(o) {
  const ix = ['Pos0_TranslationXFromOrigin', 'Pos0_TranslationZFromOrigin', 'Pos1_TranslationXFromOrigin', 'Pos1_TranslationZFromOrigin'].map(n => fieldAt(o, n)); if (ix.some(i => i < 0)) return null;
  const at = fieldAt(o, 'AppearType'), wa = fieldAt(o, 'WaitActMoveType'), appear = paramWord(o, at), act = wa >= 0 ? paramWord(o, wa) : null;
  const mode = wa >= 0 ? (appear === 0 ? WP_MODES[act] || 'stand' : 'other') : WP_MODES[appear] || 'other';   // robots / pawns: Behaviour WAIT_ACT + its move type
  const w = [[paramWord(o, ix[0], true) || 0, 0, paramWord(o, ix[1], true) || 0], [paramWord(o, ix[2], true) || 0, 0, paramWord(o, ix[3], true) || 0]];
  return {ix, mode, range: paramWord(o, fieldAt(o, 'MoveRange'), true) || 0, w, set: w.map(p => Math.abs(p[0]) > .5 || Math.abs(p[2]) > .5)};
}
function flyerInfo(o) {
  const pt = paramWord(o, fieldAt(o, 'PathType')), preset = enemyPresets?.[pt] || null, edit = project.enemyPaths?.[String(pt)];
  return {on: paramWord(o, fieldAt(o, 'AppearType')) === 1, type: pt, preset, edited: !!edit, points: edit?.points || preset?.points || null, closed: edit?.closed ?? preset?.closed ?? false, reversed: paramWord(o, fieldAt(o, 'PatrolReversed')) === 1};
}
// #160 (user: "super hard to have depth perception ... as it overlays on top"): a guide line is drawn IN the scene (hidden
// behind walls and floors like the stage), and once more faint + dashed over everything, so its hidden parts still show
function guideLine(pts, colour, opacity = 1, dashed = false) {
  const g = new THREE.BufferGeometry().setFromPoints(pts), grp = new THREE.Group();
  const m = dashed ? new THREE.LineDashedMaterial({color: colour, dashSize: 14, gapSize: 10, transparent: true, opacity}) : new THREE.LineBasicMaterial({color: colour, transparent: true, opacity});
  const l = new THREE.Line(g, m); if (dashed) l.computeLineDistances(); l.renderOrder = 15;
  const hm = new THREE.LineDashedMaterial({color: colour, dashSize: 6, gapSize: 10, transparent: true, opacity: opacity * .3, depthTest: false, depthWrite: false});
  const h = new THREE.Line(g, hm); h.computeLineDistances(); h.renderOrder = 14;
  for (const x of [l, h]) { x.raycast = () => {}; grp.add(x); }
  grp.raycast = () => {}; grp.userData.guideLine = true; return grp;
}
// #160 a thin drop-line from a key point down to the ground under it and a small ground ring: heights read at a glance
function dropLine(p, colour, env = null, fallbackY = null) {   // fallbackY: down to that height when no ground is found under p
  const e = env || (arcColliders && arcEnv()), hit = e?.hit?.([p.x, p.y, p.z], [p.x, p.y - 4000, p.z]);
  const y = hit ? hit.point[1] : fallbackY;
  if (y === null || p.y - y < 6) return null;
  const g = new THREE.Group(), q = new THREE.Vector3(p.x, y, p.z);
  g.add(guideLine([p, q], colour, .55, true));
  const ring = new THREE.Mesh(new THREE.RingGeometry(5, 7, 24), new THREE.MeshBasicMaterial({color: colour, side: THREE.DoubleSide, transparent: true, opacity: .7, depthWrite: false}));
  ring.position.copy(q).add(new THREE.Vector3(0, .6, 0)); ring.rotation.x = -Math.PI / 2; ring.renderOrder = 15; ring.raycast = () => {}; g.add(ring);
  g.raycast = () => {}; g.userData.dropLine = true; return g;
}
function drawEnemyGuide(force = false) {
  const o = selected && !multi.size ? selected.o : null, kind = enemyKind(o);
  const sig = kind ? [o.id, o.params, o.position, o.rotation, kind === 'fly' ? JSON.stringify(project.enemyPaths?.[String(paramWord(o, fieldAt(o, 'PathType')))] || null) + !!enemyPresets : ''].join('|') : '';
  if (!force && sig + '|' + wpSel === enemyGuide.userData.sig) return; enemyGuide.userData.sig = sig + '|' + wpSel; enemyGuide.userData.handleK = null;
  for (const c of [...enemyGuide.children]) { enemyGuide.remove(c); c.traverse?.(x => { x.geometry?.dispose?.(); x.material?.map?.dispose?.(); x.material?.dispose?.(); }); }
  if (!kind) return; const m = kind === 'wp' ? wpFrame(o) : enemyFrame(o), spot = new THREE.Vector3(...o.position), handles = [];
  if (kind === 'wp') {
    const w = waypointInfo(o); if (!w) return; const p1 = new THREE.Vector3(...w.w[0]).applyMatrix4(m), p2 = new THREE.Vector3(...w.w[1]).applyMatrix4(m), [C1, C2, CH] = WP_COLOURS.map(c => new THREE.Color(c).getHex());
    if (w.mode === 'linear') enemyGuide.add(guideLine([spot, p1], C1), guideLine([p1, spot], CH, .7, true));   // each leg in the colour of where it goes
    else if (w.mode === 'triangle') enemyGuide.add(guideLine([spot, p1], C1), guideLine([p1, p2], C2), guideLine([p2, spot], CH, .8));
    else enemyGuide.add(guideLine([spot, p1], C1, .35, true), guideLine([spot, p2], C2, .35, true));   // not walked with this Behaviour: faint
    if (w.range > 0) { const L = wpLeash(o, w), col = L.out ? '#f87171' : '#e2e8f0';   // #98 the leash circle
      enemyGuide.add(guideLine(Array.from({length: 65}, (_, i) => new THREE.Vector3(Math.cos(i / 64 * Math.PI * 2) * w.range, 0, Math.sin(i / 64 * Math.PI * 2) * w.range).add(spot)), new THREE.Color(col).getHex(), L.out ? 1 : .8));
      const sp = labelSprite(`Move range ${Math.round(w.range)}${L.out ? ' · a waypoint is outside' : ''}`, col); sp.center.set(.5, -.4); sp.position.copy(spot).add(new THREE.Vector3(0, 8, w.range)); enemyGuide.add(sp); }
    const used = wpUsed(w), pts = [p1, p2];
    for (let k = 0; k < 2; k++) { if (k >= used && !w.set[k]) continue;   // an unused waypoint at its spot (0, 0) is not drawn: Add waypoint / Place at mouse put it somewhere
      handles.push(pts[k]); enemyGuide.userData.handleK = [...(enemyGuide.userData.handleK || []), k];
      enemyGuide.add(guideLine([spot, pts[k]], new THREE.Color(WP_COLOURS[k]).getHex(), k < used ? .55 : .35, true));   // always a line from the enemy
      const h = wpHandle(WP_COLOURS[k], wpSel === k); h.position.copy(pts[k]); h.userData.k = k; enemyGuide.add(h);
      const dl = dropLine(pts[k], new THREE.Color(WP_COLOURS[k]).getHex()); if (dl) enemyGuide.add(dl);   // #160
      const sp = labelSprite(`Waypoint ${k + 1}${k < used ? '' : ' (not walked)'} · approx.`, WP_COLOURS[k]); sp.center.set(.5, -1.1); sp.position.copy(pts[k]); enemyGuide.add(sp); }
    enemyGuide.userData.colours = (enemyGuide.userData.handleK || []).map(k => WP_COLOURS[k]);
  } else {
    loadEnemyPresets(); const f = flyerInfo(o); if (!f.points?.length) return;
    const world = f.points.map(p => new THREE.Vector3(...p).applyMatrix4(m)), C = new THREE.Color(f.on ? PATH_COLOUR : PATH_OFF).getHex();
    enemyGuide.add(guideLine(f.closed ? [...world, world[0]] : world, C, f.on ? 1 : .6, !f.on)); handles.push(...world);
    const start = labelSprite(`${f.reversed ? 'end' : 'start'} · ${f.edited ? 'edited preset' : 'preset'} ${f.type}`, f.on ? '#67e8f9' : '#94a3b8'); start.position.copy(world[0]).add(new THREE.Vector3(0, 28, 0)); enemyGuide.add(start);
    enemyGuide.userData.colours = handles.map(() => f.on ? PATH_COLOUR : PATH_OFF);
  }
  enemyGuide.userData.handles = handles.map(p => p.clone()); if (kind === 'wp') return;   // waypoints use the handle sprites
  const g = new THREE.BufferGeometry().setFromPoints(handles); g.setAttribute('color', new THREE.Float32BufferAttribute(enemyGuide.userData.colours.flatMap(c => new THREE.Color(c).toArray()), 3));
  const dots = new THREE.Points(g, new THREE.PointsMaterial({vertexColors: true, size: kind === 'wp' ? 14 : 9, sizeAttenuation: false, transparent: true}));
  const dimDots = new THREE.Points(g, new THREE.PointsMaterial({vertexColors: true, size: 6, sizeAttenuation: false, depthTest: false, depthWrite: false, transparent: true, opacity: .35}));
  dots.renderOrder = 16; dimDots.renderOrder = 14; enemyGuide.add(dots, dimDots);   // #160 in the scene + faint through walls
  handles.forEach((q, i) => { const dl = dropLine(q, new THREE.Color(enemyGuide.userData.colours[i]).getHex()); if (dl) enemyGuide.add(dl); });
}
let guideDrag = null; const UP = new THREE.Vector3(0, 1, 0);
// #92 handles, #160 as 3D objects: a small solid marker of the waypoint / arc colour (radius 7 units in the world, never
// smaller than 7 px or bigger than 15 px on screen: it no longer fills the view up close), lit in the scene and faint
// through walls. Hits are still tested on screen (16 px) before the gizmo and the selection, so a waypoint on top of the
// enemy is grabbable; a clicked handle gets the move gizmo (snap + undo like any object)
const WP_HANDLE_PX = 22, WP_HIT_PX = 16, HANDLE_R = 7, HANDLE_MIN_PX = 7, HANDLE_MAX_PX = 15;
function clampOnScreen(obj, worldR, minPx, maxPx) {   // scale obj so a radius of worldR stays between minPx and maxPx on screen
  const fit = (r, sc, cam) => {   // three.js calls this on the drawn parts (meshes / lines), not on a group: each part scales the whole
    const h = r.domElement.clientHeight || 600, d = Math.max(1, cam.position.distanceTo(obj.getWorldPosition(new THREE.Vector3())));
    const pxPerUnit = h / (2 * d * Math.tan(THREE.MathUtils.degToRad(cam.fov / 2))), px = THREE.MathUtils.clamp(worldR * pxPerUnit, minPx, maxPx);
    const k = px / (pxPerUnit * worldR); if (obj.scale.x !== k) { obj.scale.setScalar(k); obj.updateMatrixWorld(true); }
  };
  obj.traverse(x => { if (x.isMesh || x.isLine || x.isPoints) x.onBeforeRender = fit; });
}
const wpTex = new Map();
function wpHandle(colour, on) {   // #160: a small 3D marker (the old screen disc is wpDisc below, kept for reference)
  const g = new THREE.Group(), geo = new THREE.OctahedronGeometry(HANDLE_R, 1);
  const solid = new THREE.Mesh(geo, new THREE.MeshBasicMaterial({color: colour}));
  const edge = new THREE.LineSegments(new THREE.EdgesGeometry(geo), new THREE.LineBasicMaterial({color: on ? 0xffffff : 0x0f1116}));
  const ghost = new THREE.Mesh(geo, new THREE.MeshBasicMaterial({color: colour, transparent: true, opacity: .28, depthTest: false, depthWrite: false}));
  solid.renderOrder = 18; edge.renderOrder = 19; ghost.renderOrder = 17;
  if (on) edge.scale.setScalar(1.15);
  for (const x of [solid, edge, ghost]) { x.raycast = () => {}; g.add(x); }
  const r = on ? HANDLE_R * 1.25 : HANDLE_R; g.userData.px = on ? HANDLE_MAX_PX * 1.25 : HANDLE_MAX_PX; g.raycast = () => {};
  clampOnScreen(g, r, HANDLE_MIN_PX * (on ? 1.25 : 1), HANDLE_MAX_PX * (on ? 1.25 : 1)); return g;
}
function wpDisc(colour, on) {
  const key = colour + on; if (!wpTex.has(key)) { const c = document.createElement('canvas'); c.width = c.height = 64; const x = c.getContext('2d'); x.beginPath(); x.arc(32, 32, 27, 0, 7); x.fillStyle = colour; x.fill(); x.lineWidth = on ? 8 : 5; x.strokeStyle = on ? '#ffffff' : '#0f1116'; x.stroke(); if (on) { x.beginPath(); x.arc(32, 32, 9, 0, 7); x.fillStyle = '#ffffff'; x.fill(); } wpTex.set(key, new THREE.CanvasTexture(c)); }
  const sp = new THREE.Sprite(new THREE.SpriteMaterial({map: wpTex.get(key), depthTest: false, transparent: true, sizeAttenuation: false}));
  const px = WP_HANDLE_PX * (on ? 1.25 : 1); sp.userData.px = px; sp.renderOrder = 18; sp.raycast = () => {}; sp.onBeforeRender = (r, sc, cam) => { const h = r.domElement.clientHeight || 600; sp.scale.setScalar(px / h * 2 * Math.tan(THREE.MathUtils.degToRad(cam.fov / 2))); }; return sp;
}
function screenOf(p) { const r = renderer.domElement.getBoundingClientRect(), v = p.clone().project(camera); return v.z > 1 ? null : [r.left + (v.x + 1) / 2 * r.width, r.top + (1 - v.y) / 2 * r.height]; }
function handleAt(x, y) {   // -> index into enemyGuide.userData.handles of the nearest handle within the hit radius
  let best = -1, bd = Infinity; (enemyGuide.userData.handles || []).forEach((p, i) => { const s = screenOf(p); if (!s) return; const d = Math.hypot(s[0] - x, s[1] - y); if (d < bd) { bd = d; best = i; } });
  return bd <= WP_HIT_PX ? best : -1;
}
const wpUsed = w => w.mode === 'linear' ? 1 : w.mode === 'triangle' ? 2 : 0;   // how many waypoints its Behaviour walks
function setBehaviourWaypoints(o, n) {   // Stand / Patrol / Triangle (soldiers: AppearType; robots / pawns: WAIT_ACT + its move type)
  const at = fieldAt(o, 'AppearType'), wa = fieldAt(o, 'WaitActMoveType');
  if (wa >= 0) { setParamWord(o, at, 0); setParamWord(o, wa, n); } else setParamWord(o, at, n);
}
function placeWaypoint(it, k, point, why = '') {
  const o = it.o, w = waypointInfo(o); if (!w || !point) return; snapshot();
  const l = point.clone().applyMatrix4(wpFrame(o).invert()), lim = 3000, x = THREE.MathUtils.clamp(l.x, -lim, lim), z = THREE.MathUtils.clamp(l.z, -lim, lim);
  setParamWord(o, w.ix[k * 2], round2(x), true); setParamWord(o, w.ix[k * 2 + 1], round2(z), true);
  if (wpUsed(w) <= k) setBehaviourWaypoints(o, k + 1);   // placing an unused waypoint makes the Behaviour walk it
  wpSel = k; changed(); drawEnemyGuide(true); attachGizmo(); renderInspector();
  toast(`Waypoint ${k + 1} placed${why} at ${Math.round(x)}, ${Math.round(z)} from ${it.name}${x !== l.x || z !== l.z ? ' (clamped to 3000 from its spot)' : ''}`); leashCheck(it);
}
function armWaypoint(it, k) { placing = {name: `Waypoint ${k + 1}`, waypoint: k, it}; placingKeep = false; renderer.domElement.style.cursor = 'crosshair'; toast(`Click the surface where Waypoint ${k + 1} goes · Esc cancels`); }
function addWaypoint(it) {
  const o = it.o, w = waypointInfo(o); if (!w) return; const n = wpUsed(w); if (n >= 2) return toast('The game has two waypoints per enemy (Pos0, Pos1): both are in use', true);
  snapshot(); const k = n, def = k === 0 ? [0, 0, -150] : [150, 0, -150];   // visible at once (ahead of it); then the next click places it
  if (!w.set[k]) { setParamWord(o, w.ix[k * 2], def[0], true); setParamWord(o, w.ix[k * 2 + 1], def[2], true); }
  setBehaviourWaypoints(o, k + 1); wpSel = k; changed(); drawEnemyGuide(true); attachGizmo(); renderInspector(); armWaypoint(it, k);
}
function removeWaypoint(it, k) {
  const o = it.o, w = waypointInfo(o); if (!w) return; const n = wpUsed(w); snapshot();
  if (k === 0 && n === 2) { setParamWord(o, w.ix[0], w.w[1][0], true); setParamWord(o, w.ix[1], w.w[1][2], true); }   // Waypoint 2 becomes Waypoint 1
  const last = k === 0 && n === 2 ? 1 : k; setParamWord(o, w.ix[last * 2], 0, true); setParamWord(o, w.ix[last * 2 + 1], 0, true);
  if (n > 0) setBehaviourWaypoints(o, Math.max(0, n - 1));
  wpSel = null; changed(); drawEnemyGuide(true); attachGizmo(); renderInspector(); toast(`Waypoint ${k + 1} removed${n > 0 ? ` · Behaviour: ${['Stand', 'Patrol back and forth'][Math.max(0, n - 1)]}` : ''} (Ctrl Z undoes)`);
}
renderer.domElement.addEventListener('pointerdown', e => {
  if (e.button !== 0 || e.altKey || !project || !selected || multi.size || placing || drawing || faceMode || surfMode || !enemyKind(selected.o)) return;
  const hi = handleAt(e.clientX, e.clientY); if (hi < 0) return;
  const kind = enemyKind(selected.o), i = kind === 'wp' ? enemyGuide.userData.handleK[hi] : hi;
  if (gizmo.axis && !(kind === 'wp' && wpSel !== i)) return;   // the gizmo already on this waypoint: its arrows win
  e.preventDefault(); e.stopImmediatePropagation();
  const w = enemyGuide.userData.handles[hi].clone(), dir = new THREE.Vector3(); camera.getWorldDirection(dir);
  if (kind === 'wp' && wpSel !== i) { wpSel = i; drawEnemyGuide(true); attachGizmo(); renderInspector(); }
  guideDrag = {it: selected, kind, i, plane: new THREE.Plane().setFromNormalAndCoplanarPoint(kind === 'wp' ? UP : dir, w), x: e.clientX, y: e.clientY, moved: false, snapped: false};
  try { renderer.domElement.setPointerCapture?.(e.pointerId); } catch {}
}, true);
renderer.domElement.addEventListener('pointermove', e => {
  if (!guideDrag) return; const {it, kind, i} = guideDrag, o = it.o;
  if (!guideDrag.moved && Math.hypot(e.clientX - guideDrag.x, e.clientY - guideDrag.y) < 3) return;
  if (!guideDrag.snapped) { snapshot(); guideDrag.snapped = true; } guideDrag.moved = true;
  const r = renderer.domElement.getBoundingClientRect(); ndc.set((e.clientX - r.left) / r.width * 2 - 1, -(e.clientY - r.top) / r.height * 2 + 1); ray.setFromCamera(ndc, camera);
  const w = e.ctrlKey && kind === 'fly' ? surfacePoint(e.clientX, e.clientY) : ray.ray.intersectPlane(guideDrag.plane, new THREE.Vector3()); if (!w) return;
  const local = w.clone().applyMatrix4((kind === 'wp' ? wpFrame(o) : enemyFrame(o)).invert());
  if (kind === 'wp') { const wi = waypointInfo(o), lim = 3000; setParamWord(o, wi.ix[i * 2], round2(THREE.MathUtils.clamp(local.x, -lim, lim)), true); setParamWord(o, wi.ix[i * 2 + 1], round2(THREE.MathUtils.clamp(local.z, -lim, lim)), true); }
  else { const f = flyerInfo(o); if (!f.points) return; const pts = f.points.map(p => [...p]); pts[i] = local.toArray().map(v => round2(THREE.MathUtils.clamp(v, -20000, 20000))); (project.enemyPaths ||= {})[String(f.type)] = {points: pts, closed: f.closed}; }
  drawEnemyGuide(true);
}, true);
addEventListener('pointerup', () => {
  if (!guideDrag) return; const {it, kind, moved} = guideDrag; guideDrag = null; if (!moved) return;
  changed(); if (kind === 'wp') attachGizmo(); renderInspector();
  if (kind === 'fly') toast(`Flight path preset ${flyerInfo(it.o).type} changed: every flyer using it in this mod flies the new shape (Reset to original in the inspector)`);
  else { toast('Waypoint moved'); leashCheck(it); }
}, true);
// ---- #124 launch arcs (springs, dash rings, ramps, panels): the path Shadow flies, hands off the stick (springs.js,
// the measured model). Shown for the selected launcher; the Launch arcs lens shows every one. Stops where it meets
// the stage's collision (or the art when the stage has none, and your shapes) or after 3 s. Drag the yellow disc (the
// landing) or the violet disc (the top) to aim it: rotation + Power (+ Height / No control) in one undo step.
const springGuide = new THREE.Group(); springGuide.renderOrder = 15; scene.add(springGuide);
let showArcs = (() => { try { return localStorage.getItem('ui2.arcs') === '1'; } catch { return false; } })(), arcDrag = null, arcGrid = null;
const ARC_LAND = '#fde047', ARC_TOP = '#a78bfa';
const launcherKind = o => o?.kind === 'native' ? LAUNCHERS[`${o.list}-${o.type}`] || null : null;
function arcState(o) { const f = n => { const i = fieldAt(o, n); return i >= 0 ? paramWord(o, i, true) : null; }; return {rot: [...(o.rotation || [0, 0, 0])], S: f('Strength') ?? 2, H: f('Height') ?? 0, nct: f('NoControlTime') ?? 0}; }
function arcColliders() {
  const base = coll.children.length ? coll.children : art.children.filter(m => m.visible && !m.userData.backdrop && !m.userData.shellIn);
  const geo = items.filter(it => it.o.kind === 'geometry').map(it => nodeOf.get(it)).filter(Boolean);
  return {list: [...base, ...geo], key: base.map(m => m.id).join() + '|' + geo.map(n => n.id + ':' + n.matrixWorld.elements.join()).join()};
}
// a grid of the collider triangles (cells of 200 units) so a 3-frame segment tests a few hundred triangles, not all
function arcEnv() {
  const c = arcColliders(); if (arcGrid?.key === c.key) return arcGrid.env;
  const T = [], cells = new Map(), big = [], C = 200, v = new THREE.Vector3();
  for (const m of c.list) m.traverse(x => { if (!x.isMesh || !x.geometry?.attributes?.position) return; x.updateWorldMatrix(true, false); const pa = x.geometry.attributes.position, ix = x.geometry.index, n = ix ? ix.count : pa.count;
    for (let i = 0; i + 2 < n; i += 3) { const k = T.length / 9; for (let j = 0; j < 3; j++) { v.fromBufferAttribute(pa, ix ? ix.getX(i + j) : i + j).applyMatrix4(x.matrixWorld); T.push(v.x, v.y, v.z); }
      const lo = [0, 1, 2].map(a => Math.floor(Math.min(T[k * 9 + a], T[k * 9 + 3 + a], T[k * 9 + 6 + a]) / C)), hi = [0, 1, 2].map(a => Math.floor(Math.max(T[k * 9 + a], T[k * 9 + 3 + a], T[k * 9 + 6 + a]) / C));
      if ((hi[0] - lo[0] + 1) * (hi[1] - lo[1] + 1) * (hi[2] - lo[2] + 1) > 512) { big.push(k); continue; }
      for (let a = lo[0]; a <= hi[0]; a++) for (let b = lo[1]; b <= hi[1]; b++) for (let d = lo[2]; d <= hi[2]; d++) { const key = a + ',' + b + ',' + d; let l = cells.get(key); if (!l) cells.set(key, l = []); l.push(k); } } });
  const stamp = new Uint32Array(T.length / 9); let gen = 0;
  const seg = (p, q) => {   // nearest triangle crossing p -> q (Moller-Trumbore, both sides)
    gen++; let best = null, bt = 1.0001; const d = [q[0] - p[0], q[1] - p[1], q[2] - p[2]];
    const test = k => { if (stamp[k] === gen) return; stamp[k] = gen; const o = k * 9, e1 = [T[o + 3] - T[o], T[o + 4] - T[o + 1], T[o + 5] - T[o + 2]], e2 = [T[o + 6] - T[o], T[o + 7] - T[o + 1], T[o + 8] - T[o + 2]];
      const h = [d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]], a = e1[0] * h[0] + e1[1] * h[1] + e1[2] * h[2]; if (Math.abs(a) < 1e-9) return;
      const f = 1 / a, s = [p[0] - T[o], p[1] - T[o + 1], p[2] - T[o + 2]], u = f * (s[0] * h[0] + s[1] * h[1] + s[2] * h[2]); if (u < 0 || u > 1) return;
      const qq = [s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0]], w = f * (d[0] * qq[0] + d[1] * qq[1] + d[2] * qq[2]); if (w < 0 || u + w > 1) return;
      const t = f * (e2[0] * qq[0] + e2[1] * qq[1] + e2[2] * qq[2]); if (t < 0 || t >= bt) return;
      bt = t; const nx = e1[1] * e2[2] - e1[2] * e2[1], ny = e1[2] * e2[0] - e1[0] * e2[2], nz = e1[0] * e2[1] - e1[1] * e2[0], nl = Math.hypot(nx, ny, nz) || 1, sg = nx * d[0] + ny * d[1] + nz * d[2] > 0 ? -1 : 1;
      best = {point: [p[0] + d[0] * t, p[1] + d[1] * t, p[2] + d[2] * t], normal: [sg * nx / nl, sg * ny / nl, sg * nz / nl]}; };
    const lo = [0, 1, 2].map(a => Math.floor(Math.min(p[a], q[a]) / C)), hi = [0, 1, 2].map(a => Math.floor(Math.max(p[a], q[a]) / C));
    if ((hi[0] - lo[0] + 1) * (hi[1] - lo[1] + 1) * (hi[2] - lo[2] + 1) <= 64) { for (let a = lo[0]; a <= hi[0]; a++) for (let b = lo[1]; b <= hi[1]; b++) for (let e = lo[2]; e <= hi[2]; e++) cells.get(a + ',' + b + ',' + e)?.forEach(test); }
    else for (const l of cells.values()) l.forEach(test);
    big.forEach(test); return best;
  };
  const env = {hit: seg, floor: p => seg(p, [p[0], p[1] - 80, p[2]])?.point[1] ?? null, tris: T.length / 9};
  arcGrid = {key: c.key, env}; return env;
}
const arcCache = new Map();
function arcOf(o) {
  const kind = launcherKind(o); if (!kind) return null; const env = arcEnv(), key = JSON.stringify([o.position, o.rotation, o.params]) + arcGrid.key.length + ':' + env.tris;
  const c = arcCache.get(o.id); if (c?.key === key) return c.arc;
  const arc = arcTrace(kind, arcState(o), o.position, env); if (arc) arc.kind = kind; arcCache.set(o.id, {key, arc}); return arc;
}
function arcSummary(a) {
  if (!a) return ''; const p0 = a.points[0], far = Math.round(Math.hypot(a.end.p[0] - p0[0], a.end.p[2] - p0[2])), top = Math.round(a.apex.p[1] - p0[1]);
  const where = far < 10 ? 'back on the spot' : `${far} away`;
  return a.end.why === 'time' ? `no ground within ${a.end.t.toFixed(1)} s · ${where}` : `${a.ground ? 'runs and stops' : 'lands'} after ${a.end.t.toFixed(1)} s · ${where}${a.ground ? '' : ` · ${top} high`}`;
}
function drawSpringGuide(force = false) {
  const sel = selected && !multi.size && launcherKind(selected.o) ? selected : null;
  const list = showArcs ? items.filter(it => launcherKind(it.o) && visible(it)) : sel ? [sel] : [];
  const sig = list.map(it => it.o.id + JSON.stringify([it.o.position, it.o.rotation, it.o.params])).join('|') + '|' + (sel?.o.id || '') + '|' + (list.length ? arcColliders().key.length : 0) + '|' + (arcDrag?.why || '');
  if (!force && sig === springGuide.userData.sig) return; springGuide.userData.sig = sig; springGuide.userData.handles = [];
  for (const c of [...springGuide.children]) { springGuide.remove(c); c.traverse?.(x => { x.geometry?.dispose?.(); if (!x.userData?.sharedMat) { x.material?.map?.dispose?.(); x.material?.dispose?.(); } }); }
  for (const it of list) {
    const a = arcOf(it.o), on = it === sel; if (!a || a.points.length < 2) continue; const pts = a.points.map(p => new THREE.Vector3(...p)), C = new THREE.Color(ARC_LAND).getHex();
    springGuide.add(guideLine(pts, C, on ? .9 : .4, true));
    const dots = new THREE.Points(new THREE.BufferGeometry().setFromPoints(pts.filter((_, i) => i % 6 === 0)), new THREE.PointsMaterial({color: C, size: on ? 5 : 3, sizeAttenuation: false, transparent: true, opacity: on ? 1 : .5}));
    dots.renderOrder = 15; dots.raycast = () => {}; springGuide.add(dots);   // a dot every 0.1 s: wide gaps = fast
    const end = pts.at(-1), lost = a.end.why === 'time';
    const ring = new THREE.Mesh(new THREE.RingGeometry(lost ? 6 : 12, lost ? 8 : 16, 32), new THREE.MeshBasicMaterial({color: lost ? 0xf87171 : C, side: THREE.DoubleSide, transparent: true, opacity: on ? .9 : .5, depthWrite: false}));
    ring.position.copy(end).add(new THREE.Vector3(...(a.end.normal || [0, 1, 0])).multiplyScalar(.6)); ring.lookAt(end.clone().add(new THREE.Vector3(...(a.end.normal || [0, 1, 0])).multiplyScalar(2)));
    ring.renderOrder = 15; ring.raycast = () => {}; clampOnScreen(ring, lost ? 8 : 16, 6, 40); springGuide.add(ring);   // #160: never fills the view up close
    if (on && !a.ground) { const dl = dropLine(new THREE.Vector3(...a.apex.p), new THREE.Color(ARC_TOP).getHex(), null, a.end.p[1]); if (dl) springGuide.add(dl); }
    if (!on) continue;
    const high = !a.ground && a.apex.p[1] > a.points[0][1] + 20;
    if (high) { const g = shadowGhost(new THREE.Color(ARC_TOP).getHex()), i = Math.round(a.apex.t * 60), v = a.points[Math.min(a.points.length - 1, i + 1)], u = a.points[Math.max(0, i - 1)];
      g.traverse(x => { if (x.isMesh && x.material?.map) x.userData.sharedMat = true; }); g.position.set(...a.apex.p); g.position.y -= 4; g.rotation.y = Math.atan2(v[0] - u[0], v[2] - u[2]); springGuide.add(g); }
    const why = arcDrag?.why ? ' · ' + arcDrag.why : '', sp = labelSprite(`${LAUNCHER_NAMES[a.kind]}: ${arcSummary(a)}${why}`, arcDrag?.out || lost ? '#f87171' : ARC_LAND); sp.center.set(.5, -.8); sp.position.copy(end); springGuide.add(sp);
    const hs = []; if (a.kind !== 'digital') hs.push(['land', end, ARC_LAND]); if (high && a.kind !== 'panel') hs.push(['top', new THREE.Vector3(...a.apex.p), ARC_TOP]);
    for (const [k, p, col] of hs) { const h = wpHandle(col, arcDrag?.k === k); h.userData.sharedMat = true; h.position.copy(p); springGuide.add(h); springGuide.userData.handles.push({k, p: p.clone()}); }
  }
}
function arcHandleAt(x, y) { let best = null, bd = Infinity; for (const h of springGuide.userData.handles || []) { const s = screenOf(h.p); if (!s) continue; const d = Math.hypot(s[0] - x, s[1] - y); if (d < bd) { bd = d; best = h; } } return bd <= WP_HIT_PX ? best : null; }
function applyArc(it, st) {
  const o = it.o; o.rotation = st.rot.map(v => Math.round(((v + 540) % 360 - 180) * 10) / 10);
  nodeOf.get(it)?.rotation.set(...o.rotation.map(THREE.MathUtils.degToRad));
  for (const [f, v] of [['Strength', st.S], ['Height', st.H], ['NoControlTime', st.nct]]) { const i = fieldAt(o, f); if (i >= 0 && Number.isFinite(v)) setParamWord(o, i, round2(v), true); }
}
function aimArc(it, k, q) {   // -> the solve, applied to the object
  const o = it.o, kind = launcherKind(o), st = arcState(o), r = k === 'top' ? solveApex(kind, st, o.position, q.toArray()) : solveLanding(kind, st, o.position, q.toArray(), arcEnv());
  if (!r) return null; applyArc(it, r.state); return r;
}
renderer.domElement.addEventListener('pointerdown', e => {
  if (e.button !== 0 || e.altKey || !project || !selected || multi.size || placing || drawing || faceMode || surfMode || !launcherKind(selected.o)) return;
  const h = arcHandleAt(e.clientX, e.clientY); if (!h || gizmo.axis) return; e.preventDefault(); e.stopImmediatePropagation();
  const dir = new THREE.Vector3(); camera.getWorldDirection(dir);
  arcDrag = {it: selected, k: h.k, plane: new THREE.Plane().setFromNormalAndCoplanarPoint(h.k === 'top' ? dir : UP, h.p), x: e.clientX, y: e.clientY, moved: false, why: ''};
  try { renderer.domElement.setPointerCapture?.(e.pointerId); } catch {} drawSpringGuide(true);
}, true);
function arcDragTo(clientX, clientY) {
  const r = renderer.domElement.getBoundingClientRect(); ndc.set((clientX - r.left) / r.width * 2 - 1, -(clientY - r.top) / r.height * 2 + 1); ray.setFromCamera(ndc, camera);
  let q = null; if (arcDrag.k === 'land') { const own = nodeOf.get(arcDrag.it), hit = ray.intersectObjects([...art.children.filter(m => m.visible && !m.userData.backdrop && !m.userData.shellIn), ...nodes.children.filter(n => !n.userData.volume && n !== own)], true)[0]; q = hit?.point || null; }
  q ||= ray.ray.intersectPlane(arcDrag.plane, new THREE.Vector3()); if (!q) return null;
  const s = aimArc(arcDrag.it, arcDrag.k, q); if (!s) return null; arcDrag.out = s.err > 25; arcDrag.why = arcDrag.out ? `out of reach: ${Math.round(s.err)} off` : '';
  drawSpringGuide(true); attachGizmo(); return s;
}
renderer.domElement.addEventListener('pointermove', e => {
  if (!arcDrag) return; if (!arcDrag.moved && Math.hypot(e.clientX - arcDrag.x, e.clientY - arcDrag.y) < 3) return;
  if (!arcDrag.moved) { snapshot(); arcDrag.moved = true; } arcDragTo(e.clientX, e.clientY);
}, true);
function arcDragEnd() {
  const {it, moved, out, why, k} = arcDrag; arcDrag = null; drawSpringGuide(true);
  if (!moved) { arcSel = k; attachGizmo(); toast(`${it.name}: the ${k === 'top' ? 'top' : 'landing'} of its arc has the move gizmo (drag the arrows; Esc / another click lets go)`); return; }   // #160 a click selects the handle
  changed(); attachGizmo(); renderInspector(); const st = arcState(it.o), kind = launcherKind(it.o);
  toast(`${it.name} aimed by its ${k === 'top' ? 'top' : 'landing'}: rotation ${it.o.rotation.join(', ')} · Power ${round2(st.S)}${kind === 'ramp' ? ` · Height ${round2(st.H)}` : ''}${kind === 'panel' ? ` · No control ${round2(st.nct)} s` : ''}${out ? ` · ${why}: the closest it gets` : ''} (Ctrl Z undoes)`, out);
}
addEventListener('pointerup', () => { if (arcDrag) arcDragEnd(); }, true);
function appendArcCard() {
  const el = $('inspector'), o = selected && !multi.size ? selected.o : null, kind = launcherKind(o); if (!el || !kind) return; const a = arcOf(o);
  const html = `<details class="sect" open id="arcCard"><summary>Launch arc <small class="tagNo">approximate</small></summary><div><p>${esc(a ? arcSummary(a) : 'no arc')}.</p><p class="muted">The dotted line is the path Shadow flies with the stick untouched (measured in game: about 10 units off over 3 s; a dot every 0.1 s). ${kind === 'digital' ? 'Drag the violet disc in the 3D view to set how high it throws (Power).' : `Drag the yellow disc in the 3D view to aim where he lands${kind === 'panel' ? '' : ', or the violet one to set the top of the arc'}: rotation and Power${kind === 'ramp' ? ' and Height' : ''}${kind === 'panel' ? ' (on the floor: No control, about 750 units per second)' : ''} change in one undo step.`}${kind === 'digital' ? ' Only straight up was measured.' : ''}</p><div class="wsBtns"><button class="ghost small" id="arcAll">${showArcs ? 'Hide' : 'Show'} every launch arc</button></div></div></details>`;
  const par = [...el.querySelectorAll('details.sect')].find(d => d.querySelector('summary')?.textContent.startsWith('Parameters'));
  if (par) par.insertAdjacentHTML('afterend', html); else el.insertAdjacentHTML('beforeend', html);
  el.querySelector('#arcAll')?.addEventListener('click', () => setShowArcs(!showArcs));
}
function setShowArcs(on) { showArcs = on; try { localStorage.setItem('ui2.arcs', on ? '1' : '0'); } catch {} renderLens(); if (selected) renderInspector(); }
function appendEnemyCard() {
  const el = $('inspector'), o = selected && !multi.size ? selected.o : null, kind = enemyKind(o); if (!el || !kind) return;
  let html;
  if (kind === 'wp') {
    const w = waypointInfo(o); if (!w) return;
    const what = {stand: 'stands at its spot (the waypoints are not walked)', linear: 'walks spot <-> Waypoint 1', triangle: 'walks spot -> Waypoint 1 -> Waypoint 2 -> spot', random: `wanders inside Move range (${Math.round(w.range)})`, other: 'enters / moves by its Behaviour (the waypoints are not walked)'}[w.mode];
    const used = wpUsed(w);
    html = `<details class="sect" open id="enemyCard"><summary>Waypoints <small class="tagNo">approximate</small></summary><div><div class="wsBtns"><button class="ghost small" id="wpAdd" ${used >= 2 ? 'disabled title="The game has two waypoints per enemy"' : ''}>+ Add waypoint</button>${used ? `<button class="ghost small" id="wpDel">Remove waypoint ${used}</button>` : ''}</div><p class="muted">${used} of 2 walked. Click a coloured disc in the 3D view to move it with the arrows, or drag the disc itself: Waypoint 1 / 2 = Pos0 / Pos1 X, Z from its spot (${w.w.map(p => `${Math.round(p[0])}, ${Math.round(p[2])}`).join(' and ')}). With this Behaviour it ${what}. Offsets are on the world axes, whichever way the enemy faces (proven in game).</p>${(() => { const L = wpLeash(o, w); return !L || !(w.range > 0) ? '' : L.out ? `<p class="badText">Waypoint ${Math.round(L.far)} away but Move range is ${Math.round(L.range)}: the enemy stops at the circle (the build raises it in its copy). <button class="ghost small" id="wpExtend">Extend range to ${L.need}</button></p>` : `<p class="muted">Move range ${Math.round(L.range)}: the light circle it never leaves.</p>`; })()}</div></details>`;
  } else {
    loadEnemyPresets(); const f = flyerInfo(o), ol = catalog.get(`${o.list}-${o.type}`)?.fields?.find(x => x.name === 'PathType')?.optionLabels?.[String(f.type)];
    html = `<details class="sect" open id="enemyCard"><summary>Flight path preset <small class="tagNo">shared</small></summary><div>
      ${!enemyPresets ? '<p class="muted">Reading the presets (enemy/EnemyPath.one)…</p>' : !f.points ? `<p class="badText">No preset for Flight path ${f.type}.</p>` : `<p><b>${esc(ol?.label || 'Preset ' + f.type)}</b> (${esc(f.preset?.file || '?')}) · ${f.points.length} points${f.closed ? ' · closed' : ''} · ${f.edited ? '<b>edited in this project</b>' : 'original'}${f.reversed ? ' · flown backwards (Patrol reversed)' : ''}${f.on ? '' : ' · <span class="warnText">not flown: Behaviour is not "Fly a preset path"</span>'}</p>
      <p class="warnText">This preset is one shared file (enemy/EnemyPath.one): editing it changes Flight path ${f.type} for EVERY flyer that uses it in stages played with this mod.</p>
      <p class="muted">Drag its points in the 3D view (in the view plane; Ctrl = onto the surface). The flown shape is not checked in game yet.</p>${f.edited ? '<div class="wsBtns"><button class="ghost small" id="epReset">Reset to original</button></div>' : ''}
      <details class="wpBox" style="--wp:${f.on ? PATH_COLOUR : PATH_OFF}"><summary class="wpHead"><span class="wpDot"></span>Path points<small>${f.points.length} · local to the enemy (forward = -Z) · the ${f.on ? 'cyan' : 'grey'} points in the 3D view</small></summary>
        <div class="ptGrid"><span></span><b>X</b><b>Y</b><b>Z</b>${f.points.map((p, i) => `<span class="ptN" title="${i === 0 ? (f.reversed ? 'end (flown backwards)' : 'start') : ''}">${i + 1}${i === 0 ? '▸' : ''}</span>${p.map((v, k) => `<input class="num" type="number" step="any" data-epp="${i}" data-epk="${k}" value="${+(+v).toFixed(2)}">`).join('')}`).join('')}</div></details>`}</div></details>`;
  }
  if (kind === 'wp') { const w = waypointInfo(o), used = wpUsed(w);
    el.querySelectorAll('.wpBtns[data-wpk]').forEach(b => { const k = +b.dataset.wpk; b.innerHTML = `${k < used ? '' : '<small class="muted">not walked with this Behaviour; placing it makes it walked · </small>'}<button class="ghost small" data-wpa="mouse" data-k="${k}">Place at mouse</button><button class="ghost small" data-wpa="cam" data-k="${k}" title="The surface under the centre of the 3D view">Place at camera view</button>${w.set[k] || k < used ? `<button class="ghost small" data-wpa="sel" data-k="${k}">${wpSel === k ? 'Selected' : 'Select'}</button><button class="ghost small" data-wpa="del" data-k="${k}">Remove</button>` : ''}`; });
    el.querySelectorAll('[data-wpa]').forEach(b => b.onclick = () => { const k = +b.dataset.k, a = b.dataset.wpa; if (a === 'mouse') armWaypoint(selected, k); else if (a === 'cam') placeWaypoint(selected, k, centrePoint(), ' at the camera view'); else if (a === 'del') removeWaypoint(selected, k); else { wpSel = k; drawEnemyGuide(true); attachGizmo(); renderInspector(); } }); }
  const par = [...el.querySelectorAll('details.sect')].find(d => d.querySelector('summary')?.textContent.startsWith('Parameters'));
  if (par) par.insertAdjacentHTML('afterend', html); else el.insertAdjacentHTML('beforeend', html);
  el.querySelector('#wpExtend')?.addEventListener('click', () => extendRange(selected));
  el.querySelectorAll('[data-epp]').forEach(inp => inp.onchange = () => { const v = +inp.value; if (!Number.isFinite(v)) return; const f = flyerInfo(o); if (!f.points) return; snapshot();
    const pts = f.points.map(p => [...p]); pts[+inp.dataset.epp][+inp.dataset.epk] = round2(THREE.MathUtils.clamp(v, -20000, 20000)); (project.enemyPaths ||= {})[String(f.type)] = {points: pts, closed: f.closed}; changed(); drawEnemyGuide(true);
    const open = el.querySelector('#enemyCard details.wpBox')?.open; renderInspector(); const d = $('inspector').querySelector('#enemyCard details.wpBox'); if (d && open) d.open = true; });
  el.querySelector('#wpAdd')?.addEventListener('click', () => addWaypoint(selected));
  el.querySelector('#wpDel')?.addEventListener('click', () => removeWaypoint(selected, wpUsed(waypointInfo(selected.o)) - 1));
  el.querySelector('#epReset')?.addEventListener('click', () => { const f = flyerInfo(o); snapshot(); delete project.enemyPaths[String(f.type)]; if (!Object.keys(project.enemyPaths).length) delete project.enemyPaths; changed(); drawEnemyGuide(true); renderInspector(); toast(`Flight path ${f.type}: back to the original preset`); });
}
// #77 path points in the viewport (the selected path): drag a blue point (in the view plane; Ctrl = onto the surface
// under the cursor), click the line to insert a point there (and drag it), Shift-drag the first / last point to extend
// the path, Delete removes the selected point. Points stay in the path's local frame (o.points), so the path's
// position / rotation / scale and the built PATH.PTP agree.
let ptDrag = null;
const pickThreshold = () => Math.max(6, camera.position.distanceTo(orbit.target) * .012);
renderer.domElement.addEventListener('pointerdown', e => {
  if (e.button !== 0 || e.altKey || !project || !selected || selected.o.kind !== 'path' || placing || drawing || faceMode || surfMode || (gizmo.axis && !e.shiftKey)) return;   // Shift (extend an end) wins over the handle
  const it = selected, n = nodeOf.get(it); if (!n) return;
  const r = renderer.domElement.getBoundingClientRect(); ndc.set((e.clientX - r.left) / r.width * 2 - 1, -(e.clientY - r.top) / r.height * 2 + 1); ray.setFromCamera(ndc, camera);
  const th = pickThreshold(); ray.params.Points.threshold = th; ray.params.Line.threshold = th * .6;
  const dots = n.children.find(c => c.isPoints), line = n.children.find(c => c.isLine && !c.isPoints);
  const hp = dots && ray.intersectObject(dots, false).sort((a, b) => a.distanceToRay - b.distanceToRay)[0];
  let i = hp?.index ?? null, made = false;
  n.updateMatrixWorld(true);
  if (i !== null && e.shiftKey && (i === 0 || i === it.o.points.length - 1)) {   // extend from an end
    snapshot(); const at = i === 0 ? 0 : it.o.points.length; it.o.points.splice(at, 0, [...it.o.points[i]]); i = at; made = true;
  } else if (i === null && line) {   // insert on the line
    const hl = ray.intersectObject(line, false)[0]; if (!hl) return;
    const seg = hl.index ?? 0, local = n.worldToLocal(hl.point.clone()).toArray().map(round2);
    snapshot(); it.o.points.splice(seg + 1, 0, local); i = seg + 1; made = true;
  }
  if (i === null || (gizmo.axis && !made)) return;
  e.preventDefault(); e.stopImmediatePropagation();
  const world = new THREE.Vector3(...it.o.points[i]).applyMatrix4(n.matrixWorld), dir = new THREE.Vector3(); camera.getWorldDirection(dir);
  ptDrag = {it, i, plane: new THREE.Plane().setFromNormalAndCoplanarPoint(dir, world), moved: made, snapped: made, x: e.clientX, y: e.clientY};
  if (made) { it.o.autoOrient = true; refreshPath(it); relocate(it); }
  renderer.domElement.setPointerCapture?.(e.pointerId);
}, true);
renderer.domElement.addEventListener('pointermove', e => {
  if (!ptDrag) return; const {it} = ptDrag, n = nodeOf.get(it); if (!n) return;
  if (!ptDrag.moved && Math.hypot(e.clientX - ptDrag.x, e.clientY - ptDrag.y) < 3) return;
  if (!ptDrag.snapped) { snapshot(); ptDrag.snapped = true; } ptDrag.moved = true;
  const r = renderer.domElement.getBoundingClientRect(); ndc.set((e.clientX - r.left) / r.width * 2 - 1, -(e.clientY - r.top) / r.height * 2 + 1); ray.setFromCamera(ndc, camera);
  const w = e.ctrlKey ? surfacePoint(e.clientX, e.clientY) : ray.ray.intersectPlane(ptDrag.plane, new THREE.Vector3()); if (!w) return;
  n.updateMatrixWorld(true); it.o.points[ptDrag.i] = n.worldToLocal(w.clone()).toArray().map(round2); it.o.autoOrient = true; refreshPath(it);
  if (pathPoint === ptDrag.i) attachGizmo();
}, true);
addEventListener('pointerup', () => {
  if (!ptDrag) return; const {it, i, moved} = ptDrag; ptDrag = null;
  if (moved) { relocate(it); changed(); drawFlow(); }
  pathPoint = i; attachGizmo(); renderInspector();
}, true);
function insertPathPoint(it, after) {   // inspector: a point halfway to the next one (or 100 further along)
  const pts = it.o.points, a = pts[after], b = pts[after + 1]; snapshot();
  pts.splice(after + 1, 0, b ? a.map((v, k) => round2((v + b[k]) / 2)) : a.map((v, k) => round2(v + (k === 2 ? -100 : 0))));
  it.o.autoOrient = true; refreshPath(it); relocate(it); changed(); pathPoint = after + 1; attachGizmo(); renderInspector();
}
function deletePathPoint(it, i) {
  if (it.o.points.length <= 2) return toast('A path needs at least two points.', true);
  snapshot(); it.o.points.splice(i, 1); it.o.autoOrient = true; refreshPath(it); relocate(it); changed();
  pathPoint = Math.min(i, it.o.points.length - 1); attachGizmo(); renderInspector(); toast(`Point ${i + 1} removed (${it.o.points.length} left)`);
}
const nextSplineId = () => Math.max(1000, ...(project.paths || []).map(p => p.splineId || 0)) + 1;   // classic: new paths above every used ID
function setPathPoint(i) { pathPoint = i; attachGizmo(); renderInspector(); toast(`Point ${i + 1} of ${selected.o.points.length} · drag the arrows · Esc / click elsewhere ends`); }
function refreshPath(it) {   // keep the node (the handle is attached) and update its line and points in place
  const n = nodeOf.get(it), o = it.o; if (!n) return; const pts = o.points.map(q => new THREE.Vector3(...q));
  for (const c of n.children) { if (c.isPoints) c.geometry.setFromPoints(pts); else if (c.isLine) c.geometry.setFromPoints(o.closed ? [...pts, pts[0]] : pts); c.geometry.computeBoundingSphere(); }
  selRing.children.forEach(c => c.update?.());
}
// #44 per-object texture: project.textureOverrides = {<game texture name>: custom material from /api/upload}; previewed
// in every model / stage-art material that uses the name; opening the file needs the server (NOTE)
R.texOverride = name => name && project?.textureOverrides?.[name]?.asset;
const texPicker = Object.assign(document.createElement('input'), {type: 'file', accept: 'image/png,image/*', hidden: true}); document.body.append(texPicker); let texFor = null;
texPicker.onchange = async () => {
  const f = texPicker.files[0], name = texFor; if (!f || !project || !name) return;
  try {
    const bytes = new Uint8Array(await f.arrayBuffer()); let bin = ''; for (let i = 0; i < bytes.length; i += 16384) bin += String.fromCharCode(...bytes.slice(i, i + 16384));
    const m = await api('/api/upload', {project: project.id, name: f.name, kind: 'texture', data: btoa(bin)});
    snapshot(); if (!project.materials.some(x => x.id === m.id)) project.materials.push(m);
    (project.textureOverrides ||= {})[name] = {id: m.id, asset: m.asset, file: m.file, name: f.name, width: m.width, height: m.height};
    refreshTextures(); changed(); renderInspector(); toast(`${name} replaced by ${f.name} (${m.width}×${m.height}) in the editor`);
  } catch (e) { toast(e.message, true); } finally { texPicker.value = ''; }
};
function refreshTextures() { buildArt(); buildNodes(); attachGizmo(); }
async function openTextureFile(asset) {
  if (!asset) return toast('This texture has no image on the server.', true);
  try { const r = await api('/api/reveal', {asset}); toast(r.path ? 'Opened in Explorer: ' + r.path : 'Opened in Explorer'); }
  catch (e) { toast('Could not open it in Explorer: ' + e.message, true); }
}
function rebuildNode(it) { const n = nodeOf.get(it); if (n) { nodes.remove(n); nodeOf.delete(it); } addNode(it); buildMarkers(); attachGizmo(); }
viewport.addEventListener('dragover', e => { if (dragRow) { e.preventDefault(); e.dataTransfer.dropEffect = 'copy'; } });
viewport.addEventListener('drop', e => { if (!dragRow) return; e.preventDefault(); place(dragRow, surfacePoint(e.clientX, e.clientY)); dragRow = null; if (panel === 'add') renderAdd.refresh?.(); });
addEventListener('dragend', () => dragRow = null);

// ---- move / rotate handles, delete, duplicate -------------------------------------------------------------------------
const gizmo = new TransformControls(camera, renderer.domElement); gizmo.setSize(.9); scene.add(gizmo.getHelper());
let proxy = new THREE.Object3D(); scene.add(proxy);
const gizmoBar = Object.assign(document.createElement('div'), {className: 'gizmoBar', id: 'gizmoBar'});
gizmoBar.innerHTML = [['translate', 'Move', 'G'], ['rotate', 'Rotate', 'R'], ['scale', 'Scale', 'T cycles']].map(([m, l, k]) => `<button data-gm="${m}" title="${l} (${k})">${l}</button>`).join('');
viewport.append(gizmoBar);
gizmoBar.onclick = e => { const b = e.target.closest('[data-gm]'); if (b && !b.disabled) setGizmoMode(b.dataset.gm); };
// ---- #94 snap / magnet (like Forge): Snap on = while the gizmo moves something (or right after a piece is placed) it
// is pulled onto its neighbours within the snap distance: a piece's box flush face to face, edge to edge or corner to
// corner with nearby pieces (shapes, native objects; stage art pieces against art and pieces), else its bottom onto the
// stage surface below; a waypoint / path point onto a nearby corner or centre of a piece (a path point also onto the
// surface). Grid (units, 0 = off) and rotation steps use the gizmo's own snap. Hold Alt to invert Snap for the moment.
// The contact face is drawn amber (the target piece's box faintly) and the bar says what snapped where, with the distance.
// One undo step per drag, as before; settings per browser (ui2.snap); N or Ctrl K > Toggle snap.
const snapState = (() => { const d = {on: false, dist: 30, rot: 15, grid: 0}; try { return {...d, ...JSON.parse(localStorage.getItem('ui2.snap') || '{}')}; } catch { return d; } })();
const saveSnap = () => { try { localStorage.setItem('ui2.snap', JSON.stringify(snapState)); } catch {} };
const snapGroup = new THREE.Group(); scene.add(snapGroup); let snapCache = null, snapAlt = false, snapNested = false;
const snapOn = () => snapState.on !== snapAlt;   // Alt held inverts
gizmoBar.insertAdjacentHTML('beforeend', `<span class="gzSep"></span><button data-snap title="Snap / magnet (N, or Ctrl K > Toggle snap): pull what you move flush to nearby faces, edges, corners and the surface below; hold Alt to invert for the moment">Snap</button><label class="gzNum" title="Snap distance (units)"><span>reach</span><input type="number" id="snapDist" min="1" max="500" step="1"></label><label class="gzNum" title="Grid step while Snap is on (units, 0 = no grid)"><span>grid</span><input type="number" id="snapGrid" min="0" max="1000" step="1" placeholder="off"></label><label class="gzNum" title="Rotation step (degrees) while Snap is on"><span>turn</span><select id="snapRot">${[5, 10, 15, 30, 45, 90].map(v => `<option value="${v}">${v}°</option>`).join('')}</select></label><span class="gzInfo" id="snapInfo"></span>`);
function applySnapUi() {
  const b = gizmoBar.querySelector('[data-snap]'), on = snapOn(); b.classList.toggle('on', snapState.on); b.classList.toggle('inv', snapAlt); b.textContent = snapAlt ? `Snap ${on ? 'on' : 'off'} (Alt)` : 'Snap';
  $('snapDist').value = snapState.dist; $('snapGrid').value = snapState.grid || ''; $('snapRot').value = String(snapState.rot);
  gizmo.setRotationSnap(on ? THREE.MathUtils.degToRad(snapState.rot) : null); gizmo.setTranslationSnap(on && snapState.grid > 0 ? snapState.grid : null);
}
function toggleSnap() { snapState.on = !snapState.on; saveSnap(); applySnapUi(); toast(snapState.on ? `Snap on: within ${snapState.dist} units${snapState.grid ? `, grid ${snapState.grid}` : ''}, rotation in ${snapState.rot}° steps (Alt inverts)` : 'Snap off'); }
gizmoBar.querySelector('[data-snap]').addEventListener('click', e => { e.stopPropagation(); toggleSnap(); });
$('snapDist').addEventListener('change', e => { snapState.dist = THREE.MathUtils.clamp(+e.target.value || 30, 1, 500); saveSnap(); applySnapUi(); });
$('snapGrid').addEventListener('change', e => { snapState.grid = THREE.MathUtils.clamp(+e.target.value || 0, 0, 1000); saveSnap(); applySnapUi(); });
$('snapRot').addEventListener('change', e => { snapState.rot = +e.target.value || 15; saveSnap(); applySnapUi(); });
addEventListener('keydown', e => { if (e.key === 'Alt' && !snapAlt) { snapAlt = true; applySnapUi(); } });
addEventListener('keyup', e => { if (e.key === 'Alt' && snapAlt) { snapAlt = false; applySnapUi(); } }); addEventListener('blur', () => { if (snapAlt) { snapAlt = false; applySnapUi(); } });
applySnapUi();
const snapClear = () => { clear(snapGroup); const i = $('snapInfo'); if (i) i.textContent = ''; };
// boxes near a point (they do not move during the drag: cached per drag); skip = what is being moved
function nearBoxes(at, skip, withArt) {
  const out = [], v = new THREE.Vector3();
  for (const o of items) { if (o === skip || skip?.has?.(o) || !['geometry', 'native'].includes(o.o.kind)) continue; const n = nodeOf.get(o); if (!n || !n.visible || n.userData.volume) continue;
    if (Math.hypot(...o.at.map((x, i) => x - at[i])) > 4000) continue; n.updateMatrixWorld(true); const b = new THREE.Box3().setFromObject(n); if (!b.isEmpty() && b.getSize(v).length() < 6000) out.push({name: o.name, box: b}); }
  if (withArt) for (const g of art.children) { if (g === skip || !g.visible || g.userData.backdrop || g.userData.shellIn) continue; const b = new THREE.Box3().setFromObject(g); if (b.isEmpty() || b.distanceToPoint(new THREE.Vector3(...at)) > 4000 || b.getSize(v).length() > 6000) continue; out.push({name: g.userData.world, box: b}); }
  return out;
}
const corners = b => [0, 1, 2, 3, 4, 5, 6, 7].map(i => new THREE.Vector3(i & 1 ? b.max.x : b.min.x, i & 2 ? b.max.y : b.min.y, i & 4 ? b.max.z : b.min.z));
const surfaceBelow = (p, d) => { const r = new THREE.Raycaster(p.clone().add(new THREE.Vector3(0, d, 0)), new THREE.Vector3(0, -1, 0), 0, d * 2); return r.intersectObjects(art.children.filter(m => m.visible && !m.userData.backdrop && !m.userData.shellIn), true)[0]?.point || null; };
function snapShow(best, box, move) {   // the contact faces amber, the target boxes faint; the bar text with the distances
  clear(snapGroup); const A = ['x', 'y', 'z'], seen = new Set();
  for (const [a, b] of best.entries()) { if (!b?.c) continue;
    if (!seen.has(b.c)) { seen.add(b.c); snapGroup.add(new THREE.Box3Helper(b.c.box.clone().expandByScalar(.5), 0x8a6a1f)); }
    if (b.kind === 'face') { const k = A[a], f = new THREE.Box3(); const moved = box.clone().translate(move); for (const q of A) { f.min[q] = Math.max(moved.min[q], b.c.box.min[q]); f.max[q] = Math.min(moved.max[q], b.c.box.max[q]); } f.min[k] = f.max[k] = b.delta > 0 ? moved.min[k] : moved.max[k]; f.expandByScalar(.8); snapGroup.add(new THREE.Box3Helper(f, 0xfbbf24)); } }
  $('snapInfo').textContent = '⌖ ' + best.map((b, a) => b ? `${b.kind === 'vertex' ? 'XYZ' : A[a].toUpperCase()}: ${b.kind === 'surface' ? 'surface' : `${b.kind} of ${b.c.name}`} (${Math.abs(b.dist ?? b.delta).toFixed(1)})` : '').filter(Boolean).join(' · ');
}
// the move that makes `box` flush with a neighbour: a corner pair within 60% of the distance wins (corner to corner),
// else per axis the smallest move (faces - needing real overlap on the other two axes - before edges), and the bottom onto
// the surface below when no face join won on Y
function boxMagnet(box, cands, d) {
  const A = ['x', 'y', 'z'], best = [null, null, null]; let vtx = null;
  for (const c of cands) {
    for (const p of corners(box)) for (const q of corners(c.box)) { const l = p.distanceTo(q); if (l <= d * .6 && l > 1e-4 && (!vtx || l < vtx.dist)) vtx = {move: q.clone().sub(p), dist: l, c}; }
    for (let a = 0; a < 3; a++) {
      const k = A[a], o1 = A[(a + 1) % 3], o2 = A[(a + 2) % 3], ov = q => box.min[q] <= c.box.max[q] + d && box.max[q] >= c.box.min[q] - d; if (!ov(o1) || !ov(o2)) continue;
      const touch = q => box.min[q] < c.box.max[q] - .5 && box.max[q] > c.box.min[q] + .5, faceOk = touch(o1) && touch(o2);   // else a box beside another was also stacked on it
      for (const [delta, kind] of [[c.box.max[k] - box.min[k], 'face'], [c.box.min[k] - box.max[k], 'face'], [c.box.min[k] - box.min[k], 'edge'], [c.box.max[k] - box.max[k], 'edge']]) {
        if (Math.abs(delta) > d || Math.abs(delta) < 1e-4 && kind === 'edge' || kind === 'face' && !faceOk) continue; const score = Math.abs(delta) + (kind === 'edge' ? d * .25 : 0);
        if (!best[a] || score < best[a].score) best[a] = {delta, kind, c, score}; }
    }
  }
  if (vtx && !best.some(b => b?.kind === 'face' && Math.abs(b.delta) < vtx.dist)) return {move: vtx.move, best: [{kind: 'vertex', c: vtx.c, delta: vtx.dist, dist: vtx.dist}, null, null]};
  if (!best[1] || best[1].kind !== 'face') { const hit = surfaceBelow(new THREE.Vector3((box.min.x + box.max.x) / 2, box.min.y, (box.min.z + box.max.z) / 2), d); if (hit && Math.abs(hit.y - box.min.y) <= d && Math.abs(hit.y - box.min.y) > 1e-4) best[1] = {delta: hit.y - box.min.y, kind: 'surface', c: null, score: 0}; }
  const move = new THREE.Vector3(best[0]?.delta || 0, best[1]?.delta || 0, best[2]?.delta || 0); return move.lengthSq() > 1e-8 ? {move, best} : null;
}
function pointMagnet(p, cands, d, flat) {   // a waypoint (flat: X / Z only) or a path point: onto a corner or a centre of a nearby piece, else the surface
  let best = null; const v = new THREE.Vector3();
  for (const c of cands) for (const q of [...corners(c.box), c.box.getCenter(v.clone())]) { const dd = flat ? Math.hypot(q.x - p.x, q.z - p.z) : q.distanceTo(p); if (dd <= d && dd > 1e-4 && (!best || dd < best.dist)) best = {to: flat ? new THREE.Vector3(q.x, p.y, q.z) : q.clone(), dist: dd, c, kind: 'corner'}; }
  if (!best && !flat) { const hit = surfaceBelow(p, d); if (hit && Math.abs(hit.y - p.y) > 1e-4) best = {to: hit.clone(), dist: Math.abs(hit.y - p.y), c: null, kind: 'surface'}; }
  return best;
}
function magnet(it, n) {   // a piece (placed object / shape): moves its node and placement
  if (!snapOn() || !n) return null; n.updateMatrixWorld(true); const box = new THREE.Box3().setFromObject(n); if (box.isEmpty()) return null;
  const r = boxMagnet(box, snapCache?.key === it ? snapCache.list : nearBoxes(it.at, it, false), snapState.dist); if (!r) { snapClear(); return null; }
  snapShow(r.best, box, r.move); n.position.add(r.move); it.o.position = n.position.toArray().map(round2); n.updateMatrixWorld(true); return r.best;
}
gizmo.addEventListener('dragging-changed', e => {
  if (e.value && snapOn()) { const t = gizmo.object;
    if (selected && t === nodeOf.get(selected)) snapCache = {key: selected, list: nearBoxes(selected.at, selected, false)};
    else if (!selected && artSel && t === proxy) snapCache = {key: artSel, list: nearBoxes(new THREE.Box3().setFromObject(artSel).getCenter(new THREE.Vector3()).toArray(), artSel, true)};
    else if (selected && t === proxy) snapCache = {key: 'point', list: nearBoxes(proxy.position.toArray(), selected, false)};
    else if (multi.size > 1 && t === proxy) snapCache = {key: 'group', list: nearBoxes(proxy.position.toArray(), new Set(multi), false)}; }
  if (!e.value) { snapCache = null; setTimeout(() => { if (!gizmo.dragging) snapClear(); }, 1200); if (snapOn() && selected) { relocate(selected); renderInspector(); } }
});
gizmo.addEventListener('objectChange', () => {   // after the main handler wrote the move: pull it onto its neighbours
  if (snapNested || !snapOn() || gizmo.mode !== 'translate' || multi.size > 1) return; const t = gizmo.object, d = snapState.dist;
  if (selected && t === nodeOf.get(selected)) { if (magnet(selected, t)) changed(); return; }
  if (t !== proxy) return;
  const rerun = () => { snapNested = true; try { gizmo.dispatchEvent({type: 'objectChange'}); } finally { snapNested = false; } };   // the main handler writes the snapped proxy back
  if (!selected && artSel) {   // a stage art piece
    applyArtEdits(); artSel.updateMatrixWorld(true); const box = new THREE.Box3().setFromObject(artSel); const r = !box.isEmpty() && boxMagnet(box, snapCache?.key === artSel ? snapCache.list : nearBoxes(box.getCenter(new THREE.Vector3()).toArray(), artSel, true), d);
    if (r) { snapShow(r.best, box, r.move); proxy.position.add(r.move); rerun(); } else snapClear(); return;
  }
  if (selected && (wpSel !== null || pathPoint !== null)) {   // a waypoint (X / Z) or a path point
    const b = pointMagnet(proxy.position, snapCache?.key === 'point' ? snapCache.list : nearBoxes(proxy.position.toArray(), selected, false), d, wpSel !== null);
    if (b) { proxy.position.copy(b.to); rerun(); clear(snapGroup); if (b.c) snapGroup.add(new THREE.Box3Helper(b.c.box.clone().expandByScalar(.5), 0xfbbf24)); $('snapInfo').textContent = `⌖ ${b.kind === 'surface' ? 'surface' : `${b.kind} of ${b.c.name}`} (${b.dist.toFixed(1)})`; } else snapClear();
  }
});
function setGizmoMode(m) {
  if (!selected && !artSel) return toast('Select something first', true);
  if (m === 'scale' && multi.size > 1) return toast('A selection of several moves or rotates together; scale them one by one', true);
  gizmo.setMode(m);
  toast({translate: 'Move: drag the arrows', rotate: 'Rotate: drag the rings', scale: artSel && !selected ? 'Scale the piece: drag the cubes' : isDecor(selected.o) || selected.o.kind !== 'native' ? 'Scale: drag the cubes' : 'Scale: shown in the editor only - the game sizes this object from its parameters'}[m]);
}
function updateGizmoBar() {
  const any = !!(selected || artSel);
  for (const b of gizmoBar.querySelectorAll('[data-gm]')) { b.classList.toggle('on', any && gizmo.mode === b.dataset.gm); b.disabled = !any || (b.dataset.gm === 'scale' && multi.size > 1); }
}
gizmo.addEventListener('mode-changed', updateGizmoBar);
gizmo.addEventListener('dragging-changed', e => { if (!e.value && (project?.surfaceCollision || []).length) setTimeout(buildSurfColl, 0); orbit.enabled = !e.value; if (e.value) snapshot(); if (!e.value && !selected && artSel) { renderInspector(); renderIssueBadge(); } if (!e.value && selected) { const it = selected; relocate(it); renderInspector(); drawFlow(); drawTree(); } });
const isDecor = o => o.kind === 'native' && o.list === 0x25 && (o.type === 0x88 || o.type === 0x8A);
gizmo.addEventListener('dragging-changed', e => {   // end of a scale drag: Decoration1 / 258A keep their size in params words 1-3
  if (e.value || gizmo.mode !== 'scale' || !selected) return; const it = selected, o = it.o, t = gizmo.object; if (t === proxy) return;
  const sc = t.scale.toArray().map(v => Math.round(v * 1000) / 1000);
  if (isDecor(o)) { for (let i = 0; i < 3; i++) setParamWord(o, i + 1, Math.round((paramWord(o, i + 1, true) || 1) * sc[i] * 1000) / 1000, true); const n = nodeOf.get(it); if (n) { nodes.remove(n); nodeOf.delete(it); } addNode(it); buildMarkers(); attachGizmo(); renderInspector(); }
  else o.scale = sc;
  changed();
});
gizmo.addEventListener('dragging-changed', e => {   // the whole selection moves / rotates around its centre
  if (multi.size < 2 || gizmo.object !== proxy) return;
  if (e.value) { proxy.updateMatrix(); groupStart = {inv: proxy.matrix.clone().invert(), q0: proxy.quaternion.clone().invert(), members: [...multi].map(it => ({it, pos: new THREE.Vector3(...it.o.position), q: new THREE.Quaternion().setFromEuler(new THREE.Euler(...(it.o.rotation || [0, 0, 0]).map(THREE.MathUtils.degToRad), 'YXZ'))}))}; }
  else { groupStart = null; for (const it of multi) relocate(it); drawFlow(); drawTree(); renderInspector(); }
});
// uniform scale (the centre handle): three's ratio = pointer distance from the centre now / at the press explodes when
// the press is near the centre (a 25 px drag gave 35x on stage art); use 2^(drag right/up / 200 px) instead
let scaleDrag = null, pressAt = {x: 0, y: 0};
renderer.domElement.addEventListener('pointerdown', e => { pressAt = {x: e.clientX, y: e.clientY}; }, true);
renderer.domElement.addEventListener('pointermove', e => { if (scaleDrag) scaleDrag.d = (e.clientX - pressAt.x) - (e.clientY - pressAt.y); }, true);
gizmo.addEventListener('dragging-changed', e => { scaleDrag = e.value && gizmo.mode === 'scale' && gizmo.axis === 'XYZ' && gizmo.object ? {d: 0, s: gizmo.object.scale.clone()} : null; });
gizmo.addEventListener('objectChange', () => {
  if (scaleDrag && gizmo.object) gizmo.object.scale.copy(scaleDrag.s).multiplyScalar(2 ** (scaleDrag.d / 200));
  if (multi.size > 1 && groupStart && gizmo.object === proxy) {
    proxy.updateMatrix(); const delta = proxy.matrix.clone().multiply(groupStart.inv), dq = proxy.quaternion.clone().multiply(groupStart.q0);
    for (const m of groupStart.members) {
      const o = m.it.o; o.position = m.pos.clone().applyMatrix4(delta).toArray().map(round2);
      if (gizmo.mode === 'rotate' && o.kind !== 'goalpose') { const e = new THREE.Euler().setFromQuaternion(dq.clone().multiply(m.q), 'YXZ'); o.rotation = [e.x, e.y, e.z].map(v => round2(THREE.MathUtils.radToDeg(v))); }
      const n = nodeOf.get(m.it); if (n) { n.position.set(...o.position); n.rotation.set(...(o.rotation || [0, 0, 0]).map(THREE.MathUtils.degToRad)); } m.it.at = posOf(o);
    }
    updateMarkers(); changed(); return;
  }
  if (!selected && artSel && gizmo.object === proxy) { artEditFromProxy(artSel); applyArtEdits(); changed(); return; }
  if (!selected) return; const t = gizmo.object, o = selected.o;
  if (arcSel && t === proxy && launcherKind(o)) {   // #160 an arc handle on the gizmo: aim the launcher at it
    const r = aimArc(selected, arcSel, proxy.position.clone()); if (r) { nodeOf.get(selected)?.rotation.set(...o.rotation.map(THREE.MathUtils.degToRad)); drawSpringGuide(true); changed(); } return;
  }
  if (wpSel !== null && t === proxy && enemyKind(o) === 'wp') {   // #92 a waypoint: X, Z from the spot in the enemy's frame
    const w = waypointInfo(o), l = t.position.clone().applyMatrix4(wpFrame(o).invert()), lim = 3000;
    setParamWord(o, w.ix[wpSel * 2], round2(THREE.MathUtils.clamp(l.x, -lim, lim)), true); setParamWord(o, w.ix[wpSel * 2 + 1], round2(THREE.MathUtils.clamp(l.z, -lim, lim)), true); drawEnemyGuide(true); changed(); return;
  }
  if (pathPoint !== null && o.kind === 'path' && t === proxy) {   // a control point: back into the path's local frame
    const n = nodeOf.get(selected); if (!n) return; n.updateMatrixWorld(true); o.points[pathPoint] = n.worldToLocal(t.position.clone()).toArray().map(round2); o.autoOrient = true;
    refreshPath(selected); changed(); return;
  }
  o.position = t.position.toArray().map(v => Math.round(v * 100) / 100);
  if (gizmo.mode === 'rotate') { const e = new THREE.Euler().setFromQuaternion(t.quaternion, 'YXZ'); o.rotation = [e.x, e.y, e.z].map(v => Math.round(THREE.MathUtils.radToDeg(v) * 100) / 100); }
  if (t === proxy) updateMarkers(); else { selRing.children.forEach(c => c.update?.()); const ring = selRing.children.find(c => c.isMesh); ring?.position.set(o.position[0], o.position[1] + 1, o.position[2]); }
  changed();
});
// #128 a selection of several pieces (a tile's parts, a loop kit, an imported model) snaps as ONE box: registered after the
// group-move handler above, so the members have moved; the proxy takes the pull and that handler runs again
gizmo.addEventListener('objectChange', () => {
  if (snapNested || !snapOn() || gizmo.mode !== 'translate' || multi.size < 2 || gizmo.object !== proxy || !groupStart) return;
  const box = new THREE.Box3(); for (const it of multi) { const n = nodeOf.get(it); if (n) { n.updateMatrixWorld(true); box.union(new THREE.Box3().setFromObject(n)); } }
  if (box.isEmpty()) return; const r = boxMagnet(box, snapCache?.key === 'group' ? snapCache.list : nearBoxes(box.getCenter(new THREE.Vector3()).toArray(), new Set(multi), false), snapState.dist);
  if (!r) { snapClear(); return; }
  snapShow(r.best, box, r.move); proxy.position.add(r.move); snapNested = true; try { gizmo.dispatchEvent({type: 'objectChange'}); } finally { snapNested = false; }
});
let groupStart = null;
function attachGizmo() {
  gizmo.detach(); updateGizmoBar();
  if (multi.size > 1) { const c = [0, 1, 2].map(i => [...multi].reduce((a, it) => a + it.at[i], 0) / multi.size); proxy.position.set(...c); proxy.rotation.set(0, 0, 0); proxy.scale.set(1, 1, 1); if (gizmo.mode === 'scale') gizmo.setMode('translate'); gizmo.attach(proxy); return; }
  if (!selected && artSel) { artProxyFromEdit(artSel); gizmo.attach(proxy); return; }
  gizmo.showY = true;
  if (!selected || readOnlyKind(selected.o)) return;
  if (arcSel && launcherKind(selected.o)) { const a = arcOf(selected.o), q = a && (arcSel === 'top' ? a.apex.p : a.end.p);   // #160 an arc handle
    if (q) { proxy.position.set(...q); proxy.rotation.set(0, 0, 0); proxy.scale.set(1, 1, 1); gizmo.setMode('translate'); gizmo.attach(proxy); return; } arcSel = null; }
  if (wpSel !== null && enemyKind(selected.o) === 'wp') { const w = waypointInfo(selected.o); if (w) { proxy.position.copy(new THREE.Vector3(...w.w[wpSel]).applyMatrix4(wpFrame(selected.o))); proxy.rotation.set(0, 0, 0); proxy.scale.set(1, 1, 1); gizmo.setMode('translate'); gizmo.showY = false; gizmo.attach(proxy); return; } }
  const n = nodeOf.get(selected);
  if (pathPoint !== null && selected.o.kind === 'path' && n) { n.updateMatrixWorld(true); proxy.position.copy(n.localToWorld(new THREE.Vector3(...selected.o.points[pathPoint]))); proxy.rotation.set(0, 0, 0); proxy.scale.set(1, 1, 1); gizmo.setMode('translate'); gizmo.attach(proxy); }
  else if (n) gizmo.attach(n); else { proxy.position.set(...selected.o.position); gizmo.attach(proxy); }
}
const undeletable = o => o.kind === 'goalpose' || o.kind === 'stageobject' || (o.kind === 'playerstart' && o.player);   // #68: the player start can be deleted (not player 2's start, nor the finish poses)
function deleteSelected() {
  if (multi.size > 1) {
    const keep = [...multi].filter(it => undeletable(it.o)); if (keep.length) for (const it of keep) multi.delete(it); if (!multi.size) return toast('Finish poses and stage-code objects cannot be deleted: move them instead', true);
    if (keep.length) setTimeout(() => toast(`${keep.length} kept: finish poses and stage-code objects cannot be deleted`), 900);
    snapshot(); const list = [...multi]; startRemoved(list.map(it => it.o));
    for (const it of list) { const o = it.o, arr = listFor(o); const i = arr.indexOf(o); if (i >= 0) arr.splice(i, 1); items.splice(items.indexOf(it), 1); byId.delete(it.id); const n = nodeOf.get(it); if (n) { nodes.remove(n); nodeOf.delete(it); } }
    gizmo.detach(); multi.clear(); selected = null; buildMarkers(); changed(); computeSpine(); renderInspector(); renderPanel(); drawFlow(); return toast(`Deleted ${list.length} placements${list.some(it => isStart(it.o)) ? ' (with the player start: Issues shows how to place one)' : ''}`);
  }
  if (!selected) return; if (selected.o.kind === 'path' && pathPoint !== null) return deletePathPoint(selected, pathPoint);
  if (wpSel !== null && enemyKind(selected.o) === 'wp') return removeWaypoint(selected, wpSel);
  if (selected.o.kind === 'stageobject') return toast(`${selected.name} is driven by the stage's own code: read-only`, true); if (undeletable(selected.o)) return toast(`${selected.name} cannot be deleted: the game always has it. Move it (inspector: Set to camera view / Place at cursor)`, true);
  snapshot(); const it = selected, o = it.o; startRemoved([o]);
  const list = listFor(o); const i = list.indexOf(o); if (i >= 0) list.splice(i, 1);
  items.splice(items.indexOf(it), 1); byId.delete(it.id); const n = nodeOf.get(it); if (n) { nodes.remove(n); nodeOf.delete(it); }
  gizmo.detach(); selected = null; buildMarkers(); changed(); if (isStart(o)) computeSpine(); renderInspector(); renderPanel(); drawFlow(); toast(isStart(o) ? `Deleted ${it.name} (Ctrl Z brings it back) · place one with Add > Player start or right-click > Place start here; until then Build and Playtest are refused` : `Deleted ${it.name}`);
}
function startRemoved(objs) { if (objs.some(o => o.kind === 'playerstart' && !o.player)) (project.playerStart ||= {}).removedOriginal = true; }   // after snapshot(): undo restores the flag with the object
// copies of placements (duplicate, prefab insert): new ids, references inside the copies (targets, logic object
// widgets, graph targets) remapped to the copies, imported game indexes dropped, new paths get new spline ids
function insertObjects(objs) {
  const idMap = new Map(objs.map(o => [o.id, crypto.randomUUID()])); let json = JSON.stringify(objs); for (const [a, b] of idMap) json = json.split(a).join(b);
  const fresh = JSON.parse(json); resetCopiedLinks(fresh, project.objects); json = JSON.stringify(fresh);   // #83 a copy joins no existing group
  const added = [];
  for (const o of JSON.parse(json)) {
    if (['playerstart', 'goalpose', 'spawn', 'stageobject'].includes(o.kind)) continue; delete o.sourceIndex; if (o.kind === 'path') o.splineId = nextSplineId();
    if (o.kind === 'native') { const tails = Object.keys(project.native?.tails || {}); if (tails.length && !tails.includes(o.layer)) o.layer = tails.includes('cmn') ? 'cmn' : tails[0]; }
    listFor(o).push(o);
    const it = makeItem(o); items.push(it); byId.set(it.id, it); addNode(it); added.push(it);
  }
  buildMarkers(); return added;
}
// #66 copy / paste placements: Ctrl C keeps the selection in the prefab shape (positions relative to its centre) in this
// browser (localStorage 'ui2.placementClipboard', every project and stage sees it) and on the system clipboard as
// JSON; Ctrl V puts a copy on the surface under the cursor (else the screen centre) with new ids (insertObjects).
// Natives remember their stage (sourceStage) so a paste into another stage follows the All stages rules.
const CLIP_KEY = 'ui2.placementClipboard'; let cursorAt = null;
renderer.domElement.addEventListener('pointermove', e => { cursorAt = [e.clientX, e.clientY]; });
renderer.domElement.addEventListener('pointerleave', () => { cursorAt = null; });
function copySelected() {
  const list = selection().filter(it => !['playerstart', 'goalpose', 'spawn', 'stageobject'].includes(it.o.kind)); if (!list.length) return toast('Select placements to copy (the player start and finish poses are not copied)', true);
  const c = [0, 1, 2].map(i => list.reduce((a, it) => a + it.o.position[i], 0) / list.length);
  const clip = {kind: 'stage-workshop-placements', version: 1, name: list.length === 1 ? list[0].name : `${list.length} placements`, stage: project.stage, project: project.name, created: new Date().toISOString(),
    objects: list.map(it => ({...structuredClone(it.o), ...(it.o.kind === 'native' ? {sourceStage: it.o.sourceStage ?? project.stage} : {}), position: it.o.position.map((v, i) => round2(v - c[i]))}))};
  const json = JSON.stringify(clip); try { localStorage.setItem(CLIP_KEY, json); } catch { return toast('The selection is too large for the browser clipboard: save it as a prefab instead', true); }
  navigator.clipboard?.writeText(json).catch(() => {});
  toast(`Copied ${list.length} placement${list.length > 1 ? 's' : ''} · Ctrl V pastes at the cursor (any project or stage)`);
}
function pastePlacements() {
  let clip = null; try { clip = JSON.parse(localStorage.getItem(CLIP_KEY) || 'null'); } catch {}
  if (clip?.kind !== 'stage-workshop-placements' || !clip.objects?.length) return toast('Nothing copied yet: select placements and press Ctrl C', true);
  const pt = cursorAt ? surfacePoint(...cursorAt) : centrePoint();
  insertPrefab(clip, pt, `Pasted ${clip.objects.length} placement${clip.objects.length > 1 ? 's' : ''}${clip.project && clip.project !== project.name ? ' from ' + clip.project : ''}`);
}
function duplicateSelected() {
  if (multi.size > 1) { snapshot(); const added = insertObjects([...multi].map(it => ({...structuredClone(it.o), position: it.o.position.map((v, i) => round2(v + [20, 0, 20][i]))}))); changed(); setSelection(added); return toast(`Duplicated ${added.length} placements (+20, 0, +20)`); }
  if (!selected || !['native', 'geometry', 'path'].includes(selected.o.kind)) return; const p = new THREE.Vector3(...selected.o.position).add(new THREE.Vector3(20, 0, 20));
  const src = selected.o; place({name: selected.name, template: src}, p);
  selected.o.rotation = [...(src.rotation || [0, 0, 0])]; nodeOf.get(selected)?.rotation.set(...selected.o.rotation.map(THREE.MathUtils.degToRad));
  if (src.kind !== 'native') nodeOf.get(selected)?.scale.set(...(src.scale || [1, 1, 1]));
}

function arrange(kind, axis) {
  const list = selection(); if (list.length < 2) return toast('Select two or more placements first (Ctrl-click, or Shift-drag a box)', true);
  snapshot(); const i = 'xyz'.indexOf(axis);
  if (kind === 'align') { const v = list[0].o.position[i]; for (const it of list) it.o.position[i] = v; }   // to the first selected
  else { const srt = [...list].sort((a, b) => a.o.position[i] - b.o.position[i]), a = srt[0].o.position[i], b = srt.at(-1).o.position[i]; srt.forEach((it, k) => it.o.position[i] = round2(a + (b - a) * k / (srt.length - 1))); }
  for (const it of list) { nodeOf.get(it)?.position.set(...it.o.position); relocate(it); }
  updateMarkers(); attachGizmo(); changed(); drawFlow(); renderInspector(); toast(`${kind === 'align' ? 'Aligned' : 'Distributed'} ${list.length} on ${axis.toUpperCase()}`);
}
// ---- prefabs: a selection saved with positions relative to its centre, kept by the server (: GET /api/prefabs
// = names, ?name= one prefab, POST save, POST /api/prefabs/delete) in stage_projects/_prefabs; prefabs that only this
// browser or project had (saved before the server had the endpoint) are moved onto it on load. Without the server:
// kept in the project (project.prefabs) and in this browser (localStorage 'ui2.prefabs', so other
// projects see it too)
let prefabsList = null;
const prefabName = n => String(n).replace(/[^A-Za-z0-9 _().,+-]/g, '_').trim().slice(0, 80) || 'Prefab';
async function loadPrefabs() {
  let server = null; try { const list = (await api('/api/prefabs')).prefabs || []; server = (await Promise.all(list.map(e => api('/api/prefabs?name=' + encodeURIComponent(e.name)).then(pf => ({...pf, name: e.name})).catch(() => null)))).filter(pf => pf?.objects?.length); } catch {}
  let local = []; try { local = JSON.parse(localStorage.getItem('ui2.prefabs') || '[]'); } catch {}
  if (server) {
    const have = new Set(server.map(pf => pf.name));
    for (const pf of [...local, ...(project?.prefabs || [])]) { if (!pf?.objects?.length || have.has(prefabName(pf.name))) continue; const body = {...pf, name: prefabName(pf.name)}; try { await api('/api/prefabs', {name: body.name, prefab: body}); server.push(body); have.add(body.name); } catch {} }
    if (local.length && local.every(pf => have.has(prefabName(pf.name)))) try { localStorage.removeItem('ui2.prefabs'); } catch {}
  }
  const all = new Map(); for (const pf of [...(server ? [] : local), ...(server ? [] : project?.prefabs || []), ...(server || [])]) if (pf?.objects?.length) all.set(pf.name, pf); prefabsList = [...all.values()]; prefabsOnServer = !!server; if (panel === 'add') renderPanel();
}
let prefabsOnServer = false;
async function deletePrefab() {
  await loadPrefabs(); if (!prefabsList.length) return toast('No prefabs yet', true);
  const name = await askText('Delete a prefab', 'The prefab is removed from the Add panel. Placed copies stay in your stage.', prefabsList[0].name, prefabsList.map(pf => pf.name)); if (!name) return;
  if (!prefabsList.some(pf => pf.name === name)) return toast(`No prefab "${name}"`, true);
  let gone = false; if (prefabsOnServer) try { await api('/api/prefabs/delete', {name}); gone = true; } catch (e) { toast(e.message, true); }
  if (project?.prefabs?.some(pf => prefabName(pf.name) === name)) { snapshot(); project.prefabs = project.prefabs.filter(pf => prefabName(pf.name) !== name); if (!project.prefabs.length) delete project.prefabs; changed(); gone = true; }
  try { const loc = JSON.parse(localStorage.getItem('ui2.prefabs') || '[]'); if (loc.some(pf => prefabName(pf.name) === name)) { localStorage.setItem('ui2.prefabs', JSON.stringify(loc.filter(pf => prefabName(pf.name) !== name))); gone = true; } } catch {}
  await loadPrefabs(); toast(gone ? `Prefab "${name}" deleted` : `Could not delete "${name}"`, !gone);
}
async function savePrefab(nameIn) {   // a click passes its event: then ask for the name
  const list = selection().filter(it => !['playerstart', 'goalpose', 'spawn'].includes(it.o.kind)); if (!list.length) return toast('Select placements first', true);
  const name = typeof nameIn === 'string' ? nameIn : await askText('Save as prefab', 'The selected placements become one piece in the Add panel (Prefabs), to place again in any stage.', 'My prefab'); if (!name) return;
  const c = [0, 1, 2].map(i => list.reduce((a, it) => a + it.o.position[i], 0) / list.length);
  const pf = {version: 1, name: prefabName(name), created: new Date().toISOString(), stage: project.stage, objects: list.map(it => ({...structuredClone(it.o), position: it.o.position.map((v, i) => round2(v - c[i]))}))};
  let where = 'this project and this browser';
  try { await api('/api/prefabs', {name: pf.name, prefab: pf}); where = 'stage_projects/_prefabs (server)'; }
  catch { snapshot(); project.prefabs = [...(project.prefabs || []).filter(x => x.name !== pf.name), pf]; changed(); try { const loc = JSON.parse(localStorage.getItem('ui2.prefabs') || '[]').filter(x => x.name !== pf.name); loc.push(pf); localStorage.setItem('ui2.prefabs', JSON.stringify(loc)); } catch {} }
  await loadPrefabs(); toast(`Prefab "${pf.name}" saved (${pf.objects.length} placements) in ${where} · Add panel → Prefabs`);
}
function insertPrefab(pf, point, what = '') {
  snapshot(); const at = point.toArray(); const added = insertObjects(pf.objects.map(o => ({...structuredClone(o), position: o.position.map((v, i) => round2(v + at[i]))})));
  changed(); setSelection(added); drawFlow(); toast(`${what || `Inserted prefab "${pf.name}": ${added.length} placements`}${pf.stage !== project.stage ? ` (from ${stageName(pf.stage)}: objects from another stage follow the All stages rules)` : ''}`);
}

// ---- inspector -------------------------------------------------------------------------------------------------------
function select(it, fly = false, add = false) {   // single click selects (Ctrl adds / removes); double-click (or Ctrl K go-to) flies there
  if (it && artSel) { artSel = null; artBox.visible = false; } pathPoint = null; wpSel = null; arcSel = null;
  if (add && it) { if (!multi.size && selected) multi.add(selected); multi.has(it) ? multi.delete(it) : multi.add(it); selected = multi.has(it) ? it : [...multi].at(-1) || null; if (multi.size < 2) multi.clear(); }
  else { multi.clear(); selected = it || null; }
  updateMarkers(); attachGizmo(); renderInspector(); drawTree(); drawFlow(); revealRow(selected);
  if (it && fly) flyTo(it.at);
  if (it && logic.active()) logic.ownerFromSelection(it);
}
// where this placement's model comes from: the in-game capture (#35) or the catalog / artist-table guess
function modelChip(it) {
  const n = nodeOf.get(it), src = n?.userData.modelSource;
  const why = R.drawNotes[`${it.o.list}-${it.o.type}`];
  if (!n) return `<span class="chip warn" title="${esc(why || 'No model found for this object')}: drawn as a category marker">no model</span>`;
  if (why) return `<span class="chip warn" title="${esc(why)}">model fixed</span>`;
  if (src === 'capture') return `<span class="chip ok" title="Models and offsets as the game drew them (in-game capture, ${esc(n.userData.captureHow || '')})">model as in game</span>`;
  return `<span class="chip warn" title="${R.recordModels && Object.keys(R.recordModels).length ? 'The capture has no draw for this record (not activated, or invisible by design): ' : 'No capture loaded: '}model from the ${src === 'artist' ? 'stage artist table' : 'catalog'}, may be wrong">model: best guess</span>`;
}
function modelNamesOf(n) { const out = []; n?.traverse(c => { if (c.userData.modelName) out.push(c.userData.model); }); return out; }
function texturesOf(it) {   // the textures the placement's models draw with: [{texture, asset}] (one per name)
  const seen = new Map(); for (const m of modelNamesOf(nodeOf.get(it))) for (const t of m.materials || []) if (t.texture && !seen.has(t.texture)) seen.set(t.texture, t); return [...seen.values()];
}
function relations(it) {
  const o = it.o, out = [];
  if (o.kind === 'native' && o.link) for (const x of items) if (x !== it && x.o.kind === 'native' && x.o.link === o.link) out.push([x, `shares Link ID ${o.link}`]);
  for (const x of items) {
    const t = x.o; const targets = [t.target, ...(t.actions || []).map(a => a.target), ...((t.graph?.nodes) || []).map(n => n.action?.target)].filter(Boolean);
    if (targets.includes(o.id)) out.push([x, 'targets this']);
    if (x === it) for (const id of new Set(targets)) { const y = byId.get(id); if (y) out.push([y, 'target']); }
  }
  return out;
}
function lightingSection(o) {
  if (o.kind === 'lightregion') return `<details class="sect" open><summary>Atmosphere</summary><div>
    <div class="vec2"><label class="field"><span>Priority</span><input class="num" type="number" step="1" data-lr="priority" value="${o.priority ?? 10}"></label><label class="field"><span>Blend seconds</span><input class="num" type="number" step="any" min="0" max="30" data-lr="transition" value="${o.transition ?? 1.5}"></label></div>
    <label class="field"><span>Bounds follow</span><select class="num" data-lr="follow"><option value="player" ${o.follow !== 'camera' ? 'selected' : ''}>The player (player_bounds: new in L1, runtime pending)</option><option value="camera" ${o.follow === 'camera' ? 'selected' : ''}>The camera (bounds: works in today's runtime)</option></select></label>
    <label class="field"><span>Trigger (optional: only while a node turned it on)</span><input class="num" data-lr="trigger" placeholder="none" value="${esc(o.trigger || '')}"></label>
    ${profileFields(o.profile, 'data-lp')}
    <p class="muted">Empty = the stage's own value. Bounds = this box (position ± size / 2 × scale, along the world axes). The keys are the ones lighting_profiles.ini understands (notes/LIGHTING-PROFILES.md); "the player" writes player_bounds (the game does not read it yet), "the camera" writes bounds. Build turns it into lighting rules - nothing of this is in game yet.</p></div></details>`;
  if (o.kind === 'light') { const hex = '#' + new THREE.Color(...(o.colour || [1, 1, 1]).map(v => Math.min(1, v))).getHexString();
    return `<details class="sect" open><summary>Light</summary><div>
    <label class="field"><span>Colour</span><input type="color" data-ll="colour" value="${hex}"></label>
    <div class="vec2"><label class="field"><span>Intensity</span><input class="num" type="number" step="any" min="0" max="20" data-ll="intensity" value="${o.intensity ?? 2}"></label><label class="field"><span>Radius</span><input class="num" type="number" step="any" min="1" data-ll="radius" value="${o.radius ?? 400}"></label></div>
    <p class="muted">A point light for the lighting engine (Build writes it into the stage's lighting data). The editor previews it with the Lighting preview chip; the game draws nothing from it yet.</p></div></details>`; }
  return '';
}
function wireLighting(el, it) {
  const o = it.o, again = () => { rebuildNode(it); changed(); renderIssueBadge(); lightPreview.key = null; };
  el.querySelectorAll('[data-lr]').forEach(inp => inp.onchange = () => { snapshot(); const k = inp.dataset.lr; o[k] = k === 'follow' ? (inp.value === 'camera' ? 'camera' : 'player') : k === 'trigger' ? inp.value.trim().slice(0, 63) : k === 'transition' ? Math.min(30, Math.max(0, +inp.value || 0)) : Math.round(Math.min(1000, Math.max(-1000, +inp.value || 0))); again(); });
  el.querySelectorAll('[data-lp]').forEach(inp => inp.onchange = () => { snapshot(); o.profile ||= {}; const err = readProfileKey(o.profile, inp.dataset.lp, inp.value); if (err) toast(err, true); again(); renderInspector(); });
  el.querySelectorAll('[data-ll]').forEach(inp => inp.onchange = () => { snapshot(); const k = inp.dataset.ll; if (k === 'colour') { const c = new THREE.Color(inp.value); o.colour = [c.r, c.g, c.b].map(round2); } else o[k] = Math.max(k === 'radius' ? 1 : 0, +inp.value || 0); again(); });
  el.querySelectorAll('[data-vec="size"],[data-vec="scale"]').forEach(inp => inp.addEventListener('change', () => { rebuildNode(it); lightPreview.key = null; }));
}
// Stage settings: lighting profiles = rules without bounds that the Set lighting profile node turns on by trigger name
// ---- the stage lighting card (docs/lighting/MATCHING-THE-ORIGINAL.md section 7, UI2 spec 7f): the stage's generated
// profile (tools/lighting/make_profiles.py, GET /api/atmosphere .generated) pre-fills one row per lighting_profiles.ini
// key; moving / typing a value writes project.lighting.auto.overrides[key], "Reset to auto" deletes it; "Auto off" =
// enabled false. Builds write the generated rule with the overrides under the project's own rules. In-game
// look previews the same values (approximately: the editor's lights are not the runtime's).
const autoState = () => (lightingData().auto ||= {enabled: true, overrides: {}});
const fmtL = v => Array.isArray(v) ? v.map(x => +(+x).toFixed(3)).join(', ') : String(+(+v).toFixed(v < .05 && v > 0 ? 6 : 3));
const swatch = v => `rgb(${v.slice(0, 3).map(x => Math.round(Math.min(1, Math.max(0, x)) * 255)).join(',')})`;
function autoLightCard() {
  if (!project) return '';
  const head = `<h4>Stage lighting <small class="tagNo">made from the stage's own files</small></h4>`;
  if (!atmos || atmosFor !== project.stage) return `<div class="card">${head}<p class="muted">Reading the stage's generated lighting…</p></div>`;
  const g = atmos.generated; if (!g?.rule) return `<div class="card">${head}${lookSection()}<p class="muted">${atmos.error ? 'No /api/atmosphere: ' + esc(atmos.error) : 'No generated profile for this stage yet (tools/lighting/make_profiles.py).'}</p></div>`;
  const a = project.lighting?.auto || {enabled: true, overrides: {}}, ov = a.overrides || {}, on = a.enabled !== false;
  const rows = PROFILE_KEYS.map(([k, label, n, lo, hi]) => {
    const has = Object.prototype.hasOwnProperty.call(ov, k), v = has ? ov[k] : g.rule[k]; if (v === undefined) return '';
    const approx = /azimuth|elevation/.test(k) ? ' <em class="approx" title="Sun direction from the light.bin angles: the angle is not proven in game yet">approximate</em>' : '';
    const step = hi <= .02 ? .00001 : hi <= 3 ? .01 : hi <= 360 ? 1 : 50;
    const ctl = n === 3 ? `<span class="alSw" style="background:${swatch(v)}"></span><input class="num alTxt" data-al="${k}" value="${esc(fmtL(v))}" title="three numbers ${lo}..${hi} (r, g, b)">`
      : `<input type="range" class="alRange" min="${lo}" max="${hi}" step="${step}" data-alr="${k}" value="${v}"><input class="num alNum" data-al="${k}" value="${esc(fmtL(v))}" title="${lo}..${hi}">`;
    return `<div class="alRow ${has ? 'over' : ''}"><span class="alLabel" title="${esc(k)}">${label}${approx}</span>${ctl}${has ? `<button class="ghost small" data-alreset="${k}" title="Back to the generated value: ${esc(fmtL(g.rule[k]))}">Reset to auto</button>` : '<small class="alAuto" title="The generated value">auto</small>'}</div>`;
  }).join('');
  const p = g.pending || {}, pre = g.prelit || {}, cal = g.calibration, nOv = Object.keys(ov).length;
  return `<div class="card alCard ${on ? '' : 'off'}">${head}
    <div class="alTop"><span class="toggle"><span class="switch ${on ? 'on' : ''}" id="alEnabled" title="Auto off: the build writes no generated rule for this stage (the project's own rules and the runtime defaults apply)"></span> Auto ${on ? 'on' : 'off'}</span>${nOv ? `<button class="ghost small" id="alResetAll">Reset all to auto (${nOv})</button>` : ''}</div>
    ${lookSection()}<div class="alRows">${rows}</div>
    <p class="muted">Not used in game yet (shown only): exposure <b>${p.exposure ?? '-'}</b> · AO strength <b>${p.ao_strength ?? '-'}</b>.
      Prelit art: target luminance ${pre.target ?? '-'} · p10 ${pre.p10 ?? '-'} · ${g.regions ?? 0} per-chunk fog rules · calibration: ${cal ? `exposure ${cal.exposure ?? '-'}, sky x ${cal.sky_strength_scale ?? '-'}, sun x ${cal.sun_strength_scale ?? '-'} (${cal.poses ?? '?'} poses)` : 'not run yet'} · made ${esc(g.made || '?')}.
      Turn on <b>In-game look</b> to preview these values here (approximately).</p></div>`;
}
// ---- the stage's Look (the build, server.py generated_lighting + make_profiles.py overlay_looks): the default is the
// game's own (vanilla) lighting with the real-time layer on top (user decision 2026-10-18). project.lighting.look = the
// preset NAME ('stylized' (the generator's default) / 'original' / 'dramatic'; absent = the stage's own default); the
// preset's values come from the stage's generated looks (atmos.generated.looks); a moved slider or a tickbox writes
// project.lighting.auto.overrides[key] with the build's key names (exposure, shadow_strength, shadow_tint, sky_strength =
// Ambient, saturation); Reset deletes the override. Replace / Vanilla blob (circle) shadows / World shading only / Characters
// cast shadows are the LAUNCHER's settings for every stage (settings_ui.h light_replace / light_blob / light_world_only /
// light_char_cast; the runtime reads the stage file's world_shading_only / characters_cast_shadows but ignores them,
// lighting_environment.cpp): here they are preview-only (this browser), with the launcher's defaults.
const LOOK_PRESETS = {stylized: {label: 'Stylized', exposure: .6, shadow_strength: .45, shadow_tint: [.68, .68, .85], sky_strength: 1, saturation: 1.1, world_shading_only: 1, characters_cast_shadows: 0},
  original: {label: 'Original match', exposure: .8, shadow_strength: .6, shadow_tint: [1, 1, 1], sky_strength: 1, saturation: 1, world_shading_only: 1, characters_cast_shadows: 0},
  dramatic: {label: 'Dramatic', exposure: .75, shadow_strength: .75, shadow_tint: [.6, .55, .75], sky_strength: 1, saturation: 1.2, world_shading_only: 1, characters_cast_shadows: 0}};   // fallbacks when the stage has no generated looks
const LOOK_KEYS = [['exposure', 'Sun shading (exposure)', 1, .05, 4], ['shadow_strength', 'Shadow strength', 1, 0, 1], ['shadow_tint', 'Shadow colour (r, g, b)', 3, 0, 3], ['sky_strength', 'Ambient fill', 1, 0, 3], ['saturation', 'Saturation', 1, 0, 2]];   // the launcher's labels (runtime/launcher.cpp Look card)
const lookPresetName = () => { const l = project?.lighting?.look; return typeof l === 'string' && LOOK_PRESETS[l] ? l : atmos?.generated?.lookDefault || 'stylized'; };
function lookBase(name) { const g = atmos?.generated?.looks?.[name]; return {...LOOK_PRESETS[name], ...(g || {}), label: LOOK_PRESETS[name].label, generated: !!g}; }
const LAUNCH_DEFAULT = {replace: false, blob: false, world: true, cast: true};   // runtime/settings_ui.h defaults
let launchPrev = {...LAUNCH_DEFAULT}; try { launchPrev = {...LAUNCH_DEFAULT, ...JSON.parse(localStorage.getItem('ui2.launcherPreview') || '{}')}; } catch {}
const saveLaunchPrev = () => { try { localStorage.setItem('ui2.launcherPreview', JSON.stringify(launchPrev)); } catch {} };
function lookOf() {   // the effective look (what the build writes) + the launcher preview tickboxes, with the editor names the preview uses
  const name = lookPresetName(), b = lookBase(name), ov = project?.lighting?.auto?.overrides || {}, o = launchPrev, v = k => ov[k] ?? b[k];
  return {preset: name, generated: b.generated, exposure: v('exposure'), shadow_strength: v('shadow_strength'), shadow_tint: v('shadow_tint'), ambient: v('sky_strength'), sky_strength: v('sky_strength'), saturation: v('saturation'),
    sun_strength: v('sun_strength'), world_only: !!o.world, characters_cast_shadows: !!o.cast, replace: !!o.replace, blob_shadows: !!o.blob};
}
function normalizeLook(p) {   // the first version of this card stored {preset, keys...} in lighting.look: the build wants the name
  const l = p?.lighting?.look; if (!l || typeof l === 'string') return; const ov = ((p.lighting.auto ||= {enabled: true, overrides: {}}).overrides ||= {});
  for (const k of ['exposure', 'shadow_strength', 'shadow_tint', 'saturation']) if (l[k] !== undefined) ov[k] = l[k];
  if (l.ambient !== undefined) ov.sky_strength = l.ambient; if (l.world_only === false) ov.world_shading_only = 0; if (l.characters_cast_shadows) ov.characters_cast_shadows = 1;
  if (l.replace) (p.lighting.options ||= {}).replace = true; p.lighting.look = LOOK_PRESETS[l.preset] ? l.preset : undefined; if (!p.lighting.look) delete p.lighting.look;
}
// what the generator picked for this stage (make_profiles.py overlay_looks): the chosen look's values, the current value
// (an override marked) and 'Reset all to auto' (the stage default look, no Look overrides)
const AUTO_SHOW = [['sun_strength', 'Sun'], ['sun_tint', 'Sun tint'], ['exposure', 'Sun shading (exposure)'], ['shadow_strength', 'Shadow strength'], ['shadow_tint', 'Shadow colour'], ['sky_strength', 'Ambient fill'], ['saturation', 'Saturation']];
function autoValuesReadout(preset) {
  const g = atmos?.generated, lk = g?.looks?.[preset]; if (!lk) return '';
  const ov = project?.lighting?.auto?.overrides || {}, n = AUTO_SHOW.filter(([k]) => ov[k] !== undefined).length + (project?.lighting?.look ? 1 : 0);
  return `<details class="autoVals"><summary>Auto values · ${esc(LOOK_PRESETS[preset]?.label || preset)} for this stage (generated ${esc(g.made || '?')})${n ? ` · ${n} changed` : ''}</summary>
    <div class="avGrid">${AUTO_SHOW.map(([k, l]) => { const a = lk[k], c = ov[k]; return `<span>${l}</span><b>${esc(fmtL(a ?? '-'))}</b><i class="${c !== undefined ? 'over' : ''}">${c !== undefined ? esc(fmtL(c)) : ''}</i>`; }).join('')}</div>
    ${n ? '<button class="ghost small" id="lookAutoAll">Reset all to auto</button>' : ''}</details>`;
}
function lookSection() {
  const ov = project?.lighting?.auto?.overrides || {}, v = lookOf(), b = lookBase(v.preset);
  const rows = LOOK_KEYS.map(([k, label, n, lo, hi]) => { const has = ov[k] !== undefined, val = ov[k] ?? b[k];
    const ctl = n === 3 ? `<span class="alSw" style="background:${swatch(val)}"></span><input class="num alTxt" data-lk="${k}" value="${esc(fmtL(val))}" title="three numbers ${lo}..${hi}">` : `<input type="range" class="alRange" min="${lo}" max="${hi}" step=".01" data-lkr="${k}" value="${val}"><input class="num alNum" data-lk="${k}" value="${esc(fmtL(val))}" title="${lo}..${hi}">`;
    return `<div class="alRow ${has ? 'over' : ''}"><span class="alLabel">${label}</span>${ctl}${has ? `<button class="ghost small" data-lkreset="${k}" title="Back to the ${b.label} value: ${esc(fmtL(b[k]))}">Reset to preset</button>` : `<small class="alAuto">${esc(b.label)}</small>`}</div>`; }).join('');
  const tick = (id, key, label, help) => `<label class="check"><input type="checkbox" id="${id}" ${launchPrev[key] ? 'checked' : ''}> ${label}${launchPrev[key] !== LAUNCH_DEFAULT[key] ? ' <small class="tagNo">not the launcher default</small>' : ''}<small class="muted">${help}</small></label>`;
  return `<div class="lookBox"><div class="alTop"><b>Look</b><select class="num" id="lookPreset">${Object.entries(LOOK_PRESETS).map(([k, x]) => `<option value="${k}" ${v.preset === k ? 'selected' : ''}>${x.label}${k === (atmos?.generated?.lookDefault || 'stylized') ? ' (default)' : ''}</option>`).join('')}</select>${project?.lighting?.look || LOOK_KEYS.some(([k]) => ov[k] !== undefined) ? '<button class="ghost small" id="lookReset">Reset look</button>' : ''}</div>
    <p class="muted">${b.generated ? "Values generated for this stage (its light.bin), tuning the real-time layer." : 'This stage has no generated looks yet: editor defaults shown.'}</p>
    <div class="alRows">${rows}</div>
    ${autoValuesReadout(v.preset)}
    <p class="muted">In game the launcher's own Look (Display > Sun and sky; default Stylized = these values as written) and its % sliders (100 = as written) apply on top of this.</p>
    <div class="launchBox"><b>Launcher settings</b> <small class="muted">the same for every stage (Launcher > Display > Sun and sky); here they only change the In-game look preview. Ticked as the launcher's defaults.</small>
    ${tick('lookReplace', 'replace', "Replace the game's lighting and bloom", "off (default) = the game's own lighting and bloom, with real-time shadows and shading on top; on = only the new lighting")}
    ${tick('lookWorld', 'world', 'World shading only', "characters, partners, NPCs and enemies keep the game's own shading (they do not receive the new shadows)")}
    ${tick('lookCast', 'cast', 'Characters cast shadows', 'Shadow, partners, NPCs and enemies cast real-time shadows onto the stage')}
    ${tick('lookBlob', 'blob', 'Vanilla blob (circle) shadows', "the game's circles under characters; off by default while real-time shadows are on")}</div>
    <p class="muted">In-game look previews the look's sun, sun shading, ambient fill, shadow colour / strength and saturation (approximately).</p></div>`;
}
function wireLook(root) {
  const ovs = () => ((autoState().overrides ||= {})), opts = () => (lightingData().options ||= {}), redraw = () => { const el = $('autoLight'); if (el) { el.innerHTML = autoLightCard(); wireAutoLight(el); } lightPreview.key = null; shadowCount = -2; };
  root.querySelector('#lookPreset')?.addEventListener('change', e => { snapshot(); lightingData().look = e.target.value; changed(); redraw(); });
  root.querySelector('#lookReset')?.addEventListener('click', () => { snapshot(); delete lightingData().look; for (const [k] of LOOK_KEYS) delete ovs()[k]; changed(); redraw(); toast('Look: back to the stage default'); });
  root.querySelectorAll('[data-lkreset]').forEach(b => b.onclick = e => { e.preventDefault(); snapshot(); delete ovs()[b.dataset.lkreset]; changed(); redraw(); });
  root.querySelectorAll('[data-lkr]').forEach(r => {
    r.addEventListener('input', () => { if (!r.dataset.snap) { snapshot(); r.dataset.snap = 1; } ovs()[r.dataset.lkr] = +r.value; const num = r.parentNode.querySelector('.alNum'); if (num) num.value = fmtL(+r.value); r.closest('.alRow').classList.add('over'); changed(); lightPreview.key = null; });
    r.addEventListener('change', () => { delete r.dataset.snap; redraw(); });
  });
  root.querySelectorAll('[data-lk]').forEach(inp => inp.onchange = () => { const k = inp.dataset.lk, [, label, n, lo, hi] = LOOK_KEYS.find(x => x[0] === k), nums = inp.value.split(/[\s,;]+/).filter(Boolean).map(Number);
    if (nums.length !== n || nums.some(x => !Number.isFinite(x))) { toast(`${label}: ${n === 3 ? 'three numbers' : 'a number'} ${lo}..${hi}`, true); return redraw(); }
    snapshot(); const c = nums.map(x => Math.min(hi, Math.max(lo, x))); ovs()[k] = n === 3 ? c : c[0]; changed(); redraw(); });
  for (const [id, key] of [['lookReplace', 'replace'], ['lookWorld', 'world'], ['lookCast', 'cast'], ['lookBlob', 'blob']]) root.querySelector('#' + id)?.addEventListener('change', e => { launchPrev[key] = e.target.checked; saveLaunchPrev(); redraw(); });   // preview only: not saved in the project, no undo
  root.querySelector('#lookAutoAll')?.addEventListener('click', () => { snapshot(); delete lightingData().look; for (const [k] of AUTO_SHOW) delete ovs()[k]; changed(); redraw(); toast('Lighting: back to the generated values for this stage'); });
}
function readAutoValue(k, text) {   // -> [value] or [undefined, error]
  const [, label, n, lo, hi] = PROFILE_KEYS.find(x => x[0] === k), nums = String(text).split(/[\s,;]+/).filter(Boolean).map(Number);
  if (nums.length !== n || nums.some(x => !Number.isFinite(x))) return [undefined, `${label}: ${n === 3 ? 'three numbers (r, g, b)' : 'a number'} ${lo}..${hi}`];
  const c = nums.map(x => Math.min(hi, Math.max(lo, x))); return [n === 3 ? c : c[0]];
}
function wireAutoLight(root) {
  wireLook(root);
  const redraw = () => { const el = $('autoLight'); if (el) { el.innerHTML = autoLightCard(); wireAutoLight(el); } lightPreview.key = null; };
  root.querySelector('#alEnabled')?.addEventListener('click', () => { snapshot(); const a = autoState(); a.enabled = a.enabled === false; changed(); redraw(); });
  root.querySelector('#alResetAll')?.addEventListener('click', () => { snapshot(); autoState().overrides = {}; changed(); redraw(); toast('Stage lighting: back to the generated values'); });
  root.querySelectorAll('[data-alreset]').forEach(b => b.onclick = () => { snapshot(); delete autoState().overrides[b.dataset.alreset]; changed(); redraw(); });
  root.querySelectorAll('[data-alr]').forEach(r => {   // a slider: one undo step per drag
    r.addEventListener('input', () => { if (!r.dataset.snap) { snapshot(); r.dataset.snap = 1; } const a = autoState(); (a.overrides ||= {})[r.dataset.alr] = +r.value; const num = r.parentNode.querySelector('.alNum'); if (num) num.value = fmtL(+r.value); r.closest('.alRow').classList.add('over'); changed(); lightPreview.key = null; });
    r.addEventListener('change', () => { delete r.dataset.snap; redraw(); });
  });
  root.querySelectorAll('[data-al]').forEach(inp => inp.onchange = () => { const [v, err] = readAutoValue(inp.dataset.al, inp.value); if (err) { toast(err, true); return redraw(); } snapshot(); (autoState().overrides ||= {})[inp.dataset.al] = v; changed(); redraw(); });
}
// ---- Stage files (UI2 spec 7c, build): the game picks the fog, light, start / area camera and music files by the
// stage number, so the build can write another stage's file under this slot's name. project.stageFiles = {fog, light,
// cam (stage | 'none'), music (stage | {asset})}; absent = this stage's own. Preview: In-game look uses the chosen fog /
// light (GET /api/atmosphere of that stage); music shows the file names. Clear colour: set in code, not supported.
let atmosOwn = null; const atmosCache = new Map();
const atmosOf = st => { if (!atmosCache.has(st)) atmosCache.set(st, api('/api/atmosphere?stage=' + st).catch(e => ({error: e.message}))); return atmosCache.get(st); };
const MUSIC_EXCEPTIONS = {411: 'sng_stg0310.adx', 412: 'sng_stg0210.adx', 511: 'sng_stg0310.adx', 613: 'sng_stg0611.adx', 614: 'sng_stg0612.adx', 615: 'sng_stg0612.adx', 616: 'sng_stg0610.adx', 617: 'sng_stg0610.adx', 618: 'sng_stg0611.adx', 800: 'sng_stg0800.adx', 801: 'sng_stg0800.adx', 802: 'sng_stg0800.adx'};
const musicFile = st => MUSIC_EXCEPTIONS[st] || `sng_stg${String(st).padStart(4, '0')}.adx`;   // server.py music_file (the game's table)
const SF_KINDS = [['fog', 'Fog', 'Distance fade: the colour and the start / end distances per chunk group (stgNNNN.fog).', '.fog'],
  ['light', 'Light', "The stage's sun and ambient colours and their angles (stgNNNN_light.bin); the lighting card's generated values come from this stage's own file.", '_light.bin'],
  ['cam', 'Camera', "Start / area camera regions (stgNNNN_cam.dat): fixed or scripted cameras inside boxes. A new stage keeps its slot's regions unless set here, which can lock the camera; 'No camera regions' = the follow camera everywhere.", '_cam.dat']];   // music: its own card (#100 musicCard)
const sfPreview = {};   // kind -> preview text (filled from /api/atmosphere of the chosen stage)
const stageOpts = () => [...config.stages].sort((a, b) => (/^stg\d+$/i.test(a.name) ? 1 : 0) - (/^stg\d+$/i.test(b.name) ? 1 : 0) || a.id - b.id);
function sfWrites(kind, v) {   // what the build writes, in words
  const own = `stg${String(project.stage).padStart(4, '0')}`;
  if (kind === 'music') return v == null ? `nothing (${musicFile(project.stage)} stays the game's)` : typeof v === 'object' ? `${musicFile(project.stage)} = ${(project.customAssets || []).find(a => [a.id, a.file].includes(v.asset))?.name || v.asset}` : `${musicFile(project.stage)} = ${musicFile(v)} (${stageName(v)})`;
  const suf = SF_KINDS.find(k => k[0] === kind)[3];
  if (kind === 'cam' && (v === 'none' || (v == null && project.mode === 'new'))) return `${own}${suf} with every region moved out of reach${v == null ? ' (the default for a new stage)' : ''}`;
  return v == null ? `nothing (this stage's own ${own}${suf})` : `${own}${suf} = stg${String(v).padStart(4, '0')}${suf} (${stageName(v)})`;
}
function stageFilesCard() {
  if (!project) return ''; const sf = project.stageFiles || {}, nSet = Object.keys(sf).filter(k => sf[k] != null).length;
  const rows = SF_KINDS.map(([k, label, desc]) => {
    const v = sf[k], has = v != null, opts = stageOpts().filter(st => st.id !== project.stage);
    let sel = `<option value="">This stage's own${k === 'cam' && project.mode === 'new' ? ' (new stage: none)' : ''}</option>`;
    if (k === 'cam') sel += `<option value="none" ${v === 'none' ? 'selected' : ''}>No camera regions (follow camera everywhere)</option>`;
    if (k === 'music') { const adx = (project.customAssets || []).filter(a => a.kind === 'music'); if (adx.length) sel += `<optgroup label="Imported into this project">${adx.map(a => { const ok = a.format === 'adx' || String(a.file || '').endsWith('.adx'); return `<option value="asset:${esc(a.id)}" ${typeof v === 'object' && v?.asset === a.id ? 'selected' : ''} ${ok ? '' : 'disabled'}>${esc(a.name)}${ok ? '' : ` (${esc(a.format || '?')}: the game plays ADX only)`}</option>`; }).join('')}</optgroup>`; }
    sel += `<optgroup label="Another stage's">${opts.map(st => `<option value="${st.id}" ${v === st.id ? 'selected' : ''}>${esc(st.name)} (${st.id})${k === 'music' ? ' · ' + musicFile(st.id) : ''}</option>`).join('')}</optgroup>`;
    return `<div class="sfRow ${has ? 'over' : ''}"><div class="sfHead"><b>${label}</b><select class="num" data-sf="${k}">${sel}</select>${has ? `<button class="ghost small" data-sfreset="${k}" title="Back to this stage's own file">Reset</button>` : ''}</div>
      <small class="sfDesc">${esc(desc)}</small><small class="sfWrites">Build writes: <code>${esc(sfWrites(k, v))}</code></small>${sfPreview[k] ? `<small class="sfPrev">${sfPreview[k]}</small>` : ''}</div>`;
  }).join('');
  return `<div class="card sfCard"><h4>Fog, light &amp; camera files <small class="tagOk">works in game</small></h4><p class="muted">Borrow another stage's fog, sun and camera set-up.</p>
    <div class="sfRows">${rows}<div class="sfRow off"><div class="sfHead"><b>Clear colour</b><span class="tagNo">not supported</span></div><small class="sfDesc">The background colour behind everything: the game sets it in code, so no file can change it (the build ignores it).</small></div></div>
    ${nSet ? `<div class="wsBtns"><button class="ghost small" id="sfResetAll">Reset all to this stage's own (${nSet})</button></div>` : ''}<p class="muted">Turn on <b>In-game look</b> to preview the chosen fog and light here.</p></div>`;
}
function setStageFile(k, v) {
  snapshot(); const sf = {...(project.stageFiles || {})}; if (v == null || v === project.stage) delete sf[k]; else sf[k] = v;
  if (Object.keys(sf).length) project.stageFiles = sf; else delete project.stageFiles;
  changed(); applyStageFilesAtmos(); loadStageFilesPreview(); refreshStageFiles(); renderIssueBadge();
}
function refreshStageFiles() { const el = $('stageFiles'); if (el) { el.innerHTML = stageFilesCard(); wireStageFiles(el); } }
function wireStageFiles(root) {
  root.querySelectorAll('[data-sf]').forEach(sel => sel.onchange = () => { const k = sel.dataset.sf, raw = sel.value; setStageFile(k, raw === '' ? null : raw === 'none' ? 'none' : raw.startsWith('asset:') ? {asset: raw.slice(6)} : +raw); });
  root.querySelectorAll('[data-sfreset]').forEach(b => b.onclick = () => setStageFile(b.dataset.sfreset, null));
  root.querySelector('#sfResetAll')?.addEventListener('click', () => { snapshot(); delete project.stageFiles; changed(); applyStageFilesAtmos(); loadStageFilesPreview(); refreshStageFiles(); renderIssueBadge(); });
}
async function loadStageFilesPreview() {
  if (!project) return; const sf = project.stageFiles || {};
  for (const k of ['fog', 'light']) {
    const st = typeof sf[k] === 'number' ? sf[k] : project.stage, a = await atmosOf(st), who = st === project.stage ? 'this stage' : stageName(st);
    if (k === 'fog') { const es = (a?.fog?.entries || []).filter(e => e.enabled); sfPreview.fog = a?.error ? `No fog data: ${esc(a.error)}` : !a?.fog ? `<span class="badText">${esc(who)} has no fog file</span>` : `${esc(who)}: ${es.slice(0, 4).map(e => `<span class="alSw inl" style="background:${esc(e.colourHex)}"></span>${Math.round(e.start)}..${Math.round(e.end)}`).join(' · ')}${es.length > 4 ? ` · +${es.length - 4}` : ''}`; }
    else { const m = a?.light?.main; sfPreview.light = a?.error ? `No light data: ${esc(a.error)}` : !m ? `<span class="badText">${esc(who)} has no light file</span>` : `${esc(who)}: sun <span class="alSw inl" style="background:${swatch(m.colour || [1, 1, 1])}"></span> ambient <span class="alSw inl" style="background:${swatch((m.ambient || [.3, .3, .3]).map(x => Math.min(1, x * 3)))}"></span> angles ${(m.angles || []).map(Math.round).join(' / ')}`; }
    stageFilesMissing[k] = typeof sf[k] === 'number' && !a?.error && (k === 'fog' ? !a?.fog : !a?.light?.main) ? sf[k] : null;
  }
  const mu = sf.music; sfPreview.music = mu == null ? `plays ${musicFile(project.stage)} (this stage's)` : typeof mu === 'object' ? 'an imported file (name only: no player here)' : `plays ${musicFile(mu)} (${esc(stageName(mu))}) - name only, no player here`;
  sfPreview.cam = sf.cam === 'none' || (sf.cam == null && project.mode === 'new') ? 'the normal follow camera everywhere (no fixed / scripted camera boxes)' : typeof sf.cam === 'number' ? `${esc(stageName(sf.cam))}'s camera boxes (made for its layout: check them in a playtest)` : "this stage's own camera boxes";
  refreshStageFiles(); renderIssueBadge(); if (panel === 'issues') renderPanel();
}
const stageFilesMissing = {};
async function applyStageFilesAtmos() {   // In-game look previews the chosen fog / light
  if (!atmosOwn || atmosOwn.error) return; const sf = project?.stageFiles || {}, a = {...atmosOwn};
  if (typeof sf.fog === 'number') { const o = await atmosOf(sf.fog); if (o?.fog) a.fog = o.fog; }
  if (typeof sf.light === 'number') { const o = await atmosOf(sf.light); if (o?.light) a.light = o.light; }
  atmos = a; lightPreview.key = null;
}
function stageFilesIssues() {   // [title, text] of what the build would refuse or what is odd
  const sf = project?.stageFiles || {}, out = [];
  for (const k of ['fog', 'light']) if (stageFilesMissing[k] != null) out.push([`Stage files: ${stageName(stageFilesMissing[k])} has no ${k} file`, `The build is refused (stageFiles.${k}). Pick another stage or Reset to this stage's own.`, true]);
  if (typeof sf.music === 'object' && sf.music) { const a = (project.customAssets || []).find(x => [x.id, x.file].includes(sf.music.asset));
    if (!a) out.push(['Music: the chosen file is gone', 'The song it points to is not in this project any more: the build is refused. Pick another in Stage → Music.', true]);
    else if (!['adx', 'wav', 'mp3', 'm4a', 'aac', 'flac', 'wma', 'ogg'].includes(String(a.format || '').toLowerCase()) && !String(a.file || '').endsWith('.adx')) out.push([`Music: ${a.name} is ${String(a.format || '?').toUpperCase()}`, 'The editor converts WAV, MP3, M4A, FLAC, WMA and OGG songs to the game format: export the song as one of those.', true]); }
  if (sf.clearColour != null) out.push(['Stage files: clear colour is not supported', 'The game sets the background colour in code; the build ignores it.', false]);
  if (typeof sf.cam === 'number') out.push([`Stage files: camera boxes of ${stageName(sf.cam)}`, "They were made for that stage's layout; in this one they may hold the camera in odd places. Check in a playtest, or choose 'No camera regions'.", false]);
  return out;
}
// ---- Draw distance (user: 'very low pop-in for objects and enemies'; the build, spec 7j): project.activationScale =
// 'wider' (the 'Wider draw distance' preset: 2x, rings / items / decor / breakables 3x, triggers / checkpoints / goal /
// spawners / mission objects as the game has them) or a number 1..3 ('Custom'; 3x is the cap - more measured 8-21 fps);
// absent = the game's own. Each record's activation byte x 100 units x the scale = how far from the player it is created.
// Preview: objects / enemies active around the camera now (the camera stands in for the player), at 1x and with the setting.
const actSetting = () => project?.activationScale ?? null;
const WIDER_VANILLA = /Trigger|Checkpoint|Save Point|SavePoint|Goal|Generator|Mission|Hint|Talking|Cage|Key|Chaos Emerald/i;
function actScaleOf(it, set) {
  if (set == null) return 1; if (typeof set === 'number') return set;
  if (set === 'wider') return WIDER_VANILLA.test(it.cls || '') ? 1 : ['pickups', 'decor'].includes(it.cat) ? 3 : 2; return 1;
}
function activeAt(set) {
  const p = camera.position; let n = 0, e = 0;
  for (const it of items) { const o = it.o; if (o.kind !== 'native' || !o.render) continue; if (Math.hypot(p.x - it.at[0], p.y - it.at[1], p.z - it.at[2]) <= Math.min(255, o.render * actScaleOf(it, set)) * 100) { n++; if (it.cat === 'enemies') e++; } }
  return [n, e];
}
const ddLabel = set => set == null ? '100%' : set === 'wider' ? 'Wider' : `${Math.round(set * 100)}%`;   // shown in % like the launcher's Object draw distance (100..300); stored as the scale 1..3
const ddPrevText = () => { const set = actSetting(), [n1, e1] = activeAt(null), [n2, e2] = activeAt(set); return `At the camera now: <b>${n1}</b> objects (${e1} enemies) active as the game has them${set != null ? `, <b>${n2}</b> (${e2} enemies) with ${ddLabel(set)}` : ''}.`; };
function drawDistCard() {
  if (!project) return ''; const set = actSetting(), custom = typeof set === 'number', log = (lastReport?.warnings || []).find(w => /^Activation distance/.test(w));
  return `<div class="card ddCard"><h4>Draw distance <small class="tagNo">being tested</small></h4>
    <label class="check"><input type="checkbox" id="ddWider" ${set === 'wider' ? 'checked' : ''}> Wider draw distance <small class="muted">200%; rings, items, decorations and breakables 300%; triggers, checkpoints, the goal, spawners and mission objects as the game has them</small></label>
    <label class="check"><input type="checkbox" id="ddCustom" ${custom ? 'checked' : ''}> Custom</label>
    ${custom ? `<div class="alRow over"><span class="alLabel">Object draw distance %</span><input type="range" class="alRange" id="ddRange" min="100" max="300" step="25" value="${Math.round(set * 100)}"><input class="num alNum" id="ddNum" value="${Math.round(set * 100)}"><small class="alAuto">100..300%</small></div>` : ''}
    <p class="muted">How far from the player objects and enemies come to life. Wider = less pop-in, but more alive at once (frame rate, memory), enemies that notice you earlier and triggers / spawners that may fire early: our tests show how far is safe.</p>
    <p class="ddPrev" id="ddPrev">${ddPrevText()}</p>${log ? `<p class="muted">Last build: ${esc(log)}</p>` : ''}</div>`;
}
function wireDrawDist(root) {
  const redraw = () => { const el = $('drawDist'); if (el) { el.innerHTML = drawDistCard(); wireDrawDist(el); } };
  const put = v => { if (v == null) delete project.activationScale; else project.activationScale = v; changed(); };
  root.querySelector('#ddWider')?.addEventListener('change', e => { snapshot(); put(e.target.checked ? 'wider' : null); redraw(); });
  root.querySelector('#ddCustom')?.addEventListener('change', e => { snapshot(); put(e.target.checked ? 1.5 : null); redraw(); });
  const r = root.querySelector('#ddRange'); if (!r) return;
  r.addEventListener('input', () => { if (!r.dataset.snap) { snapshot(); r.dataset.snap = 1; } project.activationScale = Math.round(THREE.MathUtils.clamp(+r.value, 100, 300)) / 100; changed(); root.querySelector('#ddNum').value = Math.round(project.activationScale * 100); root.querySelector('#ddPrev').innerHTML = ddPrevText(); });
  r.addEventListener('change', () => { delete r.dataset.snap; redraw(); });
  root.querySelector('#ddNum').onchange = e => { snapshot(); put(Math.round(THREE.MathUtils.clamp(+e.target.value || 100, 100, 300)) / 100); redraw(); };
}
setInterval(() => { if (panel === 'settings' && project && $('ddPrev') && !document.querySelector('#ddRange')?.dataset.snap) $('ddPrev').innerHTML = ddPrevText(); }, 1000);   // follows the camera
// ---- B3 custom stage (the custom_stage.py, docs/custom-stages): what the launcher's Custom stages list and the
// .shadowstage package show (project.customStage = {description, author, cover: uploaded image asset id, goals:
// {neutral|dark|hero: {type: goal|defeat|rings|timer, count | seconds}}}), Validate (GET /api/custom-stage/validate),
// Export (GET /api/package). Native today in slot 100: Neutral = goal ring, Dark = defeat GUN, Hero = defeat Black Arms;
// other goals are written for the runtime and marked 'needs the runtime'.
const CS_ROUTES = [['neutral', 'Neutral', 'goal'], ['dark', 'Dark', 'defeat'], ['hero', 'Hero', 'defeat']];
const CS_TYPES = [['', 'No goal'], ['goal', 'Goal ring'], ['defeat', 'Defeat'], ['rings', 'Rings'], ['timer', 'Time limit']];
const CS_GUN = /^(GUNSoldier|GUNBeetle|GUN ?Robot|GUN ?Bigfoot)$/, CS_BLACK = /^(Bk |Brown Cocoon|Black Arms Mine)/;
let csCheck = null;   // the last Validate result (this project)
function csEnemies(route) { const re = route === 'dark' ? CS_GUN : CS_BLACK; return project.objects.filter(o => o.kind === 'native' && re.test(o.nativeName || o.name || '')).length; }
// ---- Stage panel cards of #100-#103: music, sky, voice / hint lines, rules
// #101 Rules: only rules proven in game are offered. project.stageRules = {timeLimit, startRings, startLives} -> the build
// writes mods/stage_rules.json for the runtime (ask in AGENTS-LIVE, 2026-10-19). STARTSPD (start running) is
// written into the stage parameters but has no effect on a directly started stage (quiet run 2026-10-19), so it is not shown.
const RULES = [   // [key, label, help, unit, min, max, proven]
  ['timeLimit', 'Time limit', 'The mission fails when the stage clock reaches this (empty = no limit). The on-screen timer still counts up from 0, it does not count down.', 'seconds', 0, 3600, true],
  ['startRings', 'Starting rings', 'Shadow begins the stage (and every restart) holding this many rings.', 'rings', 0, 999, true],
  ['startLives', 'Lives', 'Lives at the start of the stage.', 'lives', 1, 99, true]];   // all three proven: quiet runs stage-rules-runtime (2026-10-19)
function rulesCard() {
  if (!project) return ''; const r = project.stageRules || {}, shown = RULES.filter(x => x[6]);
  if (!shown.length) return `<div class="card"><h4>Rules <small class="tagOff">coming soon</small></h4><p class="muted">Time limit, starting rings and lives arrive with the next game runtime; they show here once they work in game.</p></div>`;
  return `<div class="card"><h4>Rules <small class="tagOk">works in game</small></h4>${shown.map(([k, label, help, unit, mn, mx]) => `<label class="field"><span>${label} <em class="unit">${unit}</em></span><input class="num" type="number" min="${mn}" max="${mx}" step="1" data-rule="${k}" value="${r[k] ?? ''}" placeholder="off"><small>${help}</small></label>`).join('')}</div>`;
}
function wireRules(root) {
  root.querySelectorAll('[data-rule]').forEach(inp => inp.onchange = () => { snapshot(); const r = {...(project.stageRules || {})}, k = inp.dataset.rule;
    if (inp.value === '') delete r[k]; else r[k] = Math.round(+inp.value); if (Object.keys(r).length) project.stageRules = r; else delete project.stageRules; changed(); });
}
// #100 Music: project.stageFiles.music = null (the slot's own song) | 'none' (silence) | {track: 'sng_x.adx'} (any game
// track) | {asset: id, loopStart?: s} (your WAV, converted to the game's ADX at build, looping) | stage number (older
// projects). server.py music_bytes / music_tracks / music-preview; tools/audio/adx.py.
let musicTracks = null;
const musicPicker = Object.assign(document.createElement('input'), {type: 'file', accept: '.wav,.mp3,.m4a,.aac,.flac,.wma,.ogg,.adx,audio/*', hidden: true}); document.body.append(musicPicker);
musicPicker.onchange = async () => {
  const f = musicPicker.files[0]; if (!f || !project) return;
  try {
    toast(`Importing ${f.name}…`); const bytes = new Uint8Array(await f.arrayBuffer()); let bin = ''; for (let i = 0; i < bytes.length; i += 16384) bin += String.fromCharCode(...bytes.slice(i, i + 16384));
    const m = await api('/api/upload', {project: project.id, name: f.name, kind: 'music', data: btoa(bin)});
    snapshot(); const list = (project.customAssets ||= []); if (!list.some(a => a.id === m.id)) list.push(m);
    project.stageFiles = {...(project.stageFiles || {}), music: {asset: m.id}}; changed(); redrawMusic(); renderIssueBadge();
    toast(['wav', 'adx'].includes(m.format) ? `Music: ${f.name}` : `Music: ${f.name} is ${m.format.toUpperCase()} - export it as WAV to build (no converter for ${m.format.toUpperCase()} yet)`, !['wav', 'adx'].includes(m.format));
  } catch (e) { toast(e.message, true); } finally { musicPicker.value = ''; }
};
function musicValue() { const v = project.stageFiles?.music; return v == null ? '' : v === 'none' ? 'none' : typeof v === 'number' ? 'track:' + musicFile(v) : v.track ? 'track:' + v.track : 'asset:' + v.asset; }
function musicCard() {
  if (!project) return '';
  if (!musicTracks) { musicTracks = []; api('/api/music-tracks').then(r => { musicTracks = r.tracks; redrawMusic(); }).catch(e => toast(e.message, true)); }
  const cur = musicValue(), own = musicFile(project.stage), ownName = musicTracks.find(t => t.file === own)?.name || stageName(project.stage);
  const mine = (project.customAssets || []).filter(a => a.kind === 'music'), groups = [...new Set(musicTracks.map(t => t.group))];
  const sel = `<option value="" ${cur === '' ? 'selected' : ''}>${esc(ownName)} music${project.mode === 'new' ? ' (the slot this stage borrows)' : " (this stage's own)"}</option><option value="none" ${cur === 'none' ? 'selected' : ''}>None (silence)</option>
    <option value="pick">Your own song… (WAV, MP3, M4A, FLAC, OGG)</option>${mine.length ? `<optgroup label="Your songs">${mine.map(a => `<option value="asset:${esc(a.id)}" ${cur === 'asset:' + a.id ? 'selected' : ''}>${esc(a.name)}</option>`).join('')}</optgroup>` : ''}
    ${groups.map(g => `<optgroup label="${esc(g)}">${musicTracks.filter(t => t.group === g).map(t => `<option value="track:${esc(t.file)}" ${cur === 'track:' + t.file ? 'selected' : ''}>${esc(t.name)}</option>`).join('')}</optgroup>`).join('')}`;
  const asset = cur.startsWith('asset:') ? mine.find(a => 'asset:' + a.id === cur) : null, fmt = String(asset?.format || '').toLowerCase(), songOk = ['adx', 'wav', 'mp3', 'm4a', 'aac', 'flac', 'wma', 'ogg'].includes(fmt);
  const src = cur === 'none' ? '' : asset ? `/api/music-preview?id=${encodeURIComponent(project.id)}&file=${encodeURIComponent(asset.file)}` : `/api/music-preview?track=${encodeURIComponent(cur.startsWith('track:') ? cur.slice(6) : own)}`;
  const note = cur === 'none' ? 'No music plays in this stage.' : asset ? (songOk ? `Converted to the game's music format when you build, and it loops${asset.decoder ? ` (${fmt.toUpperCase()} read by ${esc(asset.decoder)})` : ''}. Fades or a quiet start belong in the file itself.` : `<span class="badText">${fmt.toUpperCase()} cannot be converted. Export the song as WAV, MP3, M4A or FLAC.</span>`) : 'Loops for the whole stage.';
  return `<div class="card"><h4>Music <small class="tagOk">works in game</small></h4>
    <label class="field"><span>Song</span><select class="num" id="musicSel">${sel}</select><small>${note}</small></label>
    ${asset && songOk ? `<label class="field"><span>Loop back to (seconds)</span><input class="num" type="number" min="0" step="0.1" id="musicLoop" value="${+(project.stageFiles.music.loopStart || 0)}"><small>When the song ends it restarts from here (0 = the beginning).</small></label>` : ''}
    ${src ? `<div class="field"><span>Listen</span><audio controls preload="none" src="${src}" style="width:100%"></audio><small>The first 20 seconds.</small></div>` : ''}</div>`;
}
function redrawMusic() { const el = $('musicCard'); if (el) { el.innerHTML = musicCard(); wireMusic(el); } }
function wireMusic(root) {
  root.querySelector('#musicSel')?.addEventListener('change', e => { const v = e.target.value;
    if (v === 'pick') { e.target.value = musicValue(); return musicPicker.click(); }
    snapshot(); const sf = {...(project.stageFiles || {})};
    if (v === '') delete sf.music; else sf.music = v === 'none' ? 'none' : v.startsWith('track:') ? {track: v.slice(6)} : {asset: v.slice(6)};
    if (Object.keys(sf).length) project.stageFiles = sf; else delete project.stageFiles; changed(); redrawMusic(); renderIssueBadge(); });
  root.querySelector('#musicLoop')?.addEventListener('change', e => { const m = project.stageFiles?.music; if (!m || typeof m !== 'object') return; snapshot(); m.loopStart = Math.max(0, +e.target.value || 0); changed(); });
}
// Sound effects (server.py stageFiles.sound): 'auto' (default) = this stage's sound bank + the bank of every boss
// placed from another stage | <stage id> = that stage's bank instead | 'none' = this stage's own bank only (no boss banks)
function soundCard() {
  const v = project.stageFiles?.sound ?? 'auto', stages = (config.stages || []).filter(st => st.id >= 100 && st.id <= 710 && st.name && !/^stg/.test(st.name));
  const opt = (val, label) => `<option value="${val}" ${String(v) === String(val) ? 'selected' : ''}>${esc(label)}</option>`;
  const note = v === 'none' ? "Only this stage's own sound effects: a boss from another stage plays without its sounds." : v === 'auto' ? "Auto: this stage's own sound effects, and bosses bring their own sounds." : `The sound effects of ${esc(stageName(+v))} instead of this stage's own.`;
  return `<div class="card"><h4>Sound effects</h4><label class="field"><span>Sound bank</span><select class="num" id="soundSel">${opt('auto', 'Auto (recommended)')}${opt('none', 'This stage only (no boss sounds)')}<optgroup label="Another stage's sounds">${stages.map(st => opt(st.id, st.name)).join('')}</optgroup></select><small>${note}</small></label></div>`;
}
function wireSound(root) {
  root.querySelector('#soundSel')?.addEventListener('change', e => { const v = e.target.value; snapshot(); const sf = {...(project.stageFiles || {})};
    if (v === 'auto') delete sf.sound; else sf.sound = v === 'none' ? 'none' : +v;
    if (Object.keys(sf).length) project.stageFiles = sf; else delete project.stageFiles; changed(); const el = $('soundCard'); if (el) { el.innerHTML = soundCard(); wireSound(el); } });
}
// #103 Sky: the game has no sky file - a stage's sky is geometry in its backdrop chunks. Picking one inserts real geometry
// rows (sky: true, no collision, own textures) via POST /api/sky (tools/stage_editor/sky.py): another stage's backdrop
// re-centred here, or a dome with your picture. project.sky = {stage} | {image} remembers the choice for this card.
let skyList = null, skyLift = false;   // skyLift: the Lift the sky box before a sky is picked (server.py /api/sky lift)
const skyPicker = Object.assign(document.createElement('input'), {type: 'file', accept: 'image/png,image/jpeg,image/webp', hidden: true}); document.body.append(skyPicker);
skyPicker.onchange = async () => {
  const f = skyPicker.files[0]; if (!f || !project) return;
  try {
    const bytes = new Uint8Array(await f.arrayBuffer()); let bin = ''; for (let i = 0; i < bytes.length; i += 16384) bin += String.fromCharCode(...bytes.slice(i, i + 16384));
    const m = await api('/api/upload', {project: project.id, name: f.name, kind: 'texture', data: btoa(bin)});
    if (!project.materials.some(x => x.id === m.id)) project.materials.push(m);
    await setSky({material: m.id, materials: [m]}, {image: m.id, name: f.name});
  } catch (e) { toast(e.message, true); } finally { skyPicker.value = ''; }
};
function startPos() { const s = project.objects.find(o => o.kind === 'playerstart' && !o.player) || project.objects.find(o => o.kind === 'spawn'); return s ? s.position : [0, 0, 0]; }
function clearSkyRows() {
  for (const it of items.filter(x => x.o.kind === 'geometry' && x.o.sky)) { const arr = listFor(it.o), i = arr.indexOf(it.o); if (i >= 0) arr.splice(i, 1); items.splice(items.indexOf(it), 1); byId.delete(it.id); const n = nodeOf.get(it); if (n) { nodes.remove(n); nodeOf.delete(it); } }
}
async function setSky(req, choice) {
  toast('Making the sky…'); const r = choice ? await api('/api/sky', {project: project.id, start: startPos(), ...req}) : {geometry: [], materials: []};
  snapshot(); clearSkyRows(); for (const m of r.materials) if (!project.materials.some(x => x.id === m.id)) project.materials.push(m);
  for (const g of r.geometry) addGeometry(g, false);
  if (choice) project.sky = choice; else delete project.sky;
  buildMarkers(); changed(); drawFlow(); renderPanel(); toast(choice ? `Sky: ${choice.name || stageName(choice.stage)} (${r.geometry.length} piece${r.geometry.length === 1 ? '' : 's'}) - fly out to see it` : 'Sky removed');
}
function skyCard() {
  if (!project) return '';
  if (!(project.mode === 'new' || project.customStage)) return `<div class="card"><h4>Sky <small class="tagOk">the stage's own</small></h4><p class="muted">This stage keeps its own sky. Turn on <b>Backdrop art</b> in the view to see it.</p></div>`;
  if (!skyList) { skyList = []; api('/api/skies').then(r => { skyList = r.skies; const el = $('skyCard'); if (el) { el.innerHTML = skyCard(); wireSky(el); } }).catch(e => toast(e.message, true)); }
  const cur = project.sky || null, img = cur?.image && project.materials.find(m => m.id === cur.image);
  const tile = (key, label, thumb, on) => `<button class="pickTile ${on ? 'on' : ''}" data-sky="${key}" title="${esc(label)}">${thumb ? `<img src="${thumb}" alt="" loading="lazy">` : '<div class="noImg"></div>'}<span>${esc(label)}</span></button>`;
  return `<div class="card"><h4>Sky <small class="tagOk">works in game</small></h4><p class="muted">Pick another stage's sky or your own picture. It is added as scenery (no collision) around the stage; the view shows it.</p>
    <div class="pickGrid">${tile('none', 'None (plain colour)', '', !cur)}${tile('image', img ? 'Your picture' : 'Your picture…', img?.asset ? `/asset/${img.asset}` : '', !!cur?.image)}
    ${skyList.length ? skyList.map(s => tile(String(s.stage), s.name.replace('Boss: ', ''), s.thumb, cur?.stage === s.stage)).join('') : '<p class="muted">Loading the skies… (the first time takes a minute)</p>'}</div>
    <label class="field chk"><input type="checkbox" id="skyLift" ${(cur?.stage != null ? cur.lift : skyLift) ? 'checked' : ''}> <span>Lift the sky</span><small>Raises skies that sit below the stage, e.g. Westopolis.</small></label></div>`;
}
function wireSky(root) {
  root.querySelectorAll('[data-sky]').forEach(b => b.onclick = async () => { const k = b.dataset.sky;
    try { if (k === 'none') return await setSky(null, null); if (k === 'image') return skyPicker.click(); const s = skyList.find(x => String(x.stage) === k); await setSky({stage: +k, lift: skyLift}, {stage: +k, name: s?.name, ...(skyLift ? {lift: true} : {})}); }
    catch (e) { toast(e.message, true); } });
  root.querySelector('#skyLift')?.addEventListener('change', async e => { skyLift = e.target.checked; const cur = project.sky;   // a stage sky already chosen: made again, lifted or not
    if (cur?.stage != null) try { await setSky({stage: cur.stage, lift: skyLift}, {stage: cur.stage, name: cur.name, ...(skyLift ? {lift: true} : {})}); } catch (err) { toast(err.message, true); } });
}
// #102 Voice / hint lines: a custom stage borrows a slot (100 = Westopolis), whose mission records speak their own
// reminder lines (Mission::Message update fn_802EEAF0 -> talk request fn_801FAF00, every ~20 s). project.customStage
// .voice = 'none' (default for custom stages) | 'slot' -> mods/custom_stage.json 'voice' for the runtime (ask).
// Hint balls / talk triggers placed in the stage always speak (they are the stage author's own).
function voiceCard() {
  if (!project || !(project.mode === 'new' || project.customStage)) return '';
  const v = project.customStage?.voice || 'none', slot = stageName(project.stage);
  return `<div class="card"><h4>Voice &amp; hint lines <small class="tagOk" title="Checked in game (2026-10-19)">works in game</small></h4>
    <label class="field"><span>Mission voice lines</span><select class="num" id="voiceSel"><option value="none" ${v === 'none' ? 'selected' : ''}>None</option><option value="slot" ${v === 'slot' ? 'selected' : ''}>${esc(slot)}'s lines (the borrowed slot)</option></select>
    <small>${v === 'none' ? `No mission reminders from ${esc(slot)}.` : `Characters repeat ${esc(slot)}'s mission reminders during play.`} Hint balls and talk triggers you place always speak.</small></label></div>`;
}
function wireVoice(root) {
  root.querySelector('#voiceSel')?.addEventListener('change', e => { snapshot(); const c = (project.customStage ||= {}); if (e.target.value === 'none') delete c.voice; else c.voice = e.target.value; changed(); const el = $('voiceCard'); if (el) { el.innerHTML = voiceCard(); wireVoice(el); } });
}
// ---- #99 Missions (2026-10-19): one card, one block per route; the objective decides which fields show. Data =
// project.customStage.goals[route] = {type, count?, seconds?, target?: object id, next?: stage | null} (custom_stage.py
// plan_goals / write_sidecar). 'Works in game' = the native records of the stage's slot (100: Neutral goal ring, Dark
// defeat GUN, Hero defeat Black Arms; missions-phase7.md); everything else is written for the runtime and says so.
const MS_ROUTES = [['neutral', 'Neutral', 'goal'], ['dark', 'Dark', 'defeat'], ['hero', 'Hero', 'defeat']];
const MS_OBJECTIVES = route => [['', 'No mission on this route'], ['goal', 'Reach the goal ring'], ['defeat', route === 'dark' ? 'Defeat GUN enemies' : route === 'hero' ? 'Defeat Black Arms enemies' : 'Defeat enemies'],
  ['rings', 'Collect rings'], ['timer', 'Reach the goal ring before time runs out'], ['reach', 'Reach a spot']];
const MS_HELP = {goal: 'Touch the goal ring to finish.', defeat: 'Beat this many of them to finish (enemies placed in the stage count).', rings: 'Finish once Shadow holds this many rings.',
  timer: 'Reach the goal ring before the clock runs out.', reach: 'Get to the chosen object to finish.'};
const msGoals = () => (project.customStage ||= {}).goals ||= {};
function msTarget(route, g) {
  const want = g.type === 'reach' ? 'the spot to reach' : 'the goal ring', t = g.target && byId.get(g.target);
  const rings = project.objects.filter(o => (o.nativeName || o.name) === 'Goal Ring'), auto = !g.target && (g.type === 'goal' || g.type === 'timer') && rings.length === 1 ? byId.get(rings[0].id) : null;
  const shown = t ? esc(t.name) : auto ? `${esc(auto.name)} <small class="muted">(the only one)</small>` : g.target ? '<span class="badText">gone: select another</span>' : `<span class="muted">none selected</span>`;
  return `<div class="field"><span>${g.type === 'reach' ? 'Spot to reach' : 'Goal ring'}</span><div class="msTarget"><b>${shown}</b>
    <button class="ghost small" data-mspick="${route}" title="Click it in the 3D view">Select…</button>${t || auto ? ` <button class="ghost small" data-msshow="${route}" title="Fly the camera there">Show</button>` : ''}${t ? ` <button class="ghost small" data-msclear="${route}" title="Clear the choice">✕</button>` : ''}</div>
    ${!t && !auto && g.type !== 'reach' && !rings.length ? '<small class="badText">Place a Goal Ring first (Add panel → Goal Ring).</small>' : `<small>Pick ${want} in the 3D view.</small>`}</div>`;
}
function missionsCard() {
  if (!project) return '';
  if (!(project.mode === 'new' || project.customStage)) return `<div class="card"><h4>Missions <small class="tagOk">the game's own</small></h4><p class="muted">This stage keeps its original Hero, Neutral and Dark missions. Grades and the enemy counts below change how they are scored.</p></div>`;
  const g = msGoals(), stages = (config.stages || []).filter(st => st.id !== project.stage && st.id >= 100 && st.id <= 710 && st.name && !/^stg/.test(st.name));
  const block = ([r, label, native]) => { const x = g[r] || {}, t = x.type || '', nat = t && t === native && project.mode === 'new';
    const n = t === 'defeat' ? csEnemies(r) : 0, need = +x.count || 1;
    const chip = !t ? '<small class="tagOff">off</small>' : nat ? '<small class="tagOk">works in game</small>' : '<small class="tagWarn" title="The build writes it for the runtime (custom mission records); the unmodified game does not count it yet">needs the runtime</small>';
    const fields = [];
    if (t === 'defeat') fields.push(`<label class="field"><span>How many</span><input class="num" type="number" min="1" step="1" data-msn="${r}" value="${x.count ?? Math.max(1, n)}"><small class="${r === 'neutral' || n >= need ? 'muted' : 'badText'}">${r === 'neutral' ? 'Any enemy counts.' : `${n} ${r === 'dark' ? 'GUN' : 'Black Arms'} enemies placed${n < need ? ` - place ${need - n} more` : ''}.`}</small></label>`);
    if (t === 'rings') fields.push(`<label class="field"><span>Rings needed</span><input class="num" type="number" min="1" step="1" data-msn="${r}" value="${x.count ?? 100}"></label>`);
    if (t === 'timer') fields.push(`<label class="field"><span>Time limit (seconds)</span><input class="num" type="number" min="5" step="1" data-mss="${r}" value="${x.seconds ?? 120}"></label>`);
    if (['goal', 'timer', 'reach'].includes(t)) fields.push(msTarget(r, x));
    if (t) fields.push(`<label class="field"><span>After clearing</span><select class="num" data-msnext="${r}"><option value="">Back to the stage select</option><optgroup label="Go on to another stage (needs the runtime)">${stages.map(st => `<option value="${st.id}" ${x.next === st.id ? 'selected' : ''}>${esc(st.name)}</option>`).join('')}</optgroup></select><small>${x.next == null ? 'The results screen, then back to where the stage was started (stage select or launcher).' : 'Without the runtime the game still returns to the stage select.'}</small></label>`);
    return `<div class="msRoute"><div class="msHead"><b>${label}</b>${chip}</div>
      <label class="field"><span>Objective</span><select class="num" data-mso="${r}">${MS_OBJECTIVES(r).map(([v, l]) => `<option value="${v}" ${v === t ? 'selected' : ''}>${l}${v && !(v === native && project.mode === 'new') ? ' (needs the runtime)' : ''}</option>`).join('')}</select>${t ? `<small>${MS_HELP[t]}</small>` : ''}</label>${fields.join('')}</div>`; };
  return `<div class="card msCard"><h4>Missions</h4><p class="muted">One mission per route. Each shows only the settings its objective needs.</p>${MS_ROUTES.map(block).join('')}</div>`;
}
function redrawMissions() { const el = $('missionsCard'); if (el) { el.innerHTML = missionsCard(); wireMissions(el); } }
function wireMissions(root) {
  const goal = r => msGoals()[r];
  root.querySelectorAll('[data-mso]').forEach(sel => sel.onchange = () => { snapshot(); const r = sel.dataset.mso, old = goal(r) || {}; if (!sel.value) delete msGoals()[r];
    else msGoals()[r] = {type: sel.value, ...(sel.value === 'defeat' ? {count: Math.max(1, csEnemies(r))} : sel.value === 'rings' ? {count: 100} : sel.value === 'timer' ? {seconds: 120} : {}), ...(old.next != null ? {next: old.next} : {}), ...(old.target && sel.value !== 'defeat' && sel.value !== 'rings' ? {target: old.target} : {})};
    changed(); redrawMissions(); });
  root.querySelectorAll('[data-msn]').forEach(inp => inp.onchange = () => { const g = goal(inp.dataset.msn); if (!g) return; snapshot(); g.count = Math.max(1, Math.round(+inp.value || 1)); changed(); redrawMissions(); });
  root.querySelectorAll('[data-mss]').forEach(inp => inp.onchange = () => { const g = goal(inp.dataset.mss); if (!g) return; snapshot(); g.seconds = Math.max(5, Math.round(+inp.value || 120)); changed(); redrawMissions(); });
  root.querySelectorAll('[data-msnext]').forEach(sel => sel.onchange = () => { const g = goal(sel.dataset.msnext); if (!g) return; snapshot(); if (sel.value === '') delete g.next; else g.next = Number(sel.value); changed(); redrawMissions(); });
  root.querySelectorAll('[data-msclear]').forEach(b => b.onclick = () => { const g = goal(b.dataset.msclear); if (!g) return; snapshot(); delete g.target; changed(); redrawMissions(); });
  root.querySelectorAll('[data-msshow]').forEach(b => b.onclick = () => { const g = goal(b.dataset.msshow) || {}, rings = project.objects.filter(o => (o.nativeName || o.name) === 'Goal Ring'); const id = g.target || (rings.length === 1 ? rings[0].id : null); if (id) focusObject(id); });
  root.querySelectorAll('[data-mspick]').forEach(b => b.onclick = async () => { const r = b.dataset.mspick, g = goal(r); if (!g) return;
    const id = await pickObject({filter: g.type === 'reach' ? '' : 'native', label: g.type === 'reach' ? `the ${r} route's spot to reach` : `the ${r} route's goal ring`}); if (!id) return redrawMissions();
    const o = byId.get(id)?.o; if (g.type !== 'reach' && o && (o.nativeName || o.name) !== 'Goal Ring') toast(`${byId.get(id).name} is not a Goal Ring: the game finishes this route at a Goal Ring`, true);
    snapshot(); g.target = id; changed(); showPanel('settings'); redrawMissions(); });
}
function customStageCard() {
  if (!project) return ''; const c = project.customStage || {}, g = c.goals || {}, cover = (project.customAssets || []).find(a => a.id === c.cover || a.file === c.cover);
  const goalRow = ([r, label, native]) => { const x = g[r] || {}, t = x.type || '', nat = t && t === native && project.mode === 'new';
    const n = t === 'defeat' ? csEnemies(r) : 0, need = t === 'defeat' ? (+x.count || 1) : 0;
    const amount = t === 'defeat' || t === 'rings' ? `<input class="num csN" type="number" min="1" step="1" data-csn="${r}" value="${x.count ?? (t === 'defeat' ? Math.max(1, n) : 100)}" title="how many">` : t === 'timer' ? `<input class="num csN" type="number" min="5" step="1" data-css="${r}" value="${x.seconds ?? 120}" title="seconds">` : '';
    const note = !t ? '' : nat ? (t === 'defeat' ? `<small class="${n >= need ? 'okText' : 'badText'}">${n} ${r === 'dark' ? 'GUN' : 'Black Arms'} placed${n < need ? `, ${need} needed` : ''}</small>` : project.objects.some(o => (o.nativeName || o.name) === 'Goal Ring') ? '<small class="okText">works in game</small>' : '<small class="badText">place a Goal Ring</small>')
      : `<small class="warnText" title="Written into the build for the runtime (custom mission records); the game does not count it yet">needs the runtime</small>`;
    return `<div class="csGoal"><b>${label}</b><select class="num ${amount ? '' : 'wide'}" data-cst="${r}">${CS_TYPES.map(([v, l]) => `<option value="${v}" ${v === t ? 'selected' : ''}>${l}</option>`).join('')}</select>${amount}${note}</div>`; };
  const chk = csCheck && csCheck.id === project.id ? `<div class="csCheck">${csCheck.errors.map(x => `<p class="badText">✕ ${esc(x)}</p>`).join('')}${csCheck.warnings.map(x => `<p class="warnText">! ${esc(x)}</p>`).join('')}${csCheck.checks.map(x => `<p class="okText">✓ ${esc(x)}</p>`).join('')}<p class="muted">${csCheck.ok ? 'Playable: build it, then Playtest or play it from the launcher.' : 'Fix the red lines, then Validate again.'}</p></div>` : '';
  return `<div class="card csCard"><h4>Name, picture &amp; sharing</h4>
    <p class="muted">How this stage shows in the launcher's Custom stages list and in a shared .shadowstage file.</p>
    <label class="field"><span>Description</span><textarea class="num" id="csDesc" rows="3" maxlength="400" placeholder="A line or two: what the stage is, what to do">${esc(c.description || '')}</textarea></label>
    <label class="field"><span>Author</span><input class="num" id="csAuthor" maxlength="60" value="${esc(c.author || '')}" placeholder="your name"></label>
    <div class="field"><span>Cover image</span><div class="csCover">${cover ? `<img src="/api/project-asset?id=${encodeURIComponent(project.id)}&file=${encodeURIComponent(cover.file)}" alt="cover">` : '<div class="csNoCover">no cover</div>'}<div><button class="ghost small" id="csCoverPick">${cover ? 'Change…' : 'Choose an image…'}</button>${cover ? ' <button class="ghost small" id="csCoverDel">Remove</button>' : ''}<small class="muted">16:9 looks best (shown at 192 × 108)</small></div></div></div>
    <div class="wsBtns"><button class="ghost" id="csValidate">Validate</button> <button class="ghost" id="csExport" ${isFresh() ? 'disabled title="Save the project first"' : ''}>Export .shadowstage</button></div>${chk}</div>`;
}
const csPicker = Object.assign(document.createElement('input'), {type: 'file', accept: 'image/png,image/jpeg,image/webp', hidden: true}); document.body.append(csPicker);
csPicker.onchange = async () => {
  const f = csPicker.files[0]; if (!f || !project) return;
  try {
    const bytes = new Uint8Array(await f.arrayBuffer()); let bin = ''; for (let i = 0; i < bytes.length; i += 16384) bin += String.fromCharCode(...bytes.slice(i, i + 16384));
    const m = await api('/api/upload', {project: project.id, name: f.name, kind: 'texture', data: btoa(bin)});
    snapshot(); const list = (project.customAssets ||= []); if (!list.some(a => a.id === m.id)) list.push({id: m.id, name: f.name, file: m.file, kind: 'cover', width: m.width, height: m.height});
    (project.customStage ||= {}).cover = m.id; changed(); redrawCustomStage(); toast(`Cover: ${f.name} (${m.width}×${m.height})`);
  } catch (e) { toast(e.message, true); } finally { csPicker.value = ''; }
};
function redrawCustomStage() { const el = $('customStage'); if (el) { el.innerHTML = customStageCard(); wireCustomStage(el); } }
function wireProjectName(root) {   // UX follow-up: rename a project (any project; the name is in its project.json)
  const el = root.querySelector('#projName'); if (!el) return;
  const commit = () => { const v = el.value.trim().slice(0, 80); if (!v) { el.value = project.name; return toast('A project needs a name', true); } if (v === project.name) return;
    snapshot(); project.name = v; $('projectTitle').textContent = v; document.title = `• ${v} · Stage Forge`; changed(); toast(`Renamed to "${v}" (Save keeps it; Ctrl Z undoes)`); };
  el.addEventListener('change', commit); el.addEventListener('keydown', e => { e.stopPropagation(); if (e.key === 'Enter') el.blur(); });
}
function wireCustomStage(root) {
  const cs = () => (project.customStage ||= {}), goals = () => (cs().goals ||= {});
  const text = (id, key) => { const el = root.querySelector('#' + id); if (!el) return; el.addEventListener('focus', () => { el.dataset.snap = ''; }); el.addEventListener('input', () => { if (el.dataset.snap === '') { snapshot(); el.dataset.snap = '1'; } const v = el.value.trim(); if (v) cs()[key] = el.value; else delete cs()[key]; changed(); }); };
  text('csDesc', 'description'); text('csAuthor', 'author');
  root.querySelector('#csCoverPick')?.addEventListener('click', () => csPicker.click());
  root.querySelector('#csCoverDel')?.addEventListener('click', () => { snapshot(); delete cs().cover; changed(); redrawCustomStage(); });
  root.querySelectorAll('[data-cst]').forEach(sel => sel.onchange = () => { snapshot(); const r = sel.dataset.cst; if (!sel.value) delete goals()[r]; else goals()[r] = sel.value === 'defeat' ? {type: 'defeat', count: Math.max(1, csEnemies(r))} : sel.value === 'rings' ? {type: 'rings', count: 100} : sel.value === 'timer' ? {type: 'timer', seconds: 120} : {type: sel.value}; changed(); redrawCustomStage(); });
  root.querySelectorAll('[data-csn]').forEach(inp => inp.onchange = () => { const g = goals()[inp.dataset.csn]; if (!g) return; snapshot(); g.count = Math.max(1, Math.round(+inp.value || 1)); changed(); redrawCustomStage(); });
  root.querySelectorAll('[data-css]').forEach(inp => inp.onchange = () => { const g = goals()[inp.dataset.css]; if (!g) return; snapshot(); g.seconds = Math.max(5, Math.round(+inp.value || 120)); changed(); redrawCustomStage(); });
  root.querySelector('#csValidate')?.addEventListener('click', async () => {
    try { if (dirty || isFresh()) await save(); if (dirty) return; const v = await api('/api/custom-stage/validate?id=' + encodeURIComponent(project.id)); csCheck = {...v, id: project.id}; redrawCustomStage(); toast(v.ok ? 'Custom stage: playable' : `Custom stage: ${v.errors.length} problem${v.errors.length > 1 ? 's' : ''}`, !v.ok); }
    catch (e) { toast(e.message, true); }
  });
  root.querySelector('#csExport')?.addEventListener('click', async () => { if (dirty) await save(); if (dirty) return; const a = Object.assign(document.createElement('a'), {href: '/api/package?id=' + encodeURIComponent(project.id)}); document.body.append(a); a.click(); a.remove(); toast('Exporting the .shadowstage file (your browser saves it)'); });
}
function profilesCard() {
  const list = project.lighting?.profiles || [];
  return `<div class="card"><h4>Lighting profiles <small class="tagNo">being tested</small></h4><p class="muted">Rules with no bounds: the Set lighting profile node (Logic) turns one on by its trigger name. Regions with a Trigger work the same way inside their box.</p>
    ${list.map((pf, i) => `<details class="sect"><summary>${esc(pf.name || pf.trigger)} <span>${esc(pf.trigger)}</span></summary><div>
      <label class="field"><span>Name</span><input class="num" data-pf="${i}" data-pk="name" value="${esc(pf.name || '')}"></label><label class="field"><span>Trigger</span><input class="num" data-pf="${i}" data-pk="trigger" value="${esc(pf.trigger)}"></label>
      <div class="vec2"><label class="field"><span>Priority</span><input class="num" type="number" data-pf="${i}" data-pk="priority" value="${pf.priority ?? 50}"></label><label class="field"><span>Blend seconds</span><input class="num" type="number" step="any" data-pf="${i}" data-pk="transition" value="${pf.transition ?? 1.5}"></label></div>
      ${profileFields(pf.profile, `data-pf="${i}" data-pp`)}<button class="ghost" data-pfdel="${i}">Delete profile</button></div></details>`).join('')}
    <button class="ghost" id="pfAdd">+ Lighting profile</button></div>`;
}
function wireProfiles(body) {
  const list = () => lightingData().profiles ||= [];
  body.querySelector('#pfAdd')?.addEventListener('click', () => { snapshot(); const n = list().length + 1; list().push({id: crypto.randomUUID(), name: 'Profile ' + n, trigger: 'profile_' + n, priority: 50, transition: 1.5, profile: {sun_strength: .5}}); changed(); renderPanel(); });
  body.querySelectorAll('[data-pfdel]').forEach(b => b.onclick = () => { snapshot(); list().splice(+b.dataset.pfdel, 1); changed(); renderPanel(); });
  body.querySelectorAll('[data-pf][data-pk]').forEach(inp => inp.onchange = () => { snapshot(); const pf = list()[+inp.dataset.pf], k = inp.dataset.pk; pf[k] = k === 'priority' ? Math.round(+inp.value || 0) : k === 'transition' ? Math.min(30, Math.max(0, +inp.value || 0)) : inp.value.trim().slice(0, 63); changed(); renderIssueBadge(); });
  body.querySelectorAll('[data-pf][data-pp]').forEach(inp => inp.onchange = () => { snapshot(); const pf = list()[+inp.dataset.pf]; pf.profile ||= {}; const err = readProfileKey(pf.profile, inp.dataset.pp, inp.value); if (err) toast(err, true); changed(); lightPreview.key = null; });
}
function renderInspector() { renderInspector0(); appendSurfCard(); appendEnemyCard(); appendArcCard(); appendTileCard(); appendBossCard(); appendLogicCard(); organizeInspector(); }
function renderInspector0() {
  const el = $('inspector');
  if (selected?.o.kind === 'stageobject') {
    const o = selected.o, n = nodeOf.get(selected), by = {}; for (const c of o.commands || []) by[c.action] = (by[c.action] || 0) + 1;
    el.innerHTML = `<div class="idCard" style="--tint:#94a3b833;--tintText:#cbd5e1"><div class="kind">Stage code · read-only</div><div class="title">${esc(o.name)}</div>
      <div class="chips"><span class="chip warn">decoded, not proven</span><span class="chip">${n?.userData.ghostModel ? 'ghost: ' + esc(n.userData.ghostModel) : 'no model: marker'}</span></div></div>
      <details class="sect" open><summary>Driven by the stage's own code</summary><div><p><b>Read-only; decoded, not proven.</b> ${esc(o.notes || '')}</p>
      <p>Model ${esc(o.model || '-')} (${esc(o.modelArchive || '')}) · starts at ${(o.start || []).map(Math.round).join(', ')} on route ${o.startRoute ?? 0} (${Math.round((o.startFraction || 0) * 100)}%) · scale unknown (drawn at 1).</p>
      <p>Routes: ${(o.routes || []).map(r => `${r.route}: spline ${r.splineId} (${r.points} points)`).join(' · ')}</p>
      <p>${(o.commands || []).length} command boxes: ${Object.entries(by).map(([k, v]) => `${v} ${esc(cmdText({action: k}))}`).join(', ')} (the labelled boxes; they are ordinary Command Collision objects, editable).</p>
      <div class="wsBtns"><button class="ghost small" id="soFly">Fly to the start</button></div></div></details>`;
    el.querySelector('#soFly').onclick = () => flyTo(o.start, 600); return;
  }
  if (!selected && artSel) {
    const u = artSel.userData, tris = artSel.children.reduce((a, c) => a + (c.geometry?.index?.count || 0) / 3, 0);
    const KIND = {O: 'main geometry', OS: 'main geometry (OS)', G: 'extra layer G (meaning not confirmed)', D: 'extra layer D (meaning not confirmed)', A: 'layer A (translucent pieces in Sky Troops; meaning not confirmed)', P: 'distant stand-in (low detail)'};
    el.innerHTML = `<div class="idCard" style="--tint:#5fb0ff33;--tintText:#5fb0ff"><div class="kind">Stage art · chunk ${u.chunk}</div><div class="title">${esc(u.world)}</div>
      <div class="chips"><span class="chip">${esc(KIND[u.kind] || u.kind)}</span><span class="chip">${Math.round(tris)} triangles</span>${u.backdrop ? '<span class="chip">sky / backdrop</span>' : ''}</div></div>
      <details class="sect" open><summary>Visibility (editor only)</summary><div><button class="ghost" id="hideWorld">Hide this piece</button> <button class="ghost" id="hideChunk">Hide chunk ${u.chunk}</button> <button class="ghost" id="soloChunk">Show only chunk ${u.chunk}</button>
      <p class="muted">Hidden pieces come back in Layers → Stage art. Hiding never changes the game.</p></div></details>
      <details class="sect" open><summary>Move / rotate / scale</summary><div>${(() => { const e = project.artEdits?.[u.art] || {}, v = (label, key, val, step) => `<div class="field"><span>${label}</span><div class="vec">${['X', 'Y', 'Z'].map((ax, i) => `<div class="ax" data-ax="${ax}"><input class="num" type="number" step="${step}" data-art="${key}" data-i="${i}" value="${val[i]}"></div>`).join('')}</div></div>`;
        return v('Offset', 'offset', e.offset || [0, 0, 0], 'any') + v('Rotation (degrees)', 'rotation', e.rotation || [0, 0, 0], 'any') + v('Scale', 'scale', e.scale || [1, 1, 1], '0.01'); })()}
      <p class="muted">G move · R rotate · T cycles move / scale / rotate (handles at the piece's centre). The build rewrites this piece's geometry, its collision and the chunk's draw box; the editor's collision / kill overlay stays where it was.</p>${project.artEdits?.[u.art] ? '<button class="ghost" id="artReset">Reset this piece</button>' : ''}</div></details>`;
    el.querySelectorAll('[data-art]').forEach(inp => inp.onchange = () => { const v = +inp.value; if (!Number.isFinite(v)) return; snapshot(); const g = artSel; artProxyFromEdit(g); const k = inp.dataset.art, i = +inp.dataset.i;
      if (k === 'offset') { const pv = project.artEdits?.[g.userData.art]?.pivot || artPivot(g); proxy.position.setComponent(i, pv[i] + v); } else if (k === 'scale') proxy.scale.setComponent(i, v || 1); else { const r = [proxy.rotation.x, proxy.rotation.y, proxy.rotation.z]; r[i] = THREE.MathUtils.degToRad(v); proxy.rotation.set(...r, 'YXZ'); }
      artEditFromProxy(g); applyArtEdits(); attachGizmo(); changed(); renderInspector(); renderIssueBadge(); });
    el.querySelector('#artReset')?.addEventListener('click', () => { snapshot(); delete project.artEdits[u.art]; applyArtEdits(); attachGizmo(); changed(); renderInspector(); renderIssueBadge(); });
    $('hideWorld').onclick = () => setArtHidden('w:' + u.art, true); $('hideChunk').onclick = () => setArtHidden('c:' + u.chunk, true);
    $('soloChunk').onclick = () => { snapshot(); for (const m of art.children) if (m.userData.chunk !== u.chunk && !m.userData.backdrop) hiddenArt.add('c:' + m.userData.chunk); hiddenArt.delete('c:' + u.chunk); applyArt(); changed(); renderLens(); renderIssueBadge(); if (panel === 'layers') renderPanel(); toast('Only chunk ' + u.chunk + ' shown · Ctrl Z or "show all" brings the rest back'); };
    return;
  }
  if (multi.size > 1) {
    const list = [...multi], by = CATS.map(([c, l, col]) => [l, col, list.filter(it => it.cat === c).length]).filter(x => x[2]);
    el.innerHTML = `<div class="idCard" style="--tint:#ff4d6d22;--tintText:#ff8fa3"><div class="kind">Selection</div><div class="title">${list.length} placements</div><div class="chips">${by.map(([l, col, n]) => `<span class="chip" style="color:${col}">${n} ${esc(l)}</span>`).join('')}</div></div>
      <details class="sect" open><summary>Together</summary><div><p class="muted">G / R move or rotate them around their centre · Delete · Ctrl D duplicates · Ctrl-click adds / removes · Esc clears.</p>
      <div class="field"><span>Align to the first selected</span><div class="inline">${['x', 'y', 'z'].map(a => `<button class="ghost" data-arr="align" data-ax="${a}">${a.toUpperCase()}</button>`).join('')}</div></div>
      <div class="field"><span>Distribute evenly</span><div class="inline">${['x', 'y', 'z'].map(a => `<button class="ghost" data-arr="distribute" data-ax="${a}">${a.toUpperCase()}</button>`).join('')}</div></div>
      <button class="ghost" id="mDup">Duplicate</button> <button class="ghost" id="mDel">Delete</button> <button class="ghost" id="mPrefab">Save as prefab…</button></div></details>
      <details class="sect"><summary>Members</summary><div>${list.slice(0, 200).map(it => `<div class="rel" data-id="${esc(it.id)}"><span class="dot" style="background:${CAT[it.cat].colour}"></span><span style="flex:1">${esc(it.name)}</span><span class="muted">${esc(it.cls)}</span></div>`).join('')}</div></details>`;
    el.querySelectorAll('[data-arr]').forEach(b => b.onclick = () => arrange(b.dataset.arr, b.dataset.ax));
    el.querySelector('#mDup').onclick = duplicateSelected; el.querySelector('#mDel').onclick = deleteSelected; el.querySelector('#mPrefab').onclick = savePrefab;
    el.querySelectorAll('.rel').forEach(r => r.onclick = () => select(byId.get(r.dataset.id)));
    return;
  }
  if (!selected) { el.innerHTML = '<div class="inspEmpty"><div class="glyph">◇</div><p>Select something in the outliner, the flow strip, the minimap or the viewport.</p></div>'; return; }
  const it = selected, o = it.o, cat = CAT[it.cat], entry = o.kind === 'native' ? catalog.get(`${o.list}-${o.type}`) : null;
  const vec = (label, key, v) => `<div class="field"><span>${label}</span><div class="vec">${['X', 'Y', 'Z'].map((ax, i) => `<div class="ax" data-ax="${ax}"><input class="num" data-vec="${key}" data-i="${i}" type="number" step="any" value="${+(+v[i]).toFixed(2)}"></div>`).join('')}</div></div>`;
  let params = '';
  // Appear Types (UI2 spec 7e): catalog label / unit / help / optionLabels, the raw name and the raw value always shown
  if (entry?.fields?.length) params = groupWaypointRows(entry.fields, entry.fields.map((f, off) => {
    const float = f.type === 'float', val = paramWord(o, off, float); if (val === null) return '';
    const nice = f.label || f.name.replace(/(?<=[a-z])(?=[A-Z])/g, ' '), ol = f.optionLabels || {}, cur = ol[String(val)], wp = /^Pos(\d)_/.exec(f.name), fam = wp && wpFamily(f.name);
    const shown = wp ? (nice.replace(/^(Waypoint \d|Home \(back at origin\))\s*:?\s*/, '') || nice).replace(/^./, c => c.toUpperCase()) : nice;
    const head = `${wp ? `<i class="fam" title="${WP_FAMILY[fam][1]}">${WP_FAMILY[fam][0]}</i>` : ''}${esc(shown)}${f.unit ? ` <em class="unit">${esc(f.unit)}</em>` : ''}${f.label && f.label !== f.name ? ` <small class="rawName">${esc(f.name)}</small>` : ''}`;
    const help = [f.help, cur?.tip && `${cur.label}: ${cur.tip}`].filter(Boolean), helpHtml = help.length ? `<small>${esc(help.join(' · '))}</small>` : '';
    const fs = esc([nice, f.name, f.unit, f.help, ...(f.options || []).map(([v, n]) => `${ol[v]?.label || ''} ${n}`)].filter(Boolean).join(' ').toLowerCase());
    const advAttr = f.group === 'Advanced' || (!f.label && f.group !== 'Basic' && /^(unk|unknown|param\d|pad|reserved|flag\d|dummy)/i.test(f.name)) ? ' data-adv="1"' : '';   // catalog group, else raw-looking names
    if (f.type === 'enum') { const rawName = f.options.find(x => x[0] === val)?.[1];
      return `<label class="field pf" data-fs="${fs}"${advAttr}><span>${head}</span><select class="num" data-param="${off}">${(f.options.some(([v]) => v === val) ? '' : `<option value="${val}" selected>Other (${val})</option>`) + f.options.map(([v, n]) => `<option value="${v}" ${v === val ? 'selected' : ''} title="${esc(ol[v]?.tip || '')}">${esc(ol[v]?.label || n)}</option>`).join('')}</select><code class="rawVal" title="The value in the SET record (parameter ${off})">${esc(rawName ?? 'Other')} = ${val}</code>${helpHtml}</label>`; }
    return `<label class="field pf" data-fs="${fs}"${advAttr}><span>${head}</span><input class="num" type="number" step="any" data-param="${off}" data-float="${float ? 1 : ''}" value="${float ? +val.toFixed(4) : val}" title="Raw value (parameter ${off}, ${float ? 'float' : 'integer'})">${helpHtml}</label>`;
  }));
  const rel = relations(it);
  let pth = '';
  if (o.kind === 'path') {   // classic PATH: spline ID, banking, type, closed, local points; Add point
    const dup = (project.paths || []).filter(q => q !== o && q.splineId === o.splineId).length, pp = pathPoint !== null ? o.points[pathPoint] : null;
    pth = `<details class="sect" open><summary>Path</summary><div>
      <label class="field"><span>Native spline ID</span><input class="num" type="number" step="1" data-path="splineId" value="${o.splineId ?? 1000}">${dup ? `<small>${o.sourceIndex === undefined ? '<b>Used by another path: the build refuses a new path with a repeated ID.</b>' : 'The game data repeats this ID (ticket #9); imported paths may keep it.'}</small>` : ''}</label>
      <label class="field"><span>Type</span><select class="num" data-path="pathType">${[['rail', 'Grind rail'], ['platform', 'Platform path'], ['camera', 'Camera path'], ['loop', 'Loop (on a Loop track surface)'], ['native', 'Plain spline']].map(([x, l]) => `<option value="${x}" ${o.pathType === x ? 'selected' : ''}>${l}</option>`).join('')}</select><small>Rail paths are written as PTP type 2, platform paths as type 8; the others keep their settings word.</small></label>
      <label class="field"><span>Banking (degrees)</span><input class="num" type="number" step="any" data-path="banking" value="${o.banking || 0}"><small>Applied when the point orientations are recomputed: any point edit or a banking change does that (imported paths otherwise keep the game's own).</small></label>
      <label class="check"><input type="checkbox" data-path="closed" ${o.closed ? 'checked' : ''}> Closed loop (back to the first point)</label>
      ${pp ? `<div class="field"><span>Point ${pathPoint + 1} of ${o.points.length} (local)</span><div class="vec">${['X', 'Y', 'Z'].map((ax, i) => `<div class="ax" data-ax="${ax}"><input class="num" data-pp="${i}" type="number" step="any" value="${+(+pp[i]).toFixed(2)}"></div>`).join('')}</div><button class="ghost" id="ppDel">Delete this point</button></div>` : ''}<p class="muted">In the 3D view: drag a blue point (Ctrl: onto the surface) · click the line to insert a point · Shift-drag the first / last point to extend · select a point and press Delete to remove it.</p>
      <div class="field"><span>Points (local x, y, z)</span><div class="ptList" id="ptList">${o.points.slice(0, 800).map((q, i) => `<div class="ptRow ${i === pathPoint ? 'on' : ''}"><button class="ghost small" data-ptsel="${i}" title="Select this point">${i + 1}</button>${q.map((v, k) => `<input class="num" type="number" step="any" data-ptv="${i}:${k}" value="${(+v).toFixed(1)}">`).join('')}<button class="ghost small" data-ptdel="${i}" title="Remove this point">×</button></div>`).join('')}${o.points.length > 800 ? `<p class="muted">${o.points.length - 800} more (raw list below)</p>` : ''}</div></div>
      <button class="ghost" id="ppAdd">${pathPoint !== null ? 'Insert a point after ' + (pathPoint + 1) : 'Add a point at the end'}</button>
      <details class="sect"><summary>Raw points (paste)</summary><div>${o.points.length <= 2000 ? `<textarea class="num mono" id="pathPts" rows="6" spellcheck="false">${o.points.map(q => q.map(round2).join(', ')).join('\n')}</textarea>` : '<p class="muted">Too many points to list.</p>'}</div></details>
      <p class="muted">${o.points.length} points${o.sourceIndex !== undefined ? ' · from the game (index ' + o.sourceIndex + '); untouched game paths are written back byte for byte' : ''}.</p></div></details>`;
  }
  let geo = '';
  if (o.kind === 'geometry') {   // classic GEOMETRY & MATERIAL: chunk, material, UV repeat, solid, visible, surface
    const mats = [...project.materials.filter(m => m.custom && m.kind !== 'icon'), ...chunkMaterials(o.chunk)], cur = project.materials.find(m => m.id === o.material), sv = o.surface ?? 0x00020001;
    const SURF = [[0x00020001, 'Road / floor'], [0x60000001, 'Stone floor'], [0x10010101, 'Metal floor'], [2, 'Jump wall'], [0x60008001, 'Loop track (needed for loops)'], [32, 'Kill plane (not solid; kills when falling down through it)']];
    geo = `<details class="sect" open><summary>Geometry & material</summary><div>
      <div class="field"><span>Chunk (built into this area's own files)</span><div class="inline"><input class="num" type="number" min="1" max="99" data-geo="chunk" value="${o.chunk ?? 1}"><button class="ghost" id="geoChunk" title="The chunk whose bounds contain the shape, and that chunk's main material">From position</button></div></div>
      <div class="field"><span>Material · ${esc(cur?.texture || 'none')}${cur?.custom ? ' · imported' : cur && cur.chunk !== o.chunk ? ' · not loaded by this chunk' : ''}</span><div class="swatches">${mats.map(m => `<button class="sw ${m.id === o.material ? 'on' : ''}" data-mat="${esc(m.id)}" title="${esc(m.custom ? (m.name || m.texture) + ' · imported PNG' : m.texture + ' · ' + m.world)}" style="background-image:url(/asset/${esc(m.asset)})"></button>`).join('') || '<span class="muted">This chunk has no textured materials.</span>'}</div><button class="ghost" id="geoPng" title="Use your own image (up to 4096 x 4096); it goes into the project and the private PC texture pack">Import PNG…</button></div>
      <div class="field"><span>Texture repeat</span><div class="vec">${['X', 'Y'].map((ax, i) => `<div class="ax" data-ax="${ax}"><input class="num" data-uv="${i}" type="number" step="any" min="0.01" value="${o.uvRepeat?.[i] ?? 1}"></div>`).join('')}</div></div>
      <label class="check"><input type="checkbox" data-geo="collision" ${o.collision !== false ? 'checked' : ''}> Solid (collision)</label>
      <label class="check"><input type="checkbox" data-geo="renderVisible" ${o.renderVisible !== false ? 'checked' : ''}> Visible in game</label>
      <label class="field"><span>Surface</span><select class="num" data-geo="surface">${(SURF.some(([v]) => v === sv) ? '' : `<option value="${sv}" selected>Other (${sv})</option>`) + SURF.map(([v, n]) => `<option value="${v}" ${v === sv ? 'selected' : ''}>${esc(n)}</option>`).join('')}</select></label>
      <p class="muted">${o.vertices.length} vertices · ${o.triangles.length} triangles. Size it with T (scale handles) or Scale above.</p>
      ${o.shape === 'loop' ? '<p class="muted">Loop ribbon: solid scenery only. For a loop Shadow can run, use Add &gt; Loop (runnable); or set a loop mesh&#39;s surface to <b>Loop track</b> and draw a <b>Loop</b> path on it (about 2.5 above the surface).</p>' : ''}
      ${o.shape === 'collision' ? `<p class="muted">Original collision face ${o.sourceFace ?? ''} of ${esc(o.sourceRef || 'the stage')}, now your own invisible triangle: the game's face is left out of the build. Delete removes the collision here.${sv === 32 ? ' <b>This was a kill face: your copy still kills when Shadow falls down through it (not solid).</b>' : ''}</p>${o.sourceRef ? '<button class="ghost" id="faceBack">Put original face back</button>' : ''}` : ''}
      ${o.vertices.length <= 256 ? `<div class="field"><span>Vertices (local x, y, z per line; keep the count)</span><textarea class="num mono" id="geoVerts" rows="6" spellcheck="false">${o.vertices.map(v => v.map(x => round2(x)).join(', ')).join('\n')}</textarea></div>` : ''}</div></details>`;
  }
  const cmd = o.kind === 'native' ? commandFor(o.id) : null;
  el.innerHTML = `${cmd ? `<div class="card issue" style="margin:0 0 8px"><span class="sev">ⓘ</span><div><b>${esc(cmd[0].name)}: ${esc(cmdText(cmd[1]))}</b><p>Decoded command for the stage's own code (not proven): ${esc(JSON.stringify(Object.fromEntries(Object.entries(cmd[1]).filter(([k]) => !['id', 'layer', 'position', 'rotation'].includes(k)))))}</p></div></div>` : ''}<div class="idCard" style="--tint:${cat.colour}33;--tintText:${cat.colour}"><div class="kind">${esc(cat.label)} · ${esc(it.cls)}</div>
    <input class="title" id="nameIn" value="${esc(o.name || it.cls)}">
    <div class="chips">${o.kind === 'native' ? modelChip(it) : ''}<span class="chip link" data-chip="zone">Zone ${it.zone ?? '-'}</span>${it.layer ? (([, label, help]) => `<span class="chip link" data-chip="layer" title="${esc(help)}">${esc(label)} layer</span>`)(LAYERS.find(l => l[0] === it.layer) || [it.layer, it.layer, '']) : ''}<span class="chip" title="Position along start -> checkpoints -> goal">${Math.round(it.progress * 100)}% of the run</span>${o.kind === 'native' ? `<span class="chip dimChip" title="The game's object class (list ${o.list}, type ${o.type}), for the research notes and bug reports">class ${o.list.toString(16).padStart(2, '0').toUpperCase()}${o.type.toString(16).padStart(2, '0').toUpperCase()}</span>` : ''}</div></div>
   <details class="sect" open><summary>Transform</summary><div>${vec('Position', 'position', o.position)}${vec('Rotation (degrees)', 'rotation', o.rotation || [0, 0, 0])}${['geometry', 'path'].includes(o.kind) ? vec('Scale', 'scale', o.scale || [1, 1, 1]) : ''}${o.kind === 'trigger' || o.kind === 'lightregion' ? vec('Size', 'size', o.size || [100, 100, 100]) : ''}<button class="ghost" id="focusBtn">Focus in viewport (F)</button></div></details>
   ${['playerstart', 'goalpose', 'spawn'].includes(o.kind) ? `<details class="sect" open><summary>Move</summary><div>${moveButtons(o.id, o.kind === 'goalpose' ? 'the finish pose' : 'the player start')}${o.kind === 'playerstart' ? '<button class="ghost small" id="startRestore">Restore original start</button>' : ''}</div></details>` : ''}${pth}${geo}${lightingSection(o)}${params ? `<details class="sect" open><summary>Parameters</summary><div>${entry.fields.length > 6 ? '<input class="num pfSearch" placeholder="Search parameters (name, raw name, option)">' : ''}${params}${(() => { const r = ringOffsets(o); return r ? `<p class="muted">Drawn as a ${['', 'line', 'circle', 'arch'][r.kind]} of ${r.offsets.length} rings${r.kind === 1 ? ' along the local -Z axis of the object, spaced Length / Number (as the game draws them, from the capture)' : ' (shape from the field names, not proven in game)'}. Change Number of rings / Length here.</p>` : ''; })()}</div></details>` : ''}
   ${o.kind === 'native' ? `<details class="sect"><summary>Preview model</summary><div><label class="field"><span>Model shown in the editor</span><select class="num" id="prevModel"><option value="">Automatic (${esc(nodeOf.get(it)?.userData.modelSource === 'capture' ? 'captured' : 'catalog')})</option>${o.previewModel ? `<option value="${esc(o.previewModel)}" selected>${esc((models?.models || []).find(m => m.id === o.previewModel)?.name || o.previewModel)}</option>` : ''}</select><small>Editor only (classic's previewModel): the game keeps its own model.</small></label></div></details>` : ''}
   ${o.kind === 'native' && texturesOf(it).length ? `<details class="sect"><summary>Textures <span>${texturesOf(it).length}</span></summary><div>${texturesOf(it).map(t => { const ov = project.textureOverrides?.[t.texture];
      return `<div class="texRow"><span class="sw" style="background-image:url(/asset/${esc(ov?.asset || t.asset || '')})"></span><span style="flex:1" title="${esc(t.texture)}">${esc(t.texture)}${ov ? ' <small class="okText">replaced</small>' : ''}</span><button class="ghost small" data-texopen="${esc(t.asset || '')}" title="Open the image's folder in Explorer">Open</button><button class="ghost small" data-texrep="${esc(t.texture)}" title="Replace this texture with your own PNG (everywhere it is used)">Replace…</button>${ov ? `<button class="ghost small" data-texback="${esc(t.texture)}" title="Back to the game's texture">Undo</button>` : ''}</div>`; }).join('')}
      <p class="muted">Replacing changes this texture everywhere in the stage (the game looks textures up by name). The editor shows it at once; the build writes it into the private texture pack.</p></div></details>` : ''}
   ${['trigger', 'teleport', 'native'].includes(o.kind) ? `<details class="sect" open><summary>Logic</summary><div><button class="ghost" id="graphEdit" title="Open this placement's graph in the Logic workspace">Graphs: ${logic.count(o)} · Edit</button></div></details>` : ''}
   <details class="sect" open><summary>Relations <span>${rel.length || ''}</span></summary><div>${rel.length ? rel.map(([x, why]) => `<div class="rel" data-id="${esc(x.id)}"><span class="dot" style="background:${CAT[x.cat].colour}"></span><span style="flex:1">${esc(x.name)}</span><span class="muted">${esc(why)}</span></div>`).join('') : '<div class="muted">No links, triggers or scripts point at this yet. The Logic workspace will show and edit these as nodes.</div>'}</div></details>
   ${o.kind === 'native' ? `<details class="sect"><summary>Game data</summary><div><div class="field"><span>Activation distance (x100 units)</span><input class="num" type="number" data-key="render" value="${o.render ?? 0}"></div><div class="field"><span>Link ID</span><input class="num" type="number" data-key="link" value="${o.link ?? 0}"><small>Link ID 0 unless you build a linked group: objects with the same ID act together in game (a copy that keeps another group's ID can break, e.g. a barricade with the City Lasers' 100 draws one frame).</small></div><div class="muted">Layer ${esc(o.layer)} · record ${o.sourceIndex ?? 'new'}</div></div></details>` : ''}`;
  el.querySelector('#nameIn').onchange = e => { snapshot(); o.name = e.target.value; it.name = o.name; changed(); drawTree(); };
  el.querySelectorAll('[data-vec]').forEach(inp => inp.onchange = () => { const v = +inp.value; if (!Number.isFinite(v)) return; snapshot(); (o[inp.dataset.vec] ||= [0, 0, 0])[+inp.dataset.i] = v; if (inp.dataset.vec === 'position') { it.zone = zoneOf(o.position); it.progress = progressOf(o.position); } const n = nodeOf.get(it); if (n) { n.position.set(...o.position); n.rotation.set(...(o.rotation || [0, 0, 0]).map(THREE.MathUtils.degToRad)); if (o.kind !== 'native') n.scale.set(...(o.scale || [1, 1, 1])); } if (inp.dataset.vec === 'size') rebuildNode(it); relocate(it); changed(); updateMarkers(); drawFlow(); });
  el.querySelectorAll('[data-geo]').forEach(inp => inp.onchange = () => { snapshot(); const k = inp.dataset.geo;
    o[k] = inp.type === 'checkbox' ? inp.checked : k === 'chunk' ? THREE.MathUtils.clamp(Math.round(+inp.value || 1), 1, 99) : Math.round(+inp.value);
    rebuildNode(it); changed(); renderInspector(); renderIssueBadge(); });
  el.querySelectorAll('[data-uv]').forEach(inp => inp.onchange = () => { const v = +inp.value; if (!(v > 0)) return; snapshot(); (o.uvRepeat ||= [1, 1])[+inp.dataset.uv] = v; rebuildNode(it); changed(); });
  el.querySelectorAll('[data-mat]').forEach(b => b.onclick = () => { snapshot(); o.material = b.dataset.mat; rebuildNode(it); changed(); renderInspector(); renderIssueBadge(); });
  el.querySelector('#faceBack')?.addEventListener('click', () => putFaceBack(it));
  if (LIGHT_KINDS[o.kind]) wireLighting(el, it);
  wireMoveButtons(el); el.querySelector('#startRestore')?.addEventListener('click', restoreStart);
  el.querySelector('#graphEdit')?.addEventListener('click', () => showWorkspace('logic', o));
  const pm = el.querySelector('#prevModel');
  pm?.addEventListener('focus', () => { if (pm.dataset.filled) return; pm.dataset.filled = 1; const cur = pm.value; pm.innerHTML = pm.options[0].outerHTML + (models?.models || []).slice().sort((a, b) => a.name.localeCompare(b.name)).map(m => `<option value="${esc(m.id)}" ${m.id === cur ? 'selected' : ''}>${esc(m.name)}</option>`).join(''); });
  pm?.addEventListener('change', () => { snapshot(); if (pm.value) o.previewModel = pm.value; else delete o.previewModel; rebuildNode(it); changed(); renderInspector(); });
  el.querySelectorAll('[data-texopen]').forEach(b => b.onclick = () => openTextureFile(b.dataset.texopen));
  el.querySelectorAll('[data-texrep]').forEach(b => b.onclick = () => { texFor = b.dataset.texrep; texPicker.click(); });
  el.querySelectorAll('[data-texback]').forEach(b => b.onclick = () => { snapshot(); delete project.textureOverrides[b.dataset.texback]; refreshTextures(); changed(); renderInspector(); });
  el.querySelectorAll('[data-path]').forEach(inp => inp.onchange = () => { snapshot(); const k = inp.dataset.path;
    o[k] = inp.type === 'checkbox' ? inp.checked : k === 'pathType' ? inp.value : k === 'splineId' ? Math.round(+inp.value || 0) : +inp.value || 0;
    if (k === 'banking') o.autoOrient = true; if (k === 'pathType' || k === 'splineId') { it.cls = classOf(o); drawTree(); }
    rebuildNode(it); changed(); renderInspector(); });
  el.querySelectorAll('[data-pp]').forEach(inp => inp.onchange = () => { const v = +inp.value; if (!Number.isFinite(v)) return; snapshot(); o.points[pathPoint][+inp.dataset.pp] = v; o.autoOrient = true; refreshPath(it); attachGizmo(); relocate(it); changed(); });
  el.querySelector('#ppDel')?.addEventListener('click', () => deletePathPoint(it, pathPoint));
  el.querySelector('#ppAdd')?.addEventListener('click', () => insertPathPoint(it, pathPoint ?? o.points.length - 1));
  el.querySelectorAll('[data-ptsel]').forEach(b => b.onclick = () => { pathPoint = +b.dataset.ptsel; attachGizmo(); renderInspector(); });
  el.querySelectorAll('[data-ptdel]').forEach(b => b.onclick = () => deletePathPoint(it, +b.dataset.ptdel));
  el.querySelectorAll('[data-ptv]').forEach(inp => inp.onchange = () => { const v = +inp.value; if (!Number.isFinite(v)) return; const [i, k] = inp.dataset.ptv.split(':').map(Number); snapshot(); o.points[i][k] = round2(v); o.autoOrient = true; refreshPath(it); relocate(it); changed(); if (i === pathPoint) attachGizmo(); });
  el.querySelector('#ptList .ptRow.on')?.scrollIntoView({block: 'nearest'});
  el.querySelector('#pathPts')?.addEventListener('change', e => {
    const pts = e.target.value.trim().split('\n').map(l => l.split(/[,\s]+/).filter(Boolean).map(Number));
    if (pts.length < 2 || pts.some(q => q.length !== 3 || !q.every(Number.isFinite))) return toast('Each path point needs three numbers, and a path at least two points.', true);
    snapshot(); o.points = pts; o.autoOrient = true; pathPoint = null; rebuildNode(it); relocate(it); changed(); renderInspector();
  });
  el.querySelector('#geoPng')?.addEventListener('click', () => { pngFor = it; pngPicker.click(); });
  el.querySelector('#geoVerts')?.addEventListener('change', e => {
    const vs = e.target.value.trim().split('\n').map(l => l.split(/[,\s]+/).filter(Boolean).map(Number));
    if (vs.length !== o.vertices.length || vs.some(v => v.length !== 3 || !v.every(Number.isFinite))) return toast(`Keep ${o.vertices.length} lines of three numbers (x, y, z).`, true);
    snapshot(); o.vertices = vs; rebuildNode(it); changed(); toast('Vertices updated');
  });
  el.querySelector('#geoChunk')?.addEventListener('click', () => { snapshot(); o.chunk = defaultChunk(o.position) ?? o.chunk; if (project.materials.find(m => m.id === o.material)?.chunk !== o.chunk) o.material = defaultMaterial(o.chunk); rebuildNode(it); changed(); renderInspector(); renderIssueBadge(); toast(`Chunk ${o.chunk} · ${project.materials.find(m => m.id === o.material)?.texture || 'no material'}`); });
  el.querySelector('.pfSearch')?.addEventListener('input', e => { const q = e.target.value.trim().toLowerCase(); el.querySelectorAll('.pf').forEach(p => p.hidden = !!q && !q.split(/\s+/).every(w => p.dataset.fs.includes(w))); });
  el.querySelectorAll('[data-param]').forEach(inp => inp.onchange = () => { const v = +inp.value; if (!Number.isFinite(v)) return; snapshot(); setParamWord(o, +inp.dataset.param, v, !!inp.dataset.float); rebuildNode(it); changed(); if (enemyKind(o)) { const st = $('inspector').scrollTop; renderInspector(); $('inspector').scrollTop = st; } });   // waypoints / flight path card follow the Behaviour and the values
  el.querySelectorAll('[data-key]').forEach(inp => inp.onchange = () => { snapshot(); o[inp.dataset.key] = Math.max(0, Math.round(+inp.value || 0)); changed(); });
  el.querySelectorAll('.rel').forEach(r => { r.onclick = () => select(byId.get(r.dataset.id)); r.ondblclick = () => select(byId.get(r.dataset.id), true); });
  el.querySelector('#focusBtn').onclick = () => flyTo(o.position);
  el.querySelectorAll('[data-chip]').forEach(c => c.onclick = () => { query = c.dataset.chip === 'zone' ? `zone:${it.zone}` : `layer:${it.layer}`; panel = 'outliner'; groupBy = 'class'; renderPanel(); updateMarkers(); drawFlow(); });
}
function changed() { syncStartRecord(); syncRuleZones(); dirty = true; $('saveState').textContent = 'Unsaved changes'; renderUndo(); scheduleAutoSave(); scheduleDraft(); }

// ---- undo / redo: snapshot the editable state before every edit; restoring reuses the 3D node of every placement whose
// data did not change, so undo is instant even on 1000-object stages ---------------------------------------------------
const undoStack = [], redoStack = [], UNDO_LIMIT = 150;
const editable = () => JSON.stringify({name: project.name, chunkBoxes: project.chunkBoxes ?? null, materials: project.materials ?? null, objects: project.objects, triggers: project.triggers || [], geometry: project.geometry || [], paths: project.paths || [], collisionEdits: project.collisionEdits || {}, stageParams: project.stageParams ?? null, missions: project.missions ?? null, stageSettings: project.stageSettings ?? null, logic: project.logic ?? null, logicFunctions: project.logicFunctions ?? null, prefabs: project.prefabs ?? null, textureOverrides: project.textureOverrides ?? null, artEdits: project.artEdits ?? null, customAssets: project.customAssets ?? null, playtestStart: project.playtestStart ?? null, lighting: project.lighting ?? null, surfaceCollision: project.surfaceCollision ?? null, playerStart: project.playerStart ?? null, enemyPaths: project.enemyPaths ?? null, stageFiles: project.stageFiles ?? null, activationScale: project.activationScale ?? null, customStage: project.customStage ?? null, sky: project.sky ?? null, stageRules: project.stageRules ?? null, hiddenArt: [...hiddenArt], showStandins});
function snapshot() { if (!project) return; undoStack.push(editable()); if (undoStack.length > UNDO_LIMIT) undoStack.shift(); redoStack.length = 0; renderUndo(); }
function undo() { if (!undoStack.length) return toast('Nothing to undo'); redoStack.push(editable()); restore(undoStack.pop()); toast('Undone'); }
function redo() { if (!redoStack.length) return toast('Nothing to redo'); undoStack.push(editable()); restore(redoStack.pop()); toast('Redone'); }
function restore(json) {
  restore0(json); applyStageFilesAtmos(); loadStageFilesPreview();   // undo / redo: In-game look follows the restored stage files
}
function restore0(json) {
  const state = JSON.parse(json), keepSel = selected?.id;
  if (state.name && state.name !== project.name) { project.name = state.name; $('projectTitle').textContent = state.name; const pn = $('projName'); if (pn) pn.value = state.name; }
  const old = new Map(items.map(it => [it.id, [it, JSON.stringify(it.o)]]));
  project.objects = state.objects; project.triggers = state.triggers; if (state.lighting !== undefined) { if (state.lighting === null) delete project.lighting; else project.lighting = state.lighting; } project.geometry = state.geometry || project.geometry || []; project.paths = state.paths || project.paths || [];
  if (state.hiddenArt) { hiddenArt.clear(); for (const k of state.hiddenArt) hiddenArt.add(k); showStandins = !!state.showStandins; applyArt(); renderLens(); }
  const texBefore = JSON.stringify(project.textureOverrides ?? null), artBefore = JSON.stringify(project.artEdits ?? null);
  for (const k of ['chunkBoxes', 'materials', 'stageParams', 'missions', 'stageSettings', 'logic', 'logicFunctions', 'prefabs', 'textureOverrides', 'artEdits', 'customAssets', 'playtestStart', 'lighting', 'surfaceCollision', 'playerStart', 'enemyPaths', 'stageFiles', 'activationScale', 'customStage', 'sky', 'stageRules']) if (state[k] !== undefined) { if (state[k] === null) delete project[k]; else project[k] = state[k]; }
  drawStartMark(); setTimeout(buildSurfColl, 0);
  pathPoint = null;
  if (JSON.stringify(state.collisionEdits || {}) !== JSON.stringify(project.collisionEdits || {})) { project.collisionEdits = state.collisionEdits || {}; buildCollision(); }
  gizmo.detach(); const next = [];
  for (const o of placed().filter(o => o.position)) {
    const prev = old.get(o.id);
    if (prev && prev[1] === JSON.stringify(o)) { prev[0].o = o; next.push(prev[0]); old.delete(o.id); continue; }   // unchanged: keep item + node
    if (prev) { const n = nodeOf.get(prev[0]); if (n) { nodes.remove(n); nodeOf.delete(prev[0]); } old.delete(o.id); }
    const it = makeItem(o); next.push(it); addNode(it);
  }
  for (const [it] of old.values()) { const n = nodeOf.get(it); if (n) { nodes.remove(n); nodeOf.delete(it); } }   // removed by the restore
  items = next; byId = new Map(items.map(it => [it.id, it]));
  if (JSON.stringify(project.textureOverrides ?? null) !== texBefore) { buildArt(); buildNodes(); } else if (JSON.stringify(project.artEdits ?? null) !== artBefore) applyArtEdits();
  multi.clear(); selected = byId.get(keepSel) || null; buildMarkers(); attachGizmo(); renderInspector(); renderPanel(); drawFlow(); logic.refresh();
  dirty = true; $('saveState').textContent = 'Unsaved changes'; renderUndo(); scheduleAutoSave(); scheduleDraft();   // UX pass: a reload after Undo came back with the undone edit
}
function renderUndo() { $('undo').disabled = !undoStack.length; $('redo').disabled = !redoStack.length; $('undo').title = `Undo (Ctrl Z) · ${undoStack.length} step${undoStack.length === 1 ? '' : 's'}`; $('redo').title = `Redo (Ctrl Shift Z / Ctrl Y) · ${redoStack.length}`; }
$('undo').onclick = undo; $('redo').onclick = redo;

// ---- command palette -------------------------------------------------------------------------------------------------
let palIndex = 0, palResults = [];
function commands() {
  return [
    ['Logic workspace (node graphs)', '', () => showWorkspace('logic')],
    ['Run self-test (opens fresh copies of 3 stages; never saves your projects)', '', () => runSelfTestUi()],
    ['Toggle snap (magnet to faces / edges / corners / surface; Alt inverts)', 'N', () => toggleSnap()],
    ['Test from here (F5: save, build, play as Shadow where the camera is, with sound)', 'F5', () => testFromHere()],
    ['Run quick game check (Play from here at the camera view, in a muted window with the live Game view)', '', () => quickGameCheck()],
    ...['x', 'y', 'z'].flatMap(a => [[`Align selection on ${a.toUpperCase()} (to the first selected)`, '', () => arrange('align', a)], [`Distribute selection evenly on ${a.toUpperCase()}`, '', () => arrange('distribute', a)]]),
    ['Restore original start', '', restoreStart], ['Remove player start', '', removeStart], ['Set player start to camera view', '', () => moveMarker(startId(), centrePoint(), true)], ['Place player start at cursor', '', () => placeAtCursor(startId(), 'the player start')], ['Play from here (screen centre)', '', () => setPlayStart(centrePoint())], ['Clear playtest start', '', clearPlayStart], ['Save selection as prefab…', '', savePrefab], ['Delete a prefab…', '', deletePrefab], ['Copy selection', 'Ctrl C', copySelected], ['Paste placements', 'Ctrl V', pastePlacements], ['Select none', 'Esc', () => select(null)], ['Layout workspace', '', () => showWorkspace('layout')], ['Save project', 'Ctrl S', save], ['Playtest', '', playtest], ['Build only (check, no game)', '', buildOnly], ['Stage settings (grades, start, missions)', '', () => showPanel('settings')],
    ...(lastReport ? [['Last build report', '', () => showReport(lastReport)]] : []), ['Open another project…', '', showProjects],
    ['Toggle kill planes', '', () => { showKill = !showKill; renderLens(); }], ['Toggle stage art', '', () => { showArt = !showArt; renderLens(); }],
    ['Show all categories', '', () => { hiddenCats.clear(); refreshVisibility(); }], ['Fly to start', '', () => flyTo(spine[0].toArray(), 600)],
    ...(spine.goal ? [['Fly to goal', '', () => flyTo(spine.goal.position, 600)]] : []),
    ...spine.cps.map(o => [`Go to checkpoint ${paramWord(o, 0)}`, '', () => select(byId.get(o.id), true)]),
    ...zones.map(z => [`Fly to zone ${z.number}`, '', () => flyTo(z.min.map((v, i) => (v + z.max[i]) / 2), 1200)]),
    ['Open classic editor (read-only)', '', () => location.href = '../index.html']];
}
function openPalette() { if (!project) return showProjects(); $('paletteWrap').hidden = false; $('paletteInput').value = ''; runPalette(); $('paletteInput').focus(); }
function runPalette() {
  const q = $('paletteInput').value.toLowerCase().trim();
  const cmds = commands().filter(([n]) => !q || n.toLowerCase().includes(q)).slice(0, 8).map(([n, k, fn]) => ({n, sub: k, tag: 'command', fn}));
  const objs = q ? items.filter(it => (it.name + ' ' + it.cls).toLowerCase().includes(q)).sort((a, b) => a.progress - b.progress).slice(0, 30).map(it => ({n: it.name, sub: `${CAT[it.cat].label} · zone ${it.zone ?? '-'} · ${Math.round(it.progress * 100)}%`, tag: it.cls, fn: () => select(it, true), colour: CAT[it.cat].colour})) : [];
  palResults = [...cmds, ...objs]; palIndex = 0; drawPalette();
}
function drawPalette() {
  $('paletteList').innerHTML = palResults.map((r, i) => `<div class="pItem ${i === palIndex ? 'on' : ''}" data-i="${i}">${r.colour ? `<span class="dot" style="background:${r.colour}"></span>` : '<span class="dot" style="background:#3a4152"></span>'}<span class="name">${esc(r.n)}</span><small>${esc(r.sub)}</small><span class="tag">${esc(r.tag)}</span></div>`).join('') || '<div class="sub">No matches.</div>';
  $('paletteList').querySelectorAll('.pItem').forEach(p => p.onclick = () => { palIndex = +p.dataset.i; goPalette(); });
  $('paletteList').querySelector('.on')?.scrollIntoView({block: 'nearest'});
}
function goPalette() { const r = palResults[palIndex]; $('paletteWrap').hidden = true; r?.fn(); }
$('paletteInput').oninput = runPalette;
$('paletteInput').onkeydown = e => { if (e.key === 'ArrowDown') { palIndex = Math.min(palResults.length - 1, palIndex + 1); drawPalette(); e.preventDefault(); } else if (e.key === 'ArrowUp') { palIndex = Math.max(0, palIndex - 1); drawPalette(); e.preventDefault(); } else if (e.key === 'Enter') goPalette(); else if (e.key === 'Escape') $('paletteWrap').hidden = true; };
$('paletteWrap').onclick = e => { if (e.target === $('paletteWrap')) $('paletteWrap').hidden = true; };
$('openPalette').onclick = openPalette;
addEventListener('keydown', e => {
  const typing = /INPUT|SELECT|TEXTAREA/.test(document.activeElement?.tagName);
  if (logic.active() && !typing && $('paletteWrap').hidden) {
    if (logic.key(e)) { e.preventDefault(); return; }
    const c = e.ctrlKey || e.metaKey, k = e.key.toLowerCase();
    if (!(c && ['k', 's', 'z', 'y'].includes(k))) return;   // Layout keys (Delete, G / R / T, F, Ctrl D, Esc) do nothing on the canvas
  }
  if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'k') { e.preventDefault(); openPalette(); }
  else if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 's') { e.preventDefault(); save(); }
  else if (!typing && (e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'z') { e.preventDefault(); e.shiftKey ? redo() : undo(); }
  else if (!typing && (e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'y') { e.preventDefault(); redo(); }
  else if (!typing && e.key.toLowerCase() === 'f' && selected) flyTo(selected.at, Math.max(120, (nodeOf.get(selected) ? new THREE.Box3().setFromObject(nodeOf.get(selected)).getSize(_b).length() * 1.6 : 120)));
  else if (!typing && drawing && e.key === 'Enter') { e.preventDefault(); finishDraw(); }
  else if (!typing && drawing && e.key === 'Backspace') { e.preventDefault(); drawing.points.pop(); if (!drawing.points.length) drawing.y = null; redrawOutline(); }
  else if (e.key === 'Escape' && project && !$('emptyState').hidden) { $('emptyState').hidden = true; }   // the project / stage list can be cancelled (user request)
  else if (!typing && e.key === 'Escape' && (drawing || faceMode || surfMode)) { if (drawing) { cancelDraw(); toast('Drawing cancelled'); } else if (surfMode) setSurfMode(false); else setFaceMode(false); }
  else if (!typing && e.key === 'Escape') { if (placing) { placing = null; placingKeep = false; tileTurn = null; renderer.domElement.style.cursor = ''; if (panel === 'add') { renderPanel(); $('aq')?.focus(); } } else select(null, false); }
  else if (!typing && e.key === '/' && project && !e.ctrlKey && !e.metaKey) { e.preventDefault(); if (panel !== 'add') { renderAdd.autoFocus = true; showPanel('add'); } else $('aq')?.focus(); }
  else if (!typing && placing && e.key === 'Enter' && [document.body, renderer.domElement, null].includes(document.activeElement)) { e.preventDefault(); placeAtCentre(); }   // #85 keyboard flow: place at the view centre
  else if (!typing && placing?.tilePiece && e.key.toLowerCase() === 'r' && !e.ctrlKey && !e.metaKey) { e.preventDefault(); turnTile(e.shiftKey ? -1 : 1); }   // #128 turn the tile being placed
  else if (!typing && ['g', 'r', 't'].includes(e.key.toLowerCase()) && !selected && artSel) { const m = {g: 'translate', r: 'rotate', t: {translate: 'scale', scale: 'rotate', rotate: 'translate'}[gizmo.mode] || 'translate'}[e.key.toLowerCase()]; gizmo.setMode(m); toast({translate: 'Move the piece: drag the arrows', rotate: 'Rotate the piece: drag the rings', scale: 'Scale the piece: drag the cubes'}[m]); }
  else if (!typing && e.key.toLowerCase() === 'g' && selected) { gizmo.setMode('translate'); toast('Move: drag the arrows'); }
  else if (!typing && e.key.toLowerCase() === 'r' && selected) { gizmo.setMode('rotate'); toast('Rotate: drag the rings'); }
  else if (!typing && e.key.toLowerCase() === 't' && selected) { const next = {translate: 'scale', scale: 'rotate', rotate: 'translate'}[gizmo.mode] || 'translate'; gizmo.setMode(next);
    toast({translate: 'Move: drag the arrows', scale: isDecor(selected.o) || selected.o.kind !== 'native' ? 'Scale: drag the cubes' : 'Scale: shown in the editor only - the game sizes this object from its parameters', rotate: 'Rotate: drag the rings'}[next]); }
  else if (!typing && e.key.toLowerCase() === 'n' && project && !e.ctrlKey && !e.metaKey && !e.altKey) toggleSnap();   // #94 N = Snap on / off
  else if (!typing && e.key === 'Delete' && selected) deleteSelected();
  else if (!typing && (e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'd' && selected) { e.preventDefault(); duplicateSelected(); }
  else if (!typing && (e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'c' && project && (selected || multi.size) && !String(getSelection?.() || '')) { e.preventDefault(); copySelected(); }
  else if (!typing && (e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'v' && project) { e.preventDefault(); pastePlacements(); }
});

// ---- workspaces (only Layout is real in the test build) -------------------------------------------------------------
const WS = {  assets: ['Assets', 'Models, textures, sounds and your imports in one browser, with "where used" per asset (ties into the in-game model capture, #35).'],
  play: ['Play', 'Playtest controls, build log, last run (time, rings, deaths with where they happened) and the boot-to-stage options.']};
// ---- Logic workspace (roadmap 5a, ui2/logic.js over nodes.json) --------------------------------------------------------
// ---- #82 pick an object for a Logic field in the 3D view: the graph hides, a banner explains, the object under the
// cursor gets a box, a click picks it (a field for native objects refuses others with the reason), Esc cancels; then
// the same graph comes back with the value set (one undo step, by the graph's edit)
let objPick = null; const pickBox = new THREE.BoxHelper(undefined, 0x22d3ee); pickBox.visible = false; scene.add(pickBox);
const pickBanner = Object.assign(document.createElement('div'), {className: 'pickBanner', hidden: true}); viewport.append(pickBanner);
function itemAt(cx, cy) {
  const r = renderer.domElement.getBoundingClientRect(); ndc.set((cx - r.left) / r.width * 2 - 1, -(cy - r.top) / r.height * 2 + 1); ray.setFromCamera(ndc, camera);
  const hits = ray.intersectObjects([...nodes.children, ...instanced], true).filter(h => { for (let n = h.object; n; n = n.parent) if (!n.visible) return false; return true; });
  const list = hits.map(h => h.object.isInstancedMesh ? h.object.userData.items[h.instanceId] : h.object.userData.item).filter(Boolean);
  return list.find(x => x.cat !== 'volumes') || list[0] || null;
}
function pickObject({filter = '', label = ''} = {}) {
  if (objPick) objPick.done(null);
  return new Promise(done => {
    const back = $('workspaces').querySelector('.on')?.dataset.ws || 'layout', backOwner = back === 'logic' ? logic.currentOwner() : undefined; showWorkspace('layout');
    pickBanner.innerHTML = `Click the object to pick${label ? ' for <b>' + esc(label) + '</b>' : ''}${filter === 'native' ? ' (a native object)' : ''} · Esc cancels`; pickBanner.hidden = false; renderer.domElement.style.cursor = 'pointer';
    objPick = {filter, done: id => { objPick = null; pickBanner.hidden = true; pickBox.visible = false; renderer.domElement.style.cursor = ''; if (back !== 'layout') showWorkspace(back, backOwner ?? undefined); done(id); }};   // the same graph comes back
  });
}
function focusObject(id) { const it = byId.get(id); if (!it) return toast('That object is not in this project', true); showWorkspace('layout'); select(it, true); }
renderer.domElement.addEventListener('pointermove', e => { if (!objPick) return; const it = itemAt(e.clientX, e.clientY), n = it && nodeOf.get(it);
  if (n) { pickBox.setFromObject(n); pickBox.material.color.set(objPick.filter === 'native' && it.o.kind !== 'native' ? 0xf87171 : 0x22d3ee); pickBox.visible = true; } else pickBox.visible = false; });
renderer.domElement.addEventListener('pointerdown', e => { if (objPick && e.button === 0) objPick.down = [e.clientX, e.clientY]; }, true);
renderer.domElement.addEventListener('pointerup', e => {
  if (!objPick || e.button !== 0 || !objPick.down || Math.hypot(e.clientX - objPick.down[0], e.clientY - objPick.down[1]) > 4) return;
  e.stopImmediatePropagation(); const it = itemAt(e.clientX, e.clientY); if (!it) return toast('Nothing there: click an object (Esc cancels)', true);
  if (objPick.filter === 'native' && it.o.kind !== 'native') return toast(`${it.name} is a ${classOf(it.o)}, not a native object (SET record): pick a native object (Esc cancels)`, true);
  if (objPick.filter === 'volume' && !['trigger', 'teleport'].includes(it.o.kind)) return toast(`${it.name} is not a volume or teleport pad (Esc cancels)`, true);
  toast(`Picked ${it.name}`); objPick.done(it.id);
}, true);
addEventListener('keydown', e => { if (objPick && e.key === 'Escape') { e.preventDefault(); e.stopImmediatePropagation(); toast('Pick cancelled'); objPick.done(null); } }, true);
const logic = createLogic({host: viewport, project: () => project, items: () => items, byId: id => byId.get(id), snapshot, changed, toast, esc, classOf, select, pickObject, focusObject, askText});
function showWorkspace(ws, owner) {
  $('workspaces').querySelectorAll('button').forEach(x => x.classList.toggle('on', x.dataset.ws === ws)); viewport.querySelector('.ws')?.remove();
  if (ws === 'logic') { keys.clear(); logic.show(owner !== undefined ? owner : logic.active() ? undefined : selected ? selected.o : 'stage'); return; }
  logic.hide();
  if (ws === 'play') return renderPlayWs();
  if (ws === 'assets') return renderAssetsWs();
}
$('workspaces').querySelectorAll('button').forEach(b => b.onclick = () => showWorkspace(b.dataset.ws));
const mb = b => (b / 1048576).toFixed(2) + ' MB';
// Play workspace: the last build's report (this window), the last playtest, and a memory meter from 
// capture/memory.json (free heap at "player active": 24 MB GameCube layout, 64 MB editor runtime #60)
let memoryInfo = null, lastRun = null;
async function renderPlayWs() {
  if (!memoryInfo) memoryInfo = await fetch('../capture/memory.json').then(r => r.ok ? r.json() : {}).catch(() => ({}));
  const host = document.createElement('div'); host.className = 'ws wsPage'; viewport.append(host); const r = lastReport, st = project?.stage;
  const m24 = memoryInfo.stages?.[st], r64 = memoryInfo.runtime_64mb || {}, free64 = r64[`${st}_free_at_start`] ?? (m24 ? m24.free + 60661600 - (m24.heap_size || 18718560) : null), copied = r?.copiedBytes || 0;
  const bar = (used, free) => free ? `<div class="memBar"><span style="width:${Math.min(100, used / free * 100)}%"></span></div>` : '';
  host.innerHTML = `<div class="wsInner"><h2>Play</h2>
    <div class="wsBtns"><button class="ghost" id="wsBuild">Build only (check, no game)</button> <button class="play" id="wsPlay">▶ Playtest</button> <button class="ghost" id="wsGameView" title="Starts the private test game VISIBLE but MUTED and WITHOUT focus beside the editor, and streams this camera to it (~10 per second). The game does not follow it yet: until then it keeps its own camera">Game view (muted window + camera)</button></div>
    <section><h3>Last build</h3>${r ? `<p class="${r.ok ? 'okText' : 'badText'}">${r.ok ? 'Build OK' : 'Build failed'} · ${r.objects ?? '-'} objects · ${r.geometry ?? 0} authored meshes${r.path ? ` · <small>${esc(r.path)}</small>` : ''}</p>
      ${r.errors?.length ? `<h4>Errors</h4><ul>${r.errors.map(e => `<li>${esc(e)}</li>`).join('')}</ul>` : ''}
      ${r.warnings?.length ? `<h4>Warnings (${r.warnings.length})</h4><ul>${r.warnings.map(w => `<li>${esc(w)}</li>`).join('')}</ul>` : '<p class="muted">No warnings.</p>'}
      ${(r.dependencies || []).length ? `<h4>Copied from other stages</h4><ul>${r.dependencies.map(d => `<li><b>${esc(d.className || d.table || d.kind)}</b> from ${esc(stageName(d.sourceStage))}${d.files?.length ? ' · ' + d.files.length + ' files: ' + esc(d.files.slice(0, 6).join(', ')) + (d.files.length > 6 ? '…' : '') : ''}${d.types ? ' · types ' + esc(Object.entries(d.types).map(([a, b]) => a + '→' + b).join(', ')) : ''}</li>`).join('')}</ul><p class="muted">${mb(copied)} of files${r.copiedTextures?.length ? ` and ${r.copiedTextures.length} textures` : ''} copied.</p>` : ''}
      ${(r.setidPermitted || []).length ? `<h4>Classes allowed in this stage (setid.bin)</h4><ul>${r.setidPermitted.map(c => `<li><b>${esc(c.className)}</b> · ${c.objects} placed · from ${esc((c.sourceStages || []).map(stageName).join(', ') || '-')}</li>`).join('')}</ul>` : ''}`
      : '<p class="muted">No build in this window yet: Build only checks the project without starting the game.</p>'}</section>
    <section><details class="wsMore"><summary><h3>Memory <small class="muted">· advanced: will the stage fit?</small></h3></summary>${m24 || free64 ? `<p>Copied content <b>${mb(copied)}</b>${r ? '' : ' (build to measure)'}.</p>
      ${m24 ? `<p>24 MB layout (the original GameCube heap): ${mb(m24.free)} free at the start of ${esc(stageName(st))}, largest block ${mb(m24.largest_free)}.</p>${bar(copied, m24.largest_free)}` : ''}
      ${free64 ? `<p>64 MB editor runtime: about ${mb(free64)} free${r64[`${st}_free_at_start`] ? '' : ' (estimated: the 24 MB figure + the larger heap)'}.</p>${bar(copied, free64)}` : ''}
      <p class="muted">${esc(memoryInfo.caveat || '')}</p>` : `<p class="muted">No memory measurement for ${esc(stageName(st))} yet (capture/memory.json has ${Object.keys(memoryInfo.stages || {}).join(', ') || 'none'}).</p>`}</details></section>
    <section><h3>Start point</h3>${startCard()}</section>
    <section><h3>Last run</h3>${lastRun?.message ? `<p>${esc(lastRun.message)}</p>` : ''}<div id="ptLog"><p class="muted">Reading the playtest log…</p></div></section></div>`;
  host.querySelector('#wsBuild').onclick = async () => { await buildOnly(); $('report').hidden = true; showWorkspace('play'); };
  host.querySelector('#wsPlay').onclick = playtest;
  host.querySelector('#wsGameView').onclick = () => playtest({windowed: true});
  wireStartCard(host); loadPlayLog(host);
}
let logTimer = null;
async function loadPlayLog(host) {   // GET /api/playtest-log: the newest test game of this project; every 3 s while it runs
  clearTimeout(logTimer); const el = host.querySelector('#ptLog'); if (!el || !project) return;
  let j; try { j = await api('/api/playtest-log?id=' + encodeURIComponent(project.id)); } catch (e) { el.innerHTML = `<p class="muted">No playtest log (${esc(e.message)}).</p>`; return; }
  if (!host.isConnected) return;
  el.innerHTML = j.game ? `<p class="muted">${j.running ? '<b class="okText">game running</b>' : 'game not running'} · test game ${esc(j.game)}${j.file ? ' · ' + esc(j.file) : ''} <button class="ghost small" id="ptRefresh">Refresh</button></p>${j.log?.length ? `<pre class="log">${esc(j.log.join('\n'))}</pre>` : '<p class="muted">The log is empty so far.</p>'}`
    : (lastRun?.log ? `<pre class="log">${esc(lastRun.log)}</pre>` : '<p class="muted">No playtest of this project yet.</p>');
  el.querySelector('#ptRefresh')?.addEventListener('click', () => loadPlayLog(host));
  const pre = el.querySelector('pre'); if (pre) pre.scrollTop = pre.scrollHeight;
  if (j.running) logTimer = setTimeout(() => loadPlayLog(host), 3000);
}
// ---- Play from here (phase 8): project.playtestStart = {position [x,y,z], rotation [0, yaw deg, 0], enabled} = where a
// playtest puts Shadow when he becomes active (the sidecar's start teleport, like a Start override). The server does not
// read the field yet (NOTE to ): until then playtests start at the stage's own start. Set from the viewport
// (right-click → Start playtest here), the palette or the Play tab; drawn as a green arrow.
const startMark = new THREE.Group(); scene.add(startMark);
function drawStartMark() {
  clear(startMark); const ps = project?.playtestStart; if (!ps?.enabled || !ps.position?.every(Number.isFinite)) return;
  const mat = new THREE.MeshBasicMaterial({color: 0x34d399}), cone = new THREE.Mesh(new THREE.ConeGeometry(14, 40, 16), mat); cone.rotation.x = Math.PI; cone.position.y = 45;
  const ring = new THREE.Mesh(new THREE.TorusGeometry(26, 2.5, 8, 32), mat); ring.rotation.x = Math.PI / 2; ring.position.y = 2;
  const dir = new THREE.Mesh(new THREE.BoxGeometry(4, 4, 30), mat); dir.position.set(0, 2, 30);
  const g = new THREE.Group(); g.add(cone, ring, dir); g.position.set(...ps.position); g.rotation.y = THREE.MathUtils.degToRad(ps.rotation?.[1] || 0); startMark.add(g);
}
function setPlayStart(pos, why = '') {
  if (!project || !pos) return; const d = orbit.target.clone().sub(camera.position); const yaw = THREE.MathUtils.radToDeg(Math.atan2(d.x, d.z));
  snapshot(); project.playtestStart = {position: pos.toArray().map(round2), rotation: [0, round2(yaw), 0], enabled: true}; changed(); drawStartMark(); renderPlayAgain();
  if (!setPlayStart.quiet) toast(`Playtest start set${why} at ${project.playtestStart.position.map(Math.round).join(', ')}: the next Playtest starts there`);
}
function clearPlayStart() { if (!project?.playtestStart) return; snapshot(); delete project.playtestStart; changed(); drawStartMark(); renderPlayAgain(); toast('Playtest start cleared: playtests start at the stage\'s own start'); }
const centrePoint = () => { const r = renderer.domElement.getBoundingClientRect(); return surfacePoint(r.left + r.width / 2, r.top + r.height / 2); };
function startCard() {
  const ps = project?.playtestStart;
  return `${ps?.enabled ? `<p>Playtests start at <b>${ps.position.map(Math.round).join(', ')}</b> (facing ${Math.round(ps.rotation?.[1] || 0)}°).</p>` : '<p class="muted">Playtests start at the stage\'s own start.</p>'}
    <div class="wsBtns"><button class="ghost" id="psHere" title="The surface under the centre of the 3D view">Start here (screen centre)</button>${selected ? ' <button class="ghost" id="psSel">Start at the selection</button>' : ''}${ps ? ' <button class="ghost" id="psFly">Fly there</button> <button class="ghost" id="psClear">Clear</button>' : ''}</div>
    <p class="muted">Also: right-click a spot in the 3D view → Start playtest here. Only Playtest starts there; the exported stage always starts at its Player start.</p>`;
}
function wireStartCard(host) {
  host.querySelector('#psHere')?.addEventListener('click', () => { showWorkspace('layout'); setTimeout(() => { setPlayStart(centrePoint()); showWorkspace('play'); }, 30); });
  host.querySelector('#psSel')?.addEventListener('click', () => setPlayStart(new THREE.Vector3(...selected.at), ' at ' + selected.name));
  host.querySelector('#psFly')?.addEventListener('click', () => { showWorkspace('layout'); flyTo(project.playtestStart.position, 400); });
  host.querySelector('#psClear')?.addEventListener('click', clearPlayStart);
}
// viewport right-click (no drag: a drag pans) → a small menu
let ctxDown = null; const ctxMenu = Object.assign(document.createElement('div'), {className: 'ctxMenu', hidden: true}); document.body.append(ctxMenu);
renderer.domElement.addEventListener('pointerdown', e => { if (e.button === 2) ctxDown = [e.clientX, e.clientY]; ctxMenu.hidden = true; }, true);
renderer.domElement.addEventListener('contextmenu', e => {
  e.preventDefault(); if (!project || !ctxDown || Math.hypot(e.clientX - ctxDown[0], e.clientY - ctxDown[1]) > 4) return;
  const pt = surfacePoint(e.clientX, e.clientY);
  ctxMenu.innerHTML = `<button data-c="start">▶ Start playtest here</button>${project.playtestStart ? '<button data-c="clear">Clear playtest start</button>' : ''}<button data-c="pstart" title="Moves the player start here (or places one when the project has none); written into the build">Place start here</button>${hasStart() ? '<button data-c="rstart" title="Deletes the player start (Ctrl Z brings it back)">Remove start</button>' : ''}<button data-c="fly">Fly here</button>`;
  Object.assign(ctxMenu.style, {left: e.clientX + 'px', top: e.clientY + 'px'}); ctxMenu.hidden = false;
  ctxMenu.onclick = ev => { const c = ev.target.closest('[data-c]')?.dataset.c; ctxMenu.hidden = true; if (c === 'start') setPlayStart(pt); else if (c === 'clear') clearPlayStart(); else if (c === 'pstart') moveMarker(startId(), pt); else if (c === 'rstart') removeStart(); else if (c === 'fly') flyTo(pt.toArray(), 300); };
});
window.addEventListener('keydown', e => { if (e.key === 'Escape') ctxMenu.hidden = true; }, true);
window.addEventListener('pointerdown', e => { if (!ctxMenu.contains(e.target)) ctxMenu.hidden = true; }, true);
const renderPlayAgain = () => { if ($('workspaces').querySelector('.on')?.dataset.ws === 'play') showWorkspace('play'); };
// Assets workspace: the stage's models and textures with "where used" (placements whose 3D node draws them)
let assetTab = 'models', assetQ = '';
function renderAssetsWs() {
  const host = document.createElement('div'); host.className = 'ws wsPage'; viewport.append(host);
  const used = new Map(), texUsed = new Map();
  for (const [it, n] of nodeOf) for (const m of modelNamesOf(n)) { if (!used.has(m.name)) used.set(m.name, []); used.get(m.name).push(it); for (const t of m.materials || []) if (t.texture) { if (!texUsed.has(t.texture)) texUsed.set(t.texture, {t, items: new Set()}); texUsed.get(t.texture).items.add(it); } }
  for (const mt of project?.materials || []) if (mt.texture && !texUsed.has(mt.texture)) texUsed.set(mt.texture, {t: mt, items: new Set(), art: true}); else if (mt.texture) texUsed.get(mt.texture).art = true;
  const q = assetQ.toLowerCase(), list = (models?.models || []).filter(m => (m.name + ' ' + m.archive).toLowerCase().includes(q));
  host.innerHTML = `<div class="wsInner"><h2>Assets</h2><div class="seg"><button data-at="models" class="${assetTab === 'models' ? 'on' : ''}">Models ${models?.models?.length || 0}</button><button data-at="textures" class="${assetTab === 'textures' ? 'on' : ''}">Textures ${texUsed.size}</button><button data-at="sounds" class="${assetTab === 'sounds' ? 'on' : ''}">Sounds</button></div>
    <div class="searchBox"><input id="assetQ" placeholder="Search" value="${esc(assetQ)}"></div><div id="assetList" class="assetList"></div></div>`;
  const out = host.querySelector('#assetList');
  if (assetTab === 'models') out.innerHTML = list.slice(0, 400).map(m => { const u = used.get(m.name) || []; return `<div class="assetRow"><b>${esc(m.name)}</b><small>${esc(m.archive)}</small><span class="muted">${u.length ? `${u.length} placement${u.length > 1 ? 's' : ''}` : 'not used'}</span>${u.length ? `<button class="ghost small" data-asel="${esc(u[0].id)}">Show</button>` : ''}</div>`; }).join('') || '<p class="muted">No models (still loading?).</p>';
  else if (assetTab === 'textures') out.innerHTML = [...texUsed.entries()].filter(([name]) => name.toLowerCase().includes(q)).sort((a, b) => b[1].items.size - a[1].items.size).slice(0, 400).map(([name, v]) => { const ov = project.textureOverrides?.[name];
      return `<div class="assetRow"><span class="sw" style="background-image:url(/asset/${esc(ov?.asset || v.t.asset || '')})"></span><b>${esc(name)}</b>${ov ? '<small class="okText">replaced</small>' : ''}<span class="muted">${v.items.size ? v.items.size + ' placement' + (v.items.size > 1 ? 's' : '') : ''}${v.art ? (v.items.size ? ' · ' : '') + 'stage art' : ''}</span><button class="ghost small" data-texopen="${esc(v.t.asset || '')}">Open</button><button class="ghost small" data-texrep="${esc(name)}">Replace…</button>${ov ? `<button class="ghost small" data-texback="${esc(name)}">Undo</button>` : ''}</div>`; }).join('');
  else {   // sounds: the ones imported into this project (customAssets sfx / music); the stage's own banks have no index
    const music = project.stageSettings?.music?.asset, snd = (project.customAssets || []).filter(a => ['sfx', 'music'].includes(a.kind) && (a.name + ' ' + a.kind).toLowerCase().includes(q));
    out.innerHTML = `<div class="wsBtns"><button class="ghost small" data-sndadd="sfx">Import sound effect…</button> <button class="ghost small" data-sndadd="music">Import music…</button> <small class="muted">sounds WAV / OGG / ADX · music also MP3 / M4A / FLAC</small></div>`
      + (snd.map(a => `<div class="assetRow"><b>${esc(a.name)}</b><small>${esc(a.kind === 'music' ? 'music' : 'sound effect')} · ${esc(a.format || '')} · ${mb(a.bytes || 0)}</small><span class="muted">${music === a.id ? 'stage music (Stage settings)' : 'not used yet'}</span><button class="ghost small" data-sndrm="${esc(a.id)}">Remove</button></div>`).join('') || '<p class="muted">No sounds imported into this project yet.</p>')
      + '<p class="muted">Imported music is chosen in Stage settings → Music. The stage\'s own sound banks are not listed: there is no list of them yet.</p>';
    out.querySelectorAll('[data-sndadd]').forEach(b => b.onclick = () => { sndPicker.dataset.kind = b.dataset.sndadd; sndPicker.accept = b.dataset.sndadd === 'music' ? '.wav,.mp3,.m4a,.aac,.flac,.wma,.ogg,.adx' : '.wav,.ogg,.adx'; sndPicker.click(); });
    out.querySelectorAll('[data-sndrm]').forEach(b => b.onclick = () => { snapshot(); project.customAssets = project.customAssets.filter(a => a.id !== b.dataset.sndrm); if (music === b.dataset.sndrm) project.stageSettings.music.asset = ''; changed(); showWorkspace('assets'); });
  }
  host.querySelectorAll('[data-at]').forEach(b => b.onclick = () => { assetTab = b.dataset.at; showWorkspace('assets'); });
  const qi = host.querySelector('#assetQ'); qi.oninput = () => { assetQ = qi.value; const pos = qi.selectionStart; showWorkspace('assets'); const n = viewport.querySelector('#assetQ'); n.focus(); n.setSelectionRange(pos, pos); };
  host.querySelectorAll('[data-asel]').forEach(b => b.onclick = () => { showWorkspace('layout'); select(byId.get(b.dataset.asel), true); });
  host.querySelectorAll('[data-texopen]').forEach(b => b.onclick = () => openTextureFile(b.dataset.texopen));
  host.querySelectorAll('[data-texrep]').forEach(b => b.onclick = () => { texFor = b.dataset.texrep; texPicker.click(); });
  host.querySelectorAll('[data-texback]').forEach(b => b.onclick = () => { snapshot(); delete project.textureOverrides[b.dataset.texback]; refreshTextures(); changed(); showWorkspace('assets'); });
}

const sndPicker = Object.assign(document.createElement('input'), {type: 'file', accept: '.wav,.ogg,.adx', hidden: true, id: 'sndPicker'}); document.body.append(sndPicker);
sndPicker.onchange = async () => {
  const f = sndPicker.files[0]; if (!f || !project) return;
  try {
    const bytes = new Uint8Array(await f.arrayBuffer()); let bin = ''; for (let i = 0; i < bytes.length; i += 16384) bin += String.fromCharCode(...bytes.subarray(i, i + 16384));
    const a = await api('/api/upload', {project: project.id, name: f.name, kind: sndPicker.dataset.kind || 'sfx', data: btoa(bin)});
    snapshot(); (project.customAssets ||= []).push(a); changed(); toast(`${f.name} imported (${mb(a.bytes || bytes.length)})`); if ($('workspaces').querySelector('.on')?.dataset.ws === 'assets') showWorkspace('assets');
  } catch (e) { toast(e.message, true); } finally { sndPicker.value = ''; }
};

// ---- projects, save, playtest ----------------------------------------------------------------------------------------
function when(t) { const d = new Date(t * 1000); return d.toLocaleDateString() + ' ' + d.toLocaleTimeString([], {hour: '2-digit', minute: '2-digit'}); }
function stageName(id) { return config.stages.find(s => s.id === id)?.name || (id == null ? 'New stage' : 'Stage ' + id); }
// start screen: your projects (edits + custom stages) and the game's own stages (open a fresh copy to mess about)
let startTab = 'projects', startQuery = '';
async function refreshConfig() { try { const c = await api('/api/config'); config.projects = c.projects; } catch {} }   // the project list after a new / imported project
// B3: New blank stage (POST /api/blank-stage, the template) and Import (.shadowstage -> POST /api/package/import)
async function newBlankStage() {
  if (!(await leaveGuard('make a new stage'))) return;
  const name = await askText('New blank stage', 'A small ready arena: floor, walls, start, rings, checkpoints, goal ring and a few enemies. The name is how you find it in Your projects.', 'My stage'); if (!name) return;
  toast(`Making "${name}"…`); try { const p = await api('/api/blank-stage', {name: name.slice(0, 80), populated: true}); await refreshConfig(); await openProject(p.id); showPanel('settings'); toast(`Made "${p.name}": a ready arena. Stage settings > Custom stage sets the description, cover and goals`); }
  catch (e) { toast(e.message, true); }
}
const pkgPicker = Object.assign(document.createElement('input'), {type: 'file', accept: '.shadowstage,.zip', hidden: true}); document.body.append(pkgPicker);
pkgPicker.onchange = async () => {
  const f = pkgPicker.files[0]; if (!f) return;
  try {
    const bytes = new Uint8Array(await f.arrayBuffer()); let bin = ''; for (let i = 0; i < bytes.length; i += 16384) bin += String.fromCharCode(...bytes.slice(i, i + 16384));
    const r = await api('/api/package/import', {data: btoa(bin)}); await refreshConfig(); toast(`Imported "${r.name}" as a new project: build it to play`); if (await leaveGuard('open the imported stage')) await openProject(r.id);
  } catch (e) { toast(e.message, true); } finally { pkgPicker.value = ''; }
};
function showProjects() {
  $('emptyState').hidden = false;
  const named = config.stages.filter(st => !/^stg\d+$/i.test(st.name)), other = config.stages.filter(st => /^stg\d+$/i.test(st.name));
  const edits = id => config.projects.filter(p => p.stage === id);
  const q = startQuery.toLowerCase();
  const projRows = config.projects.filter(p => (p.name + ' ' + stageName(p.stage)).toLowerCase().includes(q));
  const stageCard = st => { const n = edits(st.id).length; return `<button class="proj stage" data-stage="${st.id}"><span><b>${esc(st.name)}</b><br><small>stg${String(st.id).padStart(4, '0')}</small></span>${n ? `<small class="badge">${n} edit${n > 1 ? 's' : ''}</small>` : '<small>open a fresh copy →</small>'}</button>`; };
  const stageRows = list => list.filter(st => (st.name + ' ' + st.id).toLowerCase().includes(q));
  $('emptyList').innerHTML = `${project ? `<button class="proj back" data-back="1" style="margin-bottom:10px"><span><b>← Back to ${esc(project.name)}</b><br><small>Esc also closes this list</small></span></button>` : ''}<div class="startTabs"><button data-t="projects" class="${startTab === 'projects' ? 'on' : ''}">Your projects <small>${config.projects.length}</small></button><button data-t="stages" class="${startTab === 'stages' ? 'on' : ''}">Game stages <small>${named.length}</small></button>
     <div class="searchBox"><svg viewBox="0 0 24 24"><circle cx="11" cy="11" r="6"/><path d="M20 20l-4-4"/></svg><input id="startQ" placeholder="Search" value="${esc(startQuery)}"></div></div>
   <div class="startList">${startTab === 'projects'
      ? `<div class="newRow"><button class="proj newBlank" id="newBlank"><span><b>＋ New blank stage</b><br><small>a small arena made from scratch: floor, walls, start, rings, checkpoints, goal ring, 2 GUN + 2 Black Arms</small></span></button><button class="proj newBlank" id="importPkg"><span><b>Import a stage…</b><br><small>a .shadowstage file someone shared: it becomes a new project</small></span></button></div>` + (projRows.map(p => `<button class="proj" data-id="${esc(p.id)}"><span><b>${esc(p.name)}</b><br><small>${esc(stageName(p.stage))}</small></span><small>saved ${esc(when(p.saved))}</small></button>`).join('') || '<p class="muted">No projects match. Open one from Game stages to start.</p>')
      : `<div class="startGroup">Story stages</div><div class="stageGrid">${stageRows(named).map(stageCard).join('')}</div>${stageRows(other).length ? `<div class="startGroup">Other stage files (tests, bosses, extras)</div><div class="stageGrid">${stageRows(other).map(stageCard).join('')}</div>` : ''}<p class="muted">A game stage opens as a fresh copy: nothing is saved (and the game files never change) until you press Save, which keeps it under Your projects.</p>`}</div>`;
  $('emptyList').querySelectorAll('[data-t]').forEach(b => b.onclick = () => { startTab = b.dataset.t; showProjects(); });
  $('emptyList').querySelectorAll('[data-back]').forEach(b => b.onclick = () => { $('emptyState').hidden = true; });
  const sq = $('startQ'); sq.oninput = () => { startQuery = sq.value; const pos = sq.selectionStart; showProjects(); const n = $('startQ'); n.focus(); n.setSelectionRange(pos, pos); };
  $('emptyList').querySelectorAll('[data-id]').forEach(b => b.onclick = () => openProject(b.dataset.id));
  $('newBlank')?.addEventListener('click', newBlankStage); $('importPkg')?.addEventListener('click', () => pkgPicker.click());
  $('emptyList').querySelectorAll('[data-stage]').forEach(b => b.onclick = () => openStage(+b.dataset.stage));
}
// ---- #75 nothing is written unless the user saves (or turns Auto-save on for a project that is already saved):
// a stage with edits asks Continue / Fresh copy / Cancel (Esc used to open a fresh copy, whose first Save, Build or
// Playtest then made one more project); leaving with changes asks Save / Discard / Stay; a never-saved fresh copy asks
// before Build / Playtest write it; Auto-save (top bar, per browser, default off) saves in place after 8 s of quiet.
// UX pass: a name / text in the app's own dialog (window.prompt is blocked in some embedded browsers and looks foreign).
// choices: a list to pick from instead of typing. -> the text (trimmed), or null for Cancel / Esc
function askText(title, text, value = '', choices = null) {
  let box = $('chooseBox'); if (!box) { box = Object.assign(document.createElement('div'), {className: 'modalWrap', id: 'chooseBox'}); document.body.append(box); }
  const field = choices ? `<select class="askIn">${choices.map(c => `<option${c === value ? ' selected' : ''}>${esc(c)}</option>`).join('')}</select>` : `<input class="askIn" type="text" maxlength="80" value="${esc(value)}">`;
  box.innerHTML = `<div class="modalBox"><h3 class="plain">${esc(title)}</h3>${text ? `<p>${text}</p>` : ''}<p>${field}</p><div class="modalBtns"><button class="play" data-v="ok">OK</button><button class="ghost" data-v="cancel">Cancel</button></div></div>`; box.hidden = false;
  const inp = box.querySelector('.askIn'); inp.focus(); inp.select?.();
  return new Promise(done => {
    const finish = ok => { const v = inp.value.trim(); box.hidden = true; removeEventListener('keydown', key, true); done(ok ? v : null); };
    const key = e => { if (e.key === 'Escape') { e.preventDefault(); e.stopPropagation(); finish(false); } else if (e.key === 'Enter') { e.preventDefault(); e.stopPropagation(); finish(true); } else e.stopPropagation(); };
    addEventListener('keydown', key, true); box.querySelector('[data-v="ok"]').onclick = () => finish(true); box.querySelector('[data-v="cancel"]').onclick = () => finish(false);
  });
}
function choose(title, text, buttons) {   // -> the chosen button's value (Esc = the last one)
  let box = $('chooseBox'); if (!box) { box = Object.assign(document.createElement('div'), {className: 'modalWrap', id: 'chooseBox'}); document.body.append(box); }
  box.innerHTML = `<div class="modalBox"><h3 class="plain">${esc(title)}</h3><p>${text}</p><div class="modalBtns">${buttons.map(([v, l], i) => `<button class="${i ? 'ghost' : 'play'}" data-v="${esc(v)}">${esc(l)}</button>`).join('')}</div></div>`; box.hidden = false;
  return new Promise(done => {
    const finish = v => { box.hidden = true; removeEventListener('keydown', key, true); done(v); };
    const key = e => { if (e.key === 'Escape') { e.preventDefault(); e.stopPropagation(); finish(buttons.at(-1)[0]); } };
    addEventListener('keydown', key, true); box.querySelectorAll('[data-v]').forEach(b => b.onclick = () => finish(b.dataset.v)); box.querySelector('[data-v]')?.focus();
  });
}
const isFresh = () => !!project && !config.projects.some(p => p.id === project.id) && diskSaved == null;
// ---- #96 a reload comes back to the same project: localStorage 'ui2.session' = {id, stage, fresh, camera, selected, ws,
// panel} (every 2 s and on pagehide); IndexedDB ui2 / drafts / 'current' = the whole project while it has unsaved
// changes (1.5 s after a change; cleared by Save / Discard). On load: a draft -> 'Restore unsaved changes?' (no question
// after Ctrl R > Keep unsaved); else the remembered project (or a fresh copy of the remembered stage) opens directly; the
// stage list only when nothing is remembered or on demand. A fresh copy comes back from its draft, never from /api/stage
// (the server gives every fresh copy of a stage the same id, #95). Chips, panel width, tile size stay per browser as before.
const draftDb = (() => { let p = null; return () => p ||= new Promise((ok, no) => { const r = indexedDB.open('ui2', 1); r.onupgradeneeded = () => r.result.createObjectStore('drafts'); r.onsuccess = () => ok(r.result); r.onerror = () => no(r.error); }); })();
async function draftPut(v) { const db = await draftDb(); return new Promise((ok, no) => { const t = db.transaction('drafts', 'readwrite'), st = t.objectStore('drafts'); if (v == null) st.delete('current'); else st.put(v, 'current'); t.oncomplete = () => ok(); t.onerror = () => no(t.error); }); }
async function draftGet() { const db = await draftDb(); return new Promise((ok, no) => { const r = db.transaction('drafts').objectStore('drafts').get('current'); r.onsuccess = () => ok(r.result || null); r.onerror = () => no(r.error); }); }
let draftTimer = null;
function scheduleDraft() { clearTimeout(draftTimer); draftTimer = setTimeout(writeDraftNow, 1500); }
async function writeDraftNow() {
  clearTimeout(draftTimer); if (!project) return;
  try { if (!dirty) return await draftPut(null); await draftPut({id: project.id, stage: project.stage, fresh: isFresh(), name: project.name, at: Date.now(), project: JSON.parse(JSON.stringify(serializable()))}); } catch (e) { lifeEvent('draft-failed', e.message); }
}
function sessionWrite() {
  if (!project || !$('emptyState').hidden) return;
  try { localStorage.setItem('ui2.session', JSON.stringify({id: project.id, stage: project.stage, fresh: isFresh(), camera: {position: camera.position.toArray(), target: orbit.target.toArray()}, selected: selected?.id || null, ws: $('workspaces').querySelector('.on')?.dataset.ws || 'layout', panel, at: Date.now()})); } catch {}
}
const sessionRead = () => { try { return JSON.parse(localStorage.getItem('ui2.session') || 'null'); } catch { return null; } };
setInterval(sessionWrite, 2000); addEventListener('pagehide', () => { sessionWrite(); if (dirty) writeDraftNow(); });
function applySessionView(s) {
  if (!s) return;
  if (s.camera && !badCamera(s.camera)) { fly = null; camera.position.fromArray(s.camera.position); orbit.target.fromArray(s.camera.target); orbit.update(); }
  if (s.selected && byId.get(s.selected)) select(byId.get(s.selected), false);
  if (s.ws && s.ws !== 'layout') showWorkspace(s.ws); if (s.panel && s.panel !== panel) showPanel(s.panel);
}
async function bootRestore(urlId, {auto = false} = {}) {
  if (urlId) { await openProject(urlId); const q = new URLSearchParams(location.search), pn = q.get('panel'), sel = q.get('select');   // ?panel=settings&select=<id>: links from the launcher and the screenshot tool
    if (pn && $('rail').querySelector(`[data-panel="${CSS.escape(pn)}"]`)) showPanel(pn); if (sel) { const it = items.find(x => x.id === sel || x.name === sel); if (it) select(it); } const ws = q.get('ws'); if (ws) document.querySelector(`#workspaces [data-ws="${CSS.escape(ws)}"]`)?.click(); return; }
  const s = sessionRead(); let d = null; try { d = await draftGet(); } catch {}
  let keep = auto; try { keep ||= sessionStorage.getItem('ui2.keepDraft') === '1'; sessionStorage.removeItem('ui2.keepDraft'); } catch {}
  if (d?.project) {
    const v = keep ? 'restore' : await choose('Restore unsaved changes?', `"${esc(d.name)}" (${esc(stageName(d.stage))}) has changes from ${new Date(d.at).toLocaleString()} that were not saved${d.fresh ? ' - a fresh copy, never saved' : ''}.`, [['restore', 'Restore them'], ['discard', 'Discard them']]);
    if (v === 'restore') {
      showLoaded(d.project, d.fresh ? 'Restored unsaved copy · not saved yet' : 'Restored unsaved changes'); if (!d.fresh) diskSaved = await diskTime(d.id).catch(() => null);
      dirty = true; $('saveState').textContent = 'Unsaved changes (restored)'; renderUndo(); applySessionView(s && s.id === d.id ? s : null); toast(`Restored the unsaved changes of "${d.name}"`); return 'draft';
    }
    await draftPut(null).catch(() => {});
  }
  if (s?.id && !s.fresh && config.projects.some(p => p.id === s.id)) { await openProject(s.id); if (project?.id === s.id) applySessionView(s); return 'project'; }
  if (s?.fresh && s.stage != null && config.stages.some(st => st.id === s.stage)) {   // a fresh copy without changes: the same stage again, no chooser
    try { const p = await api('/api/stage?id=' + s.stage); showLoaded(p, 'Fresh copy · not saved yet'); applySessionView(s); return 'fresh'; } catch (e) { toast(e.message, true); }
  }
  showProjects(); return 'list';
}
async function leaveGuard(what = 'leave this project') {   // true = go ahead
  if (!dirty || !project) return true;
  const v = await choose('Unsaved changes', `"${esc(project.name)}" has changes that are not saved. Save them before you ${esc(what)}?${isFresh() ? ' <br><small>It is a fresh copy: Save makes it a new project.</small>' : ''}`, [['save', 'Save'], ['discard', 'Discard changes'], ['stay', 'Stay']]);
  if (v === 'stay') return false; if (v === 'discard') { dirty = false; clearTimeout(draftTimer); draftPut(null).catch(() => {}); return true; }
  await save(); return !dirty;
}
let autoSaveOn = (() => { try { return localStorage.getItem('ui2.autosave') === '1'; } catch { return false; } })(), autoTimer = null;
function scheduleAutoSave() {
  clearTimeout(autoTimer); if (!autoSaveOn || !project || isFresh()) return;
  autoTimer = setTimeout(async () => { if (!dirty || !autoSaveOn || gizmo.dragging) return scheduleAutoSave(); lifeEvent('autosave', project.id); await save(true); }, 8000);
}
{ const sv = $('save'); const lab = Object.assign(document.createElement('label'), {className: 'autoSave', title: 'Auto-save: saves this project in place 8 s after your last change (never a new copy; a fresh copy needs one Save first). Off by default, remembered in this browser'});
  lab.innerHTML = `<input type="checkbox" id="autoSave" ${autoSaveOn ? 'checked' : ''}> Auto-save`; sv.before(lab);
  $('autoSave').onchange = e => { autoSaveOn = e.target.checked; try { localStorage.setItem('ui2.autosave', autoSaveOn ? '1' : '0'); } catch {} if (autoSaveOn) scheduleAutoSave(); else clearTimeout(autoTimer); toast(autoSaveOn ? (isFresh() ? 'Auto-save on: it starts after the first Save of this fresh copy' : 'Auto-save on: changes save in place after 8 s') : 'Auto-save off: only Save writes the project'); }; }
setInterval(() => {   // the dirty marker: a dot on Save and in the window title
  const base = project ? `${project.name} · ${APP_NAME}` : APP_NAME, t = (dirty ? '• ' : '') + base; if (document.title !== t) document.title = t;
  $('save')?.classList.toggle('dirty', !!dirty);
}, 400);
addEventListener('keydown', async e => {   // Ctrl R / F5 with changes: Save / Discard / Stay instead of the browser's plain prompt
  if (!dirty || !project || !((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'r' || e.key === 'F5')) return;
  e.preventDefault(); e.stopPropagation();
  const v = await choose('Unsaved changes', `"${esc(project.name)}" has changes that are not saved. Reload: save them, keep them unsaved (restored right after the reload) or discard them?`, [['keep', 'Keep unsaved, reload'], ['save', 'Save, reload'], ['discard', 'Discard, reload'], ['stay', 'Stay']]);
  if (v === 'stay') return; if (v === 'save') { await save(); if (dirty) return; }
  if (v === 'keep') { sessionWrite(); await writeDraftNow(); try { sessionStorage.setItem('ui2.keepDraft', '1'); } catch {} }
  if (v === 'discard') { clearTimeout(draftTimer); await draftPut(null).catch(() => {}); }
  dirty = false; location.reload();
}, true);
async function openProject(id) {
  if (!await leaveGuard('open another project')) return;
  toast('Loading project…');
  let p, t = null; try { p = await api('/api/project?id=' + id); t = await diskTime(id).catch(() => null); } catch (e) { toast(e.message, true); return; }
  showLoaded(p, 'Opened'); diskSaved = t; warnedFor = null;
}
async function openStage(id) {
  const mine = config.projects.filter(p => p.stage === id);
  if (mine.length) {
    const v = await choose(stageName(id), `You already have ${mine.length} saved edit${mine.length > 1 ? 's' : ''} of ${esc(stageName(id))}. Continue "${esc(mine[0].name)}" (saved ${esc(when(mine[0].saved))}), or open a fresh copy of the game stage? A fresh copy is not saved until you save it (then it becomes another project).`, [['continue', `Continue "${mine[0].name}"`], ['fresh', 'Fresh copy'], ['cancel', 'Cancel']]);
    if (v === 'cancel') return; if (v === 'continue') return openProject(mine[0].id);
  }
  if (!await leaveGuard('open another stage')) return;
  toast(`Reading ${stageName(id)} from the game files…`);
  let p; try { p = await api('/api/stage?id=' + id); } catch (e) { toast(e.message, true); return; }
  showLoaded(p, 'Fresh copy · not saved yet');
}
function showLoaded(p, state) {
  lifeEvent('open', `${p.id} ${state}`);
  project = p; normalizeLook(p); dirty = false; diskSaved = null; selected = null; undoStack.length = redoStack.length = 0; $('emptyState').hidden = true;
  $('projectTitle').textContent = project.name; $('projectMeta').textContent = project.mode === 'new' ? 'New stage' : stageName(project.stage); $('saveState').textContent = state;
  logic.reset(); if (logic.active()) setTimeout(() => logic.show('stage'), 0);
  if (gameView.on && gameView.pid !== project.id) gameViewOff();
  renderUndo(); models = null; R.drawNotes = {}; atmosFor = null; atmos = null; shadowCount = -1; lightPreview.key = null; if (gameLook.on) setTimeout(loadAtmos, 0); R.stage = project.stage; for (const e of project.surfaceCollision || []) { delete e._found; if (typeof e.source === 'string') { e.source = {kind: e.source, ref: e.ref, ...(e.model ? {model: e.model, mesh: e.mesh} : {}), faces: e.faces, mode: e.mode}; for (const k of ['ref', 'model', 'mesh', 'faces', 'mode']) delete e[k]; e.triangles ||= []; } }   /* the first #73 build's shape -> the current one */ R.materials = project.materials || []; project.paths ||= []; pathPoint = null; project.geometry ||= []; hiddenArt.clear(); for (const k of project.editor?.hiddenArt || []) hiddenArt.add(k); showStandins = !!project.editor?.showStandins; visMode = project.editor?.visibility?.mode || 'all'; visDist = project.editor?.visibility?.distance || 12000; visChunks = null; updateVisibility.key = undefined; if (drawing) cancelDraw(); faceMode = false; multi.clear();
  query = ''; hiddenLayers.clear(); hiddenZones.clear(); for (const c of [...hiddenCats]) if (c !== 'volumes') hiddenCats.delete(c);   // #63: no filter carries over from the last project
  computeZones(); computeSpine(); buildItems();
  try { buildArt(); } catch (e) { lifeEvent('art-failed', e.message); setTimeout(() => toast(`Stage art could not be drawn (${e.message}); placements still load`, true), 60); }
  buildNodes(); drawStartMark(); buildMinimap(); renderLens(); renderPanel(); renderInspector(); renderSpeed(); resize(); loadModels(project.stage);
  const cam = project.editor?.camera, bad = cam ? badCamera(cam) : null;
  if (cam && !bad) { camera.position.fromArray(cam.position); orbit.target.fromArray(cam.target); orbit.update(); }
  else { if (bad) lifeEvent('camera-reset', bad); flyTo((spine[0] || new THREE.Vector3(...(items[0]?.at || [0, 0, 0]))).toArray(), 600); }
  if (bad) { setTimeout(() => toast(`The saved camera was unusable (${bad}) - flew to the start instead`), 1500); }
  toast(`${items.length} placements · ${zones.length} zones · ${spine.cps.length} checkpoints`);
}
const modelCache = new Map();
async function loadModels(stage) {
  if (stage == null) return;
  try {
    let data = modelCache.get(stage); if (!data) { toast('Loading object and enemy models…'); data = await api('/api/models?id=' + stage); modelCache.set(stage, data); }
    if (project?.stage !== stage) return;
    R.models = data.models; R.artist = data.artist || {}; R.recordModels = data.recordModels || {}; R.classModels = data.classModels || {}; models = data; buildNodes(); if (panel === 'add') renderPanel(); if ((project.surfaceCollision || []).length) renderInspector();   // tile thumbnails need the models
    toast(`${nodeOf.size} placements with their models · ${items.length - nodeOf.size} shown as markers`);
  } catch (e) { lifeEvent('models-failed', e.message); try { if (project?.stage === stage) { models = null; buildNodes(); } } catch {} toast(`Object and enemy models failed to load (${e.message}) - they are shown as markers`, true); }
}
// Saving keeps the camera with the project (project.editor.camera, the same keys as classic) and never silently
// replaces a newer file: diskSaved = project.json's time when this window opened or last saved it; if the file is newer
// (classic, another window or a playtest build saved it), ask: reload that version, overwrite it, or cancel.
let diskSaved = null, warnedFor = null;
async function diskTime(id) { const c = await api('/api/config'); config.projects = c.projects; return c.projects.find(p => p.id === id)?.saved ?? null; }
function serializable() { if ((project.surfaceCollision || []).length) buildSurfColl(); project.editor = {...project.editor, camera: {position: camera.position.toArray(), target: orbit.target.toArray()}, hiddenArt: [...hiddenArt], showStandins, visibility: {mode: visMode, distance: visDist}}; return {...project, reference: undefined}; }
const changedOnDisk = t => t != null && diskSaved != null && Math.abs(t - diskSaved) > .001;
function conflictDialog(t, saving) {
  const box = $('conflict'); box.hidden = false;
  $('conflictText').textContent = `"${project.name}" was saved somewhere else at ${new Date(t * 1000).toLocaleTimeString()}, after this window loaded it (the classic editor, another window, or a playtest build). ${saving ? 'Saving now replaces that version' : 'If you save here you replace that version'} (the server keeps the older file in the project's history folder).`;
  $('conflictKeep').textContent = saving ? 'Overwrite with mine' : 'Keep mine for now'; $('conflictCancel').hidden = !saving;
  $('conflictReload').textContent = dirty ? 'Reload saved version (drop my changes)' : 'Reload saved version';
  lifeEvent('conflict', `${saving ? 'save' : 'focus'} disk=${t} mine=${diskSaved}`);
  return new Promise(done => { for (const [id, v] of [['conflictReload', 'reload'], ['conflictKeep', 'keep'], ['conflictCancel', 'cancel']]) $(id).onclick = () => { box.hidden = true; done(v); }; });
}
async function guard() {   // before a save: true = go ahead
  if (!$('conflict').hidden) return false; if (!project.id || diskSaved == null) return true;
  let t; try { t = await diskTime(project.id); } catch { return true; }
  if (!changedOnDisk(t)) return true;
  const choice = await conflictDialog(t, true);
  if (choice === 'reload') { dirty = false; openProject(project.id); }
  return choice === 'keep';
}
async function savedNow(out) { try { diskSaved = out?.mtime ?? await diskTime(project.id); } catch {} if (project && !config.projects.some(p => p.id === project.id)) config.projects.unshift({id: project.id, name: project.name, stage: project.stage, saved: Date.now() / 1000}); }
async function checkDisk() {   // window came back: was the project saved elsewhere meanwhile?
  if (!project || diskSaved == null || !$('conflict').hidden || performance.now() - (checkDisk.at || 0) < 3000) return; checkDisk.at = performance.now();
  let t; try { t = await diskTime(project.id); } catch { return; }
  if (!changedOnDisk(t) || t === warnedFor) return; warnedFor = t;
  if (await conflictDialog(t, false) === 'reload') { dirty = false; openProject(project.id); }
}
addEventListener('focus', checkDisk); document.addEventListener('visibilitychange', () => document.visibilityState === 'visible' && checkDisk());
async function save(auto) {
  auto = auto === true;   // (a click passes its event)
  if (!project || !await guard()) return;
  try { const out = await api('/api/save', serializable()); dirty = false; writeDraftNow(); await savedNow(out); $('saveState').textContent = (auto ? 'Auto-saved ' : 'Saved ') + new Date().toLocaleTimeString([], {hour: '2-digit', minute: '2-digit'}); if (!auto) toast('Saved. The classic editor sees the same project (reload it there).'); }
  catch (e) { toast(e.message, true); }
}
// ---- Game view (spec 7b): the editor camera streams to POST /api/live-view/camera {id, position, lookAt, fov} at up to
// ~10 Hz while on (only when it moved); off, another project or closing the page sends /api/live-view/release. The
// relay is a stub today (relayed false: the file is written, no game reads it yet - ): the chip says so.
var gameView = {on: false, timer: null, last: '', busy: false, state: ''};
const gvState = r => r.relayed ? 'live' : r.running ? 'waiting for the game (its reader is pending)' : 'waiting for the game (not running)';
function gameViewOn() {
  if (!project || gameView.on) return; gameView.on = true; gameView.last = ''; gameView.pid = project.id; gameView.state = 'starting…'; renderLens();
  gameView.timer = setInterval(async () => {
    if (!gameView.on || gameView.busy || !project || project.id !== gameView.pid) return;
    const pose = {id: project.id, position: camera.position.toArray().map(v => Math.round(v * 10) / 10), lookAt: orbit.target.toArray().map(v => Math.round(v * 10) / 10), fov: Math.round(camera.fov)}, key = JSON.stringify(pose);
    if (key === gameView.last) return; gameView.last = key; gameView.busy = true;
    try { const r = await api('/api/live-view/camera', pose); gameView.sent = (gameView.sent || 0) + 1; gameView.reply = r; setGv(gvState(r)); }
    catch (e) { setGv('no game: ' + e.message.replace(/^Live view: /, '')); }
    finally { gameView.busy = false; }
  }, 100);
}
function setGv(t) { if (t !== gameView.state) { gameView.state = t; renderLens(); } }
function gameViewOff(beacon = false) {
  if (!gameView.on) return; clearInterval(gameView.timer); gameView.on = false; const pid = gameView.pid; gameView.state = '';
  const body = JSON.stringify({id: pid});
  if (beacon) { try { fetch('/api/live-view/release', {method: 'POST', keepalive: true, headers: {'Content-Type': 'application/json', 'X-Workshop-Token': config?.token || ''}, body}); } catch {} }
  else api('/api/live-view/release', {id: pid}).catch(() => {});
  renderLens();
}
addEventListener('pagehide', () => gameViewOff(true));
async function freshGuard(what) {   // a build saves the project: a fresh copy becomes a new project
  if (!isFresh()) return true;
  return await choose(`${what} saves this copy`, `"${esc(project.name)}" is a fresh copy of the game stage and is not saved yet. ${esc(what)} saves it first, as a new project in Your projects.`, [['go', `Save and ${what.toLowerCase()}`], ['cancel', 'Cancel']]) === 'go';
}
let testDry = null;   // self-test: an array that receives the requests instead of the server (no build, no game)
async function playtest(opts) {
  const windowed = opts?.windowed === true, here = opts?.fromHere === true, step = t => { if (here) tfhState(t); toast(t); };
  if (!project) return;
  if (!testDry && (!await startGuard('Playtest') || !await freshGuard('Playtest') || !await guard())) return tfhState(null);
  step(windowed ? 'Building for the game view…' : here ? 'Saving + building…' : 'Building…');
  try {
    const body = {...serializable(), forPlaytest: true};   // a playtest build starts at project.playtestStart (Play from here / Test from here)
    if (testDry) { testDry.push({path: '/api/build', body}, {path: '/api/playtest', body: {id: project.id, build: '(dry run)', ...(windowed ? {windowed: true} : {}), ...(here ? {asPlayed: true} : {})}}); return tfhState(null); }
    const r = await api('/api/build', body); dirty = false; await savedNow(); lastReport = r;
    if (!r.ok) { tfhState(null); toast('Build refused: ' + (r.errors[0] || 'see the build report'), true); return showReport(r); }
    if (r.setidPermitted?.length) showReport(r);
    step('Starting the game…');
    const out = await api('/api/playtest', {id: project.id, build: r.path, ...(windowed ? {windowed: true} : {}), ...(here ? {asPlayed: true} : {})}); tfhState(null); lastRun = {...out, id: project.id}; logic.liveStart(project.id); if (windowed) gameViewOn(); toast(out.message + (r.warnings.length ? ` (${r.warnings.length} warnings)` : '')); renderPlayAgain();
  } catch (e) { tfhState(null); toast(e.message, true); lastRun = {message: e.message, log: (e.message.split('Last log lines:\n')[1] || ''), id: project.id}; savedNow(); renderPlayAgain(); }   // a failed build may still have saved the project
}
// build without starting the game: checks the project and shows the report (what was left out, which classes the build
// allowed in this stage via setid.bin: report.setidPermitted)
let lastReport = null;
async function buildOnly() {
  if (!project || !await startGuard('Build') || !await freshGuard('Build') || !await guard()) return; toast('Building (no game)…');
  try { const r = await api('/api/build', serializable()); dirty = false; await savedNow(); $('saveState').textContent = 'Saved + built'; lastReport = r; showReport(r); }
  catch (e) { toast(e.message, true); savedNow(); }
}
function showReport(r) {
  const box = $('report'); box.hidden = false; const perm = r.setidPermitted || [];
  $('reportBody').innerHTML = `<p class="${r.ok ? 'ok' : 'bad'}">${r.ok ? 'Build OK' : 'Build failed'} · ${r.objects ?? '-'} objects · ${r.geometry ?? 0} authored meshes${r.message ? ' · ' + esc(r.message) : ''}</p>
    ${r.errors?.length ? `<h4>Errors</h4><ul>${r.errors.map(e => `<li>${esc(e)}</li>`).join('')}</ul>` : ''}
    ${perm.length ? `<h4>Classes from other stages allowed in this stage (setid.bin)</h4><ul>${perm.map(c => `<li><b>${esc(c.className)}</b> · ${c.objects} placed · from ${esc((c.sourceStages || []).map(stageName).join(', ') || '-')}${c.reason ? ' · ' + esc(c.reason) : ''}</li>`).join('')}</ul>` : '<p class="muted">No classes from other stages needed permission.</p>'}
    ${r.warnings?.length ? `<h4>Warnings (${r.warnings.length})</h4><ul>${r.warnings.map(w => `<li>${esc(w)}</li>`).join('')}</ul>` : '<p class="muted">No warnings.</p>'}`;
  $('reportClose').onclick = () => box.hidden = true;
}
$('save').onclick = save; $('playtest').onclick = () => playtest(); $('testHere').onclick = () => testFromHere(); $('projectSwitch').onclick = showProjects;
addEventListener('beforeunload', e => { if (dirty) { e.preventDefault(); e.returnValue = ''; } });

// #80 version chip: GET /api/version {started, codeMtime, stale}. Ctrl R reloads the page, not the server's
// code: when a server file is newer than the running server, the top bar says to restart the editor server
async function checkVersion() {
  let v; try { v = await fetch('/api/version', {cache: 'no-store'}).then(r => r.ok ? r.json() : r.status === 404 ? {stale: true, old: true} : null); } catch { return; } if (!v) return;   // 404: a server from before the endpoint, so older than its code
  let chip = $('verChip'); if (!chip) { chip = Object.assign(document.createElement('span'), {id: 'verChip', className: 'verChip'}); $('saveState')?.before(chip); }
  chip.hidden = !v.stale; chip.textContent = 'Server is older than its code: restart the editor server';
  chip.title = (v.old ? 'This editor server has no /api/version, so it was started before the current code. ' : `The editor server started ${new Date(v.started * 1000).toLocaleString()}; its code changed ${new Date(v.codeMtime * 1000).toLocaleString()}. `) + 'Ctrl R only reloads this page: close the editor server and start it again to get the fixes.';
}
setInterval(checkVersion, 180000); addEventListener('focus', checkVersion);
// 'Editor updated: press Ctrl R': the editor's own files changed on disk since this page loaded (an agent's fix). The
// server sends no Last-Modified, so the sizes are compared (headers only); every minute and when the window gets focus.
// The chip reloads through the same guard as Ctrl R (Keep unsaved / Save / Discard / Stay, #96).
const UI_FILES = ['ui2.js', 'ui2.css', 'render3d.js', 'logic.js', 'selftest.js', 'index.html', '../nodes.json'];
const uiSize = async f => { try { const r = await fetch(f, {cache: 'no-store'}); const n = +r.headers.get('content-length'); r.body?.cancel?.(); return n || null; } catch { return null; } };
let uiSizes0 = null, uiNotified = false;
async function checkUiFiles() {
  const now = await Promise.all(UI_FILES.map(uiSize)); if (!uiSizes0) { uiSizes0 = now; return []; }
  const changedF = UI_FILES.filter((f, i) => now[i] && uiSizes0[i] && now[i] !== uiSizes0[i]); if (!changedF.length) return [];
  let chip = $('uiChip'); if (!chip) { chip = Object.assign(document.createElement('button'), {id: 'uiChip', className: 'verChip uiChip'}); $('saveState')?.before(chip);
    chip.onclick = () => { if (dirty) window.dispatchEvent(new KeyboardEvent('keydown', {key: 'r', ctrlKey: true, bubbles: true, cancelable: true})); else location.reload(); }; }
  chip.textContent = 'Editor updated: press Ctrl R'; chip.title = `Changed since this page loaded: ${changedF.join(', ')}. Ctrl R (or a click here) loads the new version and comes back to this project.`; chip.hidden = false;
  if (!uiNotified) { uiNotified = true; toast(`The editor was updated (${changedF.join(', ')}): press Ctrl R to load it`); }
  return changedF;
}
setInterval(checkUiFiles, 60000); addEventListener('focus', checkUiFiles); checkUiFiles();
(async () => {
  checkVersion();
  try { config = await api('/api/config'); } catch (e) { toast('Cannot reach the workshop server: ' + e.message, true); return; }
  for (const c of config.catalog) catalog.set(`${c.list}-${c.type}`, c); R.catalog = config.catalog;
  alive();
  const last = new URLSearchParams(location.search).get('project');
  const qp = new URLSearchParams(location.search);
  if (qp.get('selftest') === 'auto') runHeadlessSelfTest(qp.get('report'), (qp.get('stages') || '').split(',').map(Number).filter(Boolean), qp.get('only') || '');
  else if (qp.get('stage') && !last) shotStage(+qp.get('stage'), qp.get('cam'));   // ?stage=N[&cam=x,y,z>tx,ty,tz]: a fresh copy at a camera pose (#52 editor vs game frames, shot_ui2.py)
  else bootRestore(last);   // check_ui2.py --selftest: Edge headless runs the self-test and posts the result
})();
async function shotStage(st, cam) {
  try { dirty = false; showLoaded(await api('/api/stage?id=' + st), 'Fresh copy · not saved yet'); } catch (e) { toast('Cannot open stage ' + st + ': ' + e.message, true); return; }
  const v = (cam || '').split('>').map(t => t.split(',').map(Number));
  if (new URLSearchParams(location.search).get('art') === '0') { showArt = false; showKill = false; renderLens(); }   // objects only (#52 frames)
if (new URLSearchParams(location.search).get('arcs') === '1') { showArcs = true; renderLens(); }
if (new URLSearchParams(location.search).get('clean') === '1') document.body.classList.add('cleanShot');   // review shots (#128 tileset sheets): the 3D view alone, full window   // every launch arc (#124 frames; not remembered)
  if (v.length === 2 && v.every(a => a.length === 3 && a.every(Number.isFinite))) { fly = null; camera.position.set(...v[0]); orbit.target.set(...v[1]); camera.lookAt(...v[1]); orbit.update(); }
}
// ---- UI 2 self-test (selftest.js / selftest.md): window.workshopSelfTest() or Ctrl K > Run self-test
const selfTestApi = {api, applyLevelImport, openLevelImport, lastImport: () => lastLevelImport, TILE_GRID, tilesetKits, placeTile, tileSnap: () => snapState, armRow, cancelPlacing, tileGhost, tileYaw, addPrefs, saveAddPrefs, bossLap: o => { const l = bossLap(o); return l && {n: l.pts.length, speed: l.speed, pts: l.pts.map(v => v.toArray())}; }, lapDrawn: () => bossGroup.children.filter(c => c.isMesh && c.geometry?.type === 'ConeGeometry').length, THREE, renderer, gizmo, proxy, viewport, rulesFor, writeRule, logic, newBlankStage, csPicker, bossGroup, cameraStart, testDry: on => { testDry = on ? [] : null; return testDry; }, camera, step: (n = 1) => { for (let i = 0; i < n; i++) tick(performance.now() + i * 16); },
  project: () => project, items: () => items, selected: () => selected, select: it => select(it || null, false), selectId: id => select(byId.get(id) || null, false), setSelection, nodeOf: it => nodeOf.get(it),
  screenOf, screenOfItem: it => { const n = nodeOf.get(it); if (!n) return null; n.updateMatrixWorld(true); return screenOf(new THREE.Box3().setFromObject(n).getCenter(new THREE.Vector3())); },
  pose: (p, t) => { fly = null; orbit.target.set(...t); camera.position.set(...p); camera.lookAt(...t); orbit.update(); },
  cancelModes: () => { placing = null; placingKeep = false; if (drawing) cancelDraw(); if (faceMode) setFaceMode(false); if (surfMode) setSurfMode(false); wpSel = null; ctxMenu.hidden = true; if (objPick) objPick.done(null); renderer.domElement.style.cursor = ''; },
  dirty: () => dirty, isFresh, errors: () => uiErrors, stageName,
  openFresh: async st => { dirty = false; const p = await api('/api/stage?id=' + st); showLoaded(p, 'Fresh copy · not saved yet'); },
  ready: () => !!models && art.children.length > 0, artCount: () => art.children.length,
  storedBackdrop: () => { try { return localStorage.getItem('ui2.backdrop'); } catch { return null; } }, skyMode: () => skyMode,
  startObject: () => project?.objects.find(isStart) || null, hasStart, restoreStart,
  enclosingArt: () => art.children.filter(g => { if (!g.visible || g.userData.backdrop || ['O', 'OS'].includes(g.userData.kind)) return false; const b = g.userData.shellBox || new THREE.Box3().setFromObject(g); if (!b.containsPoint(camera.position)) return false; const sz = b.getSize(new THREE.Vector3()); let tris = 0; g.traverse(m => { if (m.isMesh) tris += (m.geometry.index?.count || 0) / 3; }); return Math.min(sz.x, sz.z) >= 3000 && tris < 2000; }).map(g => ({world: g.userData.world, shellIn: !!g.userData.shellIn})),
  pickTarget: at => { let best = null, bd = Infinity; for (const it of items) { if (it.o.kind !== 'native' || it.cat === 'volumes' || hiddenCats.has(it.cat) || !nodeOf.has(it) || !visible(it)) continue; const n = nodeOf.get(it), sz = new THREE.Box3().setFromObject(n).getSize(new THREE.Vector3()).length(); if (!(sz > 5 && sz < 400)) continue; const d = Math.hypot(...it.at.map((v, i) => v - at[i])); if (d < bd) { bd = d; best = it; } } return best; },
  undo, showPanel, showWorkspace, showProjects, placing: () => placing, countClass: (l, t) => project.objects.filter(o => o.kind === 'native' && o.list === l && o.type === t).length,
  paramWord, ringOffsets, enemyKind, params: o => o.params, guideHandles: () => enemyGuide.userData.handles || [], WP_HIT_PX, WP_HANDLE_PX,
  launcherKind, arcOf: it => arcOf(it.o), arcHandles: () => (drawSpringGuide(true), springGuide.userData.handles || []), arcLines: () => springGuide.children.filter(c => c.isLine || c.userData.guideLine).length, springGuide: () => springGuide, arcSel: () => arcSel, HANDLE_R, HANDLE_MIN_PX, HANDLE_MAX_PX, setShowArcs, showArcs: () => showArcs, arcState: it => arcState(it.o),
  arcAim: (it, k, q) => { arcDrag = {it, k, moved: true, why: ''}; snapshot(); const s = aimArc(it, k, new THREE.Vector3(...q)); arcDrag.out = !!s && s.err > 25; arcDrag.why = arcDrag.out ? 'out of reach' : ''; arcDragEnd(); return s; },
  lightState: () => [hemi.color.getHexString(), hemi.groundColor.getHexString(), hemi.intensity.toFixed(3), sun.color.getHexString(), sun.intensity.toFixed(3), scene.fog ? scene.fog.color.getHexString() + ' ' + scene.fog.near + ' ' + scene.fog.far : 'nofog', scene.background?.getHexString?.() || 'nobg'].join('|'),
  setLook: on => { if (!!gameLook.on !== !!on) document.querySelector('[data-look]')?.click(); }, editorView: () => [...document.querySelectorAll('[data-look]')].find(b => /Editor view/.test(b.textContent))?.click(), gameLookOn: () => !!gameLook.on, orbit, undoDepth: () => undoStack.length,
  savePrefab, prefabs: () => prefabsList || [], prefabName, insertPrefab, deletePrefabNamed: async name => { if (prefabsOnServer) await api('/api/prefabs/delete', {name}).catch(() => {}); if (project?.prefabs) project.prefabs = project.prefabs.filter(pf => prefabName(pf.name) !== name); await loadPrefabs(); },
  issueTitles: () => issues().map(x => x[1]), uiFilesCheck: async (fakeOld) => { const keep = uiSizes0, kn = uiNotified; if (fakeOld) uiSizes0 = uiSizes0.map(n => n && n + 1); const r = await checkUiFiles(); const shown = !$('uiChip')?.hidden && /Ctrl R/.test($('uiChip')?.textContent || ''); uiSizes0 = keep; uiNotified = kn; if ($('uiChip')) $('uiChip').hidden = true; return {changed: r, shown}; }, activeAt, lookOf, canvasFilter: () => renderer.domElement.style.filter, sessionWrite, writeDraftNow, draftGet, bootRestore, clearDraft: () => { clearTimeout(draftTimer); return draftPut(null).catch(() => {}); }, clearSession: () => { try { localStorage.removeItem('ui2.session'); } catch {} }, emptyStateShown: () => !$('emptyState').hidden, cameraNow: () => [...camera.position.toArray(), ...orbit.target.toArray()], snapState, applySnapUi, boxOf: it => { const n = nodeOf.get(it); n.updateMatrixWorld(true); return new THREE.Box3().setFromObject(n); }, tileUnlinked, centrePoint: () => centrePoint(), markChanged: () => changed(), save: () => save(), editable: () => editable(), openProject, discard: () => { dirty = false; placing = null; clearTimeout(draftTimer); }};
window.workshopSelfTest = async opts => (await import('./selftest.js')).runSelfTest(selfTestApi, opts);
// quick game check: what the camera looks at, in the real game in seconds - Play from here at the surface under the view
// centre (facing the camera's way), then the windowed playtest (muted, not focused; the live Game view follows the editor
// camera). The usual Build / Playtest guards apply (a fresh copy is saved first; a start is needed)
// Test from here (top bar button, F5): save + build + start the game as Shadow WHERE THE CAMERA IS, facing the camera's
// way: on the floor under the camera when there is one within 5000 units (2 units above it), else exactly at the camera.
// The normal game window with sound and focus and the user's own settings (server: asPlayed), the newest game exe (the
// server refreshes the editor runtime from play\ when play\ is newer); the test game has its own memory card.
const groundRay = new THREE.Raycaster();
function cameraStart() {
  const c = camera.position.clone(); groundRay.set(c, new THREE.Vector3(0, -1, 0)); groundRay.far = 5000; groundRay.layers.enableAll();
  const solid = [...art.children.filter(m => m.visible && !m.userData.backdrop && !m.userData.shellIn), ...nodes.children.filter(n => !n.userData.volume && n.visible)];
  const hit = groundRay.intersectObjects(solid, true).find(h => h.face && h.face.normal.clone().transformDirection(h.object.matrixWorld).y > .3);   // a floor, not a wall
  return {position: hit ? hit.point.clone().add(new THREE.Vector3(0, 2, 0)) : c, ground: !!hit};
}
function tfhState(text) { const b = $('testHere'); if (!b) return; b.classList.toggle('busy', !!text); b.disabled = !!text; b.textContent = text ? text.replace(/…$/, '…') : '▶ Test from here'; }
async function testFromHere() {
  if (!project) return toast('Open a stage first', true); if ($('testHere')?.disabled) return;
  const st = cameraStart(); setPlayStart.quiet = true; try { setPlayStart(st.position, ''); } finally { setPlayStart.quiet = false; }
  toast(`Test from here: ${st.ground ? 'on the floor under the camera' : 'at the camera (no floor under it)'}, facing the camera's way`);
  return playtest({fromHere: true});
}
addEventListener('keydown', e => { if (e.key === 'F5' && !e.ctrlKey && !e.shiftKey && !logic.active?.() && !/INPUT|SELECT|TEXTAREA/.test(document.activeElement?.tagName)) { e.preventDefault(); testFromHere(); } });   // F5 = Test from here (Ctrl R still reloads the page)
async function quickGameCheck() {
  if (!project) return toast('Open a stage first', true); const pt = centrePoint(); if (!pt) return toast('No surface under the view centre', true);
  setPlayStart(pt, ' at the camera view (quick game check)'); await playtest({windowed: true});
}
async function runHeadlessSelfTest(port, stages = [], only = '') {   // the result goes to check_ui2.py's receiver on 127.0.0.1:<port> (nothing else is reached)
  const post = body => fetch(`http://127.0.0.1:${+port}/result`, {method: 'POST', mode: 'no-cors', body: JSON.stringify(body)}).catch(() => {});
  const prog = row => fetch(`http://127.0.0.1:${+port}/progress`, {method: 'POST', mode: 'no-cors', body: JSON.stringify(row)}).catch(() => {});
  try { const r = await window.workshopSelfTest({log: true, progress: prog, ...(stages.length ? {stages} : {}), ...(only ? {only} : {})}); await post({summary: r.summary, results: r}); } catch (e) { await post({summary: 'UI 2 self-test crashed: ' + (e?.stack || e), results: []}); }
}
async function runSelfTestUi() {
  if (dirty) return toast('Save your changes first: the self-test opens other stages (it never saves your projects)', true);
  toast('Self-test running (about a minute; the view jumps between stages)…'); const r = await window.workshopSelfTest();
  const f = r.filter(x => x.result === 'FAIL').length, rows = r.map(x => `<tr class="${x.result}"><td>${esc(x.stage)}</td><td>${esc(x.check)}</td><td>${x.result}</td><td>${esc(x.why)}</td></tr>`).join('');
  setTimeout(() => $('stCopy')?.addEventListener('click', () => navigator.clipboard?.writeText(r.summary).then(() => toast('Copied the summary line'), () => toast('Select the line and Ctrl C', true))), 0);
  await choose(`Self-test: ${r.length - f - r.filter(x => x.result === 'skip').length} pass, ${f} fail`, `<div class="stTable"><table><tr><th>Stage</th><th>Check</th><th></th><th>Why</th></tr>${rows}</table></div><p><code class="stSum" id="stSum">${esc(r.summary || '')}</code> <button class="ghost small" id="stCopy">Copy line</button></p><p class="muted">Also in the browser console (console.table). Run <code>python tools/stage_editor/ui2/check_ui2.py --cleanup</code> afterwards: it moves the round-trip project out of Your projects.</p>`, [['ok', 'Close']]);
}
// developer hook (only with ?debug in the URL): the scene for inspecting from the console / an automated check
if (new URLSearchParams(location.search).has('debug')) window.__ui2 = {THREE, scene, camera, renderer, art, nodes, orbit, get project() { return project; }, get skyMode() { return skyMode; }, get gameLook() { return gameLook; }, step(n = 1) { for (let i = 0; i < n; i++) tick(performance.now() + i * 16); }, guide: enemyGuide, screenOf, gizmo, proxy, get selectedId() { return selected?.id; }, get wpSel() { return wpSel; }, get placing() { return placing; }, WP_HIT_PX, WP_HANDLE_PX};   // step: run frames by hand (a hidden pane pauses requestAnimationFrame)
