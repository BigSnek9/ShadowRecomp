// #124 launch arcs (springs, dash rings, ramps, panels): the measured model (docs/stage-workshop/research/
// spring-arcs-124.md, agents/claude/scripts/arc_model.py). 60 Hz explicit Euler from the launch origin:
//   p += dt * v ;  v += dt * ((0, -320, 0) + push * [t < NoControlTime] - (3 vx, 1 vy, 3 vz))
// Hands off the stick. No DOM / three.js here (plain [x, y, z] arrays), so the self-test can call it directly.
export const LAUNCHERS = {'0-1': 'spring', '0-2': 'wide', '7-222': 'digital', '0-6': 'ring', '0-4': 'ramp', '0-3': 'panel'};
export const LAUNCHER_NAMES = {spring: 'Spring', wide: 'Wide Spring', digital: 'Digital Spring', ring: 'Dash Ring', ramp: 'Dash Ramp', panel: 'Dash Panel'};
const G = 320, KH = 3, KY = 1, PUSH = 214, DT = 1 / 60, RUN_CAP = 750, RUN_TAU = .085, BRAKE = .15;
export const ARC_SECONDS = 3, S_MAX = 20;
const R = Math.PI / 180;
const rx = (a, v) => { const c = Math.cos(a), s = Math.sin(a); return [v[0], v[1] * c - v[2] * s, v[1] * s + v[2] * c]; };
const ry = (a, v) => { const c = Math.cos(a), s = Math.sin(a); return [v[0] * c + v[2] * s, v[1], -v[0] * s + v[2] * c]; };
const rz = (a, v) => { const c = Math.cos(a), s = Math.sin(a); return [v[0] * c - v[1] * s, v[0] * s + v[1] * c, v[2]]; };
// editor rotation [x, y, z] degrees: Z first, then X, then Y (three.js Euler 'YXZ'); springs ignore Y
export const rotate = (r, v, yaw = true) => { const w = rx(r[0] * R, rz(r[2] * R, v)); return yaw ? ry(r[1] * R, w) : w; };
const add = (a, b, k = 1) => [a[0] + b[0] * k, a[1] + b[1] * k, a[2] + b[2] * k];
const dist = (a, b) => Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);

// state = {rot: [x, y, z], S, H (ramp pitch), nct (NoControlTime)} -> {o, v, push, tf, d}
export function launch(kind, st, pos) {
  const S = Number.isFinite(st.S) ? st.S : 2, r = st.rot || [0, 0, 0], nct = Math.max(0, st.nct || 0);
  if (kind === 'spring' || kind === 'wide' || kind === 'digital') {
    const d = kind === 'digital' ? [0, 1, 0] : rotate(r, [0, 1, 0], false), v = 150 * (S + 1);
    let o = kind === 'digital' ? add(pos, [0, 18, 0]) : add(pos, d, 10);
    if (kind === 'wide') o = add(o, rotate(r, [0, 0, 1], false), 10);
    return {o, v: d.map(x => x * v), push: [0, 0, 0], tf: 0, d};
  }
  if (kind === 'ring') { const d = rotate(r, [0, 0, -1]); return {o: [...pos], v: d.map(x => x * 200 * (S + 2)), push: [0, 0, 0], tf: 0, d}; }
  if (kind === 'ramp') { const h = (st.H || 0) * R, d = rotate(r, [0, Math.sin(h), -Math.cos(h)]); return {o: add(pos, [0, 5, 0]), v: d.map(x => x * 300 * (S + 2)), push: d.map(x => x * PUSH), tf: nct, d}; }
  if (kind === 'panel') { const d = rotate(r, [0, 0, -1]); return {o: add(pos, [0, 5, 0]), v: d.map(x => x * 225 * (S + 1)), push: d.map(x => x * PUSH), tf: nct, d}; }
  return null;
}

