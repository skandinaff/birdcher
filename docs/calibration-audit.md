# ISP calibration audit (read-only)

Scope: find out what the shipped IMX415 calibration actually contains and
where each part came from. **Nothing was tuned, replaced or disabled.** This
exists so that when AE/AWB behaviour changes later, we know whether the cause
was the scaler, the calibration, or something else.

## The headline

The calibration is not merely "from a different board". It was **deliberately
retuned for a different application**, and that retuning actively switches off
most of what a normal camera wants. From the file's own header:

> IMX415 static (mode-invariant) calibration set for the Radxa Camera 4K on a
> Radxa Zero 2 Pro. Derived from the ARM/Amlogic neutral reference set ...
> **retuned for a marksmanship target camera feeding a grayscale computer-vision
> pipeline.**
>
> - linear response, so output DN is proportional to scene radiance
> - minimal denoise ... no sharpening
> - **nothing scene-adaptive between two consecutive frames (AE, AWB, Iridix,
>   auto-level)**, because any of those becomes a full-frame delta

So the green cast and the AE behaviour are, at least in part, **the configured
intent** rather than a tuning gap:

- white balance is *pinned* — `CALIBRATION_STATIC_WB` plus an **identity CCM**,
  so no colour correction is applied at all. A raw Bayer sensor with no CCM and
  no WB gains reads green-dominant, which is exactly what we see.
- `CALIBRATION_AE_CONTROL` additionally disables the Iridix global gain.

This must be settled **before** hand-tuning AWB/CCM, and before drawing any
conclusion about AE from the scaler fix.

## Inventory

| | static | dynamic |
| --- | --- | --- |
| calibration slots populated | 54 | 61 |
| tables carrying a `SOURCE:` note | 43 | 60 |

Every table carries a `SOURCE:` comment, which makes this auditable without
guesswork. Distribution:

| Provenance | static | dynamic | Meaning |
| --- | --- | --- | --- |
| `ARM reference (carried verbatim from the dummy set)` | 25 | 38 | Generic ISP defaults, untouched |
| `principled neutral choice for this CV application` | 11 | 11 | **Deliberately neutralised** for the CV use case |
| `IMX415-AAQR-C datasheet (E19504)` | 5 | 0 | Genuinely sensor-specific |
| `corrected geometry, derived from this camera's actual mode` | 0 | 2 | Mode/geometry derived |
| `PLACEHOLDER` (still needs a hardware measurement) | 13 | 1 | Never measured |

Note the third row is the only part that is truly *about the IMX415*, and the
second row is the part actively working against a general-purpose camera.

## Classification by kind

