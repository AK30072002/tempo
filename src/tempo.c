#include "tempo.h"

static int64_t i64_abs(int64_t v) { return v < 0 ? -v : v; }

static int64_t i64_sqrt(int64_t v)
{
    int64_t x, y;
    if (v <= 0) return 0;
    x = v;
    y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + v / x) / 2; }
    return x;
}

static int64_t div_round(int64_t num, int64_t den)
{
    if (den == 0) return 0;
    if ((num < 0) != (den < 0))
        return (num - den / 2) / den;
    return (num + den / 2) / den;
}

static uint16_t slot_at(const tempo_clock *c, uint16_t i)
{
    uint32_t p = (uint32_t)c->head + i;
    if (p >= c->cap) p -= c->cap;
    return (uint16_t)p;
}

static int64_t clock_period_ns(const tempo_clock *c)
{
    if (!c->ready) return c->nominal_ns;
    return c->beta_scaled / TEMPO_BETA_SCALE;
}

static void clock_refit(tempo_clock *c)
{
    int64_t n, sk, st, skk, skt;
    int64_t k0, t0, num, den, q, r;
    uint16_t i;

    if (c->len < 8) return;

    k0 = (int64_t)c->k[slot_at(c, 0)];
    t0 = c->t[slot_at(c, 0)];

    n = (int64_t)c->len;
    sk = st = skk = skt = 0;
    for (i = 0; i < c->len; i++) {
        uint16_t p = slot_at(c, i);
        int64_t kk = (int64_t)c->k[p] - k0;
        int64_t tt = c->t[p] - t0;
        sk  += kk;
        st  += tt;
        skk += kk * kk;
        skt += kk * tt;
    }

    den = n * skk - sk * sk;
    if (den == 0) return;
    num = n * skt - sk * st;

    q = num / den;
    r = num - q * den;
    c->beta_scaled = q * TEMPO_BETA_SCALE
                   + (r * TEMPO_BETA_SCALE) / den;

    {
        int64_t b = c->beta_scaled / TEMPO_BETA_SCALE;
        if (b <= 0 || b < c->nominal_ns / 3 || b > c->nominal_ns * 3) {
            if (b > 0 && c->resid_ns > 0 && c->resid_ns * 4 < b) {
                c->nominal_ns = b;
            } else {
                c->beta_scaled = c->nominal_ns * TEMPO_BETA_SCALE;
                return;
            }
        }
    }

    {
        int64_t num = st * TEMPO_BETA_SCALE - c->beta_scaled * sk;
        c->k_ref    = (uint32_t)k0;
        c->alpha_ns = t0 + div_round(num, n * TEMPO_BETA_SCALE);
    }

    {
        int64_t acc = 0;
        uint16_t step = (c->len > 128) ? (uint16_t)(c->len / 128) : 1;
        uint16_t cnt = 0;
        for (i = 0; i < c->len; i += step) {
            uint16_t p = slot_at(c, i);
            int64_t pred = c->alpha_ns
                         + (c->beta_scaled
                            * (int64_t)(int32_t)(c->k[p] - c->k_ref))
                           / TEMPO_BETA_SCALE;
            int64_t e = c->t[p] - pred;
            int64_t lim = (c->beta_scaled / TEMPO_BETA_SCALE) * 2;
            if (lim < 1000000) lim = 1000000;
            if (e > lim) e = lim;
            if (e < -lim) e = -lim;
            acc += e * e;
            cnt++;
        }
        c->resid_ns = cnt ? i64_sqrt(acc / cnt) : 0;
    }

    {
        int64_t p = c->beta_scaled / TEMPO_BETA_SCALE;
        c->trusted = (p > 0 && c->resid_ns * 8 < p) ? 1 : 0;
    }

    c->ready = 1;
}

static int64_t boot_period(tempo_clock *c)
{
    int64_t v[TEMPO_BOOT_GAPS];
    int64_t acc = 0;
    uint8_t i, j, n = c->boot_n, cnt = 0;

    if (!n) return 0;
    for (i = 0; i < n; i++) v[i] = c->boot_gap[i];
    for (i = 1; i < n; i++) {
        int64_t key = v[i];
        j = i;
        while (j > 0 && v[j - 1] > key) { v[j] = v[j - 1]; j--; }
        v[j] = key;
    }
    if (n < 8) return v[n / 2];

    {
        int64_t med = v[n / 2];
        int64_t lo_b = med - med / 5;
        int64_t hi_b = med + med / 5;
        for (i = 0; i < n; i++)
            if (v[i] >= lo_b && v[i] <= hi_b) { acc += v[i]; cnt++; }
    }
    return cnt ? acc / cnt : v[n / 2];
}

