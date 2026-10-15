#!/usr/bin/env bash
set -uo pipefail

KIT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$KIT/src"
TOOLS="$KIT/tools"
BIN="${BIN:-$SRC}"
OUT="${OUT:-$KIT/results}"
CAMS="${CAMS:-0,1,2,3}"
CAMLIST="${CAMS//,/ }"
NCAMS=$(echo $CAMLIST | wc -w)
DEVDIR="${DEVDIR:-/dev}"

LED_EXP="${LED_EXP:-1}"
LED_GAIN="${LED_GAIN:-20}"
STD_EXP="${STD_EXP:-2}"
STD_GAIN="${STD_GAIN:-6}"

LED_SYNC_SEC="${LED_SYNC_SEC:-120}"
LED_FREE_SECS="${LED_FREE_SECS:-600 600 600 1800}"
SYNC30_SEC="${SYNC30_SEC:-600}"
SYNC30_REPS="${SYNC30_REPS:-3}"
RATE_SEC="${RATE_SEC:-300}"
AUTO_CONTINUE_SEC="${AUTO_CONTINUE_SEC:-300}"

PY="python3 -I"
mkdir -p "$OUT"
LOG="$OUT/RUN_LOG.txt"
TEMPLOG_PID=""

log()  { echo "[$(date '+%F %T')] $*" | tee -a "$LOG"; }
uptime_s() { awk '{print int($1)}' /proc/uptime; }
say()  { echo "$*" | tee -a "$LOG"; }
bar()  { say ""; say "======================================================================"; say "  $*"; say "======================================================================"; }

restore_cams() {
    local d
    for d in $CAMLIST; do
        v4l2-ctl -d "/dev/video$d" -c frame_sync=0 >/dev/null 2>&1
        v4l2-ctl -d "/dev/video$d" -c gain="$STD_GAIN" >/dev/null 2>&1
        v4l2-ctl -d "/dev/video$d" -c exposure_auto=0 >/dev/null 2>&1
    done
}

reset_channels() { "$BIN/tempo_sync" --cams "$CAMS" --reset >/dev/null 2>&1; }

templog_start() {
    [ -n "$TEMPLOG_PID" ] && return
    local f="$OUT/temperature_log.csv" z hdr="epoch"
    for z in /sys/class/thermal/thermal_zone*; do
        [ -r "$z/type" ] && hdr="$hdr,$(cat "$z/type")"
    done
    [ -s "$f" ] || echo "$hdr" > "$f"
    (
        while :; do
            line="$(date +%s)"
            for z in /sys/class/thermal/thermal_zone*; do
                [ -r "$z/type" ] || continue
                v=$(cat "$z/temp" 2>/dev/null || echo NA)
                line="$line,$v"
            done
            echo "$line" >> "$f"
            sleep 10
        done
    ) &
    TEMPLOG_PID=$!
}

templog_stop() {
    if [ -n "$TEMPLOG_PID" ]; then
        kill "$TEMPLOG_PID" 2>/dev/null; wait "$TEMPLOG_PID" 2>/dev/null
        TEMPLOG_PID=""
    fi
}

temps_now() {
    local z s=""
    for z in /sys/class/thermal/thermal_zone*; do
        [ -r "$z/type" ] || continue
        s="$s$(cat "$z/type")=$(cat "$z/temp" 2>/dev/null || echo NA) "
    done
    echo "$s"
}

on_interrupt() {
    echo
    log "INTERRUPTED -- restoring cameras"
    templog_stop
    restore_cams
    reset_channels
    log "Cameras restored. Re-run the part you need, e.g. ./capture_orin.sh rate 6"
    exit 130
}
trap on_interrupt INT TERM
trap templog_stop EXIT

