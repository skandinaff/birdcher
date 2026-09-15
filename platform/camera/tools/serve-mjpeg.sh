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

# ffmpeg -listen 1 serves exactly one client and exits when that client goes
# away, so a browser refresh would otherwise kill the viewer for good. Restart
# the pipeline per client until the requested duration is up. Capture is
# restarted with it: the pipe dies with ffmpeg, and one owner of the camera at
# a time is the rule anyway.
END=$(( $(date +%s) + SECS ))
while [ "$(date +%s)" -lt "$END" ]; do
    LEFT=$(( END - $(date +%s) ))
    [ "$LEFT" -lt 5 ] && break
    echo "$(date +%H:%M:%S) listening on :${PORT} (${LEFT}s left)" >> /tmp/serve-mjpeg.log

    # End capture when the client goes, so the loop can re-listen. Without
    # this ds1stream outlives ffmpeg for the whole duration and the pipeline
    # never completes.
    DS1_EXIT_ON_SINK_LOSS=1 \
    "$HERE/ds1stream" "$LEFT" "$FPS" 1920 1080 "$NATIVE" 2>"/tmp/serve-capture.log" \
      | ffmpeg -hide_banner -loglevel warning \
          -f rawvideo -pix_fmt nv12 -s 1920x1080 -r "$FPS" -i - \
          -c:v mjpeg -q:v 7 \
          -f mpjpeg -content_type "multipart/x-mixed-replace;boundary=ffmpeg" -listen 1 "http://0.0.0.0:${PORT}"

    # The client disconnected (or never came). Drop the capture that was
    # feeding it before the next listener opens the camera again.
    pkill -f ds1stream 2>/dev/null
    sleep 1
done
