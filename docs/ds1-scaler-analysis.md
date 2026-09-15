# DS1 scaler path: reference comparison and current blocker

Goal: produce 1920x1080 through the DS1 scaler stream, using
`common_drivers` `khadas-vims-5.15.y` `drivers/armisp-g12b/` as the
authoritative implementation reference. No new scaler path, no FR emulation,
no enlarged FR buffers, no undocumented register pokes.

Reference revision: `3a11a86`.

**2026-09-15 continuation:** the comparison below missed a functional difference:
Khadas calls `crop_resolution_changed()` in `crop_set_resize_enable()` before
raising `event_id_crop_changed`; this port did not. Restoring that call produces
the 1920x1080 scaler-update trace before buffer arming and preserves FR capture,
but a fresh-load DS1 capture still hangs the board. See the continuation section
in [the handover](tasks/2026-09-15-ds1-1080p-handover.md) for evidence and the next
hardware-register check. DS1 bring-up is **not complete**.

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
| Buffer sizing / stride | **REGRESSION — fixed.** Sizes are identical, but the *stride* handoff was not: `isp_vb_to_tframe()` never set `line_offset`, and the pipe-enable path never programmed it. See "Root cause found" below |

Beyond the eight, the runtime DS1 plumbing was also compared:

- `dma_writer_fsm.c`, `dma_writer_intf.c` — byte identical.
- `dma_writer.c`, `dma_writer_func.c` — differences are formatting, `vmalloc`
  vs stack allocation of `dma_pipe_settings`, and extra logging. Same logic,
  same DS1 IRQ masks (`ACAMERA_IRQ_FRAME_WRITER_DS`, `ACAMERA_IRQ_FRAME_DROP_DS`).
- `crop_fsm.c` / `crop_func.c` — the original comparison missed the dropped
  `crop_resolution_changed()` call in `crop_set_resize_enable()`. Restored in
  the continuation; the interrupt path only updates on FR writer completion.
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

## The DMA stride was never programmed — necessary, but not the whole fault

> **Superseded in part, 2026-09-15.** What follows is accurate about the
> mechanism it describes and about the reference comparison, and the fix is
> committed and verified on FR. But it is **not** a complete explanation: DS1
> still corrupts memory with the stride correct. The live hypothesis is that
> `crop_func.c: _update_ds()` never runs, leaving the downscaler bypassed while
> the DMA writer expects 1920x1080. See
> `docs/tasks/2026-09-15-ds1-1080p-handover.md`, which also lists several
> conclusions on this page and elsewhere that were later falsified.

The 2026-09-15 pass widened the reference diff past the eight areas, into the
buffer-handoff path, and found the corruption source. It is two halves of one
omission, and each half makes the other look harmless:

**1. `isp-vb2.c: isp_vb_to_tframe()` never set `line_offset`.** The Khadas
reference (`isp-vb2.c:154,159`, `isp_vb_mmap_cvt()`) fills both
`frame->primary.line_offset` and `frame->secondary.line_offset` from
`pstream->cur_v4l2_fmt.fmt.pix_mp.plane_fmt[0].bytesperline`. Ours set only
`address` and `size`, so every tframe reached the firmware with a stride of 0
left over from the caller's `memset`.

**2. `dma_writer.c: dma_writer_write_frame_queue()` dropped the stride
writes.** The reference's "dma re-enabling" block programs the writer's stride
register before arming it (`dma_writer.c:293` primary, `:299` UV). Both calls
were missing here. The only surviving `line_offset_write()` calls are in the
per-frame path in `dma_writer_pipe_update()` — one frame too late.

So the DMA writer was armed for the *first* frame with whatever stride the
register happened to hold, from whatever geometry was programmed last. For FR
that is survivable: the stale value already describes a full-resolution frame
and the buffer is full-resolution too. For a 1920x1080 DS1 buffer sitting
behind a 3864-wide pipe it means the ISP writes ~8.7 MB into a 2.07 MB
allocation — 6.6 MB of kernel memory destroyed by hardware, from frame 0.

That matches the captured crash exactly, and explains its strangest feature:
the `Modules linked in:` line in `docs/logs/ds1-streamon-crash-uart.log` is
itself corrupted, degenerating mid-list into UTF-8 garbage. The module names
are ordinary kernel memory; they had already been overwritten *before* the oops
handler ran. Add the simultaneous aborts on several CPUs and the
`Insufficient stack space to handle exception!` banner, and this is not a bad
pointer in driver code — it is a DMA engine scribbling over the kernel.

