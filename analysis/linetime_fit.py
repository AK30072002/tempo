import json, numpy as np
from common import OUT
pts=json.load(open(OUT+"/rowfit_points.json"))
from collections import defaultdict
agg=defaultdict(list)
for p in pts: agg[(p['geo'],p['cam'])].append(p)
P=[dict(geo=g,cam=c,off=np.mean([x['off'] for x in v]),dM=np.mean([x['dM'] for x in v]),dE=np.mean([x['dE'] for x in v])) for (g,c),v in agg.items()]
def fit(P,key,cams=(1,2,3),common=True):
    P=[p for p in P if p['cam'] in cams]
    if common:
        X=np.zeros((len(P),len(cams)+1)); y=np.array([p['off'] for p in P])
        for i,p in enumerate(P): X[i,cams.index(p['cam'])]=1; X[i,-1]=p[key]
        b,*_=np.linalg.lstsq(X,y,rcond=None); r=y-X@b
        dof=len(y)-X.shape[1]; s2=r@r/dof; cov=s2*np.linalg.inv(X.T@X)
        return b,np.sqrt(np.diag(cov)),r,dof
for key,lab in (("dM","mean LED row"),("dE","toggle-weighted LED row")):
    b,se,r,dof=fit(P,key)
    print(f"\n== {lab}: common line time {1e3*b[-1]:.2f} +/- {1e3*se[-1]:.2f} us/row (dof {dof}); readout 480 rows = {480*b[-1]:.2f} ms")
    print("   latency vs cam0: "+"  ".join(f"cam{c} {1e3*b[i]:+.1f}+/-{1e3*se[i]:.1f} us" for i,c in enumerate((1,2,3))))
    print(f"   residual RMS {1e3*np.sqrt(np.mean(r**2)):.1f} us, max {1e3*abs(r).max():.1f} us")
    for c in (1,2,3):
        Q=[p for p in P if p['cam']==c]; x=np.array([q[key] for q in Q]); y=np.array([q['off'] for q in Q])
        A=np.vstack([np.ones_like(x),x]).T; bb,*_=np.linalg.lstsq(A,y,rcond=None); rr=y-A@bb
        se_s=np.sqrt(rr@rr/(len(y)-2)/np.sum((x-x.mean())**2))
        print(f"   cam{c} alone: {1e3*bb[1]:.2f} +/- {1e3*se_s:.2f} us/row, intercept {1e3*bb[0]:+.0f} us, row range {x.min():.0f}..{x.max():.0f} (n={len(x)})")
    Pt=[p for p in P if p['geo']!='A']; b2,se2,_,_=fit(Pt,key)
    pa=[p for p in P if p['geo']=='A']
    pred=[b2[(1,2,3).index(p['cam'])]+b2[-1]*p[key] for p in pa]
    print(f"   today-only fit: {1e3*b2[-1]:.2f} +/- {1e3*se2[-1]:.2f} us/row; predicts 13 Oct offsets "+", ".join(f"cam{p['cam']} {q:+.3f} (meas {p['off']:+.3f})" for p,q in zip(pa,pred)))
print("\npoints:")
for p in sorted(P,key=lambda p:(p['cam'],p['geo'])): print(f"  cam{p['cam']} {p['geo']:3s} off {p['off']:+.3f} dM {p['dM']:+6.1f} dE {p['dE']:+6.1f}")
