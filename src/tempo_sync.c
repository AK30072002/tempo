#define _GNU_SOURCE
#include "tempo.h"
#include "v4l2cap.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>

#define MAXCAM  TEMPO_MAX_CAMS
#define WINDOW  512
#define NPOL    3

static volatile sig_atomic_t stop_flag = 0;

static v4l2cap *g_cap = NULL;
static double   g_requested_fps = 0;

static void cleanup_capture(void)
{
    if (g_cap) { v4l2cap_close(g_cap); g_cap = NULL; }
}

static void on_signal(int s)
{
    (void)s;
    stop_flag = 1;
    if (g_cap) { cleanup_capture(); _exit(130); }
}

typedef struct {
    const char   *name;
    tempo_policy  pol;
    tempo_aligner al;
    tempo_clock   clocks[MAXCAM];
    int64_t       tbuf[MAXCAM][WINDOW];
    uint32_t      kbuf[MAXCAM][WINDOW];

    uint64_t      n;
    int64_t       sum_ns;
    int64_t       max_ns;
    int64_t       first_sum, last_sum;
    uint64_t      first_n, last_n;
    int64_t       first_t, last_t;
    uint32_t      hist[257];
    int64_t       bucket_ns;
    int64_t       firstw[512], lastw[512];
    uint32_t      firstw_n, lastw_n, lastw_pos;
} policy_t;

static policy_t POL[NPOL];

static void policy_init(policy_t *p, const char *name, tempo_policy pol,
                        int ncams, int64_t nominal_ns)
{
    int c;
    p->name = name;
    p->pol  = pol;
    p->n = 0; p->sum_ns = 0; p->max_ns = 0;
    p->first_sum = p->last_sum = 0;
    p->first_n = p->last_n = 0;
    p->firstw_n = p->lastw_n = p->lastw_pos = 0;
    p->bucket_ns = (nominal_ns * 8) / 256;
    if (p->bucket_ns < 1000) p->bucket_ns = 1000;
    memset(p->hist, 0, sizeof p->hist);
    tempo_init(&p->al, p->clocks, (uint8_t)ncams, nominal_ns);
    for (c = 0; c < ncams; c++)
        tempo_clock_attach(&p->al, (uint8_t)c, p->tbuf[c], p->kbuf[c], WINDOW);
    tempo_set_policy(&p->al, pol);
}

static void policy_record(policy_t *p, int64_t spread_ns, int64_t t_ns)
{
    int b;
    p->n++;
    p->sum_ns += spread_ns;
    if (spread_ns > p->max_ns) p->max_ns = spread_ns;

    b = (int)(spread_ns / p->bucket_ns);
    if (b < 0) b = 0;
    if (b > 256) b = 256;
    p->hist[b]++;

    if (p->firstw_n < 512) {
        p->firstw[p->firstw_n++] = spread_ns;
        p->first_t = t_ns;
    }
    p->lastw[p->lastw_pos] = spread_ns;
    p->lastw_pos = (p->lastw_pos + 1) % 512;
    if (p->lastw_n < 512) p->lastw_n++;
    p->last_t = t_ns;
}

static double window_mean_ms(const int64_t *v, uint32_t n)
{
    int64_t acc = 0;
    uint32_t i;
    if (!n) return 0;
    for (i = 0; i < n; i++) acc += v[i];
    return (double)acc / (double)n / 1e6;
}

static double pctile(const policy_t *p, double q)
{
    uint64_t target = (uint64_t)(q * (double)p->n), acc = 0;
    int i;
    for (i = 0; i <= 256; i++) {
        acc += p->hist[i];
        if (acc >= target)
            return (i + 0.5) * (double)p->bucket_ns / 1e6;
    }
    return (double)p->max_ns / 1e6;
}

static int pctile_saturated(const policy_t *p) { return p->hist[256] > 0; }

