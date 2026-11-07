import struct, numpy as np, sys
def skeleton(d, s):
    n=struct.unpack('<I',d[s+4:s+8])[0]
    w=d[s:]
    bones=[]
    for j in range(n):
        a=4*(3+20*j)
        f=struct.unpack('<19f',w[a:a+76]); par=struct.unpack('<i',w[a+76:a+80])[0]
        bones.append((np.array(f[:16]).reshape(4,4), np.array(f[16:19]), par))
    return bones
def mesh(d, c):
    k=struct.unpack('<I',d[c:c+4])[0]; offs=struct.unpack('<%dI'%k,d[c+4:c+4+4*k])
    parts=[]
    for o in offs:
        p=c+o; _,nv,nv2,nt=struct.unpack('<4I',d[p:p+16]); p+=16
        V=np.frombuffer(d[p:p+nv*20],'<f4').reshape(nv,5); p+=nv*20
        N=np.frombuffer(d[p:p+nv*12],'<f4').reshape(nv,3); p+=nv*12
        B=np.frombuffer(d[p:p+nv*2],'<u2'); p+=nv*2
        I=np.frombuffer(d[p:p+nt*6],'<u2').reshape(nt,3)
        parts.append((V,N,B,I))
    return parts
def world(bones, mode):
    W=[None]*len(bones)
    for j,(M,t,par) in enumerate(bones):
        L=M.copy()
        if mode==0: L[3,:3]=t
        elif mode==1: L[3,:3]+=t
        elif mode==2: L=np.eye(4); L[3,:3]=t
        W[j]=L if par<0 else L@W[par]
    return W
def write_obj(path, parts, W, tex=None):
    out=open(path,'w'); n=1
    if tex: out.write('mtllib %s.mtl\nusemtl t\n'%path.rsplit('/',1)[-1][:-4]); open(path[:-4]+'.mtl','w').write('newmtl t\nKd 1 1 1\nmap_Kd %s\n'%tex)
    for V,N,B,I in parts:
        for v,b in zip(V,B):
            p=np.r_[v[:3],1]@W[b] if W else np.r_[v[:3],1]
            out.write('v %f %f %f 0.5 0.5 0.5\nvt %f %f\n'%(p[0],p[1],-p[2],v[3],1-v[4]))
        for a,b,c in I: out.write('f %d/%d %d/%d %d/%d\n'%(a+n,a+n,b+n,b+n,c+n,c+n))
        n+=len(V)