set_cams() {
    local sync=$1 exp=$2 gain=$3 d rb
    for d in $CAMLIST; do
        v4l2-ctl -d "/dev/video$d" -c frame_sync="$sync" >/dev/null 2>&1 \
            || log "   warning: could not set frame_sync on /dev/video$d"
    done
    sleep 1.5
    for d in $CAMLIST; do
        v4l2-ctl -d "/dev/video$d" -c exposure_auto=1 >/dev/null 2>&1
        v4l2-ctl -d "/dev/video$d" -c exposure_time_absolute=50 >/dev/null 2>&1
        v4l2-ctl -d "/dev/video$d" -c exposure_time_absolute="$exp" >/dev/null 2>&1
        v4l2-ctl -d "/dev/video$d" -c gain="$gain" >/dev/null 2>&1
    done
    sleep 0.5
    rb=$(v4l2-ctl -d "/dev/video${CAMLIST%% *}" -C frame_sync,exposure_time_absolute,gain 2>/dev/null | tr '\n' ' ')
    log "   camera controls: frame_sync=$sync exposure=$exp gain=$gain   [readback: ${rb:-n/a}]"
}

tmux_hint() {
    if [ -z "${TMUX:-}${STY:-}" ]; then
        say ""
        say "  NOTE: you are not inside tmux/screen. If this terminal or SSH session"
        say "  closes, the run stops. Safer:   tmux new -s rev   then run this again."
        say "  (Continuing in 10 s; press Ctrl-C to stop.)"
        sleep 10
    fi
}

run_tempo() {
    local tag=$1 size=$2 fps=$3 sec=$4 sync=$5 dir=$6 rc fm ppm t0 t1 tb ta
    mkdir -p "$dir/runs" "$dir/data"
    [ -s "$dir/index.csv" ] || echo "tag,size,fps_req,frame_sync,seconds,start_epoch,end_epoch,exit,fps_meas,ppm,temps_before,temps_after" > "$dir/index.csv"
    log "run $tag : ${size} @${fps} fps, frame_sync=$sync, ${sec}s"
    set_cams "$sync" "$STD_EXP" "$STD_GAIN"
    reset_channels
    t0=$(date +%s); tb=$(temps_now)
    "$BIN/tempo_sync" --cams "$CAMS" --size "$size" --fps "$fps" \
        --seconds "$sec" --csv "$dir/data/$tag.csv" > "$dir/runs/$tag.txt" 2>&1
    rc=$?
    t1=$(date +%s); ta=$(temps_now)
    fm=$(grep -oP 'period\s+[0-9.]+ ms \(\K[0-9.]+' "$dir/runs/$tag.txt" | head -1)
    ppm=$(awk '/MEASURED CLOCKS/{m=1;next} m&&/^ +[0-9]+ +-?[0-9.]+ +[0-9]+ +[0-9]+/{printf "%s ",$2} m&&/period/{exit}' "$dir/runs/$tag.txt")
    echo "$tag,$size,$fps,$sync,$sec,$t0,$t1,$rc,${fm:-NA},\"${ppm}\",\"$tb\",\"$ta\"" >> "$dir/index.csv"
    log "   -> exit $rc   measured ${fm:-?} fps   ppm per camera: ${ppm:-?}"
    if [ "$rc" -ne 0 ] || [ -z "$fm" ]; then
        log "   WARNING: $tag did not complete -- see $dir/runs/$tag.txt"
        grep -iE "fault|error|fail" "$dir/runs/$tag.txt" | head -3 | sed 's/^/      /' | tee -a "$LOG"
        reset_channels
        return 1
    fi
    return 0
}

save_env() {
    local e="$OUT/env" d
    mkdir -p "$e"
    {
        date; hostname; uname -a
        cat /etc/nv_tegra_release 2>/dev/null
        command -v nvpmodel >/dev/null && nvpmodel -q 2>/dev/null
        df -h "$OUT"
    } > "$e/system.txt" 2>&1
    for d in $CAMLIST; do
        v4l2-ctl -d "/dev/video$d" --all > "$e/video$d.txt" 2>&1
    done
    cp "$KIT/arduino/led_gray_counter/led_gray_counter.ino" "$e/" 2>/dev/null
}

