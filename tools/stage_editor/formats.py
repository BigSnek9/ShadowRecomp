"""Shadow GC stage formats. Independently implemented from inspected files and public format facts.
All layouts/RW headers are little endian; native GX arrays/display lists are big endian.
Unknown placement bytes and source archives are retained. Never writes the extracted disc.
"""
from __future__ import annotations
import base64, math, struct, sys
from pathlib import Path
import numpy as np
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import one_archive as one

VER=0x1c020037
def chunk(t,b,ver=VER): return struct.pack('<III',t,len(b),ver)+b
def children(b):
    p=0; out=[]
    while p+12<=len(b):
        t,n,v=struct.unpack_from('<III',b,p)
        if p+12+n>len(b): raise ValueError('Truncated RenderWare chunk')
        out.append((t,b[p+12:p+12+n],v));p+=12+n
    if p!=len(b): raise ValueError('Trailing RenderWare bytes')
    return out
def first(b,t): return next((s for k,s,v in children(b) if k==t),None)
def serialize(cs): return b''.join(chunk(t,b,v) for t,b,v in cs)
def b64(b): return base64.b64encode(b).decode()
def unb64(s): return base64.b64decode(s,validate=True)

def read_set(data,layer):
    if data[:4]!=b'sky2' or len(data)<12: raise ValueError('Not a Shadow SET layout')
    count,total=struct.unpack_from('<II',data,4)
    if count>100000 or 12+44*count+total>len(data): raise ValueError('Invalid SET lengths')
    p=12+44*count;out=[]
    for i in range(count):
        row=data[12+44*i:56+44*i]
        values=struct.unpack_from('<6f',row);n=struct.unpack_from('<I',row,36)[0]
        if p+n>len(data):raise ValueError('Truncated object parameters')
        out.append(dict(id=f'{layer}-{i}',kind='native',layer=layer,position=list(values[:3]),rotation=list(values[3:]),
          scale=[1,1,1],list=row[33],type=row[32],link=row[34],render=row[35],unknown=b64(row[24:32]),
          trailing=b64(row[40:44]),params=b64(data[p:p+n]),sourceIndex=i))
        p+=n
    if p!=12+44*count+total:raise ValueError('SET parameter totals disagree')
    return out,b64(data[p:])

# Bytes 1 and 5 of a record's 8 'unknown' bytes: high nibble = the layers it belongs to (cmn 0x10, nrm 0x20, hrd 0x40,
# ds1 0x80; 0x50 = cmn+hrd seen in stg0601), low bits = flags (0x02, 0x08). An object saved into a layer whose bit it
# lacks (copied from another layer) gets that bit; originals are unchanged.
LAYER_CODES={'cmn':0x10,'nrm':0x20,'hrd':0x40,'ds1':0x80}
def write_set(objects,tail='',layer=None):
    rows=[];params=[];code=LAYER_CODES.get(layer)
    for o in objects:
        pos=[float(x) for x in o['position']];rot=[float(x) for x in o['rotation']]
        if len(pos)!=3 or len(rot)!=3 or not all(math.isfinite(x) for x in pos+rot):raise ValueError('Invalid transform')
        u=unb64(o['unknown']);t=unb64(o.get('trailing','AAAAAA=='));p=unb64(o['params'])
        if len(u)!=8 or len(t)!=4:raise ValueError('Invalid preserved native object bytes')
        if code is not None and not (u[1]&code):   # moved from another layer: that layer's bit, flags kept
            u=bytearray(u);u[1]=(u[1]&0x0F)|code;u[5]=(u[5]&0x0F)|code;u=bytes(u)
        rows.append(struct.pack('<6f',*pos,*rot)+u+bytes([o['type'],o['list'],o['link'],o['render']])+struct.pack('<I',len(p))+t)
        params.append(p)
    return b'sky2'+struct.pack('<II',len(rows),sum(map(len,params)))+b''.join(rows)+b''.join(params)+unb64(tail)