static void clock_push(tempo_clock *c, int64_t t_ns)
{
    int64_t period, gap;
    uint32_t adv = 1;
    uint16_t p;

    c->count++;

    if (c->len > 0) {
        uint16_t last = slot_at(c, (uint16_t)(c->len - 1));
        gap = t_ns - c->t[last];

        if (!c->booted) {
            if (gap > 0 && c->boot_n < TEMPO_BOOT_GAPS)
                c->boot_gap[c->boot_n++] = gap;
            if (c->boot_n >= TEMPO_BOOT_GAPS) {
                int64_t m = boot_period(c);
                if (m > 0) {
                    c->nominal_ns  = m;
                    c->beta_scaled = m * TEMPO_BETA_SCALE;
                }
                c->booted = 1;
            }
            adv = 1;
        } else {
            period = clock_period_ns(c);
            if (period <= 0) period = c->nominal_ns;

            if (gap > 0) {
                int64_t n = div_round(gap, period);
                if (n < 1) n = 1;
                if (n > 1000) n = 1000;
                adv = (uint32_t)n;
                if (adv > 1) c->dropouts += adv - 1;
            }
        }
    }

    c->next_k += adv;

    if (c->len == c->cap) {
        c->head = slot_at(c, 1);
        c->len--;
    }
    p = slot_at(c, c->len);
    c->t[p] = t_ns;
    c->k[p] = c->next_k;
    c->len++;

    if (!c->booted) return;
    if (++c->since_fit >= TEMPO_REFIT_EVERY || !c->ready) {
        c->since_fit = 0;
        clock_refit(c);
    }
}

static int32_t clock_nearest(const tempo_clock *c, int64_t target,
                             int64_t *out_t);

static int64_t clock_predict(const tempo_clock *c, uint16_t logical_slot)
{
    uint16_t p = slot_at(c, logical_slot);
    if (!c->ready) return c->t[p];
    return c->alpha_ns
         + (c->beta_scaled * (int64_t)(int32_t)(c->k[p] - c->k_ref))
           / TEMPO_BETA_SCALE;
}

static int32_t clock_nearest_pred(const tempo_clock *c, int64_t target)
{
    int32_t best = -1;
    int64_t bd = INT64_MAX;
    uint16_t i;
    if (c->len == 0) return -1;
    if (!c->ready) { int64_t tv; return clock_nearest(c, target, &tv); }
    {
        int64_t tv;
        int32_t s0 = clock_nearest(c, target, &tv);
        int32_t lo = s0 - 2, hi = s0 + 2;
        if (lo < 0) lo = 0;
        if (hi >= (int32_t)c->len) hi = (int32_t)c->len - 1;
        for (i = (uint16_t)lo; (int32_t)i <= hi; i++) {
            int64_t d = clock_predict(c, i) - target;
            if (d < 0) d = -d;
            if (d < bd) { bd = d; best = (int32_t)i; }
        }
    }
    return best;
}

static int32_t clock_nearest(const tempo_clock *c, int64_t target,
                             int64_t *out_t)
{
    uint16_t lo = 0, hi, mid;
    int32_t best;
    int64_t bd, d;
    uint16_t i;

    if (c->len == 0) return -1;
    hi = (uint16_t)(c->len - 1);
    while (lo < hi) {
        mid = (uint16_t)(lo + (hi - lo) / 2);
        if (c->t[slot_at(c, mid)] < target) lo = (uint16_t)(mid + 1);
        else hi = mid;
    }

    best = (int32_t)lo;
    bd = i64_abs(c->t[slot_at(c, lo)] - target);
    if (lo > 0) {
        i = (uint16_t)(lo - 1);
        d = i64_abs(c->t[slot_at(c, i)] - target);
        if (d < bd) { bd = d; best = (int32_t)i; }
    }
    if (lo + 1 < c->len) {
        i = (uint16_t)(lo + 1);
        d = i64_abs(c->t[slot_at(c, i)] - target);
        if (d < bd) { bd = d; best = (int32_t)i; }
    }
    if (out_t) *out_t = c->t[slot_at(c, (uint16_t)best)];
    return best;
}

