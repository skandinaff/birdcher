# Upstream bug report draft: Teflon/etnaviv returns all-zero outputs for a fan-out (SSD) graph

> **DO NOT FILE AS WRITTEN.** Later the same evening,
> `ETNA_MESA_DEBUG=ml_msgs` showed the detector's twelve outputs are each
> aliased onto a *distinct* NN-job output buffer at offset 0 with the correct
> size, so the "they collide onto one buffer" explanation below is wrong. The
> measurements in this report stand; the **Likely cause** section does not.
> See the correction in
> [../tasks/2026-09-25-teflon-detector-investigation.md](../tasks/2026-09-25-teflon-detector-investigation.md)
> before touching this file again.

**Status: drafted 2026-09-25, not filed.** Intended destination is
<https://gitlab.freedesktop.org/mesa/mesa/-/issues> with labels `etnaviv` and
`teflon`. Everything below the horizontal rule is written to be pasted as the
issue body. Local background and the full investigation trail are in
[../tasks/2026-09-25-teflon-detector-investigation.md](../tasks/2026-09-25-teflon-detector-investigation.md).

Before filing, search existing issues for `teflon` and `etnaviv_ml` — this may
already be known — and check whether the behaviour still reproduces on Mesa
main, since the report below is against the 26.0.8 that Ubuntu 26.04 ships.
Reporting against a release branch is fine, but upstream will ask.

---

## Teflon: etnaviv delivers all-zero outputs for a graph whose delegated subgraph has fan-out branches (SSD detection heads)

### System

| | |
| --- | --- |
| Board | Khadas VIM3, Amlogic A311D |
| NPU | Vivante GC8000, DT node `npu@ff100000`, `compatible = "vivante,gc"` |
| DRM | `etnaviv`, `/dev/dri/by-path/platform-etnaviv-render` → `renderD128` |
| Mesa | `26.0.8-1ubuntu0.3` (`mesa-teflon-delegate`, `mesa-libgallium`) |
| Delegate | `/usr/lib/teflon/libteflon.so` |
| TFLite | `libtensorflow-lite` 2.14.1 (`2.14.1+dfsg-3build1`) |
| Kernel | 6.18.44 aarch64 |
| Distro | Armbian 26.8.3 (Ubuntu 26.04 userspace) |

### Summary

A quantized SSD object detector produces correct results on the TFLite CPU
reference and **all-zero results** through the Teflon external delegate. The
delegated subgraph runs (the NN job is submitted and timed) and it writes to
every one of its output buffers — it writes zeros.

Classifiers are unaffected: MobileNet V1 and MobileNet V2 (both UINT8) match
the CPU reference through the same delegate on the same board, so this is not a
general breakage of convolution, depthwise convolution or residual `ADD`.

The structural difference is the shape of the graph. An SSD head fans out of
the backbone into several branches, all of whose outputs are live at the end of
the delegated subgraph. A classifier is a single chain with one live output.

### Model

`ssdlite_mobiledet_coco_qat_postprocess.tflite`, input `1x320x320x3` UINT8,
four float outputs (the standard `TFLite_Detection_PostProcess` contract).

```
sha256  32c486140391eb4dc43fca7113ad392be632dc5366687f2731f73d740678693f
```

Controls, both from the TensorFlow model zoo:

```
ecc3a67c47c5a609ec35f6a58a7d97532834e43df4cb7d3f1204a8164b7d20dd  mobilenet_v1_1.0_224_quant.tflite
f08d447cde49b4e0446428aa921aff0a14ea589fa9c5817b31f83128e9a43c1d  mobilenet_v2_1.0_224_quant.tflite
```

### How the graph is partitioned

`TEFLON_DEBUG=verbose`:

```
partition 1   120 ops   CONV / DWCONV / ADD / RESHAPE   -> 12 output tensors
CPU            11 ops   9x QUANTIZE, 2x CONCATENATION
partition 2     2 ops   RESHAPE, LOGISTIC
CPU             3 ops   2x DEQUANTIZE, TFLite_Detection_PostProcess (custom)
```

