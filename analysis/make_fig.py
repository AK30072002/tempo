import json, numpy as np, matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from common import OUT
C1, C2, C3, INK, MUTED = "#2a78d6", "#eb6834", "#1baf7a", "#2b2b2b", "#8a8a85"
plt.rcParams.update({"font.family": "serif", "font.size": 7, "axes.linewidth": 0.6,
                     "xtick.major.width": 0.6, "ytick.major.width": 0.6,
                     "axes.edgecolor": INK, "axes.labelcolor": INK,
                     "xtick.color": INK, "ytick.color": INK, "pdf.fonttype": 42})
fig = plt.figure(figsize=(3.5, 3.15))
gs = fig.add_gridspec(2, 2, height_ratios=[1, 1.05], hspace=0.55, wspace=0.42,
                      left=0.12, right=0.98, top=0.95, bottom=0.12)

d = json.load(open(OUT + "/drift1h.json"))
T = d["T"]; k = np.array(d["k"]); tmin = k * T / 60000
ax = fig.add_subplot(gs[0, :])
tt = np.linspace(0, 60, 200)
lo, hi = 7.8e-6 * tt * 60000, 12.6e-6 * tt * 60000
ax.fill_between(tt, lo, hi, color=C1, alpha=0.13, lw=0, label=r"predicted, $\rho$ = 7.8–12.6 ppm")
for name, c in d["curves"].items():
    y = np.abs(np.array(c))
    big = abs(d["rho"][name]) > 5
    ax.plot(tmin, y, color=C1 if big else MUTED, lw=1.0 if big else 0.8)
ax.axhline(T, color=C2, lw=0.9, ls="--")
ax.text(1, T + 1.2, r"$T$ = 10 ms", color=INK, fontsize=6.5)
ax.text(2.2, 33.5, "dark lines: pairs with cam0 (8.6–10.5 ppm)", color=INK, fontsize=6.2)
ax.text(36, 7.4, "other pairs (0.5–1.8 ppm)", color=INK, fontsize=6.2)
ax.annotate("first crossing\n16.6 min", xy=(16.6, T), xytext=(24, 22.5), fontsize=6.2,
            arrowprops=dict(arrowstyle="-", lw=0.5, color=INK))
ax.set_xlim(0, 60); ax.set_ylim(0, 45)
ax.set_xlabel("elapsed time (min)"); ax.set_ylabel(r"|drift| from start (ms)")
ax.legend(loc="upper left", frameon=False, fontsize=6.2)
ax.set_title("(a) Index offset growth, start-aligned, 1 h run", fontsize=7, loc="left")

r = json.load(open(OUT + "/rate_rows.json"))
ax = fig.add_subplot(gs[1, 0])
modes = [("30fps_3840x2160", 30, "o"), ("72fps_1920x1080", 72, "s"),
         ("72fps_1280x720", 72, "^"), ("100fps_640x480", 100, "D")]
allr = np.array([x["rho"] for x in r]); m = allr.mean()
ax.axhspan(0.95 * m, 1.05 * m, color=C3, alpha=0.18, lw=0)
ax.text(24, 7.55, r"band: mean $\pm$5%", fontsize=5.8, color=INK)
rng = np.random.default_rng(3)
for i, (md, f, mk) in enumerate(modes):
    v = np.array([x["rho"] for x in r if x["mode"] == md])
    off = {0: 0, 1: -3, 2: 3, 3: 0}[i]
    ax.scatter(f + off + rng.uniform(-1.2, 1.2, len(v)), v, s=4, color=MUTED, lw=0)
    ax.errorbar(f + off, v.mean(), yerr=v.std(ddof=1), fmt=mk, ms=3.5, color=C1,
                mec=C1, elinewidth=0.8, capsize=1.5)
ax.set_xlim(22, 108); ax.set_ylim(7.4, 11.4)
ax.set_xticks([30, 72, 100])
ax.set_xlabel("frame rate (fps)"); ax.set_ylabel(r"max pairwise $\rho$ (ppm)")
ax.set_title(r"(b) $\rho$ vs rate, $n$=118", fontsize=7, loc="left")

ax = fig.add_subplot(gs[1, 1])
pts = json.load(open(OUT + "/rowfit_points.json"))
from collections import defaultdict
agg = defaultdict(list)
for p in pts: agg[(p["geo"], p["cam"])].append(p)
mk = {1: ("o", C1), 2: ("s", C2), 3: ("^", C3)}
for (g, c), v in agg.items():
    x = np.mean([q["dE"] for q in v]); y = np.mean([q["off"] for q in v])
    ax.scatter(x, y * 1e3 / 1e3, marker=mk[c][0], s=10, color=mk[c][1], lw=0.4, edgecolor="white")
xx = np.linspace(-45, 50, 10)
ax.plot(xx, 16.95e-3 * xx, color=INK, lw=0.7)
ax.text(-44, 0.55, "16.95 µs/row\nresid. 1 µs RMS", fontsize=5.8, color=INK)
for c in (1, 2, 3):
    ax.scatter([], [], marker=mk[c][0], s=10, color=mk[c][1], label=f"cam{c}")
ax.legend(loc="lower right", frameon=False, fontsize=5.8, handletextpad=0.1, borderaxespad=0.1)
ax.set_xlabel("row offset to cam0"); ax.set_ylabel("optical − kernel (ms)")
ax.set_title("(c) LED, 7 setups, 2 days", fontsize=7, loc="left")
for a in fig.axes:
    a.spines[["top", "right"]].set_visible(False)
    a.grid(axis="y", color="#e4e4e0", lw=0.4); a.set_axisbelow(True)
fig.savefig(OUT + "/fig_results.pdf")
fig.savefig(OUT + "/fig_results.png", dpi=220)
print("ok")
