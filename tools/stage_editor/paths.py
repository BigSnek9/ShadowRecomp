"""Shadow GameCube PATH.PTP: big endian structures and POF0 relative relocations."""
import math,struct
import numpy as np

def read_paths(b):
    if len(b)<48 or struct.unpack_from('>I',b,20)[0]!=12610:raise ValueError('Not a GameCube PTP path file')
    out=[];q=32
    while q+4<=len(b):
        off=struct.unpack_from('>I',b,q)[0];q+=4
        if not off:break
        at=off+32
        if at+48>len(b):raise ValueError('Invalid PTP pointer')
        n,length,verts,settings=struct.unpack_from('>IfII',b,at)
        sid=struct.unpack_from('>i',b,at+28)[0];nameoff=struct.unpack_from('>I',b,at+44)[0]+32
        if n>100000 or verts+32+n*32>len(b) or nameoff>=len(b):raise ValueError('Invalid PTP path size')
        name=b[nameoff:b.find(b'\0',nameoff)].decode('ascii','replace');points=[];rot=[];tolerances=[]
        for i in range(n):
            row=struct.unpack_from('>7fi',b,verts+32+i*32);points.append(list(row[:3]));rot.append(list(row[3:6]));tolerances.append(row[7])
        typ=(settings>>8)&255
        out.append(dict(id=f'native-path-{len(out)}',kind='path',name=name,position=[0,0,0],rotation=[0,0,0],scale=[1,1,1],
          pathType='platform' if typ==8 else 'rail' if typ==2 else 'loop' if typ==1 else 'native',closed=False,points=points,banking=0,
          splineId=sid,settings=settings,nativeRotations=rot,tolerances=tolerances,autoOrient=False,sourceIndex=len(out)))
    return out

def world_points(p):
    v=np.asarray(p['points'],float)
    if v.ndim!=2 or v.shape[1]!=3 or len(v)<2 or not np.isfinite(v).all():raise ValueError(f"{p['name']}: a path needs at least two finite points")
    x,y,z=np.radians(p.get('rotation',[0,0,0]));cx,sx,cy,sy,cz,sz=np.cos(x),np.sin(x),np.cos(y),np.sin(y),np.cos(z),np.sin(z)
    rx=np.array([[1,0,0],[0,cx,-sx],[0,sx,cx]]);ry=np.array([[cy,0,sy],[0,1,0],[-sy,0,cy]]);rz=np.array([[cz,-sz,0],[sz,cz,0],[0,0,1]])
    v=(v*np.asarray(p.get('scale',[1]*3)))@(ry@rx@rz).T+np.asarray(p.get('position',[0]*3))
    if p.get('closed') and np.linalg.norm(v[-1]-v[0])>.001:v=np.vstack([v,v[0]])
    return v

def point_frame(direction,banking=0):
    """#55: a point's (rx, ry, rz) in radians as the game stores them: frame = Rz(rz) @ Rx(rx) @ Ry(ry) with forward
    = frame @ (0, 0, -1) along the segment to the NEXT point (fitted on 908 / 908 original rail and platform points;
    the last point repeats the previous segment) and banking (degrees) as roll about that forward axis."""
    f=np.asarray(direction,float);n=np.linalg.norm(f);f=f/n if n else np.array([0.,0.,-1.])
    up=np.array([0.,1.,0.]) if abs(f[1])<0.999 else np.array([0.,0.,1.]);up=up-f*np.dot(up,f);up/=np.linalg.norm(up)
    b=math.radians(banking);up=up*math.cos(b)+np.cross(f,up)*math.sin(b)
    z=-f;x=np.cross(up,z);M=np.column_stack([x,up,z])
    rx=math.asin(np.clip(M[2,1],-1,1));ry=math.atan2(-M[2,0],M[2,2]);rz=math.atan2(-M[0,1],M[1,1])
    return [rx,ry,rz]

