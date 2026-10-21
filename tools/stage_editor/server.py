"""Local-only external Stage Workshop. Run with Python; all dependencies are bundled locally."""
from __future__ import annotations
import argparse, base64, copy, hashlib, json, math, mimetypes, os, re, secrets, shutil, struct, subprocess, sys, threading, time, uuid, webbrowser
import io
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
from pathlib import Path
from urllib.parse import urlparse,parse_qs,unquote
import numpy as np
from PIL import Image
from formats import read_set,write_set,read_world,children,first,materials,one,combine,append_visual,make_collision,empty_world,b64,unb64,transform_world
from events import compile_events
import depcompiler
from texture_build import material_bytes,append_textures
from paths import read_paths,write_paths
from models import read_dff,bind_uv_anims

APP=Path(__file__).resolve().parent; ROOT=APP.parents[1]
# Stage Forge for players (tools/stage_forge_app): the packaged editor sets STAGE_FORGE_DATA (its writable folder: projects,
# caches) and STAGE_FORGE_GAME (the player's game folder: textures, settings, lighting, the exe the playtests copy).
# Unset (development) = the project layout: ROOT/stage_projects, ROOT/work, ROOT/play, ROOT/lighting.
DATA=Path(os.environ.get('STAGE_FORGE_DATA') or ROOT)
PLAY=Path(os.environ.get('STAGE_FORGE_GAME') or (ROOT/'play'/'game' if (ROOT/'play'/'game'/'shadow.exe').exists() else ROOT/'play'))   # #138: the game moved to play\game (Forge sweep: textures library empty, playtest runtime never refreshed)
LIGHT_ROOT=Path(os.environ.get('STAGE_FORGE_GAME') or ROOT)
# #157: per-stage generated lighting (tools/lighting/make_profiles.py). A player's game folder has none (promotions copy
# lighting\*.occ only), so the packaged Stage Forge brings its own copy (build_stage_forge.py) - the In-game look's stage look.
PROFILES=LIGHT_ROOT/'lighting/profiles' if (LIGHT_ROOT/'lighting/profiles').is_dir() else ROOT/'lighting/profiles'
DISC=Path(os.environ.get('SHADOW_DISC',str(ROOT.parent/'Shadow')))
PROJECTS=DATA/'stage_projects';PROJECTS.mkdir(parents=True,exist_ok=True)
TOKEN=os.environ.get('SHADOW_WORKSHOP_TOKEN') or secrets.token_urlsafe(24);CACHE={};ASSETS={};PROCESSES={};LOCK=threading.RLock()
CATALOG=json.loads((APP/'catalog.json').read_text('utf-8')) if (APP/'catalog.json').exists() else []
NAMES={(c['list'],c['type']):c for c in CATALOG}
STAGE_NAMES={100:'Westopolis',200:'Digital Circuit',201:'Glyphic Canyon',202:'Lethal Highway',300:'Cryptic Castle',301:'Prison Island',302:'Circus Park',400:'Central City',401:'The Doom',402:'Sky Troops',403:'Mad Matrix',404:'Death Ruins',500:'The ARK',501:'Air Fleet',502:'Iron Jungle',503:'Space Gadget',504:'Lost Impact',600:'GUN Fortress',601:'Black Comet',602:'Lava Shelter',603:'Cosmic Fall',604:'Final Haunt',700:'The Last Way',9900:'Test stage',
  # bosses / 2P / events (#99: friendly names everywhere; boss rows from docs/bosses/BOSS-SUPPORT.md)
  210:'Boss: Black Bull (Lethal Highway)',310:'Boss: Egg Breaker (Cryptic Castle)',410:'Boss: Heavy Dog',411:'Boss: Egg Breaker (Mad Matrix)',412:'Boss: Black Bull (Death Ruins)',
  510:'Boss: Blue Falcon',511:'Boss: Egg Breaker (Iron Jungle)',610:'Boss: Black Doom',611:'Boss: Sonic & Diablon',612:'Boss: Egg Dealer',613:'Boss: Sonic & Diablon (2)',
  614:'Boss: Egg Dealer (2)',615:'Boss: Egg Dealer (3)',616:'Boss: Black Doom (2)',617:'Boss: Black Doom (3)',618:'Boss: Sonic & Diablon (3)',710:'Boss: Devil Doom',
  800:'2P battle 1',801:'2P battle 2',802:'2P battle 3',0:'Practice',1:'Practice 1',2:'Practice 2',3:'Practice 3',5:'Event stage',
  **{n:f'Event stage {n}' for n in range(901,912)}}

def stage_dir(stage):
    if not isinstance(stage,int) or not 0<=stage<=9999:raise ValueError('Invalid stage')
    p=DISC/'files'/f'stg{stage:04d}'
    if not p.is_dir():raise ValueError('Stage does not exist in the extracted disc')
    return p
def project_dir(pid):
    if not re.fullmatch('[a-f0-9]{32}',pid):raise ValueError('Invalid project ID')
    return PROJECTS/pid
def atomic(path,data):
    path.parent.mkdir(parents=True,exist_ok=True);tmp=path.with_suffix(path.suffix+'.tmp')
    tmp.write_bytes(data);os.replace(tmp,path)
def json_bytes(x):return json.dumps(x,ensure_ascii=False,allow_nan=False,indent=2).encode()
def asset(path):
    aid=hashlib.sha256(str(path).encode()).hexdigest()[:24];ASSETS[aid]=path;return aid

# #80 (2026-10-17): GET requests run on parallel threads without LOCK. UI 2 asks /api/stage, /api/models and
# /api/object-library for one stage at once; two threads building the library at the same time left CACHE['texture_names']
# = the second dict while disc_textures had filled the first and marked the stage done -> 383 of Final Haunt's 423
# materials without a texture for the server's whole life (reproduced with 3 parallel requests). Both now hold TEX_LOCK.
TEX_LOCK=threading.RLock()
# #80 REAL cause (2026-10-17): the user's editor process could not import xxhash (installed in the USER site-packages;
# reproduced with pythonw -s: exactly 383 of Final Haunt's 423 materials without a texture). tools/texture_dump.py
# imports xxhash at the top, the import failed, disc_textures returned silently and every disc-decoded texture was
# missing. Now: the user site is added explicitly; if xxhash is still missing, tools/stage_editor/xxh64_py.py (pure
# Python, checked equal to xxhash 4.0.1) stands in, so the decoders AND the #44 replacement keys work. WHY it was
# missing: xxhash had been pip-installed --user from inside the Claude desktop app, an MSIX package whose AppData\Roaming
# writes go to its private copy (AppData\Local\Packages\Claude_pzs8sxrjxfjjc\LocalCache\Roaming\Python\...); the
# Explorer-launched editor sees the real Roaming folder, which has no xxhash. Every outcome is logged to
# work/stage-editor-cache/server.log and reported by GET /api/version (textureDecoder, discTextures).
TEX_STATUS={'decoder':None,'stages':{}}
def server_log(line):
    try:
        TEX_CACHE.parent.mkdir(parents=True,exist_ok=True)
        with open(TEX_CACHE.parent/'server.log','a',encoding='utf-8') as f:f.write(time.strftime('%Y-%m-%d %H:%M:%S ')+f'pid {os.getpid()} '+line+'\n')
    except OSError:pass
def texture_decoder():
    """tools/texture_dump as a module, importable without the user site / xxhash (see above)"""
    if 'texture_dump' in sys.modules:return sys.modules['texture_dump']
    import site,types
    try:
        us=site.getusersitepackages()
        if us and os.path.isdir(us) and us not in sys.path:sys.path.append(us)
    except Exception:pass
    if str(ROOT/'tools') not in sys.path:sys.path.insert(0,str(ROOT/'tools'))
    note='ok'
    try:import xxhash
    except ImportError as e:   # the C package is missing for this process: identical keys from the pure-Python XXH64
        import xxh64_py;stub=types.ModuleType('xxhash');stub.xxh64=xxh64_py.xxh64;sys.modules['xxhash']=stub
        note=f'pure-Python xxh64 (C xxhash not importable: {e}); texture keys identical, slower'
    import texture_dump
    TEX_STATUS['decoder']=note;server_log(f'texture decoder: {note} (python {sys.executable}, user site {site.ENABLE_USER_SITE})')
    return texture_dump
def texture_library():
    with TEX_LOCK:return _texture_library()
def _texture_library():
    if 'textures' in CACHE:return CACHE['textures']
    textures=[];by_name={}
    base=PLAY/'textures'
    # Matching filenames preserve the original TXD material names. Custom art wins when present.
    for kind in ['Original','Custom']:
        for p in (base/kind).rglob('*.png'):
            name=p.name.split(' [')[0];entry=dict(id=asset(p),name=name,path=str(p.relative_to(base)),pack=kind)
            textures.append(entry);by_name[name.lower()]=entry
    CACHE['textures']=textures;CACHE['texture_names']=by_name;return textures

TEX_CACHE=DATA/'work/stage-editor-cache/textures'
def disc_textures(stage):
    """Textures the dumped library (play/textures) lacks, decoded straight from the disc for display only: every
    RenderWare texture native in the stage's archives and the shared object/character/enemy archives
    (tools/texture_dump.py decoders). PNGs cached in work/stage-editor-cache/textures; added to the name index."""
    with TEX_LOCK:return _disc_textures(stage)
def _disc_textures(stage):
    key=f'disc-tex-{stage}'
    if key in CACHE:return
    texture_library();by_name=CACHE['texture_names'];TEX_CACHE.mkdir(parents=True,exist_ok=True)
    sys.path.insert(0,str(ROOT/'tools'))
    try:td=texture_decoder()
    except Exception as e:   # #62: do not mark the stage done - the next call retries (a failed import used to stick)
        TEX_STATUS['decoder']=f'FAILED: {e}';server_log(f'disc textures for stage {stage} unavailable: {e!r}')
        if sys.stdout:print('disc textures unavailable:',e,flush=True)
        return
    src=stage_dir(stage)
    archives=sorted(src.glob('*.one'))+[DISC/'files/stg_cmn_gdt.one',DISC/'files/character/shadow.one']+sorted((DISC/'files/common').glob('*.one'))+sorted((DISC/'files/enemy').glob('*.one'))
    added=0;failed=[]
    for archive in archives:
        if not archive.exists():continue
        try:files=one.read_all(archive.read_bytes())
        except Exception as e:failed.append(f'{archive.name}: {e!r}');continue
        for name,data in files.items():
            if b'\x15\x00\x00\x00' not in data:continue
            try:natives=list(td.natives_in(data))
            except Exception as e:failed.append(f'{archive.name}/{name}: {e!r}');continue
            for tname,fmt,w,h,raw,pal,tlfmt in natives:
                if tname.lower() in by_name:continue
                path=TEX_CACHE/(re.sub(r'[^A-Za-z0-9_.-]','_',tname)+'.png')
                try:
                    if not path.exists():Image.fromarray(td.decode(fmt,w,h,raw,pal,tlfmt),'RGBA').save(path)
                except Exception as e:failed.append(f'{tname}: {e!r}');continue
                by_name[tname.lower()]=dict(id=asset(path),name=tname,path=str(path),pack='Disc');added+=1
    frame_standins(by_name)
    CACHE[key]=added;TEX_STATUS['stages'][stage]=dict(added=added,failed=len(failed),firstFailures=failed[:5])
    server_log(f'disc textures stage {stage}: {added} added, {len(failed)} failed'+(f' (first: {failed[0]})' if failed else ''))

def frame_standins(by_name):
    """#51: textures named NAME.1 .. NAME.N are the frames of a texture animation; some first frames are empty on the
    disc (Sky Troops s0402_zn096.1 / zn097.1: every pixel = palette entry 0 = transparent). The editor shows the first
    non-empty frame of the sequence instead (entry gets standinFor = the frame it replaces); the game data is unchanged."""
    groups={}
    for k in list(by_name):
        m=re.fullmatch(r'(.+)\.(\d+)',k)
        if m:groups.setdefault(m[1],[]).append((int(m[2]),k))
    def blank(e):
        p=ASSETS.get(e.get('id'))
        if not p:return False
        ck=f'blank-{p}'
        if ck not in CACHE:
            try:
                im=Image.open(p);CACHE[ck]=im.mode in ('RGBA','LA') and im.getextrema()[-1][1]==0
            except OSError:CACHE[ck]=False
        return CACHE[ck]
    for base,items in groups.items():
        items.sort();good=next((k for n,k in items if not by_name[k].get('standinFor') and not blank(by_name[k])),None)
        if not good:continue
        for n,k in items:
            if not by_name[k].get('standinFor') and blank(by_name[k]):by_name[k]=dict(by_name[good],name=by_name[k]['name'],standinFor=good)

def native_label(o):return NAMES.get((o['list'],o['type']),{}).get('name',f"Object {o['list']:02X}{o['type']:02X}")

def display_label(name):
    known={'GUNSoldier':'G.U.N. Soldier','GUNBeetle':'G.U.N. Beetle','Bk Soldier':'Black Arms Soldier','Bk Wing Small':'Black Arms Flying Soldier','Destructable1':'Destructible Object','Light Colli':'Lighting Volume'}
    return known.get(name,re.sub(r'(?<=[a-z])(?=[A-Z])',' ',name))

def object_library(stage):
    src=stage_dir(stage);rows=[];seen=set();texture_library()
    for layer in ['cmn','nrm','hrd','ds1']:
        path=src/f'stg{stage:04d}_{layer}.dat'
        if not path.exists():continue
        for o in read_set(path.read_bytes(),layer)[0]:
            key=(o['list'],o['type'])
            if key in seen:continue
            seen.add(key);internal=native_label(o);display=display_label(internal)
            words=[w.lower() for w in re.findall('[A-Za-z]{4,}',display)]
            previews=[t for t in CACHE['textures'] if words and any(w in t['name'].lower() for w in words)]
            o.update(nativeName=internal,name=display,sourceStage=stage)
            rows.append(dict(internalName=internal,displayName=display,list=o['list'],type=o['type'],sourceStage=stage,template=o,previews=previews[:4]))
    return dict(stage=stage,objects=rows)

def artist_table(b):
    """ARTISTGADGET.BIN / ARTISTWORK.BIN (stage *_gdt.one): POF file, 32-byte header, then {table offset, count, entry
    size field, ...}, entries 0x40 bytes; entry i = the models of type i (Destructable1 DestructableType / Decoration1 DecorationType), its
    u32 fields pointing at strings: a name, the DFF(s), a .UVA (UV animation) and collision (.CCL)."""
    d=b[32:];out=[]
    if len(d)<12:return out
    # entries are 0x40 bytes (16 words): off + count*0x40 == the end offset at header word 6 (checked on Glyphic
    # Canyon: 0x15C + 36*0x40 = 0xA5C). The 0x3C in word 2 is something else (reading 0x3C strides drifts by a word).
    off,cnt,size=struct.unpack_from('>3I',d,0);size=0x40
    if cnt>512 or off+cnt*size>len(d):return out
    def text(x):
        if not 0<x<len(d):return None
        e=d.find(b'\0',x);t=d[x:e].decode('ascii','replace') if e>x else ''
        return t if re.fullmatch(r'[A-Za-z0-9_]{3,}(\.[A-Za-z]{3})?',t) else None
    for i in range(cnt):
        words=struct.unpack_from('>16I',d,off+i*size);strs=[t for t in map(text,words) if t]
        out.append(dict(name=next((t for t in strs if '.' not in t),f'type {i}'),models=[t for t in strs if t.upper().endswith('.DFF')],
          uva=next((t for t in strs if t.upper().endswith('.UVA')),None)))
    return out

def project_list():
    """Saved projects, newest first: id, name, stage, saved (unix time)."""
    out=[]
    for f in PROJECTS.glob('*/project.json'):
        try:j=json.loads(f.read_text('utf-8'))
        except (OSError,ValueError):continue
        out.append(dict(id=f.parent.name,name=j.get('name','Project'),stage=j.get('stage'),saved=f.stat().st_mtime))
    return sorted(out,key=lambda x:-x['saved'])
PARTNER_FILES={1:'sonic',2:'tails',3:'knuckles',4:'amy',5:'rouge',6:'omega',7:'vector',8:'espio',9:'maria',12:'doomseye'}
def stage_models(stage):
    key=f'models-{stage}'
    if key in CACHE:return CACHE[key]
    src=stage_dir(stage);texture_library();disc_textures(stage);tn=CACHE['texture_names'];out=[];warnings=[]
    archives=[src/f'stg{stage:04d}_gdt.one',DISC/'files/stg_cmn_gdt.one',DISC/'files/character/shadow.one',DISC/'files/common/WeaponResource.one',DISC/'files/common/vehicleResource.one']
    archives+=sorted((DISC/'files/enemy').glob('*.one'))
    # Partner (0190) characters: character/<name>.one hold the body DFF + hands (Knuckles, Sonic, Tails ...)
    archives+=[DISC/'files/character'/f'{n}.one' for n in PARTNER_FILES.values() if (DISC/'files/character'/f'{n}.one').exists()]
    for archive in archives:
        if not archive.exists():continue
        files=one.read_all(archive.read_bytes());start=len(out)
        for name,data in files.items():
            if not name.upper().endswith('.DFF'):continue
            try:
                model=read_dff(data)
                for mat in model['materials']:mat['asset']=tn.get(mat['texture'].lower(),{}).get('id')
                mid=hashlib.sha256((str(archive)+'/'+name).encode()).hexdigest()[:24]
                out.append(dict(id=mid,name=name,archive=str(archive.relative_to(DISC/'files')),**model))
            except (ValueError,IndexError,struct.error,TypeError,StopIteration) as e:warnings.append(name+': '+str(e))
        bind_uv_anims(out[start:],files)   # #33: materials[].uvAnim from the archive's .UVA files
    artist={}
    gdt=src/f'stg{stage:04d}_gdt.one'
    if gdt.exists():
        files=one.read_all(gdt.read_bytes())
        for k,n in (('gadget','ARTISTGADGET.BIN'),('work','ARTISTWORK.BIN')):
            if n in files:artist[k]=artist_table(files[n])
    rec,cls=object_models(stage,{n.upper() for n in (one.read_all(gdt.read_bytes()) if gdt.exists() else {})})
    CACHE[key]=dict(models=out,warnings=warnings,artist=artist,recordModels=rec,classModels=cls);return CACHE[key]

# ---- which models an object uses (roadmap phase 1 deliverable 4, first half). Sources, best first:
# capture = what the game itself drew / looked up for that SET record (tools/stage_editor/capture)
#   artist   = Destructable / Decoration type -> the stage's ARTISTGADGET / ARTISTWORK rows (type -> groups -> models)
#   catalog  = catalog.json models per variant (ShadowSET research; guesses across stages)
LAYER_NAMES=['cmn','nrm','hrd','ds1']
def capture_records(stage):
    """{(layer name, record index): dict(cls, created, models [drawn first], how, draws [...])} from every
    capture/stgNNNN*.jsonl of the stage. draws keep the world matrix (ltm, clump_ltm) and the record transform
    (rec_pos, rec_rot) - the hook for step 2 (per-piece offsets: inverse(record matrix) * ltm)."""
    key=f'capture-records-{stage}'
    if key in CACHE:return CACHE[key]
    out={}
    for f in sorted((APP/'capture').glob(f'stg{stage:04d}*.jsonl')):
        for line in f.read_text('utf-8','replace').splitlines():
            try:j=json.loads(line)
            except ValueError:continue
            if j.get('layer') is None or j.get('index') is None or not 0<=j['layer']<4:continue
            r=out.setdefault((LAYER_NAMES[j['layer']],j['index']),dict(cls=j.get('cls'),created=False,drawn=[],instances=[],looked=[],draws=[],inst=[],seen=set()))
            ev=j.get('ev')
            if ev=='create':r['created']=True
            elif ev=='draw' and j.get('model'):
                if j['model'] not in r['drawn']:r['drawn'].append(j['model'])
                k=(j['model'],j.get('atomic'))   # one entry per atomic (a stack of identical pieces = several atomics)
                if k not in r['seen'] and len(r['draws'])<128:
                    r['seen'].add(k);r['draws'].append({k2:j.get(k2) for k2 in ('model','atomic','during','match_dist','ltm','clump_ltm','rec_pos','rec_rot','textures')})
            elif ev=='model' and j.get('model'):
                if j['model'] not in r['instances']:r['instances'].append(j['model'])
                # instance scan (end of the run): objects created but never drawn; 'shared' atomics belong to no single record
                if not j.get('shared') and len(r['inst'])<128:
                    r['inst'].append(dict(model=j['model'],atomic=j.get('atomic'),during='instance',ltm=j.get('best'),clump_ltm=None,
                      rec_pos=j.get('rec_pos'),rec_rot=j.get('rec_rot'),textures=j.get('textures'),drawn=j.get('drawn')))
            elif ev=='lookup' and j.get('found') and str(j.get('name','')).upper().endswith('.DFF') and j['name'] not in r['looked']:r['looked'].append(j['name'])
    for r in out.values():
        # instance-scan pieces of models that never drew (or atomics no draw line covered) join the draw list
        r['draws']+=[d for d in r.pop('inst') if (d['model'],d['atomic']) not in r['seen']][:max(0,160-len(r['draws']))];r.pop('seen')
        r['models'],r['how']=next(((m,h) for m,h in ((r['drawn'],'draw'),(r['instances'],'instance'),(r['looked'],'lookup')) if m),([],None))
        for d in r['draws']:
            if d['model'] not in r['models']:r['models'].append(d['model'])
        if r['draws']:r['matchDist']=min((d.get('match_dist') or 0) for d in r['draws'])
    CACHE[key]=out;return out
def artist_type_models(raw):
    """{type id: [DFF names]} of an ARTISTGADGET / ARTISTWORK table (the game takes the LAST row of a type id)"""
    t=depcompiler.ArtistTable(raw);s=lambda f:f[1] if isinstance(f,tuple) and f[0]=='s' else None
    models={s(r[0]):s(r[1]) for r in t.lists.get(0,[]) if s(r[0])}
    groups={s(r[0]):[s(n) for n,_ in r[1][1]] for r in t.lists.get(1,[]) if s(r[0]) and isinstance(r[1],tuple) and r[1][0]=='a'}
    out={}
    for r in t.lists.get(5,[]):
        names=[s(f) for f in r[2:6] if s(f)];dffs=[]
        for n in names:
            for m in groups.get(n,[n]):
                if models.get(m) and models[m] not in dffs:dffs.append(models[m])
        out[r[1]]=dffs
    return out
def object_models(stage,gdt_names):
    """(recordModels, classModels): recordModels {'<layer>-<index>' (= the editor's native object id): {models,
    source 'capture', how, draws, matchDist}}; classModels {'LLTT': {'<variant>': {models, source, records}}} with
    variant = the record's first int parameter (DestructableType, DecorationType, Partner, ObjectType ...), sources
    capture > artist > catalog."""
    caps=capture_records(stage);imported=import_stage(stage,False)
    params={(o['layer'],o['sourceIndex']):o for o in imported['objects'] if o.get('kind')=='native'}
    rec={f'{l}-{i}':dict(models=r['models'],source='capture',how=r['how'],draws=r['draws'],matchDist=r.get('matchDist'),looked=r.get('looked') or []) for (l,i),r in caps.items() if r['models']}
    cls={}
    def put(c,v,models,source,n=0):
        e=cls.setdefault(f'{c[0]:02X}{c[1]:02X}',{});k=str(v)
        if models and (k not in e or ['catalog','artist','capture'].index(source)>['catalog','artist','capture'].index(e[k]['source'])):e[k]=dict(models=models,source=source,records=n)
        elif k in e and e[k]['source']==source:e[k]['records']+=n
    for c in CATALOG:
        for v,group in enumerate(c.get('models') or []):
            ms=[m for m in group if m.upper() in gdt_names] or [m for m in group if m and m!='None'][:1]
            put((c['list'],c['type']),v,ms,'catalog')
    gdt=stage_dir(stage)/f'stg{stage:04d}_gdt.one'
    if gdt.exists():
        files=one.read_all(gdt.read_bytes())
        for c,name in ((0x2588,'ARTISTWORK.BIN'),(0x2589,'ARTISTGADGET.BIN')):
            if name in files:
                try:
                    for v,ms in artist_type_models(files[name]).items():put((c>>8,c&0xFF),v,ms,'artist')
                except (ValueError,struct.error):pass
    for (l,i),r in caps.items():
        o=params.get((l,i))
        if not o or not r['models']:continue
        prm=base64.b64decode(o.get('params') or '');v=struct.unpack_from('<i',prm,0)[0] if len(prm)>=4 else None
        put((o['list'],o['type']),v,r['models'],'capture',1)
    return rec,cls

def archive_models(rel):
    """#54: the models of ONE archive under the disc's files/ (e.g. enemy/BkWormData.one), same shape as
    stage_models (models with materials[].asset), textures from the dumped library, else decoded from this archive's own
    TXD (display only, cached PNG). Cached per archive."""
    base=(DISC/'files').resolve();path=(base/str(rel).replace('\\','/')).resolve()
    if not path.is_relative_to(base) or path.suffix.lower()!='.one' or not path.is_file():raise ValueError('Unknown archive (a .one under files/)')
    rel=path.relative_to(base).as_posix();key=f'models-archive-{rel}'
    if key in CACHE:return CACHE[key]
    texture_library();by_name=CACHE['texture_names'];files=one.read_all(path.read_bytes());own={}
    sys.path.insert(0,str(ROOT/'tools'))
    try:
        td=texture_decoder();TEX_CACHE.mkdir(parents=True,exist_ok=True)
        for name,data in files.items():
            if b'\x15\x00\x00\x00' not in data:continue
            for tname,fmt,w,h,raw,pal,tlfmt in td.natives_in(data):
                p=TEX_CACHE/(re.sub(r'[^A-Za-z0-9_.-]','_',tname)+'.png')
                try:
                    if not p.exists():Image.fromarray(td.decode(fmt,w,h,raw,pal,tlfmt),'RGBA').save(p)
                    own[tname.lower()]=asset(p)
                except Exception:continue
    except ImportError:pass
    out=[];warnings=[]
    for name,data in files.items():
        if not name.upper().endswith('.DFF'):continue
        try:
            model=read_dff(data)
            for mat in model['materials']:
                t=mat['texture'].lower();mat['asset']=by_name.get(t,{}).get('id') or own.get(t)
            out.append(dict(id=hashlib.sha256((str(path)+'/'+name).encode()).hexdigest()[:24],name=name,archive=rel,**model))
        except (ValueError,IndexError,struct.error,TypeError,StopIteration) as e:warnings.append(name+': '+str(e))
    bind_uv_anims(out,files)
    CACHE[key]=dict(archive=rel,models=out,warnings=warnings);return CACHE[key]
