# Status

Updated 2026-09-15.

## What works

The camera stack is up and delivers a production-shaped video input.

```
IMX415 ──CSI-2(4 lane)──> G12B ISP ──> V4L2 /dev/video1
                                        stream 0  FR   3864x2192 NV12
                                        stream 2  DS1  1920x1080 NV12  <- use this
```

| | |
| --- | --- |
| Board | Khadas VIM3, A311D, Armbian 26.8.3 |
| Kernel | `6.18.44-current-meson64`, **stock, no rebuild** |
| Modules | `isp_clkc.ko`, `imx415.ko` (upstream, unmodified), `iv009_isp.ko` |
| Capture node | `/dev/video1`, streams by open order: 0=FR, 1=META, 2=DS1 |
| Working mode | DS1 1920x1080 NV12, bpl 1920, 2073600 + 1036800 |
| Frame rate | **30 fps** (`vertical_blanking=2308`, measured 29.81, 0 drops). A control, not a constant: `135000/fps - 2192`. Runtime only -- resets on reload |
| Controls | 51 verified working (42 ISP + 9 sensor) incl. AWB gains, focus, gains, EV. See [camera/controls.md](camera/controls.md) |
| Measured | 642 s at 60 fps and 240 s at 15 fps, both `frozen=0 short=0 timeouts=0`, no memory drift, no kernel complaints |

Verified: cold boot -> FR, cold boot -> DS1, FR->DS1, DS1->DS1, several cycles,
correct NV12 size/stride, no Oops or WARN while streaming.

## What does not work yet

| Item | State |
| --- | --- |
| **Image colour** | Green cast. AWB/AE/AF are open loops — see below. Not a DS1 problem: FR shows the same cast. |
| **Long-run stability** | Measured to ~11 min, not the 30 in the roadmap. No leak: Slab flat to +-0.2 MB, MemAvailable drifts both ways. Longer runs still welcome. |
| **Streaming / app** | M1 done: DS1 -> MJPEG -> HTTP viewable in a browser. Application layer not started. |
| **Hardware encode** | Silicon has `amvenc_avc` (H.264) + `cnm HevcEnc` (H.265) + JPEG; **mainline exposes none of them**, vendor drivers exist in `media_modules`. Software encode for now. |
| Module reload | Leaks three sysfs attrs (`adapt_frame`, `inject_frame`, `dol_frame`); reload throws duplicate-filename WARNs. Cold boot clean. Cosmetic. |

## The 3A finding, which reframes "image quality"

The ISP's 3A is split between kernel and a **userspace algorithm daemon** over
the sbuf channel. The kernel publishes statistics; userspace is supposed to
compute the result and hand it back. `ae_set_new_param()` is the only caller of
`fsm_raise_event( event_id_ae_result_ready )`, which is the only thing
`AE_fsm_process_event()` handles, which is what eventually programs the sensor.

No daemon, so the loop never closes:

```
ISP kernel driver      OK
IQ tables              present, unverified
statistics generation  OK
userspace 3A daemon    MISSING  ->  AWB never updates, AE partial, AF absent
```

So the next camera-quality task is **not** "fix the green tint". It is: find
the original Khadas/Amlogic userspace 3A stack and establish what AE/AWB/AF
actually depend on it. Khadas Ubuntu 5.15 produced a good image, so the chain
existed — find it, then port or replace deliberately. Do not hand-tune
calibration to compensate for an algorithm that never runs, and do not invent
an AWB.

## Two tracks from here

### Camera controls

51 controls across two surfaces, all set/read-back verified, and the ISP path
confirmed to reach pixels (brightness 16/128/240 -> frame Y mean
0.3/42.7/209.4). Enumerate as JSON with
`platform/camera/tools/vim3-camera-controls.sh`. Full catalogue and UI notes in
[camera/controls.md](camera/controls.md).

### Streaming, measured

DS1 -> MJPEG -> HTTP works (`platform/camera/tools/mjpeg-soak.sh`). ffmpeg
cannot open DS1 itself -- stream ids come from open order -- so
`ds1stream.c` bridges it. Encoded MJPEG at 1920x1080 q7 runs **4.7-5.9
Mbit/s** at 15 fps, ~45 kB/frame.

Running the sensor at the rate actually wanted, rather than 60 fps with
frames discarded, is worth about 15 C:

| | 60 fps, decimated to 15 | 15 fps native |
| --- | --- | --- |
| captured | 59.44 fps | 14.88 fps |
| drops | 379 (171 in minute 1) | 29 |
| CPU avg | 26.6 % | 19.9 % |
| hottest zone | **65.6 C** peak | **50.6 C** peak |

The CPU saving is modest because MJPEG encoding dominates and that happens at
15 fps either way. The thermal saving is large because the sensor and ISP stop
doing 4x the readout and processing.

Browser viewing is **verified**: `Content-Type:
multipart/x-mixed-replace;boundary=ffmpeg`, 116 JPEG frames pulled in 8 s
(14.5 fps, 5.6 MB) from another machine over WiFi. ffmpeg's `-f mpjpeg`
defaults to `application/octet-stream`, which a browser downloads instead of
displaying, so `-content_type` has to be set explicitly.
`platform/camera/tools/verify-preview.sh <secs>` serves it and writes its
verdict to `/tmp/preview-verify.txt`.

- **Track A (primary)** — the Birdcher application on the DS1 input:
  capture -> live preview -> recording -> frame distributor -> NPU inference.
  See [tasks/2026-09-16-streaming-infrastructure.md](tasks/2026-09-16-streaming-infrastructure.md).
- **Track B (secondary)** — camera quality via the 3A investigation above.

Track A is not blocked on Track B. The scaler is finished; leave it alone.

## History

DS1 bring-up took three bugs, all found by diffing against the Khadas
reference: Temper left in the datapath doing DMA against address 0, the CSI-2
receive path never brought back up for a DS1-only stream, and the ISP being
quiesced but never re-armed. Full trail in
[tasks/2026-09-15-ds1-1080p-handover.md](tasks/2026-09-15-ds1-1080p-handover.md).
