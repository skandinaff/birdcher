# Teflon detector: two measured defects and candidate fixes

2026-09-26, VIM3 `192.168.1.38`, saved RGB inputs only. This supersedes the
next-step recommendations in the [initial ML review](2026-09-26-teflon-ml-review.md).

## Result

Two candidate patches on Mesa main restore plausible MobileDet detections.
On three saved frames the highest-scoring detection has the same class and
score as CPU. On the original soccer frame its box is also byte-identical.
A second invocation produces byte-identical final outputs. All patched runs
below completed without matching kernel faults. This is a limited backend
comparison, not validation of bird-detection quality or bit-exact equivalence.

| Saved RGB | CPU / patched NPU top class | CPU / NPU score | Maximum top-box coordinate difference |
|---|---:|---:|---:|
| `birdcher-soccer320.rgb` | 36 / 36 | 0.46484375 / 0.46484375 | 0 |
| `birdcher-ball-new-320.rgb` | 46 / 46 | 0.5234375 / 0.5234375 | 0.004663 |
| `birdcher-two-balls-320.rgb` | 85 / 85 | 0.66796875 / 0.66796875 | 0.002859 |

Filenames do not establish ground truth. Matching CPU on the latter two files
is an execution check, not evidence that their predicted classes are correct.
Concatenated-logit uint8 mean absolute error is 0.9664, 1.0033 and 0.9717,
respectively. Lower-ranked detections and some boxes differ.

## Controls

- Own unpatched Mesa 26.0.8 reproduces all-zero detector outputs, successful
  Invoke, and MMU faults/hung-GPU recovery. The checked wrapper exits 3.
- Unpatched main `a5d39a4b743b719cfc2a0da410910ea3fd8ff770` runs this detector
  without recorded faults, but gives incorrect outputs (top class 66,
  score 0.36328125). Upgrading alone does not fix numerical correctness.
- The main build uses `opengl=true` to satisfy NIR/TGSI linking; the 26.0.8
  control was built with `opengl=false`. Neither has a source patch.
- The upstream single-convolution fixture `mobiledet/026.tflite` is byte-exact
  on CPU, own 26.0.8 and main for a deterministic 64,000-byte synthetic input.
  An all-zero instance of that operation in a full faulty graph was not proof
  that this convolution itself was broken.

With both patches, the MobileNet V1 classifier output matches CPU byte-for-byte
on saved `birdcher-soccer224.rgb`. MobileNet V2 has the same argmax (806), uint8
MAE 0.5035 and maximum difference 2. Both runs have clean checked kernel logs.

## First defect: v7 ADD bias correction reads beyond synthetic weights

Prefix extraction retains the first N TFLite operators and declares the last
operator's output. It changes graph boundaries; therefore corroborate it with
hardware dumps rather than treating host intermediate buffers as ground truth.

The first eight operators remain close to CPU: prefix 8 output 113 has uint8
MAE 0.11494, maximum difference 4. Prefix 9 adds the first residual ADD and
returns 102,400 zero bytes versus CPU range 12–250. Shipped instrumentation
(`ml_msgs,npu_no_batching,dump_shaders`) confirms zeros in the ADD hardware
output, with no recorded kernel faults.

`etna_ml_lower_add_v7` allocates eight synthetic weight bytes with a 2×2 kernel.
The v7 weight encoder handles ADD as one synthetic channel, but
`calculate_bias_correction` uses the original channel count (16 here), reading
64 bytes. The patch applies the same effective-channel rule to bias correction.

[Patch 1](../../platform/npu/mesa-build/patches/0001-etnaviv-v7-add-bias-weight-bounds.patch)
restores prefix 9 to range 12–250, MAE 0.11461, maximum difference 4. At prefix
42, MAE is 0.88677. It does not by itself restore full detector correctness.
This patch has not been shown to explain or fix the 26.0.8 MMU faults.

An isolated two-input ADD experiment also failed. After patch 1 it still had
an incorrect second hardware input: the first 102,400 bytes matched input 113,
but the next 102,400 bytes were all zero instead of input 109. This reduction
exposes an additional input-upload problem and is **not** a clean numerical
regression for patch 1. Keep the nine-op prefix as the validated reproducer.
The isolated-upload issue remains unresolved.

## Second defect: reshape preserves the wrong physical element order

With patch 1, the convolution before each first head reshape is close to CPU,
but the reshape output diverges sharply:

| Output | Before reshape MAE | After reshape MAE | After CHW→HWC permutation |
|---|---:|---:|---:|
| box 197 → 198 | 0.91042 | 22.8608 | 0.91042 |
| class 202 → 203 | 0.98125 | 17.11282 | 0.98125 |

The lowered graph shows `BYPASS 202 203`, layout expected NHWC but actual NCHW,
without a detranspose. Applying the CHW→HWC permutation to the bytes recovers
exactly the pre-reshape error. A reshape preserves the framework's linear
order; changing shape while retaining physical NCHW breaks that requirement.

[Patch 2](../../platform/npu/mesa-build/patches/0002-etnaviv-reshape-nhwc-order.patch)
inserts a detranspose before reshape when the input has a nontrivial 3D shape,
actual NCHW and expected NHWC. Together the patches produce the results above.
Other reshape forms, explicit-transpose models and hardware generations still
need upstream coverage; these are candidate patches, not a universal guarantee.

## Reproduction and artifacts

Build instructions: [Mesa build README](../../platform/npu/mesa-build/README.md).
Patches target the main commit above. System Mesa was not replaced. The tested
combined library is `/tmp/birdcher-ml-review/libteflon-layout-fix.so` on the board,
SHA-256 `1f16d76e3724bc5b4a44fbcf8f5407f857cb7285f4e6259e83e6ef195300cce2`.
The unpatched libraries and per-run raw output directories remain alongside it.

Compile the corrected probe with board TFLite headers/libraries. The
[prefix tool](../../platform/npu/tools/tflite-prefix.cc) needs TFLite schema and
FlatBuffers headers (`g++ -O2 -std=c++17`, no TFLite runtime link). Example:

```sh
./tflite-prefix model.tflite 9 prefix9.tflite
mkdir -p cpu npu
DUMP_DIR=cpu ./tflite-tensor-probe prefix9.tflite saved320.rgb cpu 114
./run-probe-checked.sh npu-run env DUMP_DIR=npu TEFLON_LIB=/path/libteflon.so \
    ./tflite-tensor-probe prefix9.tflite saved320.rgb npu 114
```

[Evidence directory](../logs/2026-09-26-teflon-followup/) contains compact JSON
metrics, the stdlib-only summary script, environment/input hashes, experimental
single-op tools and compressed original logs/commands. The summary JSON is the
quick entry point; reading all logs is unnecessary. Raw dumps are kept on the
board under `/tmp/birdcher-ml-review`, and will not survive reboot.

Next: review these two patches upstream with prefix regressions, broaden the
saved-frame comparison on the intended bird model/dataset, and investigate the
separate isolated-ADD input-upload defect. No upstream issue or message has been
sent, and no camera-dependent evaluation has been run.
