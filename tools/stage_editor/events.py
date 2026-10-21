"""Compile bounded stage events and a small custom action language, never host Python/JS."""
import math,shlex,struct

TYPES={'teleport':1,'set_speed':2,'rings':3,'score':4,'set_variable':5,'lighting':6,'enable':7,'disable':8}
EVENTS={'enter':0,'exit':1,'stay':2,'interact':3,
        # native SET object events (sidecar v3, runtime/stage_workshop.cpp SetManager hooks)
        'spawn':4,'killed':5,'removed':6,'despawn':7}
OBJECT_EVENTS=('spawn','killed','removed','despawn')
LAYER_INDEX={'cmn':0,'nrm':1,'hrd':2,'ds1':3}
COMPARE={'==':0,'!=':1,'<':2,'<=':3,'>':4,'>=':5}
LIVE_VALUES={'@rings','@score','@lives','@hero_score','@dark_score','@stage_time','@player_y','@speed'}
def name64(s):
    b=str(s).encode('utf-8')
    if len(b)>63 or b'\0' in b:raise ValueError('Event/variable names must be at most63 UTF8 bytes')
    return b.ljust(64,b'\0')
def finite(x):
    x=float(x)
    if not math.isfinite(x):raise ValueError('Nonfinite action value')
    return x
def graph_actions(trigger):
    graph=trigger.get('graph')
    if not graph:return list(trigger['actions'])
    if len(graph['nodes'])>512 or len(graph['edges'])>2048:raise ValueError('Graph limit:512 nodes /2048 connections')
    nodes={n['id']:n for n in graph['nodes']};events=[n for n in nodes.values() if n['type']=='event']
    if len(nodes)!=len(graph['nodes']):raise ValueError('Graph node IDs must be unique')
    adjacency={key:[] for key in nodes}
    for n in nodes.values():
        if n['type'] not in ('event','condition','action','delay'):raise ValueError('Unknown graph node type')
        if n['type']=='delay' and not 0<=finite(n.get('seconds',1))<=3600:raise ValueError('Delay must be0..3600 seconds')
    connections=set()
    for edge in graph['edges']:
        if edge['from'] not in nodes or edge['to'] not in nodes:raise ValueError('Graph connection points to a missing node')
        source=nodes[edge['from']];destination=nodes[edge['to']]
        if destination['type']=='event':raise ValueError('Event nodes cannot have inputs')
        ports=('true','false') if source['type']=='condition' else ('next',)
        if edge.get('port') not in ports:raise ValueError('Invalid graph output port')
        key=(edge['from'],edge['to'],edge['port'])
        if key in connections:raise ValueError('Duplicate graph connection')
        connections.add(key);adjacency[edge['from']].append(edge)
    if len(events)!=1:raise ValueError('A trigger graph needs exactly one event node')
    out=[];visits=0
    def walk(node_id,delay,conditions,chain):
        nonlocal visits
        visits+=1
        if visits>16384 or len(out)>=4096:raise ValueError('Graph expands beyond the stage action limit; simplify branches')
        if node_id in chain:raise ValueError('Graph cycle: use repeat/cooldown on the trigger instead')
        if node_id not in nodes:raise ValueError('Graph connection points to a missing node')
        if len(chain)>64:raise ValueError('Graph is too deep')
        n=nodes[node_id];chain=chain|{node_id}
        if n['type']=='action':
            a=n['action'];own=a.get('conditions',([a['condition']] if a.get('condition') else []))
            out.append(dict(a,delay=delay+finite(a.get('delay',0)),conditions=conditions+own))
        if n['type']=='delay':delay+=finite(n.get('seconds',1))
        for edge in adjacency[node_id]:
            cond=list(conditions)
            if n['type']=='condition':
                compare=n.get('compare','==')
                if edge.get('port')=='false':compare={'==':'!=','!=':'==','<':'>=','<=':'>','>':'<=','>=':'<'}[compare]
                cond.append(dict(name=n.get('variable',''),value=finite(n.get('value',0)),compare=compare))
                if len(cond)>4:raise ValueError('Maximum four nested conditions per action')
            walk(edge['to'],delay,cond,chain)
    walk(events[0]['id'],0,[],set());return out