part_check() {
    bar "1. CHECK: tools, build, self-test, cameras"
    local miss=0 c d
    for c in v4l2-ctl python3 make cc shuf awk tar; do
        if ! command -v "$c" >/dev/null 2>&1; then log "   MISSING command: $c"; miss=1; fi
    done
    if ! echo "period 9.9 ms (100.0 fps)" | grep -oP 'period\s+[0-9.]+ ms \(\K[0-9.]+' >/dev/null 2>&1; then
        log "   MISSING: grep with -P support"; miss=1
    fi
    [ $miss -eq 1 ] && { log "Install the missing tools (sudo apt install v4l-utils build-essential) and re-run."; exit 1; }

    log "building ..."
    if [ "$BIN" = "$SRC" ]; then
        ( cd "$SRC" && make clean >/dev/null && make ) >> "$LOG" 2>&1 \
            || { log "BUILD FAILED -- see $LOG"; exit 1; }
    fi
    if ! ( cd "$SRC" && ./test_tempo ) 2>&1 | tail -3 | tee -a "$LOG" | grep -q "FAILURES: 0"; then
        log "SELF-TEST FAILED -- do not continue; see $LOG"; exit 1
    fi
    log "build OK, self-test 14/14 passed"

    for d in $CAMLIST; do
        if [ ! -e "$DEVDIR/video$d" ]; then log "   /dev/video$d MISSING -- check camera cables"; exit 1; fi
        if ! v4l2-ctl -d "/dev/video$d" --list-ctrls 2>/dev/null | grep -q frame_sync; then
            log "   /dev/video$d has no frame_sync control -- wrong device?"; exit 1
        fi
    done
    log "cameras: $CAMS present, frame_sync control found on each"

    local avail
    avail=$(df -Pk "$OUT" | awk 'NR==2{print int($4/1024/1024)}')
    log "free disk: ${avail} GB"
    [ "${avail:-0}" -lt 3 ] && log "   WARNING: less than 3 GB free -- free some space before the overnight part"

    save_env
    templog_start
    log "pre-flight capture (15 s, 640x480 @100, free-running) ..."
    if ! run_tempo preflight 640x480 100 15 0 "$OUT/check"; then
        log "PRE-FLIGHT FAILED. Try:  ./capture_orin.sh reset   then   ./capture_orin.sh check"
        log "If it still fails, power-cycle the Orin (a warm reboot is not enough)."
        exit 1
    fi
    log "CHECK PASSED"
}

led_setup() {
    local d="$OUT/led/setup" rr rc ans
    mkdir -p "$d"
    while :; do
        say ""
        log "LED setup: exposure=$LED_EXP (x100 us)  gain=$LED_GAIN"
        set_cams 0 "$LED_EXP" "$LED_GAIN"
        reset_channels
        "$BIN/ledcap" --cams "$CAMS" --size 640x480 --fps 100 --snap 300 \
            --out "$d/snap" >> "$LOG" 2>&1
        $PY "$TOOLS/ledtool.py" roi --prefix "$d/snap" --ncams "$NCAMS" \
            --out "$OUT/led/roi.txt" 2>&1 | tee -a "$LOG"
        rr=${PIPESTATUS[0]}
        rc=9
        if [ "$rr" -le 1 ]; then
            log "15-second decode test ..."
            reset_channels
            "$BIN/ledcap" --cams "$CAMS" --size 640x480 --fps 100 \
                --roi "$OUT/led/roi.txt" --seconds 15 --save-every 0 \
                --out "$d/test" >> "$LOG" 2>&1
            $PY "$TOOLS/ledtool.py" check "$d/test.csv" 2>&1 | tee -a "$LOG"
            rc=${PIPESTATUS[0]}
        fi
        say ""
        say "  Annotated pictures: $d/snap_cam0_roi.ppm ... cam$((NCAMS-1))"
        say "  (open with:  eog $d/snap_cam0_roi.ppm   -- green marks must sit on the 8 LEDs)"
        if [ "$rr" -ge 2 ] || [ "$rc" -ge 2 ]; then
            say "  SETUP NOT READY -- fix what is reported above, then press r."
        elif [ "$rr" -eq 1 ] || [ "$rc" -eq 1 ]; then
            say "  SETUP USABLE WITH WARNINGS -- improve and press r, or press Enter to go on."
        else
            say "  SETUP PASSED."
        fi
        say ""
        say "  Enter = start LED measurements (~80 min; keep the room dark; do not touch the rig)"
        say "  r     = repeat the setup after adjusting"
        say "  g N   = set gain to N and repeat         e N = set exposure to N x100us and repeat"
        say "  f     = force start even though the setup did not pass"
        say "  q     = quit"
        read -r -p "  > " ans || ans="q"
        echo "  answer: '$ans'" >> "$LOG"
        case "$ans" in
            "")  if [ "$rr" -ge 2 ] || [ "$rc" -ge 2 ]; then say "  Not passed yet -- press r after fixing, or f to force."; continue; fi; break ;;
            f|F) log "user forced start"; break ;;
            r|R) continue ;;
            g\ *) LED_GAIN="${ans#g }"; continue ;;
            e\ *) LED_EXP="${ans#e }"; continue ;;
            q|Q) log "user quit during LED setup"; restore_cams; exit 1 ;;
            *)   continue ;;
        esac
    done
    cp "$OUT/led/roi.txt" "$d/roi_used.txt"
    echo "LED_EXP=$LED_EXP LED_GAIN=$LED_GAIN" > "$d/settings_used.txt"
    log "LED setup accepted: exposure=$LED_EXP gain=$LED_GAIN"
}

