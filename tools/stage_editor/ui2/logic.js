// Stage Workshop UI 2 - Logic workspace (roadmap 5a): a node canvas over tools/stage_editor/nodes.json (ComfyUI look,
// Blueprint semantics: white exec pins give the order, typed data pins carry values). The graph of an owner lives in
// the project as plain JSON (owner.logic, project.logic for the stage; shape documented at the top of nodes.json) and
// every edit compiles it back into the owner's existing v3 fields (trigger.event / once / cooldown / graph / actions,
// object.events, pad.target / cooldown), so the current build (events.py) keeps working unchanged.
export function createLogic(app) {
  const $ = id => document.getElementById(id), esc = app.esc;
  // node definitions: nodes.json, plus the project's functions ('fn.<id>': subgraphs collapsed from a selection,
  // project.logicFunctions[id] = {name, nodes, links, groups, ports [{dir in|out, name, type, node, pin}]}; a call is
  // copied in (inlined) where it is used when the graph is built, so functions build with today's v3)
  let BASE = new Map();
  const fnOf = t => P()?.logicFunctions?.[String(t).slice(3)];
  function fnDef(t) {
    const f = fnOf(t); if (!f) return null;
    const ex = dir => f.ports.filter(q => q.dir === dir && q.type === 'exec').map(q => q.name), da = dir => f.ports.filter(q => q.dir === dir && q.type !== 'exec').map(q => ({name: q.name, type: q.type || 'any'}));
    return {id: t, category: 'flow', label: 'ƒ ' + f.name, docs: `Function "${f.name}" (${f.nodes.length} nodes). Edit it from the owner list (Function: ${f.name}); it is copied in where it is used when the graph is built.`, owners: ['any'], exec: {in: ex('in'), out: ex('out')}, inputs: da('in'), outputs: da('out'), widgets: [], v3: {kind: 'call'}, fn: true};
  }
  const DEFS = {get: t => String(t || '').startsWith('fn.') ? fnDef(t) : BASE.get(t)};
  let TABLE = null, ownerRef = null, sel = new Set(), selLink = null, selGroup = null, drag = null, issues = new Map(), status = '';
  const views = new Map();   // owner -> {x, y, z}
  fetch('../nodes.json').then(r => r.json()).then(t => { TABLE = t; BASE = new Map(t.nodes.map(d => [d.id, d])); if (active()) render(); }).catch(e => app.toast('nodes.json: ' + e.message, true));

  // ---- owners ------------------------------------------------------------------------------------------------------
  const P = () => app.project();
  const kindOf = o => o === 'stage' ? 'stage' : typeof o === 'string' && o.startsWith('fn:') ? 'function' : o?.kind === 'trigger' ? 'volume' : o?.kind === 'teleport' ? 'pad' : o?.kind === 'native' ? 'native' : null;
  const NATIVE_PIN = {'action.spawn': 'template', 'action.destroy': 'target', 'action.move': 'target'};
  const lightingTriggers = () => new Set([...(P().lighting?.regions || []), ...(P().lighting?.profiles || [])].map(r => r.trigger).filter(Boolean));
  const allowed = (d, k) => k === 'function' ? d.category !== 'event' : d.owners.includes('any') || d.owners.includes(k);
  function owner() {
    if (ownerRef === 'stage') return 'stage'; if (!ownerRef || !P()) return null;
    if (ownerRef.startsWith('fn:')) return P().logicFunctions?.[ownerRef.slice(3)] ? ownerRef : null;
    return [...P().objects, ...(P().triggers || [])].find(o => o.id === ownerRef) || null;
  }
  const ownerName = o => o === 'stage' ? 'Stage' : typeof o === 'string' && o.startsWith('fn:') ? 'Function ' + (P().logicFunctions?.[o.slice(3)]?.name || '?') : o?.name || app.classOf(o);
  function graphOf(o, create = true) {
    if (o === 'stage') { if (!P().logic && create) P().logic = blank(); return P().logic; }
    if (typeof o === 'string' && o.startsWith('fn:')) { const f = P().logicFunctions?.[o.slice(3)]; if (f) { f.groups ||= []; f.links ||= []; } return f; }
    if (!o.logic && create) o.logic = importClassic(o); return o.logic;
  }
  // how many event graphs a placement has (inspector "Graphs: N"), from the new graph or the classic fields
  function count(o) {
    const k = kindOf(o); if (!k) return 0;
    if (o.logic) return o.logic.nodes.filter(n => DEFS.get(n.type)?.category === 'event').length;
    if (k === 'volume') return (o.actions?.length || o.graph?.nodes?.length > 1) ? 1 : 0;
    if (k === 'pad') return o.target ? 1 : 0; return (o.events || []).length;
  }

  // ---- graph building helpers --------------------------------------------------------------------------------------
  const blank = () => ({version: 1, nodes: [], links: [], groups: []});
  const freeId = (list, p) => { let i = list.length + 1; while (list.some(x => x.id === p + i)) i++; return p + i; };
  function addNode(g, type, x, y, values = {}) {
    const d = DEFS.get(type); const v = {}; for (const w of d?.widgets || []) v[w.name] = structuredClone(w.default); Object.assign(v, values);
    const n = {id: freeId(g.nodes, 'n'), type, x: Math.round(x), y: Math.round(y), values: v, collapsed: false, title: null}; g.nodes.push(n); return n;
  }
  const addLink = (g, a, out, b, inp) => g.links.push({id: freeId(g.links, 'l'), from: a.id, out, to: b.id, in: inp});
  function actionNode(g, a, x, y) {
    const m = {teleport: ['action.teleport', {destination: a.target || ''}], set_speed: ['action.set_speed', {velocity: a.vector || [0, 0, -(a.value ?? 150)]}],
      rings: ['action.rings', {amount: a.value ?? 0}], score: ['action.score', {points: a.value ?? 0}], set_variable: ['action.set_variable', {name: a.name || '', value: a.value ?? 0}],
      enable: ['action.enable', {volume: a.target || ''}], disable: ['action.disable', {volume: a.target || ''}], lighting: ['action.lighting', {name: a.name || '', on: !!a.value}]}[a.type];
    return m ? addNode(g, m[0], x, y, m[1]) : null;
  }
  // a classic action with its own delay / conditions: Delay -> Branch(es) -> action, returns [first, last]
  function actionChain(g, a, x, y) {
    let first = null, prev = null, port = 'then', col = x; const step = n => { if (prev) addLink(g, prev, port, n, 'exec'); else first = n; prev = n; col += 240; };
    if (+a.delay) { step(addNode(g, 'flow.delay', col, y, {seconds: +a.delay})); }
    for (const c of a.conditions || (a.condition ? [a.condition] : [])) { step(addNode(g, 'flow.branch', col, y, {variable: c.name || '', compare: c.compare || '==', value: +c.value || 0})); port = 'true'; }
    const n = actionNode(g, a, col, y); if (n) { if (prev) addLink(g, prev, port, n, 'exec'); else first = n; prev = n; }
    return [first, prev];
  }
  function importChain(g, ev, actions, x, y) {   // flat classic list: every action runs from the event with its own delay
    actions.forEach((a, i) => { const [f] = actionChain(g, a, x, y + i * 120); if (f) addLink(g, ev, 'then', f, 'exec'); });
  }
  function importV3(g, ev, graph, dx, dy) {   // classic node graph (event / condition / delay / action + edges)
    const map = new Map(), first = new Map();
    for (const n of graph.nodes) {
      const x = (n.x ?? 0) * 1.25 + dx, y = (n.y ?? 0) * 1.1 + dy;
      if (n.type === 'event') { map.set(n.id, ev); ev.x = Math.round(x); ev.y = Math.round(y); continue; }
      if (n.type === 'condition') { const b = addNode(g, 'flow.branch', x, y, {variable: n.variable || '', compare: n.compare || '==', value: +n.value || 0}); map.set(n.id, b); continue; }
      if (n.type === 'delay') { const b = addNode(g, 'flow.delay', x, y, {seconds: +n.seconds || 0}); map.set(n.id, b); continue; }
      if (n.type === 'action') { const [f, l] = actionChain(g, n.action || {}, x, y); if (l) { map.set(n.id, l); first.set(n.id, f); } }
    }
    for (const e of graph.edges || []) {
      const a = map.get(e.from), b = first.get(e.to) || map.get(e.to); if (!a || !b) continue;
      addLink(g, a, e.port === 'true' || e.port === 'false' ? e.port : 'then', b, 'exec');
    }
  }
  function importClassic(o) {
    const g = blank(), k = kindOf(o);
    if (k === 'volume') {
      const ev = addNode(g, 'event.' + (['enter', 'exit', 'stay', 'interact'].includes(o.event) ? o.event : 'enter'), 40, 60, {once: o.once ?? true, cooldown: o.cooldown ?? .1});
      if (o.graph?.nodes?.length) importV3(g, ev, o.graph, 40, 60); else importChain(g, ev, o.actions || [], 320, 60);
    } else if (k === 'pad') {
      const ev = addNode(g, 'event.touch', 40, 60, {cooldown: o.cooldown ?? 1});
      if (o.target) addLink(g, ev, 'then', addNode(g, 'action.teleport', 320, 60, {destination: o.target}), 'exec');
    } else if (k === 'native') {
      let y = 60;
      for (const e of o.events || []) {
        const ev = addNode(g, 'event.' + e.event, 40, y, {once: !!e.once});
        if (e.graph?.nodes?.length) importV3(g, ev, e.graph, 40, y); else importChain(g, ev, e.actions || [], 320, y);
        y = Math.max(y + 160, ...g.nodes.map(n => n.y + 140));
      }
    }
    return g;
  }

  // ---- types and links ---------------------------------------------------------------------------------------------
  const nodeById = (g, id) => g.nodes.find(n => n.id === id);
  function pinType(g, n, dir, pin, seen = new Set()) {
    const d = DEFS.get(n.type); if (!d) return null;
    if ((dir === 'in' ? d.exec.in : d.exec.out).includes(pin)) return 'exec';
    const p = (dir === 'in' ? d.inputs : d.outputs).find(x => x.name === pin); if (!p) return null;
    if (p.type !== 'any' || seen.has(n.id)) return p.type; seen.add(n.id);
    for (const l of g.links) {   // a reroute takes the type of what it is wired to
      if (l.to === n.id) { const t = pinType(g, nodeById(g, l.from), 'out', l.out, seen); if (t && t !== 'any') return t; }
      if (l.from === n.id) { const t = pinType(g, nodeById(g, l.to), 'in', l.in, seen); if (t && t !== 'any') return t; }
    }
    return 'any';
  }
  function canLink(g, a, out, b, inp) {
    if (a.id === b.id) return 'a node cannot wire to itself';
    const ta = pinType(g, a, 'out', out), tb = pinType(g, b, 'in', inp); if (!ta || !tb) return 'unknown pin';
    if (ta === 'any' || tb === 'any' || ta === tb) return null;
    return `${TABLE.pinTypes[ta]?.label || ta} cannot go into ${TABLE.pinTypes[tb]?.label || tb}`;
  }

  // ---- compile to the v3 fields ------------------------------------------------------------------------------------
  // exec successors of a pin, through reroutes and sequences, in link order
  function nextOf(g, n, pin, guard = 0) {
    const out = []; if (guard > 64) return out;
    for (const l of g.links.filter(l => l.from === n.id && l.out === pin)) {
      const t = nodeById(g, l.to); if (!t) continue; const d = DEFS.get(t.type);
      if (t.type === 'flow.reroute') out.push(...nextOf(g, t, 'out', guard + 1));
      else if (t.type === 'flow.sequence') for (const p of d.exec.out) out.push(...nextOf(g, t, p, guard + 1));
      else out.push(t);
    }
    return out;
  }
  // a data input: the linked constant (Self, Placement, Number, Vector, Text, through reroutes) or the widget value
  function input(g, n, name, o, bad) {
    const l = g.links.find(l => l.to === n.id && l.in === name);
    if (!l) return n.values?.[name];
    let s = nodeById(g, l.from), out = l.out; for (let i = 0; s?.type === 'flow.reroute' && i < 32; i++) { const r = g.links.find(x => x.to === s.id && x.in === 'in'); if (!r) break; out = r.out; s = nodeById(g, r.from); }
    if (!s) return n.values?.[name];
    if (s.type === 'data.self') return o === 'stage' ? '' : o.id;
    const d = DEFS.get(s.type); if (d?.v3?.kind === 'const') return s.values?.[d.v3.widget];
    bad(n, `${DEFS.get(s.type)?.label || s.type} feeding "${name}" is computed in the v4 VM; the v3 build uses the widget value`); return n.values?.[name];
  }
  function expand(g, depth = 0) {   // calls of functions -> their nodes (ids '<call>:<inner>'), wires through the ports
    if (depth > 8 || !g.nodes.some(n => n.type.startsWith('fn.') && fnOf(n.type))) return g;
    const nodes = [], links = [], ports = new Map();
    for (const n of g.nodes) {
      const f = n.type.startsWith('fn.') && fnOf(n.type); if (!f) { nodes.push(n); continue; }
      const pre = n.id + ':';
      for (const m of f.nodes) nodes.push({...m, id: pre + m.id, x: n.x + m.x, y: n.y + m.y});
      for (const l of f.links) links.push({...l, id: pre + l.id, from: pre + l.from, to: pre + l.to});
      ports.set(n.id, f.ports.map(q => ({...q, node: pre + q.node})));
    }
    for (const l of g.links) {
      let {from, out, to, in: inp} = l; const pf = ports.get(from), pt = ports.get(to);
      if (pf) { const q = pf.find(q => q.dir === 'out' && q.name === out); if (!q) continue; from = q.node; out = q.pin; }
      if (pt) { const q = pt.find(q => q.dir === 'in' && q.name === inp); if (!q) continue; to = q.node; inp = q.pin; }
      links.push({...l, from, out, to, in: inp});
    }
    return expand({...g, nodes, links}, depth + 1);
  }
  function compile(o) {
    const k = kindOf(o); issues = new Map(); const bad = (n, t) => { const key = String(n.id).split(':')[0]; if (!issues.has(key)) issues.set(key, t); };
    if (k === 'function') {
      const f = graphOf(o); for (const q of f.ports) if (!f.nodes.some(n => n.id === q.node)) issues.set(f.nodes[0]?.id || '_', `port "${q.name}" lost its inner node`);
      for (const n of f.nodes) { const d = DEFS.get(n.type); if (!d) bad(n, 'unknown node type ' + n.type); else if (d.category === 'event') bad(n, 'events stay outside a function'); else if (d.vm) bad(n, `${d.label} inside a function: the v4 compiler does not inline functions yet (track B)`); }
      status = `function · ${f.ports.filter(q => q.dir === 'in').length} in / ${f.ports.filter(q => q.dir === 'out').length} out · inlined where used`; return;
    }
    const g = expand(graphOf(o));
    const events = g.nodes.filter(n => DEFS.get(n.type)?.category === 'event').sort((a, b) => a.y - b.y);
    for (const n of g.nodes) { const d = DEFS.get(n.type); if (d?.needs === 'runtime') bad(n, `${d.label} needs a newer game build: the build leaves it out until then (the chain goes on)`); }
    for (const n of g.nodes) { const d = DEFS.get(n.type); if (!d) bad(n, 'unknown node type ' + n.type); else if (!allowed(d, k)) bad(n, `${d.label} is not available on ${k === 'stage' ? 'the stage graph' : 'this kind of placement'}`); }
    for (const n of g.nodes) {   // Spawn template / Destroy / Move target must be a native object (SET record) placed in the project
      const pin = NATIVE_PIN[n.type]; if (!pin || g.links.some(l => l.to === n.id && l.in === pin)) continue;
      const id = n.values?.[pin], t = id && app.byId(id), what = n.type === 'action.spawn' ? 'Spawn copies a native record placed in the project (a hidden pool placement, e.g. activation distance 0)' : 'only native objects can be destroyed or moved';
      if (!id) bad(n, `choose the ${pin === 'template' ? 'template' : 'object'}: a native object placed in this project`);
      else if (!t) bad(n, 'the object it points at is not in this project (pasted from elsewhere?): choose one');
      else if (t.o.kind !== 'native') bad(n, `${t.name} is a ${app.classOf(t.o)}, not a native object (SET record): ${what}`);
    }
    const v3chain = ev => {   // classic graph {nodes, edges} + the flattened action list (events.py graph_actions)
      const nodes = [{id: 'event', type: 'event', x: Math.round(ev.x / 1.25), y: Math.round(ev.y / 1.1)}], edges = [], done = new Map([[ev.id, 'event']]), path = new Set();
      const visit = n => {
        const d = DEFS.get(n.type); if (d.vm) { bad(n, `${d.label} runs in the v4 VM: the server builds it into sidecar v4 (not v3)`); return null; }
        if (done.has(n.id)) return done.get(n.id); if (path.has(n.id)) { bad(n, 'a loop of exec wires: v3 cannot repeat (use a cooldown)'); return null; }
        const x = Math.round(n.x / 1.25), y = Math.round(n.y / 1.1), v = d.v3; let node;
        if (v?.kind === 'action') {
          const a = {type: v.type};
          for (const [field, w] of Object.entries(v.fields)) a[field] = input(g, n, w, o, bad);
          if (v.type === 'lighting') a.value = a.value ? 1 : 0;
          if (n.type === 'action.fade_sun') {   // a generated rule (builds it from a.profile / a.transition) that this action 6 turns on
            const tag = x => String(x).replace(/[^A-Za-z0-9]/g, '').slice(0, 10);
            a.name = `ws_fade_sun_${o === 'stage' ? 'stage' : tag(o.id)}_${tag(n.id)}`; a.value = 1;
            a.profile = {sun_strength: Math.min(3, Math.max(0, Number(input(g, n, 'strength', o, bad)) || 0))}; a.transition = Math.min(30, Math.max(0, Number(input(g, n, 'seconds', o, bad)) || 0));
          }
          if (n.type === 'action.set_lighting_profile' && !a.name) bad(n, 'choose a lighting profile (its trigger name)');
          else if (n.type === 'action.set_lighting_profile' && !lightingTriggers().has(a.name)) bad(n, `no lighting region or profile has the trigger "${a.name}" (Stage settings → Lighting profiles, or a region's Trigger)`);
          if (['teleport', 'enable', 'disable'].includes(v.type) && !a.target) bad(n, 'choose a target');
          else if (a.target && ['teleport', 'enable', 'disable'].includes(v.type) && !app.byId(a.target)) bad(n, 'the placement it points at is not in this project (pasted from elsewhere?): choose one');
          if (v.type === 'set_variable' && (!a.name || String(a.name).startsWith('@'))) bad(n, 'a variable name (not starting with @)');
          if (v.type === 'set_speed') a.vector = (a.vector || [0, 0, 0]).map(Number);
          for (const f of ['value']) if (f in a && typeof a[f] !== 'number') a[f] = Number(a[f]) || 0;
          node = {id: n.id, type: 'action', action: a, x, y};
        } else if (v?.kind === 'condition') node = {id: n.id, type: 'condition', variable: String(input(g, n, 'variable', o, bad) || ''), compare: n.values.compare || '==', value: Number(input(g, n, 'value', o, bad)) || 0, x, y};
        else if (v?.kind === 'delay') node = {id: n.id, type: 'delay', seconds: Math.max(0, Number(input(g, n, 'seconds', o, bad)) || 0), x, y};
        else { bad(n, `${d.label} cannot run here`); return null; }
        if (node.type === 'condition' && !node.variable) bad(n, 'a variable or live value to compare');
        nodes.push(node); done.set(n.id, n.id); path.add(n.id);
        for (const [pin, port] of node.type === 'condition' ? [['true', 'true'], ['false', 'false']] : [['then', 'next']])
          for (const t of nextOf(g, n, pin)) { const id = visit(t); if (id) edges.push({from: n.id, to: id, port}); }
        path.delete(n.id); return n.id;
      };
      for (const t of nextOf(g, ev, 'then')) { const id = visit(t); if (id) edges.push({from: 'event', to: id, port: 'next'}); }
      return {graph: {nodes, edges}, actions: flatten({nodes, edges})};
    };
    const vol = events.filter(n => ['event.enter', 'event.exit', 'event.stay', 'event.interact'].includes(n.type));
    if (k === 'volume') {
      if (vol.length > 1) for (const n of vol.slice(1)) bad(n, 'a volume runs one event in v3: only the top event is built (use another volume)');
      const ev = vol[0];
      if (ev) { const c = v3chain(ev); o.event = ev.type.slice(6); o.once = !!ev.values.once; o.cooldown = Math.max(.05, +ev.values.cooldown || .1); o.graph = c.graph; o.actions = c.actions; }
      else { o.graph = {nodes: [{id: 'event', type: 'event', x: 20, y: 30}], edges: []}; o.actions = []; }
    } else if (k === 'native') {
      const evs = events.filter(n => ['event.spawn', 'event.killed', 'event.removed', 'event.despawn'].includes(n.type));
      o.events = evs.map(ev => { const c = v3chain(ev); return {event: ev.type.slice(6), once: !!ev.values.once, actions: c.actions, graph: c.graph}; });
      if (!o.events.length) delete o.events;
    } else if (k === 'pad') {
      const ev = events.find(n => n.type === 'event.touch'), next = ev ? nextOf(g, ev, 'then') : [];
      if (ev) o.cooldown = Math.max(.05, +ev.values.cooldown || 1);
      const tp = next.find(n => n.type === 'action.teleport'); if (tp) o.target = input(g, tp, 'destination', o, bad) || '';
      for (const n of next) if (n !== tp) bad(n, 'a teleport pad can only teleport in v3: use a trigger volume for more');
      if (tp && nextOf(g, tp, 'then').length) for (const n of nextOf(g, tp, 'then')) bad(n, 'a teleport pad can only teleport in v3');
      if (!tp && ev) bad(ev, 'connect Teleport player to');
    }   // a stage graph: no per-node note (it was on every node); the status line says how it builds
    const built = k === 'volume' ? (o.actions?.length || 0) : k === 'native' ? (o.events || []).reduce((a, e) => a + e.actions.length, 0) : k === 'pad' ? (o.target ? 1 : 0) : 0;
    status = k === 'stage' ? `stage graph · ${g.nodes.length} node${g.nodes.length === 1 ? '' : 's'} · the server builds it into sidecar v4 (the game's VM)${issues.size ? ` · ${issues.size} note${issues.size > 1 ? 's' : ''}` : ''}` : `${built} action${built === 1 ? '' : 's'} built${issues.size ? ` · ${issues.size} note${issues.size > 1 ? 's' : ''}` : ''}`;
  }
  // the same expansion as events.py graph_actions: delays add up along the chain, branch conditions stack
  function flatten(graph) {
    const nodes = new Map(graph.nodes.map(n => [n.id, n])), out = [];
    const walk = (id, delay, conds, depth) => {
      const n = nodes.get(id); if (!n || depth > 64 || out.length > 4096) return;
      if (n.type === 'action') out.push({...n.action, delay, conditions: conds});
      if (n.type === 'delay') delay += n.seconds || 0;
      for (const e of graph.edges.filter(e => e.from === id)) {
        let c = conds;
        if (n.type === 'condition') { let cmp = n.compare || '=='; if (e.port === 'false') cmp = {'==': '!=', '!=': '==', '<': '>=', '<=': '>', '>': '<=', '>=': '<'}[cmp]; c = [...conds, {name: n.variable, value: n.value, compare: cmp}]; }
        walk(e.to, delay, c, depth + 1);
      }
    };
    walk('event', 0, [], 0); return out;
  }

  // ---- editing (every change: snapshot first, then compile + changed) -----------------------------------------------
  function edit(fn) { const o = owner(); if (!o) return; app.snapshot(); fn(graphOf(o)); compile(o); app.changed(); render(); }
  function deleteSelection() {
    if (!sel.size && !selLink && !selGroup) return false;
    edit(g => {
      g.nodes = g.nodes.filter(n => !sel.has(n.id)); g.links = g.links.filter(l => l.id !== selLink && !sel.has(l.from) && !sel.has(l.to));
      if (selGroup) g.groups = g.groups.filter(x => x.id !== selGroup);
    });
    sel.clear(); selLink = selGroup = null; render(); return true;
  }
  function duplicateSelection() {
    if (!sel.size) return false; const map = new Map();
    edit(g => {
      for (const n of g.nodes.filter(n => sel.has(n.id))) { const c = {...structuredClone(n), id: freeId(g.nodes, 'n'), x: n.x + 40, y: n.y + 40}; g.nodes.push(c); map.set(n.id, c.id); }
      for (const l of g.links.filter(l => map.has(l.from) && map.has(l.to))) g.links.push({...l, id: freeId(g.links, 'l'), from: map.get(l.from), to: map.get(l.to)});
    });
    sel = new Set(map.values()); render(); return true;
  }
  function groupSelection() {
    const g = graphOf(owner()); const ns = g.nodes.filter(n => sel.has(n.id)); if (!ns.length) return app.toast('Select nodes first (Shift-click or Shift-drag)');
    const el = id => board().querySelector(`.lgNode[data-id="${id}"]`), r = ns.map(n => [n.x, n.y, n.x + (el(n.id)?.offsetWidth || 220), n.y + (el(n.id)?.offsetHeight || 100)]);
    edit(g => g.groups.push({id: freeId(g.groups, 'g'), title: 'Group', x: Math.min(...r.map(a => a[0])) - 24, y: Math.min(...r.map(a => a[1])) - 44, w: Math.max(...r.map(a => a[2])) - Math.min(...r.map(a => a[0])) + 48, h: Math.max(...r.map(a => a[3])) - Math.min(...r.map(a => a[1])) + 68, colour: '#7c8aa5'}));
  }

  // ---- DOM ---------------------------------------------------------------------------------------------------------
  const root = document.createElement('div'); root.className = 'lg'; root.hidden = true;
  root.innerHTML = `<div class="lgBar"><span class="lgOwner" id="lgOwner"></span><select class="num" id="lgOwnerSel" title="Whose graph"></select>
      <button class="ghost" id="lgAdd" title="Add a node (or double-click the canvas)">+ Node</button><button class="ghost" id="lgGroup" title="Group the selected nodes (Ctrl G)">Group</button><button class="ghost" id="lgFit" title="Fit the graph (F)">Fit</button><span class="lgFind"><input id="lgFindQ" placeholder="Find in graph… (Ctrl F)" autocomplete="off" title="Nodes of this graph by name, type or a value; Enter = next match"><small id="lgFindN"></small></span><select class="num" id="lgAlign" title="Line up the selected nodes"><option value="">Align…</option><option value="left">Align left edges</option><option value="top">Align top edges</option><option value="h">Space evenly across</option><option value="v">Space evenly down</option></select><button class="ghost" id="lgFn" title="Turn the selected nodes into a reusable function node (its pins = the wires that cross the selection)">ƒ Function</button><button class="ghost" id="lgTpl" title="Insert a ready graph (combo, countdown, waves, random reward, repeat, kill counter)">Templates</button><button class="ghost" id="lgExport" title="This owner's graph as JSON (the shape documented in nodes.json)">Export JSON</button><button class="ghost" id="lgImport" title="Paste a graph JSON (for example one an AI agent wrote) for this owner">Import JSON</button>
      <span class="lgStatus" id="lgStatus"></span><button class="ghost" id="lgLive" title="Live highlight (roadmap 5d): while a playtest runs, nodes light up as the game fires them">Live: off</button><span class="lgVars" id="lgVars"></span><span class="grow"></span><span class="lgHelp">drag empty space: pan · wheel: zoom · Shift-drag: select · double-click: add · drag pin to pin: wire · Del · Ctrl D · Ctrl G</span></div>
    <div class="lgView" id="lgView"><div class="lgBoard" id="lgBoard"><svg class="lgWires" id="lgWires"></svg></div><div class="lgBox" id="lgBox" hidden></div></div>
    <canvas class="lgMini" id="lgMini" width="200" height="130" title="Click to move the view"></canvas>
    <div class="lgTplBox" id="lgTplBox" hidden><h4>Templates <small>insert a ready graph at the centre of the view</small></h4><div id="lgTplList"></div><h4 class="lgMyHead">My templates <small>your own, kept in this browser for every project</small></h4><div id="lgMyList"></div><div class="lgMySave"><input id="lgMyName" placeholder="Name for the selected nodes" autocomplete="off"><button class="ghost" id="lgMySaveB">Save selection as template</button></div></div>
    <div class="lgSearch" id="lgSearch" hidden><input id="lgQ" placeholder="Search nodes…" autocomplete="off"><div class="lgList" id="lgList"></div></div>
    <div class="lgJson" id="lgJson" hidden><div class="lgJsonBox"><h3 id="lgJsonTitle"></h3><textarea id="lgJsonText" spellcheck="false"></textarea><div class="lgJsonReport" id="lgJsonReport"></div><div class="modalBtns"><button class="ghost" id="lgJsonCheck">Check</button><button class="ghost" id="lgJsonApply">Replace this graph</button><button class="ghost" id="lgJsonCopy">Copy</button><button class="ghost" id="lgJsonClose">Close</button></div></div></div>
    <datalist id="lgLive"></datalist><div class="lgMsg" id="lgMsg" hidden></div>`;
  app.host.append(root);
  const board = () => $('lgBoard'), view = () => $('lgView');
  const V = () => { const k = ownerRef || 'none'; if (!views.has(k)) views.set(k, {x: 40, y: 40, z: 1}); return views.get(k); };
  const applyView = () => { const v = V(); board().style.transform = `translate(${v.x}px, ${v.y}px) scale(${v.z})`; drawMini(); };
  // ---- templates (nodes.json 'templates'): copied into this owner's graph with fresh ids, at the centre of the view
  function showTemplates() {
    const box = $('lgTplBox'); if (!box.hidden) { box.hidden = true; return; } const k = kindOf(owner());
    const list = (TABLE?.templates || []).map(t => ({t, off: t.owners.includes('any') || t.owners.includes(k) ? '' : `for ${t.owners.map(o => o === 'stage' ? 'the stage graph' : o === 'volume' ? 'a trigger volume' : o === 'native' ? 'a native object (enemy ...)' : o).join(' / ')}`}));
    $('lgTplList').innerHTML = list.map(({t, off}, i) => `<button class="lgTpl ${off ? 'off' : ''}" data-ti="${i}" title="${esc(t.docs)}"><b>${esc(t.label)}</b><small>${esc(off || t.docs)}</small></button>`).join('') || '<p class="muted">No templates in nodes.json</p>';
    $('lgTplList').querySelectorAll('[data-ti]').forEach(b => b.onclick = () => { const c = list[+b.dataset.ti]; if (c.off) return app.toast(`${c.t.label}: ${c.off}`, true); box.hidden = true; insertTemplate(c.t); });
    renderMy(); box.hidden = false;
  }
  // ---- My templates: a selection saved under a name (localStorage 'ui2.myTemplates', the copy / paste payload shape:
  // nodes, links, the functions they call), inserted at the centre of the view with fresh ids, in any project
  const MY_KEY = 'ui2.myTemplates';
  const myTemplates = () => { try { const a = JSON.parse(localStorage.getItem(MY_KEY) || '[]'); return Array.isArray(a) ? a : []; } catch { return []; } };
  const saveMy = a => { try { localStorage.setItem(MY_KEY, JSON.stringify(a)); return true; } catch { return false; } };
  function selectionPayload() {
    const g = graphOf(owner()), ns = g.nodes.filter(n => sel.has(n.id)), fns = {};
    for (const n of ns) if (n.type.startsWith('fn.') && fnOf(n.type)) fns[n.type.slice(3)] = fnOf(n.type);
    return {kind: 'stage-workshop-nodes', version: 1, nodes: structuredClone(ns), links: structuredClone(g.links.filter(l => sel.has(l.from) && sel.has(l.to))), functions: structuredClone(fns)};
  }
  function renderMy() {
    const a = myTemplates();
    $('lgMyList').innerHTML = a.map((t, i) => `<span class="lgMyT"><button class="lgTpl" data-mi="${i}" title="${esc(t.nodes.length + ' nodes')}"><b>${esc(t.name)}</b><small>${t.nodes.length} node${t.nodes.length === 1 ? '' : 's'}</small></button><button class="ghost small" data-mdel="${i}" title="Delete this template">×</button></span>`).join('') || '<p class="muted">None yet: select nodes, name them below, Save.</p>';
    $('lgMyList').querySelectorAll('[data-mi]').forEach(b => b.onclick = () => { $('lgTplBox').hidden = true; insertPayload(myTemplates()[+b.dataset.mi], 'centre'); });
    $('lgMyList').querySelectorAll('[data-mdel]').forEach(b => b.onclick = () => { const a = myTemplates(), t = a.splice(+b.dataset.mdel, 1)[0]; saveMy(a); renderMy(); app.toast(`Template "${t?.name}" deleted`); });
  }
  function saveMyTemplate() {
    if (!owner() || !sel.size) return app.toast('Select the nodes to keep first (Shift-click or Shift-drag)', true);
    const name = ($('lgMyName').value || '').trim() || `Template ${myTemplates().length + 1}`, a = myTemplates().filter(t => t.name !== name);
    a.push({name, ...selectionPayload()}); if (!saveMy(a)) return app.toast('The browser would not store it (storage full or blocked)', true);
    $('lgMyName').value = ''; renderMy(); app.toast(`Saved "${name}" (${sel.size} node${sel.size === 1 ? '' : 's'}) to My templates`);
  }
  // nodes / links / functions of a payload into this owner's graph with fresh ids; at = 'centre' (the view's centre) or offset +40
  function insertPayload(data, at) {
    if (!owner()) return app.toast('Choose whose graph first', true);
    if (data?.kind !== 'stage-workshop-nodes' || !Array.isArray(data.nodes)) return app.toast('Not a node set', true);
    let ox = 40, oy = 40;
    if (at === 'centre' && data.nodes.length) {
      const v = V(), rect = view().getBoundingClientRect(), cx = (rect.width / 2 - v.x) / v.z, cy = (rect.height / 2 - v.y) / v.z, xs = data.nodes.map(n => n.x), ys = data.nodes.map(n => n.y);
      ox = Math.round(cx - (Math.min(...xs) + Math.max(...xs)) / 2); oy = Math.round(cy - (Math.min(...ys) + Math.max(...ys)) / 2);
    }
    const map = new Map(), fmap = new Map(); let skipped = 0;
    edit(g => {
      const fl = (P().logicFunctions ||= {});
      for (const [id, f] of Object.entries(data.functions || {})) { const same = fl[id] && JSON.stringify(fl[id]) === JSON.stringify(f); const nid = same || !fl[id] ? id : freeId(Object.keys(fl).map(k => ({id: k})), 'f'); fl[nid] = structuredClone(f); fmap.set(id, nid); }
      for (const n of data.nodes) {
        const t = n.type.startsWith('fn.') ? 'fn.' + (fmap.get(n.type.slice(3)) || n.type.slice(3)) : n.type; if (!DEFS.get(t)) { skipped++; continue; }
        const c = {...structuredClone(n), type: t, id: freeId(g.nodes, 'n'), x: n.x + ox, y: n.y + oy}; g.nodes.push(c); map.set(n.id, c.id);
      }
      for (const l of data.links || []) if (map.has(l.from) && map.has(l.to)) g.links.push({...l, id: freeId(g.links, 'l'), from: map.get(l.from), to: map.get(l.to)});
    });
    sel = new Set(map.values()); render();
    app.toast(`${at === 'centre' ? 'Inserted' : 'Pasted'} ${data.name ? `"${data.name}": ` : ''}${map.size} node${map.size === 1 ? '' : 's'}${skipped ? ` (${skipped} unknown here, skipped)` : ''}${Object.keys(data.functions || {}).length ? ' with their functions' : ''}`);
    return map.size;
  }
  function insertTemplate(t) {
    if (!owner()) return app.toast('Choose whose graph first', true);
    const v = V(), rect = view().getBoundingClientRect(), cx = (rect.width / 2 - v.x) / v.z, cy = (rect.height / 2 - v.y) / v.z;
    const src = t.graph, xs = src.nodes.map(n => n.x), ys = src.nodes.map(n => n.y), ox = cx - (Math.min(...xs) + Math.max(...xs)) / 2, oy = cy - (Math.min(...ys) + Math.max(...ys)) / 2, map = new Map(), made = [];
    edit(g => {
      for (const n of src.nodes) { const m = addNode(g, n.type, Math.round(n.x + ox), Math.round(n.y + oy), structuredClone(n.values || {})); map.set(n.id, m); made.push(m.id); }
      for (const l of src.links) { const a = map.get(l.from), b = map.get(l.to); if (a && b) addLink(g, a, l.out, b, l.in); }
      for (const gr of src.groups || []) g.groups.push({...gr, id: freeId(g.groups, 'g'), x: Math.round(gr.x + ox), y: Math.round(gr.y + oy)});
    });
    sel = new Set(made); render(); app.toast(`Inserted "${t.label}" (${made.length} nodes)${issues.size ? ' · see the notes on the nodes' : ''}`);
  }
  function active() { return !root.hidden; }
  function show(o) { root.hidden = false; if (o !== undefined) setOwner(o); else render(); }
  function hide() { root.hidden = true; closeSearch(); $('lgTplBox').hidden = true; }
  function setOwner(o) {
    const k = kindOf(o);
    if (o && typeof o !== 'string' && !k) { ownerRef = null; sel.clear(); render(`${esc(ownerName(o))} (${esc(app.classOf(o))}) cannot own a graph yet: graphs belong to trigger volumes, script objects, teleport pads, native objects or the stage.`); return; }
    ownerRef = typeof o === 'string' ? o : o?.id || 'stage'; sel.clear(); selLink = selGroup = null; issues = new Map(); status = '';
    const ow = owner(); if (ow && TABLE) { const g = graphOf(ow); if (g.nodes.length) issuesOnly(ow); }
    render(); if (!views.has(ownerRef + ':fit')) { views.set(ownerRef + ':fit', 1); setTimeout(fit, 0); }
  }
  function issuesOnly(o) {   // compile into a scratch copy: shows notes without touching the project
    const keep = typeof o === 'string' ? null : JSON.stringify({event: o.event, once: o.once, cooldown: o.cooldown, graph: o.graph, actions: o.actions, events: o.events, target: o.target});
    compile(o); if (keep) { const k = JSON.parse(keep); for (const [f, v] of Object.entries(k)) { if (v === undefined) delete o[f]; else o[f] = v; } }
  }

  function widgetHtml(n, w, linked) {
    const v = n.values?.[w.name], a = `class="lgW" data-w="${esc(w.name)}" ${linked ? 'disabled' : ''}`;
    if (linked) return `<span class="lgLinked">← wired</span>`;
    if (w.type === 'bool') return `<input type="checkbox" ${a} ${v ? 'checked' : ''}>`;
    if (w.type === 'number') return `<input type="number" step="any" ${a} value="${esc(v ?? 0)}">`;
    if (w.type === 'enum') return `<select ${a}>${w.options.map(x => `<option ${x === v ? 'selected' : ''}>${esc(x)}</option>`).join('')}</select>`;
    if (w.type === 'vector') return `<span class="lgVec">${[0, 1, 2].map(i => `<input type="number" step="any" class="lgW" data-w="${esc(w.name)}" data-i="${i}" value="${esc((v || [0, 0, 0])[i])}">`).join('')}</span>`;
    if (w.type === 'object') { const t = v && app.byId(v); return `<span class="lgObj"><select ${a} data-obj="${esc(w.filter || '')}"><option value="${esc(v || '')}">${esc(t ? t.name : v ? 'missing: ' + v : 'Choose…')}</option></select><button class="lgPick" data-pick="${esc(w.name)}" data-filter="${esc(w.filter || '')}" title="Pick it by clicking it in the 3D view">Select</button>${t ? `<button class="lgFocus" data-focus="${esc(v)}" title="Show it in the 3D view">Focus</button>` : ''}</span>`; }
    return `<input type="text" ${a} value="${esc(v ?? '')}" ${w.suggest ? 'list="lgLive"' : ''}>`;
  }
  function nodeHtml(g, n) {
    const d = DEFS.get(n.type), cat = TABLE.categories[d?.category] || {colour: '#888'};
    if (!d) return `<div class="lgNode bad" data-id="${n.id}" style="left:${n.x}px;top:${n.y}px"><div class="lgHead"><b>${esc(n.type)}</b></div><div class="lgRow">unknown node</div></div>`;
    const pin = (dir, name, type) => `<i class="pin t-${type}" data-n="${n.id}" data-p="${esc(name)}" data-d="${dir}" title="${esc(name)} · ${esc(TABLE.pinTypes[type]?.label || type)}"></i>`;
    const linkedIn = name => g.links.some(l => l.to === n.id && l.in === name), rows = [];
    for (let i = 0; i < Math.max(d.exec.in.length, d.exec.out.length); i++) {
      const a = d.exec.in[i], b = d.exec.out[i];
      rows.push(`<div class="lgRow">${a ? pin('in', a, 'exec') + (a !== 'exec' ? `<span>${esc(a)}</span>` : '') : ''}<span class="grow"></span>${b ? (b !== 'then' ? `<span>${esc(b)}</span>` : '') + pin('out', b, 'exec') : ''}</div>`);
    }
    for (const w of d.widgets) { const inp = d.inputs.find(p => p.name === w.name); rows.push(`<div class="lgRow">${inp ? pin('in', inp.name, pinType(g, n, 'in', inp.name)) : '<i class="pinGap"></i>'}<label>${esc(w.label || w.name)}</label>${widgetHtml(n, w, inp && linkedIn(inp.name))}</div>`); }
    for (const p of d.inputs.filter(p => !d.widgets.some(w => w.name === p.name))) rows.push(`<div class="lgRow">${pin('in', p.name, pinType(g, n, 'in', p.name))}<span>${esc(p.name)}</span></div>`);
    for (const p of d.outputs) rows.push(`<div class="lgRow"><span class="grow"></span><span>${esc(p.name)}</span>${pin('out', p.name, pinType(g, n, 'out', p.name))}</div>`);
    const note = issues.get(n.id), reroute = n.type === 'flow.reroute';
    if (reroute) return `<div class="lgNode reroute ${sel.has(n.id) ? 'sel' : ''}" data-id="${n.id}" style="left:${n.x}px;top:${n.y}px"><div class="lgHead" title="Reroute (drag to move)"></div>${pin('in', 'in', pinType(g, n, 'in', 'in'))}${pin('out', 'out', pinType(g, n, 'out', 'out'))}</div>`;
    return `<div class="lgNode cat-${d.category} ${d.vm ? 'vm' : ''} ${sel.has(n.id) ? 'sel' : ''} ${n.collapsed ? 'collapsed' : ''} ${note ? 'note' : ''}" data-id="${n.id}" style="left:${n.x}px;top:${n.y}px;--cat:${cat.colour}" title="${esc(d.docs)}${note ? '\n\n⚠ ' + esc(note) : ''}">
      <div class="lgHead"><b>${esc(n.title || d.label)}</b>${d.needs ? '<em class="soon" title="Defined, not in the game yet: the build leaves it out">needs game</em>' : d.vm ? '<em>VM</em>' : ''}<button class="lgCol" title="Collapse / expand">${n.collapsed ? '▸' : '▾'}</button></div>
      ${n.collapsed ? '' : rows.join('')}${note ? `<div class="lgNote">⚠ ${esc(note)}</div>` : ''}</div>`;
  }
  function render(message) {
    if (!active()) return;
    const o = owner(), msg = $('lgMsg'), sel0 = $('lgOwnerSel');
    if (!TABLE || !P()) { msg.hidden = false; msg.textContent = TABLE ? 'Open a project first.' : 'Loading the node table…'; return; }
    // owners: the stage, every volume / pad / native that has a graph, and the current one
    const owners = [...(P().triggers || []).filter(t => t.position), ...P().objects.filter(x => x.kind === 'teleport' || (x.kind === 'native' && (x.events?.length || x.logic)))];
    if (o && o !== 'stage' && !owners.includes(o)) owners.unshift(o);
    sel0.innerHTML = `<option value="stage" ${ownerRef === 'stage' ? 'selected' : ''}>Stage graph</option>` + Object.entries(P().logicFunctions || {}).map(([id, f]) => `<option value="fn:${esc(id)}" ${'fn:' + id === ownerRef ? 'selected' : ''}>Function: ${esc(f.name)}</option>`).join('') + owners.map(x => `<option value="${esc(x.id)}" ${x.id === ownerRef ? 'selected' : ''}>${esc(ownerName(x))} · ${esc({volume: x.script ? 'script object' : 'volume', pad: 'teleport pad', native: app.classOf(x)}[kindOf(x)])}</option>`).join('');
    $('lgOwner').textContent = o ? (o === 'stage' ? 'Stage' : `${ownerName(o)}`) : 'No graph';
    $('lgStatus').textContent = o ? status || 'not changed in this session' : '';
    $('lgLive').innerHTML = TABLE.liveValues.map(v => `<option value="${v}">`).join('');
    const b = board(); b.querySelectorAll('.lgNode,.lgGroup').forEach(x => x.remove());
    if (!o) { msg.hidden = false; msg.innerHTML = message || 'Select a trigger volume, teleport pad or native object (outliner, viewport), or open the Stage graph.'; $('lgWires').innerHTML = ''; drawMini(); return; }
    const g = graphOf(o); msg.hidden = !!g.nodes.length; if (!g.nodes.length) msg.innerHTML = `Empty graph. Double-click the canvas (or + Node) and start with an event: ${o === 'stage' ? 'stage graphs need the VM (5b) to build' : {volume: 'On enter / exit / while inside / interact', pad: 'On touch', native: 'On spawn / killed / removed / despawn'}[kindOf(o)]}.`;
    b.insertAdjacentHTML('beforeend', g.groups.map(x => `<div class="lgGroup ${selGroup === x.id ? 'sel' : ''}" data-g="${x.id}" style="left:${x.x}px;top:${x.y}px;width:${x.w}px;height:${x.h}px;--gc:${x.colour}"><div class="lgGHead" title="Drag to move the group; double-click to rename">${esc(x.title)}</div></div>`).join('') + g.nodes.map(n => nodeHtml(g, n)).join(''));
    applyView(); drawWires();
  }
  function pinPos(g, nid, pin, dir) {
    const b = board().getBoundingClientRect(), z = V().z, el = board().querySelector(`.pin[data-n="${nid}"][data-p="${CSS.escape(pin)}"][data-d="${dir}"]`);
    if (el && el.offsetParent) { const r = el.getBoundingClientRect(); return [(r.left + r.width / 2 - b.left) / z, (r.top + r.height / 2 - b.top) / z]; }
    const nEl = board().querySelector(`.lgNode[data-id="${nid}"]`), n = nodeById(g, nid); if (!n) return null;   // collapsed: the header edge
    return [n.x + (dir === 'out' ? (nEl?.offsetWidth || 200) : 0), n.y + 14];
  }
  const curve = ([x1, y1], [x2, y2]) => { const dx = Math.max(40, Math.abs(x2 - x1) / 2); return `M${x1},${y1} C${x1 + dx},${y1} ${x2 - dx},${y2} ${x2},${y2}`; };
  function drawWires() {
    const o = owner(), svg = $('lgWires'); if (!o) { svg.innerHTML = ''; return; } const g = graphOf(o);
    svg.innerHTML = g.links.map(l => {
      const a = pinPos(g, l.from, l.out, 'out'), b = pinPos(g, l.to, l.in, 'in'); if (!a || !b) return '';
      const t = pinType(g, nodeById(g, l.from), 'out', l.out) || 'any', c = TABLE.pinTypes[t]?.colour || '#999';
      return `<path class="wire ${t === 'exec' ? 'exec' : ''} ${l.id === selLink ? 'sel' : ''}" data-l="${l.id}" d="${curve(a, b)}" stroke="${c}"/><path class="wireHit" data-l="${l.id}" d="${curve(a, b)}"/>`;
    }).join('') + (drag?.kind === 'wire' && drag.to ? `<path class="wire temp" d="${drag.dir === 'out' ? curve(drag.from, drag.to) : curve(drag.to, drag.from)}" stroke="${TABLE.pinTypes[drag.type]?.colour || '#fff'}"/>` : '');
    drawMini();
  }
  function drawMini() {
    const c = $('lgMini'), x = c.getContext('2d'); x.clearRect(0, 0, c.width, c.height); const o = owner(); if (!o || !active()) return;
    const g = graphOf(o, false); if (!g?.nodes.length) return; const v = V(), vr = view().getBoundingClientRect();
    const xs = g.nodes.map(n => n.x), ys = g.nodes.map(n => n.y), vx0 = -v.x / v.z, vy0 = -v.y / v.z, vx1 = vx0 + vr.width / v.z, vy1 = vy0 + vr.height / v.z;
    const x0 = Math.min(...xs, vx0) - 40, y0 = Math.min(...ys, vy0) - 40, x1 = Math.max(...xs.map(a => a + 220), vx1) + 40, y1 = Math.max(...ys.map(a => a + 120), vy1) + 40, s = Math.min(c.width / (x1 - x0), c.height / (y1 - y0));
    drawMini.map = {x0, y0, s};
    for (const n of g.nodes) { x.fillStyle = TABLE.categories[DEFS.get(n.type)?.category]?.colour || '#888'; x.globalAlpha = .8; x.fillRect((n.x - x0) * s, (n.y - y0) * s, 200 * s, 70 * s); }
    x.globalAlpha = 1; x.strokeStyle = '#fff'; x.strokeRect((vx0 - x0) * s, (vy0 - y0) * s, (vx1 - vx0) * s, (vy1 - vy0) * s);
  }
  function fit() {
    const o = owner(); if (!o) return; const g = graphOf(o); if (!g.nodes.length) return; const vr = view().getBoundingClientRect(), v = V();
    const x0 = Math.min(...g.nodes.map(n => n.x)) - 40, y0 = Math.min(...g.nodes.map(n => n.y)) - 40, x1 = Math.max(...g.nodes.map(n => n.x + 260)) + 40, y1 = Math.max(...g.nodes.map(n => n.y + 160)) + 40;
    v.z = Math.max(.3, Math.min(1.2, vr.width / (x1 - x0), vr.height / (y1 - y0))); v.x = (vr.width - (x1 - x0) * v.z) / 2 - x0 * v.z; v.y = (vr.height - (y1 - y0) * v.z) / 2 - y0 * v.z; applyView(); drawWires();
  }
  const toBoard = (cx, cy) => { const r = view().getBoundingClientRect(), v = V(); return [(cx - r.left - v.x) / v.z, (cy - r.top - v.y) / v.z]; };

  // ---- search / add ------------------------------------------------------------------------------------------------
  let searchAt = null, searchWire = null, searchIdx = 0;
  function openSearch(cx, cy, wire = null) {
    if (!owner()) return app.toast('Choose a graph owner first', true);
    const s = $('lgSearch'), r = root.getBoundingClientRect(); s.hidden = false; s.style.left = Math.min(cx - r.left, r.width - 330) + 'px'; s.style.top = Math.min(cy - r.top, r.height - 360) + 'px';
    searchAt = toBoard(cx, cy); searchWire = wire; $('lgQ').value = ''; searchIdx = 0; listSearch(); $('lgQ').focus();
  }
  function closeSearch() { $('lgSearch').hidden = true; searchWire = null; if (document.activeElement === $('lgQ')) $('lgQ').blur(); }   // keys go back to the canvas (Ctrl Z, Delete)
  function candidates() {
    const k = kindOf(owner()), q = $('lgQ').value.toLowerCase().trim(), g = graphOf(owner());
    const fns = Object.keys(P().logicFunctions || {}).map(id => fnDef('fn.' + id)).filter(Boolean).filter(d => ownerRef !== 'fn:' + d.id.slice(3));
    return [...TABLE.nodes, ...fns].filter(d => !q || (d.label + ' ' + d.id + ' ' + d.docs).toLowerCase().includes(q)).filter(d => {
      if (!searchWire) return true; const probe = {id: '_', type: d.id, values: {}};
      return searchWire.dir === 'out' ? [...d.exec.in, ...d.inputs.map(p => p.name)].some(p => !canLink({...g, nodes: [...g.nodes, probe]}, nodeById(g, searchWire.node), searchWire.pin, probe, p))
        : [...d.exec.out, ...d.outputs.map(p => p.name)].some(p => !canLink({...g, nodes: [...g.nodes, probe]}, probe, p, nodeById(g, searchWire.node), searchWire.pin));
    }).map(d => ({d, off: !allowed(d, k) ? 'not on this owner' : '', rank: !q ? 0 : d.label.toLowerCase().startsWith(q) ? 0 : d.label.toLowerCase().includes(q) ? 1 : 2}))
      .sort((a, b) => a.rank - b.rank || (!!a.off - !!b.off));   // label matches first, usable nodes before greyed ones
  }
  function listSearch() {
    const list = candidates(); searchIdx = Math.min(searchIdx, list.length - 1);
    $('lgList').innerHTML = list.map(({d, off}, i) => `<div class="lgItem ${off ? 'off' : ''} ${i === searchIdx ? 'on' : ''}" data-i="${i}" title="${esc(d.docs)}"><span class="dot" style="background:${TABLE.categories[d.category].colour}"></span><b>${esc(d.label)}</b>${d.needs ? '<em class="soon">needs game</em>' : ''}<small>${esc(off || TABLE.categories[d.category].label + (d.vm ? ' · v4' : ''))}</small></div>`).join('') || '<div class="sub">No node matches.</div>';
    $('lgList').querySelectorAll('.lgItem').forEach(el => el.onclick = () => pick(list[+el.dataset.i]));
    $('lgList').querySelector('.on')?.scrollIntoView({block: 'nearest'});
  }
  function pick(c) {
    if (!c) return; if (c.off) return app.toast(`${c.d.label}: ${c.off}`, true);
    const at = searchAt, wire = searchWire; closeSearch(); let made = null;
    edit(g => {
      let [x, y] = at; while (g.nodes.some(n => Math.abs(n.x - x) < 30 && Math.abs(n.y - y) < 30)) y += 120;   // never on top of another node
      made = addNode(g, c.d.id, x, y);
      if (wire) {
        const other = nodeById(g, wire.node), pins = wire.dir === 'out' ? [...c.d.exec.in, ...c.d.inputs.map(p => p.name)] : [...c.d.exec.out, ...c.d.outputs.map(p => p.name)];
        const p = pins.find(p => wire.dir === 'out' ? !canLink(g, other, wire.pin, made, p) : !canLink(g, made, p, other, wire.pin));
        if (p) { if (wire.dir === 'out') { dropInput(g, made.id, p); addLink(g, other, wire.pin, made, p); } else { dropInput(g, other.id, wire.pin); addLink(g, made, p, other, wire.pin); } }
      }
    });
    sel = new Set([made.id]); render();
  }
  const dropInput = (g, nid, pin) => { const n = nodeById(g, nid); if (pinType(g, n, 'in', pin) !== 'exec') g.links = g.links.filter(l => !(l.to === nid && l.in === pin)); };   // a data input takes one wire
  $('lgQ').oninput = () => { searchIdx = 0; listSearch(); };
  $('lgQ').onkeydown = e => { const n = candidates().length; if (e.key === 'ArrowDown') { searchIdx = Math.min(n - 1, searchIdx + 1); listSearch(); e.preventDefault(); } else if (e.key === 'ArrowUp') { searchIdx = Math.max(0, searchIdx - 1); listSearch(); e.preventDefault(); } else if (e.key === 'Enter') pick(candidates()[searchIdx]); else if (e.key === 'Escape') closeSearch(); e.stopPropagation(); };

  // ---- pointer interaction -----------------------------------------------------------------------------------------
  // selection changes only toggle classes: re-rendering between the two clicks of a double-click would stop it
  function markSel() {
    board().querySelectorAll('.lgNode').forEach(el => el.classList.toggle('sel', sel.has(el.dataset.id)));
    board().querySelectorAll('.lgGroup').forEach(el => el.classList.toggle('sel', el.dataset.g === selGroup));
    $('lgWires').querySelectorAll('.wire').forEach(el => el.classList.toggle('sel', el.dataset.l === selLink));
  }
  const vw = view();
  vw.addEventListener('wheel', e => { e.preventDefault(); const v = V(), r = vw.getBoundingClientRect(), mx = e.clientX - r.left, my = e.clientY - r.top, z = Math.max(.25, Math.min(2, v.z * Math.exp(-e.deltaY * .0015)));
    v.x = mx - (mx - v.x) * z / v.z; v.y = my - (my - v.y) * z / v.z; v.z = z; applyView(); drawWires(); }, {passive: false});
  vw.addEventListener('pointerdown', e => { $('lgTplBox').hidden = true;
    if (e.target.closest('input,select,button,label,textarea')) return; closeSearch();
    const o = owner(); const g = o && graphOf(o);
    const pinEl = e.target.closest('.pin');
    if (pinEl && g) {
      const nid = pinEl.dataset.n, pin = pinEl.dataset.p, dir = pinEl.dataset.d, n = nodeById(g, nid);
      if (dir === 'in' && pinType(g, n, 'in', pin) !== 'exec') {   // grabbing a wired data input picks its wire up again
        const l = g.links.find(l => l.to === nid && l.in === pin);
        if (l) { app.snapshot(); g.links = g.links.filter(x => x !== l); compile(o); app.changed(); render(); drag = {kind: 'wire', node: l.from, pin: l.out, dir: 'out', type: pinType(g, nodeById(g, l.from), 'out', l.out), from: pinPos(g, l.from, l.out, 'out'), noSnap: true}; vw.setPointerCapture(e.pointerId); return; }
      }
      drag = {kind: 'wire', node: nid, pin, dir, type: pinType(g, n, dir, pin), from: pinPos(g, nid, pin, dir)}; vw.setPointerCapture(e.pointerId); return;
    }
    const head = e.target.closest('.lgHead'), gHead = e.target.closest('.lgGHead');
    if (head && g) {
      const id = head.closest('.lgNode').dataset.id; if (e.shiftKey) sel.has(id) ? sel.delete(id) : sel.add(id); else if (!sel.has(id)) sel = new Set([id]);
      selLink = selGroup = null; drag = {kind: 'move', start: [e.clientX, e.clientY], orig: new Map([...sel].map(i => { const n = nodeById(g, i); return [i, [n.x, n.y]]; })), moved: false};
      markSel(); vw.setPointerCapture(e.pointerId); return;
    }
    if (gHead && g) {
      const gid = gHead.closest('.lgGroup').dataset.g, gr = g.groups.find(x => x.id === gid); selGroup = gid; sel.clear(); selLink = null;
      const inside = g.nodes.filter(n => n.x >= gr.x && n.y >= gr.y && n.x <= gr.x + gr.w && n.y <= gr.y + gr.h).map(n => n.id);
      drag = {kind: 'group', gid, start: [e.clientX, e.clientY], gorig: [gr.x, gr.y], orig: new Map(inside.map(i => { const n = nodeById(g, i); return [i, [n.x, n.y]]; })), moved: false};
      markSel(); vw.setPointerCapture(e.pointerId); return;
    }
    const wire = e.target.closest('.wireHit');
    if (wire) { selLink = wire.dataset.l; sel.clear(); selGroup = null; markSel(); return; }
    if (e.target.closest('.lgNode')) { const id = e.target.closest('.lgNode').dataset.id; if (!sel.has(id)) { sel = new Set([id]); selLink = null; markSel(); } return; }
    if (e.shiftKey) { drag = {kind: 'box', start: [e.clientX, e.clientY]}; vw.setPointerCapture(e.pointerId); return; }
    const v = V(); drag = {kind: 'pan', start: [e.clientX, e.clientY], orig: [v.x, v.y], moved: false}; vw.setPointerCapture(e.pointerId);
  });
  vw.addEventListener('pointermove', e => {
    if (!drag) return; const o = owner(), g = o && graphOf(o), v = V();
    if (drag.kind === 'pan') { v.x = drag.orig[0] + e.clientX - drag.start[0]; v.y = drag.orig[1] + e.clientY - drag.start[1]; drag.moved = true; applyView(); }
    else if (drag.kind === 'move' || drag.kind === 'group') {
      const dx = (e.clientX - drag.start[0]) / v.z, dy = (e.clientY - drag.start[1]) / v.z;
      if (!drag.moved && Math.hypot(dx, dy) > 2) { drag.moved = true; app.snapshot(); }
      if (!drag.moved) return;
      for (const [id, [x, y]] of drag.orig) { const n = nodeById(g, id); n.x = Math.round(x + dx); n.y = Math.round(y + dy); const el = board().querySelector(`.lgNode[data-id="${id}"]`); if (el) { el.style.left = n.x + 'px'; el.style.top = n.y + 'px'; } }
      if (drag.kind === 'group') { const gr = g.groups.find(x => x.id === drag.gid); gr.x = Math.round(drag.gorig[0] + dx); gr.y = Math.round(drag.gorig[1] + dy); const el = board().querySelector(`.lgGroup[data-g="${drag.gid}"]`); el.style.left = gr.x + 'px'; el.style.top = gr.y + 'px'; }
      drawWires();
    } else if (drag.kind === 'wire') { drag.to = toBoard(e.clientX, e.clientY); drawWires(); }
    else if (drag.kind === 'box') { const b = $('lgBox'), r = vw.getBoundingClientRect(), [x0, y0] = drag.start; b.hidden = false; Object.assign(b.style, {left: Math.min(x0, e.clientX) - r.left + 'px', top: Math.min(y0, e.clientY) - r.top + 'px', width: Math.abs(e.clientX - x0) + 'px', height: Math.abs(e.clientY - y0) + 'px'}); }
  });
  vw.addEventListener('pointerup', e => {
    const d = drag; drag = null; if (!d) return; const o = owner(), g = o && graphOf(o);
    if (d.kind === 'pan' && !d.moved) { sel.clear(); selLink = selGroup = null; markSel(); }
    if ((d.kind === 'move' || d.kind === 'group') && d.moved) { app.changed(); drawMini(); }
    if (d.kind === 'box') {
      $('lgBox').hidden = true; const [a, b] = [toBoard(...d.start), toBoard(e.clientX, e.clientY)], x0 = Math.min(a[0], b[0]), x1 = Math.max(a[0], b[0]), y0 = Math.min(a[1], b[1]), y1 = Math.max(a[1], b[1]);
      sel = new Set(g.nodes.filter(n => n.x + 100 >= x0 && n.x <= x1 && n.y + 30 >= y0 && n.y <= y1).map(n => n.id)); markSel();
    }
    if (d.kind === 'wire') {
      const target = document.elementFromPoint(e.clientX, e.clientY)?.closest('.pin');
      if (target && target.dataset.d !== d.dir) {
        const [a, ap, b, bp] = d.dir === 'out' ? [d.node, d.pin, target.dataset.n, target.dataset.p] : [target.dataset.n, target.dataset.p, d.node, d.pin];
        const why = canLink(g, nodeById(g, a), ap, nodeById(g, b), bp);
        if (why) { app.toast('Cannot wire: ' + why, true); drawWires(); return; }
        if (g.links.some(l => l.from === a && l.out === ap && l.to === b && l.in === bp)) { drawWires(); return; }
        const doIt = gg => { dropInput(gg, b, bp); addLink(gg, nodeById(gg, a), ap, nodeById(gg, b), bp); };
        if (d.noSnap) { doIt(g); compile(o); app.changed(); render(); } else edit(doIt);
      } else if (!target) { openSearch(e.clientX, e.clientY, {node: d.node, pin: d.pin, dir: d.dir}); drawWires(); }
      else drawWires();
    }
  });
  vw.addEventListener('dblclick', e => {
    if (e.target.closest('input,select,button')) return;
    const gh = e.target.closest('.lgGHead'); if (gh) { app.askText('Group name', '', gh.textContent).then(t => { if (t != null) edit(g => { g.groups.find(x => x.id === gh.closest('.lgGroup').dataset.g).title = t.slice(0, 60); }); }); return; }
    const head = e.target.closest('.lgNode .lgHead b'); if (head) { const n = nodeById(graphOf(owner()), head.closest('.lgNode').dataset.id); app.askText('Node title', 'Leave it empty for the default title.', n.title || '').then(t => { if (t != null) edit(g => { nodeById(g, n.id).title = t || null; }); }); return; }
    const wire = e.target.closest('.wireHit');
    if (wire) {   // double-click a wire: a reroute dot on it
      const [x, y] = toBoard(e.clientX, e.clientY);
      edit(g => { const l = g.links.find(l => l.id === wire.dataset.l); if (!l) return; const r = addNode(g, 'flow.reroute', x - 8, y - 8); g.links = g.links.filter(z => z !== l); g.links.push({id: freeId(g.links, 'l'), from: l.from, out: l.out, to: r.id, in: 'in'}, {id: freeId(g.links, 'l') + 'b', from: r.id, out: 'out', to: l.to, in: l.in}); });
      return;
    }
    if (!e.target.closest('.lgNode')) openSearch(e.clientX, e.clientY);
  });
  // widgets
  root.addEventListener('focusin', e => {   // object pickers fill their list on first use (1000+ placements)
    const s = e.target.closest('select[data-obj]'); if (!s || s.dataset.filled) return; s.dataset.filled = 1; const cur = s.value, onlyVol = s.dataset.obj === 'volume', onlyNat = s.dataset.obj === 'native';
    const list = app.items().filter(it => onlyNat ? it.o.kind === 'native' : !onlyVol || it.o.kind === 'trigger' || it.o.kind === 'teleport').sort((a, b) => a.name.localeCompare(b.name));
    s.innerHTML = `<option value="">Choose…</option>` + list.map(it => `<option value="${esc(it.id)}" ${it.id === cur ? 'selected' : ''}>${esc(it.name)} · ${esc(it.cls)}</option>`).join('');
  });
  root.addEventListener('change', e => {
    const w = e.target.closest('.lgW'); if (!w) return; const nid = w.closest('.lgNode').dataset.id, name = w.dataset.w;
    edit(g => { const n = nodeById(g, nid), d = DEFS.get(n.type), def = d.widgets.find(x => x.name === name);
      if (def.type === 'vector') { const v = [...(n.values[name] || [0, 0, 0])]; v[+w.dataset.i] = Number(w.value) || 0; n.values[name] = v; }
      else n.values[name] = def.type === 'bool' ? w.checked : def.type === 'number' ? (Number(w.value) || 0) : w.value; });
  });
  root.addEventListener('click', e => {
    const pk = e.target.closest('.lgPick');   // #82 pick the object in the 3D view, back here with the value set
    if (pk) { const nid = pk.closest('.lgNode').dataset.id, name = pk.dataset.pick, d = DEFS.get(nodeById(graphOf(owner()), nid)?.type);
      app.pickObject({filter: pk.dataset.filter, label: `${d?.label || 'node'} · ${d?.widgets.find(x => x.name === name)?.label || name}`}).then(id => { if (id) edit(g => { const n = nodeById(g, nid); if (n) (n.values ||= {})[name] = id; }); }); return; }
    const fc = e.target.closest('.lgFocus'); if (fc) { app.focusObject(fc.dataset.focus); return; }
    const c = e.target.closest('.lgCol'); if (c) { const id = c.closest('.lgNode').dataset.id; edit(g => { const n = nodeById(g, id); n.collapsed = !n.collapsed; }); }
  });
  $('lgMini').addEventListener('click', e => { const m = drawMini.map; if (!m) return; const r = e.target.getBoundingClientRect(), v = V(), vr = view().getBoundingClientRect(); const bx = (e.clientX - r.left) / m.s + m.x0, by = (e.clientY - r.top) / m.s + m.y0; v.x = vr.width / 2 - bx * v.z; v.y = vr.height / 2 - by * v.z; applyView(); drawWires(); });
  $('lgAdd').onclick = () => { const r = view().getBoundingClientRect(); openSearch(r.left + r.width / 2 - 150, r.top + 80); };
  $('lgGroup').onclick = groupSelection; $('lgFit').onclick = fit;
  // ---- Find in graph: the nodes of this graph whose title, label, type or a widget value contains the text; Enter /
  // the next keypress steps through them (selected, centred, flashed). k / n beside the box.
  let findAt = -1;
  function findMatches(q) {
    const o = owner(); if (!o || !q) return []; const g = graphOf(o); q = q.toLowerCase();
    return g.nodes.filter(n => { const d = DEFS.get(n.type); return [n.title, d?.label, n.type, ...Object.values(n.values || {}).map(v => typeof v === 'object' ? JSON.stringify(v) : String(v))].some(t => String(t ?? '').toLowerCase().includes(q)); });
  }
  function centreOn(n) {
    const el = board().querySelector(`.lgNode[data-id="${n.id}"]`), vr = view().getBoundingClientRect(), v = V(), w = el?.offsetWidth || 220, h = el?.offsetHeight || 100;
    v.x = vr.width / 2 - (n.x + w / 2) * v.z; v.y = vr.height / 2 - (n.y + h / 2) * v.z; applyView(); drawWires();
    el?.classList.add('lgFlash'); setTimeout(() => el?.classList.remove('lgFlash'), 900);
  }
  function findStep(dir = 1) {
    const m = findMatches($('lgFindQ').value.trim()); $('lgFindN').textContent = $('lgFindQ').value.trim() ? (m.length ? `${Math.min(m.length, findAt + 1 || 1)} / ${m.length}` : 'none') : '';
    if (!m.length) return; findAt = (findAt + dir + m.length) % m.length; const n = m[findAt];
    sel = new Set([n.id]); render(); centreOn(n); $('lgFindN').textContent = `${findAt + 1} / ${m.length}`;
  }
  $('lgFindQ').oninput = () => { findAt = -1; findStep(1); };
  $('lgFindQ').onkeydown = e => { if (e.key === 'Enter') { findStep(e.shiftKey ? -1 : 1); e.preventDefault(); } else if (e.key === 'Escape') { $('lgFindQ').value = ''; $('lgFindN').textContent = ''; $('lgFindQ').blur(); } e.stopPropagation(); };
  // ---- Align: left / top edges to the first-placed (left-most / top-most) node, or even spacing across / down
  function alignSelection(how) {
    const o = owner(); if (!o) return; const g = graphOf(o), ns = g.nodes.filter(n => sel.has(n.id)); if (ns.length < 2) return app.toast('Select two or more nodes to line up', true);
    const el = id => board().querySelector(`.lgNode[data-id="${id}"]`), W = n => el(n.id)?.offsetWidth || 220, H = n => el(n.id)?.offsetHeight || 100;
    edit(gg => {
      const m = gg.nodes.filter(n => sel.has(n.id));
      if (how === 'left') { const x = Math.min(...m.map(n => n.x)); m.forEach(n => n.x = x); }
      if (how === 'top') { const y = Math.min(...m.map(n => n.y)); m.forEach(n => n.y = y); }
      if (how === 'h' || how === 'v') {
        const k = how === 'h' ? 'x' : 'y', size = how === 'h' ? W : H; m.sort((a, b) => a[k] - b[k]);
        const total = m.reduce((t, n) => t + size(n), 0), span = m[m.length - 1][k] + size(m[m.length - 1]) - m[0][k], gap = Math.max(24, (span - total) / (m.length - 1));
        let at = m[0][k]; for (const n of m) { n[k] = Math.round(at); at += size(n) + gap; }
      }
    });
    app.toast(`Lined up ${ns.length} nodes`);
  }
  $('lgAlign').onchange = e => { const v = e.target.value; e.target.value = ''; if (v) alignSelection(v); };
  $('lgMySaveB').onclick = saveMyTemplate; $('lgMyName').onkeydown = e => { if (e.key === 'Enter') saveMyTemplate(); e.stopPropagation(); };
  $('lgOwnerSel').onchange = e => { const v = e.target.value; setOwner(v === 'stage' || v.startsWith('fn:') ? v : app.byId(v)?.o || [...P().objects, ...P().triggers].find(o => o.id === v)); };
  new ResizeObserver(() => { if (active()) { drawWires(); } }).observe(root);

  // ---- Import / Export graph JSON (for people and AI agents writing a room's logic directly) ------------------------
  // validate(): version 1, unique node ids, every node type in nodes.json, links between existing nodes on pins the
  // node types have, pin types compatible (canLink), one wire per data input; groups optional. Errors block the
  // import; notes (nodes not usable on this owner, VM nodes) are listed but allowed.
  function validate(g, k) {
    const errors = [], notes = [];
    if (!g || typeof g !== 'object') return {errors: ['not a JSON object'], notes};
    if (g.version !== 1) errors.push('version must be 1');
    if (!Array.isArray(g.nodes)) errors.push('nodes must be a list'); if (!Array.isArray(g.links || [])) errors.push('links must be a list');
    if (errors.length) return {errors, notes};
    const ids = new Set();
    for (const n of g.nodes) {
      if (!n?.id || ids.has(n.id)) errors.push(`node id ${JSON.stringify(n?.id)} missing or repeated`); ids.add(n?.id);
      const d = DEFS.get(n?.type); if (!d) { errors.push(`node ${n?.id}: unknown type ${JSON.stringify(n?.type)} (not in nodes.json)`); continue; }
      if (!Number.isFinite(n.x) || !Number.isFinite(n.y)) notes.push(`node ${n.id}: no x / y, placed at 0, 0`);
      for (const w of Object.keys(n.values || {})) if (!d.widgets.some(x => x.name === w)) errors.push(`node ${n.id} (${d.label}): unknown value "${w}" (widgets: ${d.widgets.map(x => x.name).join(', ') || 'none'})`);
      if (d.vm) notes.push(`node ${n.id} (${d.label}) runs in the v4 VM (built by the server into sidecar v4)`);
      else if (!allowed(d, k)) notes.push(`node ${n.id} (${d.label}) is not available on ${k === 'stage' ? 'the stage graph' : 'this owner'}`);
    }
    const into = new Set();
    for (const l of g.links || []) {
      const a = g.nodes.find(n => n.id === l.from), b = g.nodes.find(n => n.id === l.to);
      if (!a || !b) { errors.push(`link ${l.id || '?'}: node ${!a ? l.from : l.to} does not exist`); continue; }
      if (!DEFS.get(a.type) || !DEFS.get(b.type)) continue;
      const ta = pinType(g, a, 'out', l.out), tb = pinType(g, b, 'in', l.in);
      if (!ta) { errors.push(`link ${l.id || '?'}: ${DEFS.get(a.type).label} has no output pin "${l.out}"`); continue; }
      if (!tb) { errors.push(`link ${l.id || '?'}: ${DEFS.get(b.type).label} has no input pin "${l.in}"`); continue; }
      const why = canLink(g, a, l.out, b, l.in); if (why) errors.push(`link ${l.id || '?'} ${l.from}.${l.out} -> ${l.to}.${l.in}: ${why}`);
      if (tb !== 'exec') { const key = l.to + '|' + l.in; if (into.has(key)) errors.push(`input ${l.to}.${l.in} has more than one wire`); into.add(key); }
    }
    return {errors, notes};
  }
  function openJson(mode) {
    const o = owner(); if (!o) return app.toast('Choose a graph owner first', true);
    $('lgJson').hidden = false; $('lgJsonTitle').textContent = mode === 'export' ? `Graph of ${ownerName(o)} (JSON)` : `Import a graph for ${ownerName(o)}`;
    $('lgJsonText').value = mode === 'export' ? JSON.stringify(graphOf(o), null, 2) : ''; $('lgJsonReport').innerHTML = mode === 'export' ? '<p class="muted">The shape and a full example are in tools/stage_editor/nodes.json ("about", "example").</p>' : '<p class="muted">Paste a graph (shape: tools/stage_editor/nodes.json "about" / "example"), Check, then Replace this graph (undoable).</p>';
    $('lgJsonApply').hidden = mode === 'export'; $('lgJsonCheck').hidden = mode === 'export'; $('lgJsonText').focus();
  }
  function checkJson() {
    let g; try { g = JSON.parse($('lgJsonText').value); } catch (e) { $('lgJsonReport').innerHTML = `<p class="badText">Not valid JSON: ${esc(e.message)}</p>`; return null; }
    if (g && !g.version && g.logic) g = g.logic;   // a whole owner object pasted: take its graph
    const {errors, notes} = validate(g, kindOf(owner()));
    $('lgJsonReport').innerHTML = (errors.length ? `<p class="badText">${errors.length} error${errors.length > 1 ? 's' : ''}:</p><ul>${errors.slice(0, 40).map(x => `<li>${esc(x)}</li>`).join('')}</ul>` : `<p class="okText">Valid: ${g.nodes.length} nodes, ${(g.links || []).length} links.</p>`)
      + (notes.length ? `<ul class="muted">${notes.slice(0, 40).map(x => `<li>${esc(x)}</li>`).join('')}</ul>` : '');
    return errors.length ? null : g;
  }
  $('lgExport').onclick = () => openJson('export'); $('lgImport').onclick = () => openJson('import');
  $('lgTpl').onclick = e => { e.stopPropagation(); showTemplates(); };
  $('lgJsonClose').onclick = () => $('lgJson').hidden = true; $('lgJsonCheck').onclick = checkJson;
  $('lgJsonCopy').onclick = () => { navigator.clipboard?.writeText($('lgJsonText').value).then(() => app.toast('Copied'), () => { $('lgJsonText').select(); app.toast('Select all + Ctrl C to copy'); }); };
  $('lgJsonApply').onclick = () => {
    const g = checkJson(); if (!g) return; const o = owner();
    for (const n of g.nodes) { n.x = Number.isFinite(n.x) ? n.x : 0; n.y = Number.isFinite(n.y) ? n.y : 0; n.values = {...Object.fromEntries((DEFS.get(n.type).widgets || []).map(w => [w.name, structuredClone(w.default)])), ...(n.values || {})}; n.collapsed = !!n.collapsed; n.title ??= null; }
    g.links = (g.links || []).map((l, i) => ({...l, id: l.id || 'l' + (i + 1)})); g.groups ||= [];
    app.snapshot(); if (o === 'stage') P().logic = g; else o.logic = g; compile(o); app.changed(); $('lgJson').hidden = true; sel.clear(); render(); setTimeout(fit, 0);
    app.toast(`Graph imported: ${g.nodes.length} nodes · ${status}`);
  };
  $('lgJsonText').addEventListener('keydown', e => e.stopPropagation());

  // ---- functions (subgraphs) and copy / paste between graphs and projects ---------------------------------------
  async function makeFunction() {
    const o = owner(); if (!o) return; const g = graphOf(o), ns = g.nodes.filter(n => sel.has(n.id));
    if (!ns.length) return app.toast('Select the nodes to turn into a function (Shift-click or Shift-drag)', true);
    if (ns.some(n => DEFS.get(n.type)?.category === 'event')) return app.toast('Events stay outside a function: select the nodes after the event', true);
    const name = await app.askText('Make a function', 'The selected nodes become one reusable node.', 'My function'); if (!name) return;
    const inSel = id => sel.has(id), ports = [], boundary = g.links.filter(l => inSel(l.from) !== inSel(l.to));
    const portFor = (dir, node, pin, type) => { let q = ports.find(q => q.dir === dir && q.node === node && q.pin === pin); if (!q) { let nm = pin, i = 2; while (ports.some(x => x.dir === dir && x.name === nm)) nm = `${pin} ${i++}`; q = {dir, name: nm, type, node, pin}; ports.push(q); } return q; };
    for (const l of boundary) { if (inSel(l.to)) portFor('in', l.to, l.in, pinType(g, nodeById(g, l.to), 'in', l.in)); else portFor('out', l.from, l.out, pinType(g, nodeById(g, l.from), 'out', l.out)); }
    const x0 = Math.min(...ns.map(n => n.x)), y0 = Math.min(...ns.map(n => n.y)), fl = P().logicFunctions || {}, id = freeId(Object.keys(fl).map(k => ({id: k})), 'f');
    const f = {name: name.slice(0, 60), nodes: ns.map(n => ({...structuredClone(n), x: n.x - x0, y: n.y - y0})), links: g.links.filter(l => inSel(l.from) && inSel(l.to)).map(l => ({...l})), ports, groups: []};
    let call = null;
    edit(gg => {
      (P().logicFunctions ||= {})[id] = f; call = addNode(gg, 'fn.' + id, x0, y0);
      for (const l of boundary) {
        if (inSel(l.to)) { const q = ports.find(q => q.dir === 'in' && q.node === l.to && q.pin === l.in); gg.links.push({...l, id: freeId(gg.links, 'l'), to: call.id, in: q.name}); }
        else { const q = ports.find(q => q.dir === 'out' && q.node === l.from && q.pin === l.out); gg.links.push({...l, id: freeId(gg.links, 'l'), from: call.id, out: q.name}); }
      }
      gg.nodes = gg.nodes.filter(n => !inSel(n.id)); gg.links = gg.links.filter(l => !inSel(l.from) && !inSel(l.to));
    });
    sel = new Set([call.id]); render(); app.toast(`Function "${f.name}": ${ns.length} nodes, ${ports.length} pins · reuse it from search (ƒ ${f.name}), edit it from the owner list`);
  }
  function copySelection() {
    const o = owner(); if (!o || !sel.size) return false; const g = graphOf(o), ns = g.nodes.filter(n => sel.has(n.id)), fns = {};
    for (const n of ns) if (n.type.startsWith('fn.') && fnOf(n.type)) fns[n.type.slice(3)] = fnOf(n.type);
    const payload = JSON.stringify({kind: 'stage-workshop-nodes', version: 1, nodes: ns, links: g.links.filter(l => sel.has(l.from) && sel.has(l.to)), functions: fns});
    try { localStorage.setItem('ui2.nodeClipboard', payload); } catch {}
    navigator.clipboard?.writeText(payload).catch(() => {}); app.toast(`Copied ${ns.length} node${ns.length > 1 ? 's' : ''} (paste with Ctrl V here or in another project)`); return true;
  }
  async function pasteNodes() {
    const o = owner(); if (!o) return; let data = null;
    try { data = JSON.parse(await navigator.clipboard.readText()); } catch {}
    if (data?.kind !== 'stage-workshop-nodes') { try { data = JSON.parse(localStorage.getItem('ui2.nodeClipboard')); } catch {} }
    if (data?.kind !== 'stage-workshop-nodes') return app.toast('Nothing to paste: copy nodes with Ctrl C first', true);
    insertPayload(data, 'offset');
  }
  $('lgFn').onclick = makeFunction;

  // keys while the Logic workspace is shown (returns true when handled)
  function key(e) {
    if (!$('lgJson').hidden) { if (e.key === 'Escape') { $('lgJson').hidden = true; return true; } return false; }
    if (!$('lgSearch').hidden && e.key === 'Escape') { closeSearch(); return true; }
    const c = e.ctrlKey || e.metaKey;
    if (e.key === 'Delete' || e.key === 'Backspace') return deleteSelection();
    if (c && e.key.toLowerCase() === 'd') { duplicateSelection(); return true; }
    if (c && e.key.toLowerCase() === 'c') return copySelection();
    if (c && e.key.toLowerCase() === 'v') { pasteNodes(); return true; }
    if (c && e.key.toLowerCase() === 'g') { groupSelection(); return true; }
    if (c && e.key.toLowerCase() === 'f') { $('lgFindQ').focus(); $('lgFindQ').select(); return true; }
    if (!c && e.key.toLowerCase() === 'f') { fit(); return true; }
    if (e.key === 'Escape') { sel.clear(); selLink = selGroup = null; render(); return true; }
    return false;
  }
  // ---- live highlight (roadmap 5d): the server serves the running test game's fired node ids (v4 debug table ids
  // "owner/node") and variable values; GET /api/live?id=<project>&since=<cursor> -> {lines: [{t, node, vars?}], next}.
  // Fired nodes of the shown owner flash; the bar shows the latest variable values. Without the endpoint the button
  // says so and retries every 10 s.
  let live = null;
  function liveStart(pid) { liveStop(); live = {pid, since: 0, fired: new Map(), vars: {}, missing: false, timer: null}; $('lgLive').textContent = 'Live: on'; $('lgLive').classList.add('on'); livePoll(); }
  function liveStop() { if (live?.timer) clearTimeout(live.timer); live = null; $('lgLive').textContent = 'Live: off'; $('lgLive').classList.remove('on'); $('lgVars').textContent = ''; board().querySelectorAll('.lgNode.fired').forEach(n => n.classList.remove('fired')); }
  async function livePoll() {
    if (!live) return; const me = live;
    try {
      const r = await fetch(`/api/live?id=${encodeURIComponent(me.pid)}&since=${me.since}`); if (!r.ok) throw Error(r.status);
      const j = await r.json(); if (live !== me) return; me.missing = false; me.since = j.next ?? me.since;
      for (const l of j.lines || []) { if (l.node) me.fired.set(l.node, performance.now()); if (l.vars) Object.assign(me.vars, l.vars); }
      $('lgLive').textContent = 'Live: on'; showLive();
    } catch { if (live !== me) return; me.missing = true; $('lgLive').textContent = 'Live: waiting for the server (/api/live)'; }
    if (live === me) me.timer = setTimeout(livePoll, me.missing ? 10000 : 500);
  }
  function showLive() {
    if (!live || !active()) return; const now = performance.now(), k = kindOf(owner()), key = ownerRef === 'stage' ? 'stage' : `${k}:${ownerRef}`, last = new Map();
    for (const [fk, t] of live.fired) if (fk.startsWith(key + '/')) { const base = fk.slice(key.length + 1).split(':')[0]; last.set(base, Math.max(t, last.get(base) || 0)); }
    board().querySelectorAll('.lgNode').forEach(el => { const t = last.get(el.dataset.id); el.classList.toggle('fired', !!t && now - t < 1500); });
    $('lgVars').textContent = Object.entries(live.vars).slice(0, 8).map(([k, v]) => `${k} ${v}`).join(' · ');
  }
  $('lgLive').onclick = () => live ? liveStop() : app.project() && liveStart(app.project().id);
  setInterval(showLive, 300);
  return {currentOwner: () => owner(), liveStart, liveStop, show, hide, active, render: () => render(), refresh: () => { if (active()) { const o = owner(); if (ownerRef && ownerRef !== 'stage' && !o) { ownerRef = null; } render(); } }, key, count,
    setOwner, ownerFromSelection: it => { if (!it) return; if (kindOf(it.o)) setOwner(it.o); else setOwner(it.o); }, reset: () => { ownerRef = null; views.clear(); sel.clear(); },
    compileAll: () => { for (const o of [...(P().triggers || []), ...P().objects]) if (o.logic) compile(o); }, table: () => TABLE,
    graph: (o, create = true) => graphOf(o, create), compile: o => compile(o), def: t => DEFS.get(t), kindOf, selectNodes: ids => { sel = new Set(ids); render(); }, myTemplates: () => myTemplates()};   // the inspector's Logic card (ui2.js rules) writes graphs through these
}