**Sensor-specific** (valid for us — same sensor):
`CALIBRATION_BLACK_LEVEL_{R,GR,GB,B}`, `CALIBRATION_NOISE_PROFILE`,
`CALIBRATION_WDR_NP_LUT`, decompander tables. Derived from the IMX415
datasheet, and black level was additionally measured on hardware (202, not the
datasheet's 200 — see commit `779d06b`).

**Lens / module-specific** (suspect — tied to the *Radxa Camera 4K* optics, not
the module actually attached here):
`CALIBRATION_SHADING_LS_{A,D65,TL84}_{R,G,B}`,
`CALIBRATION_SHADING_RADIAL_{R,G,B}`, `CALIBRATION_MESH_*`,
`CALIBRATION_MT_ABSOLUTE_LS_{A,D40,D50}_CCM`, `CALIBRATION_CA_CORRECTION*`,
`CALIBRATION_PF_RADIAL_*`. Lens shading and CCM are the two that most need
per-module data; both are currently ARM reference or neutralised.

**AE / AWB / statistics**:
`CALIBRATION_AE_CONTROL`, `CALIBRATION_AE_CORRECTION`,
`CALIBRATION_AE_EXPOSURE_CORRECTION`, `CALIBRATION_AE_ZONE_WGHT_{HOR,VER}`,
`CALIBRATION_AWB_{AVG_COEF,BG_MAX_GAIN,MIX_LIGHT_PARAMETERS,ZONE_WGHT_*}`,
`CALIBRATION_AWB_SCENE_PRESETS`, `CALIBRATION_AWB_WARMING_LS_*`,
`CALIBRATION_STATIC_WB`, `CALIBRATION_{RG,BG}_POS`, `CALIBRATION_CT*POS`,
`CALIBRATION_COLOR_TEMP`, `CALIBRATION_LIGHT_SRC`, `CALIBRATION_EVTOLUX_*`,
`CALIBRATION_AUTO_LEVEL_CONTROL`, `CALIBRATION_IRIDIX_*`.

The zone-weight tables are the ones tied to frame geometry, so they are worth
re-checking **after** the scaler fix. Several AWB tables carry a comment saying
they are inert because WB is pinned, and are present only because
`_GET_LUT_PTR()` spins forever on a NULL table — i.e. they are load-bearing as
*pointers* but not as *values*.

**Generic ISP tuning** (denoise, sharpening, demosaic, gamma, colour
conversion): `CALIBRATION_SINTER_*`, `CALIBRATION_TEMPER_STRENGTH`,
`CALIBRATION_SHARPEN_*`, `CALIBRATION_SHARP_ALT_*`, `CALIBRATION_DEMOSAIC*`,
`CALIBRATION_GAMMA`, `CALIBRATION_RGB2YUV_CONVERSION`,
`CALIBRATION_SATURATION_STRENGTH`, `CALIBRATION_DP_*`,
`CALIBRATION_SCALER_{H,V}_FILTER`. Mostly ARM reference, with denoise and
sharpening deliberately turned down or off for the CV pipeline.

## Is DW9714 / focus handled separately?

**No — autofocus is architecturally delegated, and nothing is connected.**

- `inc/acamera_firmware_config.h` compiles out *every* hardware VCM driver:
  `ISP_SENSOR_DRIVER_{DONGWOON,DW9800,AD5821,ROHM,LC898201,FP5510A,BU64748,AN41908A,NULL,MODEL} = 0`
  and sets `ISP_SENSOR_DRIVER_V4L2 = 1`.
- So the ISP expects the lens to be an external **V4L2 subdev**
  (`src/driver/lens/v4l2_vcm.c`, holding a `struct v4l2_subdev *soc_lens`).
- No DW9714 driver exists in this tree at all — `lens_init.c` `#include`s
  `dongwoon_vcm.h` and friends behind `#if ISP_SENSOR_DRIVER_<name>` guards
  that are all 0, and the headers/sources they name were never vendored: only
  `lens_init.c` and `v4l2_vcm.c` are present in `src/driver/lens/`.
- Nothing instantiates a lens subdev, so the AF path is inert. The
  `CALIBRATION_AF_LMS` / `CALIBRATION_AF_ZONE_WGHT_*` tables are ARM reference
  values that nothing consumes.

The hardware, however, *is* there: the camera module answers at **0x0c**, which
the Khadas vendor DTS names `dw9714@0c`, and this kernel already builds the
mainline driver — `CONFIG_VIDEO_DW9714=m`. So focus is addable later by
instantiating that driver and binding it as the ISP's lens subdev. It is not a
prerequisite for anything currently planned.

Worth noting the mismatch for the record: the calibration header states *"The
Radxa Camera 4K has a fixed lens and no focus"*, while the module attached here
has a voice-coil actuator. That is one more sign these tables describe
different optics.

## What this does NOT tell us

- Whether the AE misbehaviour is caused by calibration or by the scaler. AE
  statistics are collected over the output frame, and the ISP is currently
  writing at full sensor resolution regardless of the negotiated size, so the
  zone weighting may be applied over the wrong geometry. **Fix the scaler
  first, then re-measure AE**, exactly so this stays separable.
- Whether the "neutral CV" choices are individually wrong for Birdcher — only
  that they were made for a different purpose and should be revisited
  deliberately, not inherited by accident.

## Suggested order when we do act

1. Fix the scaler (separate change, no calibration touched).
2. Re-measure AE/AWB/exposure/gain against correct geometry.
3. Only then decide per-table: keep, re-derive from the ARM neutral reference,
   or measure on hardware. The 13 static `PLACEHOLDER` tables are the natural
   first candidates, since they were never measured for any board.
