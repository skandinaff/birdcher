# Task: video streaming infrastructure (Roadmap phases 2-3)

Status: **not started.** This page holds the survey done on 2026-09-15 so the
work can begin from facts rather than assumptions.

Input is settled and stable: `/dev/video1`, third open (stream 2), DS1,
1920x1080 NV12, 60 fps. See [../STATUS.md](../STATUS.md).

## The finding that shapes this phase: the encoders exist, mainline hides them

The roadmap asks whether mainline exposes usable A311D H.264/H.265 encoding.
The answer has two halves, and conflating them is a mistake worth avoiding.

**Mainline 6.18 exposes no encoder.** Measured on the board:

| Evidence | Result |
| --- | --- |
| `CONFIG_VIDEO_MESON_VDEC=m` | decoder only; there is no `VENC` counterpart in mainline |
| `/dev/video0` = `meson-vdec` | M2M whose **output** formats are VP9/HEVC/H264/MPEG1/MPEG2 — compressed in, raw out, i.e. a decoder |
| module search for `venc`/`amvenc` | nothing |

**The silicon has encoders, and Khadas ships drivers for them.** From the
pinned reference (`external/khadas-common_drivers`), in
`arch/arm64/boot/dts/amlogic/mesong12a.dtsi` — the G12A base that G12B/A311D
builds on:

```
amvenc_avc {                      hevc_enc {
    compatible = "amlogic, amvenc_avc";   compatible = "cnm, HevcEnc";
    interrupts = <0 45 1>;                dev_name = "HevcEnc";
    interrupt-names = "mailbox_2";    };
};
```

So there is an Amlogic AVC (H.264) encoder *and* a Chips&Media HEVC encoder
IP, plus a JPEG encoder. The drivers live in
`drivers/media_modules/frame_sink/encoder/{h264,h265,jpeg,multi,vcenc}`;
`h264/encoder.c` is 5350 lines and branches on `MESON_CPU_MAJOR_ID_G12A` /
`GXL`, so G12B is covered.

This is the **same shape as the ISP**: the hardware is real, mainline lacks
it, the vendor has a driver, and a port is possible. The difference is scale —
`media_modules` is 18 MB across 173 `.c` files and is an Amlogic framework in
its own right, not a single driver. The ISP port was already substantial; this
is larger.

**Therefore, for now, treat encoding as software** — but as a deliberate
choice with a known escape hatch, not as a hardware limitation. Do not port
the encoder before software encoding has been measured and shown to be the
actual bottleneck.

What is also available and unused: `CONFIG_VIDEO_MESON_GE2D=m`
(`.../amlogic/meson-ge2d/ge2d.ko`, not currently loaded) — the hardware 2D
engine, useful for scaling and colour-space conversion without CPU. Worth
loading and measuring before writing any CPU-side NV12 conversion.

## Platform budget, measured

| | |
| --- | --- |
| CPU | 6 cores: 2x Cortex-A53 + 4x Cortex-A73, max 2016 MHz |
| RAM | 3.7 GiB total, 3.4 GiB available |
| Disk | 29 GB root, 21 GB free |
| Thermal at idle | 39-41 C, no throttling |
| Installed | **none of** ffmpeg, go2rtc, gstreamer. `ffmpeg` apt candidate is `7:8.0.1-3ubuntu2` |

## Phase 2 — prove capture is stable (do this first)

Roadmap phase 2 asks for >= 30 minutes of continuous capture with drops,
thermals and kernel errors watched. **This has not been done** — the longest
run so far is 70 s.

`tools/ds1soak.c` already does the measuring: frame count, sustained fps,
sequence-gap drops, frozen-frame detection and short frames.

```sh
./ds1soak 2700 1920 1080          # 45 min
watch -n1 'cat /sys/class/thermal/thermal_zone*/temp'
dmesg -w
```

Record: drops, fps stability, thermal curve, and **memory growth** (the open
question — sample `/proc/meminfo` MemAvailable/Slab periodically).

Do not build streaming on top of an unproven capture path.

## Phase 3 — browser live stream

Target from the roadmap:

```
CSI -> V4L2 -> go2rtc -> WebRTC/MSE -> browser
```

Acceptance: live video in a normal browser on the LAN, no desktop environment,
survives 30 minutes, latency and CPU documented, systemd-able later.

### The decision to make before installing anything

With software encoding (see above) the three shapes are:

1. **MJPEG over HTTP.** Simplest path; no H.264 at all. Every browser renders
   it from an `<img>`. Costs bandwidth (roughly 5-10 Mbit/s at 1080p15) and
   has no inter-frame compression, but the encode is cheap and the moving
   parts are few. Good first light.
2. **Software H.264 (x264 `ultrafast` + `zerolatency`) into go2rtc, WebRTC
   out.** What the roadmap prefers and the right long-term shape. Costs real
   CPU at 1080p; that is the budget NPU inference will also want.
3. **Split resolutions.** Preview at 720p or lower for the browser, full
   1920x1080 kept for recording and inference. GE2D can do the downscale.
   Most work, best endgame.

A fourth exists but is a project, not a step: **port `amvenc_avc` from the
vendor BSP.** Keep it in reserve for when measurements justify it.

Note the sensor delivers 60 fps; the roadmap's own target is 15-30. Halving
the frame rate is the cheapest single lever available and should be pulled
before optimising anything else.

### Steps once the shape is chosen

1. `apt install ffmpeg` (and go2rtc, which is a single binary, not a package).
2. Prove one encoded file first: capture N seconds from DS1 to H.264/MJPEG on
   disk and play it back. No network yet.
3. Measure CPU and thermals at the chosen resolution/fps. Decide the budget
   before adding WebRTC.
4. Put go2rtc in front, confirm a browser on the LAN plays it.
5. Only then: 30-minute soak, latency measurement, systemd unit.

## Open questions

- Memory growth over a long capture (phase 2 answers this).
- Does GE2D actually help, or is `ge2d.ko` as unexercised as the rest of the
  vendor media stack on this kernel?
- Does the V4L2 stream survive a client disconnect/reconnect cycle, given the
  stream object is destroyed on close and stream ids are assigned by open
  order? This is a real constraint on any daemon that reopens the device.