def loop_frames(points):
    """Loop paths (settings 0x100): per-point frames and section flags as the game's loops store them
    (fitted on stg0201_lp_001: pitch within 0.4 deg, heading within 3.7 deg, flags exact). The frame has no roll: heading
    psi = the run-in direction, theta = the angle of travel in the loop plane; |theta| <= 90: (theta, psi, 0), else the
    upside-down form (+-180 - theta, psi + 180, 180). Flags: 1, + 4 while upright, + 2 from the top through the descent,
    0 on the last point (run-in 5, upper front 1, upper back 3, lower back 7, run-out 5)."""
    P=np.asarray(points,float);n=len(P);h=P[1]-P[0];h[1]=0
    h=h/np.linalg.norm(h) if np.linalg.norm(h)>1e-6 else np.array([0.,0.,-1.])
    psi=point_frame(np.array([h[0],0.,h[2]]))[1];rot=[];flags=[];top=int(np.argmax(P[:,1]))
    for i in range(n):
        f=P[i+1]-P[i] if i<n-1 else P[i]-P[i-1]
        th=math.atan2(f[1],float(np.dot(f,h)))
        if abs(th)<=math.pi/2:r=[th,psi,0.0]
        else:r=[(math.pi-th) if th>0 else (-math.pi-th),math.remainder(psi+math.pi,2*math.pi),math.pi if th<0 else -math.pi]
        rot.append(r)
        b=0 if i==n-1 else 1|(4 if r[2]==0.0 else 0)|(2 if i>=top and (f[1]<-0.15*np.linalg.norm(f) or r[2]!=0.0) else 0)
        flags.append(b)
    return rot,flags

