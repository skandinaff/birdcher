# Draft: etnaviv v7 ADD weight overread and NHWC reshape ordering in MobileDet

Not filed upstream. This draft replaces the earlier all-zero/aliasing diagnosis;
its historical measurements remain in the
[investigation](../tasks/2026-09-25-teflon-detector-investigation.md).
The [measured follow-up](../tasks/2026-09-26-teflon-two-fixes.md) has the full
controls, limitations and reproduction procedure. Before filing, search existing
Mesa issues and split these independently demonstrated defects if appropriate.

## Environment

Khadas VIM3 / A311D, kernel `6.18.44-current-meson64`, TFLite 2.14.1.
Mesa main `a5d39a4b743b719cfc2a0da410910ea3fd8ff770`, arm64 cross-build,
Teflon/etnaviv, debugoptimized, OpenGL enabled for link dependencies.
Model `ssdlite_mobiledet_coco_qat_postprocess.tflite`, SHA-256
`32c486140391eb4dc43fca7113ad392be632dc5366687f2731f73d740678693f`.
All comparisons use identical saved RGB bytes on CPU and NPU.

## Observed behavior

Unpatched main returns successful Invoke and no recorded MMU faults but gives
incorrect detections. On the soccer input CPU's top result is class 36, score
0.46484375; main gives class 66, score 0.36328125. The corrected probe preserves
TFLite tensors, dumps declared outputs and checks the kernel journal separately.

1. **ADD bias correction:** first eight operators are close to CPU (uint8 MAE
   0.11494). Appending the first residual ADD makes all 102,400 output bytes zero.
   Shipped per-operation dumps confirm zeros in hardware output. The ADD encoder
   allocates eight synthetic weight bytes for a 2×2 kernel, while v7
   `calculate_bias_correction` iterates over 2×2×16 = 64 bytes. Treating ADD as
   one synthetic weight channel, as the weight encoder already does, restores
   this prefix to CPU range 12–250 and MAE 0.11461.
2. **Reshape ordering:** after fixing ADD, first-head convolutions are close
   to CPU, but their reshape outputs are not. The lowered graph contains
   `BYPASS 202 203` with actual NCHW and expected NHWC. A CHW→HWC permutation of
   the output reduces MAE from 17.11282 to exactly the preceding convolution's
   0.98125. The corresponding box head goes from 22.8608 to 0.91042. Inserting
   a detranspose before reshaping these inputs restores element order.

Candidate patches:

- [v7 ADD bias bounds](../../platform/npu/mesa-build/patches/0001-etnaviv-v7-add-bias-weight-bounds.patch)
- [NHWC before reshape](../../platform/npu/mesa-build/patches/0002-etnaviv-reshape-nhwc-order.patch)

Together they restore the same highest-scoring class and score as CPU on three
saved inputs. The soccer box is byte-identical; the other top boxes differ by
at most 0.004663 in normalized coordinates. Concatenated-logit MAE is approximately
one uint8 level. Repeat final outputs are byte-identical, with no recorded
kernel faults. Other detections may differ; dataset-level accuracy is untested.

## Reproduction evidence and remaining scope

Use the [prefix extractor](../../platform/npu/tools/tflite-prefix.cc) with 9
operators for ADD; compare prefixes 56/57 and 59/60 for the head reshapes.
[Evidence](../logs/2026-09-26-teflon-followup/) includes source tools, command
logs, kernel logs, input/library hashes and compact numerical summaries.
See the follow-up for build and board commands. These patches have only been
validated on this board and the recorded models/inputs.

An isolated two-input ADD reduction is not a clean regression: its second
hardware input becomes zero despite a nonzero saved input. This separate upload
problem remains open; the nine-op prefix is the validated ADD reproducer.

The packaged and independently built Mesa 26.0.8 controls also produce MMU
faults/hung-GPU recovery despite Invoke success. Main did not reproduce those
faults in these tests. These two patches establish numerical defects on main,
not the root cause or a backport fix for those 26.0.8 execution faults.

Earlier claims excluding ADD/RESHAPE, implicating a particular aliasing function,
or generalizing the failure to all SSD models were not supported. Host tensors
read after Invoke without arena preservation also gave misleading intermediate
values; those tables must not be used to localize this report.