def materials(world):
    out=[];ml=first(world,8)
    for t,b,v in children(ml or b''):
        if t!=7:continue
        tex=first(b,6);name=''
        if tex:
            name=next((s.split(b'\0')[0].decode('ascii','replace') for k,s,_ in children(tex) if k==2),'')
        st=first(b,1);out.append(dict(texture=name,colour=list(st[4:8]),raw=b64(chunk(7,b,v))))
    return out

def sectors(world):
    out=[]
    def visit(b):
        for t,s,v in children(b):
            if t==9:out.append(s)
            elif t==10:visit(s)
    visit(world);return out

def native_mesh(sector):
    st=first(sector,1);_,nt,nv=struct.unpack_from('<III',st)
    ext=first(sector,3);bm=first(ext,0x50e);native=first(ext,0x510)
    if not native:return None
    body=first(native,1)
    platform,hlen,dlen=struct.unpack_from('<III',body)
    if platform!=6:raise ValueError('Only GameCube native stage meshes are supported')
    count=struct.unpack_from('>I',body,20)[0];p=24;decl=[]
    for i in range(count):
        off,kind,stride,itype,unk=struct.unpack_from('>IBBBB',body,p);p+=8;decl.append((off,kind,stride,itype))
    nm=struct.unpack_from('<I',bm,4)[0];mats=[struct.unpack_from('<II',bm,12+i*8)[1] for i in range(nm)]
    ranges=[]
    for m in mats:
        off,n=struct.unpack_from('>II',body,p);p+=8;ranges.append((m,off,n))
    base=12+hlen
    if base!=p or base+dlen>len(body):raise ValueError('Invalid native mesh header')
    verts=[];cols=[];uvs=[];tris=[];tri_m=[];cache={}
    def vertex(indices):
        key=tuple(indices)
        if key in cache:return cache[key]
        pos=(0,0,0);col=(255,255,255,255);uv=(0,0)
        for ix,(off,k,stride,typ) in zip(indices,decl):
            at=base+off+ix*stride
            if at+stride>len(body):raise ValueError('Invalid native vertex index')
            if k==9:pos=struct.unpack_from('>3f',body,at)
            elif k==11:col=tuple(body[at:at+4])
            elif k==13:uv=struct.unpack_from('>2f',body,at)
        idx=len(verts);cache[key]=idx;verts.append(pos);cols.append(col);uvs.append(uv);return idx
    for mat,off,n in ranges:
        q=base+off;end=q+n
        while q<end:
            op=body[q];q+=1
            if op==0:continue
            if op not in (0x98,0x90):raise ValueError(f'Unsupported GX stage primitive {op:#x}')
            amount=struct.unpack_from('>H',body,q)[0];q+=2;run=[]
            for _ in range(amount):
                indices=[]
                for off,k,stride,typ in decl:
                    sz=1 if typ==2 else 2 if typ==3 else 0
                    if not sz:raise ValueError('Unsupported GX index size')
                    indices.append(int.from_bytes(body[q:q+sz],'big'));q+=sz
                run.append(vertex(indices))
            for i in (range(0,len(run)-2,3) if op==0x90 else range(len(run)-2)):
                tri=run[i:i+3]
                if op==0x98 and i&1:tri=[tri[1],tri[0],tri[2]]
                if len(set(tri))==3:tris.append(tri);tri_m.append(mat)
        if q!=end:raise ValueError('Invalid native display list length')
    return dict(vertices=verts,colours=cols,uvs=uvs,triangles=tris,materials=tri_m)

def read_world(data,collision=False):
    world=first(data,11)
    if world is None:raise ValueError('No RenderWare world')
    meshes=[]
    for s in sectors(world):
        st=first(s,1);base,nt,nv=struct.unpack_from('<III',st)
        if not nv:continue
        if len(st)==44:
            m=native_mesh(s)
            if m:meshes.append(m)
            continue
        v=np.frombuffer(st,'<f4',nv*3,44).reshape(-1,3).tolist()
        tri=np.frombuffer(st,'<u2',nt*4,len(st)-nt*8).reshape(-1,4)
        m=dict(vertices=v,triangles=tri[:,:3].tolist(),materials=(tri[:,3]+base).tolist())
        if not collision:
            m['colours']=np.frombuffer(st,'u1',nv*4,44+nv*12).reshape(-1,4).tolist()
            m['uvs']=np.frombuffer(st,'<f4',nv*2,44+nv*16).reshape(-1,2).tolist()
        else:
            ud=first(first(s,3),0x11f)
            # per-face words: the 'attribute' entry of the sector's user data (#39: found by name, not position)
            attrs=user_data(ud).get('attribute') if ud else None
            m['flags']=list(attrs) if attrs is not None and len(attrs)==nt else [0x00020001]*nt
        meshes.append(m)
    return dict(meshes=meshes,materials=materials(world))

