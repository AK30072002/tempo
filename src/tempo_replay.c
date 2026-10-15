#include "tempo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXCAM  TEMPO_MAX_CAMS
#define MAXEV   8000000

typedef struct { uint8_t cam; int64_t t; } ev_t;
static ev_t *evs;
static int nev;

static int64_t  *tbuf[MAXCAM];
static uint32_t *kbuf[MAXCAM];
static tempo_clock clocks[MAXCAM];
static tempo_aligner al;

static int cmp_ev(const void *a, const void *b) {
    int64_t d = ((const ev_t*)a)->t - ((const ev_t*)b)->t;
    return d < 0 ? -1 : (d > 0 ? 1 : 0);
}

static int64_t pct(const int64_t *v, int n, double p) {
    int i = (int)(p * (n - 1));
    if (i < 0) i = 0;
    if (i >= n) i = n - 1;
    return v[i];
}
static int cmp_i64(const void *a, const void *b) {
    int64_t d = *(const int64_t*)a - *(const int64_t*)b;
    return d < 0 ? -1 : (d > 0 ? 1 : 0);
}

static int load(const char *path, int *ncam_out) {
    FILE *f = fopen(path, "r");
    char line[256];
    int seen[MAXCAM]; int maxc = -1;
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return -1; }
    memset(seen, 0, sizeof(seen));
    if (!fgets(line, sizeof line, f)) { fclose(f); return -1; }
    nev = 0;
    while (fgets(line, sizeof line, f)) {
        int c; double t;
        if (sscanf(line, "%d,%lf", &c, &t) != 2) continue;
        if (c < 0 || c >= MAXCAM) continue;
        if (nev >= MAXEV) break;
        evs[nev].cam = (uint8_t)c;
        evs[nev].t = (int64_t)(t * 1e9 + 0.5);
        nev++;
        if (!seen[c]) { seen[c] = 1; if (c > maxc) maxc = c; }
    }
    fclose(f);
    qsort(evs, nev, sizeof(ev_t), cmp_ev);
    *ncam_out = maxc + 1;
    return 0;
}

typedef struct {
    const char *name;
    double mean_ms, p50_ms, p95_ms, max_ms;
    double first_ms, last_ms, growth_ms_per_min;
    long   batches;
    double bound_ms;
} result_t;

static result_t run_policy(const char *name, tempo_policy pol,
                           int ncam, int64_t nominal_ns, int win,
                           const int64_t *lat)
{
    result_t r; memset(&r, 0, sizeof r);
    r.name = name;
    tempo_init(&al, clocks, (uint8_t)ncam, nominal_ns);
    for (int c = 0; c < ncam; c++)
        tempo_clock_attach(&al, (uint8_t)c, tbuf[c], kbuf[c], (uint16_t)win);
    tempo_set_policy(&al, pol);
    if (lat) tempo_set_latency(&al, lat);

    static int64_t *sp; static int nsp;
    if (!sp) sp = malloc(sizeof(int64_t) * 4000000);
    nsp = 0;

    tempo_batch b;
    int64_t t_first = 0, t_last = 0;
    for (int i = 0; i < nev; i++) {
        tempo_push(&al, evs[i].cam, evs[i].t);
        while (tempo_next(&al, &b) == TEMPO_OK) {
            if (nsp < 4000000) sp[nsp++] = b.spread_ns;
            if (nsp == 1) t_first = b.target_ns;
            t_last = b.target_ns;
        }
    }
    if (nsp == 0) return r;

    int64_t acc = 0;
    for (int i = 0; i < nsp; i++) acc += sp[i];
    r.mean_ms = (double)acc / nsp / 1e6;
    r.batches = nsp;

    int w = nsp / 10; if (w < 1) w = 1;
    int64_t a1 = 0, a2 = 0;
    for (int i = 0; i < w; i++) a1 += sp[i];
    for (int i = nsp - w; i < nsp; i++) a2 += sp[i];
    r.first_ms = (double)a1 / w / 1e6;
    r.last_ms  = (double)a2 / w / 1e6;
    double mins = (double)(t_last - t_first) / 6e10;
    r.growth_ms_per_min = mins > 0 ? (r.last_ms - r.first_ms) / mins : 0;

    int64_t *srt = malloc(sizeof(int64_t) * nsp);
    memcpy(srt, sp, sizeof(int64_t) * nsp);
    qsort(srt, nsp, sizeof(int64_t), cmp_i64);
    r.p50_ms = (double)pct(srt, nsp, 0.50) / 1e6;
    r.p95_ms = (double)pct(srt, nsp, 0.95) / 1e6;
    r.max_ms = (double)srt[nsp-1] / 1e6;
    free(srt);

    r.bound_ms = (double)tempo_bound_ns(&al) / 1e6;
    return r;
}

