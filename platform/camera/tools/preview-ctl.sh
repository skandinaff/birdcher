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
# `start` also re-applies the sensor frame rate and indoor image profile,
# because both reset on every module reload or reboot.
set -u

UNIT=mjpeg-preview
PORT=8090
RUN_USER=skf
TOOLS=/home/skf/birdcher-tools
VID=/dev/video1
SUB=/dev/v4l-subdev0
LINE_RATE=135000      # lines/s at the 3864x2192 preset
ACTIVE_LINES=2192
EXPOSURE_LINES="${BIRDCHER_EXPOSURE_LINES:-1350}" # 10 ms at 135000 lines/s
ANALOG_GAIN="${BIRDCHER_ANALOG_GAIN:-96}"         # log2(gain) x 32 = 8x / 18 dB

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

set_image_profile() {
    # The userspace 3A daemon is not present, so automatic exposure never
    # raises sensor gain and leaves indoor scenes severely underexposed. Keep a
    # deterministic 50 Hz-safe shutter and moderate analogue gain until that
    # loop exists. Environment overrides make measured alternatives testable
    # without editing the script.
    v4l2-ctl -d "$VID" --set-ctrl="manual_exposure_set=1,sensor_integration_timet_set=$EXPOSURE_LINES,sensor_analog_gain_set=$ANALOG_GAIN,sensor_digital_gain_set=0,isp_digital_gain_set=0" 2>/dev/null
    echo "image:      exposure=${EXPOSURE_LINES} lines, analogue_gain=${ANALOG_GAIN}/32 stops"
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
    local act lis procs vb dfps exp again
    act=$(systemctl is-active "$UNIT" 2>/dev/null)
    lis=$(ss -tln 2>/dev/null | grep -c ":${PORT}")
    procs=$(ps -eo comm= | grep -cE '^(ds1stream|ffmpeg)$')
    vb=$(v4l2-ctl -d "$SUB" --get-ctrl=vertical_blanking 2>/dev/null | cut -d: -f2 | tr -d '[:space:]')
    exp=$(v4l2-ctl -d "$SUB" --get-ctrl=exposure 2>/dev/null | cut -d: -f2 | tr -d '[:space:]')
    again=$(v4l2-ctl -d "$SUB" --get-ctrl=analogue_gain 2>/dev/null | cut -d: -f2 | tr -d '[:space:]')
    dfps=$(awk -v vb="${vb:-0}" -v lr="$LINE_RATE" -v al="$ACTIVE_LINES" \
           'BEGIN{ t=vb+al; if(t>0) printf "%.2f", lr/t; else printf "?" }')
    echo "unit:        ${act:-inactive}"
    echo "listening:   $([ "$lis" -gt 0 ] && echo "yes on :${PORT}" || echo "no")"
    echo "capture:     ${procs} process(es)"
    echo "frame rate:  ${dfps} fps (vertical_blanking=${vb:-?})"
    echo "exposure:    ${exp:-?} lines"
    echo "sensor gain: ${again:-?} (0.3 dB steps)"
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
        set_image_profile
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
