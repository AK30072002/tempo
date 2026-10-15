#!/usr/bin/env bash
set -uo pipefail

KIT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$KIT/src"
TOOLS="$KIT/tools"
OUT="${OUT:-$KIT/results_x86}"
CAMS="${CAMS:-}"
FPS="${FPS:-15}"
SIZE="${SIZE:-640x480}"
EXP="${EXP:-1}"
GAIN="${GAIN:-128}"
RUN_SECS="${RUN_SECS:-600}"
export LED_STEP_MS="${LED_STEP_MS:-1.0}"
PY="python3 -I"
PERIOD_MS=$(awk -v f="$FPS" 'BEGIN{printf "%.3f", 1000.0/f}')

mkdir -p "$OUT/env" "$OUT/led"
LOG="$OUT/RUN_LOG.txt"

log()  { echo "[$(date '+%F %T')] $*" | tee -a "$LOG"; }
say()  { echo "$*" | tee -a "$LOG"; }
bar()  { say ""; say "======================================================================"; say "  $*"; say "======================================================================"; }

detect_cams() {
    if [ -n "${CAMS:-}" ]; then return; fi
    local d n list=""
    for d in /dev/video*; do
        [ -e "$d" ] || continue
        if v4l2-ctl -d "$d" --list-formats 2>/dev/null | grep -q "YUYV"; then
            n="${d#/dev/video}"; list="$list${list:+,}$n"
        fi
    done
    CAMS="$list"
}
camlist() { echo "${CAMS//,/ }"; }
ncams()   { echo $(camlist) | wc -w; }

save_controls() {
    local d
    for d in $(camlist); do
        local f="$OUT/env/controls_before_video$d.txt"
        [ -s "$f" ] && continue
        v4l2-ctl -d "/dev/video$d" -C auto_exposure,exposure_time_absolute,exposure_dynamic_framerate,gain,power_line_frequency,white_balance_automatic \
            > "$f" 2>/dev/null
    done
}

restore_controls() {
    local d f name val
    for d in $(camlist); do
        f="$OUT/env/controls_before_video$d.txt"
        [ -s "$f" ] || continue
        v4l2-ctl -d "/dev/video$d" -c auto_exposure=1 >/dev/null 2>&1
        for name in exposure_dynamic_framerate gain power_line_frequency white_balance_automatic exposure_time_absolute auto_exposure; do
            val=$(awk -F': ' -v n="$name" '$1==n{print $2}' "$f")
            [ -n "$val" ] && v4l2-ctl -d "/dev/video$d" -c "$name=$val" >/dev/null 2>&1
        done
    done
    log "camera controls restored to the values saved before the test"
}

set_led_controls() {
    local d rb
    for d in $(camlist); do
        v4l2-ctl -d "/dev/video$d" -c auto_exposure=1 >/dev/null 2>&1 \
            || log "   warning: could not set auto_exposure=1 on /dev/video$d"
        v4l2-ctl -d "/dev/video$d" -c exposure_dynamic_framerate=0 >/dev/null 2>&1
        v4l2-ctl -d "/dev/video$d" -c power_line_frequency=0 >/dev/null 2>&1
        v4l2-ctl -d "/dev/video$d" -c white_balance_automatic=0 >/dev/null 2>&1
        v4l2-ctl -d "/dev/video$d" -c exposure_time_absolute="$EXP" >/dev/null 2>&1 \
            || log "   warning: could not set exposure_time_absolute on /dev/video$d"
        v4l2-ctl -d "/dev/video$d" -c gain="$GAIN" >/dev/null 2>&1
        rb=$(v4l2-ctl -d "/dev/video$d" -C auto_exposure,exposure_time_absolute,exposure_dynamic_framerate,gain 2>/dev/null | tr '\n' ' ')
        log "   /dev/video$d controls: ${rb:-n/a}"
    done
}

on_interrupt() { echo; log "INTERRUPTED"; restore_controls; exit 130; }
trap on_interrupt INT TERM

