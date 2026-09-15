# Khadas VIM3 Bird Camera Project — Implementation Plan

> **Progress, 2026-09-15.** Phase 0 DONE. Phase 1 DONE (and far exceeded: the
> CSI receiver and ISP had to be ported, not just configured — see
> [STATUS.md](STATUS.md)). Phase 2 NOT DONE — capture works but the 30-minute
> stability run has not been made. Phase 3 NOT STARTED, and reshaped by one
> finding: the A311D **has** H.264/HEVC/JPEG hardware encoders, but **mainline
> exposes none of them** (vendor drivers exist in `media_modules`), so encoding
> is software until someone ports them. See
> [tasks/2026-09-16-streaming-infrastructure.md](tasks/2026-09-16-streaming-infrastructure.md).
> Phases 4-10 unchanged.

## Goal

Build a headless bird-observation system on a **Khadas VIM3** running **Armbian**.

The camera is connected through **MIPI CSI**, not USB.

The final system should:

1. Capture video from the CSI camera.
2. Provide a live stream accessible from a web browser on the local network.
3. Detect whether a bird is present.
4. Preferably run object detection on the **A311D NPU** using the modern mainline Linux stack.
5. Automatically record useful video clips when a bird is detected.
6. Preserve video shortly before and after the detection event.
7. Run headless without a desktop environment.
8. Start automatically after reboot.

Do not assume that CSI, hardware video encoding, or the NPU already work. Validate each subsystem independently before integrating them.

---

## Current Platform

Hardware:

- Khadas VIM3
- Amlogic A311D
- Built-in A311D NPU
- MIPI CSI camera
- eMMC storage
- Ethernet/Wi-Fi as available

OS:

- Armbian
- Ubuntu-based minimal/headless installation
- Recent mainline kernel

The exact CSI camera sensor is not yet specified. Detect the sensor and available drivers before changing configuration.

---

# Phase 0 — Collect Baseline Information  ✅ DONE

> Recorded in `hardware/baseline.md` and `kernel/baseline.md`.

Before installing or changing anything, save the complete baseline.

Run:

```bash
uname -a
cat /etc/os-release
cat /etc/armbian-release 2>/dev/null || true

lsblk -o NAME,SIZE,FSTYPE,MOUNTPOINTS
df -h

ip addr
ip route

ls -l /dev/video* 2>/dev/null || true
ls -l /dev/media* 2>/dev/null || true
ls -l /dev/dri/ 2>/dev/null || true

dmesg | grep -Ei 'camera|csi|mipi|isp|sensor|video|v4l2|media|etnaviv|npu|galcore|meson'
```

Also collect:

```bash
sudo apt update
apt policy v4l-utils ffmpeg gstreamer1.0-tools
```

Save all relevant output in:

```text
docs/hardware-baseline.md
```

Do not make destructive bootloader, SPI, eMMC, or Device Tree changes during this phase.

---

# Phase 1 — Bring Up the CSI Camera  ✅ DONE

> Result in `camera/bringup-status.md`. Real frames captured; DS1 1920x1080
> NV12 is the working mode. The assumption that this was a configuration job
> was wrong — no mainline G12B CSI/ISP existed and it had to be ported.

This is the first major milestone.

## 1. Install camera inspection tools

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

If a package is unavailable, document it instead of silently substituting unrelated packages.

## 2. Identify the camera/media pipeline

Run:

```bash
v4l2-ctl --list-devices
media-ctl -p
```

Check:

```bash
ls -l /dev/video*
ls -l /dev/media*
```

Inspect kernel messages:

```bash
dmesg | grep -Ei 'camera|csi|mipi|isp|sensor|video|v4l2|media'
```

Determine:

- CSI receiver driver
- camera sensor driver
- ISP/media-controller nodes
- `/dev/videoX` capture node
- supported pixel formats
- supported resolutions
- supported frame rates

For every candidate video node:

```bash
v4l2-ctl -d /dev/videoX --all
v4l2-ctl -d /dev/videoX --list-formats-ext
```

## 3. Inspect Device Tree only if required

If the sensor is not detected:

```bash
grep -R . /proc/device-tree/ 2>/dev/null | grep -Ei 'camera|csi|mipi|sensor' || true
```

Inspect active DT/overlays and Armbian configuration.

Do not blindly copy Khadas vendor-kernel Device Tree overlays into a mainline Armbian kernel.

First establish:

- exact camera sensor model
- whether a mainline driver exists
- whether the VIM3 Device Tree already contains the CSI endpoint
- whether an Armbian overlay already exists

Document findings before modifying DT.

## 4. Capture one frame

Once a working V4L2 node is found:

```bash
v4l2-ctl \
  -d /dev/videoX \
  --stream-mmap \
  --stream-count=1 \
  --stream-to=frame.raw
```

If the node exposes a directly usable format, also test with FFmpeg or GStreamer.

Example only:

```bash
ffmpeg -f v4l2 -i /dev/videoX -frames:v 1 test.jpg
```

Do not assume `/dev/video0`.

## Phase 1 acceptance criteria

The phase is complete only when:

- the CSI sensor is detected
- a usable V4L2/media pipeline exists
- supported resolution and frame rate are known
- a real frame can be captured successfully
- the exact capture command is documented

---

# Phase 2 — Stable Continuous Video Capture  ⬜ NOT DONE

> Capture works; the 30-minute proof does not exist yet. Longest run 70 s
> (4195 frames, 59.91 fps, 6 drops, 0 timeouts). `tools/ds1soak.c` does the
> measuring. Memory growth is the open question.

Before ML or web streaming, prove that video capture is stable.

Run a continuous capture test for at least 30 minutes.

Preferred initial target:

```text
1920x1080
15–30 FPS
```

If the CSI pipeline cannot sustain this, use the best stable mode reported by V4L2.

Monitor:

```bash
htop
```

and:

```bash
watch -n 1 cat /sys/class/thermal/thermal_zone*/temp
```

Check kernel errors:

```bash
dmesg -w
```

Look specifically for:

- CSI errors
- dropped buffers
- DMA errors
- ISP errors
- V4L2 timeouts
- thermal throttling

Document the stable capture pipeline.

---

# Phase 3 — Browser Live Stream  ⬜ NOT STARTED

> **Reshaped:** the "investigate hardware encoding" question below is answered.
> The silicon has `amvenc_avc`, `cnm HevcEnc` and `jpegenc`; mainline exposes
> none of them. Encoding is software until someone ports the vendor drivers,
> and that software cost competes with the NPU budget. See the streaming task.

Goal:

```text
CSI camera
    ↓
V4L2 / media pipeline
    ↓
go2rtc
    ↓
WebRTC / MSE
    ↓
browser
```

Use **go2rtc** for the first streaming implementation.

Do not build a custom frontend yet.

## Requirements

The user must be able to open a URL from another computer/phone on the LAN and see the live camera.

Prefer:

- WebRTC for low latency
- MSE as fallback

Avoid unnecessary transcoding.

If the CSI output format is not directly acceptable to go2rtc, use FFmpeg or GStreamer as an input/transcoding bridge.

Measure:

- CPU load
- RAM use
- stream latency
- dropped frames
- network bitrate

## Hardware encoding

Investigate whether the current mainline Armbian kernel exposes usable A311D hardware H.264/H.265 encoding.

Do not assume Khadas vendor VPU APIs work on mainline Armbian.

Check available FFmpeg encoders:

```bash
ffmpeg -hide_banner -encoders | grep -Ei '264|265|v4l2|vaapi|meson'
```

Also inspect V4L2 codec devices:

```bash
v4l2-ctl --list-devices
```

If no reliable hardware encoder exists, initially use software encoding at a conservative resolution/frame rate.

## Phase 3 acceptance criteria

- live video works in a normal browser
- no desktop environment is required
- stream survives at least 30 minutes
- latency and CPU use are documented
- service can later be started through systemd

---

# Phase 4 — Validate the A311D NPU  ⬜ NOT STARTED

Do this independently from the camera pipeline.

Target stack:

```text
TensorFlow Lite model
        ↓
Mesa Teflon delegate
        ↓
etnaviv
        ↓
A311D NPU
```

Do not use the old Khadas KSNN/Acuity `.nb` stack unless the modern mainline path proves unusable.

## 1. Verify kernel support

Run:

```bash
dmesg | grep -Ei 'etnaviv|npu|galcore'
ls -l /dev/dri/
```

Inspect kernel config if available:

```bash
zgrep CONFIG_DRM_ETNAVIV /proc/config.gz 2>/dev/null || \
grep CONFIG_DRM_ETNAVIV /boot/config-$(uname -r)
```

Record whether an NPU-capable etnaviv device is detected.

## 2. Verify Teflon availability

Search packages:

```bash
apt search teflon
apt search mesa | grep -i teflon
```

Prefer distribution packages.

Do not compile a custom Mesa until packaged support has been ruled out.

## 3. Run a known-good TFLite inference

Use a small quantized TFLite model known to be compatible with Teflon.

First test inference without the camera.

Measure:

- successful model initialization
- whether the Teflon delegate is actually selected
- inference latency
- CPU load
- NPU/driver errors

## 4. Stress test

Run repeated inference for at least one hour.

Monitor:

```bash
dmesg -w
```

The NPU phase fails if there are:

- kernel hangs
- etnaviv faults
- GPU/NPU resets
- memory corruption
- reproducible crashes

Do not integrate an unstable NPU path into the camera service.

## Phase 4 acceptance criteria

- inference is demonstrably running through Teflon/etnaviv
- latency is measured
- one-hour stress test is stable
- exact setup is documented

---

# Phase 5 — Bird Detection

Start with a general object detector that contains a `bird` class.

Preferred model properties:

- TFLite
- quantized INT8 if supported
- relatively small input size
- compatible with Teflon
- suitable for low-power continuous inference

Do not train a custom model initially.

## Processing strategy

Do not run full-resolution inference on every video frame.

Use something similar to:

```text
CSI 1080p video
     |
     +------> live stream / recording
     |
     +------> downscaled detection frames
                   |
                 3–10 FPS
                   |
                  NPU
                   |
               bird / no bird
```

