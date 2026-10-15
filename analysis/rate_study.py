import glob, json, csv, numpy as np
from scipy import stats
from common import d, OUT
from rho import run_rho
R = d("06_rate_study_118runs")
idx = {r["tag"]: r for r in csv.DictReader(open(R + "/rate/index.csv"))}
temps = np.genfromtxt(R + "/temperature_log.csv", delimiter=",", names=True)
rows = []
for f in sorted(glob.glob(R + "/rate/data/*.csv")):
    tag = f.split("/")[-1][:-4]; ix = idx[tag]; r = run_rho(f)
    t0, t1 = int(ix["start_epoch"]), int(ix["end_epoch"])
    m = (temps["epoch"] >= t0) & (temps["epoch"] <= t1)
    rows.append(dict(tag=tag, blk=int(tag[1:3]), mode=tag.split("_", 1)[1], fps=float(r["fps"]),
                     rho=float(r["pair"]), tj=float(np.mean(temps["tjtherm"][m]) / 1000), t0=t0))
rows.sort(key=lambda x: x["t0"])
json.dump(rows, open(OUT + "/rate_rows.json", "w"))
fps = np.array([x["fps"] for x in rows]); rho = np.array([x["rho"] for x in rows])
tj = np.array([x["tj"] for x in rows]); order = np.arange(len(rows)); mean = rho.mean()
print("N runs %d   rho %.3f +- %.3f ppm (range %.2f-%.2f)" % (len(rows), mean, rho.std(ddof=1), rho.min(), rho.max()))
modes = ["100fps_640x480", "72fps_1280x720", "72fps_1920x1080", "30fps_3840x2160"]
grp = [np.array([x["rho"] for x in rows if x["mode"] == m]) for m in modes]
for m, g in zip(modes, grp): print("  %-18s n=%2d mean %.3f SD %.3f" % (m, len(g), g.mean(), g.std(ddof=1)))
F, p = stats.f_oneway(*grp); print("ANOVA F(3,%d)=%.2f p=%.2f" % (len(rows) - 4, F, p))
span = fps.max() - fps.min()
def tost(slope, se, dfree, marg):
    dd = marg * mean / span
    return max(stats.t.sf((slope + dd) / se, dfree), stats.t.sf((dd - slope) / se, dfree))
lr = stats.linregress(fps, rho); dfree = len(rho) - 2; t90 = stats.t.ppf(0.95, dfree)
print("OLS slope %+.4f +- %.4f ppm/fps; change over %.0f fps: 90%% CI [%+.1f%%, %+.1f%%] of rho" %
      (lr.slope, lr.stderr, span, 100 * (lr.slope - t90 * lr.stderr) * span / mean, 100 * (lr.slope + t90 * lr.stderr) * span / mean))
blks = sorted(set(x["blk"] for x in rows))
X = np.column_stack([fps] + [np.array([x["blk"] == b for x in rows], float) for b in blks])
b, *_ = np.linalg.lstsq(X, rho, rcond=None); res = rho - X @ b; dfw = len(rho) - X.shape[1]
cov = (res @ res / dfw) * np.linalg.pinv(X.T @ X); s, se = b[0], np.sqrt(cov[0, 0]); t90w = stats.t.ppf(0.95, dfw)
print("WITHIN-BLOCK slope %+.4f +- %.4f ppm/fps -> change %+.1f%% of rho, 90%% CI [%+.1f%%, %+.1f%%]" %
      (s, se, 100 * s * span / mean, 100 * (s - t90w * se) * span / mean, 100 * (s + t90w * se) * span / mean))
for marg in (0.10, 0.05): print("  TOST +-%d%%: p = %.2g" % (marg * 100, tost(s, se, dfw, marg)))
blkmeans = [rho[[x["blk"] == bb for x in rows]].mean() for bb in blks]
print("block means %.1f-%.1f ppm" % (min(blkmeans), max(blkmeans)))
print("corr(rho, Tj) r=%.2f ; corr(Tj, run order) r=%.2f" % (stats.pearsonr(tj, rho)[0], stats.pearsonr(tj, order)[0]))