OLD_EXAMPLE='# Use an existing trigger and a uniquely named destination.\non enter "Trigger":\n    wait 0.5\n    teleport "Destination"\n'
def compile_events(project,collect=None):
    """sidecar v3 bytes; collect (a list) receives every volume with its resolved actions and its v4 owner, for
    compile_graph_v4 (which must carry the v3 logic too: the runtime runs no v3 actions when a v4 program is present)"""
    objects=project['objects']+project['geometry']+project['triggers']+project['paths']
    def target(s):
        hits=[o for o in objects if o['id']==s or o['name']==s]
        if len(hits)!=1:raise ValueError(f'Target {s!r} must identify exactly one stage item')
        return hits[0]
    volumes=[dict(t,actions=graph_actions(t)) for t in project['triggers']]
    for pad in [o for o in project['objects'] if o.get('kind')=='teleport']:
        if not pad.get('target'):raise ValueError(f"Teleport pad {pad['name']} needs a destination")
        volumes.append(dict(id=pad['id'],name=pad['name'],position=pad['position'],rotation=pad['rotation'],scale=[1,1,1],size=[65,70,65],event='enter',once=False,cooldown=pad.get('cooldown',1),actions=[dict(type='teleport',target=pad['target'])]))
    # Native objects' own events. The object is named by layer + its record index in the BUILT layer file, which
    # server.build_project writes in project order: same filter, same order here.
    for layer,li in LAYER_INDEX.items():
        obs=[o for o in project['objects'] if o.get('kind')=='native' and o.get('layer')==layer]
        for idx,o in enumerate(obs):
            for k,e in enumerate(o.get('events',[])):
                if e.get('event') not in OBJECT_EVENTS:raise ValueError(f"{o['name']}: unknown object event {e.get('event')!r}")
                volumes.append(dict(id=f"{o['id']}#{k}",name=(f"{o['name']} {e['event']}".encode('utf-8')[:60]).decode('utf-8','ignore'),
                  position=[li,idx,0],rotation=[0,0,0],scale=[1,1,1],size=[1,1,1],event=e['event'],once=bool(e.get('once',False)),
                  cooldown=e.get('cooldown',.05),actions=graph_actions(dict(e,actions=e.get('actions',[])))))
    def volume(s):
        hits=[(i,t) for i,t in enumerate(volumes) if t['id']==s or t['name']==s]
        if len(hits)!=1:raise ValueError(f'Trigger {s!r} must identify exactly one volume')
        return hits[0]
    for script in project['scripts']:
        # the editor's old starter example (never edited) is documentation, not logic: skip it
        if script['code'].startswith(OLD_EXAMPLE) and not any(t['name']=='Trigger' for t in project['triggers']):continue
        current=None;delay=0
        for line_no,line in enumerate(script['code'].splitlines(),1):
            text=line.strip()
            if not text or text.startswith('#'):continue
            try:
                if text.startswith('on ') and text.endswith(':'):
                    words=shlex.split(text[:-1]);event=words[1];_,current=volume(' '.join(words[2:]));
                    if current['event']!=event:raise ValueError('Script event must match the trigger’s selected event')
                    delay=0;continue
                if current is None:raise ValueError('Start a block with on enter "Trigger name":')
                condition=None
                if text.startswith('if '):
                    cond,text=text[3:].split(':',1);words=shlex.split(cond)
                    if len(words)!=3 or words[1] not in COMPARE:raise ValueError('Use: if variable >= number: command')
                    condition=dict(name=words[0],value=finite(words[2]),compare=words[1]);text=text.strip()
                words=shlex.split(text);cmd=words[0];args=words[1:]
                if cmd=='wait':delay+=finite(args[0]);continue
                action=dict(type={'speed':'set_speed','set':'set_variable'}.get(cmd,cmd),delay=delay)
                if cmd=='teleport':action['target']=' '.join(args)
                elif cmd=='speed':action['vector']=[finite(x) for x in args]
                elif cmd in ('rings','score'):action['value']=finite(args[0])
                elif cmd=='set':action.update(name=args[0],value=finite(args[1]))
                elif cmd=='lighting':action.update(name=args[0],value=1 if args[1]=='on' else 0)
                elif cmd in ('enable','disable'):action['target']=' '.join(args)
                else:raise ValueError('Unknown command: '+cmd)
                if condition:action['condition']=condition
                current['actions'].append(action)
            except Exception as e:raise ValueError(f"{script['name']}:{line_no}: {e}") from e
    actions=[];rows=[];pad_ids=[o['id'] for o in project['objects'] if o.get('kind')=='teleport'];ntrig=len(project['triggers'])
    for vi,t in enumerate(volumes):
        start=len(actions)
        if collect is not None:
            if vi<ntrig:owner=(1,vi,project['triggers'][vi]['id'])
            elif t['id'] in pad_ids:owner=(3,pad_ids.index(t['id']),t['id'])
            else:owner=(2,(int(t['position'][0])<<16)|int(t['position'][1]),t['id'].split('#')[0])
            collect.append(dict(event=t['event'],once=bool(t.get('once',True)),cooldown=max(.05,finite(t.get('cooldown',.1))),owner=owner,actions=[]))
        for a in t['actions']:
            typ=a['type']
            if typ not in TYPES:raise ValueError(f'Action {typ} is not connected to the game yet')
            v=[0,0,0];value=0;name='';index=-1;delay=finite(a.get('delay',0));condition=a.get('condition',{})
            if not 0<=delay<=3600:raise ValueError('Delay must be0..3600 seconds')
            if typ=='teleport':v=target(a['target'])['position']
            elif typ=='set_speed':
                v=a.get('vector')
                if v is None:
                    value=finite(a.get('value',150));v=[0,0,-value]
                if len(v)!=3:raise ValueError('Speed needs x,y,z components')
            elif typ in ('rings','score'):value=finite(a.get('value',0))
            elif typ in ('set_variable','lighting'):name=a.get('name') or str(a.get('value','')).split('=')[0];value=finite(a.get('number',a.get('value',0)))
            elif typ in ('enable','disable'):index,_=volume(a['target'])
            if typ in ('set_variable','lighting') and not name:raise ValueError('Action needs a variable/event name')
            if typ=='set_variable' and name.startswith('@'):raise ValueError('Names starting @ are read-only game values')
            conditions=a.get('conditions',([condition] if condition else []))
            if len(conditions)>4:raise ValueError('Maximum four conditions per action')
            row=struct.pack('<I3fff',TYPES[typ],*[finite(x) for x in v],value,delay)+name64(name)+struct.pack('<iI',index,len(conditions))
            for c in conditions:
                if not c.get('name'):raise ValueError('Condition needs a variable name')
                if c['name'].startswith('@') and c['name'] not in LIVE_VALUES:raise ValueError('Unknown live game value: '+c['name'])
                row+=name64(c['name'])+struct.pack('<fI',finite(c.get('value',0)),COMPARE[c.get('compare','==')])
            row+=b'\0'*((4-len(conditions))*72)
            actions.append(row)
            if collect is not None:collect[-1]['actions'].append(dict(type=typ,v=[finite(x) for x in v],value=value,name=name,index=index,delay=delay,conditions=conditions))
        size=[abs(finite(x)*finite(s)) for x,s in zip(t['size'],t.get('scale',[1,1,1]))]
        if min(size)<=0:raise ValueError('Trigger size must be positive')
        row=name64(t['name'])+struct.pack('<II10fII',EVENTS[t['event']],int(t.get('once',True)),*t['position'],*size,*t.get('rotation',[0,0,0]),max(.05,finite(t.get('cooldown',.1))),start,len(actions)-start)
        rows.append(row)
    spawn=[o for o in project['objects'] if o.get('kind')=='spawn']
    if len(spawn)>1:raise ValueError('Use one player start per stage; checkpoints are native objects for now')
    if len(rows)>512 or len(actions)>4096:raise ValueError('Event runtime limit:512 triggers /4096 actions')
    version=3 if any(t['event'] in OBJECT_EVENTS for t in volumes) else 2
    head=b'SHSTAGE2'+struct.pack('<5I3f',project['stage'],version,len(rows),len(actions),int(bool(spawn)),*(spawn[0]['position'] if spawn else [0,0,0]))
    return head+b''.join(rows)+b''.join(actions)


# ======================================================================================================================
# Sidecar v4 (roadmap 5b): node graphs -> bytecode for a coroutine VM. Spec: docs/stage-workshop/SIDECAR-V4.md
# (a proposal; owns the runtime VM and may change it). compile_graph_v4(project, nodes) -> (bytes, debug);
# V4Machine is the reference interpreter the tests run (tools/stage_editor/test_events_v4.py). v3 stays the default.
# ======================================================================================================================
V4_OPS=dict(NOP=0,END=1,PUSH=2,LOAD=3,STORE=4,LIVE=5,MATH=6,CMP=7,NOT=8,JMP=9,JZ=10,JNZ=11,WAIT=12,YIELD=13,SPAWN=14,ACT=15,
  RAND=16,PLAYERPOS=17,OBJPOS=18,DUP=19,POP=20)