def class_models(cid):
    """#54: models of a class with its own loader: cid = 'LLTT' hex (list << 8 | type); the archive(s) its load() names
    (server.class_registry), e.g. 0090 Bk Worm -> enemy/BkWormData.one; GUN soldiers' per-stage skins
    GunSoldierData_%s.one -> every matching archive."""
    v=int(cid,16);info=class_registry().get((v>>8,v&0xFF))
    if not info:raise ValueError('Unknown class id')
    names=[]
    for f in info['files']:
        if not f.lower().endswith('.one'):continue
        names+=[p.relative_to(DISC/'files').as_posix() for p in sorted((DISC/'files').glob(f.replace('%s','*')))] if '%s' in f else [f]
    archives=[archive_models(n) for n in names if (DISC/'files'/n).is_file()]
    return dict(classId=f'{v:04X}',className=info['name'],archives=[a['archive'] for a in archives],
      models=[m for a in archives for m in a['models']],warnings=[w for a in archives for w in a['warnings']])

def import_geometry_api(data):
    """#119 POST /api/import-geometry {project, files: [{name, data (base64)}], options} -> materials, geometry, chunkBoxes,
    warnings, stats (tools/stage_editor/import_geometry.py; the editor adds the objects and the chunk boxes)"""
    import import_geometry as ig
    pid=data['project'];project_dir(pid)
    files=[(str(f['name']),base64.b64decode(f['data'],validate=True)) for f in data.get('files',[])]
    if not files:raise ValueError('Nothing to import')
    if sum(len(b) for _,b in files)>400*1024*1024:raise ValueError('Import exceeds 400 MB')
    up=lambda name,raw,kind:upload_asset(dict(project=pid,name=name,kind=kind,data=base64.b64encode(raw).decode()))
    return ig.import_geometry(pid,files,data.get('options') or {},up)
# ---- #128 tilesets (the kits come from tools/tilesets/make_batch1.py -> work/tilesets/<kit>/kit.json + <piece>.glb;
# Shadow's Tilesets tab lists and places them). Grid 50 units, pivot = the footprint's min corner, 90 degree turns.
TILESETS=Path(os.environ.get('STAGE_FORGE_TILESETS') or DATA/'work'/'tilesets')   # packaged Stage Forge: StageForge/tilesets (stage_forge.py), read-only next to the player's data
def tilesets():
    """GET /api/tilesets -> {kits: [kit.json + url per piece]}"""
    out=[]
    for kj in sorted(TILESETS.glob('*/kit.json')):
        k=json.loads(kj.read_text(encoding='utf-8'))
        for pc in k.get('pieces',[]):
            pc['url']=f"/api/tileset-file?kit={k['kit']}&file={pc['file']}"
            th=pc.get('thumb') or pc['id']+'.png'   # 256 px render (tools/tilesets/render_thumbs.py)
            if (kj.parent/th).is_file():pc['thumbUrl']=f"/api/tileset-file?kit={k['kit']}&file={th}"
        out.append(k)
    return dict(kits=out,folder=str(TILESETS))
def tileset_file(kit,name):
    f=(TILESETS/kit/name).resolve()
    if TILESETS.resolve() not in f.parents or not f.is_file():raise ValueError('No such tileset file')
    return f.read_bytes()
def tileset_piece(data):
    """POST /api/tileset-piece {project, kit, piece, position [x, y, z] (where the pivot goes), yaw (90 degree steps)}
    -> import_geometry's answer (materials, geometry rows with collision, chunkBoxes) for the editor to add as one group"""
    import import_geometry as ig
    pid=data['project'];project_dir(pid)
    kit=json.loads((TILESETS/str(data['kit'])/'kit.json').read_text(encoding='utf-8'))
    pc=next((x for x in kit['pieces'] if x['id']==data.get('piece')),None)
    if pc is None:raise ValueError(f"No piece {data.get('piece')} in kit {data['kit']}")
    up=lambda name,raw,kind:upload_asset(dict(project=pid,name=name,kind=kind,data=base64.b64encode(raw).decode()))
    out=ig.import_geometry(pid,[(pc['file'],tileset_file(kit['kit'],pc['file']))],dict(units='meters',up='y',position=data.get('position') or [0,0,0],
      yaw=float(data.get('yaw') or 0),chunks=data.get('chunks') or [1],name=pc['name'],defaultSurface=pc.get('surface',0x00020001),collision=pc.get('collision',True)),up)
    for g in out['geometry']:g['tileset']=dict(kit=kit['kit'],piece=pc['id'])
    # #128 batch 3: a piece's own paths (grind rails: kit.json 'paths', local metres a, b, c) placed like its mesh -
    # game = yaw-turned (10a, 10c, 10b) + position - returned as path rows for the project (the build makes them PATH.PTP splines)
    t=np.radians(float(data.get('yaw') or 0));R=np.array([[np.cos(t),0,np.sin(t)],[0,1,0],[-np.sin(t),0,np.cos(t)]]).round(9)
    pos=np.asarray(data.get('position') or [0,0,0],float)
    out['paths']=[dict(id=uuid.uuid4().hex,kind='path',name=f"{pc['name']} ({q.get('name') or q['pathType']})",pathType=q['pathType'],
      position=[0,0,0],rotation=[0,0,0],scale=[1,1,1],closed=False,banking=0,autoOrient=True,tileset=dict(kit=kit['kit'],piece=pc['id']),
      points=[(R@np.array([a*10,c*10,b*10],float)+pos).round(2).tolist() for a,b,c in q['points']]) for q in pc.get('paths') or []]
    return out
def import_model(data):
    pid=data['project'];project_dir(pid);model=next((m for m in stage_models(int(data['stage']))['models'] if m['id']==data['model']),None)
    if model is None:raise ValueError('Unknown native model')
    used={i for mesh in model['meshes'] for i in mesh['materials']};converted={};mats=[];geometry=[];group=uuid.uuid4().hex
    chunk=int(data['chunk']) if data.get('chunk') is not None else default_chunk(import_stage(int(data['stage']))['native']['chunks'])
    for i in used:
        source=model['materials'][i];path=ASSETS.get(source.get('asset'))
        if not path:raise ValueError('Model texture is unavailable: '+source['texture']+'; import its PNG first')
        mat=upload_asset(dict(project=pid,name=source['texture']+'.png',kind='texture',data=base64.b64encode(path.read_bytes()).decode()))
        converted[i]=mat['id'];mats.append(mat)
    for mesh in model['meshes']:
        for material in set(mesh['materials']):
            triangles=[t for t,mi in zip(mesh['triangles'],mesh['materials']) if mi==material];indices=sorted({i for t in triangles for i in t});remap={old:new for new,old in enumerate(indices)}
            geometry.append(dict(id=uuid.uuid4().hex,kind='geometry',shape='mesh',name=model['name'].removesuffix('.DFF'),nativeName=model['name'],group=group,
              sourceModel=model['id'],position=[0,0,0],rotation=[0,0,0],scale=[1,1,1],chunk=chunk,collision=True,surface=0x00020001,
              material=converted[material],uvRepeat=[1,1],colour=[255]*4,vertices=[mesh['vertices'][i] for i in indices],
              uvs=[mesh['uvs'][i] for i in indices],triangles=[[remap[i] for i in t] for t in triangles]))
    return dict(materials=mats,geometry=geometry)
# Which files of a numbered chunk archive authored geometry goes into (the build and the editor's defaults agree).
def visual_target(files):
    """The chunk's main opaque world: _O_ (Westopolis), else _OS_ (Cryptic Castle has only G/OS), else the visual
    world with the most materials."""
    rg1=[n for n in files if n.upper().endswith('.RG1') and first(files[n],11) is not None]
    for kind in ('O','OS'):   # name tokens: STG0100_O_01, STG0310_01_OS_, STG0401__02__OS_ ...
        n=next((n for n in rg1 if kind in re.split(r'[_.]',n.upper()[:-4])),None)
        if n:return n
    return max(rg1,key=lambda n:len(materials(first(files[n],11))),default=None)
def collision_files(files,stage):
    """Collision worlds of a chunk, the stage's own first (stg0910 chunk 1 also holds stg0909's)."""
    cols=[n for n in files if 'COLI' in n.upper() and n.upper().endswith('.BSP')]
    return sorted(cols,key=lambda n:(f'STG{stage:04d}' not in n.upper(),n))
def collision_role(name,cols):
    """#39: 'extra' = a companion of another collision file of the same chunk, named like it plus trailing '_'
    (Glyphic Canyon STG0201_COLI_NN_.BSP; stage 404 COLI_NN_ / __ / ___, three identical copies). Same three sector
    extensions (0x50E mesh, 0x11D tree, 0x11F user data 'attribute' + 'FVF.UserData'), but its own simpler mesh, one
    material, and 'attribute' words that are not surface flags (all 0 in 404, junk-looking in 201). What the game
    uses it for is not proven. Every other collision file is 'main'."""
    stem=name.upper()[:-4]
    return 'extra' if stem!=stem.rstrip('_') and any(o.upper()[:-4]==stem.rstrip('_') for o in cols) else 'main'
def collision_ref(num,fi,i):return f'collision-{num}-{i}' if fi==0 else f'collision-{num}-{fi}-{i}'
# stgNNNN_dat.one STGNNNN.BDT (#53, all 62 files checked): 64-byte header ('stgNNNN' then zeros), u32 row count, then
# 32-byte rows {u32 chunk, f32 lo xyz, f32 hi xyz, u32 0x50 (every row on the disc)} and nothing after them. There are
# NO stored per-chunk visibility lists: a chunk owns one or more boxes (Westopolis 28 rows for 23 chunks, Iron Jungle
# 403 139 for 79); a few chunks have none (201 chunk 20, 601 chunks 6/12/17/60, 700 13/15/16). PROVEN in game (#53 log in
# the tickets file): a chunk is DRAWN only while the player (or the camera - not separated) is inside one of its boxes
# (Westopolis chunk 1 box cut at z 100 -> black at z -300; Glyphic backdrop 60 without its box -> no backdrop; 62's box
# moved to the start -> 62's backdrop there). Not the same for collision: chunk 1's collision still held the player
# outside its box (loaded at the start, not dropped in 6 s). With NO box holding the start the stage never finishes
# loading. 'visible' below is therefore a derived candidate list, not a field of the file.
def read_bdt(dat_files):
    """{chunk: [(lo, hi), ...]} - every box, in file order"""
    name=next((n for n in dat_files if n.upper().endswith('.BDT')),None);out={}
    if not name:return out
    b=dat_files[name];count=struct.unpack_from('<I',b,64)[0]
    for i in range(count):
        at=68+i*32;num=struct.unpack_from('<I',b,at)[0];out.setdefault(num,[]).append((list(struct.unpack_from('<3f',b,at+4)),list(struct.unpack_from('<3f',b,at+16))))
    return out
def boxes_meet(a,b):return all(a[0][k]<=b[1][k] and b[0][k]<=a[1][k] for k in range(3))
def chunk_visibility(bdt):
    """{chunk: sorted chunks that can be loaded while the player is inside this chunk's boxes} = chunks with a box that
    overlaps one of its boxes (the game loads every chunk whose box holds the player, so this is the union over the
    chunk's region; the exact set at one point is: chunks with a box containing that point)."""
    return {n:sorted(m for m,bm in bdt.items() if any(boxes_meet(x,y) for x in bn for y in bm)) for n,bn in bdt.items()}
def write_bdt_boxes(bdt,overrides):
    """BDT bytes with every row of an overridden chunk replaced by its new boxes ({chunk: [(lo, hi), ...]}, [] = no box
    = never loaded); other rows keep their order and bytes. Also how a NEW chunk gets its row (first cut: one box =
    its geometry bounds plus a margin, see build_project)."""
    count=struct.unpack_from('<I',bdt,64)[0];rows=[bdt[68+32*i:100+32*i] for i in range(count)]
    keep=[r for r in rows if struct.unpack_from('<I',r)[0] not in overrides]
    for num,boxes in overrides.items():
        for lo,hi in boxes:keep.append(struct.pack('<I6fI',num,*lo,*hi,0x50))
    return bytes(bdt[:64])+struct.pack('<I',len(keep))+b''.join(keep)+bytes(bdt[68+32*count:])

# ---- stage parameters: files/nukkoro2.inf (Shift-JIS INI, CRLF). One [Name] section per stage-table row (the name is
# the row's string, main.dol stage table 0x804C4A18 + row*0x50 + 4). The game parses it once at boot
# (fn_801DFFB4); results rank fn_802028D0: score >= A/B/C/D minimum (bosses: clear time <= seconds, Normal line only).
# RANK_H/N/D: A B C D. MISSIONCOUNT_D/H: target count + second value. Research: agents/claude/research/rank.
INF_NAME='nukkoro2.inf'
def stage_row_names():
    if 'row-names' in CACHE:return CACHE['row-names']
    d=(DISC/'sys/main.dol').read_bytes();off=struct.unpack('>18I',d[0:72]);addr=struct.unpack('>18I',d[72:144]);size=struct.unpack('>18I',d[144:216])
    def rd(a,n):
        for sa,so,ss in zip(addr,off,size):
            if ss and sa<=a<sa+ss:return d[so+a-sa:so+a-sa+n]
        return b''
    out={}
    for i in range(80):
        row=rd(0x804C4A18+i*0x50,8)
        if len(row)<8:break
        num,ptr=struct.unpack('>iI',row);s=rd(ptr,64).split(b'\0')[0]
        if not s:break
        out.setdefault(num,s.decode('ascii','replace'))
    CACHE['row-names']=out;return out
def inf_sections(text):
    """[(name, [lines])] in file order; lines without the header, as written."""
    out=[];cur=None
    for line in text.split('\r\n'):
        m=re.fullmatch(r'\[(.+)\]',line.strip())
        if m:cur=(m.group(1),[]);out.append(cur)
        elif cur:cur[1].append(line)
        else:out.append((None,[line])) if not out or out[-1][0] is not None else out[-1][1].append(line)
    return out
def inf_values(lines):
    vals={}
    for line in lines:
        m=re.fullmatch(r'\s*([A-Z_0-9]+)\s*:\s*(.*?)\s*',line)
        if m:
            try:vals[m.group(1)]=[int(x) for x in m.group(2).split()]
            except ValueError:vals[m.group(1)]=m.group(2)
    return vals
# MISSIONCOUNT_<key> : A B -> mission record +8 / +0xC of the slot's mission id (missions-phase7.md); HARD = Expert id
MISSION_COUNT_KEYS=(('hero','H'),('normal','N'),('dark','D'),('expert','HARD'))
def stage_params(stage):
    name=stage_row_names().get(stage)
    if not name:return None
    text=(DISC/'files'/INF_NAME).read_bytes().decode('shift_jis')
    lines=next((l for n,l in inf_sections(text) if n==name),None)
    if lines is None:return dict(section=name,boss=stage%100>=10,lines=[],grades={},counts={})
    v=inf_values(lines)
    grades={k:v.get('RANK_'+c) for k,c in (('hero','H'),('normal','N'),('dark','D')) if isinstance(v.get('RANK_'+c),list)}
    counts={k:v.get('MISSIONCOUNT_'+c) for k,c in MISSION_COUNT_KEYS if isinstance(v.get('MISSIONCOUNT_'+c),list)}
    return dict(section=name,boss=stage%100>=10,lines=[l for l in lines if l.strip()],grades=grades,counts=counts)
GOAL_ROUTES={'N':'Normal','H':'Hero','D':'Dark','X':'Expert'}
def goal_pose_objects(sp):
    """Editor objects for the stage-clear pose positions (GOALEVENTPOS_<route>: x y z)."""
    out=[]
    for line in (sp or {}).get('lines',[]):
        m=re.fullmatch(r'\s*GOALEVENTPOS_([A-Z])\s*:\s*(-?\d+(?:\.\d+)?)\s+(-?\d+(?:\.\d+)?)\s+(-?\d+(?:\.\d+)?)\s*',line)
        if m:
            r=m.group(1);out.append(dict(id=f'goalpose-{r}',kind='goalpose',route=r,name=f"Finish pose ({GOAL_ROUTES.get(r,r)})",
              position=[float(m.group(2)),float(m.group(3)),float(m.group(4))],rotation=[0,0,0],scale=[1,1,1]))
    return out
# Start yaw (measured in game 2026-10-18, agents/claude/scripts/yaw_probe.py): the PLAYER line's angles are RADIANS (yang
# 10 -> -147.0 deg, 20 -> 65.9, 30 -> -81.1, 69 -> -6.6, 90 -> 116.6 = rad mod 360; the earlier 'x 1.29578' fitted only
# multiples of 45). Game yaw 0 = facing -z, positive toward -x = the editor's rotation[1]. Editor objects hold degrees
# (-180..180); the line holds radians with 4 decimals (integers stay integers, so unedited lines round-trip).
YAW_K=1.29578   # the old wrong factor, only for migrating projects saved with it
def deg_from_line(v):
    d=math.degrees(float(v));d=(d+180)%360-180;return round(d,2)
_ANGLE_INTS=None
def line_angle(deg):
    """the game reads the PLAYER angles as INTEGER radians (1.5708 -> 1 rad, measured): the integer n (|n| <= 2000) whose n rad,
    wrapped to a full turn, is nearest the wanted angle (the smallest |n| within 0.25 deg; worst case ~0.05 deg)"""
    global _ANGLE_INTS
    if _ANGLE_INTS is None:_ANGLE_INTS=sorted(range(-2000,2001),key=abs)
    t=math.radians(float(deg));best=(9.0,0)
    for n in _ANGLE_INTS:
        e=abs((n-t+math.pi)%(2*math.pi)-math.pi)
        if e<best[0]:best=(e,n)
        if e<math.radians(0.25):break
    return str(best[1])
def player_start_objects(sp):
    """The game's own start points: PLAYER : <player#> x y z xang yang zang (degrees; 1 = second player in 2P)."""
    out=[]
    for line in (sp or {}).get('lines',[]):
        m=re.fullmatch(r'\s*PLAYER\s*:\s*(\d+)((?:\s+-?\d+(?:\.\d+)?){6})\s*',line)
        if m:
            v=[float(x) for x in m.group(2).split()];n=int(m.group(1))
            out.append(dict(id=f'playerstart-{n}',kind='playerstart',player=n,name='Player start (game)' if n==0 else f'Player {n+1} start (game)',
              position=v[:3],rotation=[deg_from_line(v[3]),deg_from_line(v[4]),deg_from_line(v[5])],scale=[1,1,1],lineAngles=v[3:6]))
    return out
# setid.bin (disc root): 8-byte header (0, count) + count x {u32 class id = list<<8|type, u64 little-endian stage mask}.
# A class can only be created in stages whose bit is set. Bit per stage found by matching every stage's own placements
# against the masks (42 stages match 100%): story stages count down from Westopolis = 26 in stage-table order, bosses
# and extras continue from 3 and wrap to 63. (#32 research, 2026-10-17)
SETID_BIT={100:26,200:25,201:24,202:23,300:22,301:21,302:20,400:19,401:18,402:17,403:16,404:15,500:14,501:13,502:12,503:11,504:10,
  600:9,601:8,602:7,603:6,604:5,700:4,210:3,310:2,410:1,411:0,412:63,510:62,511:61,610:60,611:59,612:58,613:57,614:56,615:55,616:54,617:53,618:52,710:51}
def read_setid():
    b=(DISC/'files/setid.bin').read_bytes();n=struct.unpack_from('<I',b,4)[0]
    return {struct.unpack_from('<I',b,8+i*12)[0]:int.from_bytes(b[12+i*12:20+i*12],'little') for i in range(n)},b
def setid_allows(stage,list_,type_):
    bit=SETID_BIT.get(stage);masks,_=read_setid()
    return bit is None or (masks.get((list_<<8)|type_,0)>>bit)&1==1
def patched_setid(stage,classes):
    """setid.bin with `stage` allowed for every (list, type) in classes (classes missing from the table are appended)."""
    bit=SETID_BIT[stage];masks,b=read_setid();out=bytearray(b);n=struct.unpack_from('<I',b,4)[0];index={struct.unpack_from('<I',b,8+i*12)[0]:i for i in range(n)}
    for l,t in classes:
        oid=(l<<8)|t
        if oid in index:at=12+index[oid]*12;out[at:at+8]=(int.from_bytes(out[at:at+8],'little')|(1<<bit)).to_bytes(8,'little')
        else:out+=struct.pack('<I',oid)+(1<<bit).to_bytes(8,'little');n+=1;struct.pack_into('<I',out,4,n);index[oid]=n-1
    return bytes(out)

# ---- class registry (phase 2, #32 part 1): main.dol 0x8052B080 = 286 pointers to {info "NAME\0...", load(), unload(),
# create()}, class id (list<<8|type) in the upper half of +0x18. Marker classes split it into blocks; each block's end
# marker loads the archive its classes draw from: common gadgets stg_cmn_gdt.one, weapons common/WeaponResource.one,
# STAGE gadgets stgNNNN_gdt.one + _fx.one (only the stage's own), enemies each their own enemy/<Name>Data.one, vehicles
# common/vehicleResource.one; partners follow. A class only needs setid.bin permission when its resources come from a
# block that every stage loads (or its own file): non-empty load() in the common / weapon / vehicle / enemy blocks.
# Not permitted (left out until the phase 3 dependency compiler): the stage gadget block, empty or missing load(),
# bosses (enemy/boss/*: arena set-ups), partners, and anything this reading cannot place.
REGISTRY=0x8052B080
PARTNER_ARCHIVES=['sonic','tails','knuckles','amy','rouge','omega','vector','espio','maria','bee','eggmonitor','doomseye']   # Partner 1..12
BLOCK_MARKERS={'GadgetCommonDataStart':'common','WeaponDataStart':'weapon','GadgetStageDataStart':'stage','EnemyEntryStart':'enemy',
  'VehicleDataStart':'vehicle','VehicleDataEnd':'partner'}
# #104 bosses that need their own stage beyond their archive (docs/bosses/BOSS-SUPPORT.md section 2)
# Heavy Dog (00/B6) and Black Doom (00/B8) were stage-bound until the runtime/boss_ai.cpp (#126, 2026-10-20: both fight
# in the Blank stage on the new AI, Black Doom without his 610 / 616 / 617 helpers); Devil Doom stays bound.
BOSS_STAGE_BOUND={(0,0xBE):'Devil Doom needs the whole final-stage set-up (Super Shadow flight)'}
def class_registry():
    """{(list, type): dict(name, block, load, files, anyStage, reason)} from main.dol."""
    if 'class-registry' in CACHE:return CACHE['class-registry']
    d=(DISC/'sys/main.dol').read_bytes();off=struct.unpack('>18I',d[0:72]);addr=struct.unpack('>18I',d[72:144]);size=struct.unpack('>18I',d[144:216])
    def rd(a,n):
        for sa,so,ss in zip(addr,off,size):
            if ss and sa<=a<sa+ss:return d[so+a-sa:so+a-sa+n]
        return b''
    def u32(a):b=rd(a,4);return struct.unpack('>I',b)[0] if len(b)==4 else None
    def body(a,limit):
        out=[]
        while a and len(out)<limit:
            x=u32(a+4*len(out))
            if x is None:break
            out.append(x)
            if x==0x4E800020:break   # blr
        return out
    def strings(a,ins):
        """file names the code builds with lis/addi (or ori) pairs, here and one bl call down"""
        out=[];calls=[];hi={}
        for k,x in enumerate(ins):
            op,rt,ra,imm=x>>26,(x>>21)&31,(x>>16)&31,x&0xFFFF
            if op==15 and ra==0:hi[rt]=imm<<16
            elif op in (14,24) and ra in hi:
                s=rd(hi[ra]+(imm-0x10000 if op==14 and imm&0x8000 else imm),64).split(b'\0')[0]
                if len(s)>=4 and all(32<=c<127 for c in s):out.append(s.decode())
            elif op==18 and x&1:li=x&0x03FFFFFC;calls.append(a+4*k+(li-0x04000000 if li&0x02000000 else li))
        return out,calls
    out={};block=None
    for i in range(400):
        p=u32(REGISTRY+4*i)
        if not p or not 0x80000000<=p<0x81800000:break
        info,load=u32(p),u32(p+4);cid=u32(p+0x18)>>16;name=rd(info,64).split(b'\0')[0].decode('latin1')
        if name in BLOCK_MARKERS:block=BLOCK_MARKERS[name]
        ins=body(load,400);files,calls=strings(load,ins)
        for c in calls:
            more,_=strings(c,body(c,120));files+=[f for f in more if f not in files]
        files=[f for f in files if '.' in f and not f.startswith('!')]
        if cid>>8==0x25:anyStage,reason=False,'system class (list 0x25: block markers, art tables, hints, sound)'
        elif not load:anyStage,reason=False,'no load()'
        elif len(ins)<=1:anyStage,reason=False,'empty load(): resources from the stage'
        elif block=='stage':anyStage,reason=False,'stage gadget: models in the source stage _gdt.one'
        elif block=='enemy':
            own=[f for f in files if f.startswith('enemy/')]
            anyStage=bool(own) and not any(f.startswith('enemy/boss/') for f in own)
            reason=('own loader '+own[0]) if anyStage else ('boss: arena set-up' if own else 'enemy without a known data file')
            if own and all(f.startswith('enemy/boss/') for f in own) and (cid>>8,cid&0xFF) not in BOSS_STAGE_BOUND:
                # #104: bosses load their own enemy/boss/*.one (phase 1: Black Bull spawns, fights, two at once)
                anyStage,reason=True,'boss: own loader '+own[0]
            elif (cid>>8,cid&0xFF) in BOSS_STAGE_BOUND:reason='boss: '+BOSS_STAGE_BOUND[(cid>>8,cid&0xFF)]
        elif block in ('common','weapon','vehicle'):anyStage,reason=True,f'{block} resources (loaded in every stage)'
        elif block=='partner' and 0x91<=cid&0xFF<=0x9C and cid>>8==1:
            # partner data classes 0191..019C: a short load() passing the partner index (li 0..11) -> the global
            # character archive of that partner (same order as the Partner parameter of 0190 placements)
            f=f'character/{PARTNER_ARCHIVES[(cid&0xFF)-0x91]}.one';files=[f]
            anyStage,reason=(DISC/'files'/f).is_file(),f'partner: own loader {f} (by index)'
        else:anyStage,reason=False,f'{block or "unknown"} class'
        out.setdefault((cid>>8,cid&0xFF),dict(name=name,block=block,load=hex(load or 0),files=files,anyStage=anyStage,reason=reason))
    CACHE['class-registry']=out;return out
