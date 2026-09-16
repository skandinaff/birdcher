# Architecture review handover — 2026-09-16

## Review brief

Birdcher is a planned headless bird-observation appliance on a Khadas VIM3
(A311D) with an IMX415 CSI camera. The reusable VIM3 vision platform and the
Birdcher application are intended to remain separate. This document records
what the current development session actually demonstrated, where the live
prototype falls short of that architecture, and the decisions needed before
building the application service. It is an evidence snapshot, not a claim that
bird detection or production streaming is complete.

**Current assessment:** camera capture and browser viewing are usable for
development; a quantized classifier really runs on the A311D NPU and is much
faster than its CPU equivalent. The available object detector does *not* yet
produce usable boxes through Teflon on this board. The live boxed-ball demo
therefore localizes candidates on the CPU and classifies a crop on the NPU.
Video capture reaches about 30 fps while independent box updates arrive about
three times per second. Moving localization to a validated NPU detector is the
main technical question for the next stage.

## What was done and verified

| Area | Result and evidence | Status |
| --- | --- | --- |
| CSI/ISP capture | IMX415 → G12B ISP → DS1 `/dev/video1`, 1920×1080 NV12. Cold boot, FR/DS1 switching and repeated starts worked. Development soaks: 642 s at 60 fps and 240 s at native 15 fps, with zero frozen/short frames or timeouts and no observed memory drift. A 30 fps check measured 29.81 fps with zero drops. [Camera handover](tasks/2026-09-15-ds1-1080p-handover.md), [status](STATUS.md). | Development validated; ≥30-minute production qualification remains. |
| Camera brightness | Replaced the exact-linear ISP gamma with the Khadas photographic curve, selected 50 Hz antiflicker, and applied a measured manual 10 ms / 18 dB indoor profile at preview startup. Far-field mean luma rose from 6.57 to 19.89 at the compared gain request; near-field mean rose from 84.2 to 137.8. The room became usable in preview. [Image tuning](camera/image-tuning.md). | Fixed indoor profile only; colour cast and scene adaptation remain. |
| Browser preview | One camera owner feeds DS1 to software MJPEG/HTTP at `http://192.168.1.38:8090/`, managed by a transient systemd unit. A remote browser displayed the stream. 1080p15 MJPEG measured 4.7–5.9 Mbit/s. The current boxed demo serves 640×360. [Streaming handover](tasks/2026-09-15-streaming-handover.md), [box demo](tasks/2026-09-16-ball-box-demo.md). | Diagnostic, single viewer. |
| NPU runtime | Distribution TFLite + Mesa Teflon + etnaviv executes MobileNet V1 UINT8 on the GC8000 NPU. Delegate logs showed 27 operators compiled into the NPU graph and hardware jobs; pooling/softmax remained outside it. On the same synthetic input, CPU inference averaged 93.86 ms versus NPU 6.70 ms (14.0×). On a live tennis-ball crop, 94.15 versus 6.93 ms (13.6×), with the same top class. [NPU proof](tasks/2026-09-16-npu-proof.md). | Classification proved for this model; not a general NPU guarantee. |
| Object localization | SSDLite MobileDet on CPU found a box around the white ball but called it `apple`; on a control soccer-ball image it returned `sports ball`. The same detector through Teflon returned zero detections on both inputs. [NPU proof](tasks/2026-09-16-npu-proof.md). | NPU detector result unresolved. |
| Live inference overlay | Built a two-stage proof: CPU SSDLite proposals → NPU MobileNet tennis-ball classification → red box and quantized score in MJPEG. A worker analyzes the newest frame without blocking the preview writer. After startup, measured DS1 capture rates over three consecutive minutes were 29.97, 29.94, 29.97 fps, with sequence gaps of 3, 5, 3 frames and no frozen/short frames or timeouts. Analysis costs roughly 300–350 ms, about three analyses/s and roughly one CPU core. [Box demo](tasks/2026-09-16-ball-box-demo.md), [implementation](../platform/npu/tools/tflite-ball-stream.cc). | One-object diagnostic proof; browser paint rate and end-to-end latency have not been measured separately. |

The prior `ball_tracker` experiment's approximately 22 fps is not a direct
performance comparison: it used a grayscale threshold/contour detector on the
NV12 Y plane with a local OpenCV display. It did not run a neural detector or
encode MJPEG for a browser. Its useful lesson was to keep video delivery
independent of slower recognition; the current worker does that. The current
30 fps figure describes the **capture/video path**, not 30 neural analyses/s.

## Current data path and limits

```text
IMX415 → CSI/ISP → DS1 1920×1080 NV12 → ds1stream (sole camera owner)
                                       → ffmpeg: RGB 640×360, 30 fps
                                       → tflite-ball-stream
                                          ├─ preview writer → box/label → ffmpeg MJPEG → browser
                                          └─ latest-frame worker
                                             ├─ CPU SSDLite: 320×320 proposals (~300–350 ms)
                                             └─ NPU MobileNet: 224×224 crop (~7–8 ms)
```

