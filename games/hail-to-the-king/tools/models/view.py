"""Quick software render of an OBJ for previews: view.py <model.obj> <out.png> [yaw degrees]"""
import sys, numpy as np
from PIL import Image
obj=sys.argv[1]; out=sys.argv[2]; yaw=float(sys.argv[3]) if len(sys.argv)>3 else 0
d=obj.rsplit('/',1)[0] if '/' in obj else '.'
V=[];T=[];F=[];mat=None;texs={};mt={}
for l in open(d+'/'+obj.rsplit('/',1)[-1].replace('.obj','.mtl')):
    p=l.split()
    if p and p[0]=='newmtl': cur=p[1]
    if p and p[0]=='map_Kd': mt[cur]=np.asarray(Image.open(d+'/'+p[1]).convert('RGBA'))
for l in open(obj):
    p=l.split()
    if not p: continue
    if p[0]=='v': V.append([float(x) for x in p[1:7]])
    elif p[0]=='vt': T.append([float(x) for x in p[1:3]])
    elif p[0]=='usemtl': mat=p[1]
    elif p[0]=='f': F.append(([int(x.split('/')[0])-1 for x in p[1:4]],mat))
V=np.array(V);T=np.array(T)
c=(V[:,:3].min(0)+V[:,:3].max(0))/2; P=V[:,:3]-c
a=np.radians(yaw); R=np.array([[np.cos(a),0,np.sin(a)],[0,1,0],[-np.sin(a),0,np.cos(a)]]); P=P@R.T
W,H=400,500; s=H*0.9/(P[:,1].max()-P[:,1].min())
sx=W/2+P[:,0]*s; sy=H/2-P[:,1]*s; z=P[:,2]
img=np.full((H,W,3),40,np.uint8); zb=np.full((H,W),-1e9)
for (idx,m) in F:
    i,j,k=idx; x0,x1=int(max(0,min(sx[idx]))),int(min(W-1,max(sx[idx]))); y0,y1=int(max(0,min(sy[idx]))),int(min(H-1,max(sy[idx])))
    if x1<x0 or y1<y0: continue
    xs,ys=np.meshgrid(np.arange(x0,x1+1)+.5,np.arange(y0,y1+1)+.5)
    d0=(sx[j]-sx[i])*(sy[k]-sy[i])-(sx[k]-sx[i])*(sy[j]-sy[i])
    if abs(d0)<1e-9: continue
    b1=((xs-sx[i])*(sy[k]-sy[i])-(sx[k]-sx[i])*(ys-sy[i]))/d0
    b2=((sx[j]-sx[i])*(ys-sy[i])-(xs-sx[i])*(sy[j]-sy[i]))/d0
    b0=1-b1-b2; ins=(b0>=0)&(b1>=0)&(b2>=0)
    zz=b0*z[i]+b1*z[j]+b2*z[k]
    yy,xx=np.nonzero(ins)
    for y,x in zip(yy,xx):
        Y,X=y+y0,x+x0
        if zz[y,x]<=zb[Y,X]: continue
        zb[Y,X]=zz[y,x]
        if m in mt:
            t=mt[m]; u=b0[y,x]*T[i]+b1[y,x]*T[j]+b2[y,x]*T[k]
            tx=int(u[0]%1*t.shape[1]); ty=int((1-u[1])%1*t.shape[0]); col=t[ty,tx,:3]*V[i,3:6]*2
            img[Y,X]=np.clip(col,0,255)
        else: img[Y,X]=V[i,3:6]*255
Image.fromarray(img).save(out)