The 12 outputs of partition 1 are the box- and class-prediction heads of six
feature maps (`BoxPredictor_0..5/Reshape` and `.../Reshape_1`). The custom
post-processing op falling back to the CPU is expected and is not the problem.

### Steps to reproduce

The TFLite C API only exposes a model's declared outputs, which is not enough
here: the graph fails somewhere between the CPU and NPU partitions. The C++
interpreter can read any tensor in the primary subgraph, including the tensors
the partitions hand each other. Minimal probe (full version attached):

```cpp
auto model = tflite::FlatBufferModel::BuildFromFile(model_path);
tflite::ops::builtin::BuiltinOpResolver resolver;
std::unique_ptr<tflite::Interpreter> interp;
tflite::InterpreterBuilder(*model, resolver)(&interp);
interp->SetNumThreads(1);

auto opts = TfLiteExternalDelegateOptionsDefault("/usr/lib/teflon/libteflon.so");
TfLiteDelegate* d = TfLiteExternalDelegateCreate(&opts);
interp->ModifyGraphWithDelegate(d);          // omit for the CPU reference
interp->AllocateTensors();

memcpy(interp->tensor(interp->inputs()[0])->data.raw, rgb, rgb_size);

// Optional: poison a boundary buffer to tell "computed zeros" from "not written".
TfLiteTensor* t = interp->tensor(203);
memset(t->data.raw, 0xAA, t->bytes);

interp->Invoke();
// inspect interp->tensor(198), (203), (241) ... (322), and (326), (329)
```

Feed any 320x320x3 UINT8 image. We used a stock photograph of a football,
which the CPU path detects as COCO `sports ball` at score 0.465.

### Expected

The delegated path's outputs agree with the CPU reference, at least to
quantization noise, as they do for the two classifiers.

### Actual

Same input bytes, both backends:

| Tensor | CPU | Teflon/etnaviv |
| --- | --- | --- |
| 198 `BoxPredictor_0/Reshape` (u8) | 0..224 | **all 0** |
| 203 `BoxPredictor_0/Reshape_1` (u8) | 0..224 | **all 0** |
| ...all 12 outputs of partition 1 | plausible ranges | **all 0** |
| 324 `concat_1` (u8) | 0..224 | 0 |
| 325 `convert_scores` (u8, sigmoid) | 0..119 | 0 |
| 326 scores (f32) | 0..0.4648 | 0 |
| 329 `PostProcess:2` (f32 scores) | top 0.4648 | 0 |
| 330 `PostProcess:3` (count) | 100 | 0 |

Every CPU op downstream behaves correctly given its input: `sigmoid(-10.69)` is
`2.3e-5`, which quantizes to 0 at scale 1/256, and an all-zero score tensor
legitimately yields no detections. So the failure is upstream of the
post-processing, inside the delegated subgraph.

`TEFLON_DEBUG=verbose` reports the work as done:

```
teflon: compiling graph: 333 tensors 120 operations
teflon: compiled graph, took 61055 ms
teflon: compiling graph: 333 tensors 2 operations
teflon: compiled graph, took 1 ms
teflon: invoked graph, took 43 ms
teflon: invoked graph, took 1 ms
```

**The buffers are written, not skipped.** Filling all 12 with `0xAA` before
`Invoke()` and re-reading afterwards returns zeros, not `0xAA`.

### Controls that rule other things out

| Model | Delegated | Result |
| --- | ---: | --- |
| MobileNet V1 1.0 224 quant | 27 ops | matches CPU |
| MobileNet V2 1.0 224 quant | 62 ops | matches CPU (CPU u8 max 151, NPU 148) |

MobileNet V2 has residual `ADD`s, so residual connections and depthwise
convolutions are fine on this hardware and driver.

Two further hypotheses were tested and rejected:

- **Not `LOGISTIC`.** Tensor 325 is correct for its (already zero) input.
- **Not `RESHAPE` on a partition boundary.** MobileNet V1's second partition is
  `CONV` then `RESHAPE`, and its output is consumed correctly by the CPU
  `SOFTMAX`.