// env (optional): hit(p0, p1) -> {point, normal} | null (segment against the stage's collision), floor(p) -> y | null
// -> {points, apex, end: {p, t, why: 'hit' | 'time' | 'stop', normal}, ground}
export function trace(kind, st, pos, env = null, T = ARC_SECONDS) {
  const L = launch(kind, st, pos); if (!L) return null;
  if (kind === 'panel' && env?.floor) { const y = env.floor(add(L.o, [0, 20, 0])); if (y != null && L.o[1] - y < 30) return groundRun(L, st, env); }
  return air(L.o, L.v, L.push, L.tf, env, T, 0, [L.o.slice()]);
}
function air(p, v, push, tf, env, T, t0, points) {
  p = p.slice(); v = v.slice(); let t = t0, last = p.slice(), apex = {p: p.slice(), t};
  for (let i = 0; t < T - 1e-9; i++) {
    const on = t < tf, a = [(on ? push[0] : 0) - KH * v[0], -G + (on ? push[1] : 0) - KY * v[1], (on ? push[2] : 0) - KH * v[2]];
    p = add(p, v, DT); v = add(v, a, DT); t += DT;
    if (p[1] > apex.p[1]) apex = {p: p.slice(), t};
    if (env?.hit && (i % 3 === 2) && t > .05) {   // segments of 3 frames; the first 3 frames never stop it (the launcher's own base)
      const h = env.hit(last, p); last = p.slice();
      if (h) { points.push(h.point); return {points, apex: apex.p[1] > h.point[1] ? apex : {p: h.point, t}, end: {p: h.point, t, why: 'hit', normal: h.normal}, ground: false}; }
    } else if (i % 3 === 2) last = p.slice();
    points.push(p.slice());
  }
  return {points, apex, end: {p: p.slice(), t, why: 'time'}, ground: false};
}
// Dash Panel on the floor (Original movement): 225 (S + 1) along its forward, pulled down to the run cap 750 within
// about 0.25 s, held until NoControlTime ends, then a stop in about 0.15 s (measured 400 + 15 S for 0.5 s). Off a
// ledge he falls with the air model at that speed (no push).
function groundRun(L, st, env) {
  const v0 = Math.hypot(...L.v), hv = Math.hypot(L.d[0], L.d[2]) || 1, h = [L.d[0] / hv, 0, L.d[2] / hv], nct = Math.max(0, st.nct || 0);
  let p = L.o.slice(), t = 0, s = v0, brakeFrom = null; const points = [p.slice()];
  while (t < nct + BRAKE + DT && t < 8) {
    if (t < nct) s = v0 > RUN_CAP ? RUN_CAP + (v0 - RUN_CAP) * Math.exp(-t / RUN_TAU) : v0;
    else { brakeFrom ??= s; s = Math.max(0, brakeFrom * (1 - (t - nct) / BRAKE)); }
    const n = add(p, h, s * DT), y = env.floor(add(n, [0, 25, 0])); t += DT;
    if (env.hit && env.hit(add(p, [0, 8, 0]), add(n, [0, 8, 0]))) return {points, apex: {p: p.slice(), t}, end: {p: p.slice(), t, why: 'hit'}, ground: true};
    if (y == null || n[1] - y > 40) return air(n, h.map(x => x * s), [0, 0, 0], 0, env, t + ARC_SECONDS, t, [...points, n.slice()]);
    p = [n[0], y + 5, n[2]]; points.push(p.slice());
    if (s <= 0) break;
  }
  return {points, apex: {p: points[0], t: 0}, end: {p: p.slice(), t, why: 'stop'}, ground: true};
}

