import sys, subprocess, json, numpy as np
from common import d, OUT, HERE
A=HERE
R=d("07_led_orin")
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
    best=None
    for order in (range(8),range(7,-1,-1)):
        g=sum(bits[:,s]<<i for i,s in enumerate(order))
        n=gray2bin(g)
        ok=np.diff(seq)==1; T=np.median(np.diff(t)[ok]); E=T/0.25
        dd=(np.diff(n)%256)[ok]; err=np.minimum(abs(dd-E%256),256-abs(dd-E%256))
        sc=np.mean(err<=2.5)
        if best is None or sc>best[0]: best=(sc,n,T)
    return t,best[1],best[0],best[2],(hi-lo).min()
def unwrap(t,v,rate):
    n=np.empty_like(v,dtype=float); n[0]=v[0]
    for i in range(1,len(v)):
        pred=n[i-1]+(t[i]-t[i-1])*rate
        n[i]=v[i]+256*np.round((pred-v[i])/256)
    return n
def analyse(name,cams_use,fps=100):
    data=load(f"{R}/{name}.csv")
    cams=[c for c in sorted(data) if c in cams_use]
    dec={}
    for c in cams:
        t,v,sc,T,con=decode(data[c]); dec[c]=(t,v,sc,T,con)
    ref=cams[0]
    t0,v0=dec[ref][0],dec[ref][1]
    n0=unwrap(t0,v0,4.0); rate=np.polyfit(t0,n0,1)[0]
    tau={}; off={}; out_frac={}
    for c in cams:
        t,v=dec[c][0],dec[c][1]; n=unwrap(t,v,rate); o=n/rate-t
        k=np.round((np.median(o)-np.median(n0/rate-t0))/(256/rate)); n-=256*k; o=n/rate-t
        p=np.polyfit(t,o,1); r=o-np.polyval(p,t); bad=abs(r)>0.5
        out_frac[c]=bad.mean(); tau[c]=(t,n/rate,bad); off[c]=np.median(o)
    o_ref=off[ref]
    rel={c:off[c]-o_ref for c in cams}
    csvsel=subprocess.run([A+"/led_select",f"{R}/{name}.csv",str(fps),",".join(map(str,cams))],capture_output=True,text=True).stdout.splitlines()
    mp=csvsel[0]; lines=csvsel[1:]
    lut={}
    for j,c in enumerate(cams):
        t,ta,bad=tau[c]; lut[j]=dict(zip(np.round(t*1e3).astype(np.int64),zip(ta,bad)))
    res={}
    for p,pn in ((0,"INDEX"),(1,"NEAREST"),(2,"PREDICTED")):
        ks=[];os_=[];us=[];sel=[]
        for L in lines:
            f=L.split(",")
            if int(f[0])!=p: continue
            arr=[int(x)//1000 for x in f[4:]]
            vals=[lut[j].get(a) for j,a in enumerate(arr)]
            if any(x is None or x[1] for x in vals): continue
            ta=[x[0] for x in vals]
            ks.append(int(f[3])/1e6); us.append(int(f[2])/1e6); os_.append(max(ta)-min(ta)); sel.append(ta)
        ks=np.array(ks);os_=np.array(os_);us=np.array(us)
        res[pn]=dict(n=len(ks),k_mean=ks.mean(),k_p95=np.percentile(ks,95),k_max=ks.max(),o_mean=os_.mean(),o_p95=np.percentile(os_,95),o_max=os_.max(),
                     err_mean=np.mean(abs(os_-ks)),err_u=np.mean(abs(os_-us)),corr_u=np.corrcoef(us,os_)[0,1] if us.std()>0 else float('nan'),sel=np.array(sel))
    return dict(cams=cams,decode={c:dec[c][2] for c in cams},contrast={c:dec[c][4] for c in cams},step_ms=1/rate,rel=rel,outliers=out_frac,res=res,T=dec[ref][3])
roi={}
for line in open(f"{R}/roi.txt"):
    if line.startswith("#") or not line.strip(): continue
    f=list(map(int,line.split())); roi[f[0]]=np.mean(f[2::2])
runs=[("L0_sync60",[0,1,2,3]),("L1_free_600s",[0,1,2,3]),("L2_free_600s",[0,1,2,3]),("L3_free_600s",[0,1,2]),("L4_free_1800s",[0,1,2])]
allres={}
for name,cams in runs:
    r=analyse(name,cams); allres[name]=r
    print("\n=== %s  cams %s  T=%.4f ms  Arduino step %.5f ms"%(name,cams,r["T"],r["step_ms"]))
    print("   decode-OK:", " ".join("cam%d %.1f%%"%(c,100*r["decode"][c]) for c in cams), "| outliers:", " ".join("%.2f%%"%(100*r["outliers"][c]) for c in cams))
    print("   optical-minus-kernel offset vs cam%d:"%cams[0], " ".join("cam%d %+.3f"%(c,r["rel"][c]) for c in cams))
    print("   policy     n      KERNEL mean/p95/max        OPTICAL(true) mean/p95/max    |true-kernel| mean  |true-u| mean  corr(u,true)")
    for pn,x in r["res"].items():
        print("   %-9s %6d   %6.2f %6.2f %6.2f        %6.2f %6.2f %6.2f        %5.3f          %5.3f      %.3f"%(pn,x["n"],x["k_mean"],x["k_p95"],x["k_max"],x["o_mean"],x["o_p95"],x["o_max"],x["err_mean"],x["err_u"],x["corr_u"]))
import pickle; pickle.dump(allres,open(OUT+"/led_res.pkl","wb"))
