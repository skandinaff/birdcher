# ML review: a real execution failure, with unreliable earlier localization

> **Follow-up:** [two measured defects and candidate fixes](2026-09-26-teflon-two-fixes.md)
> now restore CPU-like detections on saved inputs. The investigation below
> records the earlier state; its outstanding build/localization steps are superseded.

Reviewed 2026-09-26 on `192.168.1.38`, using saved RGB files only.

**Verdict:** the CPU/delegate mismatch is real and worth reporting. The evidence
supports an execution-stack defect on this configuration, not a diagnosis of a
particular Mesa memory-planner function. Fresh runs expose NPU MMU faults and
recovery while TFLite reports successful invocation. Some earlier intermediate
measurements are invalid because the probe reads reused TFLite arena memory.
Neither finding supports abandoning all SSD models or making a Mesa patch the
prerequisite for validating Birdcher's bird detector.

## Evidence collected

Packaged Mesa `26.0.8-1ubuntu0.3`, TFLite `2.14.1+dfsg-3build1`, kernel
`6.18.44-current-meson64`. No packages, drivers, camera settings, or model files
were changed. The Khadas reference remains pinned at `3a11a86`; no ISP change
is proposed, so the camera implementation is not implicated by these tests.

Model SHA-256:
`32c486140391eb4dc43fca7113ad392be632dc5366687f2731f73d740678693f`.
Saved `birdcher-soccer320.rgb` SHA-256:
`b7cede04f991ed37f9414c68a5cab0f99cc7adfe8242b4ba678057d1892aedf1`.

Logs and the exact diagnostic probe source are in
[the review evidence directory](../logs/2026-09-26-teflon-review/).
The RGB remains in `~/birdcher-tools/npu/` on the board; an upstream reproducer
still needs a redistributable input or a deterministic input generator.

| Run | Final maximum score / count | Other observations |
| --- | --- | --- |
| Original probe, CPU | 0.464844 / 100 | Same baseline as the previous investigation |
| Original probe, NPU, `TEFLON_DEBUG=verbose ETNA_MESA_DEBUG=ml_msgs` | 0 / 0 | MMU fault at 10:32:25 EEST; hung GPU recovery; first partition reported 1095 ms |
| Preserve-all probe, CPU | 0.464844 / 100 | Intermediate values change substantially; final output summaries remain unchanged |
| Preserve-all probe, NPU, `TEFLON_DEBUG=verbose` | 0 / 0 | MMU fault/recovery at 10:34:02; reported first-partition time only 43 ms |
| Preserve-all probe, NPU, `ETNA_MESA_DEBUG=npu_no_batching` | 0.15625 / 100 | Repeated MMU faults/recoveries at 10:34:44–46; first partition 1432 ms; top class 0 instead of CPU class 36 |

All detector processes completed the probe's checked `Invoke()` path. Thus
successful `Invoke()` and a plausible timing are **not sufficient evidence of
successful hardware execution** on this stack. Disabling batching changes the
symptom but is not a valid workaround: the output follows hardware faults and
does not match the CPU. The two preserved runs overlapped during CPU graph
compilation; their logged hardware-fault episodes occurred at separate times.
This is a diagnostic comparison, not a latency benchmark.

A subsequent MobileNet V1 CPU/NPU control on `birdcher-soccer224.rgb` completed;
NPU partition timings were 7/0 ms, with no new kernel messages. Its final output
summaries matched (max 255, mean 0.255). This checks that recovery did not leave
the NPU universally unusable; these summaries are not a tensor-equality test.

## The measurement bug matters

The original probe calls `interp->tensor(index)` after the entire `Invoke()`.
A non-null pointer does not preserve that tensor's earlier value. TFLite can
reuse its arena after the last consumer, including for partition-boundary
tensors. The diagnostic variant enables `InterpreterOptions::SetPreserveAllTensors`
through `ApplyOptions` before applying the delegate and allocating tensors. It
also destroys the interpreter before deleting its external delegate.

| CPU tensor | Original post-Invoke probe | With preservation |
| --- | --- | --- |
| 198, box head 0 | 0..224, mean 35.154 | 18..251, mean 186.349 |
| 203, class head 0 | 0..224, mean 23.429 | 39..202, mean 100.323 |
| 324, concatenated logits | 0..224, mean 29.297 | 39..226, mean 106.162 |