def class_fields(list_,type_):
    """/api/config catalog extras (UI 2 'All stages'): anyStage = the build can place it in any stage (setid.bin patched
    when needed); gadget = needs files from its own stage; copiesFiles = phase 3: the build copies those files from the
    source stage (stage gadgets, Decoration / Destructable types) or refuses with an error naming what is missing;
    setidStages = stages that allow it today (no patch needed there); block / classReason = why."""
    info=class_registry().get((list_,type_));copies=(list_,type_) in depcompiler.DECOR or bool(info and info['block']=='stage')
    if not info:return dict(anyStage=False,gadget=True,copiesFiles=copies,block=None,classReason='class not in the game registry',setidStages=setid_stages(list_,type_))
    return dict(anyStage=info['anyStage'],gadget=not info['anyStage'],copiesFiles=copies,block=info['block'],classReason=info['reason'],setidStages=setid_stages(list_,type_))
def setid_stages(list_,type_):
    """stage ids whose setid.bin bit allows the class"""
    m=read_setid()[0].get((list_<<8)|type_,0);return [s for s,bit in SETID_BIT.items() if (m>>bit)&1]
# ---- player start (2026-10-17, user: 'manually remove and place the Start point'). The stage's real start is its PLAYER line
# in nukkoro2.inf (PLAYER : 0 x y z xang yang zang, degrees): the game loads the chunks, the start camera and the player
# there - moving it is PROVEN in game (Digital Circuit start at y 1525, 2026-10-16); the heading sign is not (#22). The
# sidecar header spawn (objects of kind 'spawn' = Start override, and Play from here) only TELEPORTS the player after the
# stage has loaded. Rules (UI2 spec 7d):
#  * playerstart objects (player 0 = the main start, 1 = second player in 2P) are written to the PLAYER lines;
#  * project.playerStart.removedOriginal = true: the original start was deleted - load_project no longer re-adds it;
#  * no playerstart for player 0 but a spawn object: the PLAYER line is written from the spawn (position + yaw), so the
#    stage starts there instead of teleporting (new stages: their 'Player start' is a spawn object);
#  * both: the playerstart is the PLAYER line, the spawn still teleports after load (it wins in game) - warned;
#  * neither: the build is refused ('No player start: place one or restore the original');
#  * a playtest build with Play from here also writes the PLAYER line at that point with its yaw (the header has no yaw).
def linked_switch_rules(p,warnings):
    """#93: a Digital Breakable Tile (7/215, ElecCristalWall) waits for the switch flag of its Link ID (record +0x22),
    which only a Trigger Digital Breakable Tile (7/225, ElecCristalWallSwitch) with the same Link ID sets."""
    nat=[o for o in p['objects'] if o.get('kind')=='native']
    links={int(o.get('link') or 0) for o in nat if (o.get('list'),o.get('type'))==(7,225)}
    for o in nat:
        if (o.get('list'),o.get('type'))==(7,215) and int(o.get('link') or 0) not in links:
            warnings.append(f"{o.get('name','Digital Breakable Tile')}: Link ID {int(o.get('link') or 0)} has no 'Trigger: Digital Breakable Tile' - it will never appear. Give it the Link ID of a trigger (or add one).")
# #98: Move range (EnemyBase word 0) is a hard leash - a circle around the enemy's spot it never leaves (quiet runs
# 2026-10-19: a GUN Soldier with Move range 100 and a triangle patrol 240 / 280 units out walks on the spot at 100 units
# for 40 s; the same soldier with 400 completes 2 laps). Patrols (soldier LINEAR / TRIANGLE, robot / pawn WAIT_ACT +
# Linear / Triangle) whose waypoints lie outside get Move range = farthest waypoint + 30 in the BUILD copy (the saved
# project keeps its value); one warning lists them. Waypoints = world-axis offsets (research/appear-types-results.md).
PATROL_CLASSES={(0,0x64):'soldier',(0,0x8D):'soldier',(0,0x68):'walker',(0,0x79):'walker'}
def patrol_rules(p,warnings):
    cat={(c['list'],c['type']):[f['name'] for f in c['fields']] for c in CATALOG if (c['list'],c['type']) in PATROL_CLASSES}
    fixed=[]
    for o in p['objects']:
        k=(o.get('list'),o.get('type'))
        if o.get('kind')!='native' or k not in PATROL_CLASSES or not o.get('params'):continue
        names=cat.get(k) or [];b=bytearray(base64.b64decode(o['params']))
        word=lambda n,fmt='<i':struct.unpack_from(fmt,b,4*names.index(n))[0] if n in names and 4*names.index(n)+4<=len(b) else None
        appear=word('AppearType')
        if PATROL_CLASSES[k]=='soldier':legs={1:1,2:2}.get(appear,0)
        else:legs={1:1,2:2}.get(word('WaitActMoveType'),0) if appear==0 else 0
        if not legs:continue
        pts=[(word(f'Pos{i}_TranslationXFromOrigin','<f') or 0.0,word(f'Pos{i}_TranslationZFromOrigin','<f') or 0.0) for i in range(legs)]
        far=max(math.hypot(x,z) for x,z in pts);mr=word('MoveRange','<f')
        if mr is None or far+30<=mr:continue
        new=float(math.ceil(far+30));struct.pack_into('<f',b,0,new);o['params']=base64.b64encode(bytes(b)).decode()
        fixed.append(f"{o.get('name','enemy')} {round(mr)}->{int(new)}")
    if fixed:warnings.append(f"Patrol: Move range raised so the enemies can reach their waypoints (#98; it is a leash around their spot): {', '.join(fixed[:12])}{' ...' if len(fixed)>12 else ''}. Raise Move range in the inspector to keep it in the project.")
# ---- activation / draw distance (user request 2026-10-18): a SET record is created while the player is within
# render x 100 units (formats.py byte 35; 'low activation = not created', research/appear-types). project.activationScale
# = number (global multiplier) or {scale: 1.0, floor: 1, cap: 255, overrides: [{layer?: 'cmn'|'nrm'|'hrd'|'ds1',
# list?: n, type?: n, match?: regex over the catalog name, scale?: x | value?: n | vanilla?: true}]} (the LAST matching
# override wins). Applied to the build's copy of the set files only; render 0 stays 0. Tests:
# research/activation-distance-tests.md.
# Opt-in preset 'Wider draw distance' (research/activation-distance-tests.md: no fps cost up to 3x, 'max' 8.5-21 fps):
# global 2x, decor / rings / items / breakables 3x, triggers / checkpoints / goal / spawners / mission objects vanilla.
ACTIVATION_PRESETS={'wider':{'scale':2,'overrides':[
    {'match':'Ring|Item|Decoration|Destructable|Effect|Fruit|Light|Case|Box|Roadblock|Ornament','scale':3},
    {'match':'Trigger|Checkpoint|Save Point|SavePoint|Goal|Generator|Mission|Hint|Talking|Cage|Key|Chaos Emerald','vanilla':True}]}}
def activation_rules(p):
    a=p.get('activationScale')
    if a in (None,{},1,1.0):return None
    if isinstance(a,str) or (isinstance(a,dict) and a.get('preset')):
        name=a if isinstance(a,str) else a['preset']
        if name not in ACTIVATION_PRESETS:raise ValueError(f"activationScale preset {name!r}: one of {', '.join(ACTIVATION_PRESETS)}")
        a=ACTIVATION_PRESETS[name]
    if isinstance(a,(int,float)):a={'scale':float(a)}
    if not isinstance(a,dict):raise ValueError('activationScale: a number or {scale, floor, cap, overrides}')
    scale=float(a.get('scale',1.0));floor=int(a.get('floor',1));cap=int(a.get('cap',255))
    if not (0<scale<=100 and 0<=floor<=cap<=255):raise ValueError('activationScale: scale 0..100, 0 <= floor <= cap <= 255')
    names={(c['list'],c['type']):c['name'] for c in json.loads((APP/'catalog.json').read_text('utf-8'))}
    ovs=[]
    for o in a.get('overrides') or []:
        if not isinstance(o,dict):raise ValueError('activationScale.overrides: a list of objects')
        ovs.append(dict(o,match=re.compile(o['match'],re.I) if o.get('match') else None))
    def new_render(obj):
        r=int(obj.get('render') or 0)
        if r==0:return 0
        s,val=scale,None
        for o in ovs:
            if o.get('layer') and o['layer']!=obj.get('layer'):continue
            if o.get('list') is not None and int(o['list'])!=obj.get('list'):continue
            if o.get('type') is not None and int(o['type'])!=obj.get('type'):continue
            if o['match'] and not o['match'].search(names.get((obj.get('list'),obj.get('type')),obj.get('name',''))):continue
            if o.get('vanilla'):s,val=1.0,r
            elif o.get('value') is not None:val=int(o['value'])
            elif o.get('scale') is not None:s,val=float(o['scale']),None
        v=val if val is not None else round(r*s)
        return r if val==r else max(floor,min(cap,v))
    return new_render
def player_start_rules(p,warnings):
    starts=[o for o in p['objects'] if o.get('kind')=='playerstart']
    spawns=[o for o in p['objects'] if o.get('kind')=='spawn']
    main=next((o for o in starts if int(o.get('player',0))==0),None)
    removed=(p.get('playerStart') or {}).get('removedOriginal')
    if p.get('_playtestStart'):   # build_project: Play from here (position, rotation) for this playtest build
        pos,rot=p['_playtestStart'];main=dict(kind='playerstart',player=0,position=pos,rotation=rot)
        starts=[o for o in starts if int(o.get('player',0))!=0]+[main]
    elif main is None and spawns:
        s=spawns[0];main=dict(kind='playerstart',player=0,position=list(s['position']),rotation=list(s.get('rotation',[0,0,0])))
        starts=starts+[main];warnings.append(f"Player start: the stage starts at '{s.get('name','Start')}' {[round(v) for v in s['position']]} (PLAYER line written from it).")
    elif main is not None and spawns:
        warnings.append(f"Player start: a Start override ({spawns[0].get('name','spawn')}) also exists - the stage loads at the Player start and then teleports to the override.")
    if main is None and (removed or p.get('mode')=='new'):
        raise ValueError('No player start: place one (Place player start) or restore the original start before building.')
    return starts
def build_stage_params(p,warnings):
    """mods/files/nukkoro2.inf with this stage's section changed (None = unchanged)."""
    sp=p.get('stageParams');orig=stage_params(p['stage'])
    if not orig:return None
    if not sp:sp=orig
    lines=[l for l in sp.get('lines',orig['lines'])]
    for g in player_start_rules(p,warnings):
        r=list(g.get('rotation',[0,0,0]));la=g.get('lineAngles')
        # an unedited imported start keeps the line's own numbers (the game's original radians, any size)
        ang=[str(x if not float(x).is_integer() else int(x)) for x in la] if la and all(abs(deg_from_line(a)-float(b))<0.01 for a,b in zip(la,r)) else [line_angle(x) for x in r]
        text=f"PLAYER : {int(g.get('player',0))} "+' '.join([str(int(round(float(v)))) for v in g['position']]+ang)
        i=next((i for i,l in enumerate(lines) if re.match(rf"\s*PLAYER\s*:\s*{int(g.get('player',0))}\b",l)),None)
        if i is None:lines.append(text)
        else:lines[i]=text
    for g in [o for o in p['objects'] if o.get('kind')=='goalpose']:
        key=f"GOALEVENTPOS_{g['route']}";text=f"{key} : {' '.join(str(int(round(float(v)))) for v in g['position'])}"
        i=next((i for i,l in enumerate(lines) if re.match(rf'\s*{key}\s*:',l)),None)
        if i is None:lines.append(text)
        else:lines[i]=text
    # #101 stage rules the game reads natively: STARTSPD : 0 <speed> 0 0 = Shadow starts running (Lethal Highway uses 100)
    ss=(p.get('stageRules') or {}).get('startSpeed')
    if ss is not None:
        if not isinstance(ss,(int,float)) or not 0<=ss<=1000:raise ValueError('Rules: start speed 0..1000')
        text=f'STARTSPD : 0 {int(round(ss))} 0 0';i=next((i for i,l in enumerate(lines) if re.match(r'\s*STARTSPD\s*:',l)),None)
        if i is None:lines.append(text)
        else:lines[i]=text
    if sp.get('grades')==orig['grades'] and sp.get('counts')==orig['counts'] and lines==orig['lines']:return None
    def put(key,values):
        text=f"{key} : {' '.join(str(int(x)) for x in values)}"
        for i,l in enumerate(lines):
            if re.match(rf'\s*{key}\s*:',l):lines[i]=text;return
        lines.append(text)
    for route,c in (('hero','H'),('normal','N'),('dark','D')):
        g=(sp.get('grades') or {}).get(route)
        if g is not None:
            if len(g)!=4 or not all(isinstance(x,(int,float)) and 0<=x<=99999999 for x in g):raise ValueError(f'Grades ({route}): four numbers A, B, C, D')
            ok=all(g[i]<=g[i+1] for i in range(3)) if orig['boss'] else all(g[i]>=g[i+1] for i in range(3))
            if not ok:raise ValueError(f"Grades ({route}): {'times must rise from A to D' if orig['boss'] else 'minimum scores must fall from A to D'}")
            put('RANK_'+c,g)
    for route,c in MISSION_COUNT_KEYS:
        n=(sp.get('counts') or {}).get(route)
        if n is not None:
            if len(n)!=2 or not all(isinstance(x,(int,float)) and 0<=x<=99999999 for x in n):raise ValueError(f'Mission count ({route}): two whole numbers A B')
            put('MISSIONCOUNT_'+c,n)
    text=(DISC/'files'/INF_NAME).read_bytes().decode('shift_jis');out=[]
    for name,ls in inf_sections(text):
        if name is not None:out.append(f'[{name}]')
        if name==orig['section']:
            out.extend(lines+[''])
        else:out.extend(ls)
    if orig['section'] not in [n for n,_ in inf_sections(text)]:out+=[f"[{orig['section']}]"]+lines+['']
    warnings.append(f"Stage parameters [{orig['section']}] changed (grades / mission counts); the game reads them at start-up.")
    return '\r\n'.join(out).encode('shift_jis')

def default_chunk(chunks,pos=None):
    """Chunk for new geometry: one with a visual world AND collision whose bounds contain pos, else the nearest such
    chunk, else any chunk with a visual world (geometry tools then warn), else None (stage without numbered chunks)."""
    good=[c for c in chunks if c.get('target') and c.get('collisionTarget')] or [c for c in chunks if c.get('target')]
    if not good:return None
    if pos is None or not all('bounds' in c for c in good):return good[0]['number']
    def dist(c):
        lo,hi=np.array(c['bounds'][0]),np.array(c['bounds'][1]);return float(np.linalg.norm(np.maximum(0,np.maximum(lo-pos,np.array(pos)-hi))))
    return min(good,key=dist)['number']

def resolve_materials(mats):
    """#62: native materials take their texture asset from the CURRENT texture index (an import cached by a long-running
    server, or a saved project, may hold assets from before the disc fallback found them)"""
    texture_library();tn=CACHE['texture_names']
    for m in mats:
        if not m.get('custom') and m.get('texture'):m['asset']=tn.get(m['texture'].lower(),{}).get('id') or m.get('asset')
    return mats
def import_stage(stage,include_visual=True):
    # #65: the art-less import (stage_models / object_models call it first) has its own cache key; it used to share
    # 'stage-<id>' with the full one, so a copy made after /api/models had no stage art and no materials. A cached
    # full import also serves an art-less request.
    key=f'stage-{stage}' if include_visual else f'stage-{stage}-novis'
    for k in (f'stage-{stage}',key):
        if k in CACHE:
            disc_textures(stage);out=json.loads(json.dumps(CACHE[k]));resolve_materials(out['materials'])
            out['id']=uuid.uuid4().hex;out['created']=time.time();return out   # #95: every copy is its own project
    src=stage_dir(stage);objects=[];tails={};originals={};chunks=[];meshes=[];mats=[]
    texture_library();disc_textures(stage);tn=CACHE['texture_names']
    for layer in ['cmn','nrm','hrd','ds1']:
        path=src/f'stg{stage:04d}_{layer}.dat'
        if path.exists():
            obs,tail=read_set(path.read_bytes(),layer);tails[layer]=tail
            originals[layer]=hashlib.sha256(path.read_bytes()).hexdigest()
            for o in obs:o.update(nativeName=native_label(o),name=display_label(native_label(o)),sourceStage=stage);objects.append(o)
    for p in sorted(src.glob(f'stg{stage:04d}_[0-9][0-9].one')):
        num=int(p.stem[-2:]);files=one.read_all(p.read_bytes());entry=dict(number=num,archive=p.name,visual=[],collision=[])
        cols=collision_files(files,stage);entry['target']=visual_target(files);entry['collisionTarget']=cols[0] if cols else None
        for fi,name in enumerate(cols):
            w=read_world(files[name],True);entry['collision'].append(name)
            for i,m in enumerate(w['meshes']):
                meshes.append(dict(id=collision_ref(num,fi,i),name=f'Collision {num:02d}',chunk=num,kind='reference',collision=True,
                  collisionFile=name,collisionRole=collision_role(name,cols),**m))
        for name,data in files.items():
            if name.upper().endswith('.RG1'):
                entry['visual'].append(name)
                if include_visual:
                    try:w=read_world(data)
                    except (ValueError,IndexError,struct.error) as e:
                        entry.setdefault('warnings',[]).append(f'{name}: {e}');continue
                    start=len(mats)
                    for m in w['materials']:
                        m['id']=f'mat-{len(mats)}';m['chunk']=num;m['world']=name;m['asset']=tn.get(m['texture'].lower(),{}).get('id');mats.append(m)
                    for i,m in enumerate(w['meshes']):
                        m['materials']=[start+x for x in m['materials']]
                        meshes.append(dict(id=f'visual-{num}-{name}-{i}',name=name,chunk=num,kind='reference',collision=False,**m))
        chunks.append(entry)
    native_paths=[];dat=src/f'stg{stage:04d}_dat.one'
    if dat.exists():
        files=one.read_all(dat.read_bytes())
        if 'PATH.PTP' in files:native_paths=read_paths(files['PATH.PTP'])
        bdt=read_bdt(files);vis=chunk_visibility(bdt)
        for entry in chunks:
            boxes=bdt.get(entry['number'],[]);entry['boxes']=[[list(lo),list(hi)] for lo,hi in boxes];entry['visible']=vis.get(entry['number'],[])
            if boxes:entry['bounds']=([min(b[0][k] for b in boxes) for k in range(3)],[max(b[1][k] for b in boxes) for k in range(3)])
    # Huge backdrop layers (measured, not named): a chunk whose every visual world spans more than 14000 units across
    # (Glyphic Canyon chunks 60-66: ~33000 x 12000 domes and a flat far plane, centred along the route). The game draws
    # them behind the level like a sky; the editor does the same (app.js referenceNode).
    for entry in chunks:
        spans=[]
        for m in meshes:
            if m['chunk']==entry['number'] and not m['collision'] and m['vertices']:
                v=np.asarray(m['vertices']);spans.append(float(max(np.ptp(v[:,0]),np.ptp(v[:,2]))))
        entry['span']=round(max(spans)) if spans else 0
        entry['backdrop']=bool(spans) and min(spans)>14000
        if entry['backdrop']:
            for m in meshes:
                if m['chunk']==entry['number'] and not m['collision']:m['backdrop']=True
    project=dict(version=1,id=uuid.uuid4().hex,name=STAGE_NAMES.get(stage,f'Stage {stage}')+' edit',stage=stage,
      objects=objects,geometry=[],paths=native_paths,triggers=[],scripts=[],environment=[],materials=mats,
      native=dict(tails=tails,originals=originals,chunks=chunks,pathImportVersion=1),reference=meshes,stageParams=(sp:=stage_params(stage)),
      editor=dict(camera=None),created=time.time())
    project['objects']+=goal_pose_objects(sp)+player_start_objects(sp)
    project['stageObjects']=stage_code_objects(stage,objects,native_paths)
    CACHE[key]=project;out=json.loads(json.dumps(project));resolve_materials(out['materials']);return out

# ---- #74 stage-code objects (read-only, /api/stage 'stageObjects'): things the stage's own code creates (no SET record),
# shown so the editor can draw a ghost. Lethal Highway (202): the Black Tank, the Black Arms ship the player must destroy.
# DECODED from the data, not proven in game (2026-10-17): capture/stg0202.jsonl draws CT0202BKTANK.DFF (stg0202_gdt.one,
# looked up with no SET record; + BRK / CCL / BARRIAR.CCL / BON / _ANIM.MTN / BKTANK.BIN). Its 49 'Black Tank Command
# Collision' records (08/99: params DetectX/Y/Z box, ActionID 0 Move / 1 Stop / 2 Accelerate / 3 Set Pos, Param0, Param1)
# drive it when the player enters their box: Accelerate = (Param0 seconds, Param1 speed), Stop = Param0 seconds (0 = until
# a Move), Set Pos = route Param0 (0-3) at fraction Param1 (0..1). Route k = the k-th PATH.PTP spline with settings 2048
# (100 / 101 / 102 / 104, the editor's 'platform' paths): every Set Pos box lies 40-1000 units from its route at nearly
# that fraction (0.74 -> 0.70, 0.35 -> 0.30, 0.2 -> 0.25, 0 -> 0), and the four splines chain across the map. Start =
# route 0 at 0 (spline 100's first point), set by the box at the player start. Scale and how the code picks the
# splines (by order or id) are questions.
STAGE_CODE={202:dict(name='Black Tank (stage code)',model='CT0202BKTANK.DFF',archive='stg0202/stg0202_gdt.one',
  files=['CT0202BKTANK.DFF','CT0202BKTANKBRK.DFF','CT0202BKTANK.CCL','CT0202BKTANKBARRIAR.CCL','CT0202BKTANK.BON','CT0202BKTANK_ANIM.MTN','BKTANK.BIN'],
  commandClass=(8,0x99),routeSettings=2048)}
def stage_code_objects(stage,objects,paths):
    spec=STAGE_CODE.get(stage)
    if not spec:return []
    routes=[q for q in paths if q.get('settings')==spec['routeSettings']];acts=['move','stop','accelerate','setpos']
    def pts(q):return [list(x['position']) if isinstance(x,dict) else list(x[:3]) for x in q['points']]
    cmds=[]
    for o in objects:
        if o.get('kind')!='native' or (o['list'],o['type'])!=spec['commandClass']:continue
        dx,dy,dz,act,p0,p1=struct.unpack_from('<3fI2f',base64.b64decode(o['params']))
        c=dict(id=o['id'],layer=o['layer'],position=o['position'],rotation=o['rotation'],detect=[dx,dy,dz],action=acts[act] if act<4 else act,param0=p0,param1=p1)
        if act==2:c.update(seconds=p0,speed=p1)
        elif act==1:c.update(seconds=p0)
        elif act==3:c.update(route=int(p0),routeSpline=routes[int(p0)]['splineId'] if 0<=int(p0)<len(routes) else None,fraction=p1)
        cmds.append(c)
    model=next((m['id'] for m in archive_models(spec['archive'])['models'] if m['name']==spec['model']),None) if spec.get('archive') else None
    start=pts(routes[0])[0] if routes else None
    return [dict(id=f'stagecode-{stage}-blacktank',kind='stageobject',name=spec['name'],readOnly=True,proven=False,model=spec['model'],
      modelId=model,modelArchive=spec['archive'],files=spec['files'],routes=[dict(route=k,splineId=q['splineId'],points=len(q['points'])) for k,q in enumerate(routes)],
      pathIds=[q['splineId'] for q in routes],start=start,startRoute=0,startFraction=0.0,commands=cmds,
      notes='Decoded from the data (command collisions + capture), not proven in game; route k = k-th PATH.PTP spline with settings 2048; scale unknown.')]

def validate_project(p):
    if p.get('version')!=1:raise ValueError('Unsupported project version')
    project_dir(p['id']);stage_dir(p['stage'])
    if not isinstance(p.get('native'),dict) or set(p['native'].get('tails',{}))-{'cmn','nrm','hrd','ds1'}:raise ValueError('Invalid native layout layers')
    for key in ['objects','geometry','paths','triggers','scripts','environment']:
        if not isinstance(p.get(key),list) or len(p[key])>100000:raise ValueError(f'Invalid {key} collection')
    ids=set()
    for o in p['objects']+p['geometry']+p['paths']+p['triggers']:
        if not isinstance(o.get('id'),str) or o['id'] in ids:raise ValueError('Each item needs a unique ID')
        ids.add(o['id'])
    # JSON parser rejects nonfinite numbers; explicit transforms keep renderer and compiler bounds sensible.
    for o in p['objects']+p['geometry']:
        for key,default in [('position',[0]*3),('rotation',[0]*3),('scale',[1]*3)]:
            vals=o.get(key,default)
            if len(vals)!=3 or not all(isinstance(x,(int,float)) and math.isfinite(x) and abs(x)<=1e7 for x in vals):raise ValueError(f"Invalid {key} on {o.get('name','item')}")
        if o.get('kind')=='native' and o.get('layer') not in p['native']['tails']:raise ValueError('Object layer is not available in this stage')
    return p

def new_stage(options):
    # Reuse the engine's resource slot while replacing all authored stage content.
    p=import_stage(100);p['id']=uuid.uuid4().hex;p['name']=str(options.get('name','New stage'))[:128] or 'New stage'
    p['mode']='new';p['reference']=[];p['paths']=[];p['nativeTemplates']=[o for o in p['objects'] if o.get('kind')=='native']
    p['objects']=[dict(id=uuid.uuid4().hex,kind='spawn',name='Player start',position=[0,90,650],rotation=[0,0,0],scale=[1,1,1])]
    def place(label,pos):
        source=next(o for o in p['nativeTemplates'] if o.get('nativeName',o['name'])==label)
        o=json.loads(json.dumps(source));o.update(id=uuid.uuid4().hex,position=pos,rotation=[0,0,0],link=0,render=source.get('render') or 10)   # render 0 = never activates
        p['objects'].append(o)
    place('Goal Ring',[0,40,-650])   # originals float ~20 above the ground (floor top is y 20)
    for g in goal_pose_objects(p.get('stageParams')):   # the clear animation plays here: next to the goal
        g['position']=[0,20,-760];p['objects'].append(g)
    if options.get('populated',True):
        place('Bk Soldier',[-250,20,-150]);place('Bk Soldier',[250,20,-300]);place('Weapon Box',[-180,20,350]);place('Weapon',[180,25,350])
    # A solid closed rectangle, with independent face vertices/UVs for later editing.
    vertices=[];triangles=[];uv=[]
    faces=[((-600,20,900),(600,20,900),(600,20,-900),(-600,20,-900)),
      ((-600,-20,-900),(600,-20,-900),(600,-20,900),(-600,-20,900)),
      ((-600,-20,900),(600,-20,900),(600,20,900),(-600,20,900)),
      ((600,-20,-900),(-600,-20,-900),(-600,20,-900),(600,20,-900)),
      ((-600,-20,-900),(-600,-20,900),(-600,20,900),(-600,20,-900)),
      ((600,-20,900),(600,-20,-900),(600,20,-900),(600,20,900))]
    for face in faces:
        base=len(vertices);vertices.extend(face);uv.extend([[0,0],[1,0],[1,1],[0,1]]);triangles.extend([[base,base+1,base+2],[base,base+2,base+3]])
    mat=next(m for m in p['materials'] if m['chunk']==1 and m['texture']=='s0100_nn005' and re.search(r'_O_\d+\.RG1$',m['world'],re.I))
    p['geometry']=[dict(id=uuid.uuid4().hex,kind='geometry',name='Starter rectangle',shape='mesh',position=[0,0,0],rotation=[0,0,0],scale=[1,1,1],
      vertices=vertices,triangles=triangles,uvs=uv,uvRepeat=[8,12],colour=[220,220,220,255],chunk=1,collision=True,surface=0x00020001,material=mat['id'])]
    p['editor']={'camera':{'position':[1300,1200,1500],'target':[0,0,0]}}
    save_project(p);return p