led_capture() {
    local tag=$1 sync=$2 sec=$3 rc
    log "LED run $tag : frame_sync=$sync, ${sec}s   (temps: $(temps_now))"
    set_cams "$sync" "$LED_EXP" "$LED_GAIN"
    reset_channels
    "$BIN/ledcap" --cams "$CAMS" --size 640x480 --fps 100 \
        --roi "$OUT/led/roi.txt" --seconds "$sec" --save-every 6000 \
        --out "$OUT/led/$tag" > "$OUT/led/${tag}_capture.txt" 2>&1
    rc=$?
    cat "$OUT/led/${tag}_capture.txt" >> "$LOG"
    [ $rc -ne 0 ] && log "   WARNING: ledcap exit $rc"
    $PY "$TOOLS/ledtool.py" check "$OUT/led/$tag.csv" > "$OUT/led/${tag}_check.txt" 2>&1
    rc=$?
    cat "$OUT/led/${tag}_check.txt" | tee -a "$LOG"
    return $rc
}

part_led() {
    bar "2. LED GROUND TRUTH  (you need to be here for the first ~15 minutes)"
    say ""
    say "  BEFORE PRESSING ENTER:"
    say "   1. The Arduino runs the NEW sketch led_gray_counter.ino (all 8 LEDs flash"
    say "      ON for 1 s when it powers up). Unplug any LED on pin D10."
    say "   2. LED board 30-60 cm in front of the cameras; all 8 LEDs visible in ALL"
    say "      4 cameras; LEDs at about the same height in every image."
    say "   3. Put black cloth/paper on the table under the LEDs (stops reflections)."
    say "   4. Room lights OFF (or as dark as possible). Nothing else that blinks."
    say ""
    read -r -p "  Press Enter when ready (q to skip the LED part) > " ans || ans="q"
    case "$ans" in q|Q) log "LED part skipped by user"; return 1 ;; esac
    templog_start
    mkdir -p "$OUT/led"
    led_setup

    bar "LED measurements -- leave the rig and the room as they are"
    local i=0 s fails=0
    led_capture L0_sync60 2 "$LED_SYNC_SEC" || fails=$((fails+1))
    for s in $LED_FREE_SECS; do
        i=$((i+1))
        led_capture "L${i}_free_${s}s" 0 "$s" || fails=$((fails+1))
    done
    restore_cams
    set_cams 0 "$STD_EXP" "$STD_GAIN"
    log "LED part finished ($fails run(s) with warnings/fails -- see the check outputs above)"
}

