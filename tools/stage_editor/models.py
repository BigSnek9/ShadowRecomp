"""Read-only RenderWare DFF previews. Bind-pose display; no skeleton/animation export."""
import struct
import numpy as np
from formats import children,first,native_mesh,materials

# ---- #33 UV animation. A .UVA is a RenderWare 3.7 stream: 0x2B UV animation dictionary = struct {u32 count} + count x
# 0x1B animation = {u32 0x100, u32 interpolator 0x1C1 (linear UV keyframes), u32 frames, u32 flags, f32 duration, u32 0,
# char name[32], 8 x u32 node -> UV channel map, frames x {f32 time, f32 uv[6], i32 previous frame}}. A DFF material
# names the animations it plays in its extension 0x135 = struct {u32 channel mask, char name[32] per set bit}.
# Interpolator 0x1C1 = RenderWare's PARAMETRIC UV keyframe: uv[6] = {theta (rad), s0, s1, skew, x, y}, identity
# [0, 1, 1, 0, 0, 0] (corrected 2026-10-16; it is not a matrix and there is no u/v swap). On the whole disc theta and skew
# are always 0, so u' = s0 * u + x, v' = s1 * v + y (s1 = -1 flips v in 82 frames; 1.1 / 1.2 / 0.647 ... scale).
def read_uva(data):
    """{name: dict(duration, flags, channels, frames=[dict(time, uv=[a, b, c, d, x, y], prev)])}; sizes must fit exactly"""
    if len(data) < 12 or struct.unpack_from('<I', data)[0] != 0x2B: raise ValueError('not a UV animation dictionary (0x2B)')
    root = first(data, 0x2B); st = first(root, 1)
    if st is None or len(st) != 4: raise ValueError('UVA dictionary without its count')
    count = struct.unpack_from('<I', st)[0]; anims = [b for t, b, v in children(root) if t == 0x1B]
    if len(anims) != count: raise ValueError(f'UVA dictionary says {count} animations, holds {len(anims)}')
    out = {}
    for a in anims:
        ver, typ, n, flags, duration, _ = struct.unpack_from('<4If I', a)
        if typ != 0x1C1: raise ValueError(f'UVA interpolator {typ:#x} (expected 0x1C1): {a[:64].hex()}')
        if len(a) != 24 + 32 + 32 + 32 * n: raise ValueError(f'UVA animation of {n} frames is {len(a)} bytes, expected {88 + 32 * n}: {a[:96].hex()}')
        name = a[24:56].split(b'\0')[0].decode('ascii', 'replace'); channels = list(struct.unpack_from('<8I', a, 56))
        frames = []
        for i in range(n):
            t, *uv, prev = struct.unpack_from('<7fi', a, 88 + 32 * i)
            frames.append(dict(time=t, uv=uv, theta=uv[0], scale=[uv[1], uv[2]], skew=uv[3], offset=[uv[4], uv[5]],
                               prev=prev if 0 <= prev < i else -1))   # frame 0's prev word is junk in the files
        f0, f1 = frames[0], frames[-1]; dt = f1['time'] - f0['time']
        velocity = [(f1['uv'][4] - f0['uv'][4]) / dt, (f1['uv'][5] - f0['uv'][5]) / dt] if n >= 2 and dt > 0 else [0.0, 0.0]
        out[name] = dict(name=name, duration=duration, flags=flags, channels=channels, frames=frames, velocity=velocity)
    return out


def bind_uv_anims(models, files):
    """#33: give each material that names UV animations (uvAnimNames) its keyframes: material.uvAnim = [anim, ...]
    with anim = read_uva entry + source (the .UVA name). Same-stem file first (X.DFF -> X.UVA), else any .UVA of the
    archive with that animation name. models = [dict(name=..., materials=[...])], files = the archive {name: bytes}."""
    dicts = {}
    for n, b in files.items():
        if n.upper().endswith('.UVA'):
            try: dicts[n.upper()] = read_uva(b)
            except (ValueError, struct.error): continue
    for m in models:
        stem = m['name'].upper().rsplit('.', 1)[0] + '.UVA'
        for mat in m.get('materials', []):
            found = []
            for name in mat.get('uvAnimNames', []):
                src = stem if name in dicts.get(stem, {}) else next((k for k, d in dicts.items() if name in d), None)
                if src: found.append(dict(dicts[src][name], source=src))
            if found: mat['uvAnim'] = found
    return models