def save_project(p):
    validate_project(p);d=project_dir(p['id']);path=d/'project.json'
    with LOCK:
        if path.exists() and p.get('fresh'):
            # #95: a fresh copy (/api/stage) whose id is already a saved project. Its own earlier save has the same
            # 'created' time (a re-save: allowed); anything else is ANOTHER project that would be overwritten.
            try:disk=json.loads(path.read_text('utf-8'))
            except (OSError,ValueError):disk={}
            if disk.get('created')!=p.get('created'):
                raise ValueError(f"Save refused: project id {p['id']} already belongs to '{disk.get('name','another project')}'. "
                                 "Reload the stage for a fresh copy (it gets its own id) and save again.")
        if path.exists():
            # Keep recovery snapshots; project file stays independent of the game build.
            snaps=d/'history';snaps.mkdir(exist_ok=True)
            shutil.copy2(path,snaps/f'{time.time_ns()}.json')
        compact={k:v for k,v in p.items() if k not in ('reference','fresh')}
        compact['updated']=time.time();atomic(path,json_bytes(compact))
        mtime=path.stat().st_mtime   # UI 2's dirty guard compares this with /api/config projects[].saved
    return dict(id=p['id'],name=p['name'],saved=True,mtime=mtime)

def load_project(pid):
    p=json.loads((project_dir(pid)/'project.json').read_text('utf-8'))
    imported=import_stage(p['stage']);p['reference']=[] if p.get('mode')=='new' else imported['reference']
    if not p.get('materials') and p.get('mode')!='new':p['materials']=imported['materials']   # #65: copies saved without materials
    if p.get('mode')!='new' and not p['native'].get('pathImportVersion'):
        p['paths']=imported['paths']+p['paths'];p['native']['pathImportVersion']=1
    for m in p['materials']:
        if m.get('custom'):m['asset']=asset(project_asset(pid,m['file']))
    resolve_materials(p['materials'])   # #62: saved projects keep the asset ids of the day they were imported
    for route in p.get('missions',[]):
        if route.get('icon'):route['icon']['asset']=asset(project_asset(pid,route['icon']['file']))
    # chunk facts measured at import (targets, bounds, backdrop) for projects imported before they existed
    facts={c['number']:c for c in imported['native']['chunks']}
    for c in p['native'].get('chunks',[]):
        for k in ('target','collisionTarget','bounds','span','backdrop'):
            if k in facts.get(c['number'],{}) and k not in c:c[k]=facts[c['number']][k]
        for k in ('boxes','visible'):   # #53: always the current reading (bounds of older imports kept the LAST box only)
            if k in facts.get(c['number'],{}):c[k]=facts[c['number']][k]
    if not any(o.get('kind')=='goalpose' for o in p['objects']):   # projects made before finish poses were editable
        p['objects']+=goal_pose_objects(p.get('stageParams') or imported.get('stageParams'))
    ps_=p.setdefault('playerStart',{})
    if ps_.get('yawUnits')!='radians-line':
        # 2026-10-18: imported starts held the raw PLAYER angles (or, for a few hours, angles x YAW_K): back to the line's
        # numbers -> degrees (the line is radians); placed starts keep their degrees
        lines=(p.get('stageParams') or {}).get('lines',[])
        for o in p['objects']:
            if o.get('kind')=='playerstart' and str(o.get('id','')).startswith('playerstart-'):
                n=int(o.get('player',0));m=next((re.fullmatch(r'\s*PLAYER\s*:\s*\d+((?:\s+-?\d+(?:\.\d+)?){6})\s*',l) for l in lines if re.match(rf'\s*PLAYER\s*:\s*{n}\b',l)),None)
                if not m:continue
                v=[float(x) for x in m.group(1).split()];r=[float(x) for x in o.get('rotation',[0,0,0])]
                if all(abs(a-b)<0.01 or abs(a*YAW_K-b)<0.05 for a,b in zip(v[3:6],r)):
                    o['rotation']=[deg_from_line(x) for x in v[3:6]];o['lineAngles']=v[3:6]
        ps_['yawUnits']='radians-line'
    if not any(o.get('kind')=='playerstart' for o in p['objects']) and p.get('mode')!='new' and not (p.get('playerStart') or {}).get('removedOriginal'):
        p['objects']+=player_start_objects(p.get('stageParams') or imported.get('stageParams'))
    return p

# ---- #44 texture overrides: project.textureOverrides = {<game texture name>: {id, asset, file (project PNG), name,
# width, height}}. The runtime's texture pack (runtime/texture_pack.cpp, Dolphin convention) replaces a texture whose key
# "tex1_<w>x<h>_<hash>[_<palette hash>]_<fmt>" matches a file "<name> [<key>].png" in the game's textures\Custom; the build
# writes the project PNG under the key of EVERY disc texture with that name (a name can map to several textures:
# Prison Island s0301_dn009 is a CMPR level surface and a C4 prop texture) into textures/Custom/Workshop, which the
# playtest copies into the private game.
def texture_keys(stage):
    """{texture name (lower case): [keys]} for the stage's archives and the shared ones the stage uses"""
    ck=f'tex-keys-{stage}'
    if ck in CACHE:return CACHE[ck]
    td=texture_decoder()
    archives=sorted(stage_dir(stage).glob('*.one'))+[DISC/'files/stg_cmn_gdt.one',DISC/'files/character/shadow.one']+sorted((DISC/'files/common').glob('*.one'))
    out={}
    for a in archives:
        if not a.exists():continue
        try:files=one.read_all(a.read_bytes())
        except Exception:continue
        for name,data in files.items():
            if b'\x15\x00\x00\x00' not in data:continue
            for tname,fmt,w,h,raw,pal,tl in td.natives_in(data):
                k=td.key_name(fmt,w,h,raw,pal);ks=out.setdefault(tname.lower(),[])
                if k not in ks:ks.append(k)
    CACHE[ck]=out;return out
def build_texture_overrides(p,build,warnings):
    ov=p.get('textureOverrides') or {}
    if not ov:return 0
    if not isinstance(ov,dict) or len(ov)>2048:raise ValueError('Invalid textureOverrides')
    keys=texture_keys(p['stage']);n=0
    for name,o in ov.items():
        if not isinstance(o,dict) or not o.get('file'):raise ValueError(f'textureOverrides {name!r}: needs the uploaded PNG (file)')
        src=project_asset(p['id'],o['file'])
        if not src.exists() or src.suffix.lower()!='.png':raise ValueError(f'textureOverrides {name!r}: {o["file"]} is not a project PNG')
        ks=keys.get(str(name).lower(),[])
        if not ks:warnings.append(f'Texture override {name}: no texture of that name in stage {p["stage"]} or the shared archives - not applied.');continue
        safe=re.sub(r'[^A-Za-z0-9_.-]','_',str(name))
        for k in ks:atomic(build/'textures/Custom/Workshop'/f'{safe} [{k}].png',src.read_bytes());n+=1
        warnings.append(f'Texture override {name}: {len(ks)} game texture(s) replaced ({", ".join(ks)}).')
    return n
# ---- small UI 2 endpoints
def reveal_asset(data):
    """POST /api/reveal {asset[, dry]}: open Explorer with the asset's file selected (dry: only report the path)"""
    path=ASSETS.get(str(data.get('asset','')))
    if not path or not Path(path).exists():raise ValueError('Unknown asset')
    if not data.get('dry'):subprocess.Popen(['explorer','/select,',str(Path(path))])
    return dict(ok=True,path=str(path))
def playtest_log(pid,lines=40):
    """GET /api/playtest-log?id=: the last lines of the newest test game's log (game.log of an editor playtest, else the
    newest *.log in that game folder)"""
    d=project_dir(pid)/'playtests'
    games=sorted([g for g in d.iterdir() if g.is_dir()],key=lambda g:g.stat().st_mtime) if d.exists() else []
    proc=PROCESSES.get(pid);running=bool(proc and proc.poll() is None)
    if not games:return dict(log=[],running=running,game=None,file=None)
    g=games[-1];logs=[g/'game.log'] if (g/'game.log').exists() else sorted(g.glob('*.log'),key=lambda f:f.stat().st_mtime)[-1:]
    if not logs:return dict(log=[],running=running,game=g.name,file=None)
    f=logs[-1];size=f.stat().st_size
    start=max(0,size-1024*1024)   # the game logs a [plr] line per frame: read enough to keep ~40 other lines
    with open(f,'rb') as h:h.seek(start);tail=h.read().decode('utf-8','replace').splitlines()
    if start and tail:tail=tail[1:]   # the first line may be cut
    tail=[l for l in tail if not l.startswith(('[plr]','[copy-quad]'))][-int(lines):]
    return dict(log=tail,running=running,game=g.name,file=f.name)

def project_asset(pid,name):
    if not re.fullmatch('[a-f0-9]{24}\\.(png|wav|ogg|adx|mp3|m4a|aac|flac|wma|glb|gltf|obj|fbx|bvh)',name):raise ValueError('Invalid project asset')
    return project_dir(pid)/'assets'/name

def upload_asset(data):
    pid=data['project'];raw=base64.b64decode(data['data'],validate=True)
    if len(raw)>32*1024*1024:raise ValueError('Asset exceeds 32 MiB')
    sha=hashlib.sha256(raw).hexdigest()[:24];kind=data.get('kind','texture')
    if kind=='texture' or kind=='icon':
        im=Image.open(io.BytesIO(raw))
        if im.width>4096 or im.height>4096:raise ValueError('Maximum image size is 4096×4096')
        im=im.convert('RGBA');output=io.BytesIO();im.save(output,format='PNG');filename=sha+'.png';atomic(project_asset(pid,filename),output.getvalue())
        name='sw_'+sha[:20]
        return dict(id='custom-'+sha,name=data.get('name','Imported image'),texture=name,file=filename,custom=True,
          asset=asset(project_asset(pid,filename)),raw=b64(material_bytes(name)),width=im.width,height=im.height,kind=kind)
    suffix=Path(data.get('name','')).suffix.lower()
    if kind in ('model','animation','sfx','music'):
        extensions={'model':('.glb','.gltf','.obj','.fbx'),'animation':('.glb','.gltf','.fbx','.bvh'),'sfx':('.wav','.ogg','.adx'),'music':('.wav','.adx','.mp3','.m4a','.aac','.flac','.wma','.ogg')}
        if suffix not in extensions[kind]:raise ValueError('Unsupported '+kind+' format')
        if suffix=='.glb' and (len(raw)<12 or raw[:4]!=b'glTF' or struct.unpack_from('<II',raw,4)!=(2,len(raw))):raise ValueError('Invalid GLB2 header')
        if suffix=='.gltf':
            doc=json.loads(raw)
            if not str(doc.get('asset',{}).get('version','')).startswith('2'):raise ValueError('Use glTF2 assets')
        if suffix=='.wav' and not (raw[:4]==b'RIFF' and raw[8:12]==b'WAVE'):raise ValueError('Invalid WAV header')
        if suffix=='.ogg' and raw[:4]!=b'OggS':raise ValueError('Invalid OGG header')
        filename=sha+suffix;atomic(project_asset(pid,filename),raw);decoder=None
        if kind=='music' and suffix not in ('.wav','.adx'):   # decoded once here (Windows' Media Foundation, else the portable ffmpeg): a song the build cannot convert is refused now
            try:decoder=song_wav(project_asset(pid,filename))[1]
            except Exception as e:
                project_asset(pid,filename).unlink(missing_ok=True)
                raise ValueError(f"{data.get('name','This song')}: this PC cannot decode {suffix[1:].upper()} ({e}). Export it as WAV, MP3, M4A or FLAC.")
        return dict(id='asset-'+sha,kind=kind,file=filename,name=str(data.get('name','Asset'))[:255],bytes=len(raw),format=suffix[1:],**({'decoder':decoder} if decoder else {}),
          runtime='pending',metadata=dict(units='game',upAxis='Y',skeleton='',clip='',loop=False,gain=1,pitch=1,spatial=True),scriptKey='asset_'+sha[:12])
    if suffix not in ('.wav','.ogg','.adx'):raise ValueError('Use WAV, OGG or ADX for a voice clip')
    filename=sha+suffix;atomic(project_asset(pid,filename),raw)
    return dict(file=filename,name=data.get('name','Voice clip'),kind='voice')

# ---- #47 moving original stage art. project.artEdits = {ref: {offset: [x,y,z], rotation: [rx,ry,rz] degrees (game order
# Z, X, Y like authored geometry), scale: [sx,sy,sz], pivot: [x,y,z] (default: the piece's box centre)}} with ref = the
# editor's reference id 'visual-<chunk>-<world>-<piece>' (one sector) or 'visual-<chunk>-<world>' (the whole world).
# The build rewrites that world's positions (formats.transform_world), moves the chunk's collision faces whose three
# corners lie inside the piece's ORIGINAL box (+2 units; collision meshes do not share vertices with the visual ones,
# so this is a first cut that can also take faces of other art inside that box), and grows the chunk's BDT boxes.
ART_REF=re.compile(r'visual-([0-9]+)-(.+?\.RG1)(?:-([0-9]+))?',re.I)
def art_matrix(edit,pivot):
    vals=[edit.get('offset',[0,0,0]),edit.get('rotation',[0,0,0]),edit.get('scale',[1,1,1]),edit.get('pivot',list(pivot))]
    if any(len(v)!=3 or not all(isinstance(x,(int,float)) and math.isfinite(x) for x in v) for v in vals) or any(abs(x)<1e-6 for x in vals[2]):
        raise ValueError('Invalid artEdits entry: offset / rotation / scale / pivot are three finite numbers, scale not 0')
    x,y,z=np.radians(vals[1]);cx,sx,cy,sy,cz,sz=np.cos(x),np.sin(x),np.cos(y),np.sin(y),np.cos(z),np.sin(z)
    rz=np.array([[cz,-sz,0],[sz,cz,0],[0,0,1]]);rx=np.array([[1,0,0],[0,cx,-sx],[0,sx,cx]]);ry=np.array([[cy,0,sy],[0,1,0],[-sy,0,cy]])
    pv=np.asarray(vals[3],float);M=np.eye(4);M[:3,:3]=(ry@rx@rz)@np.diag(vals[2]);M[:3,3]=pv+np.asarray(vals[0],float)-M[:3,:3]@pv
    return M
def art_edits_by_chunk(p):
    """{chunk: {world: {piece index or None (whole world): edit}}}"""
    out={}
    for ref,edit in (p.get('artEdits') or {}).items():
        m=ART_REF.fullmatch(str(ref))
        if not m or not isinstance(edit,dict):raise ValueError(f'Invalid artEdits reference {ref!r}')
        out.setdefault(int(m[1]),{}).setdefault(m[2],{})[None if m[3] is None else int(m[3])]=edit
    return out
def point_triangle_distance(P,T):
    """min distance from each point P (N,3) to the triangles T (M,3,3) (closest point on each triangle; chunks of 256)"""
    out=np.full(len(P),np.inf)
    a,b,c=T[:,0],T[:,1],T[:,2];ab,ac=b-a,c-a
    for s in range(0,len(P),256):
        p=P[s:s+256,None,:];ap=p-a;bp=p-b;cp=p-c
        d1=(ab*ap).sum(-1);d2=(ac*ap).sum(-1);d3=(ab*bp).sum(-1);d4=(ac*bp).sum(-1);d5=(ab*cp).sum(-1);d6=(ac*cp).sum(-1)
        va=d3*d6-d5*d4;vb=d5*d2-d1*d6;vc=d1*d4-d3*d2;den=va+vb+vc;den=np.where(np.abs(den)<1e-12,1e-12,den)
        v=vb/den;w=vc/den;q=a+v[...,None]*ab+w[...,None]*ac   # interior projection
        def seg(x,y):
            e=y-x;t=np.clip(((p-x)*e).sum(-1)/np.maximum((e*e).sum(-1),1e-12),0,1);return x+t[...,None]*e
        inside=(va>=0)&(vb>=0)&(vc>=0)
        cand=[np.where(inside[...,None],q,np.inf)]+[seg(x,y) for x,y in ((a,b),(b,c),(c,a))]
        dist=np.min([np.linalg.norm(np.where(np.isfinite(x),p-x,np.inf),axis=-1) for x in cand],axis=0)
        out[s:s+256]=dist.min(axis=1)
    return out
def move_collision_faces(m,box,M,surface=None,tol=4.0):
    """faces of collision mesh m whose three corners lie inside box (lo, hi) AND (when the moved piece's own triangles
    `surface` are given) within tol units of that surface move with M; their corners are copied so neighbours that
    stay keep theirs. Returns the number of faces moved. (#47, 2026-10-17: the box test alone also took the floor /
    walls inside a tall see-through piece's box - Prison Island STG0301_G_01 light shafts: 336 faces, now 0.)"""
    v=np.asarray(m['vertices'],float);lo,hi=np.asarray(box[0])-2,np.asarray(box[1])+2
    inside=np.all((v>=lo)&(v<=hi),axis=1) if len(v) else np.zeros(0,bool)
    hit=[i for i,t in enumerate(m['triangles']) if all(inside[j] for j in t)]
    if hit and surface is not None and len(surface):
        near=np.zeros(len(v),bool);idx=sorted({j for i in hit for j in m['triangles'][i]})
        near[idx]=point_triangle_distance(v[idx],np.asarray(surface,float))<=tol
        hit=[i for i in hit if all(near[j] for j in m['triangles'][i])]
    for i in hit:
        base=len(m['vertices'])
        for j in m['triangles'][i]:m['vertices'].append((M[:3,:3]@v[j]+M[:3,3]).tolist())
        m['triangles'][i]=[base,base+1,base+2]
    return len(hit)

# ---- #57 chunk prefabs: project.chunkPrefabs = [{sourceStage, chunk, number (a name token 1..99 unused here),
# host (an EXISTING chunk of this stage, default the first), offset [x, y, z] (optional)}]. The runtime only finds files
# the disc's file table lists (runtime/disc.cpp patches existing entries), so a new stgTTTT_MM.one would never load;
# the game does load EVERY world / collision file inside a chunk archive (stage 910 chunk 1 holds two stages' files).
# So the prefab's files are appended to the host chunk's archive, renamed (STGssss -> STGtttt, _NN -> _MM), its worlds
# moved by offset (formats.transform_world), its collision rebuilt (same faces + flags, moved), the stage textures
# its worlds name (base + MatFX passes) appended to this stage's texCM TXD, and the host chunk gets extra BDT boxes =
# the source chunk's boxes moved by offset (drawn only while the player is inside one; none -> bounds + 1000).
# Objects of the source chunk are not copied.
# newChunk: true (2026-10-17) = a REAL new chunk instead: runtime/disc.cpp now lists NEW mod files in the file
# table, and the game's chunk manager (fn_8003A6B4) loads any chunk 0..167 whose BDT box holds the player (32 resident),
# by name '%s/%s_%02d.one' (docs/stage-workshop/research/fst-new-mod-files.md). The build then writes
# stgTTTT_MM.one (files renamed _NN -> _MM, collision rebuilt moved by offset) and a BDT row for chunk MM. Not proven in
# game yet; the hosted mode stays the default.
def chunk_prefab(p,pf,existing,tex_have,host,new_chunk=False):
    from models import material_fx_textures
    s,n,m=int(pf['sourceStage']),int(pf['chunk']),int(pf['number']);off=np.asarray(pf.get('offset',[0,0,0]),float)
    top=167 if new_chunk else 99
    if not 1<=m<=top or m in existing:raise ValueError(f'Chunk prefab: number {m} is already a chunk of stage {p["stage"]} (or is not 1..{top})')
    if len(off)!=3 or not np.isfinite(off).all():raise ValueError('Chunk prefab: offset is three finite numbers')
    src_arc=stage_dir(s)/f'stg{s:04d}_{n:02d}.one'
    if not src_arc.exists():raise ValueError(f'Chunk prefab: stage {s} has no chunk {n}')
    files=one.read_all(src_arc.read_bytes());T=np.eye(4);T[:3,3]=off;out=[];tex=[];lo=np.full(3,np.inf);hi=np.full(3,-np.inf);col_meshes=[]
    main_cols=collision_files(files,s)[:1]   # the game uses one collision file per chunk (#39: the companion is not the ground)
    def rename(name):
        nm=re.sub(f'STG{s:04d}',f'STG{p["stage"]:04d}',name,flags=re.I)
        # the host chunk's number stays the chunk token (the game appears to pick a chunk's files by it), then _MM
        return re.sub(rf'_{n:02d}(?=[_.])',f'_{m:02d}' if new_chunk else f'_{host:02d}_{m:02d}',nm,count=1)
    for name,data in files.items():
        new=data
        if name.upper().endswith(('.RG1','.BSP')) and first(data,11) is not None:
            col='COLI' in name.upper() and name.upper().endswith('.BSP')
            w=read_world(data,col)
            for mesh in w['meshes']:
                v=np.asarray(mesh['vertices'],float)+off
                if len(v):lo=np.minimum(lo,v.min(axis=0));hi=np.maximum(hi,v.max(axis=0))
            if col:   # merged into the host's main collision file by the build, not copied as a file
                if name in main_cols:col_meshes+=[dict(mm,vertices=(np.asarray(mm['vertices'],float)+off).tolist()) for mm in w['meshes']]
                continue
            else:
                if off.any():new,_=transform_world(data,{i:T for i in range(len(w['meshes']))})
                wc=first(first(data,11),8)
                for t,mb,v in children(wc or b''):
                    if t!=7:continue
                    tx=first(mb,6);names=[x.split(b'\0')[0].decode('ascii','replace') for k,x,_ in children(tx) if k==2][:1] if tx else []
                    tex+=[x.lower() for x in names+material_fx_textures(mb) if x]
        out.append((rename(name),new))
    # stage textures the worlds name, from the source stage's texture archives (texCM first, then gdt / fx)
    adds={};missing=[]
    for t in dict.fromkeys(tex):
        if t in tex_have or t in adds:continue
        found=None
        for a in [stage_dir(s)/f'stg{s:04d}_texCM.one',stage_dir(s)/f'stg{s:04d}_gdt.one',stage_dir(s)/f'stg{s:04d}_fx.one']:
            if not a.exists():continue
            for nm,data in one.read_all(a.read_bytes()).items():
                if nm.upper().endswith('.TXD'):
                    found=depcompiler.txd_names(data).get(t)
                    if found:break
            if found:break
        if found:adds[t]=found
        else:missing.append(t)
    sb=read_bdt(one.read_all((stage_dir(s)/f'stg{s:04d}_dat.one').read_bytes())).get(n,[])
    boxes=[(list(np.asarray(a)+off),list(np.asarray(c)+off)) for a,c in sb] or ([(list(lo-1000),list(hi+1000))] if np.isfinite(lo).all() else [])
    info=dict(template=src_arc,collisionName=rename(main_cols[0]) if main_cols else f'STG{p["stage"]:04d}_COLI_{m:02d}.BSP')
    return out,adds,boxes,missing,(lo,hi),col_meshes,info

# ---- #73 click-a-surface collision: project.surfaceCollision = [{id, name?, chunk?, surface?, doubleSided?, triangles:
# [[[x,y,z] x3], ...] in WORLD space (the picked faces as drawn, art edits / transforms already applied), source?: {...}
# (what was picked; not read by the build)}]. Each entry becomes an invisible solid mesh of its chunk (default: the chunk
# whose boxes hold the faces' centre), so it joins that chunk's main collision file with its surface flag (default
# 0x00020001), gets correct sector boxes from make_collision, and widens the chunk's BDT box like authored geometry.
def surface_collision(p,warnings):
    for e in p.get('surfaceCollision') or []:
        tri=np.asarray(e.get('triangles',[]),float)
        if tri.ndim!=3 or tri.shape[1:]!=(3,3) or not len(tri) or not np.isfinite(tri).all() or np.abs(tri).max()>1e7:
            raise ValueError(f"Surface collision {e.get('name') or e.get('id')}: triangles must be [[x,y,z] x3] world points")
        if len(tri)>65000:raise ValueError(f"Surface collision {e.get('name') or e.get('id')}: more than 65000 faces")
        if e.get('doubleSided'):tri=np.concatenate([tri,tri[:,[1,0,2]]])
        chunk=e.get('chunk')
        if chunk is None:chunk=default_chunk(p['native']['chunks'],tri.reshape(-1,3).mean(0).tolist())
        if chunk is None:raise ValueError(f"Surface collision {e.get('name') or e.get('id')}: no chunk holds it; set its chunk")
        n=len(tri)
        p['geometry'].append(dict(id=f"surfcol-{e.get('id','')}",kind='geometry',shape='mesh',name=str(e.get('name') or 'Surface collision'),
          chunk=int(chunk),collision=True,renderVisible=False,surface=int(e.get('surface',0x00020001)),position=[0,0,0],rotation=[0,0,0],scale=[1,1,1],
          vertices=tri.reshape(-1,3).tolist(),triangles=np.arange(3*n).reshape(n,3).tolist(),uvs=[[0,0]]*(3*n)))
        warnings.append(f"Surface collision {e.get('name') or e.get('id')}: {n} face(s) added to chunk {int(chunk)}'s collision.")