tempo_status tempo_init(tempo_aligner *a, tempo_clock *clocks,
                        uint8_t ncams, int64_t nominal_period_ns)
{
    uint8_t i;
    size_t z;
    if (!a || !clocks || ncams < 2 || ncams > TEMPO_MAX_CAMS)
        return TEMPO_EINVAL;
    if (nominal_period_ns <= 0) return TEMPO_EINVAL;

    for (z = 0; z < sizeof(*a); z++) ((uint8_t *)a)[z] = 0;

    a->clocks     = clocks;
    a->ncams      = ncams;
    a->ref        = 0;
    a->policy     = TEMPO_POLICY_PREDICTED;
    a->nominal_ns = nominal_period_ns;
    a->lag_periods = 3;
    a->newest_ns  = INT64_MIN;

    for (i = 0; i < ncams; i++) {
        for (z = 0; z < sizeof(tempo_clock); z++)
            ((uint8_t *)&clocks[i])[z] = 0;
        clocks[i].nominal_ns  = nominal_period_ns;
        clocks[i].beta_scaled = nominal_period_ns * TEMPO_BETA_SCALE;
    }
    return TEMPO_OK;
}

tempo_status tempo_clock_attach(tempo_aligner *a, uint8_t cam,
                                int64_t *tbuf, uint32_t *kbuf, uint16_t cap)
{
    if (!a || cam >= a->ncams || !tbuf || !kbuf || cap < 16)
        return TEMPO_EINVAL;
    a->clocks[cam].t   = tbuf;
    a->clocks[cam].k   = kbuf;
    a->clocks[cam].cap = cap;
    a->clocks[cam].len = 0;
    a->clocks[cam].head = 0;
    return TEMPO_OK;
}

void tempo_set_policy(tempo_aligner *a, tempo_policy p)
{
    if (a) a->policy = p;
}

void tempo_set_lag(tempo_aligner *a, uint8_t periods)
{
    if (a && periods >= 1) a->lag_periods = periods;
}

tempo_status tempo_set_latency(tempo_aligner *a, const int64_t *ns)
{
    uint8_t i;
    if (!a || !ns) return TEMPO_EINVAL;
    for (i = 0; i < a->ncams; i++) a->latency_ns[i] = ns[i];
    a->latency_set = 1;
    return TEMPO_OK;
}

tempo_status tempo_push(tempo_aligner *a, uint8_t cam, int64_t t_ns)
{
    if (!a || cam >= a->ncams) return TEMPO_EINVAL;
    if (!a->clocks[cam].t) return TEMPO_EINVAL;
    clock_push(&a->clocks[cam], t_ns);
    if (t_ns > a->newest_ns) a->newest_ns = t_ns;
    return TEMPO_OK;
}

int64_t tempo_period_ns(const tempo_aligner *a)
{
    int64_t acc = 0;
    uint8_t i, n = 0;
    if (!a) return 0;
    for (i = 0; i < a->ncams; i++) {
        if (a->clocks[i].ready) { acc += clock_period_ns(&a->clocks[i]); n++; }
    }
    return n ? acc / n : a->nominal_ns;
}

int64_t tempo_bound_ns(const tempo_aligner *a)
{
    int64_t mx = 0;
    uint8_t i;
    if (!a) return 0;
    for (i = 0; i < a->ncams; i++) {
        int64_t p = a->clocks[i].ready ? clock_period_ns(&a->clocks[i])
                                       : a->nominal_ns;
        if (p > mx) mx = p;
    }
    return mx ? mx : a->nominal_ns;
}

int tempo_ready(const tempo_aligner *a)
{
    uint8_t i;
    if (!a) return 0;
    for (i = 0; i < a->ncams; i++)
        if (!a->clocks[i].ready || !a->clocks[i].booted) return 0;
    return 1;
}

int tempo_trusted_count(const tempo_aligner *a)
{
    uint8_t i; int n = 0;
    if (!a) return 0;
    for (i = 0; i < a->ncams; i++) if (a->clocks[i].trusted) n++;
    return n;
}

static uint8_t all_ready(const tempo_aligner *a)
{
    uint8_t i;
    for (i = 0; i < a->ncams; i++)
        if (!a->clocks[i].ready) return 0;
    return 1;
}

