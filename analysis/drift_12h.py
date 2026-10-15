import glob, re, itertools, statistics as st
from common import d
T = 9.9965
for cond in ("freerun", "sync60"):
    rho, drift, over, n, other = [], [], 0, 0, []
    for f in sorted(glob.glob(d("01_drift_12h_study", "analysis_block*_%s.txt" % cond))):
        sec = open(f).read().split("UNWRAPPED SKEW")[1]
        mm, proj = {0: 0.0}, {0: 0.0}
        for c, m, sd, segs, p, pj in re.findall(r"^0-(\d)\s+(\S+)\s+(\S+)\s+(\d+)\s+(\S+)\s+(\S+)", sec, re.M):
            mm[int(c)] = float(m); proj[int(c)] = float(pj)
        pairs = list(itertools.combinations(range(4), 2))
        rho.append(max(abs(mm[j] - mm[i]) for i, j in pairs) / 60000 * 1e6)
        drift.append(max(abs(mm[j] - mm[i]) for i, j in pairs))
        for i, j in pairs:
            n += 1; over += abs(proj[j] - proj[i]) >= T
            if i > 0: other.append(abs(mm[j] - mm[i]) / 60000 * 1e6)
    print("%-7s blocks %d  max pairwise rho %.3f +- %.3f ppm (max %.2f)  drift %.4f +- %.4f ms/min  pair-hours over T %d/%d  pairs without cam0 %.2f-%.2f ppm"
          % (cond, len(rho), st.mean(rho), st.stdev(rho), max(rho), st.mean(drift), st.stdev(drift), over, n, min(other), max(other)))