# ---- #47: move / rotate / scale original stage art. A world's pieces are its sectors in read_world order (the
# editor's reference ids visual-<chunk>-<world>-<i>). Stage worlds are prelit (no normals: native sectors carry
# positions as big-endian floats (GX attribute 9), colours (11) and UV sets (13 / 14)), so only positions change.
def _native_position_indices(body):
    """(offset of the position array in body, highest position index the display lists use) of a native sector"""
    platform,hlen,dlen=struct.unpack_from('<III',body)
    count=struct.unpack_from('>I',body,20)[0];p=24;decl=[]
    for i in range(count):
        off,kind,stride,itype,unk=struct.unpack_from('>IBBBB',body,p);p+=8;decl.append((off,kind,stride,itype))
    base=12+hlen;ranges=[]
    while p<base:
        off,n=struct.unpack_from('>II',body,p);p+=8;ranges.append((off,n))
    pos=next((i for i,d in enumerate(decl) if d[1]==9),None)
    if pos is None or decl[pos][2]!=12:raise ValueError('native sector without float positions')
    top=-1
    for off,n in ranges:
        q=base+off;end=q+n
        while q<end:
            op=body[q];q+=1
            if op==0:continue
            if op not in (0x98,0x90):raise ValueError(f'Unsupported GX stage primitive {op:#x}')
            amount=struct.unpack_from('>H',body,q)[0];q+=2
            for _ in range(amount):
                for j,(o,k,stride,typ) in enumerate(decl):
                    sz=1 if typ==2 else 2
                    if j==pos:top=max(top,int.from_bytes(body[q:q+sz],'big'))
                    q+=sz
    return base+decl[pos][0],top