static double cpu_seconds(void)
{
    struct rusage r;
    if (getrusage(RUSAGE_SELF, &r) != 0) return 0;
    return (double)r.ru_utime.tv_sec + r.ru_utime.tv_usec / 1e6
         + (double)r.ru_stime.tv_sec + r.ru_stime.tv_usec / 1e6;
}

static int64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void feed(int cam, int64_t t_ns, int ncams)
{
    int i;
    (void)ncams;
    for (i = 0; i < NPOL; i++) {
        tempo_batch b;
        tempo_push(&POL[i].al, (uint8_t)cam, t_ns);
        while (tempo_next(&POL[i].al, &b) == TEMPO_OK)
            policy_record(&POL[i], b.spread_ns, b.target_ns);
    }
}

static void report(int ncams, double wall_s, double cpu_s,
                   uint64_t frames, const v4l2cap *cap)
{
    tempo_report r;
    int i, c;

    tempo_get_report(&POL[2].al, &r);

    printf("\n");
    printf("================================================================\n");
    printf("MEASURED CLOCKS\n");
    printf("================================================================\n");
    printf("  %-5s %12s %11s %11s %11s\n",
           "cam", "ppm", "resid_ns", "frames", "recovered");
    for (c = 0; c < ncams; c++)
        printf("  %-5d %8d.%03d %11lld %11u %11u\n", c,
               r.ppm_x1000[c] / 1000, abs(r.ppm_x1000[c] % 1000),
               (long long)r.resid_ns[c],
               r.frames[c], r.dropouts[c]);
    {
        int tc = tempo_trusted_count(&POL[2].al);
        if (tc < ncams)
            printf("  MODEL DEGRADED: %d/%d cameras have a usable clock fit; "
                   "the rest\n                  fall back to arrival-time "
                   "selection (irregular delivery?)\n", tc, ncams);
    }
    printf("  period %.4f ms (%.3f fps)   bound %.4f ms\n",
           (double)r.period_ns / 1e6,
           r.period_ns ? 1e9 / (double)r.period_ns : 0.0,
           (double)r.bound_ns / 1e6);
    if (g_requested_fps > 0 && r.period_ns) {
        double got = 1e9 / (double)r.period_ns;
        if (got < g_requested_fps * 0.9 || got > g_requested_fps * 1.1)
            printf("  *** MEASURED RATE %.2f fps DIFFERS FROM REQUESTED "
                   "%.2f fps ***\n      hardware sync or a mode limit is in "
                   "effect; check frame_sync\n", got, g_requested_fps);
    }
    if (cap) {
        printf("  kernel timestamp semantics:");
        for (c = 0; c < ncams; c++)
            printf(" cam%d=%s", c, v4l2cap_ts_desc(cap, c));
        printf("\n");
        for (c = 0; c < ncams; c++)
            if (cap->dev[c].seq_gaps)
                printf("  cam%d: %llu frames dropped by the DRIVER "
                       "(kernel sequence gaps)\n",
                       c, (unsigned long long)cap->dev[c].seq_gaps);
    }

    printf("\n================================================================\n");
    printf("POLICY COMPARISON   identical frames, three grouping rules\n");
    printf("================================================================\n");
    printf("  %-24s %9s %9s %9s %9s %11s\n",
           "policy", "mean_ms", "p50_ms", "p95_ms", "max_ms", "batches");
    for (i = 0; i < NPOL; i++) {
        policy_t *p = &POL[i];
        if (!p->n) { printf("  %-24s  (no batches)\n", p->name); continue; }
        printf("  %-24s %9.4f %9.4f %9.4f %9.4f %11llu%s\n", p->name,
               (double)p->sum_ns / (double)p->n / 1e6,
               pctile(p, 0.50), pctile(p, 0.95),
               (double)p->max_ns / 1e6,
               (unsigned long long)p->n,
               pctile_saturated(p) ? "  (p50/p95 are lower bounds)" : "");
    }

    printf("\n  misalignment growth (first 512 batches -> last 512):\n");
    for (i = 0; i < NPOL; i++) {
        policy_t *p = &POL[i];
        double f, l, mins, excess;
        if (p->firstw_n < 64 || p->lastw_n < 64) continue;
        f = window_mean_ms(p->firstw, p->firstw_n);
        l = window_mean_ms(p->lastw,  p->lastw_n);
        mins = (double)(p->last_t - p->first_t) / 6e10;
        excess = (double)p->max_ns / (double)(r.bound_ns ? r.bound_ns : 1);
        printf("    %-24s %7.3f -> %7.3f ms   %+8.4f ms/min   max %.2fx bound%s\n",
               p->name, f, l, mins > 0 ? (l - f) / mins : 0.0, excess,
               excess > 1.6 ? "   UNBOUNDED" : "");
    }
    printf("    (the bound assumes no missing frame near the target; a\n"
           "     dropout widens the nearest-available gap to ~1.5 periods)\n");

    if (POL[1].n && POL[2].n) {
        double a = pctile(&POL[1], 0.50), b = pctile(&POL[2], 0.50);
        printf("\n  tempo vs ApproximateTime (median): %.4f -> %.4f ms",
               a, b);
        if (b > 0) printf("   %.2fx", a / b);
        printf("\n");
    }

    printf("\n================================================================\n");
    printf("COST\n");
    printf("================================================================\n");
    printf("  wall %.1f s   CPU %.2f s   (%.2f%% of one core)\n",
           wall_s, cpu_s, wall_s > 0 ? 100.0 * cpu_s / wall_s : 0.0);
    if (frames)
        printf("  %llu frames   %.3f us CPU per frame  (all three policies)\n",
               (unsigned long long)frames, 1e6 * cpu_s / (double)frames);
    printf("  per-policy state: %zu B/aligner + %d B ring/cam\n",
           sizeof(tempo_aligner) + sizeof(tempo_clock) * MAXCAM,
           WINDOW * 12);
    if (cap) {
        int eof = 0, soe = 0, c2;
        for (c2 = 0; c2 < ncams; c2++) {
            if (!cap->dev[c2].ts_flags_known) continue;
            if (cap->dev[c2].ts_src_eof) eof++; else soe++;
        }
        printf("  NOTE: alignment derives from kernel V4L2 timestamps");
        if (eof && !soe)      printf(" (end-of-frame).\n");
        else if (soe && !eof) printf(" (start-of-exposure).\n");
        else if (eof && soe)  printf(" (MIXED EOF/SoE across cameras --\n"
                                     "        cross-camera comparison is "
                                     "not meaningful).\n");
        else                  printf(".\n");
        if (eof)
            printf("        EOF sourcing includes readout latency: constant\n"
                   "        per-channel, so it cancels from a drift RATE but\n"
                   "        not from absolute alignment.\n");
        printf("        Absolute alignment needs an optical reference.\n");
    } else {
        printf("  NOTE: alignment derives from recorded timestamps; their\n"
               "        source semantics are whatever the capture used.\n");
    }
}

