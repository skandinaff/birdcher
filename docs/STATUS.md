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
| Working mode | DS1 1920x1080 NV12, 60 fps, bpl 1920, 2073600 + 1036800 |
| Measured | 4195 frames, 59.91 fps, 0 timeouts, 0 short frames, 6 drops (0.14%) |

Verified: cold boot -> FR, cold boot -> DS1, FR->DS1, DS1->DS1, several cycles,
correct NV12 size/stride, no Oops or WARN while streaming.

## What does not work yet

| Item | State |
| --- | --- |
| **Image colour** | Green cast. AWB/AE/AF are open loops — see below. Not a DS1 problem: FR shows the same cast. |
| **Long-run stability** | Unmeasured. Longest run is 70 s. `tools/ds1soak.c` exists; `./ds1soak 2700 1920 1080`. |
| **Streaming / app** | Not started. This is the next phase. |
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
