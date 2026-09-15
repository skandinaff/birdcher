#!/bin/bash
# Serve DS1 as MJPEG over HTTP, for looking at the camera in a browser.
#
#   serve-mjpeg.sh [seconds] [fps] [port] [native_fps]
#
# ffmpeg's HTTP muxer serves ONE client (-listen 1) and stops listening once
# that client connects, so this is a diagnostic viewer, not a service. Open
# http://<board>:<port>/ in a browser, or curl it.
set -u
SECS="${1:-300}"
FPS="${2:-15}"
PORT="${3:-8090}"
NATIVE="${4:-15}"
HERE="$(cd "$(dirname "$0")" && pwd)"

pkill -f ds1stream 2>/dev/null
pkill -f mpjpeg 2>/dev/null
sleep 1

"$HERE/ds1stream" "$SECS" "$FPS" 1920 1080 "$NATIVE" 2>"/tmp/serve-capture.log" \
  | ffmpeg -hide_banner -loglevel warning \
      -f rawvideo -pix_fmt nv12 -s 1920x1080 -r "$FPS" -i - \
      -c:v mjpeg -q:v 7 \
      -f mpjpeg -content_type "multipart/x-mixed-replace;boundary=ffmpeg" -listen 1 "http://0.0.0.0:${PORT}"
