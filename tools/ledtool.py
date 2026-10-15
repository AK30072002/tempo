#!/usr/bin/env python3

import argparse
import glob
import math
import os
import re
import sys

STEP_MS_NOMINAL = float(os.environ.get("LED_STEP_MS", "0.25"))
NSPOT = 8

def _read_header(f):
    toks = []
    while len(toks) < 4:
        line = f.readline()
        if not line:
            raise ValueError("truncated header")
        line = line.split(b"#")[0]
        toks += line.split()
    return toks

def read_pgm(path):
    with open(path, "rb") as f:
        t = _read_header(f)
        if t[0] != b"P5":
            raise ValueError(path + " is not a binary PGM")
        w, h = int(t[1]), int(t[2])
        data = f.read(w * h)
    return w, h, data

def read_ppm(path):
    with open(path, "rb") as f:
        t = _read_header(f)
        if t[0] != b"P6":
            raise ValueError(path + " is not a binary PPM")
        w, h = int(t[1]), int(t[2])
        data = bytearray(f.read(w * h * 3))
    return w, h, data

def write_ppm(path, w, h, data):
    with open(path, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (w, h))
        f.write(bytes(data))

def percentile(sorted_vals, q):
    if not sorted_vals:
        return 0.0
    i = (len(sorted_vals) - 1) * q
    lo = int(math.floor(i))
    hi = min(lo + 1, len(sorted_vals) - 1)
    return sorted_vals[lo] + (sorted_vals[hi] - sorted_vals[lo]) * (i - lo)

def median(vals):
    s = sorted(vals)
    return percentile(s, 0.5)

def blob_centroids(w, h, img, thr):
    seen = bytearray(w * h)
    res = []
    for start in range(w * h):
        if img[start] < thr or seen[start]:
            continue
        stack = [start]
        seen[start] = 1
        n = 0
        sw = sx = sy = 0
        peak = 0
        ymin, ymax = h, -1
        while stack:
            p = stack.pop()
            y, x = divmod(p, w)
            v = img[p]
            n += 1
            sw += v
            sx += x * v
            sy += y * v
            if v > peak:
                peak = v
            ymin = min(ymin, y)
            ymax = max(ymax, y)
            if x > 0 and not seen[p - 1] and img[p - 1] >= thr:
                seen[p - 1] = 1; stack.append(p - 1)
            if x < w - 1 and not seen[p + 1] and img[p + 1] >= thr:
                seen[p + 1] = 1; stack.append(p + 1)
            if y > 0 and not seen[p - w] and img[p - w] >= thr:
                seen[p - w] = 1; stack.append(p - w)
            if y < h - 1 and not seen[p + w] and img[p + w] >= thr:
                seen[p + w] = 1; stack.append(p + w)
        if 2 <= n <= 3000 and sw > 0:
            res.append({"x": sx / sw, "y": sy / sw, "n": n, "peak": peak,
                        "sum": sw, "height": ymax - ymin + 1})
    return res

def pick_bar(blobs, h):
    tol = max(6.0, 12.0 * h / 480.0)
    best = None
    for a in blobs:
        grp = [b for b in blobs if abs(b["y"] - a["y"]) <= tol]
        if len(grp) < NSPOT:
            continue
        grp = sorted(grp, key=lambda b: -b["sum"])[:NSPOT]
        grp = sorted(grp, key=lambda b: b["x"])
        gaps = [grp[i + 1]["x"] - grp[i]["x"] for i in range(NSPOT - 1)]
        mg = sum(gaps) / len(gaps)
        if mg <= 0:
            continue
        cv = math.sqrt(sum((g - mg) ** 2 for g in gaps) / len(gaps)) / mg
        tall = sum(1 for b in grp if b["height"] > 3 * max(1.0, math.sqrt(b["n"])))
        meanpeak = sum(b["peak"] for b in grp) / NSPOT
        meany = sum(b["y"] for b in grp) / NSPOT
        score = (cv < 0.45, -tall, meanpeak, -meany)
        key = tuple(round(b["x"]) for b in grp)
        if best is None or score > best[0]:
            best = (score, grp, cv, key)
    return best

