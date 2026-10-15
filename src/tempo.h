#ifndef TEMPO_H
#define TEMPO_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TEMPO_VERSION_MAJOR 2
#define TEMPO_VERSION_MINOR 1

#ifndef TEMPO_MAX_CAMS
#define TEMPO_MAX_CAMS 8
#endif

#ifndef TEMPO_BOOT_GAPS
#define TEMPO_BOOT_GAPS 101
#endif

#ifndef TEMPO_REFIT_EVERY
#define TEMPO_REFIT_EVERY 64
#endif

typedef enum {
    TEMPO_OK              =  0,
    TEMPO_EMPTY           = -1,
    TEMPO_ENOTREADY       = -2,
    TEMPO_EINVAL          = -3,
    TEMPO_ENOSPACE        = -4
} tempo_status;

typedef enum {
    TEMPO_POLICY_INDEX = 0,
    TEMPO_POLICY_NEAREST = 1,
    TEMPO_POLICY_PREDICTED = 2
} tempo_policy;

typedef struct {
    int64_t  *t;
    uint32_t *k;
    uint16_t  cap;
    uint16_t  len;
    uint16_t  head;

    uint32_t  next_k;
    int64_t   nominal_ns;

    int64_t   beta_scaled;
    int64_t   alpha_ns;
    uint32_t  k_ref;
    uint8_t   ready;

    int64_t   boot_gap[TEMPO_BOOT_GAPS];
    uint8_t   boot_n;
    uint8_t   booted;

    uint32_t  count;
    uint32_t  dropouts;
    uint32_t  since_fit;
    int64_t   resid_ns;
    uint8_t   trusted;
} tempo_clock;

#define TEMPO_BETA_SCALE (1LL << 12)

typedef struct {
    int64_t  target_ns;
    int64_t  capture_ns[TEMPO_MAX_CAMS];
    int64_t  arrival_ns[TEMPO_MAX_CAMS];
    uint16_t slot[TEMPO_MAX_CAMS];
    int64_t  spread_ns;
    int64_t  uncertainty_ns;
    int64_t  bound_ns;
    uint8_t  ncams;
    uint8_t  complete;
} tempo_batch;

typedef struct {
    tempo_clock *clocks;
    uint8_t      ncams;
    uint8_t      ref;
    tempo_policy policy;

    int64_t      nominal_ns;
    int64_t      latency_ns[TEMPO_MAX_CAMS];
    uint8_t      latency_set;

    int64_t      newest_ns;
    int64_t      next_target_ns;
    uint8_t      have_target;
    uint8_t      lag_periods;

    uint32_t     index_cursor[TEMPO_MAX_CAMS];
    uint32_t     emitted;
    int64_t      unc_accum_ns;
} tempo_aligner;

typedef struct {
    int64_t  period_ns;
    int64_t  bound_ns;
    int32_t  ppm_x1000[TEMPO_MAX_CAMS];
    int64_t  resid_ns[TEMPO_MAX_CAMS];
    int64_t  latency_ns[TEMPO_MAX_CAMS];
    uint32_t frames[TEMPO_MAX_CAMS];
    uint32_t dropouts[TEMPO_MAX_CAMS];
    uint32_t batches;
    int64_t  mean_uncertainty_ns;
    uint8_t  ncams;
    uint8_t  latency_calibrated;
} tempo_report;

tempo_status tempo_init(tempo_aligner *a, tempo_clock *clocks,
                        uint8_t ncams, int64_t nominal_period_ns);

tempo_status tempo_clock_attach(tempo_aligner *a, uint8_t cam,
                                int64_t *tbuf, uint32_t *kbuf, uint16_t cap);

void tempo_set_policy(tempo_aligner *a, tempo_policy p);

void tempo_set_lag(tempo_aligner *a, uint8_t periods);

tempo_status tempo_set_latency(tempo_aligner *a, const int64_t *ns);

tempo_status tempo_push(tempo_aligner *a, uint8_t cam, int64_t t_ns);

tempo_status tempo_next(tempo_aligner *a, tempo_batch *out);

void tempo_get_report(const tempo_aligner *a, tempo_report *out);

int tempo_ready(const tempo_aligner *a);

int tempo_trusted_count(const tempo_aligner *a);

int64_t tempo_period_ns(const tempo_aligner *a);

int64_t tempo_bound_ns(const tempo_aligner *a);

#ifdef __cplusplus
}
#endif
#endif
