import pickle, numpy as np
from common import d, OUT
R = pickle.load(open(OUT + "/led_res.pkl", "rb"))
W = np.array([2.0 ** -(b + 1) for b in range(8)]); W[7] = W[6]; W /= W.sum()
rows = {}
for L in open(d("07_led_orin", "roi.txt")):
    if L.startswith("#") or not L.strip(): continue
    f = list(map(int, L.split())); rows[f[0]] = (W * np.array(f[2::2], float)).sum()
lt = 16.95e-3; T = 9.9965
for name, r in R.items():
    cams = r["cams"]
    for pn in ("INDEX", "NEAREST"):
        x = r["res"][pn]; sel = np.array(x["sel"]); raw = sel.max(1) - sel.min(1)
        corr = sel - np.array([(rows[c] - rows[cams[0]]) * lt for c in cams]); cs = corr.max(1) - corr.min(1)
        print("%-14s %-8s n=%6d  raw mean %.2f max %.2f >T %5d (%.2f%%) | row-corrected mean %.2f max %.2f >T %5d | kernel mean %.2f" %
              (name, pn, len(raw), raw.mean(), raw.max(), (raw > T).sum(), 100 * (raw > T).mean(), cs.mean(), cs.max(), (cs > T).sum(), x["k_mean"]))
