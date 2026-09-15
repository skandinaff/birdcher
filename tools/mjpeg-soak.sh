#!/bin/bash
# Minimal MJPEG-over-HTTP diagnostic stream, run as a measured soak.
#
#   mjpeg-soak.sh <seconds> [out_fps] [port] [outdir]
#
# Captures DS1 1920x1080 NV12, encodes MJPEG, serves it over HTTP, and records
# FPS, CPU, memory, network bitrate, temperature, frame errors/drops and
# before/after dmesg. Detached from any ssh session: the run survives the
# controlling terminal going away, which matters on this board because the
# WiFi link is not reliable.
#
# ffmpeg's HTTP muxer serves one client at a time. A local curl keeps the
# stream drained so the encoder always has a consumer and the measurement
# continues whether or not a browser is attached; that same curl gives the
# encoded bitrate. Remote viewing is verified separately.
set -u
SECS="${1:-1800}"
FPS="${2:-15}"
PORT="${3:-8090}"
OUT="${4:-$HOME/birdcher-tools/soak-$(date +%Y%m%d-%H%M%S)}"
HERE="$(cd "$(dirname "$0")" && pwd)"

mkdir -p "$OUT"
touch "$OUT/running"

{
  echo "start:    $(date -Is)"
  echo "duration: ${SECS}s   out_fps: ${FPS}   port: ${PORT}"
  echo "uptime:   $(cut -d' ' -f1 /proc/uptime)s"
  echo "kernel:   $(uname -r)"
} > "$OUT/meta.txt"

dmesg > "$OUT/dmesg-before.txt" 2>/dev/null

"$HERE/soak-monitor.sh" "$OUT" 30 >/dev/null 2>&1 &
MON=$!

# Encoded-stream consumer: counts bytes so the bitrate is measured, not guessed.
( sleep 3
  while [ -f "$OUT/running" ]; do
      curl -s --max-time "$SECS" "http://127.0.0.1:${PORT}/" \
        | dd of=/dev/null bs=64k 2>>"$OUT/client-dd.txt"
      sleep 1
  done ) >/dev/null 2>&1 &
CLIENT=$!

"$HERE/ds1stream" "$SECS" "$FPS" 1920 1080 2>"$OUT/capture.log" \
  | ffmpeg -hide_banner -loglevel warning \
      -f rawvideo -pix_fmt nv12 -s 1920x1080 -r "$FPS" -i - \
      -c:v mjpeg -q:v 7 \
      -f mpjpeg -listen 1 "http://0.0.0.0:${PORT}" \
  > "$OUT/ffmpeg.log" 2>&1

rm -f "$OUT/running"
sleep 2
kill "$MON" "$CLIENT" 2>/dev/null
pkill -f "mpjpeg" 2>/dev/null

dmesg > "$OUT/dmesg-after.txt" 2>/dev/null
diff "$OUT/dmesg-before.txt" "$OUT/dmesg-after.txt" > "$OUT/dmesg-delta.txt" 2>/dev/null

echo "end: $(date -Is)" >> "$OUT/meta.txt"
echo "SOAK COMPLETE -> $OUT"
