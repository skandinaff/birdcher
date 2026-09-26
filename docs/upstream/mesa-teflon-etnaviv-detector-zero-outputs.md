# Teflon/etnaviv returns all-zero outputs for a quantized SSD detector

**For whoever tests this next.** Read this section first; it is the honest state.

| | |
| --- | --- |
| **Established** | A quantized SSD detector produces correct results on the TFLite CPU reference and all-zero results through the Teflon external delegate, on identical input bytes. Two different SSD models behave this way. Two classifiers are correct through the same delegate on the same board. |
| **Open** | *Why.* Two explanations were written down and then falsified by measurement. Do not inherit a theory from this document — there isn't one. |
| **Not the cause** | The post-processing op, the score threshold, `LOGISTIC`, a `RESHAPE` on a partition boundary, the number of subgraph outputs, residual `ADD`, the choice of model, and our own harness. Each was tested; see below. |
| **To do** | The experiments in *What to test next*. A local arm64 Mesa build already exists for it: `platform/npu/mesa-build/`. |

Not filed upstream yet. Intended destination is
<https://gitlab.freedesktop.org/mesa/mesa/-/issues>, labels `etnaviv`, `teflon`.
Before filing: search existing issues for `teflon` and `etnaviv_ml`, and check
whether this still reproduces on Mesa main — the measurements below are against
the 26.0.8 that Ubuntu 26.04 ships, and upstream will ask.

## Is the defect real, or is it our mistake?

Worth stating plainly, because it is the first thing to doubt.

- **Not the model.** The TFLite CPU reference reads the same `.tflite` file and
  produces correct detections from the same input bytes.
- **Not the input.** Inputs are files on disk, not camera frames. The probe reads
  one file and `memcpy`s the identical bytes into whichever interpreter is under
  test. The scene, exposure and everything else cancel out.
- **Not our harness.** The CPU and NPU runs are the same code path, differing by
  one call: `ModifyGraphWithDelegate()`. Remove that call and the results are
  correct.
- **Not one bad model file.** A second, unrelated detector (Google Coral SSD
  MobileNet V2, 300x300) also returns zero detections through Teflon while
  producing boxes on CPU.
- **Not the delegate being broken in general.** MobileNet V1 and MobileNet V2
  both match the CPU reference through the same delegate, same board, same
  session.

So the defect is real and lives in the delegated path. What it *is* remains open.

## System

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

## Models

Primary: `ssdlite_mobiledet_coco_qat_postprocess.tflite`, input `1x320x320x3`
UINT8, four float outputs (the standard `TFLite_Detection_PostProcess`
contract).

```
32c486140391eb4dc43fca7113ad392be632dc5366687f2731f73d740678693f  ssdlite_mobiledet_coco_qat_postprocess.tflite
```

Controls, from the TensorFlow model zoo:

```
ecc3a67c47c5a609ec35f6a58a7d97532834e43df4cb7d3f1204a8164b7d20dd  mobilenet_v1_1.0_224_quant.tflite
f08d447cde49b4e0446428aa921aff0a14ea589fa9c5817b31f83128e9a43c1d  mobilenet_v2_1.0_224_quant.tflite
```

Second detector that fails the same way: Google Coral SSD MobileNet V2,
`1x300x300x3` UINT8.

## How Teflon splits the detector

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

## Reproducing

The TFLite C API exposes only a model's declared outputs, which is not enough:
the graph fails somewhere between the CPU and NPU partitions. The C++
interpreter can read any tensor in the primary subgraph, including the tensors
the partitions hand each other.

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
interp->Invoke();
// inspect interp->tensor(198), (203), (241) ... (322), and (326), (329)
```

Any 320x320x3 UINT8 image reproduces it. The measurements below used a stock
photograph of a football, which the CPU path detects as COCO `sports ball` at
score 0.465.

## Expected and actual

Same input bytes, both backends:

| Tensor | CPU | Teflon/etnaviv |
| --- | --- | --- |
| 198 `BoxPredictor_0/Reshape` (u8) | 0..224 | **all 0** |
| 203 `BoxPredictor_0/Reshape_1` (u8) | 0..224 | **all 0** |
| ...all 12 outputs of partition 1 | plausible ranges | **all 0** |
| 324 `concat_1` (u8) | 0..224 | 0 |
| 325 `convert_scores` (u8, sigmoid) | 0..119 | 0 |
| 326 scores (f32) | 0..0.4648 | 0 |
| 329 `PostProcess:2` (scores) | top 0.4648 | 0 |
| 330 `PostProcess:3` (count) | 100 | 0 |

Every CPU op downstream behaves correctly *given its input*: `sigmoid(-10.69)`
is `2.3e-5`, which quantizes to 0 at scale 1/256, and an all-zero score tensor
legitimately yields no detections. The fault is upstream of all of that, inside
partition 1.

`TEFLON_DEBUG=verbose` reports the work as done:

```
teflon: compiling graph: 333 tensors 120 operations
teflon: compiled graph, took 61055 ms
teflon: compiling graph: 333 tensors 2 operations
teflon: compiled graph, took 1 ms
teflon: invoked graph, took 43 ms
teflon: invoked graph, took 1 ms
```

## What has been ruled out, and by what test

| Candidate | Test | Result |
| --- | --- | --- |
| The custom post-processing op or its threshold | read tensors 324/325/326 directly | each is correct for its own input; they faithfully propagate zeros |
| `LOGISTIC` | compare tensor 325 against its input 324 | correct |
| A `RESHAPE` at a partition boundary | MobileNet V1's second partition is `CONV` then `RESHAPE` | works; its output feeds the CPU `SOFTMAX` correctly |
| Residual `ADD`, depthwise convolution | MobileNet V2, 62 delegated ops, 10+ residual `ADD`s | matches CPU (u8 max 151 vs 148) |
| Many subgraph outputs | `Interpreter::SetOutputs()` forcing 13 outputs on MobileNet V2 | final output byte-identical to the single-output NPU run |
| The twelve outputs colliding onto one buffer | `ETNA_MESA_DEBUG=ml_msgs` | **refuted.** Each of the twelve is aliased onto a *distinct* buffer at offset 0 with its correct size; all twelve source addresses differ |

On the last row, the driver's own log is unambiguous — a real NN instruction
writes the source tensor and the graph output aliases it correctly:

```
 59 NN   195 197 in2:   0      src_tensor 197 (fa4db000) dst_tensor 198 offset 0 size 4800
 60 BYPASS 197 198             src_tensor 202 (fa4b5000) dst_tensor 203 offset 0 size 109200
 61 NN   193 200 in2:   0      ... ten more, all offset 0, all correctly sized
