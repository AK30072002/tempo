#include <stdarg.h>
#include "tempo.h"
#include <stdio.h>
#include <stdlib.h>

#define NCAM 4
#define WIN  512
static int64_t  tbuf[TEMPO_MAX_CAMS][WIN];
static uint32_t kbuf[TEMPO_MAX_CAMS][WIN];
static tempo_clock clocks[TEMPO_MAX_CAMS];
static tempo_aligner al;

static int fails = 0;
static void check(const char *name, int cond, const char *fmt, ...) {
    printf("  %s  %-34s ", cond ? "PASS" : "FAIL", name);
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n"); if (!cond) fails++;
}

typedef struct { uint8_t cam; int64_t t; } ev_t;
static ev_t evs[4000000];
static int nev;

static uint32_t rs = 12345;
static uint32_t rnd(void){ rs ^= rs<<13; rs ^= rs>>17; rs ^= rs<<5; return rs; }

static void gen(const int32_t *ppm, const int64_t *off, int64_t T,
                int64_t dur_ns, int drop_pct) {
    nev = 0;
    for (int c = 0; c < NCAM; c++) {
        int64_t per = T + (T * ppm[c]) / 1000000;
        for (int64_t k = 0; per*k < dur_ns; k++) {
            if (drop_pct && (int)(rnd() % 100) < drop_pct) continue;
            evs[nev].cam = (uint8_t)c;
            evs[nev].t = off[c] + per*k;
            nev++;
        }
    }
    int cmp(const void*a,const void*b){
        int64_t d=((ev_t*)a)->t-((ev_t*)b)->t; return d<0?-1:(d>0?1:0); }
    qsort(evs, nev, sizeof(ev_t), cmp);
}

static void setup(int64_t T, tempo_policy pol) {
    tempo_init(&al, clocks, NCAM, T);
    for (int c = 0; c < NCAM; c++)
        tempo_clock_attach(&al, (uint8_t)c, tbuf[c], kbuf[c], WIN);
    tempo_set_policy(&al, pol);
}

static void run(int64_t *max_spread, double *mean_spread,
                int64_t *first, int64_t *last, int nlast) {
    tempo_batch b; int64_t mx=0, acc=0; int n=0;
    static int64_t sp[400000]; int nsp=0;
    for (int i = 0; i < nev; i++) {
        tempo_push(&al, evs[i].cam, evs[i].t);
        while (tempo_next(&al, &b) == TEMPO_OK) {
            if (b.spread_ns > mx) mx = b.spread_ns;
            acc += b.spread_ns; n++;
            if (nsp < 400000) sp[nsp++] = b.spread_ns;
        }
    }
    *max_spread = mx;
    *mean_spread = n ? (double)acc/n : 0;
    if (nsp > nlast) {
        int64_t a1=0,a2=0;
        for (int i=0;i<nlast;i++) a1 += sp[i];
        for (int i=nsp-nlast;i<nsp;i++) a2 += sp[i];
        *first = a1/nlast; *last = a2/nlast;
    } else { *first=0; *last=0; }
}

int main(void) {
    const int64_t T = 10000000;
    int32_t ppm[NCAM] = { 0, 5, -8, 11 };
    int64_t off[NCAM] = { 0, 3100000, 6700000, 1900000 };
    int64_t mx, f, l; double mean;

    printf("tempo-c validation  (T = %lld ns)\n", (long long)T);

    printf("\n=== 1. clock model recovers ppm ===\n");
    gen(ppm, off, T, 600LL*1000000000LL, 0);
    setup(T, TEMPO_POLICY_PREDICTED);
    run(&mx, &mean, &f, &l, 200);
    tempo_report r; tempo_get_report(&al, &r);
    for (int c = 0; c < NCAM; c++) {
        int32_t want = ppm[c]*1000, got = r.ppm_x1000[c];
        int32_t err = got > want ? got-want : want-got;
        check("ppm", err < 100, "cam%d true %+d.%03d got %+d.%03d ppm",
              c, want/1000, abs(want%1000), got/1000, abs(got%1000));
    }

    printf("\n=== 2. INDEX policy error grows unboundedly ===\n");
    gen(ppm, off, T, 3600LL*1000000000LL, 0);
    setup(T, TEMPO_POLICY_INDEX);
    run(&mx, &mean, &f, &l, 200);
    check("index grows", l > f*3, "%lld -> %lld ns", (long long)f, (long long)l);
    check("index exceeds bound", l > T, "%lld vs bound %lld ns",
          (long long)l, (long long)T);
    int64_t idx_last = l;

    printf("\n=== 3. PREDICTED stays bounded ===\n");
    setup(T, TEMPO_POLICY_PREDICTED);
    run(&mx, &mean, &f, &l, 200);
    tempo_get_report(&al, &r);
    check("never exceeds bound", mx <= r.bound_ns + r.bound_ns/100,
          "max %lld of bound %lld ns", (long long)mx, (long long)r.bound_ns);
    check("beats index at 1h", l < idx_last/3, "%lld vs %lld ns",
          (long long)l, (long long)idx_last);
    double pred_mean = mean;

    printf("\n=== 4. supplied latency offsets are applied ===\n");
    gen(ppm, off, T, 600LL*1000000000LL, 0);
    setup(T, TEMPO_POLICY_NEAREST);
    run(&mx, &mean, &f, &l, 200);
    double uncal = mean;
    setup(T, TEMPO_POLICY_PREDICTED);
    { int64_t lat[NCAM]; for (int c=0;c<NCAM;c++) lat[c]=off[c]-off[0];
      tempo_set_latency(&al, lat); }
    run(&mx, &mean, &f, &l, 200);
    tempo_get_report(&al, &r);
    check("latency flagged in report", r.latency_calibrated, "");
    check("latency values stored", r.latency_ns[2] == off[2]-off[0],
          "cam2 %lld ns", (long long)r.latency_ns[2]);

    printf("\n=== 5. dropout robustness ===\n");
    int drops[3] = {1, 10, 40};
    for (int di = 0; di < 3; di++) { int d = drops[di];
        gen(ppm, off, T, 600LL*1000000000LL, d);
        setup(T, TEMPO_POLICY_PREDICTED);
        run(&mx, &mean, &f, &l, 200);
        tempo_get_report(&al, &r);
        int32_t worst = 0;
        for (int c = 0; c < NCAM; c++) {
            int32_t e = r.ppm_x1000[c] - ppm[c]*1000;
            if (e < 0) e = -e;
            if (e > worst) worst = e;
        }
        check("ppm under dropout", worst < 1000, "%d%% drop -> err %d.%03d ppm",
              d, worst/1000, worst%1000);
    }

    printf("\n=== 6. vs NEAREST (ApproximateTime) ===\n");
    printf("     index     %lld ns at 1 h\n", (long long)idx_last);
    printf("     nearest   %.0f ns\n", uncal);
    printf("     predicted %.0f ns\n", pred_mean);

    printf("\n=== 7. determinism ===\n");
    gen(ppm, off, T, 300LL*1000000000LL, 0);
    setup(T, TEMPO_POLICY_PREDICTED); run(&mx, &mean, &f, &l, 100);
    int64_t mx1 = mx; double m1 = mean;
    setup(T, TEMPO_POLICY_PREDICTED); run(&mx, &mean, &f, &l, 100);
    check("bit-identical repeat", mx==mx1 && mean==m1,
          "max %lld, mean %.1f", (long long)mx, mean);

    printf("\n========================================\n");
    printf("FAILURES: %d\n", fails);
    return fails ? 1 : 0;
}