The original bytes `0 0 128 60` are consistent with float data occupying a
former uint8 allocation; the preserve-all comparison demonstrates the problem
without relying on that interpretation. Google documents that intermediate
reads without preservation can return undefined values in its
[Interpreter API](https://ai.google.dev/edge/api/tflite/python/tf/lite/Interpreter).

On NPU, preserved tensors 198 and 203 were still zero, but tensor 324 was
**0..78**, not all zero. Therefore the old table cannot establish that all
12 heads and the concatenated logits were uniformly zero during execution.
Preservation fixes TFLite arena lifetime; it does not guarantee visibility of
internal delegate tensors or fix any delegate-side reuse. To locate the first
bad operation, capture boundaries at execution time or use verified preserved
outputs. Do not poison arbitrary arena tensors after copying the input: shared
allocations can make that perturb the input being tested.

## Claims that should be narrowed

- CPU detections establish a backend mismatch, not model accuracy on birds.
  Identical input bytes remove scene/preprocessing differences from that
  comparison, but do not validate preprocessing against the model contract.
- Working MobileNets show working configurations of convolution, ADD, reshape,
  and quantization. They do not exclude defects in other shapes, scales,
  layouts, fan-out, or combinations of these operations.
- Forcing 13 classifier outputs rejects a universal prohibition on multiple
  outputs. It does not prove SSD branch lifetimes or output delivery are sound.
- Distinct logged output addresses do not prove that hardware wrote the right
  bytes, that every memory access is mapped, or that commands completed.
- A uint8 code of zero is not necessarily real zero: for tensor 203 it means
  approximately -10.69. Its sigmoid rounds to zero at scale 1/256. That explains
  why corrupt low logits can erase detections; it does not independently prove
  that the logistic implementation is correct on useful inputs.
- A returned count of 100 is not 100 meaningful objects. Here many CPU scores
  are very small. Compare boxes, classes and scores with explicit matching and
  tolerances; counts and maxima alone are inadequate accuracy metrics.
- Two failing SSDs cannot establish that no SSD will work, or that all NPU
  object detection is blocked. Nor does classification of a selected crop
  establish full-frame bird presence detection.

The expectation that this model should execute is reasonable: Mesa's
[official Teflon documentation](https://docs.mesa3d.org/teflon.html) explicitly
lists this UINT8 SSDLite MobileDet and A311D as supported/tested. That is a reason
to investigate a version/configuration regression, not proof that this Ubuntu
package and kernel combination has been validated. Mesa main was not built or
tested in this review; the upstream regression status remains unknown.

## Recommended next work

1. Reframe the upstream issue around **MMU faults / hung NPU recovery with
   successful Invoke and incorrect outputs**, attaching kernel and delegate
   logs. Keep the exact responsible layer open: generated commands, buffer
   addressing/lifetimes, synchronization, and kernel handling need separation.
   Check existing issues and reproduce on an isolated current Mesa build before
   proposing a fix. No upstream issue was filed in this review.
2. Fix the measurement method before searching for the first arithmetic error.
   Capture every partition's inputs/outputs while valid and compare to CPU in
   dequantized units. Use tensor error metrics before NMS; small numerical
   differences can legitimately alter final box ordering, but hardware resets
   invalidate an accuracy run entirely.
3. Keep detector correctness and product quality as separate workstreams.
   Assemble saved, labelled bird/no-bird scenes representative of the intended
   distance, bird size, illumination and background. Measure event recall,
   false triggers per hour, time to trigger, and end-to-end CPU cost. A soccer
   photo is a useful execution reproducer, not a bird-quality evaluation set.
4. Establish CPU detector performance at the product's actual sampling rate
   before making NPU support a release blocker. The roadmap calls NPU
   detection preferable. A classifier behind a crop/proposal stage is another
   candidate, but the proposal stage's missed birds count as system misses;
   its viability is not established by the current ball demo.

This review establishes a reproducible failure and corrects the diagnosis. It
does not identify the faulty command, validate a patch, or demonstrate useful
bird-detection quality.