```

The full capture is kept at
[../logs/2026-09-25-etnaviv-ml-msgs-detector.log](../logs/2026-09-25-etnaviv-ml-msgs-detector.log).

### A measurement that was initially over-read

Filling the twelve output buffers with `0xAA` before `Invoke()` and finding
zeros afterwards was first taken as proof that the hardware wrote zeros. It is
not. `etna_ml_subgraph_read_outputs()` ends in
`pipe_buffer_read(res, 0, size, outputs[i])`, so the result proves only that the
**copy happened** and the GPU-side buffer held zeros. Whether any NN job wrote
them is exactly the open question.

### One loose observation, not a theory

Under the 13-output `SetOutputs()` run on MobileNet V2, three pairs of unrelated
tensors came back byte-for-byte identical through the delegate while differing
on the CPU (24/101, 33/43, 62/72 — all `project/add_fold`, i.e. residual `ADD`
inputs). The final output was still correct. This may be an artifact of forcing
intermediates to be materialized, or a real aliasing issue in the `ADD` path
that is harmless in a linear graph. It is recorded because it is reproducible,
**not** because it explains the detector.

Corollary: do not use `EXTRA_OUTPUTS`/`SetOutputs()` to read intermediate
values from this delegate as if they were ground truth.

## What to test next

In this order. The point is to answer by measurement, since reading the source
has now produced two wrong answers.

1. **Establish the bench.** Build the unpatched `libteflon.so` from Mesa 26.0.8
   and confirm it reproduces the all-zero outputs on the board. If it does not,
   nothing built on that bench counts. `platform/npu/mesa-build/` has the arm64
   container; the probe takes an alternate library through `TEFLON_LIB`, so the
   packaged Mesa never has to be touched.
2. **Find out whether those twelve buffers are written at all.** Instrument
   `etna_ml_compile_operation_nn()` to print the GPU address each compiled
   instruction writes to, and compare against the addresses
   `etna_ml_subgraph_read_outputs()` reads from. This distinguishes "never
   written" from "written somewhere else", which is the open question.
3. **Check the job submission.** 120 operations are compiled into one submission
   and reported as 43 ms. Confirm every instruction is actually in the batch and
   none were dropped — `ETNA_MESA_DEBUG=npu_no_batching` exists and is worth an
   A/B.
4. **Narrow by graph shape.** The detector's distinguishing feature versus the
   working classifiers is fan-out: tensor 193 feeds three branches, and the
   graph ends with twelve live outputs on six branches. A minimal hand-built
   model with one fan-out point would separate "fan-out" from "size" (120 ops vs
   62) and from "320x320 input".
5. **Only then write a patch**, as a test of something found.

## Secondary observation: compile time

| Model | Delegated ops | Compile |
| --- | ---: | ---: |
| MobileNet V1 1.0 224 quant | 27 | 16.5 s |
| MobileNet V2 1.0 224 quant | 62 | 25.9 s |
| SSDLite MobileDet | 120 | **61.1 s** |

Inference is 8–43 ms afterwards. Not the bug being reported, but a minute of
compilation for a 120-op graph may deserve its own look, and it rules out
loading a model per process or per client connection.

## Tooling

`platform/npu/tools/tflite-tensor-probe.cc` — one file, builds with
`g++ -O2 -std=c++17 tflite-tensor-probe.cc -ltensorflow-lite`. It dumps any
tensor of the primary subgraph for either backend, poisons buffers before
`Invoke()` (`npu+poison`), declares extra outputs (`EXTRA_OUTPUTS=a,b,c`), and
selects an alternate delegate library (`TEFLON_LIB`).

```sh
./tflite-tensor-probe model.tflite frame.rgb cpu 198 203 326
./tflite-tensor-probe model.tflite frame.rgb npu+poison 198 203 326
ETNA_MESA_DEBUG=ml_msgs ./tflite-tensor-probe model.tflite frame.rgb npu 198
```
