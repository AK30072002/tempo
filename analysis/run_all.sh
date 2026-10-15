#!/usr/bin/env bash
set -e
cd "$(dirname "$0")"
: "${ESL_DATA:?set ESL_DATA to the unzipped data folder}"
cc -O2 -I../src -o led_select led_select.c ../src/tempo.c
echo "== Fig 2a, Sec III/VI: pairwise drift and time-to-violation";   python3 drift_pairs.py
echo "== Table II cols 1-2: 12 h study, all pairs";                  python3 drift_12h.py
echo "== Sec III-A: rho range over all data sets";                    python3 sessions.py
echo "== Fig 2b, Sec III-A: rate study (118 runs)";                    python3 rate_study.py
echo "== Sec VI: bound check, NEAREST over T";                         python3 bound_check.py
echo "== Table IV: kernel vs optical spread (Orin LED)";               python3 led_optical.py
echo "== Sec VI-A: row-corrected optical spread";                      python3 led_rowcorr.py
echo "== Fig 2c: line time and channel latency";                       python3 rowfit.py && python3 linetime_fit.py
echo "== Sec VI-A: x86 USB LED run";                                   python3 x86_led.py "$ESL_DATA/09_x86_led/X1.csv"
echo "== Table II: matched-rate sync (30 Hz sync runs)";               python3 rho.py "$ESL_DATA"/04_sync_throughput/session_*/data/thrC_s30_100fps.csv
echo "== Fig 2";                                                       python3 make_fig.py
echo "outputs in analysis/out/"
