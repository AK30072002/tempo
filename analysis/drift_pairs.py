import numpy as np, itertools, json
from common import d, OUT
E = d("02_orin_live_and_muxer", "data")
for run in ("freerun_1h", "freerun_r1", "freerun_r2", "freerun_r3"):
    x = np.loadtxt(f"{E}/{run}.csv", delimiter=",", skiprows=1)
    t = {c: np.sort(x[x[:, 0] == c, 1]) * 1e3 for c in range(4)}
    n = min(len(v) for v in t.values()); T = np.median(np.diff(t[0]))
    print("%s T=%.4f ms, start offsets (frames): %s" % (run, T, [round((t[c][0] - t[0][0]) / T, 2) for c in range(4)]))
    curves, rhos = {}, {}
    for i, j in itertools.combinations(range(4), 2):
        dd = t[j][:n] - t[i][:n]; k = np.arange(n)
        rho = np.polyfit(k, dd, 1)[0] / T * 1e6; drift = dd - dd[0]
        cr = np.where(abs(drift) >= T)[0]
        print("   pair %d-%d rho %+.2f ppm  drift %+.2f ms (%.2f T)  first |drift|>=T: %s" %
              (i, j, rho, drift[-1], abs(drift[-1]) / T, ("%.1f min" % (cr[0] * T / 60000)) if len(cr) else "never"))
        curves["%d-%d" % (i, j)] = drift[::600].tolist(); rhos["%d-%d" % (i, j)] = rho
    if run == "freerun_1h":
        json.dump(dict(k=np.arange(n)[::600].tolist(), T=T, curves=curves, rho=rhos), open(OUT + "/drift1h.json", "w"))
        m = max(abs(v) for v in rhos.values())
        print("   worst pair %.2f ppm -> predicted crossing %.1f min, predicted 1 h drift %.2f T" % (m, 1e6 / m * T / 60000, m * 1e-6 * n))
