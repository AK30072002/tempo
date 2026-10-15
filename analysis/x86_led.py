import numpy as np, sys
f=sys.argv[1]
d=np.loadtxt(f,delimiter=",",skiprows=1)
def g2b(g):
    n=g.copy(); m=g>>1
    while m.any(): n^=m; m>>=1
    return n
res={}
for c in (0,1):
    x=d[d[:,0]==c]; t=x[:,1]*1e3; seq=x[:,2].astype(int); v=x[:,3:11]
    lo=np.percentile(v,5,axis=0); hi=np.percentile(v,95,axis=0); thr=(lo+hi)/2
    bits=(v>thr).astype(np.int64)
    best=None
    for order in ("A","B"):
        g=sum(bits[:,i]<<(i if order=="A" else 7-i) for i in range(8))
        n=g2b(g); dd=np.diff(n)%256; sc=np.mean((dd>=64)&(dd<=69))
        if best is None or sc>best[0]: best=(sc,order,n)
    sc,order,n=best
    u=np.empty(len(n)); u[0]=n[0]
    for i in range(1,len(n)):
        pred=u[i-1]+(t[i]-t[i-1]); u[i]=n[i]+256*np.round((pred-n[i])/256)
    k=seq-seq[0]
    dadv=np.diff(u)
    zero=np.where(np.diff(n)%256==0)[0]+1
    res[c]=dict(t=t,u=u,k=k,order=order,zero=zero,adv=dadv,n=n)
    print(f"cam{c}: frames {len(t)}, order {order}, advance 64..69 {100*sc:.1f}%, zero-advance frame indices {zero.tolist()}")
    odd=np.where((dadv<60)|(dadv>73))[0]+1
    print(f"   odd advances (not 60..73) at frame idx: {odd[:30].tolist()}  values {dadv[odd-1][:30].round(0).tolist()}")
for c in (0,1):
    r=res[c]; t,u,k=r['t'],r['u'],r['k']
    s=max([z for z in r['zero'] if z<40]+[1])+1
    r['s']=s
    tt,uu,kk=t[s:],u[s:],k[s:]
    po=np.polyfit(kk,uu,1); ro=uu-np.polyval(po,kk)
    pk=np.polyfit(kk,tt,1); rk=tt-np.polyval(pk,kk)
    step=1/np.polyfit(tt,uu,1)[0]
    o=uu-tt; o-=np.median(o)
    r.update(o=o,tt=tt,uu=uu,kk=kk)
    print(f"\ncam{c} (from frame {s}): Arduino step vs host clock {step:.5f} ms")
    print(f"   optical period {po[0]:.4f} ms/frame, residual RMS {ro.std():.3f} ms, p99 |res| {np.percentile(abs(ro),99):.3f}, max {abs(ro).max():.2f}")
    print(f"   kernel  period {pk[0]:.4f} ms/frame, residual RMS {rk.std():.3f} ms, p99 |res| {np.percentile(abs(rk),99):.3f}, max {abs(rk).max():.2f}")
    print(f"   kernel gap histogram: "+str(dict(zip(*np.unique(np.round(np.diff(tt)),return_counts=True)))))
    print(f"   optical-kernel per frame (median removed): RMS {o.std():.3f} ms, p50|.| {np.percentile(abs(o),50):.3f}, p99|.| {np.percentile(abs(o),99):.3f}, max {abs(o).max():.2f}")
r0,r1=res[0],res[1]
off0=np.median(r0['uu']-r0['tt']); off1=np.median(r1['uu']-r1['tt'])
print(f"\noptical-minus-kernel offset cam1 vs cam0: {off1-off0:+.3f} ms (mod 256 ms ambiguity resolved by unwrap with kernel time)")
t0,u0=r0['t'],r0['u']; t1,u1=r1['t'],r1['u']
kshift=np.round(((np.median(u1-t1))-(np.median(u0-t0)))/256); u1=u1-256*kshift
s=max(r0['s'],r1['s'])
n=min(len(t0),len(t1))
ik=np.abs(t0[s:n]-t1[s:n]); io=np.abs(u0[s:n]-u1[s:n])
j=np.searchsorted(t1,t0); j=np.clip(j,1,len(t1)-1); j=np.where(abs(t1[j-1]-t0)<abs(t1[j]-t0),j-1,j)
sel=np.arange(len(t0))>=s
nk=np.abs(t0-t1[j])[sel]; no=np.abs(u0-u1[j])[sel]
T=66.6653
for name,kk_,oo in (("INDEX",ik,io),("NEAREST",nk,no)):
    print(f"{name:8s} (incl. run edges) n={len(kk_)}  kernel mean/p95/max {kk_.mean():.2f}/{np.percentile(kk_,95):.2f}/{kk_.max():.2f} ms   OPTICAL mean/p95/max {oo.mean():.2f}/{np.percentile(oo,95):.2f}/{oo.max():.2f} ms   |opt-kern| mean {np.mean(abs(oo-kk_)):.2f} p99 {np.percentile(abs(oo-kk_),99):.2f} max {np.max(abs(oo-kk_)):.2f}   max/T kern {kk_.max()/T:.3f} opt {oo.max()/T:.3f}")
print("\n--- refined ---")
for c in (0,1):
    r=res[c]; tt,uu=r['tt'],r['uu']; o=uu-tt
    p=np.polyfit(tt,o,1); od=o-np.polyval(p,tt)
    r['od']=od
    print(f"cam{c}: optical-kernel after removing Arduino/host clock drift ({p[0]*1e6:.1f} ppm): RMS {od.std():.3f} ms, p50|.| {np.percentile(abs(od),50):.3f}, p95 {np.percentile(abs(od),95):.3f}, p99 {np.percentile(abs(od),99):.3f}, max {abs(od).max():.2f} ms")
    kk=r['kk']; ro=uu-np.polyval(np.polyfit(kk,uu,1),kk); rk=tt-np.polyval(np.polyfit(kk,tt,1),kk)
    print(f"       on the frame grid: kernel jitter RMS {rk.std():.3f} ms (range {rk.min():.2f}..{rk.max():.2f}), optical read noise RMS {ro.std():.3f} ms; corr {np.corrcoef(ro,rk)[0,1]:.2f}")
s=max(r0['s'],r1['s']); n=min(len(t0),len(t1))
diff=(u1[s:n]-t1[s:n])-(u0[s:n]-t0[s:n])
mins=np.array_split(diff,10)
print("cam1-cam0 optical-kernel offset per minute (median, ms):", np.round([np.median(m) for m in mins],2).tolist())
lo=max(t0[s],t1[s]); hi=min(t0[-1],t1[-1])
m=(t0>=lo)&(t0<=hi)&sel
nk=np.abs(t0-t1[j])[m]; no=np.abs(u0-u1[j])[m]
print(f"NEAREST interior n={m.sum()}: kernel mean/p95/max {nk.mean():.2f}/{np.percentile(nk,95):.2f}/{nk.max():.2f}  OPTICAL mean/p95/max {no.mean():.2f}/{np.percentile(no,95):.2f}/{no.max():.2f}  |opt-kern| mean {np.mean(abs(no-nk)):.2f} p95 {np.percentile(abs(no-nk),95):.2f} max {np.max(abs(no-nk)):.2f}   max/T: kernel {nk.max()/T:.3f}, optical {no.max()/T:.3f}")
