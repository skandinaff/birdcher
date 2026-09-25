# Why the NPU detector returns zero detections

Updated 2026-09-25. Resolves the open item carried by
[2026-09-16-npu-proof.md](2026-09-16-npu-proof.md) and
[2026-09-16-ball-box-demo.md](2026-09-16-ball-box-demo.md): SSDLite MobileDet
gave detections on CPU and none through Mesa Teflon.

**Conclusion: the fault is a tensor-aliasing defect in Mesa's etnaviv ML
memory planner, not in our code, our thresholds, the custom post-processing
op, or the choice of model. No SSD-family detector will work through this
delegate until Mesa is fixed.**

## The tool this needed

The C API only exposes a model's declared outputs, which is useless when the
graph is split across CPU and NPU and fails somewhere in between.
`platform/npu/tools/tflite-tensor-probe.cc` uses the TFLite C++ interpreter,
which can read any tensor in the primary subgraph -- including the tensors the
CPU and NPU partitions hand to each other. It has three modes that mattered
here:

| | |
| --- | --- |
| `cpu` / `npu` + tensor indices | dump each tensor's shape, quantization and value range |
| `npu+poison` | fill those tensors with `0xAA` before `Invoke` |
| `EXTRA_OUTPUTS=a,b,c` | declare extra model outputs before the delegate partitions |

The poison mode answers a question no amount of reading can: a buffer that
still reads `0xAA` was never written, which separates "computed zeros" from
"delivered nothing".

## How Teflon splits this model

`TEFLON_DEBUG=verbose` on `ssdlite_mobiledet_coco_qat_postprocess.tflite`:

```
partition 1   120 ops   CONV/DWCONV/ADD/RESHAPE      -> 12 output tensors
CPU            11 ops   9x QUANT, 2x CONCATENATION
partition 2     2 ops   RESHAPE, LOGISTIC
CPU             3 ops   2x DEQUANT, TFLite_Detection_PostProcess
```

The 12 outputs of partition 1 are the box and class heads of the six feature
maps (`BoxPredictor_0..5/Reshape{,_1}`). The custom post-processing op landing
on the CPU is expected and is not the problem.

## What was measured

Same input bytes (`birdcher-soccer320.rgb`, the control soccer-ball image) on
both backends.

| Tensor | CPU | NPU |
| --- | --- | --- |
| 198 `BoxPredictor_0/Reshape` | u8 0..224 | **all 0** |
| 203 `BoxPredictor_0/Reshape_1` | u8 0..224 | **all 0** |
| ...all 12 partition-1 outputs | plausible ranges | **all 0** |
| 324 `concat_1` | 0..224 | 0 |
| 325 `convert_scores` (sigmoid) | 0..119 | 0 |
| 326 scores, f32 | 0..0.4648 | 0 |
| 329 `PostProcess:2` | top score 0.4648 | 0 |

Every CPU op downstream is therefore behaving correctly: `sigmoid(-10.69)` is
`2.3e-5`, which quantizes to 0 at scale 1/256, and a score tensor of zeros
legitimately yields no detections. **The post-processing and the threshold are
innocent.** With `npu+poison`, the `0xAA` fill was replaced by zeros, so the
delegate does write all 12 buffers -- it computes zeros rather than failing to
deliver.

## Teflon itself is not broken

Two classifiers were run as controls, poisoned, against CPU:

| Model | Delegated | Result |
| --- | ---: | --- |
| MobileNet V1 1.0 224 quant | 27 ops | matches CPU |
| MobileNet V2 1.0 224 quant | 62 ops | matches CPU (CPU max 151, NPU 148) |

MobileNet V2 carries residual `ADD`s, so residual connections and depthwise
convolutions are fine. Two hypotheses died here: that `LOGISTIC` was wrong
(tensor 325 is correct given its input), and that a `RESHAPE` on a partition
boundary was wrong (MobileNet V1's second partition is `CONV` then `RESHAPE`
and works).

A third hypothesis -- that the delegate cannot deliver a subgraph with many
outputs -- also died. `EXTRA_OUTPUTS` was used to force MobileNet V2 to expose
13 outputs; the real output stayed byte-identical to the single-output NPU run.

