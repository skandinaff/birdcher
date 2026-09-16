# Ball bounding-box feasibility demo

The user placed a white tennis ball in the IMX415's view. On a captured DS1
1920×1080 frame, the first COCO SSDLite MobileDet model produced a box around
the ball on CPU, but called the region `apple` with score 0.488. Its Teflon
path returned zero detections, even on a separate soccer-ball control image.
The model is therefore not yet usable as a trustworthy NPU detector.

A two-stage experiment succeeded on the captured frame:

1. Resize the full frame to 320×320 RGB and run SSDLite MobileDet on CPU.
   Its `apple` proposal was `[ymin=0.258, xmin=0.192, ymax=0.596, xmax=0.400]`,
   or approximately `(369, 279)–(768, 644)` in the 1920×1080 frame. That box
   closely covered the visible ball.
2. Crop the proposal from the full-resolution frame, resize to 224×224 RGB,
   and run MobileNet V1 through Teflon on the NPU. `tennis ball` (index 853)
   was the top class, raw output 177; the next class, `lemon`, was 17. Ten
   warm inferences averaged 8.25 ms. These raw output bytes are not calibrated
   probabilities.
3. Draw a box labeled `tennis ball (NPU class; CPU box)` around the proposal.
   The diagnostic JPEG is kept outside Git because it contains a private room
   view. The source frame remains on the board in `~/birdcher-tools/npu/`.

This establishes that a CPU proposal plus NPU verification can yield a box on
a still frame. It does **not** yet establish reliable detection across scenes
or a live overlaid video stream. The CPU proposal took about 284 ms on this
frame, so it is too slow for every 30 fps camera frame. A live proof should
own the DS1 stream once, sample a few frames per second for detection, reuse
model interpreters, and draw confirmed boxes on the browser preview without
reopening `/dev/video1`. Measure latency and false detections before using it
for bird events.

An alternate Google Coral SSD MobileNet V2 quantized model was also checked.
It accepts 300×300 RGB input, but CPU inference labeled the soccer-ball
control photo as `umbrella` and this camera ball as `vase` (both score 0.277).
It is not a useful replacement for this demo. The model file stays outside Git.

This work is a feasibility step toward [Phase 5](../ROADMAP.md),
not completion of M2's uninterrupted NPU stability test.
