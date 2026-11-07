import struct, numpy as np
def twid(w,h):
    m=min(w,h); X,Y=np.meshgrid(np.arange(w),np.arange(h))
    bx,by=X//m,Y//m; x,y=X%m,Y%m; r=np.zeros_like(X); b=0; s=1
    while s<m: r|=((y>>b)&1)<<(2*b)|((x>>b)&1)<<(2*b+1); b+=1; s<<=1
    return r+(bx+by)*m*m
def conv(p,fmt):
    p=p.astype(np.int64)
    if fmt==0: a=np.where(p&0x8000,255,0); r=(p>>10&31)*255//31; g=(p>>5&31)*255//31; b=(p&31)*255//31
    elif fmt==1: a=np.full_like(p,255); r=(p>>11&31)*255//31; g=(p>>5&63)*255//63; b=(p&31)*255//31
    elif fmt==2: a=(p>>12&15)*17; r=(p>>8&15)*17; g=(p>>4&15)*17; b=(p&15)*17
    else: raise ValueError
    return np.stack([r,g,b,a],-1).astype(np.uint8)
def yuv(img):   # img: h x w uint16 in linear order
    p=img.astype(int); Y=p>>8; c=p&0xFF
    U=np.repeat(c[:,0::2],2,1); V=np.repeat(c[:,1::2],2,1)
    R=Y+1.402*(V-128); G=Y-0.344*(U-128)-0.714*(V-128); B=Y+1.772*(U-128)
    return np.dstack([np.clip(np.stack([R,G,B],-1),0,255).astype(np.uint8), np.full(p.shape,255,np.uint8)])
def pvrt(d,p):
    """PVRT block at p -> (RGBA array or None, info)"""
    if d[p:p+4]!=b'PVRT': return None,'no PVRT'
    pf,df=d[p+8],d[p+9]; w,h=struct.unpack('<HH',d[p+12:p+16]); q=p+16
    def pix(a):
        return yuv(a) if pf==3 else conv(a,pf) if pf<3 else None
    if df in (9,0xB):                       # rectangle
        a=np.frombuffer(d[q:q+w*h*2],'<u2')
        if len(a)<w*h or pf>3: return None,(pf,df,w,h)
        return pix(a.reshape(h,w)),(pf,df,w,h)
    if df in (1,2,0xD):                     # twiddled (2: mipmapped, largest level last)
        if df==2:
            sk=sum((w>>k)*(w>>k) for k in range(1,16) if w>>k)
            q+=2*(sk+0) + 2   # 1x1 level stored with 2-byte pad... approx
        a=np.frombuffer(d[q:q+w*h*2],'<u2')
        if len(a)<w*h or pf>3: return None,(pf,df,w,h)
        lin=a[twid(w,h)]
        if pf==3:   # YUV twiddled: decode pairs in linear order after untwiddle
            return yuv(lin),(pf,df,w,h)
        return pix(lin),(pf,df,w,h)
    if df in (3,4,0x10,0x11):               # VQ
        if pf>2: return None,(pf,df,w,h)
        cb=np.frombuffer(d[q:q+2048],'<u2').reshape(256,4); q+=2048
        if df in (4,0x11):
            n=w.bit_length()-1; q+= (1+((1<<(2*(n-1)))-1)//3) if n>=1 else 0
        idx=np.frombuffer(d[q:q+w*h//4],'u1')
        if len(idx)<w*h//4: return None,(pf,df,w,h)
        ti=idx[twid(w//2,h//2)]
        out=np.zeros((h,w),np.uint16)
        out[0::2,0::2]=cb[ti,0]; out[1::2,0::2]=cb[ti,1]; out[0::2,1::2]=cb[ti,2]; out[1::2,1::2]=cb[ti,3]
        return conv(out,pf),(pf,df,w,h)
    return None,(pf,df,w,h)
