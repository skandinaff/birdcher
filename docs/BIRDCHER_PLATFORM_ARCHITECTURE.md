# Birdcher + VIM3 Vision Platform Architecture

Status: architecture draft
Target hardware: Khadas VIM3 / Amlogic A311D / Khadas A1019 IMX415 camera
Target OS: Armbian, modern mainline-oriented kernel stack

## 1. Purpose

The project has two related but distinct goals:

1. Build **Birdcher**, an autonomous bird-observation appliance with live video, local neural-network inference, audio, event recording, telemetry, and a browser UI.
2. Build a **reusable computer-vision platform for Khadas VIM3** so the same Armbian installation, camera stack, microphone, NPU runtime, diagnostics, and development tools can be reused for unrelated vision experiments without depending on Birdcher.

This separation should be established now, before the application layer grows around board-specific implementation details.

Birdcher is an application **on top of** the platform. It must not become the platform itself.

---

## 2. High-level architecture

```text
┌─────────────────────────────────────────────────────────────┐
│                     Birdcher application                    │
│                                                             │
│  live UI · detection · tracking · events · recording       │
│  species classification · configuration · event database    │
└──────────────────────────────┬──────────────────────────────┘
                               │ stable platform interfaces
┌──────────────────────────────▼──────────────────────────────┐
│                  VIM3 Vision Platform                      │
│                                                             │
│ camera · audio · NPU · telemetry · diagnostics · tooling   │
│ reusable outside Birdcher                                  │
└──────────────────────────────┬──────────────────────────────┘
                               │ Linux APIs
┌──────────────────────────────▼──────────────────────────────┐
│                     Armbian / Linux                         │
│                                                             │
│ V4L2/media · ALSA · DRM/etnaviv · thermal · networking     │
└──────────────────────────────┬──────────────────────────────┘
                               │ hardware
┌──────────────────────────────▼──────────────────────────────┐
│ Khadas VIM3 / A311D                                         │
│                                                             │
│ IMX415 · ISP · I2S · NPU · CPU/GPU · storage · network     │
└─────────────────────────────────────────────────────────────┘
```

The platform layer should remain useful if Birdcher is removed completely.

---

## 3. Architectural rules

### 3.1 Khadas-first rule for camera/ISP work

Before inventing a fix, diff against Khadas `common_drivers/armisp-g12b`.

- Khadas defines expected G12B/VIM3 behaviour.
- Linux 6.18 defines the current kernel/API contracts.
- Radxa is only a forward-port reference and must not be treated as authority for VIM3 wiring, clocks, calibration, lens behaviour, GPIOs, or ISP semantics.

### 3.2 Application code must not know vendor internals

Birdcher should know about:

- V4L2 camera frames;
- ALSA audio;
- inference APIs;
- telemetry APIs;
- files/database;
- network APIs.

Birdcher should not need to know about:

- `iv009_isp` internals;
- `isp_clkc` details;
- G12B register addresses;
- stream bring-up quirks;
- clock muxes;
- kernel module load ordering.

Those belong to the platform.

### 3.3 One lifetime owner for the camera

For the current Amlogic/Khadas ISP driver this is a **hard constraint of the vendor V4L2 ABI**, not merely an application design preference.

The driver exposes several logical ISP streams through one video node and assigns `stream_id` by file-open order. With the current layout the relevant identities are:

```text
first open   → stream 0 → FR
second open  → stream 1 → META
third open   → stream 2 → DS1
```

Closing a handle releases that stream slot and deinitializes the associated stream object. Therefore stream identity is not encoded in the device-node name and must not be inferred by an independent process opening `/dev/video1`.

The platform camera layer must hide this ABI quirk. A long-lived `CameraSession`/`CameraCapture` component should open and retain all required handles for its lifetime, map them to stable platform-level names such as `FR`, `META`, and `DS1`, and distribute frames internally:

```text
/dev/video1
    ↓
CameraSession
    ├── FR handle
    ├── META handle
    └── DS1 handle
           ↓
      FrameDistributor
           ├── Preview
           ├── Detector
           ├── Recorder
           └── Diagnostics
```

This is why a generic tool such as `ffmpeg -i /dev/video1` can silently open the first stream and receive FR rather than DS1.

A second process must not independently open the ISP node while the platform service owns it. Standalone diagnostics may do so only when they have exclusive ownership, for example with the Birdcher/platform camera service stopped.