tempo_status tempo_next(tempo_aligner *a, tempo_batch *out)
{
    int64_t period, bound, horizon, target;
    int64_t lo_v, hi_v, resid_max;
    uint8_t i;

    if (!a || !out) return TEMPO_EINVAL;
    if (!all_ready(a)) return TEMPO_ENOTREADY;

    period = tempo_period_ns(a);
    bound  = tempo_bound_ns(a);
    if (period <= 0) return TEMPO_ENOTREADY;

    if (!a->have_target) {
        int64_t start = INT64_MIN, tv;
        for (i = 0; i < a->ncams; i++) {
            if (clock_nearest(&a->clocks[i],
                              a->newest_ns - (int64_t)a->lag_periods * period,
                              &tv) < 0)
                return TEMPO_EMPTY;
            if (tv > start) start = tv;
        }
        a->next_target_ns = start;
        a->have_target = 1;
    }

    horizon = a->newest_ns - (int64_t)a->lag_periods * period;
    if (a->next_target_ns > horizon) return TEMPO_EMPTY;

    target = a->next_target_ns;
    a->next_target_ns = target + period;

    out->target_ns = target;
    out->ncams     = a->ncams;
    out->bound_ns  = bound;
    out->complete  = 1;

    lo_v = INT64_MAX;
    hi_v = INT64_MIN;
    resid_max = 0;

    for (i = 0; i < a->ncams; i++) {
        tempo_clock *c = &a->clocks[i];
        int64_t arrival = 0, est, lat, raw = 0;
        int32_t s;

        if (a->policy == TEMPO_POLICY_INDEX) {
            uint32_t want = a->index_cursor[i];
            if (want >= c->count) { out->complete = 0; return TEMPO_EMPTY; }
            if (c->count - want > c->len) {
                a->index_cursor[i] = c->count - c->len;
                want = a->index_cursor[i];
            }
            s = (int32_t)(c->len - (c->count - want));
            if (s < 0 || s >= (int32_t)c->len) { out->complete = 0;
                                                 return TEMPO_EMPTY; }
            arrival = c->t[slot_at(c, (uint16_t)s)];
            raw = arrival;
        } else if (a->policy == TEMPO_POLICY_PREDICTED) {
            lat = a->latency_ns[i];
            if (c->trusted) {
                s = clock_nearest_pred(c, target + lat);
                if (s < 0) { out->complete = 0; return TEMPO_EMPTY; }
                arrival = clock_predict(c, (uint16_t)s);
                raw = c->t[slot_at(c, (uint16_t)s)];
            } else {
                s = clock_nearest(c, target + lat, &arrival);
                if (s < 0) { out->complete = 0; return TEMPO_EMPTY; }
                raw = arrival;
            }
        } else {
            lat = 0;
            s = clock_nearest(c, target, &arrival);
            if (s < 0) { out->complete = 0; return TEMPO_EMPTY; }
            raw = arrival;
        }

        lat = (a->policy == TEMPO_POLICY_PREDICTED) ? a->latency_ns[i] : 0;
        est = arrival - lat;

        out->capture_ns[i] = est;
        out->arrival_ns[i] = raw;
        out->slot[i] = (uint16_t)(s < 0 ? 0 : s);
        if (est < lo_v) lo_v = est;
        if (est > hi_v) hi_v = est;
        if (c->resid_ns > resid_max) resid_max = c->resid_ns;
    }

    if (a->policy == TEMPO_POLICY_INDEX)
        for (i = 0; i < a->ncams; i++) a->index_cursor[i]++;

    out->spread_ns = hi_v - lo_v;

    {
        int64_t s2 = out->spread_ns;
        int64_t r2 = resid_max;
        if (s2 > 2000000000LL) s2 = 2000000000LL;
        if (r2 > 2000000000LL) r2 = 2000000000LL;
        out->uncertainty_ns = i64_sqrt(s2 * s2 + r2 * r2);
    }

    a->emitted++;
    a->unc_accum_ns += out->uncertainty_ns;
    return TEMPO_OK;
}

void tempo_get_report(const tempo_aligner *a, tempo_report *out)
{
    int64_t ref_p;
    uint8_t i;
    size_t z;
    if (!a || !out) return;

    for (z = 0; z < sizeof(*out); z++) ((uint8_t *)out)[z] = 0;

    out->ncams     = a->ncams;
    out->period_ns = tempo_period_ns(a);
    out->bound_ns  = tempo_bound_ns(a);
    out->batches   = a->emitted;
    out->latency_calibrated = a->latency_set;
    out->mean_uncertainty_ns =
        a->emitted ? a->unc_accum_ns / (int64_t)a->emitted : 0;

    ref_p = a->clocks[a->ref].ready ? clock_period_ns(&a->clocks[a->ref])
                                    : a->nominal_ns;
    if (ref_p <= 0) ref_p = a->nominal_ns;

    for (i = 0; i < a->ncams; i++) {
        const tempo_clock *c = &a->clocks[i];
        int64_t p = c->ready ? clock_period_ns(c) : a->nominal_ns;
        int64_t d = p - ref_p;
        out->ppm_x1000[i] = (int32_t)((d * 1000000000LL) / ref_p);
        out->resid_ns[i]  = c->resid_ns;
        out->latency_ns[i] = a->latency_ns[i];
        out->frames[i]    = c->count;
        out->dropouts[i]  = c->dropouts;
    }
}