static int run_replay(const char *path, double fps_hint)
{
    FILE *f = fopen(path, "r");
    char line[256];
    int ncams = 0, seen[MAXCAM];
    int64_t nominal = (int64_t)(1e9 / (fps_hint > 0 ? fps_hint : 100.0));
    uint64_t frames = 0;
    double cpu0, wall0;
    int i;

    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 1; }
    memset(seen, 0, sizeof seen);

    if (!fgets(line, sizeof line, f)) { fclose(f); return 1; }
    while (fgets(line, sizeof line, f)) {
        int c; double t;
        if (sscanf(line, "%d,%lf", &c, &t) != 2) continue;
        if (c >= 0 && c < MAXCAM && !seen[c]) { seen[c] = 1;
            if (c + 1 > ncams) ncams = c + 1; }
    }
    rewind(f);
    if (ncams < 2) { fprintf(stderr, "need >= 2 cameras\n"); fclose(f);
                     return 1; }

    policy_init(&POL[0], "INDEX (naive muxer)",  TEMPO_POLICY_INDEX,
                ncams, nominal);
    policy_init(&POL[1], "NEAREST (ApproxTime)", TEMPO_POLICY_NEAREST,
                ncams, nominal);
    policy_init(&POL[2], "PREDICTED (tempo)",    TEMPO_POLICY_PREDICTED,
                ncams, nominal);

    printf("replay: %s   cameras %d\n", path, ncams);
    cpu0 = cpu_seconds();
    wall0 = (double)mono_ns() / 1e9;

    if (!fgets(line, sizeof line, f)) { fclose(f); return 1; }
    while (fgets(line, sizeof line, f)) {
        int c; double t;
        if (sscanf(line, "%d,%lf", &c, &t) != 2) continue;
        if (c < 0 || c >= ncams) continue;
        feed(c, (int64_t)(t * 1e9 + 0.5), ncams);
        frames++;
    }
    fclose(f);

    report(ncams, (double)mono_ns() / 1e9 - wall0,
           cpu_seconds() - cpu0, frames, NULL);
    for (i = 0; i < NPOL; i++) (void)i;
    return 0;
}