# #133: a NEW stage has no original kill planes (collision faces with surface 0x20): a player who falls off its edge or
# through a hole fell for ever. The build adds an invisible kill sheet (surface 0x20, collision only) 400 under the lowest
# geometry, 2000 wider than the stage on every side, unless the project already has a kill plane of its own. Falling on
# it is a normal death: a life lost and the respawn at the last checkpoint passed (else the start).
def fallout_plane(p,warnings):
    if p.get('mode')!='new' or p.get('noFalloutPlane'):return
    geo=[g for g in p.get('geometry') or [] if g.get('kind')=='geometry']
    if any(int(g.get('surface',0))==0x20 and g.get('collision',True) for g in geo):return
    pts=[]
    for g in geo:
        try:pts.append(np.asarray(transform_mesh(g)['vertices'],float))
        except (ValueError,KeyError,TypeError):continue
    if not pts:return
    v=np.concatenate(pts);lo,hi=v.min(0),v.max(0)
    if not (np.isfinite(lo).all() and np.isfinite(hi).all()):return
    y=float(lo[1])-400;x0,z0,x1,z1=float(lo[0])-2000,float(lo[2])-2000,float(hi[0])+2000,float(hi[2])+2000
    chunk=default_chunk(p['native']['chunks'],[(x0+x1)/2,y,(z0+z1)/2]) if p.get('native',{}).get('chunks') else None
    p['geometry'].append(dict(id='fallout-kill-plane',kind='geometry',shape='mesh',name='Fall-out kill plane',chunk=int(chunk if chunk is not None else 1),
      collision=True,renderVisible=False,surface=0x20,position=[0,0,0],rotation=[0,0,0],scale=[1,1,1],
      vertices=[[x0,y,z0],[x1,y,z0],[x1,y,z1],[x0,y,z1]],triangles=[[0,2,1],[0,3,2]],uvs=[[0,0]]*4))
    warnings.append(f'Fall-out kill plane added at height {y:.0f} (a fall off the stage costs a life and respawns at the last checkpoint).')

def transform_mesh(g):
    v=np.asarray(g['vertices'],dtype=float);t=np.asarray(g['triangles'],dtype=int)
    if v.ndim!=2 or v.shape[1]!=3 or t.ndim!=2 or t.shape[1]!=3 or not len(t):raise ValueError(f"Empty or malformed mesh: {g['name']}")
    if not np.isfinite(v).all() or t.min()<0 or t.max()>=len(v):raise ValueError('Invalid mesh vertices or indices')
    scale=np.asarray(g.get('scale',[1,1,1]));v*=scale
    # Game convention: Z, then X, then Y; the frontend uses matching YXZ Euler order.
    x,y,z=np.radians(g.get('rotation',[0,0,0]));cx,sx,cy,sy,cz,sz=np.cos(x),np.sin(x),np.cos(y),np.sin(y),np.cos(z),np.sin(z)
    rz=np.array([[cz,-sz,0],[sz,cz,0],[0,0,1]]);rx=np.array([[1,0,0],[0,cx,-sx],[0,sx,cx]]);ry=np.array([[cy,0,sy],[0,1,0],[-sy,0,cy]])
    v=v@(ry@rx@rz).T+np.asarray(g.get('position',[0,0,0]))
    if scale.prod()<0:t=t[:,[1,0,2]]
    colour=g.get('colour',[200,200,200,255]);uv=g.get('uvs',[[0,0]]*len(v));repeat=g.get('uvRepeat',[1,1])
    uv=(np.asarray(uv)*repeat).tolist()
    return dict(vertices=v.tolist(),triangles=t.tolist(),uvs=uv,colours=[colour]*len(v),
      flags=[int(g.get('surface',0x00020001))]*len(t))

def merge_meshes(geoms,max_verts=12000):
    """#128: geometry rows of one chunk grouped by (material, visible, collision, surface, colour) -> one row per group
    holding the transformed mesh in '_mesh' (split at max_verts); a single row stays as it is"""
    groups={}
    for g in geoms:
        k=(g.get('material'),bool(g.get('renderVisible',True)),bool(g.get('collision',True)),int(g.get('surface',0x00020001)),tuple(g.get('colour',[200,200,200,255])))
        groups.setdefault(k,[]).append(g)
    out=[]
    for k,gs in groups.items():
        if len(gs)==1:out.append(gs[0]);continue
        cur=None
        for g in gs:
            m=transform_mesh(g)
            if cur is None or len(cur['_mesh']['vertices'])+len(m['vertices'])>max_verts:
                cur=dict(gs[0],name=f"{gs[0].get('name','Mesh')} (+{len(gs) - 1} merged)",_mesh=dict(vertices=[],triangles=[],uvs=[],colours=[],flags=[]));out.append(cur)
            o=len(cur['_mesh']['vertices']);mm=cur['_mesh']
            mm['vertices']+=m['vertices'];mm['uvs']+=m['uvs'];mm['colours']+=m['colours'];mm['flags']+=m['flags']
            mm['triangles']+=[[a+o,b+o,c+o] for a,b,c in m['triangles']]
    return out

def build_project(p):
    # Play from here: a PLAYTEST build (/api/build body forPlaytest=true, sent before /api/playtest) starts the player
    # at project.playtestStart {position, rotation [0,yaw,0], enabled} instead of the Player start object; Export mod builds
    # (no flag) ignore it. The sidecar header carries the start position only (no yaw field), so rotation is not used yet.
    for_playtest=bool(p.pop('forPlaytest',False))
    validate_project(p);errors=[];warnings=[];src=stage_dir(p['stage']);d=project_dir(p['id']);save_project(p)
    # The build works on a cleaned COPY: what the game cannot take yet is left out with a warning naming it (the saved
    # project keeps everything). Only real problems (broken logic, changed source files) stop the build.
    p=copy.deepcopy(p);ps=p.get('playtestStart') or {};playtest_start=None
    if for_playtest and ps.get('enabled') and len(ps.get('position') or [])==3:
        playtest_start=[float(x) for x in ps['position']]
        p['objects']=[o for o in p['objects'] if o.get('kind')!='spawn']+[dict(id='playtest-start',kind='spawn',name='Play from here',position=playtest_start,rotation=list(ps.get('rotation') or [0,0,0]),scale=[1,1,1])]
        p['_playtestStart']=(playtest_start,[float(x) for x in (ps.get('rotation') or [0,0,0])])   # PLAYER line too: start there with its yaw
        warnings.append(f'Playtest build: the player starts at Play from here {playtest_start} (the Player start object is not used in this build).')
    surface_collision(p,warnings);fallout_plane(p,warnings);linked_switch_rules(p,warnings);patrol_rules(p,warnings)
    # #154 engine limit: the game's SetManager (ctor fn_800CB08C) has fixed 2,048-entry tables for all layers together and
    # the SET loader never checks it - 2,049+ objects write past them (2,059+ corrupt a count and the game quits).
    # #158: the Enhancement 'Objects per stage' (set_objects) raises it to 4,096 / 8,192 - the warning follows the player's
    # setting; a stage over 2,048 also needs it on every PC that plays it. The game loads cmn + nrm OR hrd + ds1.
    by_layer={}
    for o in p['objects']:
        if o.get('kind')=='native':by_layer[o.get('layer')]=by_layer.get(o.get('layer'),0)+1
    n_set=sum(v for k,v in by_layer.items() if k not in ('nrm','hrd'))+max(by_layer.get('nrm',0),by_layer.get('hrd',0))
    cap=set_object_cap()
    if n_set>cap:warnings.append(f"This stage loads {n_set} objects but the game holds at most {cap:,} (Gameplay > Stages > Objects per stage, with Original game logic off) - remove some or raise that setting, or the game can crash.")
    elif n_set>2048:warnings.append(f"This stage loads {n_set} objects: it needs Original game logic off and Gameplay > Stages > Objects per stage at {4096 if n_set<=4096 else 8192:,} or more on every PC that plays it (the original game holds 2,048).")
    elif n_set>2000:warnings.append(f'This stage loads {n_set} objects. The original game holds at most 2,048 - you are close to that limit.')
    try:player_start_rules(p,[])   # refuse early: no player start at all
    except ValueError as e:return dict(ok=False,errors=[str(e)],warnings=warnings)
    if p.get('environment'):warnings.append(f"Environment regions ({len(p['environment'])}) are saved but not built yet (no game compiler).");p['environment']=[]
    bad=[o for o in p['objects'] if o.get('kind') not in ('native','spawn','teleport','goalpose','playerstart','destination')]
    for o in bad:warnings.append(f"{o.get('name','?')}: object type {o.get('kind')!r} is not built yet - left out.")
    if any(m.get('enabled') for m in p.get('missions',[])):
        warnings.append('Custom mission routes are saved but not built yet (pause menu / progress / voice not connected) - the stage keeps its own missions.')
        for m in p['missions']:m['enabled']=False
    original=import_stage(p['stage']);allowed={(o['list'],o['type']) for o in original['objects'] if o.get('kind')=='native'}
    keep=[];registry=class_registry();permitted={};imports=[]
    for o in p['objects']:
        if o in bad:continue
        if o.get('kind')=='native':
            cls=(o['list'],o['type']);foreign=o.get('sourceStage',p['stage'])!=p['stage'] or cls not in allowed
            info=registry.get(cls)
            if cls==(1,0x90) and p['stage'] in SETID_BIT:
                # a Partner placement (0190, empty load) needs its character's data class 0191+n allowed here
                prm=base64.b64decode(o.get('params') or '');n=struct.unpack_from('<i',prm,0)[0] if len(prm)>=4 else 0
                needs=([cls] if not setid_allows(p['stage'],*cls) else [])+([(1,0x90+n)] if 1<=n<=12 and not setid_allows(p['stage'],1,0x90+n) else [])
                if any(need!=cls and not (registry.get(need) or {}).get('anyStage') for need in needs):
                    warnings.append(f"{o['name']}: partner {n} has no character archive - left out.");continue
                for need in needs:
                    ni=registry.get(need) or {}
                    e=permitted.setdefault(need,dict(list=need[0],type=need[1],className=ni.get('name',f'{need[0]:02X}/{need[1]:02X}'),
                      reason=ni.get('reason','partner placement'),sourceStages=[],objects=0,setidStages=setid_stages(*need)))
                    e['objects']+=1
                    if o.get('sourceStage',p['stage']) not in e['sourceStages']:e['sourceStages'].append(o.get('sourceStage',p['stage']))
            elif foreign and info and info['anyStage'] and p['stage'] in SETID_BIT:
                # phase 2 (#32 part 1): the class brings its own resources; setid.bin only has to allow it here
                if not setid_allows(p['stage'],*cls):
                    permitted.setdefault(cls,dict(list=cls[0],type=cls[1],className=info['name'],reason=info['reason'],
                      sourceStages=[],objects=0,setidStages=setid_stages(*cls)))
                    entry=permitted[cls];entry['objects']+=1
                    src_stage=o.get('sourceStage',p['stage'])
                    if src_stage not in entry['sourceStages']:entry['sourceStages'].append(src_stage)
            elif o.get('sourceStage',p['stage'])!=p['stage'] and p['stage'] in SETID_BIT and o['sourceStage'] in SETID_BIT and (
                cls in depcompiler.DECOR or (info and info['block']=='stage')):
                # phase 3 (#32 part 2): decor types / stage gadgets bring their files from the source stage's _gdt.one
                # (a gadget class this stage places itself keeps using this stage's own files)
                if cls in depcompiler.DECOR or cls not in allowed:imports.append((o,o['sourceStage']))
            elif o.get('sourceStage',p['stage'])!=p['stage']:
                warnings.append(f"{o['name']}: copied from stage {o['sourceStage']} - left out ({info['reason'] if info else 'class not in the game registry'}; cross-stage models/textures/sounds need the dependency compiler).");continue
            elif cls not in allowed:
                warnings.append(f"{o['name']}: this stage does not load that object type - left out (resource dependency compiler needed).");continue
            if cls[0]==0 and 0xB4<=cls[1]<=0xBE and p['stage']!=o.get('sourceStage',p['stage']) or (cls[0]==0 and 0xB4<=cls[1]<=0xBE and cls not in allowed):
                warnings.append(f"{o['name']}: boss in another stage - it fights near Shadow inside its Boss-card arena (if set); with several bosses one HP bar shows their summed HP and the stage clears when the last one is down.")
            if not o.get('render'):warnings.append(f"{o['name']}: render distance ID 0 - the game never activates it (originals use 3-10).")
            if o.get('scale',[1,1,1])!=[1,1,1]:
                warnings.append(f"{o['name']}: scaled with the gizmo - built at normal size (native objects size themselves from their parameters).");o['scale']=[1,1,1]
        keep.append(o)
    # #104 / #125: the build used to keep one boss per stage (or a proven pair) and the Egg Breaker only alone - different
    # bosses shared the resource slot 0x16 and hung while loading. runtime/boss_slots.cpp gives each boss its own archive
    # (#104) and handles many at once (#125: 4 / 6 / 8 bosses, 3 Egg Breakers + 5 others, no hang), so every
    # placed boss is kept now. Sounds: every boss family's sound bank while they fit the free sound memory (sound_banks.py fit_banks; about 6).
    p['objects']=keep
    deps=None
    if imports:
        gdt=src/f"stg{p['stage']:04d}_gdt.one"
        if not gdt.exists():errors.append(f"{imports[0][0]['name']}: this stage has no _gdt.one to receive the copied files")
        else:
            try:
                common={n.upper() for n in one.read_all((DISC/'files/stg_cmn_gdt.one').read_bytes())}
                tex=src/f"stg{p['stage']:04d}_texCM.one";tex_names=set()
                if tex.exists():
                    for n,b in one.read_all(tex.read_bytes()).items():
                        if n.upper().endswith('.TXD'):tex_names|=set(depcompiler.txd_names(b))
                deps=depcompiler.compile_imports(p['stage'],stage_dir,one.read_all(gdt.read_bytes()),imports,registry,CATALOG,common,tex_names)
            except (ValueError,OSError,struct.error) as e:errors.append(f'Dependency compiler: {e}');deps=None
            if deps:
                errors+=deps['errors'];warnings+=deps['warnings']
                for cls in deps['classes']:
                    if not setid_allows(p['stage'],*cls):
                        e=permitted.setdefault(cls,dict(list=cls[0],type=cls[1],className=(registry.get(cls) or {}).get('name',f'{cls[0]:02X}/{cls[1]:02X}'),
                          reason='dependency compiler: its files are copied from the source stage',sourceStages=[],objects=0,setidStages=setid_stages(*cls)))
                        for o,s in imports:
                            if (o['list'],o['type'])==cls:
                                e['objects']+=1
                                if s not in e['sourceStages']:e['sourceStages'].append(s)
    try: events=compile_events(p)
    except Exception as e: errors.append(str(e));events=None
    for layer,h in p['native']['originals'].items():
        if hashlib.sha256((src/f"stg{p['stage']:04d}_{layer}.dat").read_bytes()).hexdigest()!=h:errors.append('Source stage changed since import; reimport before building.')
    if errors:return dict(ok=False,errors=errors,warnings=warnings)
    # New staging folder each build means a failed build cannot leave a previously good pack half-overwritten.
    build=d/'builds'/str(time.time_ns());out=build/'mods/files'/src.name;out.mkdir(parents=True)
    try:
        act=activation_rules(p);act_n=act_c=0
        for layer in p['native']['tails']:
            obs=[o for o in p['objects'] if o.get('kind')=='native' and o.get('layer')==layer]
            if act:
                act_n+=len(obs);obs=[dict(o,render=act(o)) if act(o)!=int(o.get('render') or 0) else o for o in obs]
                act_c+=sum(1 for o,q in zip([o for o in p['objects'] if o.get('kind')=='native' and o.get('layer')==layer],obs) if o is not q)
            atomic(out/f"stg{p['stage']:04d}_{layer}.dat",write_set(obs,p['native']['tails'][layer],layer))
        if act:warnings.append(f"Activation distance: {act_c} of {act_n} set records changed (activationScale {json.dumps(p.get('activationScale'))[:120]}); objects are created further away (research/activation-distance-tests.md).")
        by_chunk={}
        for g in p['geometry']:
            if not g.get('enabled',True):continue
            if g.get('chunk') is None and not p['native']['chunks']:
                raise ValueError(f"{g.get('name','Geometry')}: stage {p['stage']} has no chunks to put geometry in (#95 sweep: stage 6000)")
            by_chunk.setdefault(int(g['chunk'] if g.get('chunk') is not None else 1),[]).append(g)
        for key in p.get('collisionEdits',{}):
            # collision-<chunk>-<mesh> (the chunk's first collision file) or collision-<chunk>-<file>-<mesh>
            if not re.fullmatch('collision-[0-9]+(-[0-9]+){1,2}',key):raise ValueError('Invalid original collision reference')
            by_chunk.setdefault(int(key.split('-')[1]),[])
        bounds={};art=art_edits_by_chunk(p);art_bounds={}
        if art and p.get('mode')=='new':raise ValueError('Moving original stage art is for imported stages (a new stage has none)')
        for num in art:by_chunk.setdefault(num,[])
        if p.get('mode')=='new':
            for entry in p['native']['chunks']:by_chunk.setdefault(entry['number'],[])
        for num,geoms in by_chunk.items():
            archive=src/f'stg{p["stage"]:04d}_{num:02d}.one'
            if not archive.exists():raise ValueError('New numbered chunk creation is not connected; choose an existing chunk')
            files=one.read_all(archive.read_bytes());reps={};art_moved=[]
            for wname,pieces in art.get(num,{}).items():
                real=next((n for n in files if n.upper()==wname.upper()),None)
                if real is None:raise ValueError(f'artEdits: chunk {num} has no world {wname}')
                meshes=read_world(files[real])['meshes'];mats={}
                for i,edit in pieces.items():
                    for k in (range(len(meshes)) if i is None else [i]):
                        if k>=len(meshes):raise ValueError(f'artEdits: {wname} has {len(meshes)} pieces, not {k + 1}')
                        if i is None:v=np.concatenate([np.asarray(mm['vertices']) for mm in meshes])
                        else:v=np.asarray(meshes[k]['vertices'])
                        mats[k]=art_matrix(edit,(v.min(axis=0)+v.max(axis=0))/2)
                reps[real],moved=transform_world(files[real],mats)
                for k,(old,lo,hi) in moved.items():
                    art_moved.append(((old.min(axis=0),old.max(axis=0)),mats[k],np.asarray(meshes[k]['vertices'],float)[np.asarray(meshes[k]['triangles'],int)] if meshes[k]['triangles'] else None))
                    a,b=art_bounds.get(num,(lo,hi));art_bounds[num]=(np.minimum(a,lo),np.maximum(b,hi))
                warnings.append(f"Chunk {num}: {real} - {len(moved)} piece(s) moved (original stage art).")
            if p.get('mode')=='new':
                for name,data in files.items():
                    if name.upper().endswith(('.RG1','.BSP')) and first(data,11) is not None:reps[name]=empty_world(data)
                if not geoms:
                    atomic(out/archive.name,one.pack(str(archive),reps));continue
            visual=visual_target(files);cols=collision_files(files,p['stage']);new_col=None
            if not cols and any(g.get('collision',True) for g in geoms):
                # #12: no collision world in this chunk (most of stage 200's): the solid meshes get a new file of their
                # own, which one_archive.pack appends to the chunk's archive
                new_col=f"STG{p['stage']:04d}_COLI_{num:02d}.BSP";cols=[new_col]
                warnings.append(f"Chunk {num}: no collision file - added {new_col} for the solid meshes.")
            for g in geoms:
                if g.get('renderVisible',True) and not visual:raise ValueError(f"{g['name']}: chunk {num} has no visual world to draw it in; choose another chunk")
            edits=p.get('collisionEdits',{})
            current=reps.get(visual,files[visual]) if visual else None;added=[]
            # per collision file: its original meshes minus removed faces; authored solid meshes join the first file
            col_meshes={}
            for fi,col in enumerate(cols):
                touched=fi==0 and (any(g.get('collision',True) for g in geoms) or p.get('mode')=='new')
                meshes_in=[] if p.get('mode')=='new' or col==new_col else read_world(files[col],True)['meshes']
                for index,m in enumerate(meshes_in):
                    removed=set(edits.get(collision_ref(num,fi,index),[]))
                    if not removed:continue
                    touched=True
                    if any(not isinstance(i,int) or i<0 or i>=len(m['triangles']) for i in removed):raise ValueError('Invalid original collision face')
                    m['triangles']=[t for i,t in enumerate(m['triangles']) if i not in removed];m['flags']=[f for i,f in enumerate(m['flags']) if i not in removed]
                for m in meshes_in:
                    for box,M,surf in art_moved:
                        n=move_collision_faces(m,box,M,surf)
                        if n:touched=True;warnings.append(f"Chunk {num}: {n} collision face(s) of {col} moved with the art.")
                if touched:col_meshes[col]=[m for m in meshes_in if m['triangles']]
            collisions=col_meshes.get(cols[0],[]) if cols else []
            available=materials(first(current,11)) if current else []
            chunk_textures={m['texture'] for n,b in files.items() if n.upper().endswith('.RG1') and first(b,11) is not None for m in materials(first(b,11))}
            # #128: many small meshes (tileset pieces: 140 rows in one chunk) crashed the game at load - meshes with the same
            # material / visibility / collision / surface / colour become ONE mesh here (at most 12000 vertices each)
            geoms=merge_meshes(geoms)
            for g in geoms:
                mat=next((m for m in p['materials'] if m['id']==g.get('material')),None)
                if not mat:mat=available[0] if available else None
                if mat is None and g.get('renderVisible',True):raise ValueError('Choose a native material for each mesh')
                # A texture must be resident in this chunk's dictionary, not merely visible in the asset browser.
                # (any visual world of this chunk counts: append_visual copies a material into the target world)
                if g.get('renderVisible',True) and not mat.get('custom') and mat['texture'] not in chunk_textures:raise ValueError(f"{g['name']}: texture {mat['texture']} is not loaded in chunk {num}; use one of this chunk's materials or import a texture")
                m=g['_mesh'] if '_mesh' in g else transform_mesh(g)
                if g.get('renderVisible',True):current=append_visual(current,m,mat)
                added.append(m)
                if g.get('collision',True):collisions.append(m)
            if visual:reps[visual]=current
            if cols and cols[0] in col_meshes:col_meshes[cols[0]]=collisions
            for col,ms in col_meshes.items():
                reps[col]=make_collision(combine(ms)) if ms else empty_world(files[col])
            atomic(out/archive.name,one.pack(str(archive),reps))
            if added:
                a=np.asarray(combine(added)['vertices']);bounds[num]=(a.min(axis=0),a.max(axis=0))
            if num in art_bounds:   # the chunk's boxes must hold the moved art (drawn only while the camera is inside one)
                lo,hi=art_bounds[num];a,b=bounds.get(num,(lo,hi));bounds[num]=(np.minimum(a,lo),np.maximum(b,hi))
        boxes_override={};texcm_adds={}
        prefabs=p.get('chunkPrefabs') or []
        if prefabs:
            if p.get('mode')=='new':raise ValueError('Chunk prefabs are for imported stages for now')
            existing={int(a.stem[-2:]) for a in src.glob(f'stg{p["stage"]:04d}_[0-9][0-9].one')}
            cm=src/f'stg{p["stage"]:04d}_texCM.one';have=set()
            if cm.exists():
                for nm,data in one.read_all(cm.read_bytes()).items():
                    if nm.upper().endswith('.TXD'):have|=set(depcompiler.txd_names(data))
            names_used=set();prefab_boxes={}
            for pf in prefabs:
                if pf.get('newChunk'):
                    m=int(pf['number']);files_new,adds,boxes,missing,(lo,hi),pcol,info=chunk_prefab(p,pf,existing,have|set(texcm_adds),m,True)
                    if m in prefab_boxes:raise ValueError(f'Chunk prefab: two prefabs use chunk number {m}')
                    files_all=list(files_new)+([(info['collisionName'],make_collision(combine(pcol)))] if pcol else [])
                    atomic(out/f'stg{p["stage"]:04d}_{m:02d}.one',one.new_archive(str(info['template']),files_all,block_name=f'blk{m:02d}'))
                    texcm_adds.update(adds);prefab_boxes[m]=list(boxes)
                    warnings.append(f"Chunk prefab: stage {pf['sourceStage']} chunk {pf['chunk']} -> NEW chunk {m} (stg{p['stage']:04d}_{m:02d}.one, {len(files_all)} file(s), {len(adds)} texture(s) copied, {len(boxes)} BDT box(es) {np.round(lo).tolist()} .. {np.round(hi).tolist()}); needs the runtime that lists new mod files (track A).")
                    for t in missing:warnings.append(f"Chunk prefab: texture {t} not found in stage {pf['sourceStage']} (the original lacks it too)")
                    continue
                host=int(pf.get('host') or min(existing));files_new,adds,boxes,missing,(lo,hi),pcol,info=chunk_prefab(p,pf,existing,have|set(texcm_adds),host)
                if host not in existing:raise ValueError(f'Chunk prefab: host chunk {host} is not a chunk of stage {p["stage"]}')
                hp=out/f'stg{p["stage"]:04d}_{host:02d}.one';base=hp if hp.exists() else src/hp.name
                clash=[n for n,_ in files_new if n.upper() in {x.upper() for x in one.read_all(base.read_bytes())} or n.upper() in names_used]
                if clash:raise ValueError(f'Chunk prefab: {clash[0]} already exists in host chunk {host}; use another number')
                names_used|={n.upper() for n,_ in files_new}
                reps=dict(files_new);host_files=one.read_all(base.read_bytes());hcols=collision_files(host_files,p['stage'])
                if pcol:
                    if not hcols:raise ValueError(f'Chunk prefab: host chunk {host} has no collision file to merge into')
                    merged=read_world(host_files[hcols[0]],True)['meshes']+pcol
                    reps[hcols[0]]=make_collision(combine(merged))
                atomic(hp,one.pack(str(base),reps))
                texcm_adds.update(adds);prefab_boxes.setdefault(host,[]).extend(boxes)
                warnings.append(f"Chunk prefab: stage {pf['sourceStage']} chunk {pf['chunk']} -> {len(files_new)} file(s) in chunk {host} ({len(adds)} texture(s) copied, box {np.round(lo).tolist()} .. {np.round(hi).tolist()}).")
                for t in missing:warnings.append(f"Chunk prefab: texture {t} not found in stage {pf['sourceStage']} (the original lacks it too)")
        for key,boxes in (p.get('chunkBoxes') or {}).items():   # #53 authored chunk regions: {chunk: [[lo xyz, hi xyz], ...]}
            if not str(key).isdigit() or not isinstance(boxes,list) or len(boxes)>64:raise ValueError('Invalid chunkBoxes entry')
            for b in boxes:
                if len(b)!=2 or any(len(v)!=3 or not all(isinstance(x,(int,float)) and math.isfinite(x) for x in v) for v in b) or any(b[0][k]>b[1][k] for k in range(3)):
                    raise ValueError(f'Invalid box for chunk {key}: [[lo x,y,z],[hi x,y,z]] with lo <= hi')
            boxes_override[int(key)]=[(list(map(float,b[0])),list(map(float,b[1]))) for b in boxes]
        if bounds or p['paths'] or p.get('mode')=='new' or boxes_override or prefabs:
            dat=src/f'stg{p["stage"]:04d}_dat.one';files=one.read_all(dat.read_bytes());name=next(n for n in files if n.endswith('.BDT'));bdt=bytearray(files[name]);count=struct.unpack_from('<I',bdt,64)[0]
            for i in range(count):
                at=68+i*32;num=struct.unpack_from('<I',bdt,at)[0]
                if num in bounds:
                    lo,hi=bounds[num];oldlo=np.array(struct.unpack_from('<3f',bdt,at+4));oldhi=np.array(struct.unpack_from('<3f',bdt,at+16))
                    if p.get('mode')=='new':struct.pack_into('<6f',bdt,at+4,*(lo-1000),*(hi+1000))
                    else:struct.pack_into('<6f',bdt,at+4,*np.minimum(lo,oldlo),*np.maximum(hi,oldhi))
                elif p.get('mode')=='new':struct.pack_into('<6f',bdt,at+4,1e6,1e6,1e6,1e6+1,1e6+1,1e6+1)
            for host,extra in (prefab_boxes if prefabs else {}).items():
                cur=boxes_override.get(host)
                if cur is None:
                    cnt=struct.unpack_from('<I',bdt,64)[0];cur=[(list(struct.unpack_from('<3f',bdt,68+32*i+4)),list(struct.unpack_from('<3f',bdt,68+32*i+16))) for i in range(cnt) if struct.unpack_from('<I',bdt,68+32*i)[0]==host]
                boxes_override[host]=list(cur)+list(extra)
            if boxes_override:
                bdt=bytearray(write_bdt_boxes(bytes(bdt),boxes_override))
                warnings.append(f"Chunk regions replaced for chunk(s) {', '.join(map(str,sorted(boxes_override)))} (a chunk is loaded only while the player is inside one of its boxes).")
            replacements={name:bytes(bdt)}
            # untouched imported paths keep the game's original PATH.PTP bytes
            game_paths=[q for q in p['paths'] if not q.get('bossLap')]   # #127 boss lap paths only feed mods/boss_path.txt
            unchanged=p.get('mode')!='new' and 'PATH.PTP' in files and game_paths==read_paths(files['PATH.PTP'])
            if (game_paths or p.get('mode')=='new') and not unchanged:replacements['PATH.PTP']=write_paths(game_paths)
            for q in [q for q in game_paths if q.get('pathType')=='loop']:   # a loop path needs 0x8000 track under it (loops-56.md)
                if not any(int(g.get('surface',0))&0x8000 and g.get('collision',True) for g in p.get('geometry') or []):
                    warnings.append(f"{q.get('name','Loop path')}: no track with the loop surface (0x8000, e.g. 'Loop track' 0x60008001) - the game will not attach the loop; give the loop's geometry that surface.")
            atomic(out/dat.name,one.pack(str(dat),replacements))
        if deps and deps['files']:
            gdt=src/f"stg{p['stage']:04d}_gdt.one";atomic(out/gdt.name,one.pack(str(gdt),deps['files']))
            warnings.append(f"{gdt.name}: {len(deps['files'])} file(s) added or extended for objects from other stages ({deps['bytes']/1024:.0f} KiB copied, {len(deps['textures'])} texture(s)).")
        if permitted:
            atomic(build/'mods/files/setid.bin',patched_setid(p['stage'],list(permitted)))
            for e in permitted.values():
                warnings.append(f"setid.bin: allowed {e['className']} ({e['list']:02X}/{e['type']:02X}, from stage {', '.join(map(str,e['sourceStages']))}) in stage {p['stage']} - {e['reason']}.")
        # phase 7 groundwork (file-driven stage identity): project.stageFiles = {fog: <stage>, light: <stage>} uses that
        # stage's stgNNNN.fog (24 x 40-byte fog entries) / stgNNNN_light.bin (stage lighting) here. Sky = Trigger Skybox
        # objects (00/59: chunks to draw) + backdrop chunks (chunkPrefabs); music is chosen in code (not in a file).
        stage_files_report=build_stage_files(p,out,warnings)
        import custom_stage;custom_plan=custom_stage.apply_goals(p,warnings)   # native goals -> MISSIONCOUNT lines
        inf=build_stage_params(p,warnings)
        if inf is not None:atomic(build/'mods/files'/INF_NAME,inf)
        custom=[m for m in p['materials'] if m.get('custom') and m.get('kind','texture')=='texture' and any(g.get('material')==m['id'] for g in p['geometry'])]
        if texcm_adds and not custom:
            archive=src/f'stg{p["stage"]:04d}_texCM.one';files=one.read_all(archive.read_bytes());txd=next(n for n in files if n.upper().endswith('.TXD'))
            atomic(out/archive.name,one.pack(str(archive),{txd:depcompiler.txd_add(files[txd],list(texcm_adds.values()))}))
        if custom:
            archive=src/f'stg{p["stage"]:04d}_texCM.one';files=one.read_all(archive.read_bytes());txd=next(n for n in files if n.endswith('.TXD'))
            base_txd=depcompiler.txd_add(files[txd],list(texcm_adds.values())) if texcm_adds else files[txd]
            replacement,keys=append_textures(base_txd,[(m['texture'],project_asset(p['id'],m['file'])) for m in custom])
            atomic(out/archive.name,one.pack(str(archive),{txd:replacement}))
            for m in custom:
                dest=build/'textures/Custom/Workshop'/f'{m["texture"]} [{keys[m["texture"]]}].png';atomic(dest,project_asset(p['id'],m['file']).read_bytes())
        build_texture_overrides(p,build,warnings)
        assets=p.get('customAssets',[])
        if p.get('stageSettings'):
            atomic(build/'mods/workshop-assets/stage-settings.json',json_bytes(dict(version=1,runtime='authoring-only',settings=p['stageSettings'])))
            warnings.append('Stage lighting, OST and trait settings are saved for later runtime integration; they are not applied yet.')
        if assets:
            ids={a['id'] for a in assets}
            if len(ids)!=len(assets):raise ValueError('Custom asset IDs must be unique')
            bindings=[]
            for o in p['objects']+p['geometry']:
                slots=o.get('assetBindings',{})
                if any(v and v not in ids for v in slots.values()):raise ValueError(f"{o['name']}: missing custom asset binding")
                if slots:bindings.append(dict(object=o['id'],name=o['name'],slots=slots))
            for a in assets:atomic(build/'mods/workshop-assets/files'/a['file'],project_asset(p['id'],a['file']).read_bytes())
            atomic(build/'mods/workshop-assets/manifest.json',json_bytes(dict(version=1,stage=p['stage'],runtime='authoring-only',assets=assets,bindings=bindings)))
            warnings.append('Custom models/animations/SFX and object asset bindings are packaged as groundwork; runtime replacement/playback is pending.')
        report=dict(ok=True,path=str(build),errors=[],warnings=warnings,objects=len(p['objects']),geometry=len(p['geometry']),setidPermitted=list(permitted.values()),
          dependencies=(deps or {}).get('report',[]),copiedBytes=(deps or {}).get('bytes',0),copiedTextures=(deps or {}).get('textures',[]),forPlaytest=for_playtest,playtestStart=playtest_start,effectsNotCopied=(deps or {}).get('effects',[]),stageMemory=(deps or {}).get('memory'),
          stageFiles=stage_files_report,experimental=bool(p['geometry']),message='Native object pack built.' if not p['geometry'] else 'Geometry pack built; new native mesh/collision writer requires private in-game validation.')
        atomic(build/f'mods/stages/stg{p["stage"]:04d}.bin',events)
        import events as ev4
        if p.get('sidecar')=='v4' or ev4.needs_v4(p):   # SIDECAR-V4.md: the runtime VM runs ALL logic from it when present; v3 stays for volumes
            v4,dbg=ev4.compile_graph_v4(p)
            atomic(build/f'mods/stages/stg{p["stage"]:04d}.v4.bin',v4);atomic(build/f'mods/stages/stg{p["stage"]:04d}.v4.json',json_bytes(dbg))
            warnings.append(f'Sidecar v4 written ({len(dbg["code"])} instructions, {len(dbg["entries"])} entry points): the game runs all logic from it (v3 kept for volumes and the spawn point).')
            if dbg.get('skipped'):warnings.append(f'Logic: {len(dbg["skipped"])} node(s) left out, not in the game yet ("needs game"): '+'; '.join(dbg['skipped'][:8]))
            for snd in dbg.get('sounds',[]):   # Play sound: the runtime plays mods/sounds/<name>.wav (WAV PCM)
                sys.path.insert(0,str(APP.parent/'audio'));import adx
                src=project_asset(p['id'],snd['file']);out=build/f"mods/sounds/{snd['name']}.wav";out.parent.mkdir(parents=True,exist_ok=True)
                raw=src.read_bytes()
                if adx.is_wav(src):atomic(out,raw)
                elif raw[:2]==b'\x80\x00':x,r=adx.decode(raw);bio=io.BytesIO();adx.write_wav(bio,x,r);atomic(out,bio.getvalue())
                else:tmp=out.with_name(out.name+'.tmp.wav');adx.song_to_wav(src,tmp);tmp.replace(out)
                warnings.append(f"Sound {snd['name']}: written to mods/sounds for Play sound.")
        if p.get('lightBake'):report['lightBake']=light_bake_build(p['stage'],build);warnings.append(f"Lighting occupancy baked ({report['lightBake']['bytes']} bytes, {report['lightBake']['chunks']} chunks): mods/lighting/stg{p['stage']:04d}.occ")
        report['lighting']=build_lighting(p,build,warnings)
        report['enemyPaths']=build_enemy_paths(p,build,warnings)
        report['customStage']=custom_stage.write_sidecar(p,build,warnings)
        rules={k:v for k,v in (p.get('stageRules') or {}).items() if k in ('timeLimit','startRings','startLives') and v is not None}
        if rules:   # #101: for the runtime (ask 2026-10-19); the unmodified game ignores it
            atomic(build/'mods/stage_rules.json',json.dumps(dict(format=1,slot=p['stage'],**rules),indent=1).encode());warnings.append(f"Rules {', '.join(rules)}: mods/stage_rules.json written for the game runtime.")
        arenas=[]   # #123 the Boss card's arena -> mods/boss_arena.txt for the runtime leash (runtime/boss_arena.cpp)
        for o in p['objects']:
            a=(o.get('boss') or {}).get('arena') or {};n=lambda v:f'{float(v):g}'
            if o.get('kind')!='native' or o.get('list')!=0 or not 0xB4<=int(o.get('type',0))<=0xBE or a.get('shape') not in ('circle','box','ring'):continue
            c=a.get('centre') or o['position']
            if a['shape']=='box':w,dp=(a.get('size') or [1600,1600])[:2];arenas.append(f"{int(o['type']):02X} box {n(c[0])} {n(c[2])} {n(w)} {n(dp)}")
            elif a['shape']=='ring':arenas.append(f"{int(o['type']):02X} ring {n(c[0])} {n(c[2])} {n(a.get('radius',800))} {n(a.get('inner',300))}")
            else:arenas.append(f"{int(o['type']):02X} circle {n(c[0])} {n(c[2])} {n(a.get('radius',800))}")
        if arenas:atomic(build/'mods/boss_arena.txt',''.join(x+chr(10) for x in arenas).encode());warnings.append(f"Boss arena: {len(arenas)} written to mods/boss_arena.txt (the runtime keeps Blue Falcon / Black Bull 0210 inside it; other bosses keep their own range).")
        elif (build/'mods/boss_arena.txt').exists():(build/'mods/boss_arena.txt').unlink()
        laps=[]   # #127 Boss card movement 'patrol' (a project path) / 'circle' (radius round the arena centre) -> mods/boss_path.txt
        for o in p['objects']:   # '<type> <speed> x z x z ...' (a closed loop; runtime/egg_breaker.cpp drives Egg Breaker on it)
            mv=(o.get('boss') or {}).get('move') or {};n=lambda v:f'{float(v):g}'
            if o.get('kind')!='native' or o.get('list')!=0 or not 0xB4<=int(o.get('type',0))<=0xBE or mv.get('mode') not in ('patrol','circle'):continue
            if mv['mode']=='patrol':
                path=next((x for x in p.get('paths') or [] if x.get('id')==mv.get('path')),None)
                pp=(path or {}).get('position') or [0,0,0];yw=math.radians(float(((path or {}).get('rotation') or [0,0,0])[1]));cy,sy=math.cos(yw),math.sin(yw)
                loc=[(q['position'] if isinstance(q,dict) else q) for q in (path or {}).get('points') or []]   # local to the path object (Y rotation applied, like three.js)
                pts=[(pp[0]+q[0]*cy+q[2]*sy,pp[2]-q[0]*sy+q[2]*cy) for q in loc]
                if len(pts)<2:warnings.append(f"{o.get('name','Boss')}: movement 'Patrol a path' has no path with 2+ points - it keeps its own AI.");continue
            else:
                a=(o.get('boss') or {}).get('arena') or {};c=a.get('centre') or o['position'];r=float(mv.get('radius') or (float(a['radius'])*.7 if a.get('radius') else 600))
                pts=[(c[0]+r*math.cos(2*math.pi*k/16),c[2]+r*math.sin(2*math.pi*k/16)) for k in range(16)]
            laps.append(f"{int(o['type']):02X} {n(mv.get('speed') or 300)} "+' '.join(f'{n(round(x,1))} {n(round(z,1))}' for x,z in pts))
        if laps:atomic(build/'mods/boss_path.txt',''.join(x+chr(10) for x in laps).encode());warnings.append(f"Boss path: {len(laps)} written to mods/boss_path.txt (Egg Breaker walks it between attacks; other bosses: not yet).")
        elif (build/'mods/boss_path.txt').exists():(build/'mods/boss_path.txt').unlink()
        atomic(build/'build-report.json',json_bytes(report));atomic(d/'latest-build.json',json_bytes(report));return report
    except Exception as e:
        shutil.rmtree(build,ignore_errors=True)   # never leave a half-written pack behind
        return dict(ok=False,errors=[str(e)],warnings=warnings)

