# DS1 scaler path: reference comparison and current blocker

Goal: produce 1920x1080 through the DS1 scaler stream, using
`common_drivers` `khadas-vims-5.15.y` `drivers/armisp-g12b/` as the
authoritative implementation reference. No new scaler path, no FR emulation,
no enlarged FR buffers, no undocumented register pokes.

Reference revision: `3a11a86`.

## Why FR cannot do this

`fw_intf_stream_set_resolution()` says so in upstream's own words:

```
 * StreamType
 *   - FR : directly update sensor resolution since FR doesn't have down-scaler.
 *   - DS : need to be implemented.
```

For FR the only lever is the sensor preset, and mainline `imx415` has exactly
one resolution: `struct imx415_mode` carries no width/height, and
`supported_modes[]` differs only by `lane_rate`. So 3864x2192 is the only size
FR can ever produce, and 1080p has to come from DS1.

## Reference comparison

| Area | Result |
| --- | --- |
| `isp_v4l2_fh_open()` / stream allocation | Identical. Stream-id-by-open-order (0=FR, 1=META, 2=DS1, 3=DS2) is the upstream design, not something this port introduced |
| `isp_v4l2_fop_open()` | Identical |
| `fw_intf_stream_set_resolution()` | Identical, including the FR/DS comment above |
| DS1 `IMAGE_RESIZE_*` programming | Identical (`IMAGE_RESIZE_TYPE_ID=SCALER`, `_WIDTH_ID`, `_HEIGHT_ID`, `_ENABLE_ID=RUN`) |
| `fw_intf_stream_set_output_format()` | Identical bar an unused `V4L2_PIX_FMT_NV21` case and extra logging |
| `fw_intf_stream_start()` | **REGRESSION — fixed**, see below |
| `fw_intf_stream_stop()` | DS1 branch present. Reference takes `stream_on_count` and stops the sensor from either branch when the last stream goes; this port substitutes a local `fr_stream_active` and does not stop the sensor on DS1 teardown |
| `dma_ds1` queue selection (`isp-vb2.c`) | Identical. The reference's second `d_type = dma_ds1` site is inside `#ifdef AUTOWRITE_MODULES_V4L2_API`, which is not built |
| Buffer sizing / stride | Identical |

Beyond the eight, the runtime DS1 plumbing was also compared:

- `dma_writer_fsm.c`, `dma_writer_intf.c` — byte identical.
- `dma_writer.c`, `dma_writer_func.c` — differences are formatting, `vmalloc`
  vs stack allocation of `dma_pipe_settings`, and extra logging. Same logic,
  same DS1 IRQ masks (`ACAMERA_IRQ_FRAME_WRITER_DS`, `ACAMERA_IRQ_FRAME_DROP_DS`).
- `crop_fsm.c` / `crop_func.c` — differences are **entirely brace-style and
  whitespace**, plus extra `LOG()` lines. The scaler FSM is faithfully ported.
- `frame_buffer_update_callback()` and its single caller in `dma_writer_fsm.c`
  — identical, so DS callback registration is not missing.
- `ISP_HAS_CROP_FSM` is 1 in both, so the `crop_info.width_ds = width_fr`
  fallback in `frame_buffer_initialize()` is not in play.

Two dead ends worth recording so they are not re-investigated:

- `set_ds1.active = (p_fsm->dma_reader_out == dma_ds1)` looks like a gate that
  would disable DS1 (the default is `dma_fr`), but `settings.active` is only
  consumed under `#if ISP_HAS_FPGA_WRAPPER && ISP_CONTROLS_DMA_READER`, i.e.
  the FPGA frame-reader path, not real silicon.
- The missing `autocapture_*.c` files correspond to
  `AUTOWRITE_MODULES_V4L2_API`, which is not built.

## The regression that was found and fixed

`fw_intf_stream_start()` tested only `V4L2_STREAM_TYPE_FR`, dropping the DS1
half of upstream's condition:

```c
if (streamType == V4L2_STREAM_TYPE_FR || streamType == V4L2_STREAM_TYPE_DS1)
    acamera_command( ctx_id, TSENSOR, SENSOR_STREAMING, ON, ... );
```

FR and DS1 share one sensor; DS1 differs only in that the ISP scales the output
on its way to a different DMA writer. With the DS1 half missing, `VIDIOC_STREAMON`
on DS1 returned 0 and its thread started, but the sensor was never told to
stream, so nothing completed and userspace blocked forever in `VIDIOC_DQBUF`.
Only the start side was asymmetric; `fw_intf_stream_stop()` kept its DS1 branch.