This requirement is specific to the current Amlogic/Khadas driver behaviour; it is not a general property of V4L2.

### 3.4 Slow consumers must never block capture

Each consumer gets a bounded queue with an explicit policy.

- Detection: latest-frame policy; old frames may be dropped.
- Preview: latest-frame policy.
- Recording: sequential policy; backpressure must be isolated from capture.
- Diagnostics: sampling policy.

No unbounded frame queues.

### 3.5 Reusable platform first, Birdcher-specific policy above it

Generic functionality belongs in the platform when it could plausibly be useful to another vision project.

Examples that belong in the platform:

- open camera;
- enumerate modes/controls;
- capture frames;
- read audio;
- run an NPU model;
- read SoC temperature;
- benchmark capture/inference;
- inspect driver state;
- install kernel modules.

Examples that belong in Birdcher:

- bird detection threshold;
- feeder zones;
- bird visit state machine;
- species aggregation;
- event retention policy;
- bird-event UI.

---

# Part I — Reusable VIM3 Vision Platform

## 4. Platform scope

The reusable platform should provide:

```text
Armbian image / host setup
├── camera kernel stack
├── camera configuration and diagnostics
├── I2S microphone support
├── NPU runtime and model tooling
├── generic capture/inference utilities
├── system telemetry helpers
├── package/install scripts
└── reproducible documentation
```

The target outcome is that a fresh VIM3 can become a usable computer-vision development board without installing Birdcher.

---

## 5. Platform components

### 5.1 Camera kernel stack

Current target:

```text
IMX415
  ↓ MIPI CSI-2 RAW10
A311D CSI receiver
  ↓
A311D ISP
  ├── FR  3864×2192
  └── DS1 1920×1080 NV12
  ↓
V4L2
```

The camera stack must be packaged independently of Birdcher.

Deliverables:

- out-of-tree kernel modules;
- required DT configuration/overlay mechanism;
- deterministic boot-time loading through real dependencies;
- no Birdcher-specific assumptions;
- package version/provenance information;
- uninstall path;
- diagnostics command.

Known working baseline:

- IMX415 detected after cold boot;
- MIPI 4 lanes;
- FR capture works;
- DS1 1920×1080 works;
- repeated DS1/FR runs work;
- no kernel rebuild required.

#### Stream ownership and handle topology

The platform API must present stable stream identities even though the underlying driver assigns streams by open order. Application code must request `FR`, `META`, or `DS1` from the platform rather than opening `/dev/video1` itself.

The platform implementation is responsible for opening the underlying handles in the required order and keeping them alive for as long as the camera session exists.

#### Frame rate is a first-class camera setting

Frame rate must be represented explicitly in the platform camera API and configuration. It must not be left to the IMX415 sensor's current power-on/default behaviour.

Current bring-up evidence shows the sensor can otherwise run at 60 fps, while the IMX415 subdevice `vertical_blanking` control can be used to select a lower effective frame rate. A tested example is a VBLANK setting of 6808 producing approximately 15 fps in the current mode.

The platform should expose a semantic control such as:

```text
CameraConfig
    stream = DS1
    width = 1920
    height = 1080
    pixel_format = NV12
    frame_rate = 15 | 30 | 60 | supported value
```

The implementation detail may currently map that request to sensor-subdevice VBLANK, but callers must not depend on that mechanism.

The platform must not capture at 60 fps merely to discard most frames later. Camera rate, preview rate, inference sampling rate, and recording rate are separate concepts:

```text
sensor / capture rate    explicit camera setting
preview rate             consumer policy
inference rate           consumer sampling policy
recording rate           event/recording policy
```

For Birdcher, the final default capture rate should be chosen from measured image-quality, motion, thermal, CPU, NPU, and recording trade-offs rather than inherited from the sensor. Initial testing should explicitly compare at least 15 fps and 30 fps.

### 5.2 Camera userspace utility

Provide a small generic CLI, for example:

```text
vim3-camera-info
vim3-camera-capture
vim3-camera-controls
vim3-camera-soak
```

Expected capabilities:

```text
vim3-camera-info
  list node(s)
  sensor name
  current format
  supported formats
  exposure/gain/focus controls
  frame statistics

vim3-camera-capture
  --stream ds1
  --width 1920
  --height 1080
  --frames 100
  --output frame.nv12

vim3-camera-soak
  --duration 3600
  --report report.json
```

These tools should work without Birdcher.

### 5.3 Camera image-quality subsystem

