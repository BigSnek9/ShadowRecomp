"""Phase 3 dependency compiler (#32 part 2): stage gadgets and decor from any stage in any stage.

A stage gadget class (registry block 'stage', server.class_registry) loads its files from the stage's own _gdt.one; a
Decoration / Destructable (ART WORK 2588 / ART GADGET 2589) picks its models by type id from the stage's ARTISTWORK.BIN /
ARTISTGADGET.BIN. To place one in another stage the build copies, from the SOURCE stage's _gdt.one into the TARGET's:
- gadgets: every file the class uses - names the game looked up (tools/stage_editor/capture/stgNNNN*.jsonl),
  the file names in the class's own table (e.g. STONEPOLE.BIN), the catalog models; then same-stem files of each
  model (X.DFF -> XBRK.DFF, X.CCL, X.BON, X_ANIM.MTN ...);
- decor: the type's rows of the artist table (type -> model groups -> models, collisions, animations) appended to the
  target's table under new names / ids, the placement's type parameter remapped;
- textures: every texture the copied DFFs name, from the source STG_GDT.TXD (or texCM) into the target STG_GDT.TXD.
Anything unresolved is an error (a missing model is a crash). Effects live in one per-stage effect database (.EFD) that
is not merged: a copied type's crash effect becomes a target effect of the same name, else the target's first one.
"""
import base64, json, re, struct
from pathlib import Path
from formats import one, children, first, serialize
from models import read_dff
from pof_table import Table

APP = Path(__file__).resolve().parent
FILE_RE = re.compile(r'[A-Za-z0-9_]+\.(DFF|CCL|UVA|BON|MTN|MTP|BIN|TXD)', re.I)
DECOR = {(0x25, 0x88): 'ARTISTWORK.BIN', (0x25, 0x89): 'ARTISTGADGET.BIN'}
SIZES = {0: 0x40, 1: 0x10, 2: 0x1C, 3: 0x18, 5: 0x48, 6: 0x1C}   # row size per list (list 4 is always empty)
ID_WORD = {0: 15, 1: 2, 2: 6, 3: 5}                                 # shared model / group / collision id
EXTS = ('.DFF', '.CCL', '.UVA', '.BON', '.MTN', '.MTP')
BUDGET = 32 * 1024 * 1024   # warning threshold for stages without a measurement (64 MB runtime; never refuses)


# ---- textures (RenderWare TXD: 0x16 dictionary = struct {u16 count, u16 device} + 0x15 native textures)
def txd_names(txd):
    out = {}
    for tag, body, ver in children(first(txd, 0x16)):
        if tag == 0x15: out[first(body, 1)[0x18:0x38].split(b'\0')[0].decode('ascii', 'replace').lower()] = (tag, body, ver)
    return out


def txd_add(txd, textures):
    """txd with the native texture chunks (tag, body, version) appended"""
    cs = children(txd); i = next(i for i, c in enumerate(cs) if c[0] == 0x16); tc = children(cs[i][1])
    si = next(j for j, c in enumerate(tc) if c[0] == 1); count, device = struct.unpack_from('<HH', tc[si][1])
    tc[si] = (1, struct.pack('<HH', count + len(textures), device) + tc[si][1][4:], tc[si][2])
    at = next((j for j, c in enumerate(tc) if c[0] == 3), len(tc)); tc[at:at] = list(textures)
    cs[i] = (0x16, serialize(tc), cs[i][2]); return serialize(cs)