Fixed in `768a66b`. Note the local `acamera_fw_stream_rearm()` /
`fr_stream_active` pair is **ours, not upstream's**, and is deliberately scoped
to FR: applying it on DS1 too hard-hung the board on the first DS1 STREAMON.

## Current state

With the fix, DS1 negotiates correctly — this part works:

```
DS1 negotiated 1920x1080 planes=2 bpl=1920 size0=2073600 size1=1036800
got 4 buffers
```

So `S_FMT` on the DS1 stream is accepted **without adjustment**, and the plane
geometry is exactly right for 1080p NV12. The scaler configuration path is
reaching the firmware.

## The blocker

The kernel dies the instant the sensor starts on the DS1 path. Captured over
UART (`docs/logs/ds1-streamon-crash-uart.log`):

```
TRACE fw_intf_stream_start: about to acamera_command(SENSOR_STREAMING, ON)
start_streaming: entering, about to call sensor s_stream(1)
start_streaming: sensor s_stream(1) returned rc = 0
imx415 streaming on
TRACE fw_intf_stream_start: acamera_command(SENSOR_STREAMING, ON) returned, rc = 0
Unable to handle kernel paging request at virtual address 4fce4f7ce4bfce62
Unable to handle kernel paging request at virtual address 007ce433ce337ce4
[6ffcfb3bd2997d08] address between user and kernel address ranges
```

Characteristics:

- The fault lands immediately after `SENSOR_STREAMING ON` returns, i.e. on the
  first DS frame interrupt, before any `dma_writer` trace for DS1 appears.
- The faulting addresses are garbage with repeating byte patterns
  (`4fce4f7ce4bfce62`, `007ce433ce337ce4`) — consistent with an uninitialised
  pointer or image data being dereferenced, not a plain NULL.
- Multiple simultaneous aborts on different CPUs, and an Overflow-stack banner,
  so it cascades.
- `ACAMERA_IRQ_FRAME_WRITER_DS` never produces a `dma_writer_pipe_update()`
  trace, so the DS1 pipe is never successfully updated.
- Earlier, with DS1 alone and before the start fix, `isp_process` sat in
  `system_semaphore_wait` and userspace blocked in `vb2_core_dqbuf`.

Both configurations fail: DS1 alone crashes as above; FR and DS1 concurrently
also hangs the board (UART last showed `AM_ADAP: reader/frontend : width = 3864`).

## Leading hypothesis for next session

This is **not** a dropped line in the eight areas audited — those are faithful.
The most likely systemic cause is the **single-context flattening** of the
firmware in this port: upstream `armisp-g12b` is multi-context and threads
`ctx_id` through `acamera_command()`, `fw_intf_*`, `acamera_fsm_mgr_*` and the
dma pipe settings (`pipe->settings.ctx_id`), while this port removed it.
`fw-interface.c` is 6068 lines upstream against 2063 here. FR happens to work
because it is context 0 and the default everywhere; DS1 may be reaching
per-context state that the flattening left uninitialised.

Suggested next steps, in order:

1. Instrument the DS branch of `dma_writer_pipe_process_interrupt()` and
   `frame_buffer_*` to print pipe/ctx pointers *before* any dereference, and
   re-run with UART capture started first.
2. Compare `acamera_fw.c` (1021 changed lines) and `acamera_fsm_mgr.c` around
   per-context buffer/pipe state, specifically anything keyed on `ctx_id` that
   became a single global here.
3. Confirm whether upstream ever runs DS1 standalone, or always alongside FR —
   `fw_intf_stream_stop()`'s `stream_on_count` guard suggests concurrent use is
   the expected mode.

## Test tooling

On the board under `~/birdcher-tools/` (rebuild with `gcc -O1 -o X X.c`; `/tmp`
does not survive a hard reset):

- `ds1probe.c` — opens `/dev/video1` three times and prints the format of each
  handle, showing the FR/META/DS1 assignment.
- `ds1cap.c` — holds three handles, sets a format on the third (DS1) and
  captures from it.
- `bothcap.c` — streams FR and DS1 concurrently, draining FR to keep the
  pipeline running, dequeuing from DS1.

UART: `/dev/ttyUSB0` at 115200. Raise the console loglevel first
(`dmesg -n 8`), or nothing reaches it. Start the reader **before** the test —
the crash takes the board down faster than ssh can report.
