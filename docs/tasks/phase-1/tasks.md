# Phase 1 — Bring Up the CSI Camera

## Objective

Prove that the MIPI CSI camera has a usable V4L2/media pipeline and capture one real frame. Do not assume a sensor, video-node number, resolution, frame rate, or hardware encoder.

## Prerequisite

- [ ] Phase 0 is complete and `docs/hardware-baseline.md` has been reviewed.

## Tasks

- [ ] Install the prescribed inspection and capture tools. If any package is unavailable, document the exact package-manager error; do not substitute unrelated packages.

  ```bash
  sudo apt install -y \
    v4l-utils \
    media-types \
    ffmpeg \
    gstreamer1.0-tools \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad
  ```

- [ ] Enumerate the media pipeline and device nodes:

  ```bash
  v4l2-ctl --list-devices
  media-ctl -p
  ls -l /dev/video* 2>/dev/null || true
  ls -l /dev/media* 2>/dev/null || true
  dmesg | grep -Ei 'camera|csi|mipi|isp|sensor|video|v4l2|media'
  ```

- [ ] Identify and record the CSI receiver driver, sensor driver, ISP/media-controller nodes, and each candidate capture node.

- [ ] For every candidate `/dev/videoX`, inspect capabilities and all supported formats, resolutions, and frame rates:

  ```bash
  v4l2-ctl -d /dev/videoX --all
  v4l2-ctl -d /dev/videoX --list-formats-ext
  ```

- [ ] Record the exact candidate node and the best directly usable format in `docs/csi-camera.md`.

- [ ] If no sensor is detected, inspect the active Device Tree and Armbian configuration only. Record the exact sensor model, mainline-driver availability, CSI endpoint status, and any available Armbian overlay before proposing a change.

  ```bash
  grep -R . /proc/device-tree/ 2>/dev/null | grep -Ei 'camera|csi|mipi|sensor' || true
  ```

- [ ] Do not copy vendor-kernel overlays into mainline Armbian and do not modify the Device Tree without documented findings and an explicit next-step decision.

- [ ] Once a working capture node is established, capture a single raw frame, replacing `/dev/videoX` with the confirmed node:

  ```bash
  v4l2-ctl \
    -d /dev/videoX \
    --stream-mmap \
    --stream-count=1 \
    --stream-to=frame.raw
  ```

- [ ] If the negotiated format can be used directly, capture a viewable frame with FFmpeg or GStreamer. Example only; adapt the node and format to the discovered pipeline:

  ```bash
  ffmpeg -f v4l2 -i /dev/videoX -frames:v 1 test.jpg
  ```

- [ ] Preserve the successful command, negotiated format, resolution, frame rate, output-file details, and any required media-controller setup in `docs/csi-camera.md`.

## Exit criteria

- [ ] The CSI sensor is detected.
- [ ] A usable V4L2/media pipeline is documented.
- [ ] Supported resolution and frame rate are known.
- [ ] One real frame has been captured successfully.
- [ ] `docs/csi-camera.md` includes the exact reproducible capture command.