## The actual defect: distinct tensors share one buffer

That same 13-output run exposed it. Three pairs of unrelated tensors came back
**byte-for-byte identical on the NPU while differing on the CPU**:

| Pair | CPU mean | NPU |
| --- | --- | --- |
| 24 `expanded_conv_1/project/add_fold` / 101 `expanded_conv_2/...` | 118.793 / 133.052 | both `152 115 111 120 112 116 119 120`, mean 136.196 |
| 33 `expanded_conv_10/...` / 43 `expanded_conv_11/...` | 126.238 / 127.267 | both `124 142 115 112 112 111 122 131` |
| 62 `expanded_conv_13/...` / 72 `expanded_conv_14/...` | 131.711 / 131.191 | both `137 136 128 145 122 126 113 136` |

All six are `project/add_fold` tensors, that is, inputs to residual `ADD`s.
In `src/gallium/drivers/etnaviv/etnaviv_ml.c` (read at tag `mesa-26.0.8`) an
`ADD` is implemented by demanding its two inputs be contiguous in one buffer:

```c
} else if (operation->type == ETNA_JOB_TYPE_NN && operation->input_count > 1) {
   recreate_tensor(subgraph, operation->input_tensors[0], sizes[0] + sizes[1]);
   reference_tensor_with_offset(subgraph, operation->input_tensors[0],
                               operation->input_tensors[1], sizes[0], sizes[1]);
}
```

and `reference_tensor_with_offset()` then repoints *every* tensor that shared
the old resource onto the new one, copying that one offset and size to all of
them:

```c
if (old_res) {
   for (int i = 0; i < num_tensors; i++)
      if (etna_ml_get_resource(subgraph, i) == old_res) {
         pipe_resource_reference(&tensors[i]->resource, src->resource);
         tensors[i]->size = size;
         tensors[i]->offset = offset;
      }
}
```

In a linear classifier the collided copies are never read again, so MobileNet
V2 still produces the right answer and the bug stays invisible. An SSD head is
the opposite shape: six branches fan out of the backbone and all twelve of
their outputs are live at the end of the graph. They get dragged onto shared
storage and read back as zeros.

This explains the uniformity of the failure -- not one wrong branch but all
twelve identically empty -- and why swapping detectors did not help. The Coral
SSD MobileNet V2 detector tried on 2026-09-16 has the same multi-head shape.

## Two side findings worth keeping

**Graph compilation is slow enough to shape the architecture.** Measured on
this board: MobileNet V1 16.5 s, MobileNet V2 25.9 s, SSDLite MobileDet
**61.1 s**. Inference afterwards is 8-43 ms. Any service must compile once in a
long-lived process; a per-request or per-reconnect model load is not viable.
The existing `tflite-ball-stream` already holds its interpreters open, which is
why its browser reconnects merely pause rather than stall for a minute.

**Do not use `EXTRA_OUTPUTS` to read intermediate values as if they were
ground truth.** The aliasing above means exported intermediates are exactly the
values this delegate gets wrong. The mode is a probe for the defect, not a
debugger for model arithmetic.

## What this means for Birdcher

Object detection on the NPU is blocked on Mesa, not on us. Three ways forward,
not mutually exclusive:

1. **Report it upstream** with this minimal reproducer. The characterisation is
   precise and the repro is two stock models plus one tool. A ready-to-paste
   issue body is drafted in
   [../upstream/mesa-teflon-etnaviv-ml-aliasing.md](../upstream/mesa-teflon-etnaviv-ml-aliasing.md);
   it is **not filed**.
2. **Keep the NPU for single-output work** and get candidate regions another
   way. This is what the current two-stage ball demo does, and it is measured:
   NPU classification 7-8 ms against 94 ms on CPU. For "is a bird present" this
   is sufficient; it is not localisation.
3. **Patch Mesa locally.** The defect is in one function and the fix is to stop
   propagating one tensor's offset and size to every tensor that shared its
   resource. This is a deliberate fork of a graphics stack, so it is a decision
   rather than a task.

Nothing here changes the camera path. The scaler and DS1 capture are untouched.
