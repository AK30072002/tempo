import glob, subprocess, numpy as np, os
from common import d, HERE
SEL = os.path.join(HERE, "led_select")
files = sorted(glob.glob(d("06_rate_study_118runs", "rate", "data", "*.csv"))) + \
        [d("02_orin_live_and_muxer", "data", r + ".csv") for r in ("freerun_1h", "freerun_r1", "freerun_r2", "freerun_r3")]
tot = over = 0; worst = 0
for f in files:
    x = np.loadtxt(f, delimiter=",", skiprows=1, usecols=(0, 1))
    T = np.median(np.diff(np.sort(x[x[:, 0] == x[0, 0], 1]))) * 1e9
    out = subprocess.run([SEL, f, "%.6f" % (1e9 / T)], capture_output=True, text=True).stdout.splitlines()
    sp = np.array([int(l.split(",")[3]) for l in out if l and not l.startswith("#") and l.split(",")[0] == "1"])
    tot += len(sp); over += int((sp > T).sum()); worst = max(worst, sp.max() / T)
print("runs %d  NEAREST batches %d  over T %d  worst spread/T %.3f" % (len(files), tot, over, worst))