static int run_live(const int *cams, int ncams, uint32_t w, uint32_t h,
                    uint32_t fps, double seconds, const char *csv_path)
{
    v4l2cap cap;
    v4l2cap_frame fr;
    FILE *csv = NULL;
    int64_t nominal = (int64_t)(1e9 / (fps ? fps : 100));
    int64_t t_end;
    uint64_t frames = 0;
    double cpu0, wall0;
    int i, rc;

    g_requested_fps = (double)fps;
    if (v4l2cap_open(&cap, cams, ncams, w, h, fps, 0, 6) < 0) {
        fprintf(stderr, "capture setup failed\n");
        return 1;
    }

    for (i = 0; i < NPOL; i++) {   }
    policy_init(&POL[0], "INDEX (naive muxer)",  TEMPO_POLICY_INDEX,
                ncams, nominal);
    policy_init(&POL[1], "NEAREST (ApproxTime)", TEMPO_POLICY_NEAREST,
                ncams, nominal);
    policy_init(&POL[2], "PREDICTED (tempo)",    TEMPO_POLICY_PREDICTED,
                ncams, nominal);

    if (csv_path) {
        csv = fopen(csv_path, "w");
        if (csv) fprintf(csv, "camera,wall_monotonic_s\n");
        else fprintf(stderr, "warning: cannot write %s\n", csv_path);
    }

    if (v4l2cap_start(&cap) < 0) { v4l2cap_close(&cap); return 1; }

    {
        uint32_t sid = v4l2cap_find_ctrl(cams[0], "frame_sync");
        uint32_t eid = v4l2cap_find_ctrl(cams[0], "exposure_time_absolute");
        int32_t v;
        printf("capture mode:");
        if (sid && v4l2cap_get_ctrl(cams[0], sid, &v) == 0)
            printf("  frame_sync=%d  %s", v,
                   v ? "*** HARDWARE SYNC ACTIVE ***" : "(free-running)");
        else
            printf("  frame_sync=<not exposed>");
        if (eid && v4l2cap_get_ctrl(cams[0], eid, &v) == 0)
            printf("   exposure=%d", v);
        printf("\n");
    }

    printf("capturing %ux%u", cap.dev[0].width, cap.dev[0].height);
    if (fps) printf(" @ %u fps", fps);
    printf(" from %d cameras for %.0f s   (Ctrl-C to stop early)\n",
           ncams, seconds);

    g_cap = &cap;
    atexit(cleanup_capture);
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP,  on_signal);
    cpu0  = cpu_seconds();
    wall0 = (double)mono_ns() / 1e9;
    t_end = mono_ns() + (int64_t)(seconds * 1e9);

    {
        int64_t t_check = mono_ns() + 5000000000LL;
        int checked = 0;
        while (!stop_flag && mono_ns() < t_end) {
            rc = v4l2cap_next(&cap, &fr, 500);
            if (rc < 0) { fprintf(stderr, "capture error\n"); break; }
            if (rc == 0) continue;

            if (!checked && mono_ns() > t_check) {
                uint64_t hi = 0, lo = (uint64_t)-1;
                int bad = -1, k;
                checked = 1;
                for (k = 0; k < ncams; k++) {
                    uint64_t f = cap.dev[k].frames;
                    if (f > hi) hi = f;
                    if (f < lo) { lo = f; bad = k; }
                }
                if (hi && lo * 2 < hi) {
                    fprintf(stderr,
                        "\nHARDWARE FAULT: cam%d delivered %llu frames while "
                        "the fastest delivered %llu.\n"
                        "This is a capture problem, not an alignment one. "
                        "Check:\n"
                        "  sudo dmesg | grep -iE 'uncorr_err|csi5|err_rec'\n"
                        "Recover stuck channels with:  ./tempo_sync --reset\n"
                        "If that fails, power-cycle the board (a warm reboot "
                        "does not drop the sensor rail).\n\n",
                        bad, (unsigned long long)lo, (unsigned long long)hi);
                    v4l2cap_release(&cap, &fr);
                    break;
                }
            }

            feed(fr.cam, fr.t_ns, ncams);
            frames++;
            if (csv) fprintf(csv, "%d,%.9f\n", fr.cam,
                             (double)fr.t_ns / 1e9);

            v4l2cap_release(&cap, &fr);
        }
    }

    v4l2cap_stop(&cap);
    report(ncams, (double)mono_ns() / 1e9 - wall0,
           cpu_seconds() - cpu0, frames, &cap);
    if (csv) { fclose(csv); printf("  wrote %s\n", csv_path); }
    cleanup_capture();
    return 0;
}

