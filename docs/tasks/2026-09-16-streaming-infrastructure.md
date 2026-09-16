# Video streaming infrastructure: survey and implemented preview

Updated 2026-09-16. DS1 stability checks and the MJPEG browser preview have
been completed since the original 2026-09-15 survey. This page records what
was measured and what remains a production follow-up; the next active
milestone is M2 NPU proof.

Input is settled and stable: `/dev/video1`, third open (stream 2), DS1,
1920x1080 NV12. The sensor defaults to 60 fps; preview startup selects 30 fps.
See [../STATUS.md](../STATUS.md).

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
| Encoder in use | `ffmpeg` software MJPEG; go2rtc is not part of the current preview |

## Phase 2 — DS1 stability checked

The original survey preceded the completed tests. Results preserved on the
board under `~/birdcher-tools/soak-run/` and
`~/birdcher-tools/soak-15fps-clean/`:

| Test | Capture result |
| --- | --- |
| 60 fps sensor, 15 fps MJPEG output, 642 s | 38,185 frames, 59.44 fps, 379 sequence-gap drops, 0 frozen/short frames/timeouts |
| 15 fps native sensor and output, 240 s | 3,571 frames, 14.88 fps, 29 drops, 0 frozen/short frames/timeouts |
| 30 fps sensor check | 29.81 fps, 0 drops |

The 642-second run showed no memory leak (Slab within ±0.2 MB) or kernel
Oops/WARN while streaming. Running the sensor at native 15 fps instead of 60
fps lowered the hottest SoC reading from 65.6 °C to 50.6 °C. These checks
establish a working capture path for further development. The original ≥30
minute endurance target has not been demonstrated; keep it as a production
qualification task, not as a reason to repeat DS1 bring-up before M2.

`platform/camera/tools/ds1soak.c` remains available for a later dedicated
capture endurance run; `soak-monitor.sh` samples CPU, memory and thermals.

## Phase 3 — browser preview working

The implemented path is:

```text
IMX415 → CSI/ISP → DS1 NV12 → ds1stream → ffmpeg MJPEG → HTTP → browser
```

`preview-ctl.sh start` applies the 30 fps sensor rate and measured indoor
exposure/gain profile, then starts a transient systemd unit. A LAN browser can
open `http://192.168.1.38:8090/`; HTTP returns multipart MJPEG with the
correct content type. A disconnect can reconnect. The server supports one
viewer at a time. 1080p15 MJPEG measured 4.7–5.9 Mbit/s. See
[2026-09-15-streaming-handover.md](2026-09-15-streaming-handover.md).

The original go2rtc/WebRTC design has not been implemented. It is a possible
upgrade when fan-out, bandwidth or latency requirements justify it. A
30-minute production-stream soak and a measured end-to-end latency/CPU budget
remain open, but neither blocks the independent M2 NPU proof.

### Transport choices for later product work

With software encoding (see above) the three shapes are:

1. **MJPEG over HTTP.** Implemented first light. Every browser renders it from
   an `<img>`; it costs measured bandwidth and currently serves one viewer.
2. **Software H.264 (x264 `ultrafast` + `zerolatency`) into go2rtc, WebRTC
   out.** What the roadmap prefers and the right long-term shape. Costs real
   CPU at 1080p; that is the budget NPU inference will also want.
3. **Split resolutions.** Preview at 720p or lower for the browser, full
   1920x1080 kept for recording and inference. GE2D can do the downscale.
   Most work, best endgame.

A fourth exists but is a project, not a step: **port `amvenc_avc` from the
vendor BSP.** Keep it in reserve for when measurements justify it.

The sensor now runs at the requested rate through `vertical_blanking`; 60 fps
is only its reset default. This control must be reapplied by each camera owner.

### If a different transport becomes necessary

1. Measure the actual limitation of the MJPEG preview under the intended load.
2. Prove an encoded file and measure software H.264 CPU/thermal cost at the
   chosen resolution and native sensor rate.
3. Add go2rtc/WebRTC only if the measured trade-off is worthwhile; then check
   browser playback, reconnection, latency and a 30-minute production soak.

## Open questions

- Production endurance beyond the measured 642 s and final-stream latency.
- Does GE2D actually help, or is `ge2d.ko` as unexercised as the rest of the
  vendor media stack on this kernel?
- How should a future multi-viewer service own and distribute one V4L2 stream?
  The current preview restarts its single-client encoder after disconnect.