part_sync30() {
    bar "3. 30 fps FREE-RUNNING vs 30 Hz HARDWARE SYNC (unattended)"
    templog_start
    local dir="$OUT/sync30" ssize=3840x2160 sfps=30 r fm
    log "pre-flight: does 3840x2160 run under 30 Hz sync?"
    run_tempo pre_sync30_3840x2160 3840x2160 30 20 1 "$dir/preflight"
    fm=$(grep -oP 'period\s+[0-9.]+ ms \(\K[0-9.]+' "$dir/preflight/runs/pre_sync30_3840x2160.txt" 2>/dev/null | head -1)
    if [ -z "$fm" ] || ! awk -v f="$fm" 'BEGIN{exit !(f>25 && f<31)}'; then
        log "   4K under sync did not give ~30 fps (${fm:-none}); using 640x480 for the sync arm"
        ssize=640x480; sfps=100
    fi
    for r in $(seq 1 "$SYNC30_REPS"); do
        if [ $((r % 2)) -eq 1 ]; then
            run_tempo "S${r}_free30_3840x2160" 3840x2160 30 "$SYNC30_SEC" 0 "$dir"
            run_tempo "S${r}_sync30_${ssize}" "$ssize" "$sfps" "$SYNC30_SEC" 1 "$dir"
        else
            run_tempo "S${r}_sync30_${ssize}" "$ssize" "$sfps" "$SYNC30_SEC" 1 "$dir"
            run_tempo "S${r}_free30_3840x2160" 3840x2160 30 "$SYNC30_SEC" 0 "$dir"
        fi
    done
    restore_cams
    $PY "$TOOLS/ledtool.py" tempo "$dir/runs" 2>&1 | tee "$dir/SUMMARY.txt" | tee -a "$LOG"
    log "sync30 part finished"
}

part_rate() {
    local hours=${1:-8}
    bar "4. CAPTURE-RATE REPLICATES, random order, ${hours} h (unattended)"
    templog_start
    local dir="$OUT/rate" deadline block=0 fails=0 order i now size fps tag n=0
    local MODES=("640x480 100" "1280x720 72" "1920x1080 72" "3840x2160 30")
    deadline=$(( $(uptime_s) + $(awk -v h="$hours" 'BEGIN{printf "%d", h*3600}') ))
    log "will stop starting new runs after ${hours} h (timed by uptime, immune to clock changes)"
    while :; do
        block=$((block+1))
        order=$(printf "%s\n" 0 1 2 3 | shuf)
        for i in $order; do
            now=$(uptime_s)
            if [ $((deadline - now)) -lt $((RATE_SEC + 60)) ]; then break 2; fi
            set -- ${MODES[$i]}; size=$1; fps=$2
            tag=$(printf "R%02d_%sfps_%s" "$block" "$fps" "$size")
            if run_tempo "$tag" "$size" "$fps" "$RATE_SEC" 0 "$dir"; then
                fails=0; n=$((n+1))
            else
                fails=$((fails+1))
            fi
            if [ $fails -ge 3 ]; then
                log "three failed runs in a row -- stopping the rate part (hardware problem?)"
                break 2
            fi
        done
    done
    restore_cams
    $PY "$TOOLS/ledtool.py" tempo "$dir/runs" > "$dir/SUMMARY.txt" 2>&1
    log "rate part finished: $n good runs in $block block(s). Summary: $dir/SUMMARY.txt"
}

part_pack() {
    bar "5. PACK RESULTS"
    templog_stop
    local name="esl_revision_results_$(hostname -s)_$(date +%Y%m%d_%H%M).tar.gz"
    tar czf "$KIT/$name" -C "$(dirname "$OUT")" "$(basename "$OUT")"
    log "UPLOAD THIS FILE:  $KIT/$name   ($(du -h "$KIT/$name" | cut -f1))"
}

cmd="${1:-help}"
case "$cmd" in
    all)
        hours="${2:-8}"
        log "capture_orin.sh all  (rate part: ${hours} h)   kit: $KIT"
        tmux_hint
        part_check
        part_led
        bar "LED part done. The rest runs unattended."
        say "  You can switch the lights on and leave. Do not unplug cameras."
        say "  Starting in ${AUTO_CONTINUE_SEC}s, or press Enter to start now."
        read -r -t "$AUTO_CONTINUE_SEC" _ || true
        part_sync30
        part_rate "$hours"
        part_pack
        log "ALL DONE."
        ;;
    check)  part_check ;;
    led)    tmux_hint; part_led; part_pack ;;
    sync30) tmux_hint; part_sync30 ;;
    rate)   tmux_hint; part_rate "${2:-8}" ;;
    pack)   part_pack ;;
    reset)  reset_channels; restore_cams; log "capture channels released, cameras set to free-running" ;;
    *)
        sed -n '2,20p' "$0"
        ;;
esac