# ---- artist tables
class ArtistTable:
    """ARTISTGADGET.BIN / ARTISTWORK.BIN as lists of rows; a row field is an int, ('s', text) or, for a model group's
    member list, ('a', [[('s', model name), int], ...])."""
    def __init__(self, raw):
        self.t = t = Table(raw); d = t.data
        pairs = [struct.unpack_from('>2I', d, 8 * k) for k in range(7)]
        self.lists = {}
        for k, (o, c) in enumerate(pairs):
            if c and k not in SIZES: raise ValueError(f'artist table list {k} not understood')
            rows = []
            for i in range(c):
                at = o + i * SIZES[k]; row = []
                for j in range(SIZES[k] // 4):
                    w = at + 4 * j; v = t.word(w)
                    if w not in t.pointers: row.append(v)
                    elif k == 1 and j == 1:
                        n = t.word(at + 12); row.append(('a', [[self._str(v + 8 * q), t.word(v + 8 * q + 4)] for q in range(n)]))
                    else: row.append(self._str(w))
                rows.append(row)
            self.lists[k] = rows

    def _str(self, ptr):
        """the name the pointer word at ptr points to"""
        if ptr not in self.t.pointers: raise ValueError('artist table: group member without a name')
        at = self.t.word(ptr); e = self.t.data.find(b'\0', at); s = self.t.data[at:e]
        if e <= at or not all(32 <= c < 127 for c in s): raise ValueError('artist table: pointer to something that is not a name')
        return ('s', s.decode())

    def names(self, k): return {r[0][1] for r in self.lists.get(k, []) if isinstance(r[0], tuple)}

    def serialize(self):
        data = bytearray(0x3C); pointers = set(); fix = []; arrays = []
        for k in (1, 0, 2, 3, 5, 6):
            rows = self.lists.get(k, [])
            if not rows: continue
            struct.pack_into('>2I', data, 8 * k, len(data), len(rows)); pointers.add(8 * k)
            for row in rows:
                for f in row:
                    if isinstance(f, tuple): (fix if f[0] == 's' else arrays).append((len(data), f[1])); data += b'\0' * 4
                    else: data += struct.pack('>I', f)
        for at, items in arrays:
            struct.pack_into('>I', data, at, len(data)); pointers.add(at)
            for name, extra in items: fix.append((len(data), name[1])); data += b'\0' * 4 + struct.pack('>I', extra)
        pool = {}
        for at, s in fix:
            if s not in pool: pool[s] = len(data); data += s.encode('ascii') + b'\0'
            struct.pack_into('>I', data, at, pool[s]); pointers.add(at)
        data += b'\0' * (-len(data) % 4)
        self.t.data = data; self.t.pointers = pointers
        return self.t.serialize()


def _strings(row):
    for f in row:
        if isinstance(f, tuple):
            if f[0] == 's': yield f[1]
            else:
                for name, _ in f[1]: yield name[1]


# Break effects (#32 leftover, investigated 2026-10-16): an artist table's crash-effect row names an effect id such as
# 0x7332 = Glyphic's effect bank 0x73, entry 0x32 (Westopolis uses 0x71xx; ids start at 0x30, the EFD header's word at
# 0x2C). The bank byte appears nowhere in STGNNNN_FX.EFD - the game assigns it per stage in code - and the EFD holds
# variable-length effect records (name, nested emitters, fx-texture names), so another stage's effect cannot be merged
# by the build. A copied type's crash effect is therefore replaced by a target effect and every replacement is listed
# in the build report ('effects'). Loading a second stage's fx bank is runtime research.
EFFECTS_LOG = []

def merge_types(target, source, type_ids, tag):
    """Append the rows source type ids need to target (an ArtistTable, or None when the target stage has no such
    table). Returns (table, {old type id: new type id}, file names, warnings)."""
    warnings = []
    if target is None:
        target = ArtistTable(source.t.serialize()); target.lists = {k: [] for k in range(7)}
    by_id = {}
    for row in source.lists.get(5, []): by_id[row[1]] = row          # the game takes the LAST row of an id
    missing = [i for i in type_ids if i not in by_id]
    if missing: raise ValueError(f'type {missing[0]} is not in the source table')
    copied = {k: [] for k in (0, 1, 2, 3)}; seen = set()
    pending = [s for i in type_ids for j, s in enumerate(_strings(by_id[i])) if j > 0]
    while pending:
        s = pending.pop()
        if s in seen: continue
        seen.add(s)
        for k in copied:
            for row in source.lists.get(k, []):
                if row[0] == ('s', s) and not any(r is row for r in copied[k]):
                    copied[k].append(row); pending += list(_strings(row))
    rename = {r[0][1]: f'{r[0][1]}_{tag}' for k in copied for r in copied[k]}
    rename.update({by_id[i][0][1]: f'{by_id[i][0][1]}_{tag}' for i in type_ids})
    # crash effects: the stage effect database (.EFD) is not merged - reuse a target effect
    effects = target.names(6); fallback = target.lists[6][0][0][1] if target.lists.get(6) else None
    src_effects = source.names(6); new_rows = []
    for i in type_ids:
        for f in by_id[i]:
            if isinstance(f, tuple) and f[0] == 's' and f[1] in src_effects and f[1] not in rename:
                if f[1] in effects: continue
                srow = next((r for r in source.lists[6] if r[0] == f), None); eid = srow[2] if srow else None
                EFFECTS_LOG.append(dict(effect=f[1], sourceId=f'{eid:#06x}' if isinstance(eid, int) else None, usedInstead=fallback))
                if fallback: rename[f[1]] = fallback; warnings.append(f"break effect '{f[1]}' is not in this stage; uses '{fallback}' (effects are not copied)")
                else:
                    row = next(r for r in source.lists[6] if r[0] == f)
                    new_rows.append(list(row)); warnings.append(f"break effect '{f[1]}' copied by id only (this stage has no effect list; may show nothing)")
    def remap(row):
        out = []
        for f in row:
            if isinstance(f, tuple) and f[0] == 's': out.append(('s', rename.get(f[1], f[1])))
            elif isinstance(f, tuple): out.append(('a', [[('s', rename.get(n[1], n[1])), x] for n, x in f[1]]))
            else: out.append(f)
        return out
    used = {r[ID_WORD[k]] for k in ID_WORD for r in target.lists.get(k, [])}
    ids = {r[ID_WORD[k]] for k in copied for r in copied[k]}
    shift = next(s for s in range(0, 1 << 20, 100) if not {i + s for i in ids} & used)
    for k in copied:
        for r in copied[k]:
            r2 = remap(r); r2[ID_WORD[k]] += shift; target.lists.setdefault(k, []).append(r2)
    for r in new_rows: r[1] = len(target.lists.setdefault(6, [])); target.lists[6].append(r)
    next_id = max([r[1] for r in target.lists.get(5, [])] + [-1]) + 1; typemap = {}
    for i in type_ids:
        r2 = remap(by_id[i]); r2[1] = next_id; typemap[i] = next_id; next_id += 1; target.lists.setdefault(5, []).append(r2)
    files = sorted({s for k in copied for r in copied[k] for s in _strings(r) if FILE_RE.fullmatch(s)} |
                   {s for i in type_ids for s in _strings(by_id[i]) if FILE_RE.fullmatch(s)})
    return target, typemap, files, warnings


# ---- gadget classes
def capture_names(stage, cls):
    """{name} the game looked up from stage's gdt for class cls (list, type) in any capture run of that stage"""
    out = set(); key = f'{cls[0] << 8 | cls[1]:04X}'
    for f in sorted((APP / 'capture').glob(f'stg{stage:04d}*.jsonl')):
        for line in f.read_text('utf-8', 'replace').splitlines():
            try: j = json.loads(line)
            except ValueError: continue
            if j.get('ev') == 'lookup' and j.get('cls') == key and j.get('found') and (j.get('archive') or '').lower().endswith(f'stg{stage:04d}_gdt.one'):
                out.add(j['name'].upper())
    return out


def table_files(raw):
    """file names inside a POF class table (e.g. STONEPOLE.BIN)"""
    try: t = Table(raw)
    except ValueError: return set()
    out = set()
    for p in t.pointers:
        w = t.word(p)
        if w < len(t.data):
            s = t.string(w)
            if FILE_RE.fullmatch(s): out.add(s.upper())
    return out


def expand(names, available):
    """same-stem files: X.DFF / X.CCL -> every model / collision / animation file whose name starts with X"""
    stems = {n.rsplit('.', 1)[0] for n in names if n.endswith(('.DFF', '.CCL'))}
    return set(names) | {a for a in available if a.endswith(EXTS) and any(a.startswith(s) for s in stems)}


def gadget_files(stage, cls, info, catalog_models, src_gdt):
    """(files needed from the source gdt, sources used, names not found)"""
    avail = {n.upper() for n in src_gdt}; names = set(); sources = []; missing = []
    cap = capture_names(stage, cls)
    if cap: names |= cap; sources.append('capture')
    for f in (info or {}).get('files', []):
        f = f.upper()
        if f in avail:
            inner = table_files(src_gdt[next(n for n in src_gdt if n.upper() == f)])
            names.add(f); names |= {x for x in inner if x in avail}; missing += [x for x in inner if x not in avail]
            sources.append(f'class table {f}')
    cat = {m.upper() for group in catalog_models for m in group}
    if cat & avail: names |= cat & avail; sources.append('catalog')
    return expand(names, avail), sources, missing


# ---- the build step
def compile_imports(stage, src_dir_of, target_gdt_files, imports, registry, catalog, common_files, target_tex_names):
    """imports: [(object, source stage)] of foreign gadget / decor placements (objects are edited in place: decor
    type parameter remapped). Returns dict(files={name: bytes} for the target gdt, report=[...], warnings=[...],
    errors=[...], classes={(list, type)}, bytes=n)."""
    out = dict(files={}, report=[], warnings=[], errors=[], classes=set(), bytes=0, effects=[]); EFFECTS_LOG.clear()
    tgt = {n.upper(): n for n in target_gdt_files}
    sources = {}
    def src(s):
        if s not in sources:
            p = src_dir_of(s) / f'stg{s:04d}_gdt.one'
            files = one.read_all(p.read_bytes()) if p.exists() else {}
            tex = src_dir_of(s) / f'stg{s:04d}_texCM.one'
            sources[s] = (files, one.read_all(tex.read_bytes()) if tex.exists() else {})
        return sources[s]
    need = {}   # name -> (bytes, source stage, why)
    def take(name, s, why, obj):
        files = src(s)[0]; real = next((n for n in files if n.upper() == name.upper()), None)
        if real is None:
            out['errors'].append(f"{obj['name']}: needs {name} from stage {s}, which does not have it"); return
        data = files[real]
        if real.upper() in tgt and target_gdt_files[tgt[real.upper()]] != data:
            out['errors'].append(f"{obj['name']}: {real} from stage {s} clashes with this stage's own {real}"); return
        if real.upper() in tgt: return
        prev = need.get(real)
        if prev and prev[0] != data: out['errors'].append(f"{obj['name']}: two source stages have different {real}"); return
        need[real] = (data, s, why)
    # decor first: group per (source stage, table) so each table is merged once
    decor = {}
    for o, s in imports:
        if (o['list'], o['type']) in DECOR:
            prm = base64.b64decode(o.get('params') or '')
            if len(prm) < 4: out['errors'].append(f"{o['name']}: no type parameter"); continue
            decor.setdefault((s, DECOR[(o['list'], o['type'])]), []).append((o, struct.unpack_from('<i', prm, 0)[0]))
    tables = {}
    for (s, tname), objs in decor.items():
        files = src(s)[0]
        if tname not in files: out['errors'].append(f"{objs[0][0]['name']}: stage {s} has no {tname}"); continue
        try:
            source = ArtistTable(files[tname])
            target = tables.get(tname) or (ArtistTable(target_gdt_files[tgt[tname]]) if tname in tgt else None)
            types = sorted({t for _, t in objs})
            target, typemap, names, warns = merge_types(target, source, types, f's{s}')
        except (ValueError, StopIteration, struct.error) as e:
            out['errors'].append(f"{objs[0][0]['name']}: {tname} of stage {s}: {e}"); continue
        tables[tname] = target; out['warnings'] += [f'{tname} from stage {s}: {w}' for w in warns]
        names = expand({n.upper() for n in names}, {n.upper() for n in files})
        for o, t in objs:
            prm = bytearray(base64.b64decode(o['params'])); struct.pack_into('<i', prm, 0, typemap[t]); o['params'] = base64.b64encode(bytes(prm)).decode()
            out['classes'].add((o['list'], o['type']))
        for n in sorted(names): take(n, s, f'{tname} types {types}', objs[0][0])
        out['report'].append(dict(kind='decor', table=tname, sourceStage=s, types={str(k): v for k, v in typemap.items()},
          objects=len(objs), files=sorted(names), source='artist table rows'))
    for tname, table in tables.items(): out['files'][tname] = table.serialize()
    out['effects'] = list(EFFECTS_LOG)
    # gadget classes
    done = {}
    for o, s in imports:
        cls = (o['list'], o['type'])
        if cls in DECOR: continue
        if (cls, s) not in done:
            files = src(s)[0]
            if not files: out['errors'].append(f"{o['name']}: stage {s} has no _gdt.one"); continue
            cat = next((c.get('models', []) for c in catalog if (c['list'], c['type']) == cls), [])
            names, used, missing = gadget_files(s, cls, registry.get(cls), cat, files)
            missing = [m for m in missing if m not in common_files]
            if missing: out['errors'].append(f"{o['name']}: {', '.join(sorted(missing))} named by its class table but not in stage {s}")
            if not any(n.endswith('.DFF') for n in names): out['errors'].append(f"{o['name']}: no model found for class {cls[0]:02X}/{cls[1]:02X} in stage {s} (no capture, table or catalog entry)")
            done[(cls, s)] = names
            out['report'].append(dict(kind='gadget', className=(registry.get(cls) or {}).get('name'), list=cls[0], type=cls[1], sourceStage=s,
              files=sorted(names), source=' + '.join(used) or 'none'))
            for n in sorted(names): take(n, s, 'class files', o)
        out['classes'].add(cls)
    # textures of every copied model
    tex_files = {}; tgt_txd_name = next((n for n in target_gdt_files if n.upper() == 'STG_GDT.TXD'), None)
    have = set(target_tex_names) | (set(txd_names(target_gdt_files[tgt_txd_name])) if tgt_txd_name else set())
    for name, (data, s, why) in list(need.items()):
        if not name.upper().endswith('.DFF'): continue
        try: mats = read_dff(data)['materials']
        except (ValueError, IndexError, struct.error, TypeError, StopIteration) as e:
            out['warnings'].append(f'{name}: textures not read ({e})'); continue
        # base textures and the MatFX dual / env-map pass textures (missing ones render white in game)
        for t in [n.lower() for m in mats for n in [m['texture']] + m.get('fxTextures', [])]:
            if not t or t in have or t in tex_files: continue
            gfiles, tfiles = src(s); found = None
            for n, b in list(gfiles.items()) + list(tfiles.items()):
                if n.upper().endswith('.TXD'):
                    found = txd_names(b).get(t)
                    if found: break
            if found: tex_files[t] = found
            else: out['warnings'].append(f'{name}: texture {t} is missing in stage {s} too (the original also lacks it)')
    if tex_files:
        if not tgt_txd_name: out['errors'].append('this stage has no STG_GDT.TXD to receive copied textures')
        else: out['files'][tgt_txd_name] = txd_add(target_gdt_files[tgt_txd_name], list(tex_files.values()))
    for name, (data, s, why) in need.items(): out['files'][name] = data
    out['bytes'] = sum(len(d) for d, _, _ in need.values()) + sum(len(b) + 12 for _, b, _ in tex_files.values())
    out['textures'] = sorted(tex_files)
    # memory: WARN only (user decision 2026-10-16: GameCube-era limits go; ticket #60 raises the heap in the runtime).
    # Compared with measured free heap at "player active" (capture/memory.json; start of stage only).
    # memory: WARN only (user decision 2026-10-16; #60 gave the game 64 MB). Free heap at the start of the stage on the
    # 64 MB runtime = measured (capture/memory.json runtime_64mb) or 64 MB heap - what the stage used at 24 MB; warn
    # above 85 % of it (roadmap section 5; later activations need the rest)
    mem = stage_memory(stage); out['memory'] = mem
    limit = 0.85 * mem['free64'] if mem else BUDGET
    if out['bytes'] > limit:
        what = (f"85 % of the {mem['free64'] / 1048576:.1f} MiB free at the start of stage {stage} on the 64 MB runtime ({mem['source']})" if mem
                else f"{BUDGET / 1048576:g} MiB (no measurement for stage {stage})")
        out['warnings'].append(f"memory: copied files total {out['bytes'] / 1048576:.2f} MiB, more than {what} - not refused")
    return out


def stage_memory(stage):
    """{free64, source} - free heap at the start of the stage on the 64 MB runtime (capture/memory.json), or None"""
    try: d = json.loads((APP / 'capture/memory.json').read_text('utf-8'))
    except (OSError, ValueError): return None
    r = d.get('runtime_64mb') or {}; heap64 = 60661600
    m = __import__('re').search(r'= ([0-9,]+) bytes', r.get('note', ''))
    if m: heap64 = int(m[1].replace(',', ''))
    if f'{stage}_free_at_start' in r: return dict(free64=r[f'{stage}_free_at_start'], source='measured')
    st = d.get('stages', {}).get(str(stage))
    if st and 'heap_size' in st and 'free' in st: return dict(free64=heap64 - (st['heap_size'] - st['free']), source='estimated from the 24 MB measurement')
    return None
