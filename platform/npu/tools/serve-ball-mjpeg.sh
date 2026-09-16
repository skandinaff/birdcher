#!/bin/bash
# DS1 -> RGB -> CPU proposals/NPU tennis-ball verification -> boxed MJPEG.
# One client at a time. Reopen the camera pipeline after each disconnect.
set -u

SECS="${1:-3600}"
FPS="${2:-15}"
PORT="${3:-8090}"
CAMERA_TOOLS=/home/skf/birdcher-tools
NPU_TOOLS=/home/skf/birdcher-tools/npu
END=$(( $(date +%s) + SECS ))

while [ "$(date +%s)" -lt "$END" ]; do
    LEFT=$(( END - $(date +%s) ))
    [ "$LEFT" -lt 5 ] && break
    echo "$(date +%H:%M:%S) starting boxed pipeline (${LEFT}s left)" >> /tmp/birdcher-ball-preview.log

    DS1_EXIT_ON_SINK_LOSS=1 \
    "$CAMERA_TOOLS/ds1stream" "$LEFT" "$FPS" 1920 1080 "$FPS" 2>>/tmp/birdcher-ball-capture.log \
      | ffmpeg -hide_banner -loglevel error \
          -f rawvideo -pix_fmt nv12 -s 1920x1080 -r "$FPS" -i - \
          -vf scale=640:360 -pix_fmt rgb24 -f rawvideo - 2>>/tmp/birdcher-ball-convert.log \
      | "$NPU_TOOLS/tflite-ball-stream" \
          "$NPU_TOOLS/ssdlite_mobiledet_coco_qat_postprocess.tflite" \
          "$NPU_TOOLS/mobilenet_v1_1.0_224_quant.tflite" 2>>/tmp/birdcher-ball-inference.log \
      | ffmpeg -hide_banner -loglevel error \
          -f rawvideo -pix_fmt rgb24 -s 640x360 -r "$FPS" -i - \
          -c:v mjpeg -q:v 6 \
          -f mpjpeg -content_type "multipart/x-mixed-replace;boundary=ffmpeg" \
          -listen 1 "http://0.0.0.0:${PORT}" 2>>/tmp/birdcher-ball-encoder.log

    pkill -f ds1stream 2>/dev/null
    sleep 1
done