- **Localization is the bottleneck.** The NPU speedup measures one classifier
  invocation, not the complete detection pipeline. The CPU SSDLite proposal
  stage dominates each analysis, so simply increasing the camera rate cannot
  increase box-update frequency materially.
- **Recognition is narrow.** The worker checks at most six proposals of COCO
  classes `apple` or `sports ball`, then considers ImageNet `tennis ball` score
  ≥10%. The displayed percent is a quantized softmax score (scale 1/256), not
  a calibrated probability. `BALL?` means tennis ball was not the top class.
  The low threshold helps the demo but can admit false boxes.
- **Only one box is retained.** The implementation selects the proposal with
  the highest tennis-ball score. In a saved frame containing a second dark
  sphere, CPU SSDLite called that object `vase`, and MobileNet called its crop
  `lemon` (tennis-ball output 7/256). Changing the output to a vector alone
  would not make this a multi-ball detector. Neither ball demo is evidence of
  bird detection.
- **The preview has a short-lived client pipeline.** It supports one viewer;
  disconnect/reconnect restarts the capture and model graph, causing a pause.
  The overlay is burned into JPEG frames. The proposed product architecture
  instead keeps the camera/model service alive across browser reconnects and
  sends detection metadata separately from clean video.
- **Camera control is incomplete.** The userspace 3A algorithm loop is absent;
  manual exposure/gain make one indoor scene usable, but AE/AWB/AF do not adapt.
  The image retains a green cast. Runtime exposure and frame-rate settings
  reset and must be applied by each camera owner.
- **Resource and endurance budgets are still open.** The 30 fps boxed preview
  reached spot SoC readings of 66.2/70.1 °C without an active cooling state.
  These are observations, not a thermal-endurance pass. The one-hour NPU soak
  was deliberately stopped after roughly 15 minutes for the camera demo.
  No 30-minute production-stream soak, multi-client test, or measured
  end-to-end video/detection latency has been completed.
- **Encoding is software today.** Mainline kernel 6.18 exposes no A311D
  hardware video encoder, although the silicon and vendor driver tree contain
  encoder implementations. A vendor port should be justified by measured
  software-encoding cost. [Streaming investigation](tasks/2026-09-16-streaming-infrastructure.md).

## Architecture implications and decisions requested

1. **Choose the detector-validation path.** First establish why the tested
   SSDLite MobileDet returns zero boxes through Teflon despite CPU boxes:
   inspect tensor values, quantization/postprocessing and delegated operator
   boundaries on known inputs. Then compare a second suitable quantized
   detector with CPU/NPU output checks. Treat CPU proposals as a diagnostic
   fallback, not the intended continuous bird-detector design. The architecture
   draft currently names NPU SSDLite as the first bird detector; that is a
   target, not a demonstrated capability.
2. **Keep one long-lived camera session.** The vendor V4L2 driver assigns
   FR/META/DS1 by `/dev/video1` open order, so independent consumers cannot
   safely open DS1 on their own. The platform should own those handles and
   distribute timestamped frames with bounded queues: latest-frame for
   preview/detection, ordered frames for recording. See the
   [platform architecture](BIRDCHER_PLATFORM_ARCHITECTURE.md).
3. **Separate product video from inference metadata.** Preserve clean video;
   publish boxes, class, score, track ID and timestamps to the UI. Keep the
   camera and inference process alive when a browser disconnects. Measure
   viewer count, latency, CPU, thermals and bandwidth before choosing MJPEG
   versus another transport.
4. **Set acceptance criteria for the next proof.** A useful gate is a detector
   that returns correct boxes on a small, saved test set through CPU and NPU,
   with measured full-pipeline analysis rate and no delegate errors; then run
   the uninterrupted one-hour NPU stress test. Only after this should the
   bird-specific event pipeline rely on NPU localization.

## Suggested next work order

1. Reproduce and isolate the SSDLite CPU/NPU output mismatch with saved inputs
   and tensor-level comparison; record whether the fault is in the delegated
   graph or detector postprocessing.
2. Validate one NPU-compatible multi-object detector on the board and measure
   accuracy, complete per-frame cost, memory and thermal behaviour. Use the
   CPU path as a correctness reference.
3. Implement a reusable long-lived camera/frame distributor and inference
   interface; keep the existing boxed MJPEG demo as a diagnostic fixture.
4. Add bird detection/tracking and event recording only after localization
   works reliably; complete NPU and production-stream endurance checks in
   parallel with product integration.

The broader milestone plan is in [ROADMAP.md](ROADMAP.md). The current feature
and test inventory is in [STATUS.md](STATUS.md); source and reproduction steps
are in [platform/npu](../platform/npu/README.md).
