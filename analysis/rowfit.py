import numpy as np, json, sys
from common import d, OUT
RT=d("08_led_board_geometry"); R13=d("07_led_orin")
def gray2bin(g):
    n=g.copy(); m=g>>1
    while m.any(): n^=m; m>>=1
    return n
def load(path):
    d=np.loadtxt(path,delimiter=",",skiprows=1)
    return {int(c):d[d[:,0]==c] for c in np.unique(d[:,0])}
def decode(rows):
    v=rows[:,3:11]; t=rows[:,1]*1e3; seq=rows[:,2]
    lo=np.percentile(v,10,axis=0); hi=np.percentile(v,90,axis=0); thr=(lo+hi)/2
    bits=(v>thr).astype(np.int64)
    g=sum(bits[:,i]<<i for i in range(8))
    n=gray2bin(g)
    ok=np.diff(seq)==1; T=np.median(np.diff(t)[ok]); E=T/0.25
    dd=(np.diff(n)%256)[ok]; err=np.minimum(abs(dd-E%256),256-abs(dd-E%256))
    return t,n,np.mean(err<=2.5)
def unwrap(t,v,rate):
    n=np.empty_like(v,dtype=float); n[0]=v[0]
    for i in range(1,len(v)):
        pred=n[i-1]+(t[i]-t[i-1])*rate
        n[i]=v[i]+256*np.round((pred-v[i])/256)
    return n
def roi(path):
    out={}
    for L in open(path):
        if L.startswith("#") or not L.strip(): continue
        f=list(map(int,L.split())); out[f[0]]=np.array(f[2::2],float)
    return out
W=np.array([2.0**-(b+1) for b in range(8)]); W[7]=W[6]; W/=W.sum()
def analyse(csv,roipath,cams,tmax=None):
    d=load(csv); rr=roi(roipath)
    t0all=min(d[c][0,1] for c in cams)
    dec={}
    for c in cams:
        x=d[c]
        if tmax: x=x[(x[:,1]-t0all)<tmax]
        dec[c]=decode(x)
    ref=cams[0]; t0,v0,_=dec[ref]
    n0=unwrap(t0,v0,4.0); rate=np.polyfit(t0,n0,1)[0]
    off={};spread={};ok={}
    oref=None
    for c in cams:
        t,v,sc=dec[c]; n=unwrap(t,v,rate); o=n/rate-t
        if oref is None: oref=np.median(o)
        k=np.round((np.median(o)-oref)/(256/rate)); o=o-256*k/rate
        p=np.polyfit(t,o,1); r=o-np.polyval(p,t); good=abs(r)<0.5
        off[c]=np.median(o[good]); ok[c]=sc
        spread[c]=np.std(o[good])/np.sqrt(good.sum()/50)
    res={}
    for c in cams:
        res[c]=dict(off=off[c]-off[ref],row_mean=rr[c].mean(),row_eff=(W*rr[c]).sum(),dec=ok[c],n=len(dec[c][0]))
    return res,1/rate
P=lambda sub: f"{RT}/{sub}"
cfgs=[
 ("13Oct_L1","A",R13+"/L1_free_600s.csv",R13+"/roi.txt",[0,1,2,3],None),
 ("13Oct_L2","A",R13+"/L2_free_600s.csv",R13+"/roi.txt",[0,1,2,3],None),
 ("13Oct_L3","A",R13+"/L3_free_600s.csv",R13+"/roi.txt",[0,1,2],None),
 ("13Oct_L4","A",R13+"/L4_free_1800s.csv",R13+"/roi.txt",[0,1,2],None),
 ("B1_first28s","B",P("B1")+"/L1_free_300s.csv",P("B1")+"/roi.txt",[0,1,2,3],28),
 ("P1","C1",P("P1")+"/L1_free_180s.csv",P("P1")+"/roi.txt",[0,1,2,3],None),
 ("P2_first28s","C2",P("P2")+"/L1_free_180s.csv",P("P2")+"/roi.txt",[0,1,2,3],28),
 ("P2b","C3",P("P2b")+"/L1_free_180s.csv",P("P2b")+"/roi.txt",[0,1,2,3],None),
 ("P3","C4",P("P3")+"/L1_free_180s.csv",P("P3")+"/roi.txt",[0,1,2,3],None),
 ("P4","C5",P("P4")+"/L1_free_180s.csv",P("P4")+"/roi.txt",[0,1,2,3],None),
]
rows=[]
for name,g,csv,rp,cams,tm in cfgs:
    r,step=analyse(csv,rp,cams,tm)
    print(f"{name:13s} step {step:.5f}  "+"  ".join(f"cam{c}: off {r[c]['off']:+.3f} rowM {r[c]['row_mean']:.1f} rowE {r[c]['row_eff']:.1f} dec {100*r[c]['dec']:.1f}%" for c in cams))
    for c in cams[1:]:
        rows.append(dict(cfg=name,geo=g,cam=c,off=r[c]['off'],dM=r[c]['row_mean']-r[0]['row_mean'],dE=r[c]['row_eff']-r[0]['row_eff']))
json.dump(rows,open(OUT+"/rowfit_points.json","w"),indent=1)
