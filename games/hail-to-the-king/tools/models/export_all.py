import sys, struct, collections, os, numpy as np
sys.path.insert(0,os.path.dirname(__file__))
from iso import *; from dcmodel import *; from dctex import *
from PIL import Image
if len(sys.argv)!=3: sys.exit('usage: export_all.py <disc.cue> <out folder>  - writes every character as OBJ + PNG')
open_disc(sys.argv[1]); out=sys.argv[2]; os.makedirs(out,exist_ok=True)
fs=files()
def load(l,s): return b''.join(sec(l+k) or b'\0'*2048 for k in range((s+2047)//2048))[:s]
def is_mesh(d,o,sz):
    k=struct.unpack('<I',d[o:o+4])[0]
    if not 0<k<64 or struct.unpack('<I',d[o+4:o+8])[0]!=4+4*k: return False
    offs=struct.unpack('<%dI'%k,d[o+4:o+4+4*k])
    for j,of in enumerate(offs):
        p=o+of; z,nv,nv2,nt=struct.unpack('<4I',d[p:p+16])
        if nv!=nv2 or not 0<nv<5000 or not 0<nt<10000: return False
        end=of+16+nv*34+nt*6; nxt=offs[j+1] if j+1<k else sz
        if not (end<=nxt<=end+4): return False
    return True
seen=set(); rows=[]
for n,l,s in fs:
    if not n.endswith('.DC') or n.endswith('V.DC'): continue
    d=load(l,s); cnt=struct.unpack('<I',d[20:24])[0]
    E=[struct.unpack('<4I',d[0x18+16*i:0x28+16*i]) for i in range(cnt)]
    byt={t:(f,o,sz) for t,f,o,sz in E}
    for t,f,o,sz in E:
        if f or not is_mesh(d,o,sz) or t in seen: continue
        seen.add(t); obj=(t>>12)&0x7FF
        P=mesh(d,o)
        sk=byt.get(t+0x800000); W=None
        if sk and sk[0]==0:
            try:
                S=skeleton(d,sk[1])
                if all(-1<=p<len(S) for _,_,p in S) and max(int(b.max()) for _,_,b,_ in P)<len(S): W=world(S,0)
            except Exception: W=None
        texs=[(tt,oo) for tt,(ff,oo,ss) in byt.items() if ff==1 and (tt>>12)&0x7FF==obj]
        name='%s_%08x'%(n[:-3],t); texfile=None
        for tt,oo in texs[:1]:
            im,info=pvrt(d,oo+16)
            if im is not None: Image.fromarray(im).save(f'{out}/{name}.png'); texfile=name+'.png'
        write_obj(f'{out}/{name}.obj',[P[0]] if W else P,W,texfile)
        rows.append((name,len(P),sum(len(x[3]) for x in P),bool(W),len(texs),texfile is not None))
for r in rows: print(*r)
