import sys, glob, os, re, csv, json
import numpy as np
def fit_cam(t):
    t=np.sort(t); d=np.diff(t); T=np.median(d)
    k=np.concatenate([[0],np.cumsum(np.round(d/T))])
    A=np.vstack([k,np.ones_like(k)]).T
    (b,a),*_=np.linalg.lstsq(A,t,rcond=None)
    res=t-(a+b*k)
    return b, np.std(res), int(np.sum(np.round(d/T)>1))
def run_rho(f):
    raw=np.loadtxt(f,delimiter=",",skiprows=1,usecols=(0,1))
    cams=sorted(set(raw[:,0].astype(int)))
    per={}; drops={}; resid={}
    for c in cams:
        b,s,dr=fit_cam(raw[raw[:,0]==c,1]); per[c]=b; drops[c]=dr; resid[c]=s
    ref=per[cams[0]]
    ppm={c:(per[c]-ref)/ref*1e6 for c in cams}
    v=list(ppm.values())
    return dict(fps=1/np.mean(list(per.values())), ppm=ppm, pair=max(v)-min(v), drops=drops, resid_us={c:resid[c]*1e6 for c in cams})
if __name__ == "__main__":
    out=[]
    for f in sys.argv[1:]:
        r=run_rho(f); r["file"]=f; out.append(r)
        print("%-60s fps=%8.3f pairwise=%6.3f ppm=%s drops=%s" % (os.path.basename(f), r["fps"], r["pair"], " ".join("%.3f"%x for x in r["ppm"].values()), list(r["drops"].values())))
    json.dump([{k:(v if k!="ppm" and k!="drops" and k!="resid_us" else {str(a):b for a,b in v.items()}) for k,v in r.items()} for r in out], open(os.environ.get("OUTJ","/dev/null"),"w"), default=float)