def comb_fit(w, h, img, bg, peak, sp_prior=None):
    cand = []
    for frac in (0.5, 0.3, 0.65, 0.8):
        c2 = [b for b in blob_centroids(w, h, img, bg + frac * (peak - bg))
              if b["height"] <= 3 * max(1.0, math.sqrt(b["n"]))]
        if len(c2) > len(cand):
            cand = c2
    if not cand:
        return None
    tol = max(6.0, 12.0 * h / 480.0)
    best = None
    for a0 in cand:
        grp = [b for b in cand if abs(b["y"] - a0["y"]) <= tol]
        tot = sum(b["sum"] for b in grp)
        if best is None or (len(grp), tot) > (len(best), sum(b["sum"] for b in best)):
            best = grp
    grp = best
    sw = sum(b["sum"] for b in grp)
    mx = sum(b["x"] * b["sum"] for b in grp) / sw
    my = sum(b["y"] * b["sum"] for b in grp) / sw
    sxx = sum(b["sum"] * (b["x"] - mx) ** 2 for b in grp)
    slope = (sum(b["sum"] * (b["x"] - mx) * (b["y"] - my) for b in grp) / sxx) if sxx else 0.0
    slope = max(-0.1, min(0.1, slope))
    xs = [b["x"] for b in grp]
    reach = int(10 * (sp_prior if sp_prior else 20))
    xl = max(0, int(min(xs)) - reach)
    xr = min(w - 2, int(max(xs)) + reach)
    prof = {}
    for x in range(xl, xr + 1):
        yc = int(round(my + slope * (x - mx)))
        v = 0
        for dy in (-2, -1, 0, 1, 2):
            yy = yc + dy
            if 0 <= yy < h:
                v = max(v, img[yy * w + x])
        prof[x] = v
    sm = {}
    for x in range(xl, xr + 1):
        vals = [prof[xx] for xx in (x - 1, x, x + 1) if xx in prof]
        sm[x] = sum(vals) / len(vals)

    def P(x):
        i = int(math.floor(x))
        if i < xl or i + 1 > xr:
            return -1e9
        f = x - i
        return sm[i] * (1 - f) + sm[i + 1] * f

    floor_v = bg + 0.25 * (peak - bg)

    def score(x0, sp):
        on = [P(x0 + j * sp) for j in range(NSPOT)]
        if min(on) < floor_v:
            return None
        off = [P(x0 + (j + 0.5) * sp) for j in range(NSPOT - 1)]
        return sum(on) - 0.7 * sum(off)

    if sp_prior:
        lo, hi = 0.75 * sp_prior, 1.3 * sp_prior
    else:
        lo, hi = 6.0, 40.0
    top = None
    sp = lo
    while sp <= hi:
        x0 = float(xl)
        while x0 + 7 * sp <= xr:
            sc = score(x0, sp)
            if sc is not None and (top is None or sc > top[0]):
                top = (sc, x0, sp)
            x0 += 1.0
        sp += 0.25
    if top is None:
        return None
    _, bx0, bsp = top
    for dsp in [i * 0.05 for i in range(-6, 7)]:
        for dx in [i * 0.25 for i in range(-5, 6)]:
            sc = score(bx0 + dx, bsp + dsp)
            if sc is not None and sc > top[0]:
                top = (sc, bx0 + dx, bsp + dsp)
    _, x0, sp = top
    pts = []
    for j in range(NSPOT):
        x = x0 + j * sp
        pts.append((int(round(x)), int(round(my + slope * (x - mx)))))
    return pts, sp, my, math.degrees(math.atan(slope))

def draw_marks(w, h, rgb, pts, color=(0, 255, 0)):
    for i, (x, y) in enumerate(pts):
        c = (255, 0, 0) if i == 0 else color
        r = 6 if i == 0 else 4
        for d in range(-r, r + 1):
            for (px, py) in ((x + d, y - r), (x + d, y + r), (x - r, y + d), (x + r, y + d)):
                if 0 <= px < w and 0 <= py < h:
                    o = (py * w + px) * 3
                    rgb[o:o + 3] = bytes(c)