int main(int argc, char **argv) {
    double fps = 100.0;
    int win = 512;
    const char *path = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--fps") && i+1 < argc) fps = atof(argv[++i]);
        else if (!strcmp(argv[i], "--window") && i+1 < argc) win = atoi(argv[++i]);
        else path = argv[i];
    }
    if (!path) {
        fprintf(stderr, "usage: %s frames.csv [--fps 100] [--window 512]\n",
                argv[0]);
        return 2;
    }

    evs = malloc(sizeof(ev_t) * MAXEV);
    if (!evs) { fprintf(stderr, "oom\n"); return 1; }

    int ncam = 0;
    if (load(path, &ncam) < 0) return 1;
    if (ncam < 2) { fprintf(stderr, "need >= 2 cameras, found %d\n", ncam);
                    return 1; }

    for (int c = 0; c < ncam; c++) {
        tbuf[c] = malloc(sizeof(int64_t) * win);
        kbuf[c] = malloc(sizeof(uint32_t) * win);
    }

    int64_t nominal = (int64_t)(1e9 / fps + 0.5);
    double dur = nev ? (double)(evs[nev-1].t - evs[0].t)/1e9 : 0;

    printf("file      : %s\n", path);
    printf("events    : %d over %.1f s   cameras: %d\n", nev, dur, ncam);
    printf("nominal   : %.2f fps (%lld ns)   window: %d\n\n",
           fps, (long long)nominal, win);

    result_t idx  = run_policy("INDEX (naive)",      TEMPO_POLICY_INDEX,
                               ncam, nominal, win, NULL);
    result_t near = run_policy("NEAREST (ApproxTime)", TEMPO_POLICY_NEAREST,
                               ncam, nominal, win, NULL);
    result_t pred = run_policy("PREDICTED (tempo)",  TEMPO_POLICY_PREDICTED,
                               ncam, nominal, win, NULL);

    tempo_report rep; tempo_get_report(&al, &rep);

    printf("MEASURED CLOCKS\n");
    printf("  %-5s %12s %12s %10s %10s\n",
           "cam", "ppm", "resid_us", "frames", "dropouts");
    for (int c = 0; c < ncam; c++)
        printf("  %-5d %8d.%03d %12lld %10u %10u\n", c,
               rep.ppm_x1000[c]/1000, abs(rep.ppm_x1000[c]%1000),
               (long long)(rep.resid_ns[c]/1000),
               rep.frames[c], rep.dropouts[c]);
    {
        int bad = 0;
        for (int c = 0; c < ncam; c++)
            if (rep.frames[c] && rep.dropouts[c] > rep.frames[c] / 2) bad = 1;
        if (bad) printf("  WARNING: dropouts approach frame count -- period "
                        "estimate is not trustworthy\n");
    }
    printf("  measured period %.4f ms (%.3f fps)   bound %.4f ms\n\n",
           (double)rep.period_ns/1e6, 1e9/(double)rep.period_ns,
           (double)rep.bound_ns/1e6);

    printf("POLICY COMPARISON  (alignment spread, ms)\n");
    printf("  %-22s %9s %9s %9s %9s %14s\n",
           "policy", "mean", "p50", "p95", "max", "growth/min");
    result_t *all[3] = { &idx, &near, &pred };
    for (int i = 0; i < 3; i++) {
        result_t *r = all[i];
        printf("  %-22s %9.4f %9.4f %9.4f %9.4f %14.5f\n",
               r->name, r->mean_ms, r->p50_ms, r->p95_ms, r->max_ms,
               r->growth_ms_per_min);
    }

    printf("\n  bound (slowest camera's period) : %.4f ms\n", pred.bound_ms);
    printf("  INDEX  first decile %.3f -> last decile %.3f ms\n",
           idx.first_ms, idx.last_ms);
    printf("  tempo  first decile %.3f -> last decile %.3f ms\n",
           pred.first_ms, pred.last_ms);

    printf("\nVERDICT\n");
    if (idx.max_ms > pred.bound_ms && pred.max_ms <= pred.bound_ms * 1.01) {
        printf("  INDEX exceeded the frame period (%.3f ms max) while tempo\n",
               idx.max_ms);
        printf("  stayed within bound (%.3f ms max). Improvement %.1fx.\n",
               pred.max_ms, idx.max_ms / (pred.max_ms > 0 ? pred.max_ms : 1));
    } else if (idx.max_ms <= pred.bound_ms) {
        printf("  INDEX has not yet exceeded the bound in this recording.\n");
        printf("  At %.4f ms/min it needs %.0f min to do so -- capture longer\n",
               idx.growth_ms_per_min,
               idx.growth_ms_per_min > 0
                   ? pred.bound_ms / idx.growth_ms_per_min : 0.0);
        printf("  to show the difference.\n");
    }
    return 0;
}
