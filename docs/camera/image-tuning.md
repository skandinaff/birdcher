# IMX415 indoor brightness tuning

Measured on the deployed Khadas VIM3 and IMX415 in a well-lit room on
2026-09-15. The reported failure was spatially obvious: the near desk was
visible while the farther half of the room appeared nearly black.

## Root causes

This was a combination of calibration and the incomplete 3A stack:

1. The calibration used an exact-linear gamma LUT chosen for grayscale frame
   differencing. That is useful for radiometric measurements, but it maps
   ordinary indoor shadows to very dark browser pixels. The pinned Khadas
   reference uses a photographic curve that lifts those shadows.
2. The kernel publishes AE statistics, but the userspace 3A algorithm daemon
   is absent. Automatic exposure reaches its integration-time limit and never
   adds sensor gain, leaving the scene underexposed.
3. Antiflicker was configured for 60 Hz. Birdcher is deployed on 50 Hz mains;
   the old setting quantised the requested exposure to 1125 lines instead of a
   50 Hz-safe 1350-line / 10 ms exposure.

Per `AGENTS.md`, the calibration was compared with the pinned, read-only Khadas
source at `external/khadas-common_drivers` commit `3a11a86` before changing it.

## Changes

- `CALIBRATION_GAMMA` now uses the 129-entry ARM/Khadas photographic curve.
- CMOS antiflicker frequency is 50 Hz.
- `preview-ctl.sh start` selects manual exposure, requests 1350 sensor lines
  (10 ms), requests analogue gain 96 (log2 x32 = 8x = 18 dB), and keeps both
  digital gain stages at zero.
- The preview status command reports the exposure and sensor gain actually
  applied. The sensor reports gain 60 because its control uses 0.3 dB units.

The runtime settings reset with the camera stack, so every camera owner must
apply them when it starts. `preview-ctl.sh` does this now. Experiments can use
`BIRDCHER_EXPOSURE_LINES` and `BIRDCHER_ANALOG_GAIN` overrides.

## Measurements

The same fixed scene was sampled as 1920x1080 DS1 NV12. “Far” is the top half
of the luma plane and “near” is the bottom half.

With the former exact-linear curve and 60 Hz quantisation, even the highest
tested analogue gain produced a far-region mean of only 14.8:

| ISP gain request | Whole-frame Y mean | Far mean | Near mean | pixels >250 |
| ---: | ---: | ---: | ---: | ---: |
| 64 | 18.8 | 2.44 | 35.1 | — |
| 80 | 29.7 | 4.0 | 55.4 | — |
| 96 | 45.4 | 6.57 | 84.2 | — |
| 112 | 64.8 | 10.0 | 119.5 | — |
| 128 | 77.4 | 14.8 | 140.0 | 0.087% |

With 50 Hz quantisation and the ARM/Khadas gamma curve:

| ISP gain request | Whole-frame Y mean | Far mean | Near mean | pixels >250 |
| ---: | ---: | ---: | ---: | ---: |
| 64 | 48.0 | 5.49 | 90.5 | 0.026% |
| 80 | 62.2 | 11.72 | 112.6 | 0.102% |
| **96** | **78.8** | **19.89** | **137.8** | **0.505%** |

Gain 96 was selected: distant room detail becomes visible while the near desk
retains highlight headroom. The deployed module and preview were cold-booted,
and read-back confirmed 1350 exposure lines and sensor gain 60 at 30 fps. The
HTTP preview then delivered 5.6 MB in five seconds with `200 OK` and the correct
multipart MJPEG content type.

## Remaining limits

- The image still has a green cast. WB and CCM are pinned neutral and the AWB
  loop is absent. That is the next image-quality problem, separate from the
  brightness correction.
- A 20 ms exposure at 30 fps is not currently available through the ISP path.
  The bridge caches an integration limit of 2242 lines during boot at the
  sensor's 60 fps default, and later clamps/overwrites a 2700-line request.
  Extending exposure requires fixing that dynamic limit or changing the boot
  mode; it is unnecessary for the selected 10 ms profile.
- The fixed profile does not adapt between daylight and dim rooms. Restoring or
  replacing the userspace 3A loop is the correct route to adaptive exposure.