def report_cam(a, c, w, h, pts, my, tilt, spacing, peak, bg, cv, rows, lines, note):
    status = 0
    rows[c] = my * 480.0 / h
    lines.append("%d %s" % (c, " ".join("%d %d" % p for p in pts)))
    print("  cam%d    %d   %7.1f   %8.2f   %10.1f   %4d   %4d" %
          (c, len(pts), my, tilt, spacing, peak, bg))
    if note:
        print(note)
    if cv >= 0.45:
        print("        WARNING: LED spacing is uneven (cv %.2f) -- check the "
              "marks in the annotated image" % cv)
        status = 1
    if abs(tilt) > 5:
        print("        WARNING: LED row is tilted %.1f deg; level the LED "
              "board or the camera" % tilt)
        status = 1
    last = "%s_cam%d_last.ppm" % (a.prefix, c)
    if os.path.exists(last):
        W, H, rgb = read_ppm(last)
        draw_marks(W, H, rgb, pts)
        write_ppm("%s_cam%d_roi.ppm" % (a.prefix, c), W, H, rgb)
    return status

def cmd_roi(a):
    rows = {}
    lines = []
    status = 0
    failed = []
    pending = []
    spacings = []
    print("")
    print("  cam  LEDs  bar-row  tilt(deg)  spacing(px)  peak  background")
    for c in range(a.ncams):
        mp = "%s_cam%d_max.pgm" % (a.prefix, c)
        if not os.path.exists(mp):
            print("  cam%d: snapshot %s missing" % (c, mp))
            return 2
        w, h, img = read_pgm(mp)
        hist = [0] * 256
        for v in img:
            hist[v] += 1
        acc, bg = 0, 0
        for v in range(256):
            acc += hist[v]
            if acc >= len(img) // 2:
                bg = v
                break
        peak = max(img)
        if peak - bg < 40:
            print("  cam%d: LEDs too dim (peak %d, background %d). Increase "
                  "gain, darken the room, or move the LEDs closer." % (c, peak, bg))
            failed.append(c)
            continue
        best = None
        for frac in (0.5, 0.65, 0.8, 0.9, 0.95, 0.3):
            blobs = blob_centroids(w, h, img, bg + frac * (peak - bg))
            cand = pick_bar(blobs, h)
            if cand is not None and cand[2] < 0.25:
                best = cand
                break
        if best is None:
            pending.append((c, w, h, img, bg, peak))
            continue
        score, grp, cv, _ = best
        xs = [b["x"] for b in grp]
        ys = [b["y"] for b in grp]
        n = len(xs)
        mx, my = sum(xs) / n, sum(ys) / n
        sxx = sum((x - mx) ** 2 for x in xs)
        slope = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx if sxx else 0
        pts = [(int(round(x)), int(round(y))) for x, y in zip(xs, ys)]
        spacing = (xs[-1] - xs[0]) / (n - 1)
        spacings.append(spacing)
        status = max(status, report_cam(a, c, w, h, pts, my, math.degrees(math.atan(slope)),
                                        spacing, peak, bg, cv, rows, lines, ""))

    for (c, w, h, img, bg, peak) in pending:
        prior = median(spacings) if spacings else None
        fit = comb_fit(w, h, img, bg, peak, prior)
        if fit is None:
            print("  cam%d: could not find 8 separate LEDs in one row. "
                  "Usual causes: LEDs too close together in the picture "
                  "(move the LED board CLOSER to the cameras), the camera is "
                  "out of focus, or a reflection/extra light. Check "
                  "%s_cam%d_last.ppm" % (c, a.prefix, c))
            failed.append(c)
            continue
        pts, spacing, my, tilt = fit
        status = max(status, 1)
        status = max(status, report_cam(a, c, w, h, pts, my, tilt, spacing, peak, bg, 0.0,
                                        rows, lines,
                                        "        NOTE: LEDs blur together on this camera; positions found by\n"
                                        "        fitting 8 equally spaced LEDs. Check the marks in the _roi.ppm;\n"
                                        "        the decode test below confirms them."))
    lines.sort()

    if failed:
        print("")
        print("  SETUP NOT READY: camera(s) %s failed (see above). roi.txt not "
              "written." % ", ".join("cam%d" % c for c in failed))
        return 2
    with open(a.out, "w") as f:
        f.write("# cam x0 y0 ... x7 y7   (LED centres, left to right in the image)\n")
        f.write("\n".join(lines) + "\n")
    spread = max(rows.values()) - min(rows.values())
    med = median(list(rows.values()))
    print("")
    print("  LED row spread across cameras: %.1f rows (of 480)" % spread)
    lt_max_ms = a.period_ms / 480.0
    print("  worst-case rolling-shutter error from this spread: %.3f ms "
          "(readout can be at most one frame period)" % (spread * lt_max_ms))
    if spread > a.row_tol:
        status = max(status, 1)
        print("  WARNING: rows differ by more than %d. To align, tilt these "
              "cameras slightly:" % a.row_tol)
        for c in sorted(rows):
            d = rows[c] - med
            if abs(d) > a.row_tol / 2:
                print("     cam%d: LEDs appear %.0f rows %s the others -> tilt "
                      "that camera %s a little" %
                      (c, abs(d), "BELOW" if d > 0 else "ABOVE",
                       "DOWN" if d > 0 else "UP"))
        print("  (You may also continue: the analysis reports the bound above.)")
    else:
        print("  PASS: LED rows match within %d rows." % a.row_tol)
    print("  wrote %s ; check the marks in %s_camN_roi.ppm (red box = first LED)"
          % (a.out, a.prefix))
    return status

