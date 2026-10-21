// UI 2 self-test (tools/stage_editor/ui2/selftest.md): window.workshopSelfTest() or Ctrl K > Run self-test. Opens fresh
// copies of Westopolis (100), Final Haunt (604) and Digital Circuit (200) - never saved, the user's projects untouched -
// and checks the editor with synthetic pointer / key events (works with a hidden window: frames are run by hand).
// One project is saved for the save + reload round trip, named 'selftest <time>';
// tools/stage_editor/ui2/check_ui2.py --cleanup moves it out of stage_projects afterwards.
// A = the editor's internals (ui2.js selfTestApi). Returns [{stage, check, result: pass | FAIL | skip, why}].
export async function runSelfTest(A, {stages = [100, 604, 200], roundTrip = true, log = true, progress = null, only = ''} = {}) {   // only: run just the checks whose name contains it (check_ui2.py --only=)
  const out = [], T = A.THREE, wait = ms => new Promise(r => setTimeout(r, ms));
  const frames = async (n = 3) => { for (let i = 0; i < n; i++) { A.step(1); await wait(20); } };
  const cv = () => A.renderer.domElement;
  const fire = (type, x, y, o = {}) => cv().dispatchEvent(new PointerEvent(type, {clientX: x, clientY: y, button: o.button ?? 0, buttons: type === 'pointerup' ? 0 : (o.button === 2 ? 2 : 1), bubbles: true, cancelable: true, pointerId: 7, pointerType: 'mouse', shiftKey: !!o.shift, ctrlKey: !!o.ctrl}));
  const click = async (x, y, o) => { fire('pointerdown', x, y, o); fire('pointerup', x, y, o); await wait(80); };
  const drag = async (x, y, dx, dy) => { fire('pointerdown', x, y); for (let i = 1; i <= 4; i++) fire('pointermove', x + dx * i / 4, y + dy * i / 4); fire('pointerup', x + dx, y + dy); window.dispatchEvent(new PointerEvent('pointerup', {bubbles: true})); await wait(80); };
  const key = async k => { document.activeElement?.blur?.(); window.dispatchEvent(new KeyboardEvent('keydown', {key: k, bubbles: true, cancelable: true})); window.dispatchEvent(new KeyboardEvent('keyup', {key: k, bubbles: true})); await wait(80); };
  const undo = async (n = 1) => { for (let i = 0; i < n; i++) { A.undo(); await wait(40); } };
  const centre = () => { const r = cv().getBoundingClientRect(); return [r.left + r.width / 2, r.top + r.height / 2]; };
  const pos = id => A.project().objects.find(o => o.id === id)?.position?.slice();
  const near = (a, b, e = .05) => a && b && a.every((v, i) => Math.abs(v - b[i]) <= e);
  async function check(stage, name, fn) {
    if (only && !name.includes(only) && name !== 'open a fresh copy') return;   // the stage still opens
    let r; try { r = await fn(); } catch (e) { r = 'threw: ' + (e?.message || e); }
    const res = r === true ? 'pass' : r?.skip ? 'skip' : r?.pass ? 'pass' : 'FAIL', why = r === true ? '' : r?.why ?? r?.skip ?? (typeof r === 'string' ? r : '');
    out.push({stage, check: name, result: res, why: String(why).slice(0, 220)}); try { progress?.(out.at(-1)); } catch {}
    A.cancelModes(); await wait(30);
  }
  if (A.dirty()) return [{stage: '-', check: 'start', result: 'FAIL', why: 'the open project has unsaved changes: save (or discard) them first - the self-test opens other stages'}];
  const prev = A.project() && !A.isFresh() ? A.project().id : null, errs0 = A.errors().length, look0 = A.gameLookOn();
  A.orbit.enabled = false;   // OrbitControls captures the pointer on press; synthetic pointers are not 'active' (setPointerCapture throws)
  const story = stages.map(s => [s, A.stageName(s)]);
  for (const [S, label] of story) {
    const tag = `${S} ${label}`, errAt = A.errors().length;
    await check(tag, 'open a fresh copy', async () => { await A.openFresh(S); for (let i = 0; i < 200 && !A.ready(); i++) await wait(100); await frames(4); return A.ready() && A.project()?.stage === S ? {pass: true, why: `${A.items().length} placements, ${A.artCount()} art pieces`} : 'models / art did not load in 20 s'; });
    if (!A.ready()) continue;
    await check(tag, 'a frame renders (not blank) + frame hash', async () => {   // the real renderer draws something: mean brightness and a short hash of the frame
      await frames(4); const c = A.renderer.domElement, o = document.createElement('canvas'); o.width = 96; o.height = 54; const g = o.getContext('2d'); A.step(1); g.drawImage(c, 0, 0, 96, 54);
      const d = g.getImageData(0, 0, 96, 54).data; let sum = 0, h = 2166136261; for (let i = 0; i < d.length; i += 4) { const l = (d[i] + d[i + 1] + d[i + 2]) / 3; sum += l; h = Math.imul(h ^ (l | 0), 16777619) >>> 0; }
      const mean = sum / (96 * 54); return mean > 1.5 ? {pass: true, why: `mean ${mean.toFixed(1)}, hash ${h.toString(16).padStart(8, '0')}`} : `the frame is blank (mean ${mean.toFixed(2)})`;
    });
    await check(tag, 'no script errors on load', () => { const e = A.errors().slice(errAt); return e.length ? `${e.length}: ${e[0].msg}` : true; });
    await check(tag, '3D view not clipped (no rounded / masked canvas)', () => {   // the oval: a CSS class shared by the canvas gave it border-radius 50%
      const bad = []; for (let el = cv(); el && el !== document.body; el = el.parentElement) { const st = getComputedStyle(el), r = st.borderRadius;
        if (r && !/^0px( 0px)*$/.test(r) && (el === cv() || st.overflow !== 'visible')) bad.push(`${el.tagName.toLowerCase()}${el.id ? '#' + el.id : ''}.${el.className}: border-radius ${r}`);
        if (st.clipPath && st.clipPath !== 'none') bad.push(`${el.tagName.toLowerCase()}: clip-path ${st.clipPath}`); if (st.maskImage && st.maskImage !== 'none') bad.push(`${el.tagName.toLowerCase()}: mask ${st.maskImage}`); }
      return bad.length ? bad.join('; ') : true;
    });
    await check(tag, 'Backdrop art off by default', () => { const stored = A.storedBackdrop(); if (stored != null) return {pass: true, why: `this browser stored '${stored}' (default checked only without a stored choice)`}; return A.skyMode() === 'hidden' ? true : `default is '${A.skyMode()}'`; });
    const st = A.startObject();
    if (st) A.pose(st.position.map((v, i) => v + [300, 260, 320][i]), st.position); await frames(4);
    await check(tag, 'no camera-enclosing shell in front', () => { const bad = A.enclosingArt().filter(g => !g.shellIn); return bad.length ? `drawn in front: ${bad.map(g => g.world).join(', ')}` : {pass: true, why: `${A.enclosingArt().length} enclosing shell(s), all in the background pass`}; });
    // a visible native object near the start
    const target = A.pickTarget(st?.position || [0, 0, 0]);
    await check(tag, 'viewport click selects', async () => {
      if (!target) return {skip: 'no visible native object near the start'};
      A.select(null); A.pose(target.at.map((v, i) => v + [140, 120, 160][i]), target.at); await frames(3);
      const s = A.screenOfItem(target); if (!s) return 'its node is off screen'; await click(s[0], s[1]); await frames(1);
      return A.selected()?.id === target.id ? {pass: true, why: target.name} : `selected ${A.selected()?.name || 'nothing'} instead of ${target.name}`;
    });
    await check(tag, 'gizmo drag moves it, undo restores', async () => {
      if (!target || A.selected()?.id !== target.id) return {skip: 'nothing selected'};
      const before = pos(target.id), n = A.nodeOf(target); if (A.gizmo.object !== n) return 'the gizmo is not on its node';
      A.gizmo.dispatchEvent({type: 'dragging-changed', value: true}); n.position.x += 50; A.gizmo.dispatchEvent({type: 'objectChange'}); A.gizmo.dispatchEvent({type: 'dragging-changed', value: false}); await wait(60);
      const moved = pos(target.id); await undo(); const back = pos(target.id);
      return Math.abs(moved[0] - before[0] - 50) < .05 && near(back, before) ? true : `moved ${moved?.[0] - before[0]}, after undo ${back}`;
    });
    await check(tag, 'Delete: allowed for an object, refused for a finish pose', async () => {
      const g = A.project().objects.find(o => o.kind === 'goalpose'); let refused = 'no finish pose in this stage';
      if (g) { A.selectId(g.id); await key('Delete'); refused = A.project().objects.some(o => o.id === g.id) ? '' : 'a finish pose was deleted'; }
      if (refused && g) return refused;
      if (!target) return {skip: 'no object to delete'}; A.selectId(target.id); await key('Delete'); const gone = !A.project().objects.some(o => o.id === target.id); await undo(); const back = A.project().objects.some(o => o.id === target.id);
      return gone && back ? {pass: true, why: g ? 'finish pose kept, object deleted + undone' : 'object deleted + undone (no finish pose here)'} : `deleted ${gone}, back after undo ${back}`;
    });
    await check(tag, 'start point: delete / place here / restore', async () => {
      const s0 = A.startObject(); if (!s0) return {skip: 'no start'}; A.selectId(s0.id); await key('Delete');
      if (A.hasStart()) return 'Delete did not remove the start';
      const removed = A.project().playerStart?.removedOriginal; const [cx, cy] = centre();
      fire('pointerdown', cx, cy, {button: 2}); cv().dispatchEvent(new MouseEvent('contextmenu', {clientX: cx, clientY: cy, button: 2, bubbles: true, cancelable: true})); await wait(60);
      const b = [...document.querySelectorAll('.ctxMenu button')].find(x => x.dataset.c === 'pstart'); if (!b) return 'no Place start here in the context menu'; b.click(); await wait(80);
      if (!A.hasStart()) return 'Place start here placed nothing';
      if (A.project().mode !== 'new') { await A.restoreStart(); if (A.startObject()?.name !== 'Player start (game)' || A.project().playerStart?.removedOriginal) return 'Restore original did not put the game start back'; }
      return {pass: true, why: `removedOriginal ${removed}; placed ${A.startObject()?.name}`};
    });
    await check(tag, 'Add panel: search + click to place', async () => {
      A.showPanel('add'); await wait(150); const aq = document.getElementById('aq'); aq.value = 'ring'; aq.dispatchEvent(new Event('input')); await wait(250);
      const tile = [...document.querySelectorAll('#vsizer .vt')].find(t => t.dataset.k === 'c:0-16'); if (!tile) return 'the Ring tile is not found by search';
      const n0 = A.countClass(0, 16); tile.click(); await wait(60); if (!A.placing()) return 'clicking the tile did not arm placing';
      const [cx, cy] = centre(); await click(cx, cy); const n1 = A.countClass(0, 16); await undo(); return n1 === n0 + 1 && A.countClass(0, 16) === n0 ? true : `rings ${n0} -> ${n1} -> ${A.countClass(0, 16)}`;
    });
    if (S === stages[0]) await check(tag, 'Test from here: button + F5 send a playtest build at the camera (floor under it, within 1 unit), asPlayed (dry run, no game)', async () => {
      const st = A.startObject(); if (!st) return {skip: 'no player start to stand the camera over'};
      const d0 = A.undoDepth(), [x, y, z] = st.position; A.pose([x, y + 400, z - 300], [x, y, z + 400]); await frames(2);
      const reqs = A.testDry(true); document.getElementById('testHere').click(); for (let i = 0; i < 40 && reqs.length < 2; i++) await wait(50);
      const exp = A.cameraStart(), cam = A.camera.position, b = reqs[0]?.body, ps = b?.playtestStart, pt = reqs[1]?.body;
      const near = ps && Math.hypot(...ps.position.map((v, i) => v - exp.position.getComponent(i))) <= 1 && Math.abs(ps.position[0] - cam.x) <= 1 && Math.abs(ps.position[2] - cam.z) <= 1;
      const n1 = reqs.length; window.dispatchEvent(new KeyboardEvent('keydown', {key: 'F5', bubbles: true, cancelable: true})); for (let i = 0; i < 40 && reqs.length < n1 + 2; i++) await wait(50);
      const f5 = reqs.length === n1 + 2 && reqs[n1].body.forPlaytest === true; A.testDry(false);
      const label = document.getElementById('testHere')?.textContent || ''; while (A.undoDepth() > d0) await undo();
      const ok = reqs[0]?.path === '/api/build' && b.forPlaytest === true && ps?.enabled && near && (!exp.ground || ps.position[1] < cam.y) && reqs[1]?.path === '/api/playtest' && pt.asPlayed === true && !pt.windowed && f5 && /Test from here/.test(label);
      return ok ? {pass: true, why: `start ${ps.position.map(Math.round).join(', ')} (${exp.ground ? 'floor under the camera' : 'at the camera'}), yaw ${Math.round(ps.rotation[1])}; build forPlaytest, playtest asPlayed; F5 the same; undone`}
        : `requests ${reqs.map(q => q.path).join(' ')}, forPlaytest ${b?.forPlaytest}, start ${ps?.position} vs ${exp.position.toArray().map(Math.round)} (ground ${exp.ground}), asPlayed ${pt?.asPlayed}, F5 ${f5}, label '${label}'`;
    });
    if (S === stages[0]) await check(tag, 'boss rules (#125 limits lifted): Devil Doom locked, Heavy Dog placeable, an Egg Breaker next to enemies kept, undo', async () => {
      A.showPanel('add'); await wait(150); const aq = document.getElementById('aq'); aq.value = ''; aq.dispatchEvent(new Event('input'));
      const here = [...document.querySelectorAll('#addFilters button')].find(b => /This stage only/.test(b.textContent)); const hereOn = here?.classList.contains('on'); if (hereOn) { here.click(); await wait(300); }
      document.querySelector('#addCats [data-c="bosses"]')?.click(); let egg = null;
      for (let i = 0; i < 100 && !(egg = [...document.querySelectorAll('#vsizer .vt')].find(x => x.dataset.k === 'c:0-185')); i++) await wait(100);
      const tileOf = k => [...document.querySelectorAll('#vsizer .vt')].find(x => x.dataset.k === k), dog = tileOf('c:0-182'), devil = tileOf('c:0-190'), dogOpen = !!dog && !dog.classList.contains('off'), devilLocked = !devil || devil.classList.contains('off');
      if (!egg) { if (hereOn) here.click(); document.querySelector('#addCats [data-c="all"]')?.click(); return 'no Egg Breaker tile'; }
      const enemies = A.project().objects.filter(o => o.kind === 'native' && o.list === 0 && o.type >= 0x64 && o.type < 0xB4).length;
      const d0 = A.undoDepth(); egg.click(); await wait(60); const [cx, cy] = centre(); await click(cx, cy); await wait(250);
      const flagged = A.issueTitles().some(x => /Egg Breaker.*left out/.test(x)), kept = A.issueTitles().every(x => !/left out/.test(x) || !/Egg Breaker/.test(x));
      while (A.undoDepth() > d0) await undo(); if (hereOn) here.click(); document.querySelector('#addCats [data-c="all"]')?.click(); await wait(100);
      return dogOpen && devilLocked && !flagged && kept ? {pass: true, why: `Heavy Dog placeable, Devil Doom ${devil ? 'locked' : 'not listed'}; Egg Breaker with ${enemies} enemies: no 'left out' issue; undone`} : `Heavy Dog placeable ${dogOpen}, Devil Doom locked ${devilLocked}, enemies ${enemies}, left-out issue ${flagged}`;
    });
    if (S === stages[0]) await check(tag, 'boss lap path (#127): Egg Breaker, Make a lap path, loop + arrows, speed, a point drag = 1 undo step, undo all', async () => {
      A.showPanel('add'); await wait(150); const aq = document.getElementById('aq'); aq.value = ''; aq.dispatchEvent(new Event('input'));
      const here = [...document.querySelectorAll('#addFilters button')].find(b => /This stage only/.test(b.textContent)); const hereOn = here?.classList.contains('on'); if (hereOn) { here.click(); await wait(300); }
      document.querySelector('#addCats [data-c="bosses"]')?.click(); let egg = null;
      for (let i = 0; i < 100 && !(egg = [...document.querySelectorAll('#vsizer .vt')].find(x => x.dataset.k === 'c:0-185')); i++) await wait(100);
      const reset = () => { if (hereOn) here.click(); document.querySelector('#addCats [data-c="all"]')?.click(); };
      if (!egg) { reset(); return 'no Egg Breaker tile'; }
      const d0 = A.undoDepth(); egg.click(); await wait(60); const [cx, cy] = centre(); await click(cx, cy); await wait(250); const boss = A.selected()?.o;
      if (!boss || boss.type !== 0xB9) { while (A.undoDepth() > d0) await undo(); reset(); return 'placing Egg Breaker did not select it'; }
      const mvSel = document.getElementById('bMove'); mvSel.value = 'patrol'; mvSel.dispatchEvent(new Event('change')); await wait(80);
      document.getElementById('bLapNew')?.click(); await wait(150); const lap = A.bossLap(boss), arrows = A.lapDrawn();
      const sp = document.getElementById('bSpeed'); if (sp) { sp.value = '450'; sp.dispatchEvent(new Event('change')); await wait(60); }
      const speed = A.bossLap(boss)?.speed, tag2 = document.querySelector('#bossCard .tagOk')?.textContent || '';
      const pathIt = A.items().find(x => x.o.id === boss.boss?.move?.path); let dragged = false, steps = 0;
      if (pathIt) { A.select(pathIt); await frames(3); const n = A.nodeOf(pathIt); n.updateMatrixWorld(true); const w = n.localToWorld(new T.Vector3(...pathIt.o.points[2]));
        A.pose(w.toArray().map((v, i) => v + [0, 700, 420][i]), w.toArray()); await frames(3); const before = JSON.stringify(pathIt.o.points[2]), s1 = A.screenOf(w), d1 = A.undoDepth();
        await drag(s1[0], s1[1], 40, 0); dragged = JSON.stringify(pathIt.o.points[2]) !== before; steps = A.undoDepth() - d1; }
      const moved = A.bossLap(boss)?.pts?.[2];
      while (A.undoDepth() > d0) await undo(); reset(); await wait(100); const gone = !A.project().objects.some(o => o.id === boss.id) && !(A.project().paths || []).some(x => x.bossLap && x.name?.startsWith(boss.name || 'Egg'));
      return lap?.n === 8 && arrows === 8 && speed === 450 && /works in game/.test(tag2) && dragged && steps === 1 && moved && gone ? {pass: true, why: `8-point loop, ${arrows} direction arrows, speed 450, 'works in game' tag, a point drag = 1 undo step, all undone`} : `lap ${lap?.n}, arrows ${arrows}, speed ${speed}, tag '${tag2}', dragged ${dragged} (${steps} steps), undone ${gone}`;
    });
    if (S === stages[0]) await check(tag, 'boss: Bosses chip, place Blue Falcon, Boss card, circle arena drawn, Issues, undo', async () => {
      A.showPanel('add'); await wait(150); const aq = document.getElementById('aq'); aq.value = ''; aq.dispatchEvent(new Event('input'));
      const here = [...document.querySelectorAll('#addFilters button')].find(b => /This stage only/.test(b.textContent)); const hereOn = here?.classList.contains('on'); if (hereOn) { here.click(); await wait(300); }
      document.querySelector('#addCats [data-c="bosses"]')?.click(); let tile = null;
      for (let i = 0; i < 100 && !(tile = [...document.querySelectorAll('#vsizer .vt')].find(x => x.dataset.k === 'c:0-183')); i++) await wait(100);   // the other stages' libraries load in the background
      const tiles = [...document.querySelectorAll('#vsizer .vt')], tags = tiles.map(x => x.querySelector('.bossT')?.textContent || '').join(','), locked = tiles.filter(x => x.classList.contains('off')).length;
      if (!tile) { if (hereOn) here.click(); document.querySelector('#addCats [data-c="all"]')?.click(); return `no Blue Falcon tile (${tiles.length} tiles: ${tags})`; }
      const d0 = A.undoDepth(), n0 = A.countClass(0, 183); tile.click(); await wait(60); const [cx, cy] = centre(); await click(cx, cy); await wait(200); const n1 = A.countClass(0, 183);
      const boss = A.project().objects.find(o => o.list === 0 && o.type === 183 && !(o.boss)); if (boss) A.selectId(boss.id); await wait(150);
      const card = document.getElementById('bossCard'), tag2 = card?.querySelector('summary small')?.textContent; const sh = document.getElementById('bArShape'); if (sh) { sh.value = 'circle'; sh.dispatchEvent(new Event('change')); } await wait(150);
      const arena = boss?.boss?.arena?.shape, drawn = A.bossGroup.children.length, setup = A.issueTitles().some(x => /set-up only/.test(x));
      while (A.undoDepth() > d0) await undo(); const back = A.countClass(0, 183) === n0;
      if (hereOn) here.click(); document.querySelector('#addCats [data-c="all"]')?.click(); await wait(100);
      return n1 === n0 + 1 && /to try/.test(tag2 || '') && arena === 'circle' && drawn > 0 && !setup && back ? {pass: true, why: `${tiles.length} boss tiles (${locked} not portable) · placed, card '${tag2}', circle arena drawn, no 'set-up only' issue (#104: the build places it), undone`} : `placed ${n0} -> ${n1}, card '${tag2}', arena ${arena}, drawn ${drawn}, issue ${setup}, undone ${back}; tags ${tags}`;
    });
    await check(tag, 'Add panel: drag a tile into the view', async () => {
      const tile = [...document.querySelectorAll('#vsizer .vt')].find(t => t.dataset.k === 'c:0-16'); if (!tile) return 'no Ring tile';
      const n0 = A.countClass(0, 16), dt = new DataTransfer(), [cx, cy] = centre();
      tile.dispatchEvent(new DragEvent('dragstart', {bubbles: true, cancelable: true, dataTransfer: dt})); A.viewport.dispatchEvent(new DragEvent('dragover', {bubbles: true, cancelable: true, dataTransfer: dt, clientX: cx, clientY: cy}));
      A.viewport.dispatchEvent(new DragEvent('drop', {bubbles: true, cancelable: true, dataTransfer: dt, clientX: cx, clientY: cy})); window.dispatchEvent(new DragEvent('dragend', {bubbles: true})); await wait(80);
      const n1 = A.countClass(0, 16); await undo(); return n1 === n0 + 1 && A.countClass(0, 16) === n0 ? true : `rings ${n0} -> ${n1}`;
    });
    await check(tag, 'ring rows: a line / circle ring draws Number of rings copies', () => {
      const r = A.project().objects.find(o => o.kind === 'native' && o.list === 0 && o.type === 16 && [1, 2].includes(A.paramWord(o, 0)) && A.paramWord(o, 1) > 1);
      if (!r) return {skip: 'no line / circle ring here'}; const offs = A.ringOffsets(r), n = A.paramWord(r, 1);
      return offs?.offsets?.length === n ? {pass: true, why: `${r.name}: ${n} rings`} : `${offs?.offsets?.length} drawn for Number of rings ${n}`;
    });
    await check(tag, 'waypoints: Add + Place at mouse + grab', async () => {
      const e = A.project().objects.find(o => A.enemyKind(o) === 'wp'); if (!e) return {skip: 'no soldier / robot / pawn here'};
      const cur = () => A.params(A.project().objects.find(o => o.id === e.id));   // undo rebuilds the objects: read it again by id
      A.selectId(e.id); A.pose(e.position.map((v, i) => v + [260, 380, 340][i]), e.position); await frames(3); const p0 = cur();
      const add = document.getElementById('wpAdd'); if (!add) return 'no Add waypoint button'; if (add.disabled) { A.cancelModes(); return {skip: 'both waypoints already used'}; }
      const d0 = A.undoDepth(); add.click(); await wait(80); await frames(2); if (A.placing()?.waypoint === undefined) return 'Add waypoint did not arm placing';
      const s = A.screenOf(new T.Vector3(...e.position)); await click(s[0] + 50, s[1] + 35); await frames(2); const p1 = cur();
      const h = A.guideHandles(); if (!h.length) return 'no waypoint handle drawn'; const hs = A.screenOf(h[0]); await drag(hs[0], hs[1], 60, 10); await frames(1); const p2 = cur();
      while (A.undoDepth() > d0) await undo(); const p3 = cur();
      return p1 !== p0 && p2 !== p1 && p3 === p0 ? {pass: true, why: `hit radius ${A.WP_HIT_PX} px, disc ${A.WP_HANDLE_PX} px`} : `added ${p1 !== p0}, dragged ${p2 !== p1}, undone ${p3 === p0}`;
    });
    await check(tag, 'launch arcs (#124): trail, drag the landing, out of reach, all arcs', async () => {
      const s = A.project().objects.find(o => A.launcherKind(o) && A.launcherKind(o) !== 'digital'); if (!s) return {skip: 'no spring / ring / ramp / panel here'};
      const cur = () => { const o = A.project().objects.find(o => o.id === s.id); return JSON.stringify([o.rotation, A.params(o)]); };
      A.selectId(s.id); A.pose(s.position.map((v, i) => v + [500, 700, 600][i]), s.position); await frames(3); const it = A.selected(), a = A.arcOf(it), k = A.launcherKind(s);
      if (!a || a.points.length < 5) return `no arc for ${k}`; if (!document.getElementById('arcCard')) return 'no Launch arc card';
      const h = A.arcHandles().find(x => x.k === 'land'); if (!h) return 'no landing disc'; const c0 = cur(), d0 = A.undoDepth(), hs = A.screenOf(h.p);
      await drag(hs[0], hs[1], 45, 25); await frames(1); const c1 = cur(), steps = A.undoDepth() - d0;
      const far = A.arcAim(A.selected(), 'land', s.position.map((v, i) => v + [40000, 0, 0][i])), c2 = cur();
      while (A.undoDepth() > d0) await undo(); const c3 = cur();
      A.selectId(s.id); const was = A.showArcs(); A.setShowArcs(true); await frames(2); const n = A.project().objects.filter(o => A.launcherKind(o)).length, lines = (A.arcHandles(), A.arcLines()); A.setShowArcs(was);
      const ok = c1 !== c0 && steps === 1 && far && far.err > 25 && c2 !== c0 && c3 === c0 && lines >= Math.min(n, 2);
      return ok ? {pass: true, why: `${k}: ${a.points.length} steps, ${a.end.why} at ${a.end.t.toFixed(2)} s; landing drag = 1 undo step; 40000 away: out of reach (${Math.round(far.err)} off); all arcs: ${lines} of ${n} drawn`} : `dragged ${c1 !== c0}, undo steps ${steps}, far err ${far && Math.round(far.err)}, undone ${c3 === c0}, all arcs ${lines} of ${n}`;
    });
    await check(tag, '#160 arc handles: a 3D marker that stays small on screen, lines in the scene (+ faint through walls), a drop-line, a click gives the move gizmo (1 undo step)', async () => {
      const s = A.project().objects.find(o => A.launcherKind(o) && A.launcherKind(o) !== 'digital'); if (!s) return {skip: 'no spring / ring / ramp / panel here'};
      const cur = () => { const o = A.project().objects.find(o => o.id === s.id); return JSON.stringify([o.rotation, A.params(o)]); };
      A.selectId(s.id); A.pose(s.position.map((v, i) => v + [500, 700, 600][i]), s.position); await frames(3);
      const a = A.arcOf(A.selected()); if (!a) return 'no arc';
      const G = A.springGuide(), handle = () => G.children.find(c => c.userData.px && c.isGroup);
      const px = () => { const h = handle(); if (!h) return -1; h.updateMatrixWorld(true); const c = h.getWorldPosition(new T.Vector3()), r = new T.Vector3().setFromMatrixColumn(A.camera.matrixWorld, 0).normalize().multiplyScalar(A.HANDLE_R * h.scale.x);
        const s0 = A.screenOf(c), s1 = A.screenOf(c.clone().add(r)); return s0 && s1 ? Math.hypot(s1[0] - s0[0], s1[1] - s0[1]) : -1; };
      const land = new T.Vector3(...a.end.p);
      A.pose(land.toArray().map((v, i) => v + [6, 9, 6][i]), land.toArray()); await frames(3); const near = px();
      A.pose(land.toArray().map((v, i) => v + [2500, 3000, 2500][i]), land.toArray()); await frames(3); const farPx = px();
      const lines = G.children.filter(c => c.userData.guideLine), depthOk = lines.length > 0 && lines.every(l => l.children[0].material.depthTest && !l.children[1].material.depthTest);
      const rises = a.apex.p[1] - Math.max(a.points[0][1], a.end.p[1]) > 20, drop = a.ground || !rises || G.children.some(c => c.userData.dropLine);   // a drop-line under a top that rises
      A.pose(s.position.map((v, i) => v + [500, 700, 600][i]), s.position); await frames(3);
      const h = A.arcHandles().find(x => x.k === 'land'); if (!h) return 'no landing handle'; const hs = A.screenOf(h.p), c0 = cur(), d0 = A.undoDepth();
      await drag(hs[0], hs[1], 0, 0); await frames(1);   // a click, no drag
      const sel = A.arcSel(), onGizmo = A.gizmo.object === A.proxy && A.proxy.position.distanceTo(h.p) < 1;
      A.gizmo.dispatchEvent({type: 'dragging-changed', value: true}); A.proxy.position.x += 60; A.gizmo.dispatchEvent({type: 'objectChange'}); A.gizmo.dispatchEvent({type: 'dragging-changed', value: false}); await frames(1);
      const c1 = cur(), steps = A.undoDepth() - d0;
      while (A.undoDepth() > d0) await undo(); const back = cur() === c0;
      const lo = A.HANDLE_MIN_PX - .6, hi = A.HANDLE_MAX_PX * 1.25 + .6;
      const ok = near >= lo && near <= hi && farPx >= lo && farPx <= hi && depthOk && drop && sel === 'land' && onGizmo && c1 !== c0 && steps === 1 && back;
      return ok ? {pass: true, why: `handle ${near.toFixed(1)} px from 10 units, ${farPx.toFixed(1)} px from 4000; ${lines.length} lines depth-tested + ghost; drop-line ${a.ground || !rises ? 'n/a (no rising top)' : 'yes'}; click = gizmo on the landing, moved = 1 undo step, undone`}
        : `handle near ${near.toFixed(1)} / far ${farPx.toFixed(1)} px (${lo}-${hi}), depth ${depthOk}, drop ${drop}, sel ${sel}, gizmo ${onGizmo}, moved ${c1 !== c0}, steps ${steps}, undone ${back}`;
    });
    await check(tag, 'rail: point drag + click the line inserts a point', async () => {
      A.showPanel('add'); await wait(100); const aq = document.getElementById('aq'); aq.value = 'grind rail'; aq.dispatchEvent(new Event('input')); await wait(200);
      const tile = [...document.querySelectorAll('#vsizer .vt')].find(t => t.dataset.k === 't:Grind rail'); if (!tile) return 'no Grind rail tile';
      tile.click(); await wait(40); const [cx, cy] = centre(); await click(cx, cy); await frames(2); const it = A.selected(); if (it?.o.kind !== 'path') return 'placing a rail did not select it';
      const n = A.nodeOf(it); n.updateMatrixWorld(true); const w = i => n.localToWorld(new T.Vector3(...it.o.points[i]));
      A.pose(w(1).toArray().map((v, i) => v + [0, 420, 260][i]), w(1).toArray()); await frames(3);
      const before = JSON.stringify(it.o.points[1]), s1 = A.screenOf(w(1)); await drag(s1[0], s1[1], 40, 0); const moved = JSON.stringify(it.o.points[1]) !== before;
      const len = it.o.points.length, mid = A.screenOf(w(0).add(w(1)).multiplyScalar(.5)); await drag(mid[0], mid[1], 0, 0); const inserted = it.o.points.length === len + 1;
      await undo(3); return moved && inserted ? true : `point moved ${moved}, inserted ${inserted}`;
    });
    await check(tag, 'Digital Breakable Tile: unlinked warning + Fix (#93)', async () => {
      if (!A.project().objects.some(o => o.kind === 'native' && o.list === 7 && o.type === 225)) return {skip: 'no Trigger: Digital Breakable Tile here'};
      A.showPanel('add'); await wait(100); const aq = document.getElementById('aq'); aq.value = 'digital breakable tile'; aq.dispatchEvent(new Event('input')); await wait(250);
      const tile = [...document.querySelectorAll('#vsizer .vt')].find(x => x.dataset.k === 'c:7-215'); if (!tile) return 'no Digital Breakable Tile tile';
      const d0 = A.undoDepth(); tile.click(); await wait(40); const [cx, cy] = centre(); await click(cx, cy); await wait(80); const it = A.selected(); if (!(it?.o.list === 7 && it.o.type === 215)) return 'placing did not select the new tile';
      it.o.link = 0; A.select(it.o.id ? A.items().find(x => x.id === it.id) : null); await wait(60);
      const warned = !!document.querySelector('#tileCard .badText') && A.issueTitles().some(x => /Never appears/.test(x)); const use = document.querySelector('#tileCard [data-tlink]'); if (!use) return `warning ${warned}, no Use this Link ID button`;
      const want = +use.dataset.tlink; use.click(); await wait(80); const cur = A.project().objects.find(o => o.id === it.id);
      const fixed = cur.link === want && !A.tileUnlinked(cur) && !!document.querySelector('#tileCard .okText') && !A.issueTitles().some(x => /Never appears/.test(x));
      while (A.undoDepth() > d0) await undo(); const gone = !A.project().objects.some(o => o.id === it.id);
      return warned && fixed && gone ? {pass: true, why: `Link 0 warned; Fix -> Link ${want}; undone`} : `warned ${warned}, fixed ${fixed}, undone ${gone}`;
    });
    await check(tag, 'snap: a moved box joins a neighbour flush (#94)', async () => {
      const s0 = {...A.snapState}; A.snapState.on = true; A.snapState.dist = 30; A.applySnapUi(); const d0 = A.undoDepth();
      try {
        A.showPanel('add'); await wait(100); const aq = document.getElementById('aq'); aq.value = 'box'; aq.dispatchEvent(new Event('input')); await wait(200);
        const tile = () => [...document.querySelectorAll('#vsizer .vt')].find(x => x.dataset.k === 't:Box / platform'); if (!tile()) return 'no Box tile';
        const [cx, cy] = centre(); tile().click(); await click(cx, cy); const a1 = A.selected(); tile().click(); await click(cx + 40, cy); const b1 = A.selected();
        if (!a1 || !b1 || a1 === b1) return 'could not place two boxes';
        const na = A.nodeOf(A.items().find(x => x.id === a1.id)), bi = A.items().find(x => x.id === b1.id), nb = A.nodeOf(bi), ba = A.boxOf(A.items().find(x => x.id === a1.id));
        const gmove = (n, f) => { A.gizmo.dispatchEvent({type: 'dragging-changed', value: true}); f(n.position); A.gizmo.dispatchEvent({type: 'objectChange'}); A.gizmo.dispatchEvent({type: 'dragging-changed', value: false}); };
        const ai = A.items().find(x => x.id === a1.id); A.selectId(ai.id); await wait(40); gmove(na, p => p.y += 4000); await wait(40);   // both far above the stage: only they count
        A.selectId(bi.id); await wait(50); if (A.gizmo.object !== nb) return 'the gizmo is not on the second box'; gmove(nb, p => p.y += 4000); await wait(40);
        const ba2 = A.boxOf(ai); ba.copy(ba2);
        const bb = A.boxOf(bi), w = bb.max.x - bb.min.x, gap = 12;   // put it 12 units right of the first box, same y / z, then move it by the gizmo
        A.gizmo.dispatchEvent({type: 'dragging-changed', value: true}); nb.position.set(nb.position.x + (ba.max.x + gap) - bb.min.x, nb.position.y + (ba.min.y - bb.min.y), nb.position.z + (ba.min.z - bb.min.z)); A.gizmo.dispatchEvent({type: 'objectChange'}); A.gizmo.dispatchEvent({type: 'dragging-changed', value: false}); await wait(60);
        const b2 = A.boxOf(A.items().find(x => x.id === bi.id)), flush = Math.abs(b2.min.x - ba.max.x) < .01, info = document.getElementById('snapInfo')?.textContent || '';
        A.snapState.on = false; A.applySnapUi(); const bx = A.boxOf(A.items().find(x => x.id === bi.id)).min.x;   // Snap off: the same move keeps its gap
        gmove(nb, p => p.x += 12); await wait(40); const off = Math.abs(A.boxOf(A.items().find(x => x.id === bi.id)).min.x - (bx + 12)) < .01;
        while (A.undoDepth() > d0) await undo(); const gone = !A.project().geometry?.some(g => [a1.id, b1.id].includes(g.id));
        if (!off) return 'with Snap off the box was still pulled';
        const side = Math.abs(b2.min.y - ba.min.y) < .06;   // beside it, not stacked on it
        return flush && side && gone ? {pass: true, why: `gap ${gap} closed to < 0.01, same height (${info.trim()}); Snap off: no pull`} : !side ? `stacked: min.y ${b2.min.y.toFixed(2)} vs ${ba.min.y.toFixed(2)}` : `flush ${flush} (min.x ${b2.min.x.toFixed(2)} vs ${ba.max.x.toFixed(2)}), undone ${gone}`;
      } finally { Object.assign(A.snapState, s0); A.applySnapUi(); }
    });
    await check(tag, 'snap: N key, grid / turn steps, wall pieces join side by side (#94)', async () => {
      const s0 = {...A.snapState}; A.snapState.on = false; A.snapState.dist = 30; A.snapState.grid = 0; A.snapState.rot = 15; A.applySnapUi(); const d0 = A.undoDepth();
      try {
        await key('n'); const keyOn = A.snapState.on === true; await key('n'); const keyOff = A.snapState.on === false; await key('n');
        A.snapState.grid = 50; A.snapState.rot = 45; A.applySnapUi(); const steps = A.gizmo.translationSnap === 50 && Math.abs(A.gizmo.rotationSnap - Math.PI / 4) < 1e-6;
        A.snapState.grid = 0; A.applySnapUi(); const noGrid = A.gizmo.translationSnap === null;
        A.showPanel('add'); await wait(100); const aq = document.getElementById('aq'); aq.value = 'box'; aq.dispatchEvent(new Event('input')); await wait(200);
        const tile = () => [...document.querySelectorAll('#vsizer .vt')].find(x => x.dataset.k === 't:Box / platform'); if (!tile()) return 'no Box tile';
        const [cx, cy] = centre(); tile().click(); await click(cx - 60, cy); const a1 = A.selected(); tile().click(); await click(cx + 60, cy); const b1 = A.selected();
        if (!a1 || !b1 || a1 === b1) return 'could not place two boxes';
        const ai = A.items().find(x => x.id === a1.id), bi = A.items().find(x => x.id === b1.id), na = A.nodeOf(ai), nb = A.nodeOf(bi);
        const gmove = (n, f) => { A.gizmo.dispatchEvent({type: 'dragging-changed', value: true}); f(n.position); A.gizmo.dispatchEvent({type: 'objectChange'}); A.gizmo.dispatchEvent({type: 'dragging-changed', value: false}); };
        A.snapState.on = false; A.applySnapUi();
        for (const [it, n] of [[ai, na], [bi, nb]]) { A.selectId(it.id); await wait(40); n.scale.set(.2, 2, 1.5); it.o.scale = [.2, 2, 1.5]; gmove(n, p => p.y += 5000); await wait(40); }   // two thin upright walls, far above the stage
        A.snapState.on = true; A.applySnapUi(); A.selectId(bi.id); await wait(50); if (A.gizmo.object !== nb) return 'the gizmo is not on the second wall';
        const ba = A.boxOf(ai), bb = A.boxOf(bi);   // 15 right of it, 8 higher, 10 along: a sloppy drop beside it
        gmove(nb, p => p.set(p.x + (ba.max.x + 15) - bb.min.x, p.y + (ba.min.y + 8) - bb.min.y, p.z + (ba.min.z + 10) - bb.min.z)); await wait(60);
        const b2 = A.boxOf(bi), dx = b2.min.x - ba.max.x, dy = b2.min.y - ba.min.y, dz = b2.min.z - ba.min.z, info = (document.getElementById('snapInfo')?.textContent || '').trim();
        const joined = Math.abs(dx) < .01 && Math.abs(dy) < .01 && Math.abs(dz) < .01, apart = b2.intersectsBox(ba) && Math.max(b2.min.x, ba.min.x) >= Math.min(b2.max.x, ba.max.x) - .01;   // touching, not overlapping
        while (A.undoDepth() > d0) await undo(); const gone = !A.project().geometry?.some(g => [a1.id, b1.id].includes(g.id));
        if (!(keyOn && keyOff)) return `N key: on ${keyOn}, off ${keyOff}`; if (!steps || !noGrid) return `gizmo steps: grid ${A.gizmo.translationSnap}, turn ${A.gizmo.rotationSnap}`;
        return joined && apart && gone ? {pass: true, why: `N toggles; grid 50 / turn 45° on the gizmo; wall off by 15 / 8 / 10 joined flush face + both edges (${info})`} : `joined ${joined} (dx ${dx.toFixed(2)} dy ${dy.toFixed(2)} dz ${dz.toFixed(2)}, ${info}), touching ${apart}, undone ${gone}`;
      } finally { Object.assign(A.snapState, s0); A.applySnapUi(); }
    });
    await check(tag, 'reload: an edited unsaved fresh copy comes back (edits, camera, selection) (#96)', async () => {
      if (!target) return {skip: 'no object'}; const it = A.items().find(x => x.id === target.id); if (!it) return 'object gone';
      A.selectId(it.id); await wait(40); const n = A.nodeOf(it); A.gizmo.dispatchEvent({type: 'dragging-changed', value: true}); n.position.x += 77; A.gizmo.dispatchEvent({type: 'objectChange'}); A.gizmo.dispatchEvent({type: 'dragging-changed', value: false}); await wait(40);
      const moved = pos(it.id), cam = A.cameraNow(), id = A.project().id; A.sessionWrite(); await A.writeDraftNow(); const d = await A.draftGet(); if (!d) return 'no draft written';
      A.discard(); A.showProjects(); await wait(60); const how = await A.bootRestore(null, {auto: true}); for (let i = 0; i < 100 && !A.ready(); i++) await wait(100);
      const ok = how === 'draft' && A.project().id === id && near(pos(it.id), moved) && A.selected()?.id === it.id && A.cameraNow().every((v, i) => Math.abs(v - cam[i]) < .01) && A.dirty();
      const why = `restored ${how}, edit ${near(pos(it.id), moved)}, selection ${A.selected()?.id === it.id}, camera ${A.cameraNow().every((v, i) => Math.abs(v - cam[i]) < .01)}, still unsaved ${A.dirty()}`;
      await undo(); await A.clearDraft(); A.discard(); return ok ? {pass: true, why} : why;
    });
    await check(tag, 'In-game look on / off restores the editor view', async () => {
      A.setLook(false); await wait(300); await frames(3); const a = A.lightState();
      A.setLook(true); await wait(300); await frames(3); await wait(300); await frames(2); const on = A.gameLookOn(), b = A.lightState();
      A.editorView(); await wait(300); await frames(3); const off = !A.gameLookOn(), c = A.lightState();
      return on && off && c === a ? {pass: true, why: b !== a ? 'the look changed the lights; Editor view restored them exactly' : 'the look changed nothing here; Editor view restored'} : `on ${on}, Editor view off ${off}, restored ${c === a}`;
    });
    await check(tag, 'inspector Basic / Logic / Advanced + Rules card: add, change, near zone, delete, undo', async () => {
      const it = A.items().filter(x => x.o.kind === 'native' && x.cat === 'enemies').sort((p, q) => p.progress - q.progress)[0]; if (!it) return {skip: 'no enemy in this stage'};
      const d0 = A.undoDepth(), z0 = (A.project().triggers || []).length; A.selectId(it.id); await wait(200); const ins = document.getElementById('inspector');
      const heads = [...ins.querySelectorAll('.iGroup>h5, .iAdv>summary')].map(x => x.textContent.split(' ')[0]).join(','), adv = ins.querySelector('.iAdv'), advShut = adv && !adv.open, gameData = !!adv?.querySelector('[data-key="link"]');
      const rawHidden = [...ins.querySelectorAll('.iGroup .rawVal, .iGroup .rawName')].every(x => getComputedStyle(x).display === 'none');
      document.getElementById('ruleNew').click(); await wait(150); let r1 = A.rulesFor(it.o);
      const k = document.querySelector('[data-rk$=":0"]'); k.value = 'rings'; k.dispatchEvent(new Event('change')); await wait(150);
      const g = it.o.logic, rings = g?.nodes.some(n => n.type === 'action.rings' && n.rule === r1[0]?.spec.id) && g.nodes.some(n => n.type === 'data.math' && n.rule === r1[0]?.spec.id), ev = g?.nodes.find(n => n.ruleSpec)?.type;
      document.getElementById('ruleNewKind').value = 'near'; document.getElementById('ruleNew').click(); await wait(200);
      const zone = (A.project().triggers || []).find(z => z.ruleZone?.forObject === it.o.id), zoneOk = zone && zone.size[0] === 600 && zone.logic?.nodes.some(n => n.type === 'event.enter' && n.ruleSpec);
      const rules = A.rulesFor(it.o).length; const del = [...document.querySelectorAll('[data-rrm]')].pop(); del?.click(); await wait(150);
      const zoneGone = !(A.project().triggers || []).some(z => z.ruleZone?.forObject === it.o.id);
      while (A.undoDepth() > d0) await undo(); const back = !A.rulesFor(it.o).length && (A.project().triggers || []).length === z0;
      return heads === 'Basic,Logic,Advanced' && advShut && gameData && rawHidden && r1.length === 1 && ev === 'event.killed' && rings && zoneOk && rules === 2 && zoneGone && back
        ? {pass: true, why: `${it.name}: groups ${heads} (Advanced shut, Link ID inside, raw values hidden in Basic); rule 'destroyed -> give rings' = event.killed + rings + math; near rule = a 600 zone with On enter; delete removes the zone; undone`}
        : `groups '${heads}' shut ${advShut} gameData ${gameData} raw hidden ${rawHidden}; rules ${r1.length} ev ${ev} rings ${rings}; zone ${zoneOk}; total ${rules}; zone gone ${zoneGone}; undone ${back}`;
    });
    await check(tag, 'City Laser drawn (beam + detect radius marker)', async () => {
      const it = A.items().find(x => x.o.kind === 'native' && x.o.list === 3 && x.o.type === 234); if (!it) return {skip: 'no City Laser in this stage'};
      const n = A.nodeOf(it); let m = false; n?.traverse(c => { if (c.userData.laserMarker) m = true; }); return m ? {pass: true, why: it.name} : 'no laser marker on the node';
    });
    await check(tag, 'Logic: import validates (bad graph refused, example accepted), undo', async () => {
      const ex = await fetch('../nodes.json').then(r => r.json()).then(d => d.example); A.showWorkspace('logic'); await wait(200);
      const own = document.getElementById('lgOwnerSel'); own.value = 'stage'; own.dispatchEvent(new Event('change')); await wait(100);
      document.getElementById('lgImport').click(); const txt = document.getElementById('lgJsonText'), rep = () => document.getElementById('lgJsonReport').innerText;
      txt.value = JSON.stringify({version: 1, nodes: [{id: 'a', type: 'no.such.node'}], links: []}); document.getElementById('lgJsonCheck').click(); const bad = /error/i.test(rep());
      const g = ex?.graph || ex; txt.value = JSON.stringify(g); document.getElementById('lgJsonCheck').click(); const good = /Valid/.test(rep());
      const before = JSON.stringify(A.project().logic ?? null); document.getElementById('lgJsonApply').click(); await wait(100); const applied = (A.project().logic?.nodes?.length || 0) === (g?.nodes?.length || -1);
      await undo(); const back = JSON.stringify(A.project().logic ?? null) === before; document.getElementById('lgJson').hidden = true; A.showWorkspace('layout'); await wait(100);
      return bad && good && applied && back ? true : `bad refused ${bad}, example valid ${good}, applied ${applied}, undone ${back}`;
    });
    await check(tag, 'Logic: Templates insert a wired graph (stage: Countdown), runtime-only nodes marked, undo', async () => {
      A.showWorkspace('logic'); await wait(200); const own = document.getElementById('lgOwnerSel'); own.value = 'stage'; own.dispatchEvent(new Event('change')); await wait(100);
      const before = JSON.stringify(A.project().logic ?? null), n0 = A.project().logic?.nodes?.length || 0, l0 = A.project().logic?.links?.length || 0;
      document.getElementById('lgTpl').click(); await wait(60); const items = [...document.querySelectorAll('#lgTplList .lgTpl')], offs = items.filter(b => b.classList.contains('off')).length;
      items.find(b => /Countdown/.test(b.innerText))?.click(); await wait(150);
      const lg = A.project().logic, added = (lg?.nodes?.length || 0) - n0, wires = (lg?.links?.length || 0) - l0, status = document.getElementById('lgStatus')?.textContent || '';
      document.getElementById('lgAdd').click(); await wait(60); const q = document.getElementById('lgQ'); q.value = 'show text'; q.dispatchEvent(new Event('input')); await wait(60);
      const soon = !!document.querySelector('#lgList .lgItem em.soon'); document.getElementById('lgSearch').hidden = true;
      await undo(); const back = JSON.stringify(A.project().logic ?? null) === before; A.showWorkspace('layout'); await wait(100);
      return items.length >= 6 && offs >= 1 && added === 4 && wires === 3 && /sidecar v4/.test(status) && back ? {pass: true, why: `${items.length} templates (${offs} for other owners), Countdown = 4 nodes / 3 wires, 'Show text' ${soon ? "badged 'needs game'" : 'runs in the game now (no badge)'}, undone`} : `templates ${items.length} (off ${offs}), added ${added} nodes / ${wires} wires, status '${status}', badge ${soon}, undone ${back}`;
    });
    await check(tag, 'Logic: Find in graph, Align, My templates (save, insert, delete), undo', async () => {
      A.showWorkspace('logic'); await wait(200); const own = document.getElementById('lgOwnerSel'); own.value = 'stage'; own.dispatchEvent(new Event('change')); await wait(100);
      const before = JSON.stringify(A.project().logic ?? null), d0 = A.undoDepth();
      document.getElementById('lgTpl').click(); await wait(60); [...document.querySelectorAll('#lgTplList .lgTpl')].find(b => /Countdown/.test(b.innerText))?.click(); await wait(150);
      const g = A.logic.graph('stage'), ids = g.nodes.slice(-4).map(n => n.id);
      const fq = document.getElementById('lgFindQ'); fq.value = 'timer'; fq.dispatchEvent(new Event('input')); await wait(80); const found = document.getElementById('lgFindN').textContent;
      fq.value = ''; fq.dispatchEvent(new Event('input'));
      A.logic.selectNodes(ids); const al = document.getElementById('lgAlign'); al.value = 'top'; al.dispatchEvent(new Event('change')); await wait(80);
      const g2 = A.logic.graph('stage'), tops = new Set(g2.nodes.filter(n => ids.includes(n.id)).map(n => n.y)).size;
      A.logic.selectNodes(ids); document.getElementById('lgTpl').click(); await wait(60); const nm = document.getElementById('lgMyName'); nm.value = '[test] selftest tpl'; document.getElementById('lgMySaveB').click(); await wait(80);
      const saved = A.logic.myTemplates().some(x => x.name === '[test] selftest tpl'); const n1 = A.logic.graph('stage').nodes.length;
      document.querySelector('#lgMyList [data-mi]')?.click(); await wait(150); const n2 = A.logic.graph('stage').nodes.length;
      document.getElementById('lgTpl').click(); await wait(60); const di = [...document.querySelectorAll('#lgMyList [data-mdel]')].pop(); di?.click(); await wait(60); document.getElementById('lgTplBox').hidden = true;
      const gone = !A.logic.myTemplates().some(x => x.name === '[test] selftest tpl');
      while (A.undoDepth() > d0) await undo(); const back = JSON.stringify(A.project().logic ?? null) === before; A.showWorkspace('layout'); await wait(100);
      return /\d+ \/ \d+/.test(found) && tops === 1 && saved && n2 === n1 + 4 && gone && back ? {pass: true, why: `find 'timer' ${found}, align top -> 1 row, saved + inserted (+4 nodes) + deleted, undone`} : `find ${found}, rows ${tops}, saved ${saved}, inserted ${n2 - n1}, deleted ${gone}, undone ${back}`;
    });
    await check(tag, 'prefab: save + insert + delete', async () => {
      const two = A.items().filter(it => it.o.kind === 'native').slice(0, 2); if (two.length < 2) return {skip: 'fewer than 2 objects'};
      A.setSelection(two); const name = '[test] selftest ' + S; await A.savePrefab(name); const pf = A.prefabs().find(p => p.name === A.prefabName(name)); if (!pf) return 'the prefab was not saved';
      const n0 = A.items().length; A.insertPrefab(pf, A.centrePoint()); const n1 = A.items().length; await undo(); await A.deletePrefabNamed(pf.name); const gone = !A.prefabs().some(p => p.name === pf.name);
      return n1 === n0 + 2 && A.items().length === n0 && gone ? true : `inserted ${n1 - n0}, after undo ${A.items().length - n0}, deleted ${gone}`;
    });
    await check(tag, 'lighting card: override + Reset to auto', async () => {
      A.showPanel('settings'); for (let i = 0; i < 40 && !document.querySelector('#autoLight [data-al]'); i++) await wait(100);
      const inp = document.querySelector('#autoLight input[data-al="sun_strength"]'); if (!inp) return {skip: document.getElementById('autoLight')?.innerText.slice(0, 80) || 'no lighting card'};
      inp.value = '0.42'; inp.dispatchEvent(new Event('change')); await wait(80); const set = A.project().lighting?.auto?.overrides?.sun_strength === .42;
      document.querySelector('#autoLight [data-alreset="sun_strength"]')?.click(); await wait(80); const reset = !('sun_strength' in (A.project().lighting?.auto?.overrides || {}));
      return set && reset ? true : `override set ${set}, reset ${reset}`;
    });
    await check(tag, 'lighting Look: preset name, slider override, Reset, tickboxes, preview, undo', async () => {
      A.showPanel('settings'); for (let i = 0; i < 40 && !document.getElementById('lookPreset'); i++) await wait(100); const sel = document.getElementById('lookPreset'); if (!sel) return 'no Look picker';
      const d0 = A.undoDepth(); sel.value = 'dramatic'; sel.dispatchEvent(new Event('change')); await wait(80); const p1 = A.lookOf(), name = A.project().lighting?.look;
      const ex = document.querySelector('#autoLight input.alNum[data-lk="exposure"]'); ex.value = '0.7'; ex.dispatchEvent(new Event('change')); await wait(80); const p2 = A.lookOf(), ov = A.project().lighting?.auto?.overrides?.exposure;
      document.querySelector('#autoLight [data-lkreset="exposure"]')?.click(); await wait(80); const p3 = A.lookOf();
      const w = document.getElementById('lookWorld'), c = document.getElementById('lookCast'), blob = document.getElementById('lookBlob'); const def = w.checked && c.checked && !blob.checked; c.checked = false; c.dispatchEvent(new Event('change')); await wait(60); const castOff = !A.lookOf().characters_cast_shadows && A.project().lighting?.auto?.overrides?.characters_cast_shadows === undefined;
      document.getElementById('lookCast').checked = true; document.getElementById('lookCast').dispatchEvent(new Event('change')); await wait(60); const castOv = castOff && A.lookOf().characters_cast_shadows ? 'preview only' : 'wrong';
      const auto = document.querySelector('#autoLight details.autoVals'), autoTxt = auto?.textContent || '';   // folded: innerText leaves the rows out
      A.setLook(true); await wait(300); await frames(3); const filt = A.canvasFilter(); A.setLook(false); await wait(300); await frames(2); const filtOff = A.canvasFilter();
      while (A.undoDepth() > d0) await undo(); const back = A.project().lighting?.look == null && A.project().lighting?.auto?.overrides?.exposure === undefined;
      const ok = name === 'dramatic' && p1.preset === 'dramatic' && p2.exposure === .7 && ov === .7 && p3.exposure === p1.exposure && def && castOv === 'preview only' && (!A.lookOf().generated || /Sun shading/.test(autoTxt)) && (p1.saturation === 1 ? true : /saturate/.test(filt)) && !filtOff && back;
      return ok ? {pass: true, why: `look 'dramatic' (${p1.generated ? 'generated' : 'fallback'} exposure ${p1.exposure}) -> override 0.7 -> Reset; launcher defaults World on / Cast on / blob off, Cast toggle ${castOv}; Auto values ${auto ? 'shown' : 'none'}; preview ${filt || 'none'}; undone`} : `name ${name}, override ${ov}, reset ${p3.exposure} vs ${p1.exposure}, defaults ${def}, cast ${castOv}, auto '${autoTxt.slice(0, 60)}', preview '${filt}' / '${filtOff}', undone ${back}`;
    });
    await check(tag, "lighting: launcher 'Replace the game's lighting' off by default, preview only (not in the project)", async () => {
      const box = document.getElementById('lookReplace'); if (!box) return 'no Replace tickbox'; const def = !box.checked && !A.lookOf().replace, d0 = A.undoDepth(), j0 = JSON.stringify(A.project().lighting || {});
      box.checked = true; box.dispatchEvent(new Event('change')); await wait(60); const on = A.lookOf().replace === true, same = JSON.stringify(A.project().lighting || {}) === j0 && A.undoDepth() === d0;
      const b2 = document.getElementById('lookReplace'); b2.checked = false; b2.dispatchEvent(new Event('change')); await wait(60);
      return def && on && same && !A.lookOf().replace ? true : `default off ${def}, ticked ${on}, project untouched ${same}, back off ${!A.lookOf().replace}`;
    });
    await check(tag, "draw distance: 'Wider' + Custom + active count preview, undo", async () => {
      const w = document.getElementById('ddWider'); if (!w) return 'no Draw distance card'; const d0 = A.undoDepth(), [n1] = A.activeAt(null);
      w.checked = true; w.dispatchEvent(new Event('change')); await wait(80); const wider = A.project().activationScale, [nw] = A.activeAt('wider'), prev = document.getElementById('ddPrev')?.textContent || '';   // the card is folded under Advanced (#99): innerText would be empty
      const c = document.getElementById('ddCustom'); c.checked = true; c.dispatchEvent(new Event('change')); await wait(80); const r = document.getElementById('ddRange'); r.value = '200'; r.dispatchEvent(new Event('input')); r.dispatchEvent(new Event('change')); await wait(80);
      const two = A.project().activationScale, [n2] = A.activeAt(2); while (A.undoDepth() > d0) await undo(); const back = A.project().activationScale == null;
      return wider === 'wider' && two === 2 && nw >= n1 && n2 >= n1 && /with Wider/.test(prev) && back ? {pass: true, why: `${n1} active as the game has them, ${nw} with Wider, ${n2} at 2x around the camera; undone`} : `wider ${wider}, custom ${two}, counts ${n1} / ${nw} / ${n2}, preview '${prev.slice(0, 80)}', undone ${back}`;
    });
    await check(tag, 'tilesets (#128): Add > Tilesets section, a piece snaps its corner to the 50 grid in 90° turns, one group, one undo', async () => {
      A.showPanel('add'); await wait(150); const aq = document.getElementById('aq'); aq.value = 'tileset'; aq.dispatchEvent(new Event('input')); await wait(300);
      const tiles = [...document.querySelectorAll('#vsizer .vt')].filter(t => /^t:/.test(t.dataset.k)), full = A.tilesetKits().flatMap(k => (k.pieces || []).map(pc => [k, pc])).find(([, pc]) => pc.size && pc.cells && Math.abs(pc.size[0] - pc.cells[0] * A.TILE_GRID) < .5 && Math.abs(pc.size[2] - pc.cells[2] * A.TILE_GRID) < .5) || [A.tilesetKits()[0], A.tilesetKits()[0]?.pieces?.[0]], [kit, pc] = full;   // a piece that fills its cells: its outer corner is the pivot
      aq.value = ''; aq.dispatchEvent(new Event('input')); if (!pc) return 'no tileset kit';
      const snap0 = {...A.tileSnap()}, d0 = A.undoDepth(), g0 = A.project().geometry.length;
      await A.placeTile({name: pc.name, tilePiece: {kit: kit.kit, piece: pc.id}, piece: pc}, new T.Vector3(1237.4, 20, -862.9)); await wait(200);
      const added = A.project().geometry.slice(g0), o = added[0], yaw = o?.tile?.yaw, steps = A.undoDepth() - d0;
      const bb = new T.Box3(); for (const x of added) { const it = A.items().find(i => i.id === x.id); if (it) bb.union(A.boxOf(it)); }
      const off = v => Math.min(Math.abs(v - Math.round(v / A.TILE_GRID) * A.TILE_GRID), 99), onGrid = !bb.isEmpty() && off(bb.min.x) < 1.5 && off(bb.min.z) < 1.5;   // the piece's outer corner on the grid
      const grp = new Set(added.map(x => x.group)).size, snapNow = A.tileSnap().grid;
      while (A.undoDepth() > d0) await undo(); Object.assign(A.tileSnap(), snap0); A.applySnapUi();
      return tiles.length && added.length && onGrid && yaw % 90 === 0 && grp === 1 && steps === 1 && snapNow > 0 && A.project().geometry.length === g0 ? {pass: true, why: `${tiles.length} tile(s) in Add (${kit.title || kit.kit}), its corner at ${bb.min.x.toFixed(1)}, ${bb.min.z.toFixed(1)}, facing ${yaw}°, ${added.length} piece(s) in one group, snap grid ${snapNow}, 1 undo step`} : `tiles ${tiles.length}, added ${added.length}, corner on grid ${onGrid} (${bb.min.x.toFixed(1)}, ${bb.min.z.toFixed(1)}), yaw ${yaw}, groups ${grp}, steps ${steps}, snap ${snapNow}`;
    });
    await check(tag, 'tile snap (#128): a road tile moved 12 units from another joins it flush (the tile moves as one group)', async () => {
      const kits = A.tilesetKits(), pick = (k, id) => { const kit = kits.find(x => x.kit === k), pc = kit?.pieces?.find(x => x.id === id); return pc && {name: pc.name, tilePiece: {kit: k, piece: id}, piece: pc}; };
      const row = pick('roads', 'road_straight') || (() => { const k = kits[0], pc = k.pieces[0]; return {name: pc.name, tilePiece: {kit: k.kit, piece: pc.id}, piece: pc}; })();
      const s0 = {...A.snapState}, d0 = A.undoDepth(), g0 = A.project().geometry.length;
      try {
        A.snapState.on = true; A.snapState.dist = 30; A.snapState.grid = 0; A.applySnapUi();
        await A.placeTile(row, new T.Vector3(4000, 4000, 4000)); await wait(150); const ga = A.project().geometry.slice(g0).map(o => o.id);
        await A.placeTile(row, new T.Vector3(4600, 4000, 4000)); await wait(150); const gb = A.project().geometry.slice(g0 + ga.length).map(o => o.id);
        A.snapState.grid = 0; A.applySnapUi();   // the first tile set the grid to 50: off again, so only the magnet acts
        const bbOf = ids => { const b = new T.Box3(); for (const id of ids) { const it = A.items().find(x => x.id === id); if (it) b.union(A.boxOf(it)); } return b; };
        const ba = bbOf(ga), bb = bbOf(gb); if (ba.isEmpty() || bb.isEmpty()) return 'the tiles were not placed';
        A.setSelection(A.items().filter(x => gb.includes(x.id))); await wait(80); if (A.gizmo.object !== A.proxy) return 'the gizmo is not on the tile group';
        A.gizmo.dispatchEvent({type: 'dragging-changed', value: true}); A.proxy.position.x += (ba.max.x + 12) - bb.min.x; A.proxy.position.z += ba.min.z - bb.min.z; A.proxy.position.y += ba.min.y - bb.min.y;
        A.gizmo.dispatchEvent({type: 'objectChange'}); A.gizmo.dispatchEvent({type: 'dragging-changed', value: false}); await wait(60);
        const b2 = bbOf(gb), gap = b2.min.x - ba.max.x, info = document.getElementById('snapInfo')?.textContent || '';
        return Math.abs(gap) < .05 && Math.abs(b2.min.z - ba.min.z) < .05 && Math.abs(b2.min.y - ba.min.y) < .05 ? {pass: true, why: `${row.name}: gap 12 closed to ${gap.toFixed(3)} (${gb.length} part(s) moved together; ${info.trim()})`} : `gap ${gap.toFixed(2)}, dz ${(b2.min.z - ba.min.z).toFixed(2)}, dy ${(b2.min.y - ba.min.y).toFixed(2)}`;
      } finally { while (A.undoDepth() > d0) await undo(); Object.assign(A.snapState, s0); A.applySnapUi(); }
    });
    await check(tag, 'Tilesets tab (#128): kit chips + kit: search narrow the tiles; armed, a ghost sits on the grid, R / Shift R turn it 90°, the placed tile takes the turn, Esc drops it', async () => {
      for (let i = 0; i < 40 && A.tilesetKits()[0]?.placeholder; i++) await wait(100);   // the kit list arrives from the server
      const kits = A.tilesetKits(), kit = kits.find(k => k.kit === 'roads') || kits[0]; if (!kit?.pieces?.length) return 'no tileset kit';
      const P = A.addPrefs, p0 = {cat: P.cat, kit: P.kit}, d0 = A.undoDepth(), g0 = A.project().geometry.length, shown = () => +((document.getElementById('addStatus')?.textContent || '').match(/(\d+) shown/) || [0, -1])[1];
      const key = e => window.dispatchEvent(new KeyboardEvent('keydown', {key: e.key, shiftKey: !!e.shift, bubbles: true}));
      try {
        A.showPanel('add'); await wait(150); const aq = document.getElementById('aq'); aq.value = ''; aq.dispatchEvent(new Event('input')); await wait(200);
        document.querySelector('#addCats [data-c="tilesets"]')?.click(); await wait(200);
        const chips = [...document.querySelectorAll('#kitChips [data-kit]')]; if (chips.length !== kits.length + 1) return `${chips.length} kit chips for ${kits.length} kits`;
        chips.find(b => b.dataset.kit === kit.kit).click(); await wait(200); const nKit = shown(), ph = document.getElementById('aq').placeholder;
        document.querySelector('#kitChips [data-kit=""]')?.click(); await wait(150); const nAll = shown();
        document.querySelector('#addCats [data-c="all"]')?.click(); await wait(150); const aq2 = document.getElementById('aq'); aq2.value = 'kit:' + kit.kit; aq2.dispatchEvent(new Event('input')); await wait(300); const nTok = shown();
        aq2.value = ''; aq2.dispatchEvent(new Event('input')); await wait(150); aq2.blur();
        if (nKit !== kit.pieces.length || nTok !== kit.pieces.length || nAll < nKit || !/kit:/.test(ph)) return `kit chip ${nKit}, kit: search ${nTok}, all kits ${nAll} (kit has ${kit.pieces.length}), placeholder '${ph}'`;
        const pc = kit.pieces[0], row = {key: 'selftest-tile', name: pc.name, tilePiece: {kit: kit.kit, piece: pc.id}, piece: pc, cat: 'tilesets'};
        A.armRow(row); await wait(80); const G = A.tileGhost, gp = G.position.clone(), on = v => Math.abs(v - Math.round(v / A.TILE_GRID) * A.TILE_GRID) < .01;
        const bx = G.children[0]?.geometry; bx?.computeBoundingBox(); const gs = bx ? bx.boundingBox.getSize(new T.Vector3()) : null, want = pc.size || pc.cells.map(c => c * A.TILE_GRID);
        if (!G.visible || !on(gp.x) || !on(gp.z) || !gs || Math.abs(gs.x - Math.max(want[0], 1)) > .5 || Math.abs(gs.z - Math.max(want[2], 1)) > .5) return `ghost visible ${G.visible} at ${gp.x.toFixed(1)}, ${gp.z.toFixed(1)}, size ${gs && gs.toArray().map(v => v.toFixed(0))} (piece ${want})`;
        const y0 = A.tileYaw(); key({key: 'r'}); await wait(40); const y1 = A.tileYaw(), r1 = Math.round(T.MathUtils.radToDeg(G.rotation.y)); key({key: 'R', shift: true}); await wait(40); const y2 = A.tileYaw(); key({key: 'r'}); await wait(40);
        await A.placeTile(A.placing(), new T.Vector3(gp.x + 3, gp.y, gp.z - 4)); await wait(150); const placed = A.project().geometry.slice(g0)[0]?.tile?.yaw;
        key({key: 'Escape'}); await wait(80); const gone = !G.visible && !A.placing();
        return y1 === (y0 + 90) % 360 && ((r1 % 360) + 360) % 360 === y1 && y2 === y0 && placed === y1 && gone ? {pass: true, why: `${kit.title || kit.kit}: chip ${nKit} / kit: ${nTok} of ${nAll} tiles; ghost ${gs.x.toFixed(0)} × ${gs.z.toFixed(0)} at ${gp.x}, ${gp.z}; R ${y0}° -> ${y1}°, Shift R back, placed at ${placed}°; Esc cleared it`} : `yaw ${y0} -> R ${y1} (ghost ${r1}) -> Shift R ${y2}, placed ${placed}, after Esc ghost ${G.visible} placing ${!!A.placing()}`;
      } finally { A.cancelPlacing(); while (A.undoDepth() > d0) await undo(); Object.assign(P, p0); A.saveAddPrefs(); if (A.panel?.() === 'add') A.showPanel('add'); }
    });
    await check(tag, 'project rename (Stage panel): the name changes, the top bar follows, Ctrl Z brings the old name back', async () => {
      A.showPanel('settings'); await wait(200); const el = document.getElementById('projName'); if (!el) return 'no Project name field';
      const old = A.project().name, d0 = A.undoDepth(); el.value = old + ' renamed'; el.dispatchEvent(new Event('change')); await wait(60);
      const now = A.project().name, top = document.getElementById('projectTitle')?.textContent; while (A.undoDepth() > d0) await undo(); const back = A.project().name;
      return now === old + ' renamed' && top === now && back === old ? {pass: true, why: `"${now}" -> undo -> "${back}"`} : `renamed ${now}, top bar ${top}, after undo ${back}`;
    });
    await check(tag, 'Sound effects (Stage panel): This stage only / another stage / Auto, undo', async () => {
      A.showPanel('settings'); await wait(200); const sel = () => document.getElementById('soundSel'); if (!sel()) return 'no Sound effects choice';
      const d0 = A.undoDepth(), pick = v => { sel().value = v; sel().dispatchEvent(new Event('change')); };
      pick('none'); await wait(60); const a = A.project().stageFiles?.sound; const other = [...sel().options].find(o => /^\d+$/.test(o.value) && +o.value !== A.project().stage);
      if (other) { pick(other.value); await wait(60); } const b = A.project().stageFiles?.sound; pick('auto'); await wait(60); const c = A.project().stageFiles?.sound;
      while (A.undoDepth() > d0) await undo();
      return a === 'none' && (!other || b === +other.value) && c === undefined ? {pass: true, why: `none -> ${b} -> auto (key removed), ${sel()?.options.length} choices, undone`} : `none ${a}, stage ${b}, auto ${c}`;
    });
    await check(tag, 'stage settings: pick another fog + Reset', async () => {
      const sel = document.querySelector('#stageFiles [data-sf="fog"]'); if (!sel) return 'no Stage files card';
      const other = [...sel.options].find(o => o.value && o.value !== 'none' && +o.value !== S); sel.value = other.value; sel.dispatchEvent(new Event('change')); await wait(200);
      const picked = A.project().stageFiles?.fog === +other.value; document.querySelector('#stageFiles [data-sfreset="fog"]')?.click(); await wait(200);
      return picked && A.project().stageFiles?.fog == null ? true : `picked ${picked}, reset ${A.project().stageFiles?.fog == null}`;
    });
    if (roundTrip && S === stages[0]) await check(tag, 'save + reload round trip ([test] project)', async () => {
      const name = `[test] selftest ${new Date().toISOString().slice(0, 16).replace('T', ' ')}`; A.project().name = name; A.project().id = crypto.randomUUID().replace(/-/g, ''); A.markChanged();   // own id: the server gives every fresh copy of a stage the same id within a session
      // a first save + reopen lets the server make its one-time migrations of a fresh copy (e.g. the start yaw to degrees,
      // playerStart.yawUnits); the second save + reopen must then give back exactly the same editable state
      const diff = (a, b) => { const x = JSON.parse(a), y = JSON.parse(b), out = [];
        for (const k of new Set([...Object.keys(x), ...Object.keys(y)])) { if (JSON.stringify(x[k]) === JSON.stringify(y[k])) continue;
          if (Array.isArray(x[k]) && Array.isArray(y[k])) { const i = x[k].findIndex((e, n) => JSON.stringify(e) !== JSON.stringify(y[k][n])); const e1 = x[k][i], e2 = y[k][i];
            out.push(`${k}[${i}]${e1?.id ? ' ' + e1.id : ''}: ${Object.keys({...e1, ...e2}).filter(f => JSON.stringify(e1?.[f]) !== JSON.stringify(e2?.[f])).map(f => `${f} ${JSON.stringify(e1?.[f])?.slice(0, 60)} -> ${JSON.stringify(e2?.[f])?.slice(0, 60)}`).join(', ')}${x[k].length !== y[k].length ? ` (length ${x[k].length} -> ${y[k].length})` : ''}`); }
          else out.push(`${k}: ${JSON.stringify(x[k])?.slice(0, 120)} -> ${JSON.stringify(y[k])?.slice(0, 120)}`); }
        return out; };
      await A.save(); if (A.dirty()) return 'save failed'; const id = A.project().id, first = A.editable(); await A.openProject(id); for (let i = 0; i < 100 && !A.ready(); i++) await wait(100);
      const migrated = diff(first, A.editable());
      A.markChanged(); await A.save(); if (A.dirty()) return 'the second save failed'; const before = A.editable(); await A.openProject(id); for (let i = 0; i < 100 && !A.ready(); i++) await wait(100);
      const d2 = diff(before, A.editable()); if (d2.length) return `the reloaded project differs: ${d2.join('; ')}`;
      A.sessionWrite(); A.showProjects(); await wait(60); const how = await A.bootRestore(null, {auto: true}); for (let i = 0; i < 100 && !A.ready(); i++) await wait(100);   // #96: a reload of a saved project returns to it
      if (how !== 'project' || A.project().id !== id) return `reload went to ${how} (${A.project()?.id})`;
      A.showProjects(); const list = A.emptyStateShown(); return list ? {pass: true, why: `${id}; second round trip identical${migrated.length ? ` (first load migrated: ${migrated.join('; ').slice(0, 120)})` : ''}; a reload returns to it; the stage list opens on demand`} : 'the stage list did not open';
    });
    A.discard();
  }
  // B3: a stage from scratch (the Blank stage template): New blank stage, the Custom stage card (goals, cover upload),
  // Validate (saves first) and the package download. Its project is named 'selftest blank ...' (--cleanup)
  if (roundTrip) await check('-', 'custom stage from scratch: New blank stage, card, cover, Validate, Export', async () => {
    const name = `[test] selftest blank ${new Date().toISOString().slice(0, 16).replace('T', ' ')}`, run = A.newBlankStage();   // the name goes in the app's own dialog (UX pass: no window.prompt)
    await wait(150); const ask = document.querySelector('#chooseBox .askIn'); if (ask) { ask.value = name; document.querySelector('#chooseBox [data-v="ok"]').click(); } await run;
    for (let i = 0; i < 100 && !(A.ready() && A.project()?.name === name); i++) await wait(100); if (A.project()?.name !== name) return 'the blank stage did not open';
    for (let i = 0; i < 40 && !document.querySelector('#customStage .csCard'); i++) await wait(100); if (!document.querySelector('#customStage .csCard')) return 'no Custom stage card';
    const goals = ['neutral', 'dark', 'hero'].map(r => document.querySelector(`[data-mso="${r}"]`)?.value).join(','), notes = [...document.querySelectorAll('#missionsCard .msRoute small')].map(x => x.className).join(',');   // #99: goals live in the Missions card
    const cv = Object.assign(document.createElement('canvas'), {width: 64, height: 36}), g = cv.getContext('2d'); g.fillStyle = '#c33'; g.fillRect(0, 0, 64, 36);
    const blob = await new Promise(r => cv.toBlob(r, 'image/png')), dt = new DataTransfer(); dt.items.add(new File([blob], 'cover.png', {type: 'image/png'}));
    A.csPicker.files = dt.files; await A.csPicker.onchange(); await wait(300);
    const cover = A.project().customStage?.cover, img = document.querySelector('#customStage .csCover img');
    document.getElementById('csValidate').click(); for (let i = 0; i < 300 && !document.querySelector('.csCheck'); i++) await wait(100);   // Validate saves first: up to 30 s late in a long run
    const chk = document.querySelector('.csCheck')?.innerText || '', ok = /Playable/.test(chk);
    const pk = await fetch('/api/package?id=' + encodeURIComponent(A.project().id)), size = (await pk.arrayBuffer()).byteLength;
    const res = goals === 'goal,defeat,defeat' && !/badText/.test(notes) && cover && img && ok && pk.ok && size > 1000;
    return res ? {pass: true, why: `goals ${goals}; cover ${cover}; Validate: ${chk.split('\n').filter(Boolean).length - 1} lines, playable; package ${size} bytes`} : `goals ${goals}, notes ${notes}, cover ${cover} img ${!!img}, validate '${chk.slice(0, 120)}', package ${pk.status} ${size}`;
  });
  // #119 Import level / model: a small OBJ (a 400 x 400 floor + a 40-unit box, two materials) through the
  // POST /api/import-geometry, applied as one undo step (pieces, materials, chunk boxes), surface per material, undo
  if (roundTrip) await check('-', 'import level (#119): OBJ through /api/import-geometry, pieces + chunk boxes, surface per material, one undo', async () => {
    const p = A.project(); if (!p || p.mode !== 'new') return {skip: 'needs the blank stage of the previous check'};
    const obj = ['mtllib none.mtl', 'v -200 0 -200', 'v 200 0 -200', 'v 200 0 200', 'v -200 0 200', 'vt 0 0', 'vt 1 0', 'vt 1 1', 'vt 0 1', 'usemtl floor', 'f 1/1 4/4 3/3 2/2',
      'v -20 0 -20', 'v 20 0 -20', 'v 20 40 -20', 'v -20 40 -20', 'v -20 0 20', 'v 20 0 20', 'v 20 40 20', 'v -20 40 20', 'usemtl box',
      'f 5 6 7 8', 'f 9 12 11 10', 'f 5 9 10 6', 'f 6 10 11 7', 'f 7 11 12 8', 'f 8 12 9 5'].join('\n');
    const g0 = p.geometry.length, cb0 = JSON.stringify(p.chunkBoxes ?? null), d0 = A.undoDepth();
    A.openLevelImport([new File([obj], 'selftest.obj')]); await wait(50);   // the real dialog and its Import button (UX pass: the button threw before, the test went around it)
    const set = (id, v) => { const el = document.getElementById(id); if (!el) return false; el.value = v; return true; };
    if (!set('impUnits', 'game') || !set('impPos', '0, 0, 0') || !set('impChunks', '1, 2')) return 'the import dialog fields are missing';
    document.getElementById('imp119Go').click(); for (let i = 0; i < 100 && p.geometry.length === g0; i++) await wait(100);
    if (p.geometry.length === g0) return 'Import (the dialog button) added nothing: ' + (A.errors().slice(-1)[0]?.msg || 'no error'); await wait(150); const data = A.lastImport() || {};
    const added = A.project().geometry.length - g0, boxes = Object.keys(A.project().chunkBoxes || {}).length, grp = new Set(A.project().geometry.slice(g0).map(o => o.group)).size;
    const sel = document.querySelector('#imp119Body [data-impmat]'); let noColl = false;
    if (sel) { sel.value = '0'; sel.dispatchEvent(new Event('change')); await wait(60); noColl = A.project().geometry.slice(g0).some(o => o.material === sel.dataset.impmat && o.collision === false); }
    const stats = document.getElementById('imp119Body')?.textContent || ''; document.getElementById('imp119').hidden = true;
    while (A.undoDepth() > d0) await undo(); const back = A.project().geometry.length === g0 && JSON.stringify(A.project().chunkBoxes ?? null) === cb0;
    return added >= 2 && boxes >= 1 && grp === 1 && noColl && /pieces/.test(stats) && back ? {pass: true, why: `${added} pieces (one group), ${data.stats?.triangles} triangles, chunk boxes ${Object.keys(data.chunkBoxes || {}).join(',')}, a material set to no collision, undone`} : `added ${added}, boxes ${boxes}, groups ${grp}, no-collision ${noColl}, stats '${stats.slice(0, 60)}', undone ${back}`;
  });
  // synthetic pointers cannot be captured (setPointerCapture throws): a test limit, not an editor error - left out here
  await check('-', "'Editor updated, press Ctrl R' notice (simulated file change)", async () => {
    const quiet = await A.uiFilesCheck(false), hit = await A.uiFilesCheck(true);
    return !quiet.changed.length && hit.changed.length && hit.shown ? {pass: true, why: `no notice while unchanged; a changed size of ${hit.changed.length} files shows the chip`} : `unchanged -> ${quiet.changed.join(', ') || 'none'}, changed -> ${hit.changed.length} files, chip ${hit.shown}`;
  });
  await check('-', 'no script errors during the whole run', () => { const e = A.errors().slice(errs0).filter(x => !/setPointerCapture|releasePointerCapture/.test(x.msg)); return e.length ? `${e.length}: ${e.slice(0, 3).map(x => x.msg).join(' | ')}` : true; });
  A.orbit.enabled = true; A.setLook(look0); A.discard(); await A.clearDraft(); if (prev) await A.openProject(prev); else { A.showProjects(); A.clearSession(); }   // no test draft / session left behind
  // one line for AGENTS-LIVE / a DONE line: totals, stages, and every FAIL with its reason
  const n = k => out.filter(r => r.result === k).length, fails = out.filter(r => r.result === 'FAIL');
  out.summary = `UI 2 self-test ${new Date().toISOString().slice(0, 16).replace('T', ' ')}: ${out.length} checks = ${n('pass')} pass, ${n('FAIL')} fail, ${n('skip')} skip on ${stages.join(' / ')}${fails.length ? ' · FAIL: ' + fails.map(r => `${r.stage.split(' ')[0]} ${r.check} (${r.why.slice(0, 80)})`).join('; ') : ''}`;
  if (log) { console.table(out); console.log(out.summary); }
  return out;
}