V4_OPNAME={v:k for k,v in V4_OPS.items()}
V4_EVENTS=dict(enter=0,exit=1,stay=2,interact=3,spawn=4,killed=5,removed=6,despawn=7,stage_start=8,every=9,custom=10,touch=11)
def fade_sun_name(owner,node):
    """rule / trigger name of a Fade sun node (same as ui2/logic.js: owner None = the stage graph)"""
    import re;tag=lambda x:re.sub('[^A-Za-z0-9]','',str(x))[:10]
    return f"ws_fade_sun_{'stage' if owner in (None,'stage') else tag(owner)}_{tag(node)}"
V4_ACT=dict(teleport=1,set_speed=2,rings=3,score=4,lighting=6,enable=7,disable=8,send=9,spawn=10,destroy=11,move=12,message=13,
  link_set=15,mission_complete=21,mission_fail=22,hud_value=23,hud_hide=24,play_sound=25)   # 15-25: VM (docs/stage-workshop/LOGIC-ACTIONS.md)
ROUTE_ID=dict(dark=0,neutral=1,hero=2)
V4_LIVE=['@rings','@score','@lives','@hero_score','@dark_score','@stage_time','@player_y','@speed']
V4_MATH=['+','-','*','/','min','max']
V4_BUDGET=dict(per_coroutine=256,per_frame=4096,coroutines=64,stack=64)
OWNER_KIND=dict(stage=0,volume=1,native=2,pad=3)

def load_nodes(path=None):
    import json,pathlib
    p=pathlib.Path(path) if path else pathlib.Path(__file__).with_name('nodes.json')
    return {n['id']:n for n in json.loads(p.read_text('utf-8'))['nodes']}