def transform_world(data,pieces):
    """data with the given pieces ({piece index: 4x4 matrix, column vectors, applied as M @ [x, y, z, 1]}) moved;
    returns (bytes, {piece index: (old positions as an (n, 3) array, new low, new high)}). Sector boxes, plane-sector
    extents and the world box are recomputed; untouched sectors keep their bytes."""
    cs=children(data);wi=next(i for i,c in enumerate(cs) if c[0]==11);wc=children(cs[wi][1])
    counter=[0];moved={}
    def sector(t,b,v):
        if t==9:
            parts=children(b);st=bytearray(first(b,1));base_,nt,nv=struct.unpack_from('<III',st)
            has=bool(nv) and (len(st)!=44 or first(first(b,3) or b'',0x510) is not None)
            idx=counter[0] if has else None
            if has:counter[0]+=1
            if idx not in pieces:
                lo=np.array(struct.unpack_from('<3f',st,24));hi=np.array(struct.unpack_from('<3f',st,12));return chunk(t,b,v),((lo,hi) if nv else None),False
            M=np.asarray(pieces[idx],float)
            if len(st)==44:
                ext=first(b,3);ec=children(ext);ni=next(i for i,c in enumerate(ec) if c[0]==0x510);nc=children(ec[ni][1])
                bi=next(i for i,c in enumerate(nc) if c[0]==1);body=bytearray(nc[bi][1]);at,top=_native_position_indices(bytes(body))
                old=np.frombuffer(bytes(body[at:at+12*(top+1)]),'>f4').reshape(-1,3).astype(float)
                new=old@M[:3,:3].T+M[:3,3];body[at:at+12*(top+1)]=new.astype('>f4').tobytes()
                nc[bi]=(1,bytes(body),nc[bi][2]);ec[ni]=(0x510,serialize(nc),ec[ni][2])
                parts=[(c[0],serialize(ec),c[2]) if c[0]==3 else c for c in parts]
            else:
                old=np.frombuffer(bytes(st[44:44+12*nv]),'<f4').reshape(-1,3).astype(float)
                new=old@M[:3,:3].T+M[:3,3];st[44:44+12*nv]=new.astype('<f4').tobytes()
            lo,hi=new.min(axis=0),new.max(axis=0);struct.pack_into('<3f',st,12,*hi);struct.pack_into('<3f',st,24,*lo)
            parts=[(1,bytes(st),c[2]) if c[0]==1 else c for c in parts];moved[idx]=(old,lo,hi)
            return chunk(t,serialize(parts),v),(lo,hi),True
        parts=children(b);plane=bytearray(parts[0][1]);out=[parts[0]];boxes=[];changed=False
        for c in parts[1:]:
            if c[0] in (9,10):cb,box,ch=sector(*c);out.append((c[0],cb[12:],c[2]));boxes.append(box);changed|=ch
            else:out.append(c)
        axis=struct.unpack_from('<I',plane)[0]//4;l,r=(boxes+[None,None])[:2]
        if changed and l is not None and r is not None:
            # both sides may now overlap along the split axis: exact extents keep box queries / culling correct
            struct.pack_into('<f',plane,16,float(l[1][axis]));struct.pack_into('<f',plane,20,float(r[0][axis]))
            out[0]=(1,bytes(plane),parts[0][2])
        box=[b for b in boxes if b is not None]
        return chunk(t,serialize(out),v),((np.min([b[0] for b in box],axis=0),np.max([b[1] for b in box],axis=0)) if box else None),changed
    ri=next(i for i,c in enumerate(wc) if c[0] in (9,10));rb,box,_=sector(*wc[ri]);wc[ri]=(wc[ri][0],rb[12:],wc[ri][2])
    missing=set(pieces)-set(moved)
    if missing:raise ValueError(f'no piece {sorted(missing)[0]} in this world ({counter[0]} pieces)')
    si=next(i for i,c in enumerate(wc) if c[0]==1);st=bytearray(wc[si][1])
    if moved and box is not None:   # the world box only grows (originals keep their own margins)
        hi=np.maximum(struct.unpack_from('<3f',st,40),box[1]);lo=np.minimum(struct.unpack_from('<3f',st,52),box[0])
        struct.pack_into('<3f',st,40,*hi);struct.pack_into('<3f',st,52,*lo)
    wc[si]=(1,bytes(st),wc[si][2]);cs[wi]=(11,serialize(wc),cs[wi][2])
    return serialize(cs),moved

def user_data(ud):
    """RenderWare user data plugin (0x11F): {name: values}; u32 count, then per entry u32 name length, name, u32 type
    (1 int, 2 float, 3 string), u32 count, values. Collision sectors carry 'attribute' (one u32 per face) and
    'FVF.UserData'."""
    out={};p=4
    for _ in range(struct.unpack_from('<I',ud)[0]):
        n=struct.unpack_from('<I',ud,p)[0];name=ud[p+4:p+4+n].split(b'\0')[0].decode('ascii','replace');p+=4+n
        typ,count=struct.unpack_from('<II',ud,p);p+=8
        if typ in (1,2):out[name]=struct.unpack_from('<'+('I' if typ==1 else 'f')*count,ud,p);p+=4*count
        elif typ==3:
            vals=[]
            for _ in range(count):
                n=struct.unpack_from('<I',ud,p)[0];vals.append(ud[p+4:p+4+n].split(b'\0')[0].decode('ascii','replace'));p+=4+n
            out[name]=vals
        else:raise ValueError(f'Unknown RenderWare user data type {typ}')
    return out

