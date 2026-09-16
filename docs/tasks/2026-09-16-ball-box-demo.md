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
rate was initially set to 3 fps for the synchronous proof. That made the
browser view visibly slow because the CPU detector takes about 300–375 ms
per analysis; MobileNet on the NPU takes about 7–8 ms for each proposed crop.

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

The next iteration separated preview from inference: the sensor and MJPEG
output were first configured for 15 fps; a worker copies the newest RGB frame and
performs detection without blocking the video writer. The last verified box
is drawn on later video frames for up to 1.5 s. A 10-second HTTP client pull
received 175 JPEG frames, including queued frames from startup. An analysis
still takes roughly 300–350 ms and uses about one CPU core. That rate describes
independent detections, not the browser video rate.

The older `ball_tracker` repo's commit `f16122b` records about 22 fps. Its
change removed the colour path, processed only the NV12 Y plane, and reduced
the local OpenCV display to half size. The C++ detector uses a brightness
threshold, contour circularity and a region of interest; it does not run a
neural object detector or encode MJPEG for a browser. The source calls its
GStreamer path zero-copy, but `readYPlane()` actually clones the Y plane.
The old and current fps numbers therefore describe different workloads. The
architectural lesson is to keep preview separate from slower recognition,
which the asynchronous worker now does.

Removing the initial 15-fps cap and setting the sensor and MJPEG viewer to 30
fps gave 29.97, 29.94 and 29.97 captured fps in three consecutive post-startup
minutes. Sequence gaps grew by 3, 5 and 3 frames in those minutes; there were
no frozen/short frames or timeouts. The spot thermal readings reached
66.2/70.1 °C without an active thermal cooling state. This proves the *video*
path can exceed the old 22 fps; detection is still about three analyses/s.

When the ball moved and shrank in the frame, the old `tennis ball` first-place
requirement hid the box: one isolated crop scored 42/256 for tennis ball while
another class scored 82/256. The live proof now limits region proposals to
COCO `apple` or `sports ball` boxes and accepts a tennis-ball softmax score of
at least 10%. It shows the score next to the box (`BALL 46%` on a verified
HTTP frame); `BALL?` indicates that tennis ball was not the model's top class.
The classifier output scale is 1/256 with zero point 0, so the displayed
percentage is a quantized softmax score, **not a calibrated probability**.
The lower threshold is useful for this demonstration but increases the chance
of a false box.

The live program currently keeps one `Box`, so it can outline only one object.
The second, dark sphere on a saved camera frame was called `vase` by the COCO
CPU detector (0.668) and `lemon` by MobileNet on the NPU (227/256 versus
7/256 for `tennis ball`). Simply changing the output container to multiple
boxes would not make it a reliable ball detection. A ball-specific contour
path might give geometric candidates but the old white-brightness threshold
will not detect this dark sphere against its dark background. A suitable
multi-object detector or a separately validated geometric method is required
before labeling both spheres as balls.

The live proof still needs broader scene testing. It recognises only ImageNet
class 853 (`tennis ball`); the COCO CPU model supplies boxes but sometimes
misnames the ball. The current browser path has one client; reconnecting
rebuilds the Teflon graph and may pause the picture. Do not treat it as bird
detection or a production 30 fps stream. A production application still needs
multi-client fan-out, a longer-lived camera/model process across browser
reconnects, tracking and measured end-to-end latency.

An alternate Google Coral SSD MobileNet V2 quantized model was also checked.
It accepts 300×300 RGB input, but CPU inference labeled the soccer-ball
control photo as `umbrella` and this camera ball as `vase` (both score 0.277).
It is not a useful replacement for this demo. The model file stays outside Git.

This work is a feasibility step toward [Phase 5](../ROADMAP.md),
not completion of M2's uninterrupted NPU stability test.