def material_uv_anims(material):
    """names of the UV animations a material (0x07 chunk body) plays, from its extension 0x135"""
    ext = next((b for t, b, v in children(material) if t == 3), b'')
    uv = first(ext, 0x135)
    if not uv: return []
    st = first(uv, 1); mask = struct.unpack_from('<I', st)[0]; names = []
    for i in range(bin(mask).count('1')):
        names.append(st[4 + 32 * i:36 + 32 * i].split(b'\0')[0].decode('ascii', 'replace'))
    if len(st) != 4 + 32 * len(names): raise ValueError(f'UV animation material extension of {len(st)} bytes for mask {mask:#x}')
    return names


def material_fx_textures(material):
    """textures a material's MatFX extension (0x120) names: dual-texture / environment-map passes carry their own
    texture chunk (0x06 with the name as its first string), e.g. Glyphic column caps: base s0201_sn026 + dual
    s0201_sp002. A copied model without them renders those passes white."""
    ext = next((b for t, b, v in children(material) if t == 3), b'')
    fx = first(ext, 0x120) if ext else None
    out = []
    if not fx: return out
    i = 0
    while i + 12 <= len(fx):
        t, size, ver = struct.unpack_from('<III', fx, i)
        if t == 6 and ver >> 16 == 0x1C02 and i + 12 + size <= len(fx):
            try:
                name = next((s.split(b'\0')[0].decode('ascii', 'replace') for k, s, _ in children(fx[i + 12:i + 12 + size]) if k == 2), '')
                if name: out.append(name)
                i += 12 + size; continue
            except ValueError: pass
        i += 4
    return out


def read_dff(data):
    clump=first(data,0x10)
    if clump is None:raise ValueError('No DFF clump')
    frames=first(first(clump,0x0e),1);count=struct.unpack_from('<I',frames)[0];matrices=[]
    for i in range(count):
        at=4+i*56;values=struct.unpack_from('<12fiI',frames,at);m=np.eye(4);m[:3,:3]=np.array(values[:9]).reshape(3,3).T;m[:3,3]=values[9:12]
        parent=values[12]
        if 0<=parent<i:m=matrices[parent]@m
        matrices.append(m)
    geometries=[];mats=[]
    for tag,body,_ in children(first(clump,0x1a) or b''):
        if tag!=0xf:continue
        start=len(mats);ms=materials(body)
        for m,(t,mb,v) in zip(ms,[c for c in children(first(body,8) or b'') if c[0]==7]):
            names=material_uv_anims(mb)
            if names:m['uvAnimNames']=names
            fx=material_fx_textures(mb)
            if fx:m['fxTextures']=fx
        mats.extend(ms);st=first(body,1);flags,nt,nv,nm=struct.unpack_from('<4I',st)
        if first(first(body,3) or b'',0x510):mesh=native_mesh(body)
        else:
            q=16;colour=[[255]*4]*nv
            if flags&8:colour=np.frombuffer(st,'u1',nv*4,q).reshape(-1,4).tolist();q+=nv*4
            uv_count=(flags>>16)&255
            if not uv_count:uv_count=2 if flags&128 else 1 if flags&4 else 0
            uv=np.frombuffer(st,'<f4',nv*2,q).reshape(-1,2).tolist() if uv_count else [[0,0]]*nv;q+=uv_count*nv*8
            tris=np.frombuffer(st,'<u2',nt*4,q).reshape(-1,4);q+=nt*8
            _,_,_,_,has_v,has_n=struct.unpack_from('<4f2I',st,q);q+=24
            if not has_v:raise ValueError('DFF has no static/bind-pose positions')
            mesh=dict(vertices=np.frombuffer(st,'<f4',nv*3,q).reshape(-1,3).tolist(),triangles=tris[:,[1,0,3]].tolist(),materials=tris[:,2].tolist(),colours=colour,uvs=uv)
        mesh['materials']=[start+i for i in mesh['materials']];geometries.append(mesh)
    meshes=[]
    for tag,body,_ in children(clump):
        if tag!=0x14:continue
        fi,gi=struct.unpack_from('<II',first(body,1))
        if gi>=len(geometries) or fi>=len(matrices):raise ValueError('Invalid atomic model reference')
        mesh=dict(geometries[gi]);v=np.asarray(mesh['vertices']);v=np.c_[v,np.ones(len(v))]@matrices[fi].T;mesh['vertices']=v[:,:3].tolist();meshes.append(mesh)
    if not meshes:meshes=geometries
    return dict(meshes=meshes,materials=mats,pose='bind_pose')
