#include "tempo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NCAM 4
#define WIN  512

static int64_t  tbuf[NCAM][WIN];
static uint32_t kbuf[NCAM][WIN];
static tempo_clock clocks[NCAM];
static tempo_aligner al;

typedef struct { uint8_t cam; int64_t arr; int64_t truth; } ev_t;
static ev_t *evs; static int nev;

static uint32_t rs;
static uint32_t rnd(void){ rs^=rs<<13; rs^=rs>>17; rs^=rs<<5; return rs; }
static int64_t jit(int64_t span){
    if (span<=0) return 0;
    int64_t a = (int64_t)(rnd()%(uint32_t)(2*span+1)) - span;
    int64_t b = (int64_t)(rnd()%(uint32_t)(2*span+1)) - span;
    return (a+b)/2;
}
static int cmp_ev(const void*a,const void*b){
    int64_t d=((const ev_t*)a)->arr-((const ev_t*)b)->arr;
    return d<0?-1:(d>0?1:0); }

static void gen(const int32_t *ppm, const int64_t *lat, int64_t T,
                int64_t dur, int64_t jitter_ns, uint32_t seed)
{
    rs = seed; nev = 0;
    for (int c=0;c<NCAM;c++){
        int64_t per = T + (T*ppm[c])/1000000;
        for (int64_t k=0; per*k<dur; k++){
            int64_t truth = per*k;
            evs[nev].cam=(uint8_t)c;
            evs[nev].truth=truth;
            evs[nev].arr = truth + lat[c] + jit(jitter_ns);
            nev++;
        }
    }
    qsort(evs,nev,sizeof(ev_t),cmp_ev);
}

typedef struct { int64_t arr, truth; } pair_t;
static pair_t *hist[NCAM]; static int hn[NCAM];

static int64_t truth_of(int cam, int64_t arr){
    int lo=0, hi=hn[cam]-1;
    while(lo<hi){ int mid=(lo+hi)/2;
        if(hist[cam][mid].arr < arr) lo=mid+1; else hi=mid; }
    if (hist[cam][lo].arr==arr) return hist[cam][lo].truth;
    int best=lo; int64_t bd=llabs(hist[cam][lo].arr-arr);
    if(lo>0 && llabs(hist[cam][lo-1].arr-arr)<bd){best=lo-1;}
    return hist[cam][best].truth;
}

typedef struct { double mean,p95,max; long n; } res_t;
static int cmp64(const void*a,const void*b){
    int64_t d=*(const int64_t*)a-*(const int64_t*)b; return d<0?-1:(d>0?1:0); }

static res_t run(tempo_policy pol, int64_t T, const int64_t *lat, int use_lat)
{
    res_t r; memset(&r,0,sizeof r);
    tempo_init(&al, clocks, NCAM, T);
    for(int c=0;c<NCAM;c++) tempo_clock_attach(&al,(uint8_t)c,tbuf[c],kbuf[c],WIN);
    tempo_set_policy(&al,pol);
    if(use_lat) tempo_set_latency(&al,lat);

    static int64_t *sp; if(!sp) sp=malloc(sizeof(int64_t)*2000000);
    int nsp=0;

    for(int c=0;c<NCAM;c++){ hn[c]=0;
        if(!hist[c]) hist[c]=malloc(sizeof(pair_t)*1000000); }

    tempo_batch b;
    for(int i=0;i<nev;i++){
        int c=evs[i].cam;
        hist[c][hn[c]].arr=evs[i].arr; hist[c][hn[c]].truth=evs[i].truth; hn[c]++;
        tempo_push(&al, evs[i].cam, evs[i].arr);
        while(tempo_next(&al,&b)==TEMPO_OK){
            int64_t lo=INT64_MAX, hi=INT64_MIN;
            for(int k=0;k<NCAM;k++){
                int64_t arr = b.arrival_ns[k];
                int64_t tv = truth_of(k, arr);
                if(tv<lo) lo=tv;
                if(tv>hi) hi=tv;
            }
            if(nsp<2000000) sp[nsp++]= hi-lo;
        }
    }
    if(!nsp) return r;
    int64_t acc=0; for(int i=0;i<nsp;i++) acc+=sp[i];
    r.mean=(double)acc/nsp/1e6; r.n=nsp;
    int64_t *s2=malloc(sizeof(int64_t)*nsp);
    memcpy(s2,sp,sizeof(int64_t)*nsp);
    qsort(s2,nsp,sizeof(int64_t),cmp64);
    r.p95=(double)s2[(int)(0.95*(nsp-1))]/1e6;
    r.max=(double)s2[nsp-1]/1e6;
    free(s2);
    return r;
}

int main(void){
    const int64_t T=10000000;
    int32_t ppm[NCAM]={0,5,-8,11};
    int64_t lat_unequal[NCAM]={0,3100000,6700000,1900000};
    int64_t lat_equal[NCAM]={0,0,0,0};
    int64_t jits[5]={0,20000,100000,500000,2000000};
    evs=malloc(sizeof(ev_t)*3000000);

    for(int scen=0; scen<2; scen++){
        const int64_t *lat = scen==0 ? lat_unequal : lat_equal;
        printf("\n%s per-camera latency {%lld,%lld,%lld,%lld} us\n",
               scen==0?"UNEQUAL":"EQUAL",
               (long long)(lat[0]/1000),(long long)(lat[1]/1000),
               (long long)(lat[2]/1000),(long long)(lat[3]/1000));
        printf("spread of TRUE capture instants, ms (mean / max), 1 h, 4 cameras, ppm {0,+5,-8,+11}\n");
        printf("%8s | %-15s | %-15s | %-15s | %-15s\n","jitter","INDEX","NEAREST",
               "PREDICTED","PRED.+lat.cal.");
        for(int j=0;j<5;j++){
            gen(ppm,lat,T,3600LL*1000000000LL,jits[j],777);
            res_t a=run(TEMPO_POLICY_INDEX,T,lat,0);
            res_t b=run(TEMPO_POLICY_NEAREST,T,lat,0);
            res_t c=run(TEMPO_POLICY_PREDICTED,T,lat,0);
            res_t d=run(TEMPO_POLICY_PREDICTED,T,lat,1);
            printf("%5lld us | %6.2f / %6.2f | %6.2f / %6.2f | %6.2f / %6.2f | %6.2f / %6.2f\n",
                   (long long)(jits[j]/1000), a.mean,a.max, b.mean,b.max,
                   c.mean,c.max, d.mean,d.max);
        }
    }
    return 0;
}