# ---- lighting L2: baked occupancy (docs/lighting/OCCUPANCY-FORMAT.md, tools/stage_editor/lighting_bake.py). A build with
# project.lightBake = true, or GET /api/lightbake?id= (bakes the project's latest build), writes mods/lighting/stgNNNN.occ
# from the BUILD's chunk archives and BDT (authored geometry, stage-art edits, moved chunk boxes included) plus the disc's
# untouched chunks. Off by default, so ordinary builds stay byte-identical.
def light_bake_build(stage,build):
    import lighting_bake
    data,r=lighting_bake.bake_stage(stage,override=Path(build)/'mods/files'/f'stg{stage:04d}',project=True)
    atomic(Path(build)/f'mods/lighting/stg{stage:04d}.occ',data)
    return dict(file=f'mods/lighting/stg{stage:04d}.occ',bytes=r['bytes'],rawBytes=r['rawBytes'],chunks=r['chunks'],voxels=r['voxels'],seconds=r['seconds'],detail=r['detail'])
def light_bake(pid):
    d=project_dir(pid);rp=d/'latest-build.json'
    if not rp.exists():raise ValueError('Lighting bake needs a successful Build first.')
    info=json.loads(rp.read_text())
    if not info.get('ok') or not Path(info.get('path','')).is_dir():raise ValueError('Lighting bake needs a successful Build first (the last build failed).')
    info['lightBake']=light_bake_build(load_project(pid)['stage'],info['path'])
    atomic(Path(info['path'])/'build-report.json',json_bytes(info));atomic(rp,json_bytes(info))
    return dict(ok=True,path=info['path'],**info['lightBake'])

# ---- in-game look (UI 2 toggle): GET /api/atmosphere?stage=N -> what the editor needs to approximate the game's
# atmosphere. Decoded from 4 stages (100 / 201 / 300 / 604), field meanings marked 'probable' are NOT proven in game.
# stgNNNN.fog = 24 entries x 40 bytes (little-endian): u32[4] chunk bit mask (bit n = chunk n), f32 1.0 (probably near),
#   f32 end distance, u32 enabled (1 = on), u32 extra (0; 300 has 1.0f), f32 start distance, colour bytes r,g,b,a(0).
#   Westopolis: 4 entries covering chunks 1-19 + 21, start 3500-8000, end 30000, colour (150, 50, 0).
# stgNNNN_light.bin = 4 sets x 4 lights x 52 bytes: f32 ambient rgba, f32 colour rgba, f32 vector (0,0,1 everywhere),
#   i32 angle a, i32 angle b (degrees; which is pitch / yaw is not proven). Set 0 light 0 is the stage's main light
#   (100: ambient (0.20,0.20,0.29), colour (1,0.59,0.59), angles 70 / 90). What selects sets 1-3 (characters /
#   objects / cutscenes?) is not proven.
# Sky: backdrop chunks (every visual world wider than 14000, import_stage) + Trigger Skybox objects 00/59 (DrawMode,
#   Radius, Chunk_1..4: force those chunks on / off inside the radius). Backdrops are ordinary world geometry: no file
#   field makes them follow the camera (not proven in game). No clear-colour file is known (set in code).
def _profile_rules():
    path=LIGHT_ROOT/'lighting_profiles.ini';out=[];cur=None
    if not path.exists():return out
    for line in path.read_text('utf-8',errors='replace').splitlines():
        line=line.strip()
        if not line or line[0] in ';#':continue
        if line.startswith('[') and line.endswith(']'):cur=dict(name=line[1:-1],keys={});out.append(cur);continue
        if cur is not None and '=' in line:
            k,v=line.split('=',1);v=v.strip()
            try:val=[float(x) for x in v.split(',')] if ',' in v else float(v)
            except ValueError:val=v
            cur['keys'][k.strip()]=val
    return out