part_check() {
    bar "1. CHECK: tools, build, self-test, webcams"
    local miss=0 c d
    for c in v4l2-ctl python3 make cc tar awk; do
        command -v "$c" >/dev/null 2>&1 || { log "   MISSING command: $c"; miss=1; }
    done
    [ $miss -eq 1 ] && { log "Install: sudo apt install v4l-utils build-essential python3"; exit 1; }
    ( cd "$SRC" && make clean >/dev/null && make ) >> "$LOG" 2>&1 || { log "BUILD FAILED -- see $LOG"; exit 1; }
    if ! ( cd "$SRC" && ./test_tempo ) 2>&1 | tail -3 | tee -a "$LOG" | grep -q "FAILURES: 0"; then
        log "SELF-TEST FAILED -- see $LOG"; exit 1
    fi
    log "build OK, self-test passed"
    detect_cams
    if [ "$(ncams)" -ne 2 ]; then
        log "expected 2 webcams with YUYV capture, found: '${CAMS}'. Set CAMS=a,b and re-run."; exit 1
    fi
    for d in $(camlist); do
        log "   /dev/video$d : $(v4l2-ctl -d /dev/video$d --info 2>/dev/null | awk -F': ' '/Card type|Bus info/{printf "%s  ", $2}')"
    done
    {
        echo "=== uname";      uname -a
        echo "=== lsusb -t";   lsusb -t 2>/dev/null
        echo "=== lsusb";      lsusb 2>/dev/null
        for d in $(camlist); do echo "=== /dev/video$d"; v4l2-ctl -d "/dev/video$d" --all 2>/dev/null; done
    } > "$OUT/env/system.txt" 2>&1
    save_controls
    log "CHECK PASSED   cameras: $CAMS   ${SIZE} @ ${FPS} fps   (T = ${PERIOD_MS} ms)   LED step ${LED_STEP_MS} ms"
}

part_setup() {
    local tag=$1 d="$OUT/led/${1}_setup" rr rc ans
    mkdir -p "$d"
    detect_cams; save_controls
    while :; do
        say ""
        log "LED setup $tag: exposure=$EXP (x100 us)  gain=$GAIN  cams=$CAMS  LED step=${LED_STEP_MS} ms"
        set_led_controls
        "$SRC/ledcap" --cams "$CAMS" --size "$SIZE" --fps "$FPS" --snap 150 \
            --out "$d/snap" >> "$LOG" 2>&1
        $PY "$TOOLS/ledtool.py" roi --prefix "$d/snap" --ncams "$(ncams)" \
            --period-ms "$PERIOD_MS" --out "$OUT/led/${tag}_roi.txt" 2>&1 | tee -a "$LOG"
        rr=${PIPESTATUS[0]}
        rc=9
        if [ "$rr" -le 1 ]; then
            log "20-second decode test ..."
            "$SRC/ledcap" --cams "$CAMS" --size "$SIZE" --fps "$FPS" \
                --roi "$OUT/led/${tag}_roi.txt" --seconds 20 --save-every 0 \
                --out "$d/test" >> "$LOG" 2>&1
            $PY "$TOOLS/ledtool.py" check "$d/test.csv" 2>&1 | tee -a "$LOG"
            rc=${PIPESTATUS[0]}
        fi
        say ""
        say "  Pictures: eog $d/snap_cam0_roi.ppm   (green marks must sit on the 8 LEDs)"
        say "  NOTE: on USB webcams 'outliers>0.5ms' and a WARNING result are EXPECTED --"
        say "        that timing jitter is exactly what this test measures."
        say "        What must be good: 8 LEDs found on both cameras, decode-OK >= 97 %."
        if [ "$rr" -ge 2 ] || [ "$rc" -ge 2 ]; then
            say "  SETUP NOT READY -- fix what is reported above, then press r."
        else
            say "  SETUP USABLE."
        fi
        say ""
        say "  Enter = start the ${RUN_SECS}s measurement (keep the room dark; do not touch the rig)"
        say "  r = repeat   g N = gain N and repeat   e N = exposure N x100us and repeat"
        say "  f = force start   q = quit"
        read -r -p "  > " ans || ans="q"
        echo "  answer: '$ans'" >> "$LOG"
        case "$ans" in
            "")  if [ "$rr" -ge 2 ] || [ "$rc" -ge 2 ]; then say "  Not ready -- press r after fixing, or f to force."; continue; fi; break ;;
            f|F) log "user forced start"; break ;;
            r|R) continue ;;
            g\ *) GAIN="${ans#g }"; continue ;;
            e\ *) EXP="${ans#e }"; continue ;;
            q|Q) log "user quit during setup"; restore_controls; exit 1 ;;
            *)   continue ;;
        esac
    done
    echo "EXP=$EXP GAIN=$GAIN CAMS=$CAMS FPS=$FPS SIZE=$SIZE LED_STEP_MS=$LED_STEP_MS" > "$d/settings_used.txt"
    log "setup $tag accepted: exposure=$EXP gain=$GAIN"
}