class V4Compiler:
    def __init__(self,project,nodes):
        self.p=project;self.defs=nodes;self.consts=[];self.strings=[];self.objects=[];self.vars=[];self.entries=[];self.skipped=[];self.sounds={}
        self.code=[];self.node_names=[];self.node_at={};self.addr={};self.cur=0
        self.triggers=project.get('triggers',[]);self.pads=[o for o in project.get('objects',[]) if o.get('kind')=='teleport']
        self.native={}
        for layer,li in LAYER_INDEX.items():
            for idx,o in enumerate([o for o in project.get('objects',[]) if o.get('kind')=='native' and o.get('layer')==layer]):self.native[o['id']]=(li,idx)
    # ---- tables
    def const(self,v):
        v=float(v)
        if not math.isfinite(v):raise ValueError('Nonfinite value')
        if v not in self.consts:self.consts.append(v)
        return self.consts.index(v)
    def string(self,s):
        s=str(s)
        if len(s.encode('utf-8'))>255:raise ValueError('Text longer than 255 bytes')
        if s not in self.strings:self.strings.append(s)
        return self.strings.index(s)
    def var(self,name,hidden_key=None,initial=0.0):
        key=('$',hidden_key) if hidden_key else name
        for i,(k,_,_) in enumerate(self.vars):
            if k==key:return i
        self.vars.append((key,None if hidden_key else name,float(initial)));return len(self.vars)-1
    def obj(self,ref,owner=None):
        if ref in (None,'') and owner is not None:ref=owner
        if ref in (None,''):raise ValueError('An object pin needs a target')
        if ref in [t.get('id') for t in self.triggers]:row=(1,[t['id'] for t in self.triggers].index(ref),0,0,0,0)
        elif ref in self.native:row=(2,*self.native[ref],0,0,0)
        elif ref in [o['id'] for o in self.pads]:row=(3,[o['id'] for o in self.pads].index(ref),0,0,0,0)
        else:
            hits=[o for o in self.p.get('objects',[])+self.p.get('geometry',[])+self.p.get('paths',[]) if o.get('id')==ref]
            if len(hits)!=1:raise ValueError(f'Object {ref!r} is not in the project')
            row=(4,0,0,*[float(x) for x in hits[0].get('position',[0,0,0])])
        if row not in self.objects:self.objects.append(row)
        return self.objects.index(row)
    # ---- code
    def emit(self,op,sub=0,arg=0):
        self.code.append([V4_OPS[op],sub,self.cur,arg]);return len(self.code)-1
    def at_node(self,owner_name,nid):
        key=f'{owner_name}/{nid}'
        if key not in self.node_at:self.node_at[key]=len(self.node_names);self.node_names.append(key)
        self.cur=self.node_at[key]
    def compile_owner(self,owner_name,owner_kind,owner_index,owner_ref,graph):
        g=dict(name=owner_name,kind=owner_kind,ref=owner_ref,nodes={n['id']:n for n in graph.get('nodes',[])},execs={},data={})
        if len(g['nodes'])!=len(graph.get('nodes',[])):raise ValueError(f'{owner_name}: node ids must be unique')
        if len(g['nodes'])>512 or len(graph.get('links',[]))>2048:raise ValueError(f'{owner_name}: graph limit 512 nodes / 2048 links')
        for n in g['nodes'].values():
            if n.get('type') not in self.defs:raise ValueError(f"{owner_name}: unknown node type {n.get('type')!r}")
        links=graph.get('links',[]);exec_reroutes=set();grew=True
        for l in links:
            if l['from'] not in g['nodes'] or l['to'] not in g['nodes']:raise ValueError(f'{owner_name}: link {l.get("id")} points to a missing node')
        while grew:   # a reroute carries exec when an exec pin (or an exec reroute) links into it
            grew=False
            for l in links:
                src=g['nodes'][l['from']]
                if g['nodes'][l['to']]['type']=='flow.reroute' and l['to'] not in exec_reroutes and (
                    l['out'] in self.defs[src['type']].get('exec',{}).get('out',[]) or l['from'] in exec_reroutes):exec_reroutes.add(l['to']);grew=True
        for l in links:
            src,dst=g['nodes'][l['from']],g['nodes'][l['to']]
            sd=self.defs[src['type']]
            if l['out'] in sd.get('exec',{}).get('out',[]) or l['from'] in exec_reroutes:
                g['execs'].setdefault((l['from'],l['out']),[]).append((l['to'],l['in']))
            else:
                if (l['to'],l['in']) in g['data']:raise ValueError(f'{owner_name}: input {l["in"]} of {l["to"]} has two links')
                g['data'][(l['to'],l['in'])]=(l['from'],l['out'])
        for n in g['nodes'].values():
            d=self.defs[n['type']]
            if d['category']!='event':continue
            ev=n['type'].split('.',1)[1];w=self.widgets(n)
            if ev not in V4_EVENTS:raise ValueError(f'{owner_name}: event {n["type"]} has no v4 code')
            period=float(w.get('seconds',0)) if ev=='every' else float(w.get('cooldown',0) or 0)
            name=self.string(w['name']) if ev=='custom' else 0xFFFF
            self.at_node(owner_name,n['id']);start=len(self.code);self.continue_to(g,n['id'],'then')
            self.entries.append((V4_EVENTS[ev],OWNER_KIND[owner_kind],owner_index,int(bool(w.get('once',False))),period,start,name))
    def timer(self,w):
        """the hidden variable of a named timer (its end on the stage clock), shared by every graph of the stage"""
        return self.var(None,f'timer/{str(w.get("name") or "timer")[:60]}')
    def widgets(self,n):
        d=self.defs[n['type']];out={w['name']:w.get('default') for w in d.get('widgets',[])};out.update(n.get('values') or {});return out
    def targets(self,g,nid,pin):
        out=[]
        for dst,dpin in g['execs'].get((nid,pin),[]):
            if g['nodes'][dst]['type']=='flow.reroute':out+=self.targets(g,dst,'out')
            else:out.append((dst,dpin))
        return out
    def continue_to(self,g,nid,pin):
        """emit the code that follows exec output pin of node nid"""
        ts=self.targets(g,nid,pin)
        if not ts:self.emit('END');return
        if len(ts)==1:self.block(g,*ts[0]);return
        spawns=[self.emit('SPAWN') for _ in ts];self.emit('END')
        for s,t in zip(spawns,ts):self.code[s][3]=self.addr_of(g,*t)
    def addr_of(self,g,nid,pin):
        """address of node nid entered through exec input pin (compiled at the end of the code the first time)"""
        key=(g['name'],nid,pin)
        if key not in self.addr:self.addr[key]=len(self.code);self.node(g,nid,pin)
        return self.addr[key]
    def block(self,g,nid,pin):
        """continue into node nid: compiled here (inline) the first time, else a JMP to it (joins and loops)"""
        key=(g['name'],nid,pin)
        if key in self.addr:self.emit('JMP',arg=self.addr[key])
        else:self.addr_of(g,nid,pin)
    def value(self,g,nid,pin,kind,depth=0):
        """push the value of input pin (linked data, else the widget); returns the number of floats pushed"""
        if depth>64:raise ValueError(f'{g["name"]}: data links form a cycle')
        link=g['data'].get((nid,pin))
        if link:return self.expr(g,*link,depth+1)
        w=self.widgets(g['nodes'][nid]).get(pin)
        if kind=='vector':
            v=list(w or [0,0,0])
            if len(v)!=3:raise ValueError('A vector needs x, y, z')
            for x in v:self.emit('PUSH',arg=self.const(x))
            return 3
        if kind=='object':self.emit('PUSH',arg=self.const(self.obj(w,g['ref'])));return 1
        if kind=='string':self.emit('PUSH',arg=self.const(self.string(w or '')));return 1
        if kind=='bool':self.emit('PUSH',arg=self.const(1 if w else 0));return 1
        self.emit('PUSH',arg=self.const(w or 0));return 1
    def expr(self,g,nid,out,depth=0):
        n=g['nodes'][nid];t=n['type'];w=self.widgets(n);keep=self.cur;self.at_node(g['name'],nid)
        try:
            if t=='flow.reroute':return self.value(g,nid,'in','number',depth)
            if t=='data.number':self.emit('PUSH',arg=self.const(w.get('number') or 0));return 1
            if t=='data.vector':
                for x in (w.get('vector') or [0,0,0]):self.emit('PUSH',arg=self.const(x))
                return 3
            if t=='data.text':self.emit('PUSH',arg=self.const(self.string(w.get('text') or '')));return 1
            if t=='data.object':self.emit('PUSH',arg=self.const(self.obj(w.get('object'),g['ref'])));return 1
            if t=='data.self':self.emit('PUSH',arg=self.const(self.obj(g['ref'])));return 1
            if t=='data.live':self.emit('LIVE',arg=V4_LIVE.index(w.get('value','@rings')));return 1
            if t=='data.get_variable':self.emit('LOAD',arg=self.var(w.get('name','my_variable')));return 1
            if t in ('data.compare','data.math'):
                self.value(g,nid,'a','number',depth);self.value(g,nid,'b','number',depth)
                if t=='data.compare':self.emit('CMP',COMPARE[w.get('op','==')])
                else:self.emit('MATH',V4_MATH.index(w.get('op','+')))
                return 1
            if t=='data.player_position':self.emit('PLAYERPOS');return 3
            if t=='data.object_position':
                self.value(g,nid,'target','object',depth);self.emit('OBJPOS',0 if out=='position' else 1);return 3 if out=='position' else 1
            if t=='flow.counter' and out=='count':self.emit('LOAD',arg=self.var(None,f'{g["name"]}/{nid}/count'));return 1
            # overnight B4 (nodes.json): built from the existing opcodes, no new VM code
            if t in ('flow.for_loop','flow.while_loop') and out=='index':self.emit('LOAD',arg=self.var(None,f'{g["name"]}/{nid}/index'));return 1
            if t=='data.and':
                self.value(g,nid,'a','bool',depth);self.value(g,nid,'b','bool',depth);self.emit('MATH',V4_MATH.index('*'))
                self.emit('PUSH',arg=self.const(0));self.emit('CMP',COMPARE['!=']);return 1
            if t=='data.or':
                for p_ in ('a','b'):self.value(g,nid,p_,'bool',depth);self.emit('PUSH',arg=self.const(0));self.emit('CMP',COMPARE['!='])
                self.emit('MATH',V4_MATH.index('+'));self.emit('PUSH',arg=self.const(0));self.emit('CMP',COMPARE['>']);return 1
            if t=='data.not':self.value(g,nid,'a','bool',depth);self.emit('NOT');return 1
            if t=='data.random_range':   # min + RAND * (max - min)
                self.emit('RAND');self.value(g,nid,'max','number',depth);self.value(g,nid,'min','number',depth);self.emit('MATH',V4_MATH.index('-'))
                self.emit('MATH',V4_MATH.index('*'));self.value(g,nid,'min','number',depth);self.emit('MATH',V4_MATH.index('+'));return 1
            if t=='data.timer_left':   # max(0, end - stage time); a timer never started has end 0
                self.emit('LOAD',arg=self.timer(w));self.emit('LIVE',arg=V4_LIVE.index('@stage_time'));self.emit('MATH',V4_MATH.index('-'))
                self.emit('PUSH',arg=self.const(0));self.emit('MATH',V4_MATH.index('max'));return 1
            if t=='action.spawn' and out=='spawned':self.emit('LOAD',arg=self.var(None,f'{g["name"]}/{nid}/spawned'));return 1
            raise ValueError(f'{g["name"]}: {t} output {out} cannot be used as data')
        finally:self.cur=keep
    def node(self,g,nid,pin):
        n=g['nodes'][nid];t=n['type'];w=self.widgets(n);self.at_node(g['name'],nid)
        slot=lambda what,init=0:self.var(None,f'{g["name"]}/{nid}/{what}',init)
        if t=='flow.branch':
            name=str(w.get('variable','@rings'))
            if name.startswith('@'):
                if name not in V4_LIVE:raise ValueError(f'{g["name"]}: unknown live value {name}')
                self.emit('LIVE',arg=V4_LIVE.index(name))
            else:self.emit('LOAD',arg=self.var(name))
            self.value(g,nid,'value','number')
            self.emit('CMP',COMPARE[w.get('compare','==')]);j=self.emit('JZ')
            self.continue_to(g,nid,'true');self.code[j][3]=len(self.code);self.at_node(g['name'],nid);self.continue_to(g,nid,'false');return
        if t=='flow.delay':self.value(g,nid,'seconds','number');self.emit('WAIT');self.continue_to(g,nid,'then');return
        if t=='flow.sequence':
            ts=[x for p in ('then 0','then 1','then 2','then 3') for x in self.targets(g,nid,p)]
            if not ts:self.emit('END');return
            spawns=[self.emit('SPAWN') for _ in ts];self.emit('END')
            for s,x in zip(spawns,ts):self.code[s][3]=self.addr_of(g,*x)
            return
        if t=='flow.do_once':
            s=slot('done')
            if pin=='reset':self.emit('PUSH',arg=self.const(0));self.emit('STORE',arg=s);self.emit('END');return
            self.emit('LOAD',arg=s);j=self.emit('JNZ');self.emit('PUSH',arg=self.const(1));self.emit('STORE',arg=s)
            self.continue_to(g,nid,'then');self.code[j][3]=len(self.code);self.emit('END');return
        if t=='flow.gate':
            s=slot('open',1 if w.get('start_open',True) else 0)
            if pin in ('open','close'):self.emit('PUSH',arg=self.const(1 if pin=='open' else 0));self.emit('STORE',arg=s);self.emit('END');return
            if pin=='toggle':self.emit('LOAD',arg=s);self.emit('NOT');self.emit('STORE',arg=s);self.emit('END');return
            self.emit('LOAD',arg=s);j=self.emit('JZ');self.continue_to(g,nid,'exit');self.code[j][3]=len(self.code);self.emit('END');return
        if t=='flow.counter':
            s=slot('count')
            if pin=='reset':self.emit('PUSH',arg=self.const(0));self.emit('STORE',arg=s);self.emit('END');return
            self.emit('LOAD',arg=s);self.emit('PUSH',arg=self.const(1));self.emit('MATH',0);self.emit('DUP');self.emit('STORE',arg=s)
            self.value(g,nid,'target','number');self.emit('CMP',COMPARE['==']);j=self.emit('JZ')
            reached=self.targets(g,nid,'reached');spawns=[self.emit('SPAWN') for _ in reached]
            self.code[j][3]=len(self.code);self.continue_to(g,nid,'then')
            for s2,x in zip(spawns,reached):self.code[s2][3]=self.addr_of(g,*x)
            return
        if t=='flow.wait_until':
            loop=len(self.code)
            if (nid,'condition') in g['data']:self.value(g,nid,'condition','bool')
            else:self.emit('PUSH',arg=self.const(1))
            j=self.emit('JNZ');self.emit('YIELD');self.emit('JMP',arg=loop);self.code[j][3]=len(self.code);self.continue_to(g,nid,'then');return
        if t=='flow.random':
            self.emit('RAND');self.emit('PUSH',arg=self.const(w.get('chance',0.5)));self.emit('CMP',COMPARE['<']);j=self.emit('JZ')
            self.continue_to(g,nid,'a');self.code[j][3]=len(self.code);self.at_node(g['name'],nid);self.continue_to(g,nid,'b');return
        if self.defs[t].get('needs')=='runtime':   # defined, not in the game yet: left out (the chain goes on), named in the build warnings
            self.skipped.append(f'{g["name"]}: {self.defs[t]["label"]}');self.continue_to(g,nid,'then') if 'then' in self.defs[t]['exec']['out'] else self.emit('END');return
        if t in ('flow.for_loop','flow.while_loop'):   # one pass a frame: SPAWN the body, YIELD, next index; then Completed
            i=slot('index');self.emit('PUSH',arg=self.const(0));self.emit('STORE',arg=i);loop=len(self.code);jumps=[]
            self.emit('LOAD',arg=i)
            if t=='flow.for_loop':self.value(g,nid,'count','number')
            else:self.emit('PUSH',arg=self.const(max(1,int(w.get('max') or 1000))))
            self.emit('CMP',COMPARE['<']);jumps.append(self.emit('JZ'))
            if t=='flow.while_loop':
                if (nid,'condition') in g['data']:self.value(g,nid,'condition','bool')
                else:self.emit('PUSH',arg=self.const(1))
                jumps.append(self.emit('JZ'))
            body=self.targets(g,nid,'body');spawns=[self.emit('SPAWN') for _ in body];self.emit('YIELD')
            self.emit('LOAD',arg=i);self.emit('PUSH',arg=self.const(1));self.emit('MATH',V4_MATH.index('+'));self.emit('STORE',arg=i);self.emit('JMP',arg=loop)
            for j in jumps:self.code[j][3]=len(self.code)
            self.at_node(g['name'],nid);self.continue_to(g,nid,'completed')
            for s2,x in zip(spawns,body):self.code[s2][3]=self.addr_of(g,*x)
            return
        if t=='action.add_variable':
            v=self.var(w.get('name','my_variable'));self.emit('LOAD',arg=v);self.value(g,nid,'amount','number');self.emit('MATH',V4_MATH.index('+'));self.emit('STORE',arg=v)
            self.at_node(g['name'],nid);self.continue_to(g,nid,'then');return
        if t=='action.timer_start':
            self.emit('LIVE',arg=V4_LIVE.index('@stage_time'));self.value(g,nid,'seconds','number');self.emit('MATH',V4_MATH.index('+'));self.emit('STORE',arg=self.timer(w))
            self.at_node(g['name'],nid);self.continue_to(g,nid,'then');return
        if t=='flow.wait_timer':
            loop=len(self.code);self.emit('LOAD',arg=self.timer(w));self.emit('LIVE',arg=V4_LIVE.index('@stage_time'));self.emit('CMP',COMPARE['<='])
            j=self.emit('JNZ');self.emit('YIELD');self.emit('JMP',arg=loop);self.code[j][3]=len(self.code);self.at_node(g['name'],nid);self.continue_to(g,nid,'then');return
        if t.startswith('action.'):
            a=t.split('.',1)[1]
            if a=='set_variable':
                self.value(g,nid,'value','number');self.emit('STORE',arg=self.var(w.get('name','my_variable')))
            elif a=='set_lighting_profile':   # = Lighting trigger with the profile's / region's trigger name
                if not w.get('profile'):raise ValueError(f'{g["name"]}: Set lighting profile needs a profile (trigger name)')
                self.value(g,nid,'profile','string');self.value(g,nid,'on','bool');self.emit('ACT',V4_ACT['lighting'])
            elif a in ('mission_complete','mission_fail'):   # the game's own mission complete / fail on that route (ACT 21 / 22)
                route=ROUTE_ID.get(str(w.get('route','neutral')),1)
                if a=='mission_fail' and str(w.get('text') or '').strip():   # the reason first, as on-screen text (the stage goes on)
                    self.emit('PUSH',arg=self.const(self.string(str(w['text']).strip())));self.emit('PUSH',arg=self.const(3));self.emit('ACT',V4_ACT['message'])
                self.emit('PUSH',arg=self.const(route));self.emit('ACT',V4_ACT[a])
            elif a=='hud_value':   # "Label: value" kept on HUD line 1-3 until the node's 'hide' input runs
                line=min(3,max(1,int(w.get('slot') or 1)))
                if pin=='hide':self.emit('PUSH',arg=self.const(line));self.emit('ACT',V4_ACT['hud_hide'])
                else:
                    self.emit('PUSH',arg=self.const(self.string(str(w.get('label') or 'Value'))))
                    self.emit('PUSH',arg=self.const(self.var(str(w.get('name') or 'my_variable'))))
                    self.emit('PUSH',arg=self.const(line));self.emit('ACT',V4_ACT['hud_value'])
            elif a=='play_sound':   # the runtime plays mods\sounds\<name> or <name>.wav; an imported sound is written there by the build
                key=str(w.get('asset') or '').strip()
                if not key:raise ValueError(f'{g["name"]}: Play sound needs a sound')
                asset=next((x for x in self.p.get('customAssets',[]) if key in (x.get('id'),x.get('file'),x.get('name'))),None)
                name=''.join(ch if ch.isalnum() or ch in '-_' else '_' for ch in (asset.get('id') if asset else key))[:60]
                if asset:self.sounds[name]=asset.get('file')
                self.emit('PUSH',arg=self.const(self.string(name)));self.emit('ACT',V4_ACT['play_sound'])
            elif a=='link_trigger':   # = the game's trigger objects: switch flag[Link ID] (ACT 15)
                self.emit('PUSH',arg=self.const(min(255,max(0,int(w.get('link') or 0)))));self.emit('ACT',V4_ACT['link_set'])
            elif a=='hud_text':   # = Show text (ACT 13: text, seconds); 'Where' is not in the runtime yet (centre)
                self.value(g,nid,'text','string');self.value(g,nid,'seconds','number');self.emit('ACT',V4_ACT['message'])
            elif a=='fade_sun':   # turns on the rule the build generates for this node (server.build_lighting)
                self.emit('PUSH',arg=self.const(self.string(fade_sun_name(g['ref'],nid))));self.emit('PUSH',arg=self.const(1));self.emit('ACT',V4_ACT['lighting'])
            else:
                pins={'teleport':[('destination','object')],'set_speed':[('velocity','vector')],'rings':[('amount','number')],
                      'score':[('points','number')],'enable':[('volume','object')],'disable':[('volume','object')],
                      'lighting':[('name','string'),('on','bool')],'send':[('name','string')],'spawn':[('template','object'),('at','vector')],
                      'destroy':[('target','object')],'move':[('target','object'),('to','vector'),('seconds','number')],
                      'message':[('text','string'),('seconds','number')]}[a]
                if a in ('destroy','move'):   # : they act on SET records (kill fn_800C9B04 / record + instance move)
                    tw=self.widgets(n).get('target');link=g['data'].get((nid,'target'))
                    if not link and tw not in self.native:raise ValueError(f'{g["name"]}: {a.title()} needs a native object placed in the project')
                for p_,k in pins:self.value(g,nid,p_,k)
                self.emit('ACT',V4_ACT[a])
                if a=='spawn':
                    # VM: the template must be a native SET record placed in the project (a 'pool' record,
                    # activation 0); it is moved to 'at', activated and created; spawning a live record just moves it
                    tw=self.widgets(n).get('template');link=g['data'].get((nid,'template'))
                    if not link and tw not in self.native:raise ValueError(f'{g["name"]}: Spawn needs a native object placed in the project as its template')
                    self.emit('STORE',arg=slot('spawned'))
            self.at_node(g['name'],nid);self.continue_to(g,nid,'then');return
        raise ValueError(f'{g["name"]}: {t} cannot be entered by exec')
    # ---- v3 logic carried into v4 (the runtime runs no v3 actions when a v4 program is present)
    def legacy(self):
        vols=[];compile_events(self.p,vols);ntrig=len(self.triggers)
        with_logic={o['id'] for o in self.triggers+self.p.get('objects',[]) if o.get('logic')}
        for k,vol in enumerate(vols):
            kind,index,ref=vol['owner']
            if ref in with_logic or not vol['actions']:continue
            name=f'v3:{ref}';self.at_node(name,'event');ev='touch' if kind==3 else vol['event']
            start=len(self.code);spawns=[self.emit('SPAWN') for _ in vol['actions']];self.emit('END')
            for i,(s,a) in enumerate(zip(spawns,vol['actions'])):
                self.at_node(name,f'action{i}');self.code[s][3]=len(self.code)
                if a['delay']>0:self.emit('PUSH',arg=self.const(a['delay']));self.emit('WAIT')
                ends=[]
                for c in a['conditions']:
                    if c['name'].startswith('@'):self.emit('LIVE',arg=V4_LIVE.index(c['name']))
                    else:self.emit('LOAD',arg=self.var(c['name']))
                    self.emit('PUSH',arg=self.const(c.get('value',0)));self.emit('CMP',COMPARE[c.get('compare','==')]);ends.append(self.emit('JZ'))
                t=a['type']
                if t=='set_variable':self.emit('PUSH',arg=self.const(a['value']));self.emit('STORE',arg=self.var(a['name']))
                else:
                    if t=='teleport':
                        row=(4,0,0,*a['v'])
                        if row not in self.objects:self.objects.append(row)
                        ops=[self.objects.index(row)]
                    elif t=='set_speed':ops=list(a['v'])
                    elif t in ('rings','score'):ops=[a['value']]
                    elif t=='lighting':ops=[self.string(a['name']),a['value']]
                    elif t in ('enable','disable'):
                        row=(1,a['index'],0,0,0,0) if a['index']<ntrig else (3,a['index']-ntrig,0,0,0,0)
                        if row not in self.objects:self.objects.append(row)
                        ops=[self.objects.index(row)]
                    else:raise ValueError(f'v3 action {t} has no v4 code')
                    for x in ops:self.emit('PUSH',arg=self.const(x))
                    self.emit('ACT',V4_ACT[t])
                for j in ends:self.code[j][3]=len(self.code)
                self.emit('END')
            self.entries.append((V4_EVENTS[ev],kind,index,int(vol['once']),vol['cooldown'],start,0xFFFF))
    # ---- output
    def build(self):
        self.legacy()
        stage=self.p.get('logic')
        if stage:self.compile_owner('stage','stage',0,None,stage)
        for i,t in enumerate(self.triggers):
            if t.get('logic'):self.compile_owner(f"volume:{t['id']}",'volume',i,t['id'],t['logic'])
        for i,o in enumerate(self.pads):
            if o.get('logic'):self.compile_owner(f"pad:{o['id']}",'pad',i,o['id'],o['logic'])
        for o in self.p.get('objects',[]):
            if o.get('kind')=='native' and o.get('logic'):
                li,idx=self.native[o['id']];self.compile_owner(f"native:{o['id']}",'native',(li<<16)|idx,o['id'],o['logic'])
        if len(self.code)>65536 or len(self.entries)>4096:raise ValueError('Logic exceeds the v4 limits (65536 instructions / 4096 entries)')
        for key,name,init in self.vars:
            if name is not None:self.string(name)
        b=V4_BUDGET
        head=b'SWVM0004'+struct.pack('<14I',int(self.p.get('stage',0)),0,len(self.consts),len(self.strings),len(self.objects),len(self.vars),
            len(self.entries),len(self.code),len(self.node_names),b['per_coroutine'],b['per_frame'],b['coroutines'],b['stack'],len(self.triggers))
        assert len(head)==64
        out=bytearray(head)+struct.pack(f'<{len(self.consts)}f',*self.consts)
        s=bytearray()
        for x in self.strings:e=x.encode('utf-8');s+=struct.pack('<H',len(e))+e
        out+=s+b'\0'*(-len(s)%4)
        for row in self.objects:out+=struct.pack('<Iii3f2I',*row,0,0)
        for key,name,init in self.vars:out+=struct.pack('<HHf',0xFFFF if name is None else self.string(name),0,init)
        for ev,ok,oi,flags,period,addr,name in self.entries:out+=struct.pack('<IIiIfII',ev,ok,oi,flags,period,addr,name)
        for op,sub,nd,arg in self.code:
            if arg is None:raise ValueError('internal: unpatched jump')
            out+=struct.pack('<BBHi',op,sub,nd,arg)
        for x in self.node_names:e=x.encode('utf-8');out+=struct.pack('<H',len(e))+e
        debug=dict(skipped=self.skipped,sounds=[dict(name=k,file=v) for k,v in self.sounds.items()],constants=self.consts,strings=self.strings,objects=self.objects,variables=[dict(name=n,slot=k[1] if isinstance(k,tuple) else None,initial=i) for k,n,i in self.vars],
                   entries=[dict(event=e[0],owner_kind=e[1],owner=e[2],once=e[3],period=e[4],address=e[5],name=e[6]) for e in self.entries],
                   code=[f'{i:4d} {V4_OPNAME[op]:<9} {sub:3d} {arg:6d}  {self.node_names[nd] if nd<len(self.node_names) else ""}' for i,(op,sub,nd,arg) in enumerate(self.code)],
                   nodes=self.node_names)
        return bytes(out),debug

