# Experimental etnaviv detector patches

Base: Mesa main `a5d39a4b743b719cfc2a0da410910ea3fd8ff770`.
Apply 0001 then 0002. Build and test using the parent README; do not replace
system Mesa as part of the experiment.

0001 bounds ADD bias-correction reads to the synthetic kernel's channel count.
0002 materializes NHWC ordering before reshape when a 3D tensor is physically
NCHW but logically NHWC. Neither patch addresses the independent two-input
upload reduction failure or establishes the cause of old Mesa MMU faults.

[Measured results and limitations](../../../../docs/tasks/2026-09-26-teflon-two-fixes.md).
The library with both patches was tested on three saved MobileDet inputs,
a repeated inference, and MobileNet V1/V2 controls on VIM3. Broader graph,
hardware and dataset coverage is required before production adoption.