// ---- aim: heading (degrees, the horizontal direction sin/cos), elevation e (degrees), Strength ------------------------
// spring / wide: e = tilt from straight up toward the heading (rotation X / Z; Y kept); digital: straight up only;
// ring: e = pitch (rotation X) and the heading is rotation Y; ramp: e = Height and the heading is rotation Y;
// panel: the heading is rotation Y (on the floor the reach comes from NoControlTime).
export function aimOf(kind, st) {
  const L = launch(kind, st, [0, 0, 0]), d = L.d, heading = Math.atan2(d[0], d[2]) / R;
  const e = kind === 'spring' || kind === 'wide' ? Math.acos(Math.max(-1, Math.min(1, d[1]))) / R : kind === 'ring' ? st.rot[0] : kind === 'ramp' ? st.H || 0 : 0;
  return {heading, e, S: st.S};
}
export function withAim(kind, st, aim) {
  const n = {...st, rot: [...(st.rot || [0, 0, 0])], S: aim.S ?? st.S}, hd = aim.heading * R;
  if (kind === 'spring' || kind === 'wide') {
    const t = aim.e * R, d = [Math.sin(t) * Math.sin(hd), Math.cos(t), Math.sin(t) * Math.cos(hd)];
    n.rot[0] = Math.atan2(d[2], d[1]) / R; n.rot[2] = -Math.asin(Math.max(-1, Math.min(1, d[0]))) / R;
  } else if (kind === 'ring' || kind === 'ramp' || kind === 'panel') {
    if (kind === 'ring') n.rot[0] = aim.e; if (kind === 'ramp') n.H = aim.e;
    n.rot[1] = Math.atan2(-Math.sin(hd), -Math.cos(hd)) / R;
    for (let k = 0; k < 3; k++) { const got = aimOf(kind, n).heading, err = ((aim.heading - got + 540) % 360) - 180; if (Math.abs(err) < .05) break; n.rot[1] += err; }   // X / Z turned too: correct the yaw
  }
  return n;
}
const E_RANGE = {spring: [0, 75], wide: [0, 75], ring: [-80, 85], ramp: [0, 85]};
function nearest(points, q) { let b = Infinity; for (let i = 1; i < points.length; i++) { const a = points[i - 1], c = points[i], ab = [c[0] - a[0], c[1] - a[1], c[2] - a[2]], l = ab[0] ** 2 + ab[1] ** 2 + ab[2] ** 2 || 1, k = Math.max(0, Math.min(1, ((q[0] - a[0]) * ab[0] + (q[1] - a[1]) * ab[1] + (q[2] - a[2]) * ab[2]) / l)); b = Math.min(b, dist(q, add(a, ab, k))); } return b; }
const headingTo = (pos, q) => Math.atan2(q[0] - pos[0], q[2] - pos[2]) / R;
// Drag the landing: the arc passes through q. Keeps Strength when a tilt / pitch reaches q, else changes Strength
// the least. -> {state, err} (err = how far the best arc passes from q; > ~25 = out of reach)
export function solveLanding(kind, st, pos, q, env = null) {
  const cur = aimOf(kind, st), heading = Math.hypot(q[0] - pos[0], q[2] - pos[2]) > 1 ? headingTo(pos, q) : cur.heading;
  const score = s => nearest(trace(kind, s, pos, null).points, q);
  if (kind === 'panel' && env?.floor && trace(kind, st, pos, env).ground) {   // on the floor: NoControlTime sets the reach
    let best = null; for (let n = 0; n <= 5.0001; n += .02) { const s = {...withAim(kind, st, {heading, e: 0}), nct: Math.round(n * 100) / 100}, r = trace(kind, s, pos, env), err = dist(r.end.p, q); if (!best || err < best.err - .01) best = {state: s, err}; }
    return best;
  }
  if (kind === 'digital') return {state: st, err: score(st)};
  if (kind === 'panel') return search1(S => withAim(kind, st, {heading, e: 0, S}), score, 0, S_MAX, st.S);
  const [lo, hi] = E_RANGE[kind], row = [];
  for (let e = lo; e <= hi + 1e-9; e += 1) { const s = withAim(kind, st, {heading, e, S: st.S}); row.push({state: s, err: score(s), e}); }
  const ok = row.filter((c, i) => c.err < 20 && !(row[i - 1]?.err < c.err) && !(row[i + 1]?.err < c.err));   // each tilt that reaches it (a low and a high arc), at its best degree
  if (ok.length) return refine(kind, ok.reduce((a, b) => Math.abs(b.e - cur.e) < Math.abs(a.e - cur.e) ? b : a), score, false);
  let grid = null;   // no tilt reaches it: Strength too, the smallest change that gets there
  for (let S = 0; S <= S_MAX + 1e-9; S += .5) for (let e = lo; e <= hi + 1e-9; e += 2.5) { const s = withAim(kind, st, {heading, e, S}), err = score(s), k = err + (err < 20 ? 0 : 1000) + Math.abs(S - st.S) * 2; if (!grid || k < grid.k) grid = {state: s, err, e, k}; }
  return refine(kind, grid, score);
}
// Drag the apex: tilt / pitch and Strength so the top of the arc is at q (digital: Strength for the height)
export function solveApex(kind, st, pos, q) {
  const heading = Math.hypot(q[0] - pos[0], q[2] - pos[2]) > 1 ? headingTo(pos, q) : aimOf(kind, st).heading, score = s => dist(trace(kind, s, pos, null).apex.p, q);
  if (kind === 'digital' || kind === 'panel') return search1(S => withAim(kind, st, {heading, e: aimOf(kind, st).e, S}), score, 0, S_MAX, st.S);
  const [lo, hi] = E_RANGE[kind]; let best = null;
  for (let S = 0; S <= S_MAX + 1e-9; S += .5) for (let e = lo; e <= hi + 1e-9; e += 2.5) { const s = withAim(kind, st, {heading, e, S}), err = score(s); if (!best || err < best.err) best = {state: s, err, e}; }
  return refine(kind, best, score);
}
function search1(make, score, lo, hi, cur) {
  let best = null; for (let x = lo; x <= hi + 1e-9; x += .1) { const s = make(Math.round(x * 100) / 100), err = score(s); if (!best || err < best.err - .01 || (Math.abs(err - best.err) <= .01 && Math.abs(x - cur) < Math.abs(best.x - cur))) best = {state: s, err, x}; }
  for (let step = .05; step >= .01; step /= 2) for (const d of [-step, step]) { const x = Math.max(lo, Math.min(hi, best.x + d)), s = make(Math.round(x * 100) / 100), err = score(s); if (err < best.err) best = {state: s, err, x}; }
  return {state: best.state, err: best.err};
}
function refine(kind, c, score, strength = true) {   // a few local steps on (e, S); strength false: e only
  if (!c) return null; let {state, err} = c, a = aimOf(kind, state);
  for (const step of [1, .5, .25, .1]) for (let k = 0; k < 6; k++) { let moved = false;
    for (const [de, dS] of strength ? [[step, 0], [-step, 0], [0, step / 5], [0, -step / 5]] : [[step, 0], [-step, 0]]) {
      const n = {...a, e: a.e + de, S: Math.max(0, Math.min(S_MAX, a.S + dS))}, [lo, hi] = E_RANGE[kind] || [0, 0]; if (n.e < lo || n.e > hi) continue;
      const s = withAim(kind, state, n), e2 = score(s); if (e2 < err - .01) { state = s; err = e2; a = n; moved = true; } }
    if (!moved) break; }
  return {state, err};
}