- **Not the number of subgraph outputs as such.** Using
  `Interpreter::SetOutputs()` to declare 13 outputs on MobileNet V2 before
  delegation still produced a byte-identical final output.

### Likely cause: distinct tensors are made to share one buffer

That 13-output MobileNet V2 run is what exposed something concrete. Three pairs
of unrelated tensors came back **byte-for-byte identical through the delegate**
while differing on the CPU:

| Pair | CPU mean | Teflon/etnaviv |
| --- | --- | --- |
| 24 `expanded_conv_1/project/add_fold` / 101 `expanded_conv_2/project/add_fold` | 118.793 / 133.052 | both `152 115 111 120 112 116 119 120`, mean 136.196 |
| 33 `expanded_conv_10/...` / 43 `expanded_conv_11/...` | 126.238 / 127.267 | both `124 142 115 112 112 111 122 131` |
| 62 `expanded_conv_13/...` / 72 `expanded_conv_14/...` | 131.711 / 131.191 | both `137 136 128 145 122 126 113 136` |

All six are `project/add_fold` tensors, i.e. inputs to residual `ADD`s. In
`src/gallium/drivers/etnaviv/etnaviv_ml.c` an `ADD` is implemented by requiring
its two inputs to be contiguous in one buffer:

```c
} else if (operation->type == ETNA_JOB_TYPE_NN && operation->input_count > 1) { /* Add or Subtraction */
   recreate_tensor(subgraph, operation->input_tensors[0],
                   operation->input_tensor_sizes[0] + operation->input_tensor_sizes[1]);
   reference_tensor_with_offset(subgraph,
                               operation->input_tensors[0],
                               operation->input_tensors[1],
                               operation->input_tensor_sizes[0],
                               operation->input_tensor_sizes[1]);
}
```

and `reference_tensor_with_offset()` then repoints *every* tensor that shared
the destination's old resource onto the new one, assigning all of them that one
offset and size:

```c
if (old_res) {
   for (int i = 0; i < num_tensors; i++) {
      if (etna_ml_get_resource(subgraph, i) == old_res) {
         pipe_resource_reference(&tensors[i]->resource, src->resource);
         tensors[i]->size = size;
         tensors[i]->offset = offset;
      }
   }
}
```

In a single-chain classifier the collided aliases are never read again, so
MobileNet V2 still returns the right answer and the aliasing stays invisible.
An SSD head is the opposite shape: six branches fan out of the backbone and all
twelve of their outputs are live at the end of the subgraph, so they cannot
tolerate being relocated onto shared storage.

This would also explain the uniformity of the failure — not one wrong branch but
all twelve identically empty.

**Stated honestly:** the aliasing above is observed under `SetOutputs()`
instrumentation, which itself changes which tensors must be materialized. That
the *same* mechanism is what empties the detector's twelve outputs is a
hypothesis consistent with all the measurements, not something proven. The
detector's all-zero outputs, and the two classifiers' correctness, are direct
observations.

### Secondary observation: compile time

Graph compilation on this board, measured:

| Model | Delegated ops | Compile |
| --- | ---: | ---: |
| MobileNet V1 1.0 224 quant | 27 | 16.5 s |
| MobileNet V2 1.0 224 quant | 62 | 25.9 s |
| SSDLite MobileDet | 120 | **61.1 s** |

Inference afterwards is 8–43 ms. This is not the bug being reported, but a
minute of compilation for a 120-op graph may be worth a look on its own, and it
makes any per-process model load impractical.

### Attachment

`tflite-tensor-probe.cc` — dumps any tensor of the primary subgraph for either
backend, can poison buffers before `Invoke()` to distinguish "computed zeros"
from "never written", and can declare extra outputs via an `EXTRA_OUTPUTS`
environment variable. Single file, builds with:

```sh
g++ -O2 -std=c++17 tflite-tensor-probe.cc -ltensorflow-lite -o tflite-tensor-probe
./tflite-tensor-probe model.tflite frame.rgb npu+poison 198 203 326
```