def atmosphere(stage):
    src=stage_dir(stage);out=dict(stage=stage,proven=False,notes=[])
    fog=[];f=src/f'stg{stage:04d}.fog'
    if f.exists():
        b=f.read_bytes()
        for i in range(len(b)//40):
            w=b[i*40:i*40+40]
            if not w.strip(b'\0'):continue
            mask=int.from_bytes(w[0:16],'little');near,end,enabled=struct.unpack_from('<ffI',w,16);extra=w[28:32].hex();start=struct.unpack_from('<f',w,32)[0]
            fog.append(dict(index=i,chunks=[n for n in range(128) if mask>>n&1],enabled=bool(enabled),start=start,end=end,near=near,
              colour=list(w[36:39]),colourHex='#'+w[36:39].hex(),extra=extra))
    by_chunk={}
    for e in fog:
        if e['enabled']:
            for n in e['chunks']:by_chunk.setdefault(str(n),e['index'])
    out['fog']=dict(file=f.name if f.exists() else None,entries=fog,byChunk=by_chunk,
      meaning='probable: chunks = bit mask, start / end = linear fog distances, colour = r,g,b bytes; not proven in game')
    sets=[];f=src/f'stg{stage:04d}_light.bin'
    if f.exists():
        b=f.read_bytes()
        for si in range(len(b)//208):
            lights=[]
            for li in range(4):
                at=si*208+li*52;v=struct.unpack_from('<11f2i',b,at)
                lights.append(dict(ambient=[round(x,4) for x in v[0:4]],colour=[round(x,4) for x in v[4:8]],vector=[round(x,4) for x in v[8:11]],angles=list(v[11:13])))
            sets.append(lights)
    out['light']=dict(file=f.name if f.exists() else None,sets=sets,main=sets[0][0] if sets else None,
      meaning='probable: set 0 light 0 = stage light (ambient + directional colour, angles in degrees, order not proven)')
    p=import_stage(stage);mats=p['materials'];chunks=p['native']['chunks']
    triggers=[]
    for o in p['objects']:
        if o.get('kind')=='native' and (o['list'],o['type'])==(0,0x59):
            i0,mode,radius,*cs=struct.unpack_from('<IIf4i',base64.b64decode(o['params']))
            triggers.append(dict(id=o['id'],layer=o['layer'],position=o['position'],radius=radius,drawMode='force on' if mode else 'force off',chunks=[c for c in cs if c>0]))
    worlds={}
    for m in p['reference']:
        if m['collision'] or not m['vertices']:continue
        k=(m['chunk'],m['name']);w=worlds.setdefault(k,dict(chunk=m['chunk'],world=m['name'],meshes=[],materials=set(),lo=np.full(3,np.inf),hi=np.full(3,-np.inf)))
        v=np.asarray(m['vertices'],float);w['meshes'].append(m['id']);w['materials'].update(m['materials']);w['lo']=np.minimum(w['lo'],v.min(0));w['hi']=np.maximum(w['hi'],v.max(0))
    sky=[]
    for (num,name),w in sorted(worlds.items()):
        span=float(max(w['hi'][0]-w['lo'][0],w['hi'][2]-w['lo'][2]))
        if span<=14000:continue
        c=next((c for c in chunks if c['number']==num),{})
        sky.append(dict(chunk=num,world=name,backdropChunk=bool(c.get('backdrop')),span=round(span),bounds=[w['lo'].round(1).tolist(),w['hi'].round(1).tolist()],
          centre=((w['lo']+w['hi'])/2).round(1).tolist(),meshes=w['meshes'],boxes=c.get('boxes',[]),
          materials=[dict(id=mats[i]['id'],texture=mats[i].get('texture'),asset=mats[i].get('asset')) for i in sorted(w['materials'])]))
    out['sky']=dict(backdropChunks=[c['number'] for c in chunks if c.get('backdrop')],worlds=[w for w in sky if w['backdropChunk']],wideWorlds=[w for w in sky if not w['backdropChunk']],triggers=triggers,
      follow='world-fixed (no camera-follow field in any file; not proven in game)',scale=1.0,
      rule='a backdrop chunk is drawn while the player is inside one of its BDT boxes (#53) or a Trigger Skybox in range forces it on; draw it first, without depth writes, at its own world position (render3d.js referenceMesh does)')
    out['clearColour']=None;out['notes'].append('No clear / background colour file is known (the game sets it in code); the fog colour of the chunk the camera is in is the closest stand-in.')
    out['profiles']=[r for r in _profile_rules() if r['keys'].get('stage') in (0,0.0,float(stage))]
    gj=PROFILES/f'stg{stage:04d}.auto.json'   # tools/lighting/make_profiles.py
    if gj.exists():
        g=json.loads(gj.read_text());out['generated']=dict(rule=g['rule'],pending=g['pending'],regions=len(g['regions']),prelit=dict(target=g['prelit']['target'],p10=g['prelit']['p10']),
          calibration=g.get('calibration'),made=g.get('generated'),looks=g.get('looks'),lookDefault=g.get('lookDefault'),
          runtimeKeys=sorted(runtime_light_keys()))   # looks: the lighting card's Look picker (pending keys = not in runtimeKeys)
    else:out['generated']=None
    return out

# ---- lighting L4: project.lighting (UI2 spec 'Lighting') + Fade sun nodes -> mods/stages/stgNNNN.light.ini (rules in
# the lighting_profiles.ini format of runtime/lighting_environment.cpp: one [section] per region / profile / Fade sun) and
# mods/stages/stgNNNN.lights.json (point lights, for L3). Regions: bounds = position +- size*scale/2; follow 'player'
# writes player_bounds= (L1, runtime pending), 'camera' writes bounds=. Fade sun: trigger ws_fade_sun_<owner>_<node>
# (events.fade_sun_name, the same name UI 2's v3 action and the v4 node turn on), sun_strength, transition, priority 1000.
# The runtime does not read these files yet (: load them after lighting_profiles.ini, and the player_bounds key).
# Nothing is written for a project without lighting, so ordinary builds stay byte-identical.
LIGHT_KEYS=dict(sun_tint=(3,0,3),sky=(3,0,3),ground=(3,0,3),fog_colour=(3,0,3),sun_strength=(1,0,3),sky_strength=(1,0,3),
  fog_density=(1,0,0.02),fog_start=(1,0,60000),sun_azimuth=(1,0,360),sun_elevation=(1,2,90),
  exposure=(1,0.05,4),baked=(1,0,1),unlit_distance=(1,0,200000),
  # looks (2026-10-18, make_profiles.py LOOKS): names proposed to written as comments until the runtime parses them
  shadow_strength=(1,0,1),shadow_tint=(3,0,3),saturation=(1,0,2),world_shading_only=(1,0,1),characters_cast_shadows=(1,0,1))
LIGHT_LOOKS=('stylized','original','dramatic')
def runtime_light_keys():
    """keys runtime/lighting_environment.cpp parses today (an unknown key makes it reject the whole file)"""
    if 'runtime-light-keys' not in CACHE:
        src=ROOT/'runtime/lighting_environment.cpp'
        CACHE['runtime-light-keys']=set(re.findall(r'key == "(\w+)"',src.read_text('utf-8','replace'))) if src.exists() else set(LIGHT_KEYS)
    return CACHE['runtime-light-keys']
def light_line(k,v):
    return f'{k}={v}' if k in runtime_light_keys() else f'; {k}={v}   (pending runtime key)'
def _fade_sun_rules(p,warnings):
    import events as ev;out={}
    def walk(x):
        if isinstance(x,dict):
            if x.get('type')=='lighting' and str(x.get('name','')).startswith('ws_fade_sun_') and isinstance(x.get('profile'),dict):
                out[x['name']]=dict(strength=x['profile'].get('sun_strength',1),seconds=x.get('transition',2))
            for v in x.values():walk(v)
        elif isinstance(x,list):
            for v in x:walk(v)
    walk([p.get('triggers',[]),p.get('objects',[]),p.get('logic')])   # v3 actions UI 2 compiled (they carry profile / transition)
    owners=[(None,p.get('logic'))]+[(o.get('id'),o.get('logic')) for o in p.get('triggers',[])+p.get('objects',[])]
    for owner,g in owners:   # v4 graphs: the node's own values
        for n in (g or {}).get('nodes',[]) if isinstance(g,dict) else []:
            if n.get('type')!='action.fade_sun':continue
            v=n.get('values') or {};name=ev.fade_sun_name(owner,n.get('id'))
            if any(l.get('to')==n.get('id') and l.get('in') in ('strength','seconds') for l in g.get('links',[])):
                warnings.append(f'Fade sun {name}: a wired Strength / Seconds is not known at build time - the rule uses the node\'s own values.')
            out.setdefault(name,dict(strength=v.get('strength',0.3),seconds=v.get('seconds',2)))
    return out
# Generated stage profile (tools/lighting/make_profiles.py -> lighting/profiles/stgNNNN.auto.ini, MATCHING-THE-ORIGINAL.md):
# every build writes it FIRST into stgNNNN.light.ini at priority -1, so hand rules (project.lighting regions / profiles,
# priority >= 0) win; project.lighting.auto = {enabled: false} leaves it out; auto.overrides = {key: value} (the lighting
# card's sliders) replace keys of the generated stage rule; 'reset to auto' = delete the key. Mode 'new' stages get the
# stage rule only (the slot's per-chunk fog regions describe the original art).
def generated_lighting(p,warnings):
    A=(p.get('lighting') or {}).get('auto') or {}
    if A.get('enabled') is False:return [],0
    f=PROFILES/f'stg{p["stage"]:04d}.auto.ini'
    if not f.exists():return [],0
    over={}
    # look (make_profiles.py): project.lighting.look picks one of the generated key sets (default = the file's own)
    look=(p.get('lighting') or {}).get('look')
    if look is not None:
        if look not in LIGHT_LOOKS:raise ValueError(f"Lighting look {look!r}: one of {', '.join(LIGHT_LOOKS)}")
        jf=f.with_suffix('').with_suffix('.json') if False else PROFILES/f'stg{p["stage"]:04d}.auto.json'
        lk=(json.loads(jf.read_text('utf-8')).get('looks') or {}).get(look) if jf.exists() else None
        if lk is None:warnings.append(f'Lighting look {look}: the generated profile of stage {p["stage"]} has no looks yet (run tools/lighting/make_profiles.py) - default used.')
        for k,v in (lk or {}).items():
            if k in LIGHT_KEYS:over[k]=','.join(f'{float(x):g}' for x in (v if isinstance(v,list) else [v]))
    for k,v in (A.get('overrides') or {}).items():
        if k not in LIGHT_KEYS:warnings.append(f'Lighting auto override {k}: not a lighting rule key - left out.');continue
        n,lo,hi=LIGHT_KEYS[k];vals=v if isinstance(v,list) else [v]
        if len(vals)!=n or not all(isinstance(x,(int,float)) and math.isfinite(x) for x in vals):raise ValueError(f'Lighting auto override {k}: needs {n} number(s)')
        over[k]=','.join(f'{min(hi,max(lo,float(x))):g}' for x in vals)
    out=[];section=0;skip=False;seen=set()
    for line in f.read_text('utf-8').splitlines():
        s=line.strip()
        if s.startswith('[') and s.endswith(']'):
            section+=1;skip=section>1 and p.get('mode')=='new'
            if section==2 and over:out+=[light_line(k,v) for k,v in over.items() if k not in seen]
        if skip:continue
        if s.startswith('priority='):line='priority=-1'
        k=s.split('=',1)[0]
        if section==1 and k.lstrip('; ') in over and k.startswith(';'):continue   # a pending-key comment the look / override restates
        if section==1 and k in over:line=light_line(k,over[k]);seen.add(k)
        out.append(line)
    if section==1 and over:out+=[light_line(k,v) for k,v in over.items() if k not in seen]
    rules=sum(1 for l in out if l.strip().startswith('['))
    return out+[''],rules
def build_lighting(p,build,warnings):
    L=p.get('lighting') or {};fades=_fade_sun_rules(p,warnings);gen,gen_rules=generated_lighting(p,warnings)
    if not (L.get('regions') or L.get('profiles') or L.get('lights') or fades or gen):return None
    num=lambda x,lo,hi:min(hi,max(lo,float(x)))
    used=set()
    def section(name):
        base=re.sub(r'[\[\];#=\r\n]','',str(name or 'rule')).strip()[:60] or 'rule';n=base;i=2
        while n.lower() in used:n=f'{base} {i}';i+=1
        used.add(n.lower());return n
    def keys(prof,where):
        out=[]
        for k,v in (prof or {}).items():
            if k not in LIGHT_KEYS:warnings.append(f'Lighting {where}: key {k} is not a lighting rule key - left out.');continue
            n,lo,hi=LIGHT_KEYS[k];vals=v if isinstance(v,list) else [v]
            if len(vals)!=n or not all(isinstance(x,(int,float)) and math.isfinite(x) for x in vals):raise ValueError(f'Lighting {where}: {k} needs {n} number(s)')
            out.append(light_line(k,','.join(f'{num(x,lo,hi):g}' for x in vals)))
        return out
    lines=[f'; Stage Workshop lighting rules for stage {p["stage"]} (project {p.get("name","")}); format = lighting_profiles.ini','']+gen
    rules=0;player=0
    for r in L.get('regions',[]):
        pos=np.asarray(r.get('position',[0,0,0]),float);half=np.abs(np.asarray(r.get('size',[0,0,0]),float)*np.asarray(r.get('scale',[1,1,1]),float))/2
        if not np.isfinite(pos).all() or not np.isfinite(half).all() or not (half>0).all():raise ValueError(f"Lighting region {r.get('name')}: needs a size > 0 on every axis")
        lo,hi=pos-half,pos+half;b=','.join(f'{x:g}' for x in np.round(np.concatenate([lo,hi]),2))
        follow=r.get('follow','player');body=keys(r.get('profile'),f"region {r.get('name')}")
        if not body:warnings.append(f"Lighting region {r.get('name')}: changes nothing - left out.");continue
        lines+=[f"[{section(r.get('name'))}]",f"stage={p['stage']}",f"priority={int(num(r.get('priority',0),-1000,1000))}",f"transition={num(r.get('transition',1.5),0,30):g}"]
        if r.get('trigger'):lines.append(f"trigger={r['trigger']}")
        lines.append(('player_bounds=' if follow!='camera' else 'bounds=')+b);player+=follow!='camera'
        lines+=body+[''];rules+=1
    for r in L.get('profiles',[]):
        body=keys(r.get('profile'),f"profile {r.get('name')}")
        if not body or not r.get('trigger'):warnings.append(f"Lighting profile {r.get('name')}: needs a trigger and at least one value - left out.");continue
        lines+=[f"[{section(r.get('name'))}]",f"stage={p['stage']}",f"priority={int(num(r.get('priority',0),-1000,1000))}",f"transition={num(r.get('transition',1.5),0,30):g}",f"trigger={r['trigger']}"]+body+[''];rules+=1
    for name,f in sorted(fades.items()):
        lines+=[f'[{section(name)}]',f"stage={p['stage']}",'priority=1000',f"transition={num(f['seconds'],0,30):g}",f'trigger={name}',f"sun_strength={num(f['strength'],0,3):g}",''];rules+=1
    out=dict(rules=rules,generatedRules=gen_rules,autoOverrides=sorted(((L.get('auto') or {}).get('overrides') or {}).keys()),playerRegions=player,fadeSun=len(fades),lights=0)
    if rules+gen_rules>64:raise ValueError(f'Lighting: {rules+gen_rules} rules - the runtime reads at most 64 per file')
    rules+=gen_rules
    if rules:
        atomic(build/f'mods/stages/stg{p["stage"]:04d}.light.ini','\r\n'.join(lines).encode('utf-8'));out['file']=f'mods/stages/stg{p["stage"]:04d}.light.ini'
        warnings.append(f"Lighting: {rules} rule(s) written to {out['file']} ({player} player region(s) use player_bounds); the game does not read this file yet (runtime pending, track A).")
    lights=[]
    for l in L.get('lights',[]):
        pos=[float(x) for x in l.get('position',[0,0,0])];col=[num(x,0,1) for x in l.get('colour',[1,1,1])]
        if float(l.get('radius',0))<=0:warnings.append(f"Light {l.get('name')}: no radius - left out.");continue
        lights.append(dict(name=str(l.get('name','light')),position=pos,colour=col,intensity=num(l.get('intensity',1),0,20),radius=float(l['radius'])))
    if lights:
        atomic(build/f'mods/stages/stg{p["stage"]:04d}.lights.json',json_bytes(dict(version=1,stage=p['stage'],lights=lights)));out['lights']=len(lights)
        warnings.append(f'Lighting: {len(lights)} point light(s) written to mods/stages/stg{p["stage"]:04d}.lights.json (for L3; not read by the game yet).')
    return out

# ---- stage identity files (docs/stage-workshop/research/stage-identity-music-camera-sky.md): the game picks the fog,
# light, start-camera and music files by the stage NUMBER (fn_8016A200 / fn_80048710 / fn_801C839C / fn_801D3B08), so a
# build changes them by writing this slot's file name into mods/files. project.stageFiles = {fog: <stage>, light:
# <stage>, cam: <stage> | 'none', music: <stage> | {asset: <project music asset id or file>}}. cam 'none' (the default
# for mode 'new') = this slot's own camera regions moved 1e6 units up, so none can hold the player (#81: a new stage in
# slot 100 kept Westopolis' opening-drop camera regions around the origin -> a locked camera). Music must be ADX (no
# encoder here). The clear (background) colour is set in code - not supported (research file).
MUSIC_EXCEPTIONS={411:'sng_stg0310.adx',412:'sng_stg0210.adx',511:'sng_stg0310.adx',613:'sng_stg0611.adx',614:'sng_stg0612.adx',
  615:'sng_stg0612.adx',616:'sng_stg0610.adx',617:'sng_stg0610.adx',618:'sng_stg0611.adx',800:'sng_stg0800.adx',801:'sng_stg0800.adx',802:'sng_stg0800.adx'}
def music_file(stage):return MUSIC_EXCEPTIONS.get(stage,f'sng_stg{stage:04d}.adx')   # lbl_8053CD58 rows -> stages
# #100 every music file on the disc, with a friendly name and a group for the Stage panel's Music dropdown
JINGLES={'awakedark':'Dark power awakens','awakehero':'Hero power awakens','cblast_e':'Chaos Blast','ccslow':'Chaos Control (slow)','ccslow_e':'Chaos Control (slow, alt)',
  'cctrl':'Chaos Control','cctrl_e':'Chaos Control (alt)','invincible':'Invincibility','roundclear':'Stage clear'}
def music_tracks():
    if 'music-tracks' in CACHE:return CACHE['music-tracks']
    out=[]
    for f in sorted((DISC/'files').glob('sng_*.adx')):
        n=f.stem[4:];m=re.fullmatch(r'stg(\d{4})(b?)',n)
        if m:
            st=int(m.group(1));users=[k for k in STAGE_NAMES if music_file(k)==f.name]
            name=STAGE_NAMES.get(st,f'Stage {st}')+(' (part 2)' if m.group(2) else '')
            if st==800:name='2P battle'
            out.append(dict(file=f.name,name=name.replace('Boss: ',''),group='Boss music' if name.startswith('Boss') else 'Stage music',stages=users or [st]))
        elif n.startswith('jin_'):out.append(dict(file=f.name,name=JINGLES.get(n[4:],n[4:]),group='Jingles (short)'))
        elif n.startswith('sys'):out.append(dict(file=f.name,name=f'Menu music {int(n[3:])}',group='Menus'))
        elif n.startswith('vox'):out.append(dict(file=f.name,name=f'Vocal song {int(n[3:])}',group='Vocal songs'))
        elif n.startswith('Battle'):out.append(dict(file=f.name,name=f'2P battle {n[6:]}',group='Menus'))
        else:out.append(dict(file=f.name,name='Cutscene '+n.replace('_E','').replace('EV_music','theme ').replace('EV','').strip(),group='Cutscene music'))
    order=['Stage music','Boss music','Vocal songs','Menus','Jingles (short)','Cutscene music']
    out.sort(key=lambda t:(order.index(t['group']),t.get('stages',[0])[0] if t['group'] in ('Stage music','Boss music') else 0,t['name']))
    CACHE['music-tracks']=out;return out
def song_wav(f):
    """(the decoded WAV next to a non-WAV song asset, the decoder's name): made once with tools/audio/adx.song_to_wav"""
    sys.path.insert(0,str(APP.parent/'audio'));import adx
    w=f.with_name(f.stem+'.decoded.wav')
    if not w.exists():
        tmp=w.with_name(w.name+'.tmp.wav');how=adx.song_to_wav(f,tmp);tmp.replace(w);w.with_name(f.stem+'.decoder.txt').write_text(how)
    d=w.with_name(f.stem+'.decoder.txt');return w,(d.read_text() if d.exists() else '?')
def music_preview(track=None,pid=None,asset_file=None,seconds=20):
    """a WAV of the first seconds of a disc ADX or of a project's music asset (the editor plays it)"""
    sys.path.insert(0,str(APP.parent/'audio'));import adx
    if track:
        if track not in {t['file'] for t in music_tracks()}:raise ValueError('no such track')
        x,r=adx.decode((DISC/'files'/track).read_bytes(),seconds)
    else:
        f=project_asset(pid,asset_file);data=f.read_bytes()
        if data[:2]!=b'\x80\x00' and not adx.is_wav(f):x,r=adx.read_wav(song_wav(f)[0]);x=x[:int(r*seconds)]   # MP3 / M4A / FLAC ...: the decoded copy
        elif data[:2]!=b'\x80\x00':return data
        else:x,r=adx.decode(data,seconds)
    bio=io.BytesIO();adx.write_wav(bio,x,r);return bio.getvalue()
def music_bytes(p,mu):
    """the ADX for stageFiles.music = stage | {track} | 'none' | {asset} (a WAV is converted and cached next to it)"""
    sys.path.insert(0,str(APP.parent/'audio'));import adx
    if mu=='none':return adx.encode(adx.np.zeros((48000*2,2))),'silence'
    if isinstance(mu,dict) and mu.get('track'):
        f=DISC/'files'/str(mu['track'])
        if not re.fullmatch(r'sng_[A-Za-z0-9_]+\.adx',str(mu['track'])) or not f.exists():raise ValueError(f"Music: no game track {mu['track']}")
        return f.read_bytes(),mu['track']
    if isinstance(mu,dict):
        a=next((x for x in p.get('customAssets',[]) if mu.get('asset') in (x.get('id'),x.get('file'))),None)
        if not a:raise ValueError('Music: the chosen file is not in this project any more (pick another)')
        f=project_asset(p['id'],a['file']);data=f.read_bytes();fmt=str(a.get('format') or f.suffix[1:]).lower()
        if data[:2]==b'\x80\x00':return data,a.get('name')
        ls=float(mu.get('loopStart') or 0);cache=f.with_name(f'{f.stem}.loop{int(ls*1000)}.adx')
        if not cache.exists():
            try:w=f if adx.is_wav(f) else song_wav(f)[0]
            except Exception as e:raise ValueError(f"Music: {a.get('name')} ({fmt.upper()}) could not be decoded on this PC ({e}). Export the song as WAV.")
            atomic(cache,adx.wav_to_adx(w,True,ls))
        return cache.read_bytes(),f"{a.get('name')} (converted)"
    srcf=DISC/'files'/music_file(int(mu))
    if not srcf.exists():raise ValueError(f'Music: stage {mu} has no {srcf.name}')
    return srcf.read_bytes(),STAGE_NAMES.get(int(mu),int(mu))
def neutral_cam(data,stage):
    """stgNNNN_cam.dat (header u32 version 3, date, stage, 2 more; then 220-byte records = 6 u32 + 49 f32: region centre at
    f[3:6], size f[9:12], camera f[12:15], target f[15:18]) with every region / camera moved up 1e6 units"""
    b=bytearray(data);struct.pack_into('<I',b,8,stage);o=0x14
    while o+220<=len(b):
        for k in (3,12,15):
            at=o+24+4*(k+1);struct.pack_into('<f',b,at,struct.unpack_from('<f',b,at)[0]+1e6)
        o+=220
    return bytes(b)
def build_stage_files(p,out,warnings):
    sf=dict(p.get('stageFiles') or {});rep=[];st=p['stage']
    if p.get('mode')=='new' and 'cam' not in sf:sf['cam']='none'
    for kind,suffix in (('fog','.fog'),('light','_light.bin'),('cam','_cam.dat')):
        s_=sf.get(kind)
        if s_ in (None,'',st):continue
        if kind=='cam' and s_=='none':
            srcf=stage_dir(st)/f'stg{st:04d}_cam.dat'
            if not srcf.exists():continue
            atomic(out/srcf.name,neutral_cam(srcf.read_bytes(),st));rep.append(dict(kind='cam',source='none',file=srcf.name))
            warnings.append(f'Stage camera: {srcf.name} regions moved out of reach (the game camera follows the player everywhere).');continue
        srcf=stage_dir(int(s_))/f'stg{int(s_):04d}{suffix}'
        if not srcf.exists():raise ValueError(f'stageFiles: stage {s_} has no {srcf.name}')
        data=srcf.read_bytes()
        if kind=='cam':data=bytearray(data);struct.pack_into('<I',data,8,st);data=bytes(data)
        atomic(out/f'stg{st:04d}{suffix}',data);rep.append(dict(kind=kind,source=int(s_),file=f'stg{st:04d}{suffix}'))
        warnings.append(f'Stage {kind}: {srcf.name} from stage {s_}.')
    mu=sf.get('music')
    if mu not in (None,'',st):
        target=out.parent/music_file(st);data,src=music_bytes(p,mu)
        atomic(target,data);rep.append(dict(kind='music',source=src,file=target.name))
        warnings.append(f'Stage music: {target.name} = {src} ({len(data)//1024} KiB).')
    sb=sf.get('sound')   # stage sound-effect bank: 'auto' (default) = this slot's bank + every placed boss's own bank (sound_banks.py) | <stage> = that stage's bank instead | 'none'
    if sb in (None,'','auto') and st in SOUND_BANK:
        need=[]
        for o in p['objects']:
            if o.get('kind')!='native' or o.get('list')!=0 or not 0xB4<=int(o.get('type',0))<=0xBE or o.get('sourceStage') in (None,st):continue
            b=SOUND_BANK.get(int(o['sourceStage']))
            if b and b!=SOUND_BANK[st] and b not in need:need.append(b)
        if need:
            import sound_banks
            snd=DISC/'files'/'sound';dst=out.parent/'sound';extra=[];sizes={};lines=[];tabs=dict(need)
            kept,left,used=sound_banks.fit_banks([m for m,_ in need],{m:sound_banks.aram_bytes((snd/m).stat().st_size) for m,_ in need})
            if left:warnings.append(f"Stage sound banks: not enough sound memory (ARAM) for every boss family - left out: {', '.join(left)} (their sounds stay silent).")
            need=[(m,tabs[m]) for _,m in kept]
            for bank,(mlt,tab) in zip(sound_banks.EXTRA_BANKS,need):
                raw=sound_banks.renumber_mlt((snd/mlt).read_bytes(),bank);atomic(dst/mlt,raw)
                sizes[bank]=sound_banks.aram_bytes(len(raw));extra.append((bank,(snd/tab).read_bytes()));lines.append(f'{bank} sound/{mlt} {sizes[bank]}')
            merged,added=sound_banks.merged_table((snd/SOUND_BANK[st][1]).read_bytes(),extra)
            atomic(dst/SOUND_BANK[st][1],merged)
            atomic(out.parent.parent/'sound_banks.txt',''.join(x+chr(10) for x in lines).encode())
            rep.append(dict(kind='sound',banks=lines,added=len(added)))
            warnings.append(f"Stage sound banks: this slot's {SOUND_BANK[st][0]} kept + {', '.join(f'{m} as bank {b}' for b,(m,_) in zip(sound_banks.EXTRA_BANKS,need))} ({len(added)} boss sound ids added to {SOUND_BANK[st][1]}; {used//1024} of {(sound_banks.ARAM_REST-sound_banks.ARAM_MARGIN)//1024} KiB of free sound memory used).")
        sb=None
    if sb not in (None,'',st,'none','auto') and int(sb) in SOUND_BANK and st in SOUND_BANK:
        (smlt,sbin),(dmlt,dbin)=SOUND_BANK[int(sb)],SOUND_BANK[st]
        for s_,d_ in ((smlt,dmlt),(sbin,dbin)):
            srcf=DISC/'files'/'sound'/s_
            if srcf.exists():atomic(out.parent/'sound'/d_,srcf.read_bytes())
        rep.append(dict(kind='sound',source=int(sb),file=f'sound/{dmlt}'))
        warnings.append(f"Stage sound bank: {smlt} / {sbin} (stage {sb}) under this slot's names {dmlt} / {dbin} - the boss's sounds play; this slot's own stage-object sounds ({dmlt}) are replaced.")
    return rep
# the game picks the stage sound-effect bank by stage number (tables at 8053E1B8 mlt / 8053CF20 AudioData, read from the DOL)
_SB={'B71_CITY':'City','B72_ELEC':'Elec','B73_NAZCA':'Canyon','B74_PARK':'Park','B75_JUNGLE':'Jungle','B76_ARK2':'Ark2','B77_ARK1':'Ark1','B78_BASE':'Base',
  'B79_DOOM':'Doom','B7B_BKBOSS':'BKBoss','B7C_EGGBOSS':'EggBoss','B7D_GUNBOSS':'GunBoss','B7E_LASTGUN':'LastGunBoss','B7F_LASTEGG':'LastEggBoss',
  'B80_LASTBK':'LastBKBoss','B81_FINALDOOM':'FinalBoss'}
SOUND_BANK={s:(m+'.mlt',f'AudioDataStage{_SB[m]}.bin') for m,ss in {'B71_CITY':[100,202,400],'B72_ELEC':[200,403],'B73_NAZCA':[201,402],
  'B74_PARK':[300,302],'B75_JUNGLE':[301,404,502],'B76_ARK2':[401,504],'B77_ARK1':[500,503,603],'B78_BASE':[501,600,602,800,801,802],
  'B79_DOOM':[601,604,700],'B7B_BKBOSS':[210,412],'B7C_EGGBOSS':[310,411,511],'B7D_GUNBOSS':[410,510],'B7E_LASTGUN':[611,613,618],
  'B7F_LASTEGG':[612,614,615],'B80_LASTBK':[610,616,617],'B81_FINALDOOM':[710]}.items() for s in ss}

# ---- enemy path presets (2026-10-17, research/appear-types.md): flyers with Behaviour 'Fly a preset path' fly one of 9
# shapes in enemy/EnemyPath.one (PathType 0..8), placed at the enemy and turned with it. project.enemyPaths =
# {"<PathType>": {points: [[x, y, z], ...] LOCAL to the enemy (forward = -Z), closed?}} replaces those presets: the build
# writes mods/files/enemy/EnemyPath.one. The file is shared, so the change applies to every flyer using that PathType in
# stages played with this mod (no per-enemy shapes: PathType 9+ needs game code). GET /api/enemy-paths = the presets.
def enemy_path_presets():
    import paths as P_
    files=one.read_all((DISC/'files/enemy/EnemyPath.one').read_bytes())
    return [dict(pathType=k,file=n,**{x:v for x,v in P_.read_ptb(files[n]).items() if x in ('points','length','closed','settings')}) for k,n in enumerate(P_.PTB_FILES)]
def build_enemy_paths(p,build,warnings):
    ep=p.get('enemyPaths') or {}
    if not ep:return None
    import paths as P_
    src=DISC/'files/enemy/EnemyPath.one';files=one.read_all(src.read_bytes());reps={};out=[]
    for key,e in ep.items():
        k=int(key)
        if not 0<=k<len(P_.PTB_FILES):raise ValueError(f'enemyPaths: PathType {key} is not 0..8')
        name=P_.PTB_FILES[k];orig=P_.read_ptb(files[name]);pts=e.get('points')
        settings=(orig['settings']|2) if e.get('closed',orig['closed']) else (orig['settings']&~2)
        reps[name]=P_.write_ptb(pts,settings,orig['name'],orig['flags'] if len(pts)==len(orig['points']) else None,orig['rotations'])
        out.append(dict(pathType=k,file=name,points=len(pts)))
    atomic(build/'mods/files/enemy/EnemyPath.one',one.pack(str(src),reps))
    warnings.append(f"Enemy paths: {len(out)} preset(s) replaced in enemy/EnemyPath.one (PathType {', '.join(str(o['pathType']) for o in out)}) - every flyer using them in this mod flies the new shape.")
    return out

# Test from here (2026-10-18): the editor tests the SAME game the user plays. Before a playtest the private runtime
# folder (work/stage-editor-runtime/game) takes play\'s shadow.exe set when play\'s is newer (the old exe kept as
# shadow.exe.before-play-<time>); play\ is only read, never written (its saves and settings stay the user's). as_played:
# the test game uses a copy of the user's play\shadow_settings.ini (lighting, look, volume ...) in a window, plus the
# lighting files it needs, with sound and focus; its own memory card (the runtime's saves) as always.
PLAY_SET=['shadow.exe','SDL3.dll','webgpu_dawn.dll','dxcompiler.dll','dxil.dll','libpng16.dll','zlib1.dll']
def refresh_runtime():
    play,run=PLAY,DATA/'work/stage-editor-runtime/game'
    if not (play/'shadow.exe').is_file() or not (run/'shadow.exe').is_file():return None
    if (play/'shadow.exe').stat().st_mtime<=(run/'shadow.exe').stat().st_mtime+1:return None
    shutil.copy2(run/'shadow.exe',run/f"shadow.exe.before-play-{time.strftime('%Y%m%d-%H%M%S')}")
    for f in PLAY_SET+['lighting_profiles.ini','movement_presets.ini']:
        if (play/f).is_file():shutil.copy2(play/f,run/f)
    if (play/'lighting').is_dir():shutil.copytree(play/'lighting',run/'lighting',dirs_exist_ok=True)
    return f"Editor runtime updated from play\\shadow.exe ({time.strftime('%Y-%m-%d %H:%M',time.localtime((play/'shadow.exe').stat().st_mtime))})"
def set_object_cap():
    """#158: SET objects the player's game holds per stage (shadow_settings.ini, read only): set_objects 2,048 / 4,096 / 8,192,
    only with original_logic=0 ('Original game logic' on, the default, keeps the original 2,048)"""
    try:
        ini={k:v for k,v in (l.split('=',1) for l in (PLAY/'shadow_settings.ini').read_text('utf-8','replace').splitlines() if '=' in l)}
        v=int(ini.get('set_objects','0')) if ini.get('original_logic','1').strip()=='0' else 0
    except (OSError,ValueError):v=0
    return 8192 if v>=2 else 4096 if v==1 else 2048
def as_played_settings(game):
    """the user's play\\shadow_settings.ini (read only) for a test game: in a window, its own textures folder"""
    try:lines=(PLAY/'shadow_settings.ini').read_text('utf-8','replace').splitlines()
    except OSError:lines=[]
    vals={k:v for k,v in (l.split('=',1) for l in lines if '=' in l)}
    vals.update(fullscreen='0',custom_textures='1',custom_textures_dir=str(game/'textures/Custom'))
    if not vals.get('width') or vals.get('width')=='0':vals.update(width='1280',height='720')
    return '\n'.join(f'{k}={v}' for k,v in vals.items())+'\n'
def playtest_folder(d,build_name):
    """#154 disk: ONE test game folder per project (playtests\\game), updated in place by every Playtest / --prepare-project
    (it was a new ~100 MB copy per build, never removed). The build's mods \\ textures are replaced whole (no stale
    overrides); cache, saves, stats and logs stay. If that game is still running (its shadow.exe cannot be deleted): the
    old way, a folder named after the build."""
    g=d/'playtests'/'game'
    if g.is_dir():
        try:
            if (g/'shadow.exe').exists():(g/'shadow.exe').unlink()
        except OSError:
            g=d/'playtests'/build_name;g.mkdir(parents=True,exist_ok=True);return g
        for sub in ('mods','textures'):shutil.rmtree(g/sub,ignore_errors=True)
    g.mkdir(parents=True,exist_ok=True);os.utime(g)   # newest-folder lookups (playtest log, agent scripts) pick it
    return g
def playtest(pid,prepare_only=False,build_path=None,windowed=False,as_played=False):
    d=project_dir(pid);p=load_project(pid)
    report=d/'latest-build.json'
    if build_path:   # the build the editor just made (must be one of this project's successful builds)
        bp=Path(build_path).resolve()
        if not bp.is_relative_to((d/'builds').resolve()) or not (bp/'build-report.json').exists():raise ValueError('Playtest: that build is not a finished build of this project')
        report=bp/'build-report.json'
    if not report.exists():raise ValueError('Playtest needs a successful Build first (no finished build of this project yet).')
    info=json.loads(report.read_text())
    if not info.get('ok') or not info.get('path') or not Path(info['path']).is_dir():raise ValueError('Playtest needs a successful Build first (the last build failed - see the build messages).')
    # Playtest again = replace this project's own previous test game (its private copy only; never play\shadow.exe)
    old=PROCESSES.get(pid)
    if old and old.poll() is None:
        old.terminate()
        try:old.wait(timeout=5)
        except subprocess.TimeoutExpired:old.kill();old.wait(timeout=5)
    # Last validated lighting executable, explicitly lighting OFF. Parked candidate never used.
    source=DATA/'work/stage-editor-runtime/game'
    if not (source/'shadow.exe').exists() and (PLAY/'shadow.exe').exists():source=PLAY   # a player's Stage Forge: their own game (only read and copied)
    try:refreshed=refresh_runtime()
    except OSError as e:refreshed=f'Editor runtime not updated from play ({e})'
    if not (source/'shadow.exe').exists():raise ValueError('Stage Workshop runtime build is not ready yet')
    game=playtest_folder(d,Path(info['path']).name)   # only this build's mods / textures in it (no stale overrides)
    for file in ['shadow.exe','SDL3.dll','webgpu_dawn.dll','dxcompiler.dll','dxil.dll','libpng16.dll','zlib1.dll']:shutil.copy2(source/file,game/file)
    shutil.copytree(Path(info['path'])/'mods',game/'mods',dirs_exist_ok=True)
    if (Path(info['path'])/'textures').exists():shutil.copytree(Path(info['path'])/'textures',game/'textures',dirs_exist_ok=True)
    shutil.copytree(source/'saves',game/'saves',dirs_exist_ok=True)
    (game/'cache').mkdir(exist_ok=True)
    if as_played and not windowed:   # Test from here: the user's settings + the lighting files they need
        for f in ['lighting_profiles.ini','movement_presets.ini']:
            if (source/f).is_file():shutil.copy2(source/f,game/f)
        if (source/'lighting').is_dir():shutil.copytree(source/'lighting',game/'lighting',dirs_exist_ok=True)
        (game/'shadow_settings.ini').write_text(as_played_settings(game))
    else:(game/'shadow_settings.ini').write_text('light=0\nwidth=1280\nheight=720\nfullscreen=0\nfps_target=60\nwidescreen=1\nvol_master='+('0' if windowed else '60')+'\ncustom_textures=1\ncustom_textures_dir='+str(game/'textures/Custom')+'\n')
    if prepare_only:return dict(path=str(game),stage=p['stage'],disc=str(DISC))
    env=os.environ.copy();env['SHADOW_CACHE_DIR']=str(game/'cache');env['SHADOW_NO_LAUNCHER']='1';env.pop('INPUT_FILE',None)
    # Always a normal, controllable window: an agent's quiet test server once served the user's editor and their
    # playtest ignored the pad. Agents test with agents/claude/scripts/wstest.ps1 (--prepare-project + their own --quiet launch).
    args,extra,si=playtest_launch(game,p['stage'],windowed);env.update(extra)
    log=open(game/'game.log','w');proc=subprocess.Popen(args,cwd=game,env=env,stdout=log,stderr=log,startupinfo=si)
    log.close();PROCESSES[pid]=proc;LIVE_VIEW[pid]=dict(game=str(game),windowed=bool(windowed),started=time.time(),seq=0)
    # A game that dies straight away (missing DLL, bad pack, crash at boot) must not look like a silent success.
    try:
        code=proc.wait(timeout=4)
        tail=(game/'game.log').read_text('utf-8','replace').splitlines()[-12:]
        raise ValueError(f'The test game exited immediately (code {code}). Last log lines:\n'+'\n'.join(tail))
    except subprocess.TimeoutExpired:pass
    return dict(ok=True,pid=proc.pid,path=str(game),windowed=bool(windowed),asPlayed=bool(as_played and not windowed),runtime=refreshed,
                message=('Live game view started (visible, muted, not focused). ' if windowed else 'Test game started with your settings (window opening). ' if as_played else 'Private stage playtest started (game window opening). ')+'Installed game unchanged.'+(f' {refreshed}.' if refreshed else ''))

# ---- live game view (roadmap section 8 item 10). /api/playtest {windowed: true} - only the editor (POST + token) asks
# for it, agent scripts never do (they use --prepare-project + their own --quiet launch): the private test game opens
# VISIBLE but MUTED (vol_master=0 in its own shadow_settings.ini) and without taking focus (SDL hint
# SDL_WINDOW_ACTIVATE_WHEN_SHOWN=0 read from the environment, plus a no-activate first show), so the user keeps typing
# in the editor. Without the flag the playtest is the normal controllable window, as before. No runtime change needed.
LIVE_VIEW={}
def playtest_launch(game,stage,windowed):
    """(args, env additions, STARTUPINFO or None) for the private test game"""
    args=[str(Path(game)/'shadow.exe'),'--disc',str(DISC),'--no-launcher','--stage',str(stage),'--widescreen']
    if not windowed:return args,{},None
    si=subprocess.STARTUPINFO();si.dwFlags|=subprocess.STARTF_USESHOWWINDOW;si.wShowWindow=4   # SW_SHOWNOACTIVATE
    return args,{'SDL_WINDOW_ACTIVATE_WHEN_SHOWN':'0'},si
# POST /api/live-view/camera {id, position [x,y,z], lookAt [x,y,z] | yaw, pitch (degrees), fov (degrees, default 60)} and
# POST /api/live-view/release {id}: a file channel read by the runtime (docs/stage-workshop/LIVE-VIEW.md). The
# request is validated, normalised (both lookAt and yaw / pitch; yaw 0 = +z, 90 = +x, pitch up positive) and written to
# <test game folder>/live-view-camera.json {seq, mode 'camera' | 'release', position, lookAt, yaw, pitch, fov, t}; the
# reply says relayed=true while the project's test game runs (optional 'player': 'stay' is passed on). The folder is the project's running live game (else its newest one).
def _live_game(pid):
    lv=LIVE_VIEW.get(pid);proc=PROCESSES.get(pid);running=bool(proc and proc.poll() is None)
    if lv:return Path(lv['game']),running,lv
    d=project_dir(pid)/'playtests';games=sorted((g for g in d.iterdir() if g.is_dir()),key=lambda g:g.stat().st_mtime) if d.exists() else []
    if not games:raise ValueError('Live view: no test game of this project yet (start a playtest first)')
    lv=LIVE_VIEW.setdefault(pid,dict(game=str(games[-1]),windowed=False,started=0,seq=0));return games[-1],running,lv
def live_view_camera(data):
    pid=data.get('id');game,running,lv=_live_game(pid)
    vec=lambda v,name:[float(x) for x in v] if isinstance(v,(list,tuple)) and len(v)==3 and all(isinstance(x,(int,float)) and math.isfinite(x) and abs(x)<1e7 for x in v) else (_ for _ in ()).throw(ValueError(f'Live view: {name} must be [x, y, z]'))
    pos=vec(data.get('position'),'position');fov=float(data.get('fov',60))
    if not 10<=fov<=150:raise ValueError('Live view: fov is 10..150 degrees')
    if data.get('lookAt') is not None:
        look=vec(data['lookAt'],'lookAt');dv=np.subtract(look,pos);n=float(np.linalg.norm(dv))
        if n<1e-6:raise ValueError('Live view: lookAt equals position')
        yaw=math.degrees(math.atan2(dv[0],dv[2]));pitch=math.degrees(math.asin(max(-1,min(1,dv[1]/n))))
    elif all(isinstance(data.get(k),(int,float)) and math.isfinite(data[k]) for k in ('yaw','pitch')):
        yaw,pitch=float(data['yaw']),float(data['pitch']);y,pt=math.radians(yaw),math.radians(pitch)
        look=[pos[0]+math.sin(y)*math.cos(pt)*100,pos[1]+math.sin(pt)*100,pos[2]+math.cos(y)*math.cos(pt)*100]
    else:raise ValueError('Live view: give lookAt [x, y, z] or yaw and pitch')
    lv['seq']+=1;cam=dict(seq=lv['seq'],mode='camera',position=pos,lookAt=[round(x,3) for x in look],yaw=round(yaw,3),pitch=round(pitch,3),fov=fov,t=time.time())
    if data.get('player')=='stay':cam['player']='stay'   # LIVE-VIEW.md: do not carry the player behind the camera
    atomic(game/'live-view-camera.json',json_bytes(cam))
    return dict(ok=True,relayed=running,running=running,file=str(game/'live-view-camera.json'),camera=cam,channel='file channel read by the runtime every 50 ms (docs/stage-workshop/LIVE-VIEW.md)')
def live_view_release(data):
    pid=data.get('id');game,running,lv=_live_game(pid);lv['seq']+=1
    atomic(game/'live-view-camera.json',json_bytes(dict(seq=lv['seq'],mode='release',t=time.time())))
    return dict(ok=True,relayed=running,running=running,file=str(game/'live-view-camera.json'),channel='file channel (docs/stage-workshop/LIVE-VIEW.md)')

# ---- 5d live highlight: the runtime appends <test game folder>/live-highlight.jsonl while a v4 program runs (:
# {ev: nodes, t, nodes: ['<owner>/<node>', ...]} per frame, {ev: vars, t, vars: {name: value}} when variables change,
# {ev: error ...}, {ev: capped}). GET /api/live?id=<project>&since=<byte cursor> -> {lines: [{t, node} | {t, vars} |
# {t, error}], next, running, game}; the newest test game folder of the project; empty lines when none.
def live_tail(pid,since=0):
    d=project_dir(pid);files=sorted((d/'playtests').glob('*/live-highlight.jsonl'),key=lambda f:f.stat().st_mtime) if (d/'playtests').exists() else []
    proc=PROCESSES.get(pid);running=bool(proc and proc.poll() is None)
    if not files:return dict(lines=[],next=0,running=running,game=None)
    f=files[-1];size=f.stat().st_size;running=running or time.time()-f.stat().st_mtime<5
    since=max(0,int(since or 0))
    if since>size:since=0   # a new game started (the file was rewritten)
    with open(f,'rb') as h:h.seek(since);data=h.read(min(size-since,512*1024))
    end=data.rfind(b'\n')+1;lines=[]
    for raw in data[:end].splitlines():
        try:j=json.loads(raw)
        except ValueError:continue
        ev=j.get('ev')
        if ev=='nodes':lines+=[dict(t=j.get('t'),node=n) for n in j.get('nodes',[])]
        elif ev=='vars':lines.append(dict(t=j.get('t'),vars=j.get('vars',{})))
        elif ev in ('error','capped'):lines.append(dict(t=j.get('t'),error=j.get('error') or j.get('msg') or ev,node=j.get('node')))
    return dict(lines=lines[-500:],next=since+end,running=running,game=f.parent.name)

# ---- prefabs (UI 2): JSON blobs under stage_projects/_prefabs/<name>.json; reads open, writes token-guarded (POST)
PREFABS=PROJECTS/'_prefabs'
def prefab_path(name):
    name=str(name).strip()
    if not re.fullmatch(r'[A-Za-z0-9 _().,+-]{1,80}',name):raise ValueError('Prefab names: 1-80 letters, digits, spaces and _ ( ) . , + -')
    return PREFABS/f'{name}.json'
def prefab_list():
    out=[]
    for f in sorted(PREFABS.glob('*.json')) if PREFABS.exists() else []:
        try:j=json.loads(f.read_text('utf-8'))
        except (OSError,ValueError):continue
        out.append(dict(name=f.stem,saved=f.stat().st_mtime,bytes=f.stat().st_size,stage=j.get('stage') if isinstance(j,dict) else None))
    return dict(prefabs=out)
def prefab_save(data):
    path=prefab_path(data.get('name'));body=data.get('prefab')
    if not isinstance(body,(dict,list)):raise ValueError('prefab must be a JSON object or list')
    raw=json_bytes(body)
    if len(raw)>16*1024*1024:raise ValueError('Prefab larger than 16 MB')
    PREFABS.mkdir(parents=True,exist_ok=True);atomic(path,raw);return dict(name=path.stem,saved=path.stat().st_mtime)
def prefab_delete(data):
    path=prefab_path(data.get('name'))
    if not path.exists():raise ValueError('No such prefab')
    path.unlink();return dict(name=path.stem,deleted=True)

# The editor lives as long as an editor window does: every open page sends /api/alive every few seconds and
# /api/bye when it closes. No window for a while -> the server exits (never in the middle of a build or save).
PAGES={};PAGES_LOCK=threading.Lock();STARTED=time.time()
# #48 (patch from agents/claude/backups/ui2-trackC-20261016/server-48-patch.md): why did the editor window go
# away? UI 2 sends its last lifecycle event on /api/alive (&last=, first beat &prev=) and its last events on /api/bye (&ev=).
EVENTS_LOG=DATA/'work/stage-editor-browser/editor-events.log'
PAGE_LAST={}
def log_event(text):
    try:
        EVENTS_LOG.parent.mkdir(parents=True,exist_ok=True)
        with open(EVENTS_LOG,'a',encoding='utf-8') as f:f.write(time.strftime('%Y-%m-%d %H:%M:%S')+'  '+text.replace(chr(13),'').replace(chr(10),' | ')[:4000]+chr(10))
    except OSError:pass
def note_page_events(page,alive,q):
    with PAGES_LOCK:gap=time.time()-PAGES[page] if page in PAGES else None
    last=q.get('last',[''])[0]
    if alive:
        if gap is None:log_event(f'page {page[:8]} first beat'+(f"; previous page ended with: {q['prev'][0]}" if q.get('prev') else ''))
        elif gap>20:log_event(f'page {page[:8]} heartbeat gap {gap:.0f} s (last event: {last})')
        PAGE_LAST[page]=last
    else:
        PAGE_LAST.pop(page,None)
        log_event(f"page {page[:8]} bye (last beat {gap:.1f} s ago): {q.get('ev',[''])[0]}" if gap is not None else f"page {page[:8]} bye (unknown page): {q.get('ev',[''])[0]}")
def note_page(page,alive):
    with PAGES_LOCK:
        if alive:PAGES[page]=time.time()
        else:PAGES.pop(page,None)
def watchdog(server):
    seen=False;empty_since=None
    while True:
        time.sleep(1);now=time.time()
        with PAGES_LOCK:
            # crashed / killed windows stop beating; 180 s because browsers throttle timers of hidden or minimised pages
            # to about one per minute (closing the window sends /api/bye and exits within seconds)
            for k in [k for k,t in PAGES.items() if now-t>180]:
                log_event(f'page {k[:8]} dropped: no heartbeat for 180 s (last event: {PAGE_LAST.pop(k,"")})');PAGES.pop(k)
            live=len(PAGES)
        if live:seen=True;empty_since=None;continue
        if empty_since is None:empty_since=now
        # a reload gives a new page within a second or two; before the first page, allow time for the window to open
        if now-empty_since<(6 if seen else 90):continue
        if not LOCK.acquire(blocking=False):continue   # a build/save is running: wait for it
        if sys.stdout:print('Stage Workshop: no editor window open - exiting',flush=True)
        log_event(f'server exit: no editor page for {now-empty_since:.0f} s')
        os._exit(0)

class Handler(BaseHTTPRequestHandler):
    def log_message(self,*args):pass
    def reply(self,status,data,kind='application/json'):
        if not isinstance(data,bytes):data=json_bytes(data)
        self.send_response(status);self.send_header('Content-Type',kind);self.send_header('Content-Length',str(len(data)));self.send_header('Cache-Control','no-store');self.end_headers();self.wfile.write(data)
    def safe(self):
        host=self.headers.get('Host','').split(':')[0]
        return host in ('127.0.0.1','localhost')
    def do_GET(self):
        if not self.safe():return self.reply(403,{'error':'Local access only'})
        u=urlparse(self.path);q=parse_qs(u.query)
        try:
            if u.path in ('/api/alive','/api/bye'):
                if q.get('t',[''])[0]!=TOKEN or not re.fullmatch('[A-Za-z0-9_-]{8,64}',q.get('page',[''])[0]):return self.reply(403,{'error':'Invalid local editor session'})
                note_page_events(q['page'][0],u.path=='/api/alive',q)
                note_page(q['page'][0],u.path=='/api/alive');return self.reply(200,{'ok':True})
            if u.path=='/api/tilesets':return self.reply(200,tilesets())
            if u.path=='/api/tileset-file':
                fn=q['file'][0];return self.reply(200,tileset_file(q['kit'][0],fn),'model/gltf-binary' if fn.endswith('.glb') else 'image/png' if fn.endswith('.png') else 'application/octet-stream')
            if u.path=='/api/skies':import sky;texture_library();return self.reply(200,dict(skies=sky.list_skies()))
            if u.path=='/api/sky-thumb':
                import sky;f=sky.THUMBS/f"{int(q['stage'][0])}.png"
                return self.reply(200,f.read_bytes(),'image/png') if f.exists() else self.reply(404,{'error':'no thumbnail'})
            if u.path=='/api/music-tracks':return self.reply(200,dict(tracks=music_tracks()))
            if u.path=='/api/music-preview':
                wav=music_preview(q.get('track',[None])[0],q.get('id',[None])[0],q.get('file',[None])[0])
                self.send_response(200);self.send_header('Content-Type','audio/wav');self.send_header('Content-Length',str(len(wav)));self.end_headers();self.wfile.write(wav);return
            if u.path=='/api/config':return self.reply(200,dict(app='Shadow Stage Workshop',version=1,token=TOKEN,stages=[dict(id=int(p.name[3:]),name=STAGE_NAMES.get(int(p.name[3:]),p.name)) for p in sorted((DISC/'files').glob('stg[0-9][0-9][0-9][0-9]'))],catalog=[dict(c,internalName=c['name'],name=display_label(c['name']),**class_fields(c['list'],c['type'])) for c in CATALOG],projects=project_list()))
            if u.path=='/api/stage':return self.reply(200,dict(import_stage(int(q['id'][0])),fresh=True))   # #95: a never-saved copy
            if u.path=='/api/object-library':return self.reply(200,object_library(int(q['id'][0])))
            if u.path=='/api/playtest-log':return self.reply(200,playtest_log(q['id'][0]))
            if u.path=='/api/enemy-paths':return self.reply(200,dict(presets=enemy_path_presets()))
            if u.path=='/api/version':   # #80: a server older than its code (fixes need a restart) -> UI 2 shows a chip
                code=max(f.stat().st_mtime for f in Path(__file__).resolve().parent.glob('*.py'))
                return self.reply(200,dict(started=STARTED,codeMtime=code,stale=STARTED<code,pid=os.getpid(),python=sys.executable,textureDecoder=TEX_STATUS['decoder'],discTextures=TEX_STATUS['stages']))
            if u.path=='/api/atmosphere':return self.reply(200,atmosphere(int(q['stage'][0])))
            if u.path=='/api/lightbake':
                with LOCK:return self.reply(200,light_bake(q['id'][0]))
            if u.path=='/api/live':return self.reply(200,live_tail(q['id'][0],q.get('since',['0'])[0]))
            if u.path=='/api/prefabs':
                if 'name' in q:return self.reply(200,json.loads(prefab_path(q['name'][0]).read_text('utf-8')))
                return self.reply(200,prefab_list())
            if u.path=='/api/models':
                if 'archive' in q:return self.reply(200,archive_models(q['archive'][0]))
                if 'class' in q:return self.reply(200,class_models(q['class'][0]))
                return self.reply(200,stage_models(int(q['id'][0])))
            if u.path=='/api/project':return self.reply(200,load_project(q['id'][0]))
            # custom stages (custom_stage.py, docs/custom-stages): launcher list, validation, package download, cover
            if u.path=='/api/custom-stages':import custom_stage;return self.reply(200,custom_stage.list_stages())
            if u.path=='/api/custom-stage/validate':import custom_stage;return self.reply(200,custom_stage.validate(load_project(q['id'][0])))
            if u.path=='/api/custom-stage/stats':import custom_stage;return self.reply(200,custom_stage.stats(q['id'][0]))
            if u.path=='/api/package':
                import custom_stage;data,name=custom_stage.export_package(q['id'][0])
                self.send_response(200);self.send_header('Content-Type','application/zip');self.send_header('Content-Disposition',f'attachment; filename="{name}"')
                self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data);return
            if u.path=='/api/project-asset':
                f=project_asset(q['id'][0],q['file'][0])
                if not f.is_file():return self.reply(404,{'error':'Unknown asset'})
                return self.reply(200,f.read_bytes(),mimetypes.guess_type(f.name)[0] or 'application/octet-stream')
            if u.path=='/api/stage-params':return self.reply(200,stage_params(int(q['id'][0])) or {})
            if u.path=='/api/textures':return self.reply(200,texture_library())
            if u.path.startswith('/asset/'):
                p=ASSETS.get(u.path.rsplit('/',1)[-1]);
                if not p:return self.reply(404,{'error':'Unknown asset'})
                return self.reply(200,p.read_bytes(),mimetypes.guess_type(p.name)[0] or 'application/octet-stream')
            rel=unquote(u.path).lstrip('/') or 'index.html';p=(APP/rel).resolve()
            if not p.is_relative_to(APP) or not p.is_file() or p.suffix not in ['.html','.css','.js','.json']:return self.reply(404,{'error':'Not found'})
            return self.reply(200,p.read_bytes(),mimetypes.guess_type(p.name)[0] or 'text/plain')
        except Exception as e:self.reply(400,{'error':str(e)})
    def do_POST(self):
        if not self.safe() or self.headers.get('X-Workshop-Token')!=TOKEN:return self.reply(403,{'error':'Invalid local editor session'})
        try:
            n=int(self.headers.get('Content-Length','0'))
            if n<=0 or n>(560 if self.path=='/api/import-geometry' else 64)*1024*1024:raise ValueError('Invalid project size')   # #119 imports carry base64 models + textures
            p=json.loads(self.rfile.read(n),parse_constant=lambda x: (_ for _ in ()).throw(ValueError('Nonfinite number')))
            with LOCK:
                if self.path=='/api/save':out=save_project(p)
                elif self.path=='/api/new':out=new_stage(p)
                elif self.path=='/api/upload':out=upload_asset(p)
                elif self.path=='/api/loop-kit':   # a runnable loop: {entry [x, floor y, z], yaw, radius, width, runIn, runOut, material} -> track + loop path rows (loop_kit.py)
                    import loop_kit;out=loop_kit.make(p.get('entry') or [0,0,0],float(p.get('yaw') or 0),float(p.get('radius') or 250),float(p.get('width') or 120),
                      float(p.get('runIn') or 300),float(p.get('runOut') or 400),material=p.get('material'))
                elif self.path=='/api/sky':   # #103 sky geometry rows for a project (stage's backdrop or a picture dome)
                    import sky;q_=dict(id=p['project'],objects=[dict(kind='playerstart',position=p.get('start') or [0,0,0])])
                    mat=next((m for m in p.get('materials',[]) if m.get('id')==p.get('material')),None) if p.get('material') else None
                    out=sky.make(q_,stage=p.get('stage'),material=mat,lift=bool(p.get('lift')))   # lift: raise a sky that sits below the start (Westopolis)
                elif self.path=='/api/import-model':out=import_model(p)
                elif self.path=='/api/import-geometry':out=import_geometry_api(p)
                elif self.path=='/api/tileset-piece':out=tileset_piece(p)
                elif self.path=='/api/build':out=build_project(p)
                elif self.path=='/api/playtest':out=playtest(p['id'],build_path=p.get('build'),windowed=bool(p.get('windowed')),as_played=bool(p.get('asPlayed')))
                elif self.path=='/api/live-view/camera':out=live_view_camera(p)
                elif self.path=='/api/live-view/release':out=live_view_release(p)
                elif self.path=='/api/prefabs':out=prefab_save(p)
                elif self.path=='/api/reveal':out=reveal_asset(p)
                elif self.path=='/api/prefabs/delete':out=prefab_delete(p)
                elif self.path=='/api/package/import':import custom_stage;out=custom_stage.import_package(base64.b64decode(p['data'],validate=True))
                elif self.path=='/api/blank-stage':import custom_stage;out=custom_stage.blank_stage(str(p.get('name') or 'Blank stage'),bool(p.get('populated',True)))
                else:raise ValueError('Unknown operation')
            self.reply(200,out)
        except Exception as e:self.reply(400,{'error':str(e)})

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--port',type=int,default=49012);parser.add_argument('--no-browser',action='store_true');parser.add_argument('--prepare-project');parser.add_argument('--watch',action='store_true',help='exit when no editor page is alive (default with a browser window)');parser.add_argument('--ui2',action='store_true',help='open the UI 2 test build instead of the classic editor');args=parser.parse_args()
    if args.prepare_project:
        print(json.dumps(playtest(args.prepare_project,True)));raise SystemExit(0)
    url=f'http://127.0.0.1:{args.port}'
    def open_window():
        edge=Path(os.environ.get('PROGRAMFILES(X86)','C:/Program Files (x86)'))/'Microsoft/Edge/Application/msedge.exe'
        page=url+('/ui2/index.html' if args.ui2 else '')
        if edge.exists():subprocess.Popen([str(edge),'--app='+page,'--window-size=1600,1000','--user-data-dir='+str(DATA/'work/stage-editor-browser')])
        else:webbrowser.open(page)
    try:server=ThreadingHTTPServer(('127.0.0.1',args.port),Handler)
    except OSError:
        import urllib.request
        data=json.load(urllib.request.urlopen(url+'/api/config',timeout=2))
        if data.get('app')!='Shadow Stage Workshop':raise RuntimeError('Stage Workshop port is occupied by another application')
        if not args.no_browser:open_window()
        raise SystemExit(0)
    if not args.no_browser:threading.Timer(.5,open_window).start()
    if not args.no_browser or args.watch:threading.Thread(target=watchdog,args=(server,),daemon=True).start()   # --no-browser alone (tools) stays up
    if sys.stdout:print('Stage Workshop: '+url,flush=True)
    server.serve_forever()
