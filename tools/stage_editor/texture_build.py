"""New per-project materials: native128px RGBA8 plus full-resolution replacement PNG.
Small native images keep guest memory bounded; the PC texture pack supplies the source resolution.
"""
import struct
import numpy as np
from PIL import Image
from formats import chunk,children,first,serialize

def xxh64(data):
    # XXH64 (seed0), matching the texture replacer's source keys; no extra Python dependency.
    mask=(1<<64)-1;p1=11400714785074694791;p2=14029467366897019727;p3=1609587929392839161;p4=9650029242287828579;p5=2870177450012600261
    def rot(x,n):return ((x<<n)|(x>>(64-n)))&mask
    def rnd(a,v):return rot((a+v*p2)&mask,31)*p1&mask
    n=len(data);at=0
    if n>=32:
        v=[(p1+p2)&mask,p2,0,(-p1)&mask]
        while at<=n-32:
            words=struct.unpack_from('<4Q',data,at);at+=32;v=[rnd(a,b) for a,b in zip(v,words)]
        h=sum(rot(a,b) for a,b in zip(v,[1,7,12,18]))&mask
        for a in v:h=((h^rnd(0,a))*p1+p4)&mask
    else:h=p5
    h=(h+n)&mask
    while at<=n-8:
        h^=rnd(0,struct.unpack_from('<Q',data,at)[0]);h=(rot(h,27)*p1+p4)&mask;at+=8
    if at<=n-4:h^=struct.unpack_from('<I',data,at)[0]*p1&mask;h=(rot(h,23)*p2+p3)&mask;at+=4
    while at<n:h^=data[at]*p5&mask;h=rot(h,11)*p1&mask;at+=1
    h^=h>>33;h=h*p2&mask;h^=h>>29;h=h*p3&mask;h^=h>>32
    return f'{h:016x}'

def rw_string(s):return chunk(2,(s.encode('ascii')+b'\0').ljust((len(s)+4)//4*4,b'\0'))
def material_bytes(name):
    tex=chunk(6,chunk(1,struct.pack('<I',0x11102))+rw_string(name)+rw_string('')+chunk(3,b''))
    return chunk(7,chunk(1,struct.pack('<I4BII3f',0,255,255,255,255,0,1,1.,1.,1.))+tex+chunk(3,b''))
def native_texture(path,name,template):
    im=Image.open(path).convert('RGBA').resize((128,128),Image.Resampling.LANCZOS)
    px=np.array(im).reshape(32,4,32,4,4).transpose(0,2,1,3,4).reshape(-1,16,4)
    tiled=np.concatenate([px[:,:,[3,0]],px[:,:,[1,2]]],axis=1).tobytes()
    hdr=bytearray(template[:0x68]);hdr[0x18:0x38]=name.encode('ascii').ljust(32,b'\0');hdr[0x38:0x58]=b'\0'*32
    struct.pack_into('>IHH',hdr,0x58,0x504,128,128);hdr[0x60:0x64]=bytes([32,1,6,0]);hdr[0x64:0x68]=b'\0'*4
    raw=bytes(hdr)+struct.pack('>I',len(tiled))+tiled
    key=f'tex1_128x128_{xxh64(tiled)}_6'
    return chunk(0x15,chunk(1,raw)+chunk(3,b'')),key
def append_textures(txd,entries):
    cs=children(txd);idx=next(i for i,c in enumerate(cs) if c[0]==0x16);tc=children(cs[idx][1]);si=next(i for i,c in enumerate(tc) if c[0]==1)
    template=first(next(b for t,b,v in tc if t==0x15),1);count,device=struct.unpack('<HH',tc[si][1]);new=[];keys={}
    for name,path in entries:
        b,key=native_texture(path,name,template);new+=children(b);keys[name]=key
    tc[si]=(1,struct.pack('<HH',count+len(new),device),cs[idx][2]);ext=next((i for i,c in enumerate(tc) if c[0]==3),len(tc));tc[ext:ext]=new
    cs[idx]=(0x16,serialize(tc),cs[idx][2]);return serialize(cs),keys