def make_native_sector(vertices,triangles,uvs,colours,mat=0):
    v=np.asarray(vertices,dtype='>f4');uv=np.asarray(uvs,dtype='>f4')
    unique=list(dict.fromkeys(tuple(c) for c in colours))
    if len(unique)>256:raise ValueError('Native colour palette exceeds 256 entries')
    c=np.asarray(unique,dtype='u1');colour_indices={c:i for i,c in enumerate(unique)}
    if len(v)>65535:raise ValueError('Split geometry: more than 65535 vertices per sector')
    low=v.astype(float).min(axis=0);high=v.astype(float).max(axis=0)
    display=bytearray()
    # Independent three-vertex strips preserve winding and avoid index remapping ambiguity.
    for tri in triangles:
        display+=b'\x98\x00\x03'
        for idx in tri:display+=struct.pack('>HBH',idx,colour_indices[tuple(colours[idx])],idx)
    display+=b'\0'*((-len(display))%32)
    pieces=[bytes(display),v.tobytes(),c.tobytes(),uv.tobytes()];payload=bytearray();offsets=[]
    for b in pieces:
        offsets.append(len(payload));payload+=b;payload+=b'\0'*((-len(payload))%32)
    hdr=struct.pack('>HHII',0x1161,0,1,3)
    for off,k,stride,typ in zip(offsets[1:],[9,11,13],[12,4,8],[3,2,3]):hdr+=struct.pack('>IBBBB',off,k,stride,typ,0)
    hdr+=struct.pack('>II',0,len(display))
    nd=struct.pack('<III',6,len(hdr),len(payload))+hdr+payload
    bm=struct.pack('<IIIII',1,1,len(triangles)*3,len(triangles)*3,mat)
    st=struct.pack('<III6fII',0,len(triangles),len(v),*high,*low,0,0)
    fvf=struct.pack('<II',1,13)+b'FVF.UserData\0'+struct.pack('<III',1,1,0x3003)
    return chunk(9,chunk(1,st)+chunk(3,chunk(0x50e,bm)+chunk(0x510,chunk(1,nd))+chunk(0x11f,fvf)))

def append_visual(source,mesh,material):
    cs=children(source);wi=next(i for i,c in enumerate(cs) if c[0]==11);wc=children(cs[wi][1])
    si=next(i for i,c in enumerate(wc) if c[0]==1);st=bytearray(wc[si][1])
    root=next(i for i,c in enumerate(wc) if c[0] in (9,10));old=chunk(*[wc[root][0],wc[root][1],wc[root][2]])
    ml=next(i for i,c in enumerate(wc) if c[0]==8);mc=children(wc[ml][1]);n=struct.unpack_from('<I',mc[0][1])[0]
    mat_index=next((i for i,m in enumerate(materials(cs[wi][1])) if m['texture']==material['texture']),None)
    if mat_index is None:
        mat_index=n;mc[0]=(1,struct.pack('<I',n+1)+mc[0][1][4:]+struct.pack('<i',-1),VER)
        mc+=children(unb64(material['raw']));wc[ml]=(8,serialize(mc),VER)
    v=np.asarray(mesh['vertices']);low=v.min(axis=0);high=v.max(axis=0)
    oldhigh=np.array(struct.unpack_from('<3f',st,40));oldlow=np.array(struct.unpack_from('<3f',st,52))
    new=make_native_sector(v,mesh['triangles'],mesh['uvs'],mesh['colours'],mat_index)
    # Overlapping plane branches retain authored sector tree and permit additions anywhere.
    plane=struct.pack('<IfIIff',0,0.0,int(wc[root][0]==9),1,float(oldhigh[0]),float(low[0]))
    wc[root]=(10,chunk(1,plane)+old+new,VER)
    struct.pack_into('<I',st,0,0)
    for at,add in [(16,len(mesh['triangles'])),(20,len(v)),(24,1),(28,1)]:struct.pack_into('<I',st,at,struct.unpack_from('<I',st,at)[0]+add)
    struct.pack_into('<3f',st,40,*np.maximum(high,oldhigh));struct.pack_into('<3f',st,52,*np.minimum(low,oldlow))
    wc[si]=(1,bytes(st),VER);cs[wi]=(11,serialize(wc),VER);return serialize(cs)