def gray_to_bin(g):
    n = g
    m = g >> 1
    while m:
        n ^= m
        m >>= 1
    return n

def load_csv(path):
    cams = {}
    with open(path) as f:
        f.readline()
        for line in f:
            p = line.rstrip().split(",")
            if len(p) < 3 + NSPOT:
                continue
            c = int(p[0])
            t = float(p[1]) * 1e3
            seq = int(p[2])
            v = [int(x) for x in p[3:3 + NSPOT]]
            cams.setdefault(c, []).append((t, seq, v))
    for c in cams:
        cams[c].sort(key=lambda r: r[0])
    return cams

def decode_camera(rows):
    info = {}
    thr, contrast = [], []
    for s in range(NSPOT):
        vals = sorted(r[2][s] for r in rows[:: max(1, len(rows) // 20000)])
        lo, hi = percentile(vals, 0.10), percentile(vals, 0.90)
        thr.append((lo + hi) / 2.0)
        contrast.append(hi - lo)
    info["contrast_min"] = min(contrast)
    amb = 0
    bits = []
    for r in rows:
        b = 0
        near = False
        for s in range(NSPOT):
            v = r[2][s]
            if v > thr[s]:
                b |= 1 << s
            if contrast[s] > 0 and abs(v - thr[s]) < 0.15 * contrast[s]:
                near = True
        amb += near
        bits.append(b)
    info["ambiguous"] = amb / max(1, len(rows))

    gaps = [rows[i + 1][0] - rows[i][0] for i in range(min(len(rows) - 1, 5000))
            if rows[i + 1][1] - rows[i][1] == 1]
    T = median(gaps) if gaps else 10.0
    E = T / STEP_MS_NOMINAL
    best = None
    for order in ("A", "B"):
        vals = []
        for b in bits:
            if order == "B":
                b = int("{:08b}".format(b)[::-1], 2)
            vals.append(gray_to_bin(b))
        good = tot = 0
        for i in range(1, len(rows)):
            if rows[i][1] - rows[i - 1][1] != 1:
                continue
            d = (vals[i] - vals[i - 1]) % 256
            e = E % 256
            dd = min(abs(d - e), 256 - abs(d - e))
            tot += 1
            good += dd <= 2.5
        score = good / tot if tot else 0.0
        if best is None or score > best[0]:
            best = (score, order, vals)
    info["score"], info["order"], vals = best
    info["T_ms"] = T
    return vals, info

def unwrap_counts(rows, vals, steps_per_ms):
    out = []
    prev_n = prev_t = None
    for (t, _, _), v in zip(rows, vals):
        if prev_n is None:
            n = v
        else:
            pred = prev_n + (t - prev_t) * steps_per_ms
            n = v + 256 * round((pred - v) / 256.0)
        out.append(n)
        prev_n, prev_t = n, t
    return out

def linfit(xs, ys):
    n = len(xs)
    mx = sum(xs) / n
    my = sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    b = sxy / sxx
    return my - b * mx, b

def cmd_check(a):
    cams = load_csv(a.csv)
    if len(cams) < 2:
        print("  FAIL: fewer than two cameras in %s" % a.csv)
        return 2
    status = 0
    dec = {}
    print("")
    print("  cam  frames  decode-OK  bit-order  min-contrast  ambiguous")
    for c in sorted(cams):
        rows = cams[c]
        vals, info = decode_camera(rows)
        dec[c] = (vals, info)
        flag = ""
        if info["score"] < 0.90:
            flag = "  <-- FAIL"
            status = 2
        elif info["score"] < 0.97:
            flag = "  <-- marginal"
            status = max(status, 1)
        print("  cam%d %7d   %6.1f%%       %s       %6.0f       %5.1f%%%s" %
              (c, len(rows), 100 * info["score"], info["order"],
               info["contrast_min"], 100 * info["ambiguous"], flag))
    print("  (ambiguous = frames where one LED was caught mid-change. Gray code keeps")
    print("   the error of such a frame to one step, %.2f ms; up to ~40%% is normal.)" % STEP_MS_NOMINAL)
    if status == 2:
        print("")
        print("  The LED counter could not be read reliably. Usual causes:")
        print("   - exposure too long (LEDs look half-lit): lower LED_EXP")
        print("   - LEDs too dim (contrast < 40): raise LED_GAIN or darken room")
        print("   - wrong spots: re-run the setup and check the _roi.ppm marks")
        print("   - Arduino not running the NEW Gray-code sketch")
        return 2

    ref = min(dec, key=lambda c: -dec[c][1]["score"])
    rows0 = cams[ref]
    E = dec[ref][1]["T_ms"] / STEP_MS_NOMINAL
    n0 = unwrap_counts(rows0, dec[ref][0], 1.0 / STEP_MS_NOMINAL)
    _, rate = linfit([r[0] for r in rows0], n0)
    step_ms = 1.0 / rate
    wrap_ms = 256.0 * step_ms

    opt = {}
    offs = {}
    resid = {}
    for c in sorted(cams):
        rows = cams[c]
        n = unwrap_counts(rows, dec[c][0], rate)
        o = [nn / rate - r[0] for nn, r in zip(n, rows)]
        mo = median(o)
        opt[c] = (n, o)
        offs[c] = mo
    for c in sorted(cams):
        d = offs[c] - offs[ref]
        k = round(d / wrap_ms)
        if k:
            n, o = opt[c]
            n = [nn - 256 * k for nn in n]
            o = [oo - k * wrap_ms for oo in o]
            opt[c] = (n, o)
            offs[c] -= k * wrap_ms
        ts = [r[0] for r in cams[c]]
        a0, b0 = linfit(ts, opt[c][1])
        res = [oo - (a0 + b0 * t) for oo, t in zip(opt[c][1], ts)]
        rs = sorted(abs(x) for x in res)
        resid[c] = (percentile(rs, 0.5), percentile(rs, 0.99),
                    sum(1 for x in rs if x > 0.5) / len(rs))

    print("")
    print("  Arduino step measured against the host clock: %.5f ms "
          "(nominal %.5f)" % (step_ms, STEP_MS_NOMINAL))
    print("")
    print("  cam   optical-minus-kernel offset vs cam%d   scatter p50/p99   outliers>0.5ms" % ref)
    for c in sorted(cams):
        p50, p99, out = resid[c]
        print("  cam%d          %+8.3f ms                    %.3f / %.3f ms     %.2f%%" %
              (c, offs[c] - offs[ref], p50, p99, 100 * out))
        if out > 0.02:
            status = max(status, 1)
    print("  (A non-zero offset means that camera's kernel timestamp is late or")
    print("   early relative to its true exposure, compared with cam%d.)" % ref)

    tau = {}
    for c in sorted(cams):
        n, _ = opt[c]
        tau[c] = [nn / rate for nn in n]
    ks = {c: [r[0] for r in cams[c]] for c in cams}
    clist = sorted(cams)

    def stats(xs):
        s = sorted(xs)
        return (sum(s) / len(s), percentile(s, 0.95), s[-1])

    nmin = min(len(ks[c]) for c in clist)
    ik, io = [], []
    for k in range(nmin):
        kt = [ks[c][k] for c in clist]
        ot = [tau[c][k] for c in clist]
        ik.append(max(kt) - min(kt))
        io.append(max(ot) - min(ot))
    nk, no = [], []
    ptr = {c: 0 for c in clist}
    for k in range(len(ks[ref])):
        t0 = ks[ref][k]
        kt, ot = [t0], [tau[ref][k]]
        ok = True
        for c in clist:
            if c == ref:
                continue
            arr = ks[c]
            j = ptr[c]
            while j + 1 < len(arr) and abs(arr[j + 1] - t0) <= abs(arr[j] - t0):
                j += 1
            ptr[c] = j
            if abs(arr[j] - t0) > dec[ref][1]["T_ms"]:
                ok = False
                break
            kt.append(arr[j])
            ot.append(tau[c][j])
        if ok:
            nk.append(max(kt) - min(kt))
            no.append(max(ot) - min(ot))
    T = dec[ref][1]["T_ms"]
    print("")
    print("  FIRST LOOK at ground truth (frame period T = %.3f ms)" % T)
    print("  policy    batches   kernel mean/p95/max (ms)     OPTICAL mean/p95/max (ms)")
    for name, kk, oo in (("INDEX", ik, io), ("NEAREST", nk, no)):
        if not kk:
            continue
        a1, b1, c1 = stats(kk)
        a2, b2, c2 = stats(oo)
        print("  %-8s %8d   %6.2f %6.2f %6.2f         %6.2f %6.2f %6.2f" %
              (name, len(kk), a1, b1, c1, a2, b2, c2))
    print("  (The full analysis, including PREDICTED, is done offline from the CSV.)")
    print("")
    print("  RESULT: %s" % ("PASS" if status == 0 else
                           "WARNING - usable, see notes above" if status == 1 else "FAIL"))
    return status

def cmd_tempo(a):
    files = sorted(glob.glob(os.path.join(a.dir, "*.txt")))
    if not files:
        print("  no run outputs in %s" % a.dir)
        return 1
    print("  %-34s %9s %9s  %s" % ("run", "fps", "maxpair", "ppm per camera"))
    for f in files:
        txt = open(f, errors="replace").read()
        m = re.search(r"period\s+([0-9.]+) ms \(([0-9.]+) fps\)", txt)
        fps = m.group(2) if m else "-"
        ppm = []
        sec = txt.split("MEASURED CLOCKS")
        if len(sec) > 1:
            for line in sec[1].splitlines()[1:12]:
                mm = re.match(r"^\s+(\d+)\s+(-?[0-9.]+)\s+\d+\s+\d+\s+\d+", line)
                if mm:
                    ppm.append(float(mm.group(2)))
        mp = (max(ppm) - min(ppm)) if ppm else float("nan")
        print("  %-34s %9s %9.2f  %s" % (os.path.basename(f)[:-4], fps, mp,
                                          " ".join("%.2f" % p for p in ppm)))
    return 0

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest="cmd")
    r = sp.add_parser("roi")
    r.add_argument("--prefix", required=True)
    r.add_argument("--ncams", type=int, default=4)
    r.add_argument("--out", required=True)
    r.add_argument("--row-tol", type=int, default=8)
    r.add_argument("--period-ms", type=float, default=10.0)
    c = sp.add_parser("check")
    c.add_argument("csv")
    t = sp.add_parser("tempo")
    t.add_argument("dir")
    a = ap.parse_args()
    if a.cmd == "roi":
        sys.exit(cmd_roi(a))
    if a.cmd == "check":
        sys.exit(cmd_check(a))
    if a.cmd == "tempo":
        sys.exit(cmd_tempo(a))
    ap.print_help()
    sys.exit(2)

if __name__ == "__main__":
    main()
