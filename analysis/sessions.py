import glob, re
from common import d
from rho import run_rho
T = 9.9965
S = {}
v = []
for f in sorted(glob.glob(d("01_drift_12h_study", "analysis_block*_freerun.txt"))):
    sec = open(f).read().split("UNWRAPPED SKEW")[1]
    p = [0.0] + [float(x) for x in re.findall(r"^0-\d\s+\S+\s+\S+\s+\d+\s+(\S+)", sec, re.M)]
    v.append(max(p) - min(p))
S["12 h study (vendor driver)"] = v
S["live runs (vendor driver)"] = [run_rho(f)["pair"] for f in sorted(glob.glob(d("02_orin_live_and_muxer", "data", "freerun_*.csv")))]
S["rate study (stock driver)"] = [run_rho(f)["pair"] for f in sorted(glob.glob(d("06_rate_study_118runs", "rate", "data", "*.csv")))]
S["LED runs (stock driver)"] = [run_rho(f)["pair"] for f in sorted(glob.glob(d("07_led_orin", "*.csv")) + glob.glob(d("08_led_board_geometry", "*", "*.csv")))]
for k, x in S.items():
    print("%-28s n=%3d  max pairwise rho %.2f-%.2f ppm" % (k, len(x), min(x), max(x)))
a = [y for x in S.values() for y in x]
lo, hi = min(a), max(a)
print("all sessions: %.2f-%.2f ppm -> k_viol %.1f-%.1f kframes (%.1f-%.1f min at T=%.4f ms)"
      % (lo, hi, 1e3 / hi, 1e3 / lo, 1e6 / hi * T / 60000, 1e6 / lo * T / 60000, T))