int main(int argc, char **argv)
{
    int cams[MAXCAM] = {0, 1, 2, 3};
    int ncams = 4;
    uint32_t w = 640, h = 480, fps = 100;
    double seconds = 60.0;
    const char *csv_path = NULL, *replay = NULL;
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--cams") && i + 1 < argc) {
            char *s = argv[++i], *tok;
            ncams = 0;
            for (tok = strtok(s, ","); tok && ncams < MAXCAM;
                 tok = strtok(NULL, ","))
                cams[ncams++] = atoi(tok);
        } else if (!strcmp(argv[i], "--size") && i + 1 < argc) {
            sscanf(argv[++i], "%ux%u", &w, &h);
        } else if (!strcmp(argv[i], "--fps") && i + 1 < argc) {
            fps = (uint32_t)atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) {
            seconds = atof(argv[++i]);
        } else if (!strcmp(argv[i], "--csv") && i + 1 < argc) {
            csv_path = argv[++i];
        } else if (!strcmp(argv[i], "--replay") && i + 1 < argc) {
            replay = argv[++i];
        } else if (!strcmp(argv[i], "--reset")) {
            printf("releasing capture channels ...\n");
            v4l2cap_reset(cams, ncams);
            printf("done. retry capture now; if it still fails, "
                   "power-cycle the board.\n");
            return 0;
        } else if (!strcmp(argv[i], "--help")) {
            printf("usage: %s [--cams 0,1,2,3] [--size WxH] [--fps N]\n"
                   "          [--seconds N] [--csv out.csv]\n"
                   "          [--replay frames.csv] [--reset]\n"
                   "\n  --reset   release stuck V4L2 capture channels\n",
                   argv[0]);
            return 0;
        }
    }

    printf("tempo_sync -- multi-camera temporal alignment\n");
    printf("no GStreamer, no DeepStream, no libv4l2: V4L2 ioctls only\n\n");

    if (replay) return run_replay(replay, fps);
    if (ncams < 2) { fprintf(stderr, "need >= 2 cameras\n"); return 1; }
    return run_live(cams, ncams, w, h, fps, seconds, csv_path);
}
