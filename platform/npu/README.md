# VIM3 NPU proof

The A311D's GC8000 NPU appears as `/dev/dri/by-path/platform-etnaviv-render`
(`renderD128` on the current board). The `skf` user belongs to `render`.
Ubuntu 26.04 packages Mesa Teflon and TensorFlow Lite, so this proof needs no
custom Mesa build:

```sh
sudo apt-get install mesa-teflon-delegate libtensorflow-lite-dev
g++ -O2 -std=c++17 tools/tflite-compare.cc -ltensorflow-lite -o tflite-compare
```

Download the official MobileNet V1 UINT8 model used by the
[Mesa Teflon documentation](https://docs.mesa3d.org/teflon.html):

```sh
curl -fL -o mobilenet_v1_1.0_224_quant.tgz \
  https://storage.googleapis.com/download.tensorflow.org/models/mobilenet_v1_2018_08_02/mobilenet_v1_1.0_224_quant.tgz
echo 'd32432d28673a936b2d6281ab0600c71cf7226dfe4cdcef3012555f691744166  mobilenet_v1_1.0_224_quant.tgz' | sha256sum -c -
tar -xzf mobilenet_v1_1.0_224_quant.tgz ./mobilenet_v1_1.0_224_quant.tflite
```

The extracted `.tflite` file's SHA-256 is
`ecc3a67c47c5a609ec35f6a58a7d97532834e43df4cb7d3f1204a8164b7d20dd`.
The model stays outside Git. `tflite-compare` uses a deterministic synthetic
224×224×3 UINT8 image by default. Pass a raw RGB24 image as the fourth argument
to check a real camera crop. Both interpreters receive identical bytes. The
tool warms up each interpreter, reports steady-state latency, lists the top
classes when given an ImageNet label file, and compares all 1001 output bytes.
Its smoke test requires the same top class and a maximum byte difference of 16;
this is a diagnostic threshold, not a formal accuracy guarantee. Output byte
values are quantized model scores, not calibrated probabilities.

```sh
TEFLON_DEBUG=verbose ./tflite-compare mobilenet_v1_1.0_224_quant.tflite 50 compare
./tflite-compare mobilenet_v1_1.0_224_quant.tflite 550000 npu
./tflite-compare mobilenet_v1_1.0_224_quant.tflite 20 compare ball-crop224.rgb imagenet_labels.txt
```

Use the verbose option only for a short proof run: it prints the delegated
operators and hardware job times. A longer `npu` run omits the expensive CPU
comparison. The default Teflon library path is
`/usr/lib/teflon/libteflon.so`. The initial board measurements and stress-test
status are in [docs/tasks/2026-09-16-npu-proof.md](../../docs/tasks/2026-09-16-npu-proof.md).

`tools/tflite-inspect.cc` prints a model's tensor contract. The experimental
`tools/tflite-detect.cc` runs a quantized COCO SSD model on a raw RGB24 frame
already resized to the model's input dimensions. On this board, SSDLite
MobileDet CPU inference gives detections, while
Teflon currently returns zero detections for the same inputs. Do not use that
detector's NPU result for an application until the mismatch is understood.

## Live tennis-ball box preview (diagnostic)

The two-stage demo uses SSDLite on CPU for candidate rectangles and MobileNet
V1 on the NPU to verify ImageNet class 853 (`tennis ball`). It is deliberately
limited to one ball class and a 640×360, 30 fps browser view. Inference runs
asynchronously on the newest frame at roughly 3 analyses/s; the last accepted
box is drawn on the intervening preview frames. The model's quantized softmax
score appears next to the box as a percentage. `BALL?` means tennis ball was
not the model's top class. The score is not a calibrated probability. Source and measured
results are in [the ball-box task](../../docs/tasks/2026-09-16-ball-box-demo.md).

On the board, with the models and `ds1stream` already in
`~/birdcher-tools/`:

```sh
g++ -O2 -std=c++17 -Wall -Wextra -pthread tools/tflite-ball-stream.cc \
  -ltensorflow-lite -o ~/birdcher-tools/npu/tflite-ball-stream
sudo ~/birdcher-tools/npu/ball-preview-ctl.sh start 30
# Open http://192.168.1.38:8090/ and refresh if it was already open.
sudo ~/birdcher-tools/npu/ball-preview-ctl.sh stop  # restores ordinary preview
```

Deploy `tools/serve-ball-mjpeg.sh` and `tools/ball-preview-ctl.sh` to
`~/birdcher-tools/npu/` as executable files first. The controller switches
the camera owner between the normal and boxed viewers; do not start both
pipelines independently. The boxed viewer runs as `mjpeg-ball-preview.service`
for one hour, one browser client at a time. Model compilation can delay the
first frame and each reconnect. Check `/tmp/birdcher-ball-{capture,inference,encoder}.log`
for diagnosis.
