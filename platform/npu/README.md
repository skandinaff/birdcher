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
224×224×3 UINT8 image so both interpreters receive identical bytes. It warms
up each interpreter, reports steady-state latency and compares all 1001 output
bytes. Quantized CPU/NPU rounding is allowed to differ by at most eight levels;
the top output index must match.

```sh
TEFLON_DEBUG=verbose ./tflite-compare mobilenet_v1_1.0_224_quant.tflite 50 compare
./tflite-compare mobilenet_v1_1.0_224_quant.tflite 550000 npu
```

Use the verbose option only for a short proof run: it prints the delegated
operators and hardware job times. A longer `npu` run omits the expensive CPU
comparison. The default Teflon library path is
`/usr/lib/teflon/libteflon.so`. The initial board measurements and stress-test
status are in [docs/tasks/2026-09-16-npu-proof.md](../../docs/tasks/2026-09-16-npu-proof.md).