def empty_world(source):
    """Retain a world's format/materials, replacing all old sectors by an empty leaf."""
    cs=children(source);wi=next(i for i,c in enumerate(cs) if c[0]==11);wc=children(cs[wi][1])
    si=next(i for i,c in enumerate(wc) if c[0]==1);st=bytearray(wc[si][1])
    root=next(i for i,c in enumerate(wc) if c[0] in (9,10))
    struct.pack_into('<I',st,0,1)
    struct.pack_into('<4I',st,16,0,0,0,1)
    struct.pack_into('<6f',st,40,0,0,0,0,0,0)
    wc[si]=(1,bytes(st),VER)
    wc[root]=(9,chunk(1,struct.pack('<III6fII',0,0,0,0,0,0,0,0,0,0,0))+chunk(3,b''),VER)
    cs[wi]=(11,serialize(wc),cs[wi][2]);return serialize(cs)

def make_collision(mesh):
    v=np.asarray(mesh['vertices'],dtype='<f4');tri=np.asarray(mesh['triangles'],dtype='<u2');nt=len(tri)
    if len(v)>65535 or nt>65535:raise ValueError('Collision sector exceeds 16-bit index limits')
    low=v.min(axis=0);high=v.max(axis=0);splits=[];refs=[]
    def branch(ids,axis,negative):
        if not ids:return (axis*4+int(negative),0,0,0.)
        bounds=v[tri[ids]].reshape(-1,3);pos=float(bounds[:,axis].max() if negative else bounds[:,axis].min())
        if len(ids)<=200:
            at=len(refs);refs.extend(ids);return(axis*4+int(negative),len(ids),at,pos)
        idx=split(ids);return(axis*4+int(negative),255,idx,pos)
    def split(ids):
        idx=len(splits);splits.append(None)
        centres=v[tri[ids]].mean(axis=1);axis=int(np.ptp(centres,axis=0).argmax())
        order=[ids[i] for i in np.argsort(centres[:,axis])];mid=max(1,len(order)//2)
        a=branch(order[:mid],axis,True);b=branch(order[mid:],axis,False);splits[idx]=(a,b);return idx
    split(list(range(nt)))
    tree=struct.pack('<I6fII',1,*low,*high,len(refs),len(splits))
    for a,b in splits:tree+=struct.pack('<BBHfBBHf',*a,*b)
    tree+=struct.pack('<'+'H'*len(refs),*refs)
    flags=mesh.get('flags',[0x00020001]*nt)
    ud=struct.pack('<II',2,10)+b'attribute\0'+struct.pack('<II',1,nt)+struct.pack('<'+'I'*nt,*flags)
    ud+=struct.pack('<I',13)+b'FVF.UserData\0'+struct.pack('<III',1,1,0x3003)
    indices=tri.flatten().astype('<u4').tobytes();bm=struct.pack('<IIIII',0,1,nt*3,nt*3,0)+indices
    ext=chunk(0x50e,bm)+chunk(0x11d,struct.pack('<I',0x37002)+chunk(0x2c,chunk(1,tree)))+chunk(0x11f,ud)
    ts=np.column_stack([tri,np.zeros(nt,dtype='<u2')]).astype('<u2').tobytes()
    sector=chunk(9,chunk(1,struct.pack('<III6fII',0,nt,len(v),*high,*low,0x12f410,0)+v.tobytes()+ts)+chunk(3,ext))
    material=chunk(7,chunk(1,struct.pack('<I4BII3f',0,255,255,255,255,0,0,1.,1.,1.))+chunk(3,b''))
    ml=chunk(8,chunk(1,struct.pack('<Ii',1,-1))+material)
    ws=struct.pack('<I3f6I6f',1,0.,0.,0.,nt,len(v),0,1,0,0x40000041,*high,*low)
    return chunk(11,chunk(1,ws)+ml+sector+chunk(3,b''))

def combine(meshes):
    v=[];tri=[];uv=[];col=[];flags=[]
    for m in meshes:
        off=len(v);v+=m['vertices'];tri += [[x+off for x in t] for t in m['triangles']]
        uv+=m.get('uvs',[[0,0]]*len(m['vertices']));col+=m.get('colours',[[255]*4]*len(m['vertices']))
        flags+=m.get('flags',[0x00020001]*len(m['triangles']))
    return dict(vertices=v,triangles=tri,uvs=uv,colours=col,flags=flags)
