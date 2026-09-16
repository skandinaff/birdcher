#!/bin/bash
# Switch the diagnostic browser viewer to/from the boxed-ball preview.
set -u

UNIT=mjpeg-ball-preview
CAMERA_TOOLS=/home/skf/birdcher-tools
NPU_TOOLS=/home/skf/birdcher-tools/npu
NORMAL="$CAMERA_TOOLS/preview-ctl.sh"
FPS="${2:-30}"

[ "$(id -u)" -eq 0 ] || exec sudo -n "$0" "$@" 2>/dev/null || exec sudo "$0" "$@"

case "${1:-status}" in
    start)
        if [ "$FPS" -lt 1 ] || [ "$FPS" -gt 30 ]; then
            echo "fps must be 1..30" >&2
            exit 2
        fi
        "$NORMAL" stop
        systemctl stop "$UNIT" 2>/dev/null
        systemctl reset-failed "$UNIT" 2>/dev/null
        VB=$(( 135000 / FPS - 2192 ))
        v4l2-ctl -d /dev/v4l-subdev0 --set-ctrl=vertical_blanking="$VB" || exit 1
        v4l2-ctl -d /dev/video1 --set-ctrl='manual_exposure_set=1,sensor_integration_timet_set=1350,sensor_analog_gain_set=96,sensor_digital_gain_set=0,isp_digital_gain_set=0' || exit 1
        systemd-run --unit="$UNIT" --uid=skf --gid=skf \
            --working-directory="$NPU_TOOLS" \
            "$NPU_TOOLS/serve-ball-mjpeg.sh" 3600 "$FPS" 8090 || exit 1
        echo "boxed preview starting: http://192.168.1.38:8090/"
        ;;
    stop)
        systemctl stop "$UNIT" 2>/dev/null
        systemctl reset-failed "$UNIT" 2>/dev/null
        pkill -f tflite-ball-stream 2>/dev/null
        pkill -f ds1stream 2>/dev/null
        "$NORMAL" start 3600 30
        ;;
    status)
        systemctl show "$UNIT" -p ActiveState -p Result -p ExecMainStatus
        ss -ltn | grep ':8090' || true
        tail -n 4 /tmp/birdcher-ball-inference.log 2>/dev/null || true
        ;;
    *)
        echo "usage: $0 start [fps] | stop | status" >&2
        exit 2
        ;;
esac
