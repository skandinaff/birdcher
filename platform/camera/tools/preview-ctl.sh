#!/bin/bash
# Start, stop, restart or inspect the MJPEG preview.
#
#   preview-ctl.sh start [seconds] [fps]
#   preview-ctl.sh stop
#   preview-ctl.sh restart [seconds] [fps]
#   preview-ctl.sh status
#
# Runs the viewer as a transient systemd unit so it survives the ssh session
# that started it -- this board is on WiFi and dropped links have killed
# backgrounded commands repeatedly.
#
# `start` also re-applies the sensor frame rate, because vertical_blanking is a
# runtime setting that resets to 60 fps on every module reload or reboot.
set -u

UNIT=mjpeg-preview
PORT=8090
RUN_USER=skf
TOOLS=/home/skf/birdcher-tools
SUB=/dev/v4l-subdev0
LINE_RATE=135000      # lines/s at the 3864x2192 preset
ACTIVE_LINES=2192

[ "$(id -u)" -eq 0 ] || exec sudo -n "$0" "$@" 2>/dev/null || exec sudo "$0" "$@"

cmd="${1:-status}"
secs="${2:-3600}"
fps="${3:-30}"

set_fps() {
    local want="$1" vb
    vb=$(( LINE_RATE / want - ACTIVE_LINES ))
    if [ "$vb" -lt 58 ]; then vb=58; fi
    v4l2-ctl -d "$SUB" --set-ctrl=vertical_blanking="$vb" 2>/dev/null
    echo "frame rate: ${want} fps (vertical_blanking=$vb)"
}

stop_all() {
    systemctl stop "$UNIT" 2>/dev/null
    systemctl reset-failed "$UNIT" 2>/dev/null
    # serve-mjpeg.sh relists per client, so kill the unit before the pipeline
    # or it simply starts another one.
    pkill -f serve-mjpeg 2>/dev/null
    pkill -f ds1stream 2>/dev/null
    pkill -f mpjpeg 2>/dev/null
    sleep 2
}

status() {
    local act lis procs vb dfps
    act=$(systemctl is-active "$UNIT" 2>/dev/null)
    lis=$(ss -tln 2>/dev/null | grep -c ":${PORT}")
    procs=$(ps -eo comm= | grep -cE '^(ds1stream|ffmpeg)$')
    vb=$(v4l2-ctl -d "$SUB" --get-ctrl=vertical_blanking 2>/dev/null | cut -d: -f2 | tr -d '[:space:]')
    dfps=$(awk -v vb="${vb:-0}" -v lr="$LINE_RATE" -v al="$ACTIVE_LINES" \
           'BEGIN{ t=vb+al; if(t>0) printf "%.2f", lr/t; else printf "?" }')
    echo "unit:        ${act:-inactive}"
    echo "listening:   $([ "$lis" -gt 0 ] && echo "yes on :${PORT}" || echo "no")"
    echo "capture:     ${procs} process(es)"
    echo "frame rate:  ${dfps} fps (vertical_blanking=${vb:-?})"
    echo "url:         http://$(hostname -I | awk '{print $1}'):${PORT}/"
    if [ "$act" = "active" ] && [ "$lis" -eq 0 ] && [ "$procs" -le 1 ]; then
        echo
        echo "note: a client has the stream, or one disconnected and the next"
        echo "      listener has not opened yet. 'restart' clears it."
    fi
}

case "$cmd" in
    start)
        stop_all
        set_fps "$fps"
        systemd-run --unit="$UNIT" --uid="$RUN_USER" --gid="$RUN_USER" \
            --working-directory="$TOOLS" \
            "$TOOLS/serve-mjpeg.sh" "$secs" "$fps" "$PORT" "$fps" >/dev/null || exit 1
        sleep 6
        status
        ;;
    stop)
        stop_all
        echo "stopped."
        status
        ;;
    restart)
        exec "$0" start "$secs" "$fps"
        ;;
    status)
        status
        ;;
    *)
        sed -n '2,10p' "$0"
        exit 2
        ;;
esac