def write_paths(paths):
    if len(paths)>4096:raise ValueError('Too many paths')
    ids=[int(p.get('splineId',1000+i)) for i,p in enumerate(paths)]
    # The game's own data repeats some IDs (Cryptic Castle: 212 twice), so imported paths may share one; a path made
    # in the editor needs an ID no other path uses.
    for i,p in enumerate(paths):
        if 'sourceIndex' not in p and ids.count(ids[i])>1:raise ValueError(f"{p['name']}: spline ID {ids[i]} is already used by another path")
    data=bytearray(32+((len(paths)+1)*4+15)//16*16);offsets=[];ptrs=[]
    for i,p in enumerate(paths):
        points=world_points(p)
        if p.get('pathType')=='rail' and 'sourceIndex' not in p:
            # #55: the game turns the grinding player from one point's direction to the next ACROSS a segment while the
            # position follows the straight line; on a long climbing segment before a descent the direction swung down
            # mid-climb and the player dropped (run, z ~ -181 of a 300-unit segment). Original rails are dense
            # (points ~10-100 apart), so a new rail is subdivided to segments of at most 50 units.
            dense=[points[0]]
            for a,b in zip(points[:-1],points[1:]):
                n=max(1,int(math.ceil(np.linalg.norm(b-a)/50)))
                dense+=[a+(b-a)*(k/n) for k in range(1,n+1)]
            points=np.asarray(dense)
        count=len(points);at=len(data);offsets.append(at-32);ptrs.extend([i*4,at-32+8,at-32+44])
        distances=np.linalg.norm(np.diff(points,axis=0),axis=1);distances=np.r_[distances,0.]
        rotations=p.get('nativeRotations',[])
        loop_flags=None
        if p.get('pathType')=='loop' and (p.get('autoOrient',True) or len(rotations)!=count):
            rotations,loop_flags=loop_frames(points)   # a loop needs the game's roll-free loop frames + section flags
        elif p.get('autoOrient',True) or len(rotations)!=count:
            rotations=[point_frame(points[j+1]-points[j] if j<count-1 else points[j]-points[j-1],p.get('banking',0)) for j in range(count)]
        # settings word: imported paths keep theirs (the old code OR'd 1 into every rail / platform - 196 originals
        # changed on a rewrite); a new path, or one whose kind changed, gets the usual original word: rail 0x200 (168 of
        # 178 rails), platform 0x801 (ShadowSET: path-following platforms need a nearby spline with type 8 and low byte 1)
        typ={'platform':8,'rail':2,'loop':1}.get(p.get('pathType'));settings=p.get('settings')
        if settings is None or (typ is not None and ((int(settings)>>8)&255)!=typ):settings={8:0x801,2:0x200,1:0x100}.get(typ,0x200)
        settings=int(settings)
        data+=struct.pack('>IfII3fi3fI',count,float(distances.sum()),at-32+48,settings,*points.max(axis=0),ids[i],*points.min(axis=0),0)
        tol=loop_flags if loop_flags is not None and len(p.get('tolerances',[]))!=count else p.get('tolerances',[])
        for j,point in enumerate(points):data+=struct.pack('>7fi',*point,*rotations[j],distances[j],int(tol[j] if j<len(tol) else 4 if j<count-1 else 0))
    for i,p in enumerate(paths):
        struct.pack_into('>I',data,32+i*4,offsets[i]);struct.pack_into('>I',data,32+offsets[i]+44,len(data)-32)
        name=p['name'].encode('ascii','replace')
        if len(name)>255:raise ValueError('Path name exceeds255 bytes')
        data+=name+b'\0'
    data+=b'\0'*((-len(data))%4);pof_at=len(data)-32;pof=bytearray();last=0
    for loc in sorted(ptrs):
        delta=(loc-last)//4;last=loc
        pof+=bytes([0x40|delta]) if delta<64 else struct.pack('>H',0x8000|delta) if delta<16384 else struct.pack('>I',0xc0000000|delta)
    pof+=b'\0'*((-len(pof))%4);data+=pof+b'\0'*8+b'Stage Workshop\0';data+=b'\0'*((-len(data))%4)
    struct.pack_into('>8I',data,0,len(data),pof_at,len(pof),1,0,12610,0,0)
    return bytes(data)

# ---- enemy path presets (2026-10-17, docs/stage-workshop/research/appear-types.md): enemy/EnemyPath.one holds
# ENEPATH01_EN.PTB .. ENEPATH08_EN.PTB, ENEPATH09_CR_EN.PTB = the 9 PathType shapes flyers (GUN Beetle 00/65, Bk Wing
# 00/8E / 00/8F) fly with AppearType MOVE_ON_PATH (archive row = {2..10}[PathType], fn_801A6790). Each file is a Sega
# POF table whose data block is ONE PATH.PTP spline entry (48-byte header {count, length, points ptr 0x30, settings
# 0x400 (09: 0x402), max xyz, id 0, min xyz, name ptr} + 32-byte points {x, y, z, rx, ry, rz, length to next, flag})
# + the authoring name; points are LOCAL to the enemy (fn_801A6688 places them at its position / rotation).
PTB_FILES=['ENEPATH01_EN.PTB','ENEPATH02_EN.PTB','ENEPATH03_EN.PTB','ENEPATH04_EN.PTB','ENEPATH05_EN.PTB','ENEPATH06_EN.PTB',
           'ENEPATH07_EN.PTB','ENEPATH08_EN.PTB','ENEPATH09_CR_EN.PTB']
def read_ptb(raw):
    if len(raw)<80 or struct.unpack_from('>I',raw,20)[0]!=12610:raise ValueError('Not a PTB enemy path')
    d=raw[32:];n,length,verts,settings=struct.unpack_from('>IfII',d,0);pts=[];rot=[];flags=[]
    for i in range(n):
        row=struct.unpack_from('>7fi',d,verts+i*32);pts.append([round(x,3) for x in row[:3]]);rot.append(list(row[3:6]));flags.append(row[7])
    nameoff=struct.unpack_from('>I',d,44)[0];name=d[nameoff:nameoff+256].split(b'\0')[0].decode('ascii','replace')
    return dict(points=pts,rotations=rot,flags=flags,length=length,settings=settings,closed=bool(settings&2),name=name)
def write_ptb(points,settings=0x400,name='Stage Workshop enemy path',flags=None,base_rotations=None):
    """one PTB file: the PTP entry written like write_paths (orientations along the path, segment lengths, bounds).
    base_rotations (the preset's own, same point count): keep their pitch / roll (designer tilts) and take only the yaw
    from the new direction; flags of the preset are kept the same way."""
    pts=np.asarray(points,float)
    if pts.ndim!=2 or pts.shape[1]!=3 or len(pts)<2 or len(pts)>4096 or not np.isfinite(pts).all() or np.abs(pts).max()>1e6:
        raise ValueError('An enemy path needs 2-4096 finite points [x, y, z]')
    count=len(pts);dist=np.r_[np.linalg.norm(np.diff(pts,axis=0),axis=1),0.]
    rots=[point_frame(pts[j+1]-pts[j] if j<count-1 else pts[j]-pts[j-1]) for j in range(count)]
    if base_rotations is not None and len(base_rotations)==count:rots=[[b[0],r[1],b[2]] for r,b in zip(rots,base_rotations)]
    fl=list(flags) if flags is not None and len(flags)==count else [2]*(count-1)+[0]
    data=bytearray(struct.pack('>IfII3fi3fI',count,float(dist.sum()),48,int(settings),*pts.max(axis=0),0,*pts.min(axis=0),48+32*count))
    for j in range(count):data+=struct.pack('>7fi',*pts[j],*rots[j],dist[j],int(fl[j]))
    data+=name.encode('ascii','replace')[:255]+b'\0';data+=b'\0'*((-len(data))%4)
    pof=bytes([0x40|2,0x40|9]);pof+=b'\0'*((-len(pof))%4)   # pointers at +8 and +44 (deltas 2 and 9 words)
    body=bytes(data)+pof
    return struct.pack('>8I',32+len(body),len(data),len(pof),1,0,12610,0,0)+body