Note also that the enable block has **no size check at all**, unlike the
per-frame path, which does compare `frame_size` against `primary.size`. So the
one arming site that could not validate its geometry was also the one that
stopped programming it.

### What was changed

- `isp-vb2.c` — `isp_vb_to_tframe()` now takes `pstream` and sets
  `line_offset` on both planes from the negotiated `bytesperline`, matching the
  reference. (The reference's equivalent takes `pstream` for the same reason.)
- `dma_writer.c` — restored both `line_offset_write()` /
  `line_offset_write_uv()` calls in the enable block.
- `dma_writer.c` — **not upstream**: a guard that refuses to arm the pipe when
  `stride * height` exceeds the buffer, logs loudly, and leaves the writer
  disabled. Given that this failure mode takes the whole board down with no
  recoverable log, a mismatch should become a message, not a reset.

### Verified on hardware

FR, after the fix:

```
TRACE wfq: type=0 len=1 enabled=0 init=1 3864x2192 stride=3968 bufsz=8699904
TRACE dma arm: pipe=0 3864x2192 stride=3968 need=8697856 have=8699904
```

The stride now arrives as 3968 (it was 0 before) and the arm check passes.
FR capture is unaffected.

**DS1 is not yet verified end to end.** The session lost access to the board
before the 1080p run could be made, so it is still unknown whether the DS1
pipe's `settings.width/height` actually follow the negotiated 1920x1080. The
guard makes that observable rather than fatal: if the pipe is still configured
for 3864x2192 while the buffer is 1080p, the next run logs

```
refusing to arm pipe 1 -- 3864x2192 stride 1920 needs ... bytes, buffer is 2073600
```

instead of corrupting memory — which would then point at the crop/scaler
geometry as the remaining problem rather than at the DMA writer.

## Earlier hypothesis (now superseded)

Kept for the record: the single-context flattening below was the leading theory
before the stride omission was found. It is no longer needed to explain the
crash, and should only be revisited if DS1 still misbehaves after the geometry
is confirmed.

This is **not** a dropped line in the eight areas audited — those are faithful.
The most likely systemic cause is the **single-context flattening** of the
firmware in this port: upstream `armisp-g12b` is multi-context and threads
`ctx_id` through `acamera_command()`, `fw_intf_*`, `acamera_fsm_mgr_*` and the
dma pipe settings (`pipe->settings.ctx_id`), while this port removed it.
`fw-interface.c` is 6068 lines upstream against 2063 here. FR happens to work
because it is context 0 and the default everywhere; DS1 may be reaching
per-context state that the flattening left uninitialised.

## Next steps

1. Run `~/birdcher-tools/ds1run 1920 1080 6 /tmp/ds1.raw` on the board with the
   rebuilt `iv009_isp.ko` and read back the `TRACE wfq` / `TRACE dma arm` /
   `refusing to arm` lines. Three outcomes, all informative:
   - armed with stride 1920 and frames arrive → DS1 works, render and check.
   - `refusing to arm` → the DS1 pipe geometry is not following the negotiated
     format; chase `crop_info.width_ds/height_ds` and the `IMAGE_RESIZE_*`
     path next.
   - no DS trace at all → the DS1 pipe is never reconfigured; chase
     `frame_buffer_configure()`.
2. Only if DS1 still misbehaves with correct geometry: compare `acamera_fw.c`
   (1021 changed lines) and `acamera_fsm_mgr.c` around per-context buffer/pipe
   state, for anything keyed on `ctx_id` that became a single global here.
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
- `ds1arm.c` — opens the three handles, sets the DS1 format, REQBUFs and QBUFs,
  then stops *before* STREAMON. Useful to confirm negotiation without any risk,
  but note it cannot observe the DMA arming: vb2 only hands queued buffers to
  the driver inside `vb2_start_streaming()`, so `dma_writer_write_frame_queue()`
  is not reached until STREAMON.
- `ds1run.c` — the real test: same setup plus STREAMON, a `select()`-guarded
  DQBUF loop with a 5 s timeout, and an optional raw NV12 dump of the last
  frame. `./ds1run 1920 1080 6 /tmp/ds1.raw`. Not yet built on the board.

UART: `/dev/ttyUSB0` at 115200. Raise the console loglevel first
(`dmesg -n 8`), or nothing reaches it. Start the reader **before** the test —
the crash takes the board down faster than ssh can report.