part_run() {
    local tag=$1 rc
    detect_cams
    [ -s "$OUT/led/${tag}_roi.txt" ] || { log "no ROI for $tag -- run setup first"; return 1; }
    bar "MEASUREMENT $tag: ${RUN_SECS}s -- do not touch the board or the cameras"
    set_led_controls
    log "run $tag started"
    "$SRC/ledcap" --cams "$CAMS" --size "$SIZE" --fps "$FPS" \
        --roi "$OUT/led/${tag}_roi.txt" --seconds "$RUN_SECS" --save-every 3000 \
        --out "$OUT/led/$tag" > "$OUT/led/${tag}_capture.txt" 2>&1
    rc=$?
    cat "$OUT/led/${tag}_capture.txt" >> "$LOG"
    [ $rc -ne 0 ] && log "   WARNING: ledcap exit $rc"
    $PY "$TOOLS/ledtool.py" check "$OUT/led/$tag.csv" > "$OUT/led/${tag}_check.txt" 2>&1
    cat "$OUT/led/${tag}_check.txt" | tee -a "$LOG"
    log "run $tag finished"
}

part_pack() {
    bar "PACK"
    local name="esl_x86_led_$(hostname -s)_$(date +%Y%m%d_%H%M).tar.gz"
    tar czf "$KIT/$name" -C "$(dirname "$OUT")" "$(basename "$OUT")"
    log "UPLOAD THIS FILE:  $KIT/$name   ($(du -h "$KIT/$name" | cut -f1))"
}

cmd="${1:-help}"
case "$cmd" in
    all)
        part_check
        bar "2. POSITION X1 -- LED board about 30-40 cm in front of BOTH webcams, room dark"
        say "  All 8 LEDs must be visible in both webcams. Black cloth under the board."
        read -r -p "  Press Enter when the board is placed (q to quit) > " a || a=q
        [ "$a" = "q" ] && { restore_controls; exit 1; }
        part_setup X1
        part_run X1
        bar "X1 DONE -- you may move the board now"
        say "  POSITION X2: raise the board about 8 cm (books/box), same distance,"
        say "  square to the webcams, all 8 LEDs still visible in both webcams."
        read -r -p "  Press Enter when the board is placed (q to stop here and pack) > " a || a=q
        if [ "$a" != "q" ]; then
            part_setup X2
            part_run X2
            bar "X2 DONE"
        fi
        restore_controls
        part_pack
        log "ALL DONE"
        ;;
    check)   part_check ;;
    setup)   part_setup "${2:?tag, e.g. X1}" ;;
    run)     part_run "${2:?tag, e.g. X1}" ;;
    pack)    part_pack ;;
    restore) detect_cams; restore_controls ;;
    *)       sed -n '2,14p' "$0" ;;
esac