Image transport is considered separate from image quality.

Still open:

- AE behaviour and convergence;
- AWB;
- CCM/tuning;
- DW9714 autofocus;
- possible Khadas/Amlogic userspace 3A daemon dependency;
- calibration provenance and suitability.

Do not hand-tune image quality until the original Khadas 3A path has been understood.

The Khadas 5.15 BSP should be treated as the primary reference for how AE/AWB/AF were expected to operate.

### 5.4 Camera temperature investigation

This is now a motivated hardware/telemetry task rather than a speculative feature. External thermal-camera observation has already shown that the camera module has meaningful self-heating during operation.

Linux SoC thermal zones are already available and should be exposed independently. The open question is specifically whether the A1019 module, IMX415 sensor, or another device on the camera module exposes a documented die/module temperature that software can read.

Do not assume such a register exists merely because the sensor datasheet specifies operating-temperature ranges, and do not use undocumented register guesses.

Platform task:

1. inspect Sony IMX415 register documentation;
2. inspect Khadas IMX415 vendor driver;
3. inspect A1019 schematic/BOM if available;
4. enumerate all devices on the camera I2C bus;
5. determine whether a documented die/module temperature source exists;
6. if it exists, establish units, accuracy, update rate, valid operating range, and whether reads are safe during streaming.

The telemetry API should distinguish the sources explicitly:

```text
soc_temperature_c       required
camera_temperature_c    optional capability
```

If no documented camera temperature is available, report that capability as unavailable rather than synthesizing a value from SoC temperature.

---

## 6. I2S audio platform

### 6.1 Goal

Support a generic digital I2S MEMS microphone independently of Birdcher.

Suggested initial class of microphones:

- INMP441;
- ICS-43434;
- SPH0645-class devices.

Prefer a microphone that does not require an external MCLK.

Expected signals:

```text
BCLK
LRCLK
DATA
3.3 V
GND
```

Initial target configuration:

```text
48 kHz
mono
16- or 24-bit PCM
```

Exact sample format should follow the selected microphone and ALSA driver support.

### 6.2 ALSA abstraction

Platform utility examples:

```text
vim3-audio-info
vim3-audio-capture
vim3-audio-loopback-test
```

Birdcher should see a normal ALSA PCM source and not care about pinmux or DT details.

### 6.3 Timestamping

Audio blocks must be timestamped using a monotonic clock compatible with video timestamping.

Do not synchronize A/V using wall-clock time.

---

## 7. NPU platform

### 7.1 Hardware

A311D contains a VeriSilicon/Vivante-family NPU (VIPNano-QI.7120 class), exposed through the modern open-source stack using etnaviv and Mesa Teflon.

The platform target is:

```text
LiteRT / TFLite model
        ↓
Teflon delegate
        ↓
Mesa
        ↓
etnaviv
        ↓
A311D NPU
```

Avoid making proprietary Acuity/KSNN `.nb` tooling a required dependency for the platform.

### 7.2 Generic NPU API

Provide a reusable inference abstraction:

```text
InferenceEngine
    loadModel()
    inspectModel()
    infer()
    benchmark()
```

Initial implementations:

```text
TfliteTeflonEngine
TfliteCpuEngine
```

The CPU backend is required for:

- correctness comparison;
- development on non-VIM3 hosts;
- fallback;
- debugging unsupported Teflon operators.

### 7.3 Generic NPU tools

Examples:

```text
vim3-npu-info
vim3-npu-run model.tflite input.bin
vim3-npu-benchmark model.tflite
vim3-npu-compare model.tflite
```

`vim3-npu-compare` should compare CPU and NPU outputs with configurable tolerance.

### 7.4 First validated model set

Before Birdcher-specific training, validate known-supported quantized models.

Preferred baseline:

- MobileNet V2 UINT8;
- SSDLite MobileDet UINT8.

This establishes that the NPU platform is healthy before debugging custom bird models.

---

## 8. Generic frame-processing layer

Provide a small platform library for reusable frame operations:

```text
Frame
FramePool
FrameQueue
FrameConverter
FrameScaler
FrameTimestamp
```

A `Frame` should contain at minimum:

```text
frame_id
monotonic timestamp
width
height
pixel format
plane metadata
buffer ownership/reference
```

Avoid unconditional copies into `std::vector<uint8_t>`.

The design should allow later zero-copy improvements without changing high-level APIs.

---

## 9. Platform telemetry

Generic telemetry should include:

- SoC temperature;
- CPU load;
- RAM usage;
- disk usage;
- camera FPS;
- dropped frames;
- camera exposure/gain where available;
- NPU inference time;
- NPU inference rate;
- audio level/clipping;
- uptime;
- software and driver versions.

Suggested output format for CLI/API: JSON.

---

## 10. Platform packaging

The reusable platform should be installable independently of Birdcher.

Suggested package split:

```text
vim3-vision-camera
vim3-vision-audio
vim3-vision-npu
vim3-vision-tools
vim3-vision-dev
```

A single meta-package may later depend on all of them:

```text
vim3-vision-platform
```

Do not require `.deb` packaging immediately if it slows development. First establish clean install/uninstall scripts and explicit file ownership, then convert those into packages.

Every installable component must state:

- files installed;
- modules loaded;
- configuration changed;
- services created;
- uninstall procedure;
- required kernel/Armbian version.

---

## 11. Platform repository layout

Suggested structure:

```text
platform/
├── camera/
│   ├── kernel/
│   ├── config/
│   ├── tools/
│   └── docs/
├── audio/
│   ├── config/
│   ├── tools/
│   └── docs/
├── npu/
│   ├── runtime/
│   ├── tools/
│   ├── models/
│   └── docs/
├── common/
│   ├── frames/
│   └── telemetry/
├── packaging/
├── scripts/
└── tests/
```

Khadas source should remain a pinned read-only reference, not active production code unless a file is explicitly ported into the platform.

---

# Part II — Birdcher Application

## 12. Birdcher runtime architecture

Primary service:

```text
birdcherd
```

Recommended language: C++.

Responsibilities:

```text
birdcherd
├── CameraCapture
├── FrameDistributor
├── Inference
├── Tracker
├── EventEngine
├── AudioCapture
├── Recorder
├── CameraControl
├── Telemetry
├── Storage
└── HTTP/WebSocket API
```

Prefer one main service with clear internal modules over many small processes during the 0→1 phase.

Use systemd for lifecycle management.

---

## 13. Camera usage in Birdcher

Primary operating stream:

```text
DS1
1920×1080
NV12
```

FR is optional for:

- high-resolution still capture;
- diagnostic capture;
- high-quality event snapshot.

Birdcher must consume the platform camera API and must not contain board-specific setup logic.

---

## 14. FrameDistributor

The distributor provides explicit consumers:

```text
CameraCapture
     ↓
FrameDistributor
  ├── PreviewQueue
  ├── DetectionQueue
  ├── RecordingRing
  └── DiagnosticsQueue
```

Policies:

- Preview: latest frame wins.
- Detection: latest frame wins.
- Recording: preserve order.
- Diagnostics: sample periodically.

A stalled consumer must not stall V4L2 capture.

---

## 15. Neural-network strategy

### 15.1 Do not make YOLO the first dependency

The first target is not “run YOLO somehow”.

The first target is a stable, measurable NPU pipeline using models already known to fit the current Teflon/etnaviv stack.

### 15.2 Phase ML-0 — stock bird detector

Use a quantized COCO detector such as SSDLite MobileDet UINT8.

COCO already contains a `bird` class.

Target flow:

```text
IMX415 DS1 1920×1080
        ↓
resize / letterbox
        ↓
~320×320 UINT8
        ↓
SSDLite MobileDet
        ↓
A311D NPU
        ↓
bird bounding boxes
```

Target initial inference rate:

```text
~10 FPS
```

The video stream may continue at 25/30 FPS.

There is no requirement to run inference on every frame.

### 15.3 Tracking

Add a simple IoU tracker.

A track contains:

```text
track_id
first_seen
last_seen
bbox history
confidence history
```

Do not begin with a neural tracker.

### 15.4 Species classification

Second-stage classifier:

```text
bird bbox
   ↓
crop
   ↓
224×224
   ↓
MobileNet V2 UINT8
   ↓
species probabilities
```

Run classification on a small number of good frames per track rather than every frame.

Aggregate classifications across the track.

### 15.5 Later YOLO evaluation

YOLO may be evaluated later if:

- Teflon supports the exported graph sufficiently;
- accuracy is materially better;
- latency remains acceptable;
- integration cost is justified.

YOLO is an R&D/optimization branch, not a prerequisite for Birdcher 0→1.

---

## 16. Event engine

State machine:

```text
IDLE
  ↓ bird candidate
CANDIDATE
  ↓ confidence/time condition
ACTIVE
  ↓ bird missing
GRACE
  ↓ timeout
FINISH
```

Configurable parameters:

- minimum consecutive detections;
- detector confidence;
- grace period;
- pre-roll duration;
- post-roll duration;
- detection zones.

---

## 17. Detection zones

The UI should allow the user to draw one or more polygons over the image.

Events may be limited to detections intersecting the configured feeder zone.

This is useful for reducing false events from birds flying through the background.

---

## 18. Audio in Birdcher

```text
I2S microphone
   ↓
ALSA
   ↓
AudioCapture
   ↓
Audio ring buffer
   ↓
EventRecorder
```

Audio must not be required for video/detection functionality.

If audio fails, Birdcher continues operating without audio and exposes the fault in telemetry.

---

## 19. A/V synchronization

Use monotonic timestamps for video frames and audio blocks.

Wall-clock timestamps are metadata only.

```text
video frame: monotonic 123.400 s
audio block: monotonic 123.395–123.405 s
```

The recorder aligns media using monotonic time.

---

## 20. Recording

Maintain short ring buffers so an event can include footage from before the detector fired.

```text
video ring: ~5 s
audio ring: ~5 s
```

On event start:

1. write pre-roll;
2. append live frames/audio;
3. continue through grace/post-roll;
4. finalize event media.

Codec decisions should be measurement-driven.

For development:

- MJPEG is acceptable for live preview;
- efficient H.264/H.265 becomes important for long recordings, bandwidth, and CPU/NPU contention.

Do not make codec work block inference/application development.

---

## 21. Storage

Suggested layout:

```text
/var/lib/birdcher/
├── events/
│   └── YYYY/MM/DD/
├── snapshots/
└── birdcher.db
```

Use SQLite initially.

Event record fields:

```text
id
start_time
end_time
species
species_confidence
detector_confidence
snapshot_path
video_path
audio_present
metadata_json
```

---

## 22. Web architecture

### 22.1 Frontend

Recommended:

```text
React
TypeScript
Vite
```

Node.js is used for frontend development/build tooling.

Do not require a permanent Node.js server in production unless a later requirement justifies it.

Production flow:

```text
npm build
   ↓
static HTML / JS / CSS
   ↓
birdcherd serves assets
```

### 22.2 Backend

`birdcherd` provides HTTP APIs and realtime updates.

Suggested endpoints:

```text
/api/v1/status
/api/v1/camera
/api/v1/camera/controls
/api/v1/detections
/api/v1/events
/api/v1/events/:id
/api/v1/config
/api/v1/system
```

Realtime telemetry/detections:

- WebSocket, or
- Server-Sent Events if one-way updates are sufficient.

### 22.3 Live preview

Initial diagnostic path:

```text
DS1 NV12
   ↓
MJPEG
   ↓
HTTP
   ↓
browser
```

This is a diagnostic and early-product transport, not a permanent architectural constraint.

### 22.4 Detection overlay

Keep video clean.

Send detection metadata separately:

```text
x
y
w
h
class
confidence
track_id
```

The browser draws overlays using Canvas or SVG.

---

## 23. Web UI screens

### Live

- live video;
- bounding boxes;
- track/species label;
- audio level;
- FPS;
- exposure/gain;
- temperature;
- NPU latency.

### Events

- chronological event list;
- thumbnail;
- timestamp;
- species;
- confidence;
- duration.

### Event detail

- event video;
- audio;
- best frame;
- classification results;
- detection history.

### Camera

- AE on/off;
- exposure;
- gain;
- AWB state;
- focus;
- FR snapshot;
- 3A diagnostics later.

### System

- CPU temperature/load;
- RAM/disk;
- camera status;
- NPU status;
- audio status;
- versions;
- logs.

### Settings

- detection threshold;
- event timeout;
- pre/post roll;
- detection rate;
- recording options;
- detection zones;
- retention policy.

---

## 24. Camera quality work remains active

Camera bring-up is not considered complete merely because frames arrive.

Open camera-quality tasks:

```text
AE behaviour
AWB
CCM / colour
DW9714 autofocus
3A userspace integration
long-run stability
possible sensor/module temperature
```

These can proceed in parallel with Birdcher application development.

The live-preview system should become the main observability tool for camera-quality work.

---

## 25. Failure isolation

Required behaviour:

```text
NPU failure       → preview continues
browser disconnect → detection continues
recorder slow      → camera capture continues
audio missing      → video/detection continue
classifier failure → bird detection continues
```

