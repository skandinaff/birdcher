# VIM3 Vision Platform

Reusable computer-vision platform for the Khadas VIM3 / Amlogic A311D. It must
stay useful with Birdcher removed entirely — see
[../docs/BIRDCHER_PLATFORM_ARCHITECTURE.md](../docs/BIRDCHER_PLATFORM_ARCHITECTURE.md).

```
platform/
├── camera/tools/   capture, streaming and soak tools for the IMX415/ISP stack
├── scripts/        host/build tooling
└── (audio/, npu/, common/ to follow — created when they have content)
```

The camera **kernel** stack is the `external/radxa-zero2pro-camera` submodule
(`isp_clkc.ko`, `imx415.ko`, `iv009_isp.ko`), built against the running Armbian
kernel. No kernel rebuild is required; see `docs/kernel/build-provenance.md`.

## Two things application code must never have to know

These are the board specifics the platform exists to hide (architecture §3.2,
§13). Both have already cost real debugging time.

**1. Stream identity comes from open order, not from a device node.**
`/dev/video1` serves three streams, and the id is assigned by the order in
which the node is opened: **0 = FR (3864x2192), 1 = META, 2 = DS1
(1920x1080)**. A single `open()` therefore lands on FR — which is why
`ffmpeg -i /dev/video1` silently gives 3864x2192 instead of DS1. A consumer
must hold all three handles open for as long as it streams, because the stream
object is destroyed on close. `camera/tools/ds1stream.c` is the worked example.

This is also why the architecture's "one owner for the camera" rule (§3.3) is a
hard constraint rather than a preference: a second process opening the node
gets different stream identities.

**2. Frame rate is a control, and 60 fps is the wrong default.**
The sensor comes up at 60 fps. The imx415 subdev's `vertical_blanking` control
sets the frame period and the ISP does not override it:

```
total_lines = 135000 / target_fps        # sensor runs 135000 lines/s
vertical_blanking = total_lines - 2192   # 6808 => 15 fps (max 1046383)
```

Capturing at 60 and discarding frames costs roughly 4x the sensor readout, ISP
work, power and heat for identical delivered output. Ask for the rate you want.

Nothing here persists: a module reload or reboot returns the sensor to 60 fps
and every ISP control to its default. `preview-ctl.sh start` re-applies the
frame rate for that reason; anything else that cares must do the same.

## Preview

```sh
preview-ctl.sh start [seconds] [fps]   # also sets the sensor rate
preview-ctl.sh stop
preview-ctl.sh restart
preview-ctl.sh status
```

It runs as a transient systemd unit, because a backgrounded ssh command does
not reliably survive this board's WiFi. `ffmpeg -listen 1` serves one client at
a time, so `serve-mjpeg.sh` restarts the pipeline per client — a browser
refresh reconnects instead of killing the viewer. It is still a diagnostic
viewer, not a service: one viewer at a time, and a fan-out transport is a later
measurement-driven decision.

## Tools

| Tool | Purpose |
| --- | --- |
| `ds1stream.c` | DS1 -> stdout as raw NV12, for piping into an encoder |
| `ds1run.c` | DS1 capture with optional raw dump |
| `ds1soak.c` | long-run DS1 capture stability measurement |
| `frds1.c` | FR streaming with DS1 configured but never armed (bring-up diagnostic) |
| `ds1arm.c` | DS1 negotiate/REQBUF/QBUF, stops before STREAMON (bring-up diagnostic) |
| `serve-mjpeg.sh` | DS1 -> MJPEG -> HTTP viewer, re-listens per client |
| `preview-ctl.sh` | start / stop / restart / status for the preview |
| `verify-preview.sh` | self-contained check that the HTTP path serves a client |
| `vim3-camera-controls.sh` | every camera control as JSON, with derived fps |
| `mjpeg-soak.sh` | DS1 -> MJPEG -> HTTP, instrumented as a soak |
| `soak-monitor.sh` | CPU/memory/thermal/network sampler, TSV out |
| `soak-report.py` | summarises a soak directory |

Built on the board with `gcc -O2 -o X X.c`. They are deliberately plain V4L2
with no dependencies, so they work on a bare Armbian install.

These will become the `vim3-camera-*` CLIs of architecture §5.2 once there is
an install path; the names are kept as-is until then to avoid churning the
build and deploy commands mid-bring-up.