def needs_v4(project,nodes=None):
    """True when the project has logic only the v4 VM runs: a stage graph, or a node marked vm in nodes.json"""
    defs=nodes or load_nodes()
    if (project.get('logic') or {}).get('nodes'):return True
    for o in project.get('triggers',[])+project.get('objects',[]):
        if any(defs.get(n.get('type'),{}).get('vm') for n in (o.get('logic') or {}).get('nodes',[])):return True
    return False

def compile_graph_v4(project,nodes=None):
    """bytes of mods/stages/stgNNNN.v4.bin (SIDECAR-V4.md) + a readable debug dict (constants, entries, disassembly)."""
    return V4Compiler(project,nodes or load_nodes()).build()

def read_v4(data):
    """parse a v4 program back (the reference interpreter's loader; also a format check)"""
    if data[:8]!=b'SWVM0004':raise ValueError('not a v4 program')
    stage,flags,nc,ns,no,nv,ne,ncode,nn,bc,bf,bco,bst,v3_triggers=struct.unpack_from('<14I',data,8);p=64
    consts=list(struct.unpack_from(f'<{nc}f',data,p));p+=4*nc;strings=[]
    for _ in range(ns):l=struct.unpack_from('<H',data,p)[0];strings.append(data[p+2:p+2+l].decode('utf-8'));p+=2+l
    p+=-p%4;objects=[];variables=[];entries=[];code=[];nodes=[]
    for _ in range(no):objects.append(struct.unpack_from('<Iii3f',data,p));p+=32
    for _ in range(nv):variables.append(struct.unpack_from('<HHf',data,p));p+=8
    for _ in range(ne):entries.append(struct.unpack_from('<IIiIfII',data,p));p+=28
    for _ in range(ncode):code.append(struct.unpack_from('<BBHi',data,p));p+=8
    for _ in range(nn):l=struct.unpack_from('<H',data,p)[0];nodes.append(data[p+2:p+2+l].decode('utf-8'));p+=2+l
    if p!=len(data):raise ValueError(f'v4 program: {len(data) - p} trailing bytes')
    return dict(stage=stage,v3_triggers=v3_triggers,consts=consts,strings=strings,objects=objects,variables=variables,entries=entries,code=code,nodes=nodes,
                budget=dict(per_coroutine=bc,per_frame=bf,coroutines=bco,stack=bst))