Hardware-facing modules must expose health state without bringing down unrelated functionality.

---

## 26. Configuration

Suggested file:

```text
/etc/birdcher/birdcher.toml
```

Sections:

```text
[camera]
[inference]
[audio]
[events]
[recording]
[web]
[storage]
[telemetry]
```

Do not scatter product policy across hard-coded constants.

---

## 27. Birdcher repository structure

```text
birdcher/
├── app/
│   ├── camera/
│   ├── frames/
│   ├── inference/
│   ├── tracking/
│   ├── events/
│   ├── audio/
│   ├── recording/
│   ├── telemetry/
│   ├── storage/
│   └── web_api/
├── web/
│   ├── src/
│   ├── package.json
│   └── vite.config.ts
├── models/
│   ├── detector/
│   └── classifier/
├── config/
├── tests/
├── tools/
├── docs/
└── external/
```

The reusable VIM3 platform may live in the same monorepo initially, but its source, packages, APIs, and documentation must remain logically independent from Birdcher.

---

# Part III — Development Plan

## 28. Milestones

### M0 — Camera transport

```text
IMX415 → CSI → ISP → FR/DS1 → V4L2
```

Status: complete. DS1 1920×1080 NV12 works across cold boot and restart.

### M1 — Live observability

```text
DS1 → MJPEG → browser
```

Status: complete for development. MJPEG preview works in a LAN browser,
`preview-ctl.sh` manages its systemd unit, and camera controls are enumerated.
The completed DS1 runs reached 642 s at 60 fps and 240 s at native 15 fps;
the longer production-endurance target is tracked separately in the roadmap.

### M2 — Platform NPU proof

Status: in progress. Packaged Mesa Teflon runs MobileNet V1 on the NPU;
CPU/NPU comparison is measured and the endurance run is pending.

Run a known-supported quantized model through:

```text
TFLite/LiteRT → Teflon → etnaviv → A311D NPU
```

Compare CPU/NPU outputs and benchmark latency.

### M3 — Reusable platform baseline

Establish generic camera/audio/NPU tools and clean install scripts.

At this point another unrelated CV program should be able to use the board without Birdcher.

### M4 — Birdcher core service

Implement:

- `birdcherd`;
- CameraCapture;
- FrameDistributor;
- telemetry;
- HTTP API;
- basic React UI.

### M5 — Bird detection

Add:

- SSDLite MobileDet;
- NPU inference;
- bird detection;
- simple tracker;
- live bbox overlay.

### M6 — Audio

Bring up:

```text
I2S → ALSA → AudioCapture
```

Add browser audio diagnostics and A/V timestamp validation.

### M7 — Events and recording

Implement:

- EventEngine;
- pre/post-roll;
- event DB;
- snapshots;
- synchronized video/audio recording.

### M8 — Camera quality

Complete:

- AE/AWB investigation;
- Khadas 3A path;
- DW9714 autofocus;
- calibration verification;
- long-run camera stability.

This work may begin earlier and proceed in parallel.

### M9 — Species classifier

Train/adapt MobileNet V2 bird classifier and integrate track-level classification.

### M10 — Optimization

Only after measurement:

- hardware H.264/H.265;
- reduced-copy/zero-copy paths;
- model optimization;
- alternate detectors such as YOLO;
- CPU/NPU scheduling improvements.

---

## 29. Definition of platform success

The VIM3 vision platform is successful when, without installing Birdcher, a developer can:

1. install the platform on supported Armbian;
2. capture stable IMX415 FR/DS1 frames;
3. inspect camera controls and telemetry;
4. record from an I2S microphone;
5. run a quantized TFLite model on the A311D NPU;
6. compare NPU and CPU model output;
7. access reproducible diagnostics and soak tests;
8. build an unrelated computer-vision application using documented Linux/platform interfaces.

---

## 30. Definition of Birdcher 0→1 success

Birdcher 0→1 is successful when the device can autonomously:

1. boot into a healthy camera/audio/NPU state;
2. show live 1080p video in a browser;
3. detect birds locally on the NPU;
4. track a visit as one event;
5. retain pre/post-roll video and audio;
6. store the event and metadata locally;
7. show event history in the web UI;
8. continue operating without cloud connectivity.

Species-level recognition, optimal codecs, perfect ISP tuning, upstream-quality drivers, and every possible camera mode are not prerequisites for 0→1.
