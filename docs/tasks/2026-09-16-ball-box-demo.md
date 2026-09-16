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

## Live browser proof

`platform/npu/tools/tflite-ball-stream.cc` now reuses one CPU detector and one
Teflon classifier across frames. The camera is owned by one `ds1stream`
process; FFmpeg scales its DS1 frames to 640×360 RGB, the application resizes
them for each model and draws a red rectangle, and a second FFmpeg serves MJPEG
on the existing browser URL `http://192.168.1.38:8090/`. The camera's sensor
rate is set to 3 fps for this proof, not 30 fps with discarded frames.

The program first succeeded on the saved frame: box `(125,93)–(256,215)` in
640×360, tennis-ball output 163 versus 31 for the next class. Then it ran on
the live stream and boxed the white ball after it moved. An HTTP client got
`200 OK`, `multipart/x-mixed-replace` and actual 640×360 JPEG frames with a
box. After the first client disconnected, the service restarted the capture
pipeline for a new client. Once the models were warm, the second pipeline's
first 60 seconds captured 177 frames (2.93 fps), with two sequence gaps and
no frozen frames, short frames or timeouts. The last measured processing time
was about 307–314 ms/frame. The application used roughly one CPU core and the
SoC thermal readings were 57.6/59.4 °C during a spot check.

The live proof still needs broader scene testing. It recognises only ImageNet
class 853 (`tennis ball`); the COCO CPU model supplies boxes but sometimes
misnames the ball. The current browser path has one client; reconnecting
rebuilds the Teflon graph and may pause the picture. Do not treat it as bird
detection or a production 30 fps stream. A later application should keep one
camera owner, distribute full-rate preview frames separately from sampled
inference, and retain the last confirmed box between detection frames.

An alternate Google Coral SSD MobileNet V2 quantized model was also checked.
It accepts 300×300 RGB input, but CPU inference labeled the soccer-ball
control photo as `umbrella` and this camera ball as `vase` (both score 0.277).
It is not a useful replacement for this demo. The model file stays outside Git.

This work is a feasibility step toward [Phase 5](../ROADMAP.md),
not completion of M2's uninterrupted NPU stability test.