class V4Machine:
    """Reference interpreter of SIDECAR-V4.md (tests only; the game runs the runtime's VM). fire(event, owner_kind,
    owner) starts coroutines; step(dt) advances stage time one frame; actions are appended to .log as
    (time, action name, operands)."""
    def __init__(self,data,live=None,seed=1):
        import random
        self.prog=read_v4(data);self.vars=[v[2] for v in self.prog['variables']];self.live=dict(live or {});self.time=0.0
        self.co=[];self.log=[];self.errors=[];self.dropped=0;self.last={};self.done=set();self.rng=random.Random(seed)
        self.names={self.prog['strings'][n]:i for i,(n,_,_) in enumerate(self.prog['variables']) if n!=0xFFFF}
    def var(self,name):return self.vars[self.names[name]]
    def fire(self,event,owner_kind=0,owner=0,name=None):
        ev=V4_EVENTS[event] if isinstance(event,str) else event
        for i,(e,ok,oi,flags,period,addr,nm) in enumerate(self.prog['entries']):
            if e!=ev or ok!=owner_kind or oi!=owner:continue
            if nm!=0xFFFF and self.prog['strings'][nm]!=name:continue
            if flags&1 and i in self.done:continue
            if ev!=V4_EVENTS['every'] and period and self.time-self.last.get(i,-1e9)<period:continue
            self.done.add(i);self.last[i]=self.time;self.start(addr)
    def start(self,addr):
        if len(self.co)>=self.prog['budget']['coroutines']:self.dropped+=1;return
        self.co.append(dict(pc=addr,stack=[],wake=self.time))
    def step(self,dt=1/60):
        self.time+=dt
        for i,(e,ok,oi,flags,period,addr,nm) in enumerate(self.prog['entries']):
            # every N seconds on a fixed schedule (period, 2 x period ... of stage time; no drift from frame steps)
            if e==V4_EVENTS['every'] and period>0 and self.time+1e-9>=self.last.get(i,0.0)+period:self.last[i]=self.last.get(i,0.0)+period;self.start(addr)
        total=0;k=0
        while k<len(self.co):
            c=self.co[k]
            if c['wake']>self.time+1e-9:k+=1;continue
            n,alive=self.run(c,min(self.prog['budget']['per_coroutine'],self.prog['budget']['per_frame']-total));total+=n
            if not alive:self.co.pop(k)
            else:k+=1
            if total>=self.prog['budget']['per_frame']:break
    def run(self,c,budget):
        code=self.prog['code'];st=c['stack'];n=0;C=self.prog['consts']
        def node():return self.prog['nodes'][code[c['pc']][2]] if c['pc']<len(code) else '?'
        while n<budget:
            if not 0<=c['pc']<len(code):self.errors.append(('pc out of range',node()));return n,False
            op,sub,nd,arg=code[c['pc']];c['pc']+=1;n+=1;name=V4_OPNAME[op]
            if len(st)>self.prog['budget']['stack']:self.errors.append(('stack overflow',self.prog['nodes'][nd]));return n,False
            if name=='END':return n,False
            elif name=='PUSH':st.append(C[arg])
            elif name=='LOAD':st.append(self.vars[arg])
            elif name=='STORE':self.vars[arg]=st.pop()
            elif name=='LIVE':st.append(float(self.live.get(V4_LIVE[arg],0)))
            elif name=='MATH':
                b,a=st.pop(),st.pop();st.append([a+b,a-b,a*b,a/b if b else 0.0,min(a,b),max(a,b)][sub])
            elif name=='CMP':
                b,a=st.pop(),st.pop();st.append(float([a==b,a!=b,a<b,a<=b,a>b,a>=b][sub]))
            elif name=='NOT':st.append(0.0 if st.pop() else 1.0)
            elif name=='JMP':c['pc']=arg
            elif name=='JZ':
                if not st.pop():c['pc']=arg
            elif name=='JNZ':
                if st.pop():c['pc']=arg
            elif name=='WAIT':c['wake']=self.time+min(3600.0,max(0.0,st.pop()));return n,True
            elif name=='YIELD':c['wake']=self.time+1e-6;return n,True
            elif name=='SPAWN':self.start(arg)
            elif name=='RAND':st.append(self.rng.random())
            elif name=='PLAYERPOS':st.extend(self.live.get('player_position',[0.0,0.0,0.0]))
            elif name=='OBJPOS':
                o=self.prog['objects'][int(st.pop())];st.extend(list(o[3:6]) if sub==0 else [1.0])
            elif name=='DUP':st.append(st[-1])
            elif name=='POP':st.pop()
            elif name=='ACT':
                act={v:k for k,v in V4_ACT.items()}[sub];want={'teleport':1,'set_speed':3,'rings':1,'score':1,'lighting':2,'enable':1,'disable':1,
                     'send':1,'spawn':4,'destroy':1,'move':5,'message':2,'link_set':1,'mission_complete':1,'mission_fail':1,
                     'hud_value':3,'hud_hide':1,'play_sound':1}[act]
                args=st[-want:];del st[-want:]
                self.log.append((round(self.time,4),act,args))
                if act=='rings':self.live['@rings']=min(9999.0,max(0.0,args[0]))   # SET, like v3
                if act=='score':self.live['@score']=self.live.get('@score',0)+args[0]
                if act=='send':self.fire('custom',0,0,self.prog['strings'][int(args[0])])
                if act=='spawn':st.append(float(args[0]))   # : the spawned object IS the template record (index pushed back)
        return n,True
