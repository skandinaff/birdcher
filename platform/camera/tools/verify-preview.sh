#!/bin/bash
# Self-contained check that DS1 -> MJPEG -> HTTP actually serves a client.
# Writes its verdict to /tmp/preview-verify.txt so the result survives a
# dropped ssh session.
#
#   verify-preview.sh [seconds] [port]
set -u
SECS="${1:-600}"
PORT="${2:-8090}"
R=/tmp/preview-verify.txt
HERE="$(cd "$(dirname "$0")" && pwd)"

: > "$R"
say() { echo "$*" >> "$R"; }

# Only one process may own the camera: stream ids on /dev/video1 come from open
# order, so a leftover capture makes a new one pick up different streams.
pkill -9 -f mjpeg-soak; pkill -9 -f ds1stream; pkill -9 -f ffmpeg; pkill -9 -f soak-monitor
sleep 3
say "survivors after cleanup: $(ps -eo comm= | grep -cE '^(ds1stream|ffmpeg)$')"
say "vblank: $(v4l2-ctl -d /dev/v4l-subdev0 --get-ctrl=vertical_blanking 2>/dev/null | cut -d: -f2 | tr -d '[:space:]')"

"$HERE/ds1stream" "$SECS" 15 1920 1080 15 2>/tmp/preview-capture.log \
  | ffmpeg -hide_banner -loglevel error \
      -f rawvideo -pix_fmt nv12 -s 1920x1080 -r 15 -i - \
      -c:v mjpeg -q:v 7 -f mpjpeg -content_type "multipart/x-mixed-replace;boundary=ffmpeg" -listen 1 "http://0.0.0.0:${PORT}" \
  >/tmp/preview-ffmpeg.log 2>&1 &
PIPE=$!

for i in $(seq 1 20); do
    sleep 2
    if ss -tln 2>/dev/null | grep -q ":${PORT}"; then
        say "listening after ${i}x2s"
        break
    fi
done
ss -tln 2>/dev/null | grep -q ":${PORT}" || say "NEVER LISTENED"

say "--- capture log ---"; head -3 /tmp/preview-capture.log >> "$R" 2>/dev/null
say "--- ffmpeg log ---";  head -3 /tmp/preview-ffmpeg.log  >> "$R" 2>/dev/null
say "URL: http://$(hostname -I | awk '{print $1}'):${PORT}/"
say "serving until $(date -d "+${SECS} seconds" +%H:%M:%S)"
wait $PIPE 2>/dev/null