Start around 5 FPS detection.

Use configurable values for:

```text
confidence threshold
minimum bird size
detection FPS
trigger duration
release duration
```

Avoid triggering on a single positive frame.

Example event state machine:

```text
IDLE
  |
bird detected for N consecutive frames
  ↓
ACTIVE
  |
bird absent for T seconds
  ↓
IDLE
```

Store detection metadata:

```text
timestamp
confidence
bounding box
event start
event end
```

---

# Phase 6 — Event Recording

Record video around bird events.

Desired behaviour:

```text
continuous ring buffer
        |
        | bird detected
        ↓
save:
    10 s before detection
    event duration
    15–30 s after last detection
```

Do not start the encoder only after detection because the bird's arrival may be lost.

Preferred implementation options:

1. Frigate recording/event pipeline
2. segmented FFmpeg recording with application-controlled retention

Prefer Frigate unless it conflicts with the CSI/NPU pipeline.

Recorded files should use a structure such as:

```text
recordings/
  2026-09-12/
    16-42-05_bird.mp4
```

Also keep a lightweight event index.

---

# Phase 7 — Frigate Integration

Evaluate **Frigate** after CSI streaming and NPU validation work independently.

Desired architecture:

```text
CSI camera
    ↓
go2rtc / FFmpeg
    ├────────────→ Browser live view
    │
    └────────────→ Frigate
                      |
                  motion gate
                      |
                  bird detector
                      |
                     NPU
                      |
              events + recordings
```

Use motion detection to avoid unnecessary object inference.

Configure tracking for:

```yaml
objects:
  track:
    - bird
```

If current Frigate supports the Mesa/Teflon detector on this ARM64 system, configure it only after proving Teflon works outside Frigate.

Do not debug CSI + Frigate + NPU simultaneously.

Integrate in this order:

1. CSI
2. go2rtc
3. NPU standalone
4. Frigate with stream only
5. Frigate object detection
6. recording/events

---

# Phase 8 — Web Interface

For the MVP, use the Frigate web UI.

Required pages/functions:

- live camera
- recent bird events
- event recordings
- timestamp
- detection snapshot if available

Do not write React/Vue frontend until the backend pipeline is stable.

Later, if a custom UI is desired:

```text
custom web frontend
        |
        +---- Frigate HTTP API
        |
        +---- go2rtc WebRTC
```

Keep the backend independent from frontend presentation.

---

# Phase 9 — Services and Autostart

Every production component must run without a logged-in shell.

Use:

- Docker Compose if using Frigate
- systemd for native go2rtc/custom services

Required behaviour:

```text
power on
  ↓
network
  ↓
camera pipeline
  ↓
streaming
  ↓
detector
  ↓
web UI
```

Services must restart after crashes.

Do not use shell sessions, `screen`, or `tmux` as the final deployment mechanism.

---

# Phase 10 — Storage Management

The system will live on eMMC, so recording retention must be bounded.

Do not continuously fill eMMC.

Implement:

- configurable event retention
- maximum disk usage
- automatic deletion of old recordings
- no uncontrolled debug logs

Check storage periodically:

```bash
df -h
du -sh recordings/
```

Avoid writing every inference result or every frame to disk.

---

# Repository Layout

Use a simple repository:

```text
vim3-bird-camera/
├── README.md
├── docs/
│   ├── hardware-baseline.md
│   ├── csi-camera.md
│   ├── npu.md
│   └── streaming.md
├── config/
│   ├── frigate.yml
│   └── go2rtc.yml
├── docker/
│   └── docker-compose.yml
├── scripts/
│   ├── camera-test.sh
│   ├── npu-test.sh
│   └── diagnostics.sh
└── systemd/
```

Do not add application code before it is required.

---

# Implementation Rules for the Agent

1. Work incrementally.
2. Do not make unrelated system changes.
3. Do not replace the kernel unless a concrete blocker is demonstrated.
4. Do not overwrite SPI flash, eMMC bootloader, or Device Tree blindly.
5. Do not assume `/dev/video0`.
6. Do not assume a particular CSI sensor.
7. Do not assume hardware H.264/H.265 encoding is available.
8. Do not assume NPU support just because `/dev/dri/renderD128` exists.
9. Prove that inference actually uses Teflon/etnaviv.
10. Prefer distro packages over manually compiled libraries.
11. Keep the system headless.
12. Record every important command/configuration in the repository.
13. After each phase, stop and verify acceptance criteria before continuing.
14. If something fails, identify the failing layer rather than changing several components at once.

---

# Immediate First Task

Do only Phases 0 and 1 initially.

The immediate goal is:

> Determine exactly how the CSI camera is exposed under the current Armbian kernel and successfully capture a real image from it.

Start by collecting:

```bash
uname -a
cat /etc/os-release
cat /etc/armbian-release

v4l2-ctl --list-devices
media-ctl -p

ls -l /dev/video*
ls -l /dev/media*
ls -l /dev/dri/

dmesg | grep -Ei 'camera|csi|mipi|isp|sensor|video|v4l2|media|etnaviv|npu|galcore'
```

Do not proceed to Frigate, Docker, or ML until CSI capture works reliably.
